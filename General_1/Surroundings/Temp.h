#ifndef __TEMP_H
#define __TEMP_H

#include "stm32f10x.h"

/* 接触式温度传感器（USART1 / PB7-RX）
 *
 * 协议（沿用 ContactTemp 工程）：ASCII 帧，共 9 字节
 *      'A'  '+'/'-'  tens  ones  '.'  tenths  'B'  '\r'  '\n'
 *   例：A+36.5B\r\n  => 365（单位 0.1℃）
 *
 * 稳定性判据：连续 3 帧数值完全一致才算一次有效结果。
 * 用法：
 *      Temp_Init();
 *      while (1) {
 *          Temp_Poll(Tick_GetMs());          // 排空串口，内部做帧解析
 *          if (Temp_Get(&bodyX10)) { ... }
 *      }
 */

void    Temp_Init(void);

/* 排空接收缓冲并解析。返回 1 表示本次得到了新的"3 帧一致"结果 */
uint8_t Temp_Poll(uint32_t nowMs);

/* 取最新稳定值（单位 0.1℃）。返回 1=有效（3 秒内更新过） */
uint8_t Temp_Get(int16_t *valueX10);

/* 1 = 1 秒内收到过完整帧（传感器在发数据） */
uint8_t Temp_SensorAlive(void);

#endif
