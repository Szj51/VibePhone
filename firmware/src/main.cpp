/*
 * VibePhone · ESP32-S3 「键盘 + 串口」复合设备固件  v2
 * ---------------------------------------------------------------
 * 一个 USB 口（板子上标 USB/OTG 的那个），同时提供两个功能：
 *
 *   ① HID 键盘接口  —— 按键伪装成标准 USB 键盘，发 F13 / F14。
 *                       拔下来插到任何电脑都还能当键盘用，通用性保留。
 *
 *   ② CDC 串口接口  —— 按键事件的 JSON 消息。电脑端桥接程序读这个口，
 *                       不依赖全局快捷键，也不需要猜键码。
 *
 * 两条独立的调试/通信通道：
 *
 *   Serial   = USB CDC  → 给电脑端桥接程序读的【设备协议】
 *   Serial0  = UART0    → 走板上 COM 口的【调试日志】（FTDI）
 *
 * 设备协议（按行分隔的 JSON，每条一行，以 \n 结尾）：
 *
 *   开机握手：  {"t":"hello","ver":1,"btns":2}
 *   按下：      {"t":"key","btn":1,"ev":"down"}
 *   松开：      {"t":"key","btn":1,"ev":"up"}
 *
 *   btn 是按键编号（1 起），对应电脑端 config.json 里 device.buttons 数组的下标。
 *
 * ⚠️ 硬件前提：
 *   1. 必须插在标着 USB/OTG 的 USB-C 口上（不是标 COM 的那个）
 *   2. 引脚接法见 config.h，详细说明见 README.md
 *
 * 编译烧录：见 README.md（PlatformIO）
 */

#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "config.h"
#include "display.h"

USBHIDKeyboard Keyboard;

// ============================================================
//  按键表
//  一个按键 = 一个引脚 + 一个要发的键码 + 一个协议编号，
//  再加消抖所需的内部状态。
// ============================================================
struct Button {
    uint8_t  pin;        // GPIO 引脚
    uint8_t  keycode;    // 按下时要发的 HID Usage ID
    uint8_t  number;     // 协议里的按键编号（1 起）
    bool     stable;     // 消抖后的确定状态，true = 按下
    bool     raw;        // 最近一次原始读数（未经消抖）
    uint32_t changedAt;  // 原始读数最后一次翻转的时间戳
};

static Button g_buttons[] = {
#ifdef ENABLE_TEST_BUTTON
    // 板载 BOOT 键。借用按键1 的键码和编号，所以不接线也能把整条链路
    // （HID + 串口协议）跑通一遍。默认关闭，原因见 config.h。
    { PIN_BTN_TEST, KEY_BTN_1, 1, false, false, 0 },
#endif
    { PIN_BTN_1,    KEY_BTN_1, 1, false, false, 0 },   // GPIO4 → 编号1 → Codex
    { PIN_BTN_2,    KEY_BTN_2, 2, false, false, 0 },   // GPIO5 → 编号2 → Claude Code
};

static const size_t BUTTON_COUNT = sizeof(g_buttons) / sizeof(g_buttons[0]);

// 上一次发心跳的时刻
static uint32_t g_lastHeartbeat = 0;

// 每个按键被按下的累计次数（下标 0 对应按键编号 1），显示在屏幕上
static uint32_t g_pressCount[2] = { 0, 0 };

// 上一次刷新屏幕运行时间的秒数，用来做"每秒更新一次"的判断
static uint32_t g_lastShownSecond = 0xFFFFFFFF;

// 读引脚：低电平有效（按下 = 接通 GND）
static inline bool isPressed(uint8_t pin) {
#if BTN_ACTIVE_LOW
    return digitalRead(pin) == LOW;
#else
    return digitalRead(pin) == HIGH;
#endif
}

static void initButtons() {
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
#if BTN_ACTIVE_LOW
        pinMode(g_buttons[i].pin, INPUT_PULLUP);
#else
        pinMode(g_buttons[i].pin, INPUT_PULLDOWN);
#endif
        // 上电时先采一次当前状态，避免开机瞬间误判成"刚按下"
        g_buttons[i].raw    = isPressed(g_buttons[i].pin);
        g_buttons[i].stable = g_buttons[i].raw;
    }
}

