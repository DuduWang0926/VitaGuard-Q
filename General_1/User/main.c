#include "stm32f10x.h"
#include "Delay.h"
#include "OLED.h"
#include "Board.h"
#include "MyI2C.h"
#include "Max30102.h"
#include "SHT3X.h"
#include "SpO2.h"
#include "UART1.h"
#include "Temp.h"
#include "Key.h"
#include "Tick.h"

/*
 * VitaGuard-Q —— 第二版：温湿度 + 血氧/心率 + 接触温度，两页显示
 *
 * 引脚：
 *   屏幕        PB8/PB9    软件 I2C（SCL/SDA 已按用户要求互换，见 OLED.c）
 *   传感器 I2C  PA15/PA12  SHT3X(0x44) + MAX30102(0x57)
 *   USART1      PB6/PB7    接触温度传感器（重映射，115200 8N1）
 *   按键        PA8=SW1(下一页)  PA9=SW2(上一页)  PA10/PA11 预留
 *
 * 初始化顺序不能乱：
 *   Board_Init()  ← 先把所有重映射做完，最后关 JTAG（PA15 才可用）
 *   → OLED / 按键 / 传感器
 *   任何"普通重映射"都必须留在 Board_Init 里，否则会把 JTAG 重新打开。
 */

#define PAGE_COUNT        2
#define SHT_PERIOD_MS     500
#define UI_PERIOD_MS      200
#define KEY_PERIOD_MS     10

#define UI_ROWS           4
#define UI_COLS           16

/* ------------------------------------------------------------------ */
/* 显示：先画到 RAM 里的 4x16 影子缓冲，再和上一帧比对，只写变化的字符    */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint8_t spo2Ok;
    uint8_t shtOk;
    uint8_t spo2Valid;
    uint8_t finger;
    uint8_t fifoErr;
    uint8_t spo2;
    uint8_t hr;
    int16_t tempX10;
    int16_t humiX10;
    int16_t bodyX10;
    uint8_t bodyValid;
} UI_State;

static char s_shadow[UI_ROWS][UI_COLS];    /* 本帧应显示的内容 */
static char s_shown [UI_ROWS][UI_COLS];    /* 屏幕上已经显示的内容 */

static const uint32_t s_pow10[5] = {1u, 10u, 100u, 1000u, 10000u};

static void UI_ClearBuffer(void)
{
    uint8_t r, c;

    for (r = 0; r < UI_ROWS; r++)
    {
        for (c = 0; c < UI_COLS; c++)
        {
            s_shadow[r][c] = ' ';
        }
    }
}

static void UI_Char(uint8_t Line, uint8_t Column, char Ch)
{
    if (Line < 1 || Line > UI_ROWS)   return;
    if (Column < 1 || Column > UI_COLS) return;

    s_shadow[Line - 1][Column - 1] = Ch;
}

static void UI_Text(uint8_t Line, uint8_t Column, const char *Text)
{
    while (*Text != '\0')
    {
        UI_Char(Line, Column, *Text);
        Column++;
        Text++;
    }
}

/* 右对齐无符号数，前导 0 变空格 */
static void UI_UInt(uint8_t Line, uint8_t Column, uint32_t Value, uint8_t Width)
{
    uint8_t i;

    for (i = 0; i < Width; i++)
    {
        uint32_t Div = s_pow10[Width - 1 - i];

        if (Value < Div && i < (uint8_t)(Width - 1))
            UI_Char(Line, (uint8_t)(Column + i), ' ');
        else
            UI_Char(Line, (uint8_t)(Column + i),
                    (char)('0' + (uint8_t)((Value / Div) % 10u)));
    }
}

/* 显示 "±xxx.y"，总宽 = 1(符号) + IntWidth + 2 */
static void UI_Fixed1(uint8_t Line, uint8_t Column, int16_t ValueX10, uint8_t IntWidth)
{
    int16_t  Magnitude = (ValueX10 < 0) ? (int16_t)(-ValueX10) : ValueX10;
    uint32_t IntPart   = (uint32_t)(Magnitude / 10);
    uint8_t  FracPart  = (uint8_t)(Magnitude % 10);
    uint8_t  i;

    UI_Char(Line, Column, (ValueX10 < 0) ? '-' : ' ');

    for (i = 0; i < IntWidth; i++)
    {
        uint32_t Div = s_pow10[IntWidth - 1 - i];

        if (IntPart < Div && i < (uint8_t)(IntWidth - 1))
            UI_Char(Line, (uint8_t)(Column + 1u + i), ' ');
        else
            UI_Char(Line, (uint8_t)(Column + 1u + i),
                    (char)('0' + (uint8_t)((IntPart / Div) % 10u)));
    }

    UI_Char(Line, (uint8_t)(Column + 1u + IntWidth), '.');
    UI_Char(Line, (uint8_t)(Column + 2u + IntWidth), (char)('0' + FracPart));
}

/* 把影子缓冲同步到屏幕：只写有变化的字符，换页不闪、不残留 */
static void UI_Flush(void)
{
    uint8_t r, c;

    for (r = 0; r < UI_ROWS; r++)
    {
        for (c = 0; c < UI_COLS; c++)
        {
            if (s_shadow[r][c] != s_shown[r][c])
            {
                OLED_ShowChar((uint8_t)(r + 1u), (uint8_t)(c + 1u), s_shadow[r][c]);
                s_shown[r][c] = s_shadow[r][c];
            }
        }
    }
}

/* ---------------- 第 1 页：血氧 / 心率 ---------------- */

