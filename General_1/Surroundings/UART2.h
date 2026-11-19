#ifndef __UART2_H
#define __UART2_H

#include "stm32f10x.h"

/* USART2 —— 串口血压计
 *   PA2 = TX（AF_PP）
 *   PA3 = RX（浮空输入，建议外部 10k 上拉）
 *   115200 8N1，无流控，**不需要任何重映射**（PA2/PA3 就是 USART2 的默认脚）
 *
 * 结构与 UART1.c 保持一致：中断里只搬字节进环形缓冲，解析在主循环做。
 * 缓冲 128 字节；血压计 0.5s 才发一帧（最长 10 字节），余量极大。
 */

void     UART2_Init(void);
void     UART2_SendBuf(const uint8_t *buf, uint16_t len);
void     UART2_SendStr(const char *str);        /* 目前零调用，保留备用 */
uint8_t  UART2_Pop(uint8_t *byte);              /* 1=取到，0=空 */
uint16_t UART2_Count(void);
void     UART2_Flush(void);                     /* 丢弃缓冲里所有字节 */

#endif
