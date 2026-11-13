
#ifndef __MAX30102_REG_H
#define __MAX30102_REG_H

#include "stm32f10x.h"

/*================ I2C 硬件映射 ================*/
// I2C 从机地址（7 bit）
#define MAX30102_ADDRESS          0x57

// 写地址字节
#define MAX30102_WRITE_ADDRESS    (MAX30102_ADDRESS << 1)

// 读地址字节
#define MAX30102_READ_ADDRESS     ((MAX30102_ADDRESS << 1) | 0x01)

/*================ MAX30102 寄存器映射 ================*/

#define MAX30102_INT_STATUS1      0x00
#define MAX30102_INT_STATUS2      0x01

#define MAX30102_INT_ENABLE1      0x02
#define MAX30102_INT_ENABLE2      0x03

#define MAX30102_FIFO_WR_PTR      0x04
#define MAX30102_OVF_COUNTER      0x05
#define MAX30102_FIFO_RD_PTR      0x06
#define MAX30102_FIFO_DATA        0x07

#define MAX30102_FIFO_CONFIG      0x08
#define MAX30102_MODE_CONFIG      0x09
#define MAX30102_SPO2_CONFIG      0x0A

#define MAX30102_LED1_PA          0x0C
#define MAX30102_LED2_PA          0x0D

#define MAX30102_TEMP_INTEGER     0x1F
#define MAX30102_TEMP_FRACTION    0x20

#define MAX30102_PART_ID          0xFF

/*================ 数据配置 ================*/

#define MAX30102_FIFO_SIZE        32
#define MAX30102_DATA_LENGTH      6
#define MAX30102_ADC_BITS         18

#endif