static void UI_PageSpO2(const UI_State *State)
{
    UI_Text(1, 1, "1/2 SpO2 & HR");

    UI_Text(2, 1, "SpO2:");
    if (State->spo2Valid)
        UI_UInt(2, 7, State->spo2, 3);
    else
        UI_Text(2, 7, " --");
    UI_Char(2, 10, '%');

    UI_Text(2, 12, "HR:");
    if (State->hr > 0)
        UI_UInt(2, 15, State->hr, 2);
    else
        UI_Text(2, 15, "--");

    if (!State->spo2Ok)
        UI_Text(3, 1, "MAX30102 ERR");
    else if (State->fifoErr)
        UI_Text(3, 1, "FIFO ERROR");
    else if (!State->finger)
        UI_Text(3, 1, "Place finger");
    else if (State->spo2Valid)
        UI_Text(3, 1, "Measuring OK");
    else
        UI_Text(3, 1, "Measuring...");

    UI_Text(4, 1, "SW1/SW2: page");
}

/* ---------------- 第 2 页：接触温度 + 环境 ---------------- */

static void UI_PageTemp(const UI_State *State)
{
    UI_Text(1, 1, "2/2 Body & Amb");

    UI_Text(2, 1, "Body:");
    if (State->bodyValid)
    {
        UI_Fixed1(2, 7, State->bodyX10, 2);
        UI_Char(2, 12, 'C');
    }
    else
    {
        UI_Text(2, 7, "--.- C");
    }

    UI_Text(3, 1, "Amb :");
    if (State->shtOk)
    {
        UI_Fixed1(3, 6, State->tempX10, 3);   /* 整数字段留 3 位：SHT3X 上限 125℃ */
        UI_Char(3, 12, 'C');
    }
    else
    {
        UI_Text(3, 7, "--.- C");
    }

    UI_Text(4, 1, "Humi:");
    if (State->shtOk)
    {
        UI_Fixed1(4, 6, State->humiX10, 3);
        UI_Char(4, 12, '%');
    }
    else
    {
        UI_Text(4, 7, "--.- %");
    }
}

static void UI_Render(uint8_t Page, const UI_State *State)
{
    UI_ClearBuffer();

    if (Page == 0)
        UI_PageSpO2(State);
    else
        UI_PageTemp(State);

    UI_Flush();
}

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
    uint32_t LastKeyMs;
    uint8_t  Spo2Ok;
    uint8_t  ShtOk = 0;
    uint8_t  Page  = 0;
    uint8_t  NeedRedraw = 1;
    float    Temp = 0.0f;
    float    Humi = 0.0f;
    int16_t  BodyX10 = 0;
    uint8_t  r, c;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

    /* 1. 重映射一步到位（内部最后才关 JTAG） */
    Board_Init();

    Tick_Init();
    OLED_Init();
    Key_Init();
    MyI2C_Init();
    UART1_Init();

    /* 影子缓冲先填满空格，且认为屏幕上什么都没有 */
    UI_ClearBuffer();
    for (r = 0; r < UI_ROWS; r++)
    {
        for (c = 0; c < UI_COLS; c++)
        {
            s_shown[r][c] = '\0';
        }
    }

    SHT3X_Init();
    Temp_Init();
    Spo2Ok = SpO2_Init();

    /* 开机自检画面直接用 OLED，绕过影子缓冲，随后清干净 */
    OLED_ShowString(1, 1, "VitaGuard-Q     ");
    OLED_ShowString(2, 1, "Init sensors... ");
    if (Spo2Ok)
    {
        OLED_ShowString(3, 1, "MAX30102 OK     ");
    }
    else
    {
        OLED_ShowString(3, 1, "MAX30102 ERR");
        OLED_ShowNum(3, 14, Max30102_ErrorStep, 2);
    }

    Delay_ms(800);
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
    State.bodyX10   = 0;
    State.bodyValid = 0;

    LastShtMs = Tick_GetMs();
    LastUiMs  = LastShtMs;
    LastKeyMs = LastShtMs;

    while (1)
    {
        uint32_t now = Tick_GetMs();

        /* 1. 按键：10ms 扫一次（消抖需要周期调用） */
        if ((uint32_t)(now - LastKeyMs) >= KEY_PERIOD_MS)
        {
            uint8_t events;

            LastKeyMs = now;
            events = Key_Scan(now);

            if ((events & KEY_EVENT_SW1) != 0)
            {
                Page = (uint8_t)((Page + 1u) % PAGE_COUNT);
                NeedRedraw = 1;
            }

            if ((events & KEY_EVENT_SW2) != 0)
            {
                Page = (uint8_t)((Page + PAGE_COUNT - 1u) % PAGE_COUNT);
                NeedRedraw = 1;
            }
        }

        /* 2. 接触温度：串口中断在后台收，这里只解析 */
        (void)Temp_Poll(now);

        /* 3. 血氧：每轮都要排空 FIFO */
        if (Spo2Ok)
        {
            (void)SpO2_Task();
        }

        /* 4. 温湿度：500ms 一次（阻塞约 22ms，血氧样本先积在 FIFO 里） */
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

        /* 5. 刷屏：200ms 一次，换页时立刻重画 */
        if (NeedRedraw || Tick_Elapsed(&LastUiMs, UI_PERIOD_MS))
        {
            NeedRedraw = 0;

            SpO2_Get(&State.spo2, &State.hr, &State.spo2Valid);
            State.finger  = SpO2_FingerPresent();
            State.fifoErr = SpO2_FifoError();

            State.bodyValid = Temp_Get(&BodyX10);
            State.bodyX10   = BodyX10;

            UI_Render(Page, &State);
        }
    }
}
