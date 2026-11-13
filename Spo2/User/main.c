#include "stm32f10x.h"
#include "Delay.h"
#include "OLED.h"
#include "MyI2C.h"
#include "Max30102.h"

uint32_t RED = 0;
uint32_t IR = 0;

uint8_t FIFO_Count;
uint8_t FIFO_Error = 0;
uint8_t SpO2_Value = 0;
uint8_t SpO2_Valid = 0;

#define SPO2_WINDOW_SIZE      200
#define SPO2_FINGER_IR_MIN    10000
#define SPO2_AC_MIN           100

static uint32_t Red_Filtered;
static uint32_t IR_Filtered;
static uint32_t IR_Previous;
static int32_t IR_OldChange;
static uint8_t Filter_Initialized;

static uint32_t Red_Min;
static uint32_t Red_Max;
static uint32_t IR_Min;
static uint32_t IR_Max;
static uint32_t Red_Sum;
static uint32_t IR_Sum;
static uint16_t SpO2_SampleCount;
static uint8_t Pulse_PeakCount;

static void SpO2_ResetWindow(void)
{
    Red_Min = 0xFFFFFFFF;
    Red_Max = 0;
    IR_Min = 0xFFFFFFFF;
    IR_Max = 0;
    Red_Sum = 0;
    IR_Sum = 0;
    SpO2_SampleCount = 0;
    Pulse_PeakCount = 0;
}

/*
 * PDF中的leaky integration：
 * filtered = 0.75 * old + 0.25 * raw
 * 这里使用整数运算，避免Cortex-M3进行软件浮点运算。
 */
static uint8_t SpO2_ProcessSample(uint32_t Red_Raw, uint32_t IR_Raw)
{
    int32_t IR_Change;
    uint32_t Red_DC;
    uint32_t IR_DC;
    uint32_t Red_AC;
    uint32_t IR_AC;
    uint32_t Ratio_x1000;
    uint32_t CalculatedSpO2;
    unsigned long long Numerator;
    unsigned long long Denominator;

    if(!Filter_Initialized)
    {
        Red_Filtered = Red_Raw;
        IR_Filtered = IR_Raw;
        IR_Previous = IR_Filtered;
        IR_OldChange = 0;
        Filter_Initialized = 1;
    }
    else
    {
        Red_Filtered = (3 * Red_Filtered + Red_Raw) / 4;
        IR_Filtered = (3 * IR_Filtered + IR_Raw) / 4;

        IR_Change = (int32_t)IR_Filtered - (int32_t)IR_Previous;

        // 平滑信号的斜率由正变负，表示检测到一个脉搏峰值。
        if(IR_Change < 0 && IR_OldChange > 0)
        {
            if(Pulse_PeakCount < 0xFF)
                Pulse_PeakCount++;
        }

        IR_Previous = IR_Filtered;

        // 整数滤波在峰顶可能出现零斜率，保留最近一次非零方向以免漏峰。
        if(IR_Change != 0)
            IR_OldChange = IR_Change;
    }

    if(Red_Filtered < Red_Min) Red_Min = Red_Filtered;
    if(Red_Filtered > Red_Max) Red_Max = Red_Filtered;
    if(IR_Filtered < IR_Min) IR_Min = IR_Filtered;
    if(IR_Filtered > IR_Max) IR_Max = IR_Filtered;

    Red_Sum += Red_Filtered;
    IR_Sum += IR_Filtered;
    SpO2_SampleCount++;

    if(SpO2_SampleCount < SPO2_WINDOW_SIZE)
        return 0;

    Red_DC = Red_Sum / SPO2_WINDOW_SIZE;
    IR_DC = IR_Sum / SPO2_WINDOW_SIZE;
    Red_AC = Red_Max - Red_Min;
    IR_AC = IR_Max - IR_Min;

    SpO2_Valid = 0;

    // 直流量用于判断手指，交流量和峰值用于排除无脉搏或信号过弱。
    if(IR_DC >= SPO2_FINGER_IR_MIN &&
       Red_DC > 0 &&
       Red_AC >= SPO2_AC_MIN &&
       IR_AC >= SPO2_AC_MIN &&
       Pulse_PeakCount > 0)
    {
        Numerator = (unsigned long long)Red_AC * IR_DC * 1000;
        Denominator = (unsigned long long)IR_AC * Red_DC;

        if(Denominator != 0)
        {
            Ratio_x1000 = (uint32_t)(Numerator / Denominator);

            // 常用线性近似：SpO2 = 110 - 25R。
            if(Ratio_x1000 > 0 && Ratio_x1000 < 2000)
            {
                CalculatedSpO2 = 110 - (25 * Ratio_x1000 + 500) / 1000;

                if(CalculatedSpO2 > 100)
                    CalculatedSpO2 = 100;

                SpO2_Value = (uint8_t)CalculatedSpO2;
                SpO2_Valid = 1;
            }
        }
    }

    SpO2_ResetWindow();
    return 1;
}

