#ifndef __BOARD_H
#define __BOARD_H

#include "stm32f10x.h"

/* 板上所有"重映射 / 全局设置"的唯一入口
 *
 * 必须集中在这里做的原因：StdPeriph 的 GPIO_PinRemapConfig() 在每次
 * 做普通重映射时都会把 SWJ_CFG 写成 111（JTAG+SWD 全关）。
 * 也就是说，如果你先关了 JTAG 再调 GPIO_Remap_USART1，JTAG 会被重新打开，
 * PA15 立刻失效 —— 这种顺序 bug 极难查。
 *
 * 所以：先做完所有普通重映射，最后一步才处理 SWJ_CFG。
 */

void Board_Init(void);

#endif
