
#include "stm32f10x.h"
#include "MyI2C.h"
#include "Delay.h"
#include "Max30102Reg.h"

/*
 * 错误代码：
 * 0  = 正常
 * 10 = PART_ID读取失败（I2C无应答）
 * 11 = 芯片ID错误
 * 20 = 复位寄存器写入失败
 * 21 = 复位状态读取失败
 * 22 = 复位超时
 * 30 = 清空FIFO失败
 * 31 = FIFO_CONFIG配置失败
 * 32 = SPO2_CONFIG配置失败
 * 33 = LED1配置失败
 * 34 = LED2配置失败
 * 35 = MODE_CONFIG配置失败
 * 40 = FIFO数据读取失败
 * 41 = FIFO无数据
 * 50 = FIFO指针读取失败
 */

volatile uint8_t Max30102_ErrorStep = 0;
volatile uint8_t Max30102_PartID = 0;


/* ==================== 底层寄存器操作 ==================== */

/*
 * 写寄存器
 * 成功返回1，失败返回0
 */
static uint8_t Max30102_WriteReg(uint8_t Reg, uint8_t Data)
{
    MyI2C_Start();

    MyI2C_SendByte(MAX30102_WRITE_ADDRESS);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;

    MyI2C_SendByte(Reg);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;

    MyI2C_SendByte(Data);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;

    MyI2C_Stop();
    return 1;

ERROR:
    MyI2C_Stop();
    return 0;
}


/*
 * 读寄存器
 * 成功返回1，失败返回0
 */
static uint8_t Max30102_ReadReg(uint8_t Reg, uint8_t *Data)
{
    if(Data == 0) return 0;

    MyI2C_Start();

    MyI2C_SendByte(MAX30102_WRITE_ADDRESS);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;

    MyI2C_SendByte(Reg);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;

    // 重复起始信号，切换到读取模式
    MyI2C_Start();

    MyI2C_SendByte(MAX30102_READ_ADDRESS);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;

    *Data = MyI2C_ReceiveByte();

    // 单字节读取结束，主机发送NACK
    MyI2C_SendAck(1);

    MyI2C_Stop();

    return 1;

ERROR:
    MyI2C_Stop();
    return 0;
}


/* ==================== 芯片复位 ==================== */

uint8_t Max30102_Reset(void)
{
    uint8_t Mode;
    uint8_t Timeout;

    Max30102_ErrorStep = 20;

    // MODE_CONFIG寄存器 bit6 = RESET
    if(!Max30102_WriteReg(MAX30102_MODE_CONFIG, 0x40))
    {
        return 0;
    }

    // 等待芯片自动清除RESET位
    for(Timeout = 0; Timeout < 100; Timeout++)
    {
        Delay_ms(1);

        if(!Max30102_ReadReg(MAX30102_MODE_CONFIG, &Mode))
        {
            Max30102_ErrorStep = 21;
            return 0;
        }

        if((Mode & 0x40) == 0)
        {
            return 1;
        }
    }

    Max30102_ErrorStep = 22;
    return 0;
}


/* ==================== 清空FIFO ==================== */

uint8_t Max30102_ClearFIFO(void)
{
    Max30102_ErrorStep = 30;

    if(!Max30102_WriteReg(MAX30102_FIFO_WR_PTR, 0x00))
        return 0;

    if(!Max30102_WriteReg(MAX30102_OVF_COUNTER, 0x00))
        return 0;

    if(!Max30102_WriteReg(MAX30102_FIFO_RD_PTR, 0x00))
        return 0;

    return 1;
}


/* ==================== SpO2模式初始化 ==================== */