// ============================================================
//  设备协议（走 USB CDC）
// ============================================================

// ⚠️⚠️ 千万不要直接调用 Serial.print/printf 发协议 ⚠️⚠️
//
// 这是踩过的坑，而且症状极具迷惑性——【整个固件卡死，但设备还能被枚举】。
//
// 原因在 ESP32 Arduino core 的 USBCDC::write() 里：
//
//     while (to_send) {
//         if (!tud_cdc_n_connected(itf)) { size = so_far; break; }
//         size_t space = tud_cdc_n_write_available(itf);
//         if (!space) {
//             tud_cdc_n_write_flush(itf);
//             continue;          // ← 没有超时、没有 yield，纯忙等死循环
//         }
//         ...
//     }
//
// 那个 setTxTimeoutMs() 只管【互斥锁】的等待，管不到这个等空间的循环。
//
// 后果：主机"连着"（DTR 拉高）却不读数据时，TX 缓冲区满了以后这个循环
// 永远出不来 → loop() 再也不执行 → HID 键盘和串口【同时哑掉】，
// 但 USB 栈跑在独立任务里，所以设备管理器里看它还活着、枚举正常。
//
// 所以：写之前先问 availableForWrite()（它返回的正是 TX FIFO 剩余空间），
// 放不下就【丢弃这一条】，绝不阻塞。对按键设备来说，丢一条消息远好过整机卡死。
static void emitLine(const char *line) {
    const int room = Serial.availableForWrite();
    if (room < (int)strlen(line) + 2) {      // +2 给 println 的 CRLF 留余量
        return;                              // 没空间就丢，不冒险
    }
    Serial.println(line);
}

// 开机握手。电脑端靠它确认"这个口是 VibePhone 设备"，而不是别的串口设备。
static void sendHello() {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "{\"t\":\"hello\",\"ver\":1,\"btns\":%u}",
             (unsigned)BUTTON_COUNT);
    emitLine(buffer);
}

// 按键事件。
static void sendKeyEvent(uint8_t number, bool pressed) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "{\"t\":\"key\",\"btn\":%u,\"ev\":\"%s\"}",
             (unsigned)number, pressed ? "down" : "up");
    emitLine(buffer);
}

// 心跳。用途有两个：
//   ① 让电脑端能判别"这个串口是不是真的连到了设备"——收不到心跳就换下一个候选口。
//   ② 里面的 up（运行秒数）可以从电脑端看出来应用有没有重启过。
static void sendHeartbeat(uint32_t uptimeSeconds) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "{\"t\":\"ping\",\"up\":%lu}",
             (unsigned long)uptimeSeconds);
    emitLine(buffer);
}

// ============================================================
//  按键处理
// ============================================================

// 状态真正变化时才调用。按下 press，松开 release。
//
// ⚠️⚠️ 必须用 pressRaw / releaseRaw，不能用 press / release ⚠️⚠️
//
// 这是踩过的坑，务必别改回去。ESP32 的 USBHIDKeyboard 里这两个函数含义完全不同：
//
//   press(k)       k < 0x80 时被当作【ASCII 字符】，查 _asciimap 表转换成键码。
//                  所以 press(0x68) 发出的不是 F13，而是 ASCII 0x68 = 'h'！
//                  实测就是这个问题：按按键，浏览器测试页显示的是 "h"。
//                  它只适合 press('a') 这种用法，或 press(0x88 + 键码) 绕开查表。
//
//   pressRaw(k)    k 就是【原始 USB HID Usage ID】，直接进报告，不做任何转换。
//                  F13 是 0x68，F14 是 0x69，这正是我们要的。
//
// 另外必须成对：只发 press 不发 release，电脑会认为这个键一直卡着。
static void handleChange(size_t index, bool pressed) {
    Button &b = g_buttons[index];

    // ① HID 键盘：保持通用性（拔下来插别的电脑也能用）
    if (pressed) {
        Keyboard.pressRaw(b.keycode);
    } else {
        Keyboard.releaseRaw(b.keycode);
    }

    // ② 串口协议：给电脑端桥接程序读的精确消息
    sendKeyEvent(b.number, pressed);

    // ③ 屏幕反馈：只在按下时动，松开不动——避免一次按键闪两下
    if (pressed && b.number >= 1 && b.number <= 2) {
        g_pressCount[b.number - 1]++;
#if TFT_ENABLE
        displayFlash(b.number);                          // 整行反白一下
        displaySetCount(b.number, g_pressCount[b.number - 1]);  // flash 会清掉数字，补回来
#endif
    }

#ifdef ENABLE_SERIAL_DEBUG
    Serial0.printf("[btn] GPIO%-2u 编号%u %s  键码 0x%02X\n",
                   b.pin, b.number, pressed ? "按下 →" : "松开 ←", b.keycode);
#endif
}

