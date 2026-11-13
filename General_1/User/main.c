#include "stm32f10x.h"
#include "Delay.h"
#include "OLED.h"
#include "MyI2C.h"
#include "Max30102.h"
#include "SHT3X.h"
#include "SpO2.h"
#include "Tick.h"

/*
 * VitaGuard-Q  ——  第一步：温湿度 + 血氧/心率 同时采集并显示
 *
 * 引脚（General_1/引脚.txt）：
 *   屏幕      : PB8=SCL  PB9=SDA   （本版仍是软件 I2C，和硬件 I2C1 重映射同脚，以后可直接换）
 *   传感器 I2C: PA15=SCL PA12=SDA  （SHT3X 0x44 + MAX30102 0x57 挂在同一条总线上）
 *   UART1/2/3 : 本版暂未使用（体温 / 血压 / 树莓派）
 *   按键      : 本版暂未使用
 *
 * 主循环是"协作式"的：
 *   - 血氧每轮都去排空 MAX30102 的 FIFO（100Hz，FIFO 只有 32 样本，不能停）；
 *   - 温湿度每 500ms 测一次（单次测量约 20~50ms，期间血氧样本积在 FIFO 里，不会丢）；
 *   - 屏幕每 200ms 刷一次。
 * 时间基准来自 TIM2 的 1ms 心跳（Tick.c），不用 Delay 卡时间。
 */

#define SHT_PERIOD_MS     500
#define UI_PERIOD_MS      200

/* ------------------------------------------------------------------ */
/* 显示                                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint8_t spo2Ok;                 /* MAX30102 初始化是否成功 */
    uint8_t shtOk;                  /* 温湿度最近一次测量是否成功 */
    uint8_t spo2Valid;              /* SpO2 数值是否有效 */
    uint8_t finger;                 /* 是否检测到手指 */
    uint8_t fifoErr;                /* MAX30102 FIFO 是否出错 */
    uint8_t spo2;
    uint8_t hr;                     /* 0 表示无效 */
    int16_t tempX10;
    int16_t humiX10;
} UI_State;

static const uint32_t s_pow10[5] = {1u, 10u, 100u, 1000u, 10000u};

/* 右对齐显示无符号数，前导 0 变成空格 */
static void UI_ShowUInt(uint8_t Line, uint8_t Column, uint32_t Value, uint8_t Width)
{
    uint8_t i;

    for (i = 0; i < Width; i++)
    {
        uint32_t Div = s_pow10[Width - 1 - i];

        if (Value < Div && i < (uint8_t)(Width - 1))
            OLED_ShowChar(Line, (uint8_t)(Column + i), ' ');
        else
            OLED_ShowChar(Line, (uint8_t)(Column + i),
                          (char)('0' + (uint8_t)((Value / Div) % 10u)));
    }
}

/* 显示 "±xxx.y"，总宽 = 1(符号) + IntWidth + 2(小数点+1位小数) */
static void UI_ShowFixed1(uint8_t Line, uint8_t Column, int16_t ValueX10, uint8_t IntWidth)
{
    int16_t  Magnitude = (ValueX10 < 0) ? (int16_t)(-ValueX10) : ValueX10;
    uint32_t IntPart   = (uint32_t)(Magnitude / 10);
    uint8_t  FracPart  = (uint8_t)(Magnitude % 10);
    uint8_t  i;

    OLED_ShowChar(Line, Column, (ValueX10 < 0) ? '-' : ' ');

    for (i = 0; i < IntWidth; i++)
    {
        uint32_t Div = s_pow10[IntWidth - 1 - i];

        if (IntPart < Div && i < (uint8_t)(IntWidth - 1))
            OLED_ShowChar(Line, (uint8_t)(Column + 1u + i), ' ');
        else
            OLED_ShowChar(Line, (uint8_t)(Column + 1u + i),
                          (char)('0' + (uint8_t)((IntPart / Div) % 10u)));
    }

    OLED_ShowChar(Line, (uint8_t)(Column + 1u + IntWidth), '.');
    OLED_ShowChar(Line, (uint8_t)(Column + 2u + IntWidth), (char)('0' + FracPart));
}