uint8_t Max30102_STMeasure_SPO2(void)
{
    uint8_t PartID;
    uint8_t Retry;

    // 1. 检查芯片ID
    Max30102_ErrorStep = 10;
    Max30102_PartID = 0;

    // 上电或总线刚恢复时可能出现瞬态错误，最多尝试3次。
    for(Retry = 0; Retry < 3; Retry++)
    {
        if(Max30102_ReadReg(MAX30102_PART_ID, &PartID))
        {
            Max30102_PartID = PartID;

            if(PartID == 0x15)
                break;

            Max30102_ErrorStep = 11;
        }
        else
        {
            Max30102_ErrorStep = 10;
        }

        Delay_ms(5);
    }

    if(Retry == 3)
        return 0;


    // 2. 软件复位
    if(!Max30102_Reset())
        return 0;


    // 3. 清空FIFO
    if(!Max30102_ClearFIFO())
        return 0;


    // 4. 配置FIFO
    Max30102_ErrorStep = 31;

    /*
     * FIFO_CONFIG = 0x0F
     * SMP_AVE = 000：不平均
     * FIFO_ROLLOVER_EN = 0
     * FIFO_A_FULL = 1111
     */
    if(!Max30102_WriteReg(MAX30102_FIFO_CONFIG, 0x0F))
        return 0;


    // 5. 配置SpO2
    Max30102_ErrorStep = 32;

    /*
     * SPO2_CONFIG = 0x27
     * ADC量程 = 4096nA
     * 采样率 = 100Hz
     * LED脉冲宽度 = 411us
     * ADC分辨率 = 18bit
     */
    if(!Max30102_WriteReg(MAX30102_SPO2_CONFIG, 0x27))
        return 0;


    // 6. 配置RED LED电流
    Max30102_ErrorStep = 33;

    if(!Max30102_WriteReg(MAX30102_LED1_PA, 0x3F))
        return 0;


    // 7. 配置IR LED电流
    Max30102_ErrorStep = 34;

    if(!Max30102_WriteReg(MAX30102_LED2_PA, 0x3F))
        return 0;


    // 8. 设置SpO2工作模式
    Max30102_ErrorStep = 35;

    if(!Max30102_WriteReg(MAX30102_MODE_CONFIG, 0x03))
        return 0;


    Max30102_ErrorStep = 0;

    return 1;
}


/* ==================== 获取FIFO样本数量 ==================== */

uint8_t Max30102_GetFIFOCount(void)
{
    uint8_t WR_PTR;
    uint8_t RD_PTR;
    uint8_t OVF_COUNTER;

    Max30102_ErrorStep = 50;

    // 读取写指针
    if(!Max30102_ReadReg(MAX30102_FIFO_WR_PTR, &WR_PTR))
        return 0xFF;

    // 读取读指针
    if(!Max30102_ReadReg(MAX30102_FIFO_RD_PTR, &RD_PTR))
        return 0xFF;

    // 读写指针相等既可能表示空，也可能表示32个样本全部占满。
    // FIFO满后丢失的新样本会使OVF_COUNTER非零；弹出一个样本后该计数器自动清零。
    if(!Max30102_ReadReg(MAX30102_OVF_COUNTER, &OVF_COUNTER))
        return 0xFF;

    WR_PTR &= 0x1F;
    RD_PTR &= 0x1F;
    OVF_COUNTER &= 0x1F;

    if(WR_PTR == RD_PTR)
    {
        return (OVF_COUNTER != 0) ? MAX30102_FIFO_SIZE : 0;
    }

    // FIFO深度32，指针为5bit。
    return (WR_PTR - RD_PTR) & 0x1F;
}


/* ==================== 读取FIFO数据 ==================== */

uint8_t Max30102_ReadFIFO(uint32_t *RED, uint32_t *IR)
{
    uint8_t Data[6];
    uint8_t i;
    uint8_t Count;

    if(RED == 0 || IR == 0)
        return 0;

    // 检查FIFO是否有数据
    Count = Max30102_GetFIFOCount();

    if(Count == 0xFF)
        return 0;

    if(Count == 0)
    {
        Max30102_ErrorStep = 41;
        return 0;
    }

    Max30102_ErrorStep = 40;

    // 设置读取FIFO_DATA寄存器
    MyI2C_Start();

    MyI2C_SendByte(MAX30102_WRITE_ADDRESS);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;

    MyI2C_SendByte(MAX30102_FIFO_DATA);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;

    // 重复起始，进入读取模式
    MyI2C_Start();

    MyI2C_SendByte(MAX30102_READ_ADDRESS);
    if(MyI2C_ReceiveAck() != 0) goto ERROR;


    // 连续读取6字节
    for(i = 0; i < 6; i++)
    {
        Data[i] = MyI2C_ReceiveByte();

        if(i == 5)
        {
            MyI2C_SendAck(1);   // 最后一个字节发送NACK
        }
        else
        {
            MyI2C_SendAck(0);   // 前5个字节发送ACK
        }
    }

    MyI2C_Stop();


    // 组合18bit RED数据
    *RED = ((uint32_t)(Data[0] & 0x03) << 16)
         | ((uint32_t)Data[1] << 8)
         | Data[2];


    // 组合18bit IR数据
    *IR = ((uint32_t)(Data[3] & 0x03) << 16)
        | ((uint32_t)Data[4] << 8)
        | Data[5];


    Max30102_ErrorStep = 0;

    return 1;


ERROR:

    MyI2C_Stop();

    Max30102_ErrorStep = 40;

    return 0;
}
	