// 采样 + 消抖。
// 思路：读数一变就重新计时，只有新状态持续超过 DEBOUNCE_MS 才认定生效。
// 这样抖动期间的反复翻转会被不断重置，不会产生假的按下/松开。
static void pollButtons() {
    const uint32_t now = millis();

    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        Button    &b      = g_buttons[i];
        const bool pressed = isPressed(b.pin);

        if (pressed != b.raw) {
            // 读数刚翻转：重新计时，暂不认定
            b.raw       = pressed;
            b.changedAt = now;
        } else if (pressed != b.stable && (now - b.changedAt) >= DEBOUNCE_MS) {
            // 新状态已稳定超过消抖时长，确认为真实变化
            b.stable = pressed;
            handleChange(i, pressed);
        }
    }
}

void setup() {
#ifdef ENABLE_SERIAL_DEBUG
    // 调试日志走 UART0（COM 口），与 USB CDC 完全独立，
    // 这样即使 USB 侧出问题，也还能看到设备在说什么。
    Serial0.begin(115200);
    delay(200);  // 等 UART 就绪，否则开机头几行日志会丢
    Serial0.println();
    Serial0.println("=========================================");
    Serial0.println("  VibePhone · ESP32-S3 键盘+串口  v2");
    Serial0.printf ("  按键数：%u\n", (unsigned)BUTTON_COUNT);
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        Serial0.printf("    编号%u  GPIO%-2u  →  键码 0x%02X\n",
                       g_buttons[i].number, g_buttons[i].pin, g_buttons[i].keycode);
    }
    Serial0.println("  USB CDC = 设备协议 | UART0 = 本日志");
    Serial0.println("=========================================");
#endif

    initButtons();

#if TFT_ENABLE
    displayInit();
#endif

    Keyboard.begin();
    USB.begin();

    // 把互斥锁的等待时间压到很短。协议发送一律走 emitLine()，
    // 它靠 availableForWrite() 预检查来避免阻塞，不依赖这里的超时。
    Serial.setTxTimeoutMs(10);

    // 注意：这里【不能】写 while(!Serial) ——
    // 那会等到主机打开串口才继续，没插 USB 时设备就卡死在这儿了。
    delay(300);   // 给 USB 枚举留一点时间，提高 hello 被收到的概率
    sendHello();

#ifdef ENABLE_SERIAL_DEBUG
    Serial0.println("[sys] 已通过 USB CDC 发送开机握手");
#endif
}

void loop() {
    pollButtons();

    const uint32_t now = millis();

    // 心跳：电脑端靠它确认串口连对了，也靠它发现应用是否重启过。
    if (now - g_lastHeartbeat >= HEARTBEAT_MS) {
        g_lastHeartbeat = now;
        sendHeartbeat(now / 1000);
    }

#if TFT_ENABLE
    // 屏幕每秒只刷一次——millis() 一直在变，不加这个判断会疯狂重画。
    // setLink 也放在这里：连接状态变化很慢，一秒一次的粒度足够。
    const uint32_t sec = now / 1000;
    if (sec != g_lastShownSecond) {
        g_lastShownSecond = sec;
        displaySetUptime(sec);
        displaySetLink((bool)Serial);   // USBCDC 的 operator bool() = 主机是否已连上
    }
#endif

    delay(POLL_MS);
}
