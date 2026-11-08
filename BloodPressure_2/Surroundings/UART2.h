#ifndef __UART2_H
#define __UART2_H

#include "stm32f10x.h"
#include <stdint.h>

/* USART2: PA2 = TX, PA3 = RX, 115200 8N1
 * 接收为中断 + 128 字节环形缓冲，中断里只搬运字节，不做任何解析。 */

void     UART2_Init(void);                              /* GPIO + USART2 + NVIC */
void     UART2_SendBuf(const uint8_t *buf, uint16_t len); /* 阻塞发送任意长度 */
void     UART2_SendStr(const char *str);                 /* 阻塞发送字符串 */
uint8_t  UART2_Pop(uint8_t *byte);                       /* 取一个字节：1=取到 0=空 */
uint16_t UART2_Count(void);                              /* 缓冲中待取字节数 */
void     UART2_Flush(void);                              /* 丢弃缓冲中所有字节 */

#endif
