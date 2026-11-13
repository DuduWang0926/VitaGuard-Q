#ifndef __UART1_H
#define __UART1_H

#include "stm32f10x.h"

/* USART1 —— 接触式温度传感器
 *   PB6 = TX（本版基本不用，但配成完整 UART）
 *   PB7 = RX
 *   USART1 默认在 PA9/PA10，这里必须开 AFIO + USART1 重映射才能挪到 PB6/PB7
 *   （因为 PA9/PA10 已经分配给了按钮 2/3）
 *   115200 8N1
 *
 * 结构与 UART2.c 保持一致：中断里只搬字节进环形缓冲，解析在主循环做。
 */

void     UART1_Init(void);
void     UART1_SendBuf(const uint8_t *buf, uint16_t len);
void     UART1_SendStr(const char *str);
uint8_t  UART1_Pop(uint8_t *byte);      /* 1=取到，0=空 */
uint16_t UART1_Count(void);
void     UART1_Flush(void);

#endif
