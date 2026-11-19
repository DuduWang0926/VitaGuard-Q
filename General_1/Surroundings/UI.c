#include "stm32f10x.h"
#include "OLED.h"
#include "BP.h"
#include "UI.h"

/*
 * 界面层：只负责"把数字画成 4x16 的字符网格"，不碰任何传感器。
 *
 * 为什么要有影子缓冲：
 *   直接调 OLED_ShowString 有两个毛病 ——
 *   1) 换页残留：上一页第 2 行有 16 个字符，新页只有 8 个，后面 8 个不擦就留着；
 *   2) 闪烁：先 OLED_Clear() 再重画，中间会闪一帧全黑。
 *   现在的做法是先在 RAM 里画满 s_shadow，再和 s_shown 逐格比对，
 *   只写真正变化的字符 —— 换页瞬间完成，平时每帧通常只改几个数字。
 */

static char s_shadow[UI_ROWS][UI_COLS];     /* 本帧应当显示的内容 */
static char s_shown [UI_ROWS][UI_COLS];     /* 屏幕上已经显示的内容 */

static const uint32_t s_pow10[5] = {1u, 10u, 100u, 1000u, 10000u};

static const char * const s_menuName[UI_ITEM_COUNT] =
{
    "Ambient",
    "Body Temp",
    "SpO2 & HR",
    "Blood Pres"
};

/* ------------------------------------------------------------------ */
/* 绘制原语（全部写进 s_shadow，不碰屏幕）                                */
/* ------------------------------------------------------------------ */

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

/* 显示 "±xxx.y"，占宽 = 1(符号) + IntWidth + 2 */
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

/* 把影子缓冲同步到屏幕：只写有变化的字符 */
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

/* ------------------------------------------------------------------ */
/* 一级菜单                                                            */
/* ------------------------------------------------------------------ */

static void UI_MenuBody(uint8_t Cursor)
{
    uint8_t i;

    for (i = 0; i < UI_ITEM_COUNT; i++)
    {
        UI_Char((uint8_t)(i + 1u), 1, (i == Cursor) ? '>' : ' ');
        UI_Text((uint8_t)(i + 1u), 2, s_menuName[i]);
    }
}

/* ------------------------------------------------------------------ */
/* 二级子页面                                                          */
/* ------------------------------------------------------------------ */

/* 1/4 环境温湿度 */
static void UI_PageAmbient(const UI_State *State)
{
    UI_Text(1, 1, "1/4 Ambient");           /* 11 列，留 5 列给未来状态位 */

    UI_Text(2, 1, "Temp:");
    if (State->shtOk)
    {
        UI_Fixed1(2, 6, State->tempX10, 3); /* 6..11，整数 3 位（SHT3X 上限 125℃） */
        UI_Char(2, 12, 'C');
    }
    else
    {
        UI_Text(2, 6, "  --.- C");
    }

    UI_Text(3, 1, "Humi:");
    if (State->shtOk)
    {
        UI_Fixed1(3, 6, State->humiX10, 3);
        UI_Char(3, 12, '%');
    }
    else
    {
        UI_Text(3, 6, "  --.- %");
    }

    UI_Text(4, 1, "SW4: back");
}

/* 2/4 接触体温 */
static void UI_PageBody(const UI_State *State)
{
    UI_Text(1, 1, "2/4 Body Temp");

    UI_Text(2, 1, "Body:");
    if (State->bodyValid)
    {
        UI_Fixed1(2, 6, State->bodyX10, 2); /* 6..10，体温两位整数足够 */
        UI_Char(2, 11, 'C');
    }
    else
    {
        UI_Text(2, 6, "  --.- C");
    }

    /* 第三行专门说清"为什么没数"，避免把没接和没稳定混为一谈 */
    if (!State->bodyAlive)
        UI_Text(3, 1, "No sensor");
    else if (!State->bodyValid)
        UI_Text(3, 1, "Waiting...");
    else
        UI_Text(3, 1, "Stable OK");

    UI_Text(4, 1, "SW4: back");
}

