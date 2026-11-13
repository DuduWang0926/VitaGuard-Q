#ifndef __TICK_H
#define __TICK_H

#include "stm32f10x.h"

/* 1 ms 系统心跳（TIM2 中断），只提供"过了多久"这种判断，
 * 不替代 Delay.c —— Delay 走 SysTick 轮询，两者互不干扰。 */

void     Tick_Init(void);
uint32_t Tick_GetMs(void);

/* 若距上次 *lastMs 已超过 periodMs，则把 *lastMs 更新为当前值并返回 1 */
uint8_t  Tick_Elapsed(uint32_t *lastMs, uint32_t periodMs);

#endif
