#pragma once
#include <stdint.h>

// ============================================================
//  1.54" ST7789 屏幕（240x240 IPS，SPI）
//
//  设计原则：只在局部重画变化的区域，不做整屏刷新——整屏刷会闪，
//  而且 240x240x2 = 115KB 一次，走 SPI 有明显延迟。
// ============================================================

// 初始化屏幕并画出静态框架（标题栏、分隔线、各行的标签）。
// 调用后屏幕是一个完整的、各数值为 0 的画面。
void displayInit();

// 更新运行时间那一行（每秒调一次）
void displaySetUptime(uint32_t seconds);

// 更新某个按键的按下次数。buttonIndex 从 1 开始，对应协议里的按键编号。
void displaySetCount(uint8_t buttonIndex, uint32_t count);

// 更新底部连接状态（USB CDC 是否已连上主机）
void displaySetLink(bool connected);

// 按键时的即时视觉反馈：整条行反白一下。
// 放在按键处理里调用，让人一眼看到"这个键被认到了"。
void displayFlash(uint8_t buttonIndex);
