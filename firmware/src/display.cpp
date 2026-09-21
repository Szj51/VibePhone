/*
 * VibePhone · 1.54" ST7789 屏幕界面
 * ---------------------------------------------------------------
 * 240x240 IPS，SPI。接线见 config.h。
 *
 * 设计原则：**只重画变化的小区域**，绝不整屏刷新。
 *   整屏一次是 240x240x2 = 115KB，走 SPI 有明显延迟，而且会闪。
 *
 * ⚠️ 界面文字全部用 ASCII —— Adafruit_GFX 的默认字体只有 5x7 点阵的
 *    西文字符，写中文会渲染成空白或乱码。要显示中文必须另外加载中文字库
 *    （占 flash 且需要取模工具），本版本不做。
 */

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "config.h"
#include "display.h"

#if TFT_ENABLE

static Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);

// ---- 配色（RGB565）----
static const uint16_t C_BG     = ST77XX_BLACK;
static const uint16_t C_TITLE  = 0x0A33;   // 深蓝，标题栏底色
static const uint16_t C_WHITE  = ST77XX_WHITE;
static const uint16_t C_DIM    = 0x7BEF;   // 浅灰，用于次要文字
static const uint16_t C_GREEN  = 0x2FE5;   // 亮绿，计数和正常状态
static const uint16_t C_ORANGE = 0xFC60;   // 橙，异常状态

// ---- 布局 ----
static const int16_t TITLE_H   = 38;    // 标题栏高度
static const int16_t ROW1_LBL  = 56;    // 按键1 标签的 y
static const int16_t ROW2_LBL  = 116;   // 按键2 标签的 y
static const int16_t ROW_GAP  = 60;     // 两行之间的间距
static const int16_t DIVIDER_Y = 176;   // 分隔线
static const int16_t UP_Y      = 190;   // 运行时间行
static const int16_t LINK_Y    = 214;   // 连接状态行

static const int16_t COUNT_RIGHT = 224; // 计数的右对齐位置

// 按键编号 → 显示标签。
// 这里只写通用的 "KEY n"——因为设备并不知道自己打开的是 Codex 还是别的什么，
// 那个对应关系在电脑端的 config.json 里。等以后做了「电脑→设备」的反向通道，
// 就可以让电脑把真实名字发过来显示。
static const char *labelFor(uint8_t index) {
    return (index == 1) ? "KEY 1" : "KEY 2";
}

// 右对齐画一行字
static void drawRight(const char *text, int16_t right, int16_t y, uint8_t size, uint16_t color) {
    int16_t x1, y1;
    uint16_t w, h;
    tft.setTextSize(size);
    tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor(right - (int16_t)w, y);
    tft.setTextColor(color, C_BG);
    tft.print(text);
}

// 画某个按键那一行的静态部分（标签 + 一条细分隔线）
static void drawRowLabel(uint8_t index, int16_t y) {
    tft.setTextSize(2);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(16, y);
    tft.print(labelFor(index));

    // 标签下面一条淡淡的横线，视觉上把两行分开
    tft.drawFastHLine(16, y + 20, 208, 0x2104);
}

// ---- 原始 SPI 自检（完全绕开 Adafruit 库）----
// 目的：把"硬件/接线"和"库的配置"这两件事分开。
// 这段代码不依赖库的任何逻辑，直接按 ST7789 数据手册发命令并填满整屏红色。
// 如果屏能变红，说明 SPI、接线、模块全都是好的，问题就只能在库里。
static void rawSpiSelfTest() {
    pinMode(TFT_DC_PIN, OUTPUT);

    SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, -1);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

    // DC 低 = 命令，DC 高 = 数据
    // （注意：这几个 lambda 之间不能互相调用，无捕获的 lambda 抓不到彼此，
    //   所以 d16 把两次 transfer 展开写，而不是去调 d8。）
    auto cmd = [](uint8_t c) { digitalWrite(TFT_DC_PIN, LOW);  SPI.transfer(c); };
    auto d8  = [](uint8_t d) { digitalWrite(TFT_DC_PIN, HIGH); SPI.transfer(d); };
    auto d16 = [](uint16_t d) {
        digitalWrite(TFT_DC_PIN, HIGH);
        SPI.transfer((uint8_t)(d >> 8));
        SPI.transfer((uint8_t)(d & 0xFF));
    };

    cmd(0x01); delay(150);            // SWRESET  软件复位
    cmd(0x11); delay(120);            // SLPOUT   退出睡眠
    cmd(0x3A); d8(0x55);              // COLMOD   16bit/pixel（RGB565）
    cmd(0x36); d8(0x00);              // MADCTL   扫描方向
    cmd(0x21);                        // INVON    IPS 屏通常需要反显
    cmd(0x13);                        // NORON    正常显示模式
    cmd(0x29); delay(20);             // DISPON   开显示

    // 设置整屏为写区域，然后填红
    cmd(0x2A); d16(0); d16(TFT_WIDTH - 1);    // 列地址
    cmd(0x2B); d16(0); d16(TFT_HEIGHT - 1);   // 行地址
    cmd(0x2C);                                 // RAMWR 开始写显存
    for (uint32_t i = 0; i < (uint32_t)TFT_WIDTH * TFT_HEIGHT; i++) {
        d16(0xF800);                           // RGB565 纯红
    }

    SPI.endTransaction();
    digitalWrite(TFT_DC_PIN, HIGH);
}

