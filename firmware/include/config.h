#pragma once
#include <stdint.h>

// ============================================================
//  硬件配置
//  目标：优信电子 YD-ESP32-23 V1.3 (ESP32-S3-WROOM, N16R8)
// ============================================================

// ---- 按键引脚 ----
//
// ⚠️ 这些引脚不可用，别往上接：
//      GPIO19 / GPIO20  —— S3 原生 USB（D- / D+），HID 就靠它
//      GPIO26 ~ GPIO32  —— 模组内部连 SPI Flash
//      GPIO33 ~ GPIO37  —— N16R8 的 8MB Octal PSRAM 占用
//      GPIO43 / GPIO44  —— UART0（就是 COM 口的日志）
//      GPIO0 / GPIO3 / GPIO45 / GPIO46 —— strapping 脚，只能当输入且要小心
//
// 下面选的 GPIO4 / GPIO5 都不在上面这些冲突范围内，是这块板子上最安全的选择。
#define PIN_BTN_1        4      // 按键1 → Codex
#define PIN_BTN_2        5      // 按键2 → Claude Code

// ---- 板载 BOOT 键（GPIO0）----
// 作用：不接任何线就能先验证固件能不能发按键。它和按键1 发同一个键码。
//
// 【已关闭】实体按键（GPIO4/GPIO5）验证通过后关掉，原因有二：
//
//   1. DTR 干扰：ESP32 开发板的自动复位电路把 USB-串口的 DTR 接在 GPIO0 上，
//      所以**只要打开串口监视器就会误发一次 F13**。桥接程序在跑的时候，
//      这会导致莫名其妙弹出 Codex。
//
//   2. 释放 strapping 脚：GPIO0 本该只用于 boot 模式选择。
//
// 需要临时用它调试（比如换板子、还没接线）时，把下面这行取消注释即可。
//#define ENABLE_TEST_BUTTON

#define PIN_BTN_TEST     0

// ---- 电气特性 ----
// 按键一端接 GPIO、另一端接 GND，使用芯片内部上拉，低电平有效。
// 不需要外接电阻。
#define BTN_ACTIVE_LOW   1

// 消抖时间。轻触开关的机械抖动通常 <10ms，30ms 足够且手感不迟钝。
#define DEBOUNCE_MS      30

// 主循环采样间隔
#define POLL_MS          5

// ============================================================
//  HID 键码
//  使用原始 USB HID Usage ID（Keyboard/Keypad page 0x07），
//  不依赖库里的枚举名，避免不同 core 版本命名差异。
//
//  ⚠️ 这些值交给 Keyboard.pressRaw() / releaseRaw() 使用，不能交给
//     Keyboard.press() / release()——后者会把 < 0x80 的值当成 ASCII 字符
//     查表转换，导致 0x68 发出的不是 F13 而是 'h'。详见 src/main.cpp 注释。
// ============================================================
// 常用键码备查：
//     0x68 = F13    0x69 = F14    0x6A = F15    0x6B = F16
//     0x28 = Enter  0x29 = Esc    0x2A = Backspace
//     0x04 = a      0x1E = 1      0x1F = 2

#define KEY_BTN_1        0x68   // F13 → 打开 Codex
#define KEY_BTN_2        0x69   // F14 → 打开 Claude Code

// ---- 1.54" ST7789 屏幕（240x240 IPS，SPI）----
//
// 接法（方案 A：省 CS / RES，占用 GPIO 最少）：
//
//     屏幕          ESP32-S3
//     GND    ────   GND
//     VCC    ────   3V3        ⚠️ 3.3V，不要接 5V
//     SCL    ────   GPIO12     SPI 时钟   ← 正好是硬件 SPI 默认 SCK
//     SDA    ────   GPIO11     SPI 数据   ← 正好是硬件 SPI 默认 MOSI
//     DC     ────   GPIO9      ⚠️ 必须接，它区分命令和数据
//     CS     ────   GND        （永久选中，这条 SPI 上只有屏幕一个设备）
//     RES    ────   3V3        （改用软件复位）
//     BLK    ────   不用接     （本模块内部已拉高，背光常亮）
//
// SCL/SDA 用 11/12 是因为它们就是 ESP32-S3 硬件 SPI（FSPI）的默认 MOSI/SCK，
// 走硬件 SPI 比软件模拟快得多。
//
// ⚠️ DC 特意【不】放在 GPIO13：13 是硬件 SPI 的默认 MISO。虽然我们不读数据，
//    但库初始化 SPI 时会把 13 设成输入，和 DC 需要的输出冲突。
//    默认 SPI 引脚表见 framework-arduinoespressif32/variants/esp32s3/pins_arduino.h：
//        SS=10  MOSI=11  SCK=12  MISO=13
//
// 用 -1 表示"这个脚没接 GPIO"——Adafruit 的库专门支持这种接法。
// 【临时关闭用于排查】屏幕代码全部不编译 —— 用来判断"应用会停"是不是屏幕代码引起的。
// 排查完改回 1。
#define TFT_ENABLE       0
#define TFT_SCLK_PIN     12
#define TFT_MOSI_PIN     11
#define TFT_DC_PIN       9
#define TFT_CS_PIN       (-1)   // -1 = 接 GND
#define TFT_RST_PIN      (-1)   // -1 = 接 3V3，用软件复位
#define TFT_BLK_PIN      (-1)   // -1 = 不用接（模块内部拉高）

#define TFT_WIDTH        240
#define TFT_HEIGHT       240

// ---- 心跳 ----
// 每隔这么久通过 USB CDC 发一条 {"t":"ping","up":<运行秒数>}。
//
// 为什么需要它：电脑端要靠"设备是不是在说话"来判断自己连对了串口。
// 设备刚上电的头一两秒，ESP32-S3 还挂在 ROM 的 USB-Serial-JTAG 上，
// 那时也会出现一个 VID_303A 的串口——连上去却永远收不到数据。
// 有心跳，监听脚本最多等 HEARTBEAT_MS 就能判别并换口。
//
// 十来字节一条，对 USB 和 CPU 都没有可感影响。调试时可以注释掉。
#define HEARTBEAT_MS     3000

// ---- 调试输出 ----
// 通过 COM 口（UART0）打印，115200。量产/正式使用时可以注释掉减少开销。
#define ENABLE_SERIAL_DEBUG
