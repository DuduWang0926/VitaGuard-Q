#ifndef __MYI2C_H
#define __MYI2C_H

/* 软件 I2C（温湿度 SHT3X + 血氧 MAX30102 共用一条总线）
 *   SCL = PA15
 *   SDA = PA12
 * 注意：PA15 复位后是 JTDI，MyI2C_Init() 里会关闭 JTAG（保留 SWD）后才可用。
 */

#include "stm32f10x.h"

void    MyI2C_Init(void);
void    MyI2C_Start(void);
void    MyI2C_Stop(void);
void    MyI2C_SendByte(uint8_t Byte);
uint8_t MyI2C_ReceiveByte(void);
void    MyI2C_SendAck(uint8_t AckBit);
uint8_t MyI2C_ReceiveAck(void);

/* 引脚级操作，协议层内部使用，也可用于调试 */
void    MyI2C_W_SCL(uint8_t BitValue);
void    MyI2C_W_SDA(uint8_t BitValue);
uint8_t MyI2C_R_SDA(void);

#endif
