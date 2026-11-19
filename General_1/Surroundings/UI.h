#ifndef __UI_H
#define __UI_H

#include "stm32f10x.h"

/* 两级界面（全部 OLED 文案集中在这里，方便和临床合作者逐字校对）
 *
 *   一级：菜单，只列出四项，不采集任何传感器
 *           SW1 上翻 / SW2 下翻 / SW3 进入 / SW4 退出（顶层无效）
 *   二级：子页面，显示该传感器的实测值
 *           SW1 上一个 / SW2 下一个 / SW4 退回菜单
 *           血压页特殊：SW3 = 开始测量；测量中 SW1/SW2/SW3 全部锁死，
 *                       SW4 = 中止（发 A1），再按一次才退回菜单
 *
 * 渲染方式：先在 RAM 里画满 4x16 的 s_shadow，再和 s_shown 逐格比对，
 * 只把真正变化的字符写进 OLED —— 换页无闪、无残字，平时每秒几十字节。
 */

#define UI_ROWS         4
#define UI_COLS         16

/* 菜单项顺序 = 子页面编号，两者共用一套下标 */
typedef enum
{
    UI_PAGE_AMBIENT = 0,        /* SHT3X 环境温湿度 */
    UI_PAGE_BODY,               /* 接触式体温 */
    UI_PAGE_SPO2,               /* 血氧 + 心率 */
    UI_PAGE_BP                  /* 血压：USART2 串口血压计 */
} UI_PageId;

#define UI_ITEM_COUNT   4

/* 一帧要显示的数值，由 main 填好再交给 UI 渲染 */
typedef struct
{
    uint8_t shtOk;              /* 1 = 上一次 SHT3X 测量成功 */
    uint8_t spo2Ok;             /* 1 = MAX30102 初始化成功 */
    uint8_t spo2Valid;          /* 1 = 本窗算出了有效 SpO2 */
    uint8_t finger;             /* 1 = 手指在位 */
    uint8_t fifoErr;            /* 1 = FIFO/I2C 出错 */
    uint8_t spo2;               /* % */
    uint8_t hr;                 /* bpm，0 = 无 */
    int16_t tempX10;            /* 环境温度 0.1℃ */
    int16_t humiX10;            /* 环境湿度 0.1% */
    int16_t bodyX10;            /* 体温 0.1℃ */
    uint8_t bodyValid;          /* 1 = 3 帧一致且 3s 内更新过 */
    uint8_t bodyAlive;          /* 1 = 1s 内收到过帧（传感器在发） */

    /* ---- 血压：bpState 里存的是 BP.h 中 BP_State 的值 ---- */
    uint8_t  bpState;           /* BP_IDLE / BP_CHECKING / BP_MEASURING / ... */
    uint8_t  bpHasProgress;     /* 1 = 本次测量已收到过程帧 */
    uint16_t bpCuff;            /* 袖带气压 mmHg（过程帧 QYL） */
    uint8_t  bpPulse;           /* 1 = 过程帧报告检测到心跳（QYH） */
    uint8_t  bpSys;             /* 收缩压 */
    uint8_t  bpDia;             /* 舒张压 */
    uint8_t  bpHr;              /* 心率 */
    uint8_t  bpIrregular;       /* 1 = 心律不齐 */
    uint8_t  bpFail;            /* 失败码 X */
} UI_State;

/* 必须在 OLED_Clear() 之后调用：把"屏幕上已显示内容"清空 */
void UI_Init(void);

void UI_RenderMenu(uint8_t Cursor);
void UI_RenderPage(uint8_t Page, const UI_State *State);

#endif