static void OLED_ShowSpO2(void)
{
    if(SpO2_Valid)
    {
        OLED_ShowString(3, 1, "SpO2:   %       ");
        OLED_ShowNum(3, 6, SpO2_Value, 3);
        OLED_ShowString(4, 1, "SIGNAL OK       ");
    }
    else
    {
        OLED_ShowString(3, 1, "SpO2:---%       ");
        OLED_ShowString(4, 1, "PLACE FINGER    ");
    }
}

int main(void)
{
    uint8_t Result_Ready = 0;
    uint8_t Last_FIFO_Error = 0;

    OLED_Init();
    MyI2C_Init();

    OLED_ShowString(1, 1, "MAX30102 TEST");

    // 初始化 MAX30102
    if(Max30102_STMeasure_SPO2() == 0)
    {
        if(Max30102_ErrorStep == 10)
        {
            OLED_ShowString(2, 1, "I2C NO RESPONSE ");
            OLED_ShowString(3, 1, "CHECK PA15 PA12 ");
            OLED_ShowString(4, 1, "PULL-UP TO 3V3  ");
        }
        else if(Max30102_ErrorStep == 11)
        {
            OLED_ShowString(2, 1, "PART ID ERROR   ");
            OLED_ShowString(3, 1, "READ ID:0x      ");
            OLED_ShowHexNum(3, 11, Max30102_PartID, 2);
            OLED_ShowString(4, 1, "EXPECTED:0x15   ");
        }
        else
        {
            OLED_ShowString(2, 1, "INIT ERROR      ");
            OLED_ShowString(3, 1, "ERROR CODE:     ");
            OLED_ShowNum(3, 12, Max30102_ErrorStep, 2);
        }

        while(1);
    }

    SpO2_ResetWindow();

    OLED_ShowString(2, 1, "SpO2 MONITOR    ");
    OLED_ShowString(3, 1, "SpO2:---%       ");
    OLED_ShowString(4, 1, "MEASURING...    ");

    while(1)
    {
        FIFO_Count = Max30102_GetFIFOCount();

        if(FIFO_Count == 0xFF)
        {
            FIFO_Error = 1;
        }
        else
        {
            FIFO_Error = 0;

            // 每轮把当前积压样本全部读完，避免OLED刷新期间FIFO再次堆满。
            while(FIFO_Count > 0)
            {
                if(!Max30102_ReadFIFO(&RED, &IR))
                {
                    FIFO_Error = 1;
                    break;
                }

                if(SpO2_ProcessSample(RED, IR))
                    Result_Ready = 1;

                FIFO_Count = Max30102_GetFIFOCount();

                if(FIFO_Count == 0xFF)
                {
                    FIFO_Error = 1;
                    break;
                }
            }
        }

        if(FIFO_Error != Last_FIFO_Error)
        {
            if(FIFO_Error)
                OLED_ShowString(2, 1, "FIFO ERROR      ");
            else
                OLED_ShowString(2, 1, "SpO2 MONITOR    ");

            Last_FIFO_Error = FIFO_Error;
        }

        if(Result_Ready && !FIFO_Error)
        {
            OLED_ShowSpO2();
            Result_Ready = 0;
        }

        Delay_ms(1);
    }
}
