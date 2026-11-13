#ifndef __KEY_H
#define __KEY_H

#include "stm32f10x.h"

/* 按键（共 4 个，本版用到 SW1/SW2）
 *   SW1 = PA8，SW2 = PA9，SW3 = PA10，SW4 = PA11
 *   一端接 GPIO，一端接 GND，内部上拉 => 松开=1，按下=0
 *
 * 用法：
 *      Key_Init();
 *      while (1) {
 *          uint8_t ev = Key_Scan(Tick_GetMs());   // 只有"按下沿"才置位
 *          if (ev & KEY_EVENT_SW1) { ... }
 *      }
 */

#define KEY_EVENT_SW1   0x01
#define KEY_EVENT_SW2   0x02
#define KEY_EVENT_SW3   0x04
#define KEY_EVENT_SW4   0x08

void    Key_Init(void);

/* 非阻塞扫描，必须由主循环周期调用（间隔 < 20ms 才能可靠消抖）。
 * 返回按位组合的 KEY_EVENT_xxx。按住不放不会重复触发。 */
uint8_t Key_Scan(uint32_t nowMs);

/* 当前稳定电平：1=按下。用于调试或长按判断 */
uint8_t Key_IsDown(uint8_t Index);

#endif