/* 3/4 血氧 + 心率 */
static void UI_PageSpO2(const UI_State *State)
{
    UI_Text(1, 1, "3/4 SpO2 & HR");

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

    UI_Text(4, 1, "SW4: back");
}

/* 4/4 血压 —— 按模块状态分屏，测量中实时显示袖带气压 */
static void UI_PageBloodPressure(const UI_State *State)
{
    UI_Text(1, 1, "4/4 Blood Pres");

    switch (State->bpState)
    {
    case BP_CHECKING:
        UI_Text(2, 1, "Checking...");
        UI_Text(3, 1, "BP device");
        UI_Text(4, 1, "SW4: stop");
        break;

    case BP_MEASURING:
        if (State->bpHasProgress)
        {
            UI_Text(2, 1, "Cuff:");
            UI_UInt(2, 7, State->bpCuff, 3);
            UI_Text(2, 11, "mmHg");
            UI_Text(3, 1, State->bpPulse ? "Pulse: yes" : "Pulse: --");
        }
        else
        {
            UI_Text(2, 1, "Starting...");
            UI_Text(3, 1, "Inflating");
        }
        UI_Text(4, 1, "SW4: stop");
        break;

    case BP_DONE:
        UI_Text(2, 1, "SYS:");
        UI_UInt(2, 5, State->bpSys, 3);
        UI_Text(2, 9, "DIA:");
        UI_UInt(2, 13, State->bpDia, 3);
        UI_Text(3, 1, "HR:");
        UI_UInt(3, 4, State->bpHr, 3);
        UI_Text(3, 8, State->bpIrregular ? "IR:1" : "IR:0");
        UI_Text(4, 1, "SW3: redo");
        break;

    case BP_FAILED:
        UI_Text(2, 1, "Failed");
        switch (State->bpFail)
        {
        case 0:  UI_Text(3, 1, "No pulse found");  break;
        case 1:  UI_Text(3, 1, "Pressure low");    break;
        case 2:  UI_Text(3, 1, "Abnormal result"); break;
        case 3:  UI_Text(3, 1, "Over pressure");   break;
        case 4:  UI_Text(3, 1, "Too much move");   break;
        default: UI_Text(3, 1, "Unknown");         break;
        }
        UI_Text(4, 1, "SW3: retry");
        break;

    case BP_OFFLINE:
        UI_Text(2, 1, "No BP device");
        UI_Text(3, 1, "Check PA2/PA3");
        UI_Text(4, 1, "SW3: retry");
        break;

    case BP_TIMEOUT:
        UI_Text(2, 1, "Timeout");
        UI_Text(3, 1, "120s no result");    /* 16 列放不下 "No result in 120s" */
        UI_Text(4, 1, "SW3: retry");
        break;

    case BP_ABORTED:
        UI_Text(2, 1, "Stopped");
        UI_Text(3, 1, "Cuff deflating");
        UI_Text(4, 1, "SW3: start");
        break;

    default:                        /* BP_IDLE */
        UI_Text(2, 1, "Ready");
        UI_Text(3, 1, "Wear the cuff");
        UI_Text(4, 1, "SW3: start");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void UI_Init(void)
{
    uint8_t r, c;

    /* 调用前必须 OLED_Clear()，所以认为屏幕是空的 */
    for (r = 0; r < UI_ROWS; r++)
    {
        for (c = 0; c < UI_COLS; c++)
        {
            s_shown[r][c] = '\0';
        }
    }

    UI_ClearBuffer();
}

void UI_RenderMenu(uint8_t Cursor)
{
    UI_ClearBuffer();
    UI_MenuBody(Cursor);
    UI_Flush();
}

void UI_RenderPage(uint8_t Page, const UI_State *State)
{
    UI_ClearBuffer();

    switch (Page)
    {
        case UI_PAGE_AMBIENT: UI_PageAmbient(State);        break;
        case UI_PAGE_BODY:    UI_PageBody(State);           break;
        case UI_PAGE_SPO2:    UI_PageSpO2(State);           break;
        default:              UI_PageBloodPressure(State);  break;
    }

    UI_Flush();
}