void displayInit() {
    // 背光：本模块内部已上拉，这里只在明确接了 GPIO 时才动它
    if (TFT_BLK_PIN >= 0) {
        pinMode(TFT_BLK_PIN, OUTPUT);
        digitalWrite(TFT_BLK_PIN, HIGH);
    }

#ifdef ENABLE_SERIAL_DEBUG
    Serial0.println("[tft] 开始初始化屏幕");
#endif

    // ---- 先跑一遍原始 SPI 自检（结果应该是整屏变红）----
    rawSpiSelfTest();
    delay(800);   // 停一下，让红色能被看见

#ifdef ENABLE_SERIAL_DEBUG
    Serial0.println("[tft] 原始 SPI 自检已执行（屏幕若变红则硬件正常）");
#endif

    // ---- 显式把 SPI 开在我们的引脚上 ----
    // 库内部随后还会调一次 SPI.begin()（无参数）。总线已经初始化过，
    // 那次调用会直接返回，所以这里设的引脚不会被覆盖。
    SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, -1);

    // ---- 手动发一条软件复位（SWRESET）----
    // ⚠️ 这一步【必须自己做】，不能指望库。原因在 Adafruit_SPITFT::initSPI()：
    //
    //     if (_rst >= 0) {
    //         // ...拉低 RST 200ms 的硬件复位时序...
    //     }
    //     // ← 没有 else 分支
    //
    // 也就是说 RES 没有接 GPIO（_rst = -1）时，库【既不硬件复位也不软件复位】，
    // 什么都不做。屏在初始化前处于未复位状态，表现为【背光亮但完全没有画面】。
    //
    // 这里自己发 0x01 (SWRESET)，之后延时 150ms（数据手册要求 ≥120ms）。
    pinMode(TFT_DC_PIN, OUTPUT);
    digitalWrite(TFT_DC_PIN, LOW);                       // DC 低 = 本次是命令
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    SPI.transfer(ST77XX_SWRESET);
    SPI.endTransaction();
    delay(150);
    digitalWrite(TFT_DC_PIN, HIGH);                      // 恢复到数据模式

    tft.init(TFT_WIDTH, TFT_HEIGHT);
    tft.setRotation(0);
    // 24MHz 对跳线连接比较稳妥；换成 PCB 走线后可以往上调到 40~80MHz
    tft.setSPISpeed(24000000);
    tft.fillScreen(C_BG);

    // ---- 标题栏 ----
    tft.fillRect(0, 0, TFT_WIDTH, TITLE_H, C_TITLE);
    tft.setTextSize(3);
    tft.setTextColor(C_WHITE, C_TITLE);
    {
        const char *title = "VIBEPHONE";
        int16_t x1, y1;
        uint16_t w, h;
        tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
        tft.setCursor((TFT_WIDTH - (int16_t)w) / 2, (TITLE_H - (int16_t)h) / 2);
        tft.print(title);
    }

    // ---- 两个按键行 ----
    drawRowLabel(1, ROW1_LBL);
    drawRowLabel(2, ROW2_LBL);

    // ---- 分隔线 ----
    tft.drawFastHLine(0, DIVIDER_Y, TFT_WIDTH, 0x3186);

    // ---- 底部两行 ----
    tft.setTextSize(2);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(16, UP_Y);
    tft.print("UP");

    tft.setCursor(16, LINK_Y);
    tft.print("USB");

    // 数值先画成 0 / 未连接，之后由 displaySet* 局部刷新
    displaySetCount(1, 0);
    displaySetCount(2, 0);
    displaySetUptime(0);
    displaySetLink(false);
}

void displaySetCount(uint8_t buttonIndex, uint32_t count) {
    if (buttonIndex < 1 || buttonIndex > 2) return;

    const int16_t y = (buttonIndex == 1) ? ROW1_LBL + 22 : ROW2_LBL + 22;

    // 先清掉数字区域（靠右 110px 宽、40px 高），再重画——
    // 只清这一小块，屏幕其余部分完全不动，所以不会闪。
    tft.fillRect(COUNT_RIGHT - 110, y, 110, 40, C_BG);

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%lu", (unsigned long)count);
    drawRight(buffer, COUNT_RIGHT, y, 3, C_GREEN);
}

void displaySetUptime(uint32_t seconds) {
    const uint32_t h = seconds / 3600;
    const uint32_t m = (seconds % 3600) / 60;
    const uint32_t s = seconds % 60;

    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)s);

    // 清掉旧值再画新的
    tft.fillRect(56, UP_Y, 168, 20, C_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE, C_BG);
    tft.setCursor(56, UP_Y);
    tft.print(buffer);
}

void displaySetLink(bool connected) {
    tft.fillRect(56, LINK_Y, 168, 20, C_BG);
    tft.setTextSize(2);
    tft.setTextColor(connected ? C_GREEN : C_ORANGE, C_BG);
    tft.setCursor(56, LINK_Y);
    tft.print(connected ? "HID+CDC OK" : "HID only");
}

void displayFlash(uint8_t buttonIndex) {
    if (buttonIndex < 1 || buttonIndex > 2) return;

    const int16_t y = (buttonIndex == 1) ? ROW1_LBL - 6 : ROW2_LBL - 6;
    const int16_t h = 48;

    // 反白一下：整行底色变绿
    tft.fillRect(0, y, TFT_WIDTH, h, C_GREEN);
    tft.setTextSize(2);
    tft.setTextColor(C_BG, C_GREEN);
    tft.setCursor(16, y + 6);
    tft.print(labelFor(buttonIndex));

    delay(110);   // 足够看清，又不会让人觉得按键迟滞

    // 恢复：清底色、重画标签，数字由调用方随后的 displaySetCount 补上
    tft.fillRect(0, y, TFT_WIDTH, h, C_BG);
    drawRowLabel(buttonIndex, (buttonIndex == 1) ? ROW1_LBL : ROW2_LBL);
}

#endif  // TFT_ENABLE
