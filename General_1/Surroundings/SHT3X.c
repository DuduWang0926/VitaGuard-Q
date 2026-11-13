#include "stm32f10x.h"                  // Device header
#include "MyI2C.h"
#include "Delay.h"
#define SHT3X_ADDRESS (0X44<<1)
void SHT3X_Init(void)
{
	MyI2C_Init();									//先初始化底层的I2C
}

uint8_t SHT3X_STMeasure(void){
	MyI2C_Start();
MyI2C_SendByte(SHT3X_ADDRESS);  // 发送写地址
	    if (MyI2C_ReceiveAck() != 0)
    {
        MyI2C_Stop();
		return 0;
    }
MyI2C_SendByte(0x24);       // 命令高字节
	    if (MyI2C_ReceiveAck() != 0)
    {
        MyI2C_Stop();
		return 0;
    }

MyI2C_SendByte(0x00);       // 命令低字节
	    if (MyI2C_ReceiveAck() != 0)
    {
        MyI2C_Stop();
		return 0;
    }
MyI2C_Stop();

Delay_ms(20); 
return 1;// 等待测量完成
}

uint8_t SHT3X_ReadData(uint8_t *Data)
{
    uint8_t i;

    MyI2C_Start();	

    // 发送读地址
    MyI2C_SendByte(SHT3X_ADDRESS | 0x01);

    if (MyI2C_ReceiveAck() != 0)
    {
        MyI2C_Stop();
        return 0;
    }

    // 连续读取6个字节
    for (i = 0; i < 6; i++)
    {
        Data[i] = MyI2C_ReceiveByte();

        if (i < 5)
        {
            MyI2C_SendAck(0);  // ACK
        }
        else
        {
            MyI2C_SendAck(1);  // 最后一个字节发送NACK
        }
    }

    MyI2C_Stop();
    return 1;
}

uint8_t SHT3X_CRC(uint8_t *Data, uint8_t Length)
{
    uint8_t myCRC = 0xFF;
    uint8_t i, j;

    for (i = 0; i < Length; i++)
    {
        myCRC ^= Data[i];

        for (j = 0; j < 8; j++)
        {
            if (myCRC & 0x80)
            {
                myCRC = (myCRC << 1) ^ 0x31;
            }
            else
            {
                myCRC = myCRC << 1;
            }
        }
    }

    return myCRC;
}

 uint8_t SHT3X_GTMeasure(float *Tmp,float *hum){
	uint16_t RawT,RawH;
	uint8_t Data[6];
	if (SHT3X_STMeasure() == 0)
{
    return 0;
}
	if(SHT3X_ReadData(Data)==0){
		return 0;
	}
	if (SHT3X_CRC(&Data[0], 2) != Data[2])
    {
        return 0;
    }

    // 校验湿度数据
    if (SHT3X_CRC(&Data[3], 2) != Data[5])
    {
        return 0;
    }
	RawT = ((uint16_t)Data[0] << 8) | Data[1];
	*Tmp= -45.0f + 175.0f * RawT / 65535.0f;
	RawH = ((uint16_t)Data[3] << 8) | Data[4];
	*hum = 100.0f * RawH / 65535.0f;
	return 1;
}
