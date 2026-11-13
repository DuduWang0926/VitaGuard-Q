#include "stm32f10x.h"
#include "Delay.h"
#include "MyI2C.h"

/*
 * 软件 I2C 引脚（按 General_1/引脚.txt）
 *   SCL = PA15
 *   SDA = PA12
 *
 * 三个必须知道的前提：
 *   1) PA15 复位后是 JTAG 的 JTDI，必须先把 JTAG 关掉（SWD 仍可用）才能当 GPIO；
 *   2) 两个引脚都是开漏输出，OD 模式下内部上拉不生效，必须靠模块板上的
 *      4.7k~10k 上拉到 3.3V（SHT3X / MAX30102 模块一般都自带）；
 *   3) 每个半周期延时 10us，SCL 约 50kHz。两种传感器都支持 400kHz，
 *      这里刻意放慢是为了兼容长杜邦线和弱上拉。
 */

#define MYI2C_PORT      GPIOA
#define MYI2C_SCL_PIN   GPIO_Pin_15
#define MYI2C_SDA_PIN   GPIO_Pin_12

/* ==================== 引脚层 ==================== */

void MyI2C_W_SCL(uint8_t BitValue)
{
    GPIO_WriteBit(MYI2C_PORT, MYI2C_SCL_PIN, (BitAction)BitValue);
    Delay_us(10);
}

void MyI2C_W_SDA(uint8_t BitValue)
{
    GPIO_WriteBit(MYI2C_PORT, MYI2C_SDA_PIN, (BitAction)BitValue);
    Delay_us(10);
}

uint8_t MyI2C_R_SDA(void)
{
    uint8_t BitValue;

    BitValue = GPIO_ReadInputDataBit(MYI2C_PORT, MYI2C_SDA_PIN);
    Delay_us(10);
    return BitValue;
}

void MyI2C_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);

    /* PA15 复位后是 JTDI，需要先关 JTAG 才能当 GPIO。
     * 这件事统一交给 Board_Init() 在最后一步做（顺序有讲究，见 Board.c），
     * 这里只负责把引脚配成开漏。 */

    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Pin   = MYI2C_SCL_PIN | MYI2C_SDA_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(MYI2C_PORT, &GPIO_InitStructure);

    /* 释放总线（开漏 + 外部上拉 => 高电平） */
    GPIO_SetBits(MYI2C_PORT, MYI2C_SCL_PIN | MYI2C_SDA_PIN);
}

/* ==================== 协议层 ==================== */

void MyI2C_Start(void)
{
    MyI2C_W_SDA(1);
    MyI2C_W_SCL(1);
    MyI2C_W_SDA(0);     /* SCL 高电平期间拉低 SDA = 起始 */
    MyI2C_W_SCL(0);
}

void MyI2C_Stop(void)
{
    MyI2C_W_SDA(0);
    MyI2C_W_SCL(1);
    MyI2C_W_SDA(1);     /* SCL 高电平期间释放 SDA = 终止 */
}

void MyI2C_SendByte(uint8_t Byte)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        MyI2C_W_SDA(!!(Byte & (0x80 >> i)));
        MyI2C_W_SCL(1);
        MyI2C_W_SCL(0);
    }
}

uint8_t MyI2C_ReceiveByte(void)
{
    uint8_t i;
    uint8_t Byte = 0x00;

    MyI2C_W_SDA(1);     /* 主机释放 SDA，交给从机驱动 */
    for (i = 0; i < 8; i++)
    {
        MyI2C_W_SCL(1);
        if (MyI2C_R_SDA()) Byte |= (0x80 >> i);
        MyI2C_W_SCL(0);
    }
    return Byte;
}

void MyI2C_SendAck(uint8_t AckBit)
{
    MyI2C_W_SDA(AckBit);
    MyI2C_W_SCL(1);
    MyI2C_W_SCL(0);
}

uint8_t MyI2C_ReceiveAck(void)
{
    uint8_t AckBit;

    MyI2C_W_SDA(1);
    MyI2C_W_SCL(1);
    Delay_us(5);        /* 给从机多一点时间把 SDA 拉低（MAX30102 需要） */
    AckBit = MyI2C_R_SDA();
    MyI2C_W_SCL(0);
    return AckBit;
}