static void UI_Update(const UI_State *State)
{
    /* 第 1 行： SpO2: 98% HR: 72   （无效时显示 ---） */
    OLED_ShowString(1, 1, "SpO2:");
    if (State->spo2Valid)
        UI_ShowUInt(1, 6, State->spo2, 3);
    else
        OLED_ShowString(1, 6, " --");   /* 3 个字符全写，避免留下旧数字 */
    OLED_ShowChar(1, 9, '%');

    OLED_ShowString(1, 10, " HR:");
    if (State->hr > 0)
        UI_ShowUInt(1, 14, State->hr, 3);
    else
        OLED_ShowString(1, 14, "---");

    /* 第 2 行： Temp:  23.4 C */
    OLED_ShowString(2, 1, "Temp:");
    if (State->shtOk)
    {
        UI_ShowFixed1(2, 6, State->tempX10, 3);
        OLED_ShowChar(2, 12, ' ');
        OLED_ShowChar(2, 13, 'C');
    }
    else
    {
        OLED_ShowString(2, 6, " --.- C ");
    }

    /* 第 3 行： Humi:  45.6 % */
    OLED_ShowString(3, 1, "Humi:");
    if (State->shtOk)
    {
        UI_ShowFixed1(3, 6, State->humiX10, 3);
        OLED_ShowChar(3, 12, ' ');
        OLED_ShowChar(3, 13, '%');
    }
    else
    {
        OLED_ShowString(3, 6, " --.- % ");
    }

    /* 第 4 行：状态 */
    if (!State->spo2Ok)
        OLED_ShowString(4, 1, "MAX30102 ERR");
    else if (State->fifoErr)
        OLED_ShowString(4, 1, "FIFO ERROR      ");
    else if (!State->finger)
        OLED_ShowString(4, 1, "Place finger    ");
    else if (State->spo2Valid)
        OLED_ShowString(4, 1, "Measuring OK    ");
    else
        OLED_ShowString(4, 1, "Measuring...    ");
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static int16_t FloatToX10(float Value)
{
    float Scaled = Value * 10.0f;

    if (Scaled >= 0.0f) Scaled += 0.5f;
    else                Scaled -= 0.5f;

    if (Scaled >  9999.0f) Scaled =  9999.0f;
    if (Scaled < -9999.0f) Scaled = -9999.0f;

    return (int16_t)Scaled;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    UI_State State;
    uint32_t LastShtMs;
    uint32_t LastUiMs;
    uint8_t  Spo2Ok;
    uint8_t  ShtOk = 0;
    float    Temp = 0.0f;
    float    Humi = 0.0f;

    /* 全局只设一次优先级分组 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

    Tick_Init();
    OLED_Init();
    MyI2C_Init();               /* 必须在 SHT3X / MAX30102 之前：PA15 要关 JTAG */

    OLED_ShowString(1, 1, "VitaGuard-Q     ");
    OLED_ShowString(2, 1, "Init sensors... ");

    /* SHT3X_Init 内部会再调一次 MyI2C_Init，幂等，无副作用 */
    SHT3X_Init();

    Spo2Ok = SpO2_Init();

    if (Spo2Ok)
    {
        OLED_ShowString(3, 1, "MAX30102 OK     ");
    }
    else
    {
        OLED_ShowString(3, 1, "MAX30102 ERR");
        OLED_ShowNum(3, 14, Max30102_ErrorStep, 2);
    }

    Delay_ms(800);              /* 让开机信息停一下，方便看错误码 */
    OLED_Clear();

    State.spo2Ok    = Spo2Ok;
    State.shtOk     = 0;
    State.spo2Valid = 0;
    State.finger    = 0;
    State.fifoErr   = 0;
    State.spo2      = 0;
    State.hr        = 0;
    State.tempX10   = 0;
    State.humiX10   = 0;

    LastShtMs = Tick_GetMs();
    LastUiMs  = LastShtMs;

    while (1)
    {
        /* 1. 血氧：每轮都要把 FIFO 排空 */
        if (Spo2Ok)
        {
            (void)SpO2_Task();
        }

        /* 2. 温湿度：500ms 一次（内部阻塞约 20~50ms，样本先积在 FIFO 里） */
        if (Tick_Elapsed(&LastShtMs, SHT_PERIOD_MS))
        {
            if (SHT3X_GTMeasure(&Temp, &Humi))
            {
                ShtOk         = 1;
                State.tempX10 = FloatToX10(Temp);
                State.humiX10 = FloatToX10(Humi);
            }
            else
            {
                ShtOk = 0;
            }
            State.shtOk = ShtOk;
        }

        /* 3. 刷屏：200ms 一次 */
        if (Tick_Elapsed(&LastUiMs, UI_PERIOD_MS))
        {
            SpO2_Get(&State.spo2, &State.hr, &State.spo2Valid);
            State.finger  = SpO2_FingerPresent();
            State.fifoErr = SpO2_FifoError();

            UI_Update(&State);
        }
    }
}
