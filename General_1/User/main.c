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
#include "BP.h"
#include "Key.h"
#include "Tick.h"
#include "UI.h"

/*
 * VitaGuard-Q —— 第三版：两级菜单 + 按需采集
 *
 * 一级菜单只列功能，不采集任何传感器；进入子项后才启动对应传感器。
 * 这样做直接把主循环占用从"三项常驻 ≈52%"降到"菜单态 <2%"：
 *   血氧  SpO2_Task()        45%  → 只在血氧页跑
 *   温湿度 SHT3X_GTMeasure() 4.5% → 只在环境页跑（且它是阻塞 22ms 的）
 *   体温  Temp_Poll()        ~0%  → 常驻（见下）
 *
 * 体温为什么不能停：USART1 靠 128 字节环形缓冲收帧，115200 下约 1.3KB/s，
 * 缓冲 100ms 就会满。停掉排空只会让缓冲溢出丢帧，而排空成本本来就 <0.1ms，
 * 所以它照旧每轮跑，属于必要开销。
 *
 * 血压为什么不阻塞：旧版 BloodPressure_2 的 BP_Start() 用 Delay_ms(500) 循环
 * 120 次，最长阻塞 60 秒，期间按键、体温串口、刷屏全部停摆。这里改成
 * BP_Begin() 只发命令 + BP_Task() 每轮解析，永不阻塞。
 * 附带好处：主循环间隔始终 <30ms，远小于 MAX30102 FIFO 的 320ms 余量，
 * 所以"血压阻塞导致血氧 FIFO 溢出"这个老问题自动消失了。
 *
 * 引脚：
 *   屏幕        PB8/PB9    软件 I2C（SCL/SDA 已互换，见 OLED.c）
 *   传感器 I2C  PA15/PA12  SHT3X(0x44) + MAX30102(0x57)
 *   USART1      PB6/PB7    接触温度传感器（重映射，115200 8N1）
 *   USART2      PA2/PA3    串口血压计（默认脚，无需重映射，115200 8N1）
 *   按键        PA8=SW1 上翻  PA9=SW2 下翻  PA10=SW3 进入/开始测血压  PA11=SW4 退出/中止
 *
 * 初始化顺序不能乱：
 *   Board_Init()  ← 先把所有重映射做完，最后关 JTAG（PA15 才可用）
 *   → OLED / 按键 / 传感器
 *   任何"普通重映射"都必须留在 Board_Init 里，否则会把 JTAG 重新打开。
 */

#define SHT_PERIOD_MS     500
#define UI_PERIOD_MS      200
#define KEY_PERIOD_MS     10

#define MODE_MENU         0
#define MODE_PAGE         1

/* ------------------------------------------------------------------ */
/* 界面状态（文件内私有）                                                */
/* ------------------------------------------------------------------ */

static UI_State s_state;
static uint8_t  s_mode;
static uint8_t  s_cursor;           /* 一级菜单光标 0~3 */
static uint8_t  s_page;             /* 当前子页面 */
static uint8_t  s_spo2Ok;           /* MAX30102 是否初始化成功 */
static uint8_t  s_shtOk;
static uint8_t  s_needRedraw;

static uint32_t s_lastShtMs;

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
/* 状态机                                                              */
/* ------------------------------------------------------------------ */

/*
 * 进入某个子页面。这里是"按需采集"的开关点：
 * 页面专用的传感器在这里重置成干净状态，离开页面后就不再有读取开销。
 */
static void State_EnterPage(uint8_t Page, uint32_t Now)
{
    s_page = Page;

    if (Page == UI_PAGE_AMBIENT)
    {
        /* 立刻测一次，别让用户进来先看到 500ms 的 --.- */
        s_lastShtMs = (uint32_t)(Now - SHT_PERIOD_MS);
    }
    else if (Page == UI_PAGE_SPO2)
    {
        /*
         * 开机自检失败过的话，进页面再试一次 —— 否则只能断电重启。
         * 常见的失败原因是上电瞬间总线没准备好（现在由 MyI2C_Init 的
         * 9 时钟恢复 + Max30102 的 3 次重试化解），但现场重新插拔模块后
         * 同样需要这个"手动重试"入口。
         */
        if (!s_spo2Ok)
        {
            s_spo2Ok       = SpO2_Init();
            s_state.spo2Ok = s_spo2Ok;
        }

        /*
         * 菜单态不读 MAX30102，但芯片仍在按 100Hz 采样，FIFO 只有 32 深度
         * （ROLLOVER 已关闭）→ 320ms 后必然溢出。所以每次进来先清 FIFO、
         * 重置算法窗口，否则头几秒读到的是陈旧的溢出数据。
         * 芯片没初始化成功就跳过：那会白跑一遍 I2C 并把 ErrorStep 改写成 30，
         * 反倒把开机自检留下的真实错误码掩盖掉。
         */
        if (s_spo2Ok)
        {
            SpO2_Restart();
        }
    }
    else if (Page == UI_PAGE_BP)
    {
        /* 进页面清掉上次结果，回到 Ready；只清状态，不动串口配置 */
        BP_Reset();
    }
    /* 体温：串口一直在后台收，进页面直接就有值，无需额外动作 */
}

/* 每个主循环调用一次：只刷新"当前可见"的那一路传感器 */
static void State_Update(uint32_t Now)
{
    /* 体温排空常驻，理由见文件头注释 */
    (void)Temp_Poll(Now);

    /*
     * 血压解析常驻：BP_Task() 只是把环形缓冲里的字节取出来喂给帧状态机，
     * 几百纳秒量级，永不阻塞。常驻的好处是即使不在血压页也不会积压，
     * 且 1s/60s 两级超时能按时生效。
     */
    BP_Task(Now);

    if (s_mode != MODE_PAGE)
    {
        return;                     /* 菜单态：不读任何传感器 */
    }

    if (s_page == UI_PAGE_AMBIENT)
    {
        if (Tick_Elapsed(&s_lastShtMs, SHT_PERIOD_MS))
        {
            float Temp = 0.0f;
            float Humi = 0.0f;

            if (SHT3X_GTMeasure(&Temp, &Humi))
            {
                s_shtOk         = 1;
                s_state.tempX10 = FloatToX10(Temp);
                s_state.humiX10 = FloatToX10(Humi);
            }
            else
            {
                s_shtOk = 0;
            }
            s_state.shtOk = s_shtOk;
        }
    }
    else if (s_page == UI_PAGE_SPO2)
    {
        if (s_spo2Ok)
        {
            (void)SpO2_Task();
        }
    }
}

/* 把当前状态交给 UI 层画出来 */
static void State_Render(void)
{
    BP_Result Bp;

    if (s_mode == MODE_MENU)
    {
        UI_RenderMenu(s_cursor);
        return;
    }

    if (s_page == UI_PAGE_SPO2)
    {
        SpO2_Get(&s_state.spo2, &s_state.hr, &s_state.spo2Valid);
        s_state.finger  = SpO2_FingerPresent();
        s_state.fifoErr = SpO2_FifoError();
    }

    /* 血压：模块状态 + 过程帧（袖带气压/心跳）+ 终帧结果 */
    s_state.bpState       = (uint8_t)BP_GetState();
    s_state.bpHasProgress = BP_GetProgress(&s_state.bpCuff, &s_state.bpPulse);
    s_state.bpFail        = BP_GetFailCode();
    if (BP_GetResult(&Bp))
    {
        s_state.bpSys       = Bp.Systolic;
        s_state.bpDia       = Bp.Diastolic;
        s_state.bpHr        = Bp.HeartRate;
        s_state.bpIrregular = Bp.Irregular;
    }

    s_state.bodyValid = Temp_Get(&s_state.bodyX10);
    s_state.bodyAlive = Temp_SensorAlive();

    UI_RenderPage(s_page, &s_state);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    uint32_t LastUiMs;
    uint32_t LastKeyMs;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

    /* 1. 重映射一步到位（内部最后才关 JTAG） */
    Board_Init();

    Tick_Init();
    OLED_Init();
    Key_Init();
    MyI2C_Init();
    UART1_Init();
    BP_Init();                      /* USART2 + 清掉血压计上电噪声，不碰屏幕 */

    s_mode       = MODE_MENU;
    s_cursor     = UI_PAGE_AMBIENT;
    s_page       = UI_PAGE_AMBIENT;
    s_shtOk      = 0;
    s_needRedraw = 1;

    SHT3X_Init();
    Temp_Init();
    s_spo2Ok = SpO2_Init();

    /* 开机自检画面直接用 OLED，绕过界面层，随后清干净 */
    OLED_ShowString(1, 1, "VitaGuard-Q     ");
    OLED_ShowString(2, 1, "Init sensors... ");
    if (s_spo2Ok)
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

    UI_Init();                      /* 必须在 OLED_Clear() 之后 */

    s_state.shtOk     = 0;
    s_state.spo2Ok    = s_spo2Ok;
    s_state.spo2Valid = 0;
    s_state.finger    = 0;
    s_state.fifoErr   = 0;
    s_state.spo2      = 0;
    s_state.hr        = 0;
    s_state.tempX10   = 0;
    s_state.humiX10   = 0;
    s_state.bodyX10   = 0;
    s_state.bodyValid = 0;
    s_state.bodyAlive = 0;
    s_state.bpState       = (uint8_t)BP_IDLE;
    s_state.bpHasProgress = 0;
    s_state.bpCuff        = 0;
    s_state.bpPulse       = 0;
    s_state.bpSys         = 0;
    s_state.bpDia         = 0;
    s_state.bpHr          = 0;
    s_state.bpIrregular   = 0;
    s_state.bpFail        = 0;

    s_lastShtMs = Tick_GetMs();
    LastUiMs    = s_lastShtMs;
    LastKeyMs   = s_lastShtMs;

    while (1)
    {
        uint32_t now = Tick_GetMs();

        /* ---- 1. 按键：10ms 扫一次（消抖需要周期调用） ---- */
        if ((uint32_t)(now - LastKeyMs) >= KEY_PERIOD_MS)
        {
            uint8_t events;

            LastKeyMs = now;
            events = Key_Scan(now);

            if (s_mode == MODE_MENU)
            {
                if ((events & KEY_EVENT_SW1) != 0)          /* 上翻 */
                {
                    s_cursor = (uint8_t)((s_cursor + UI_ITEM_COUNT - 1u) % UI_ITEM_COUNT);
                    s_needRedraw = 1;
                }

                if ((events & KEY_EVENT_SW2) != 0)          /* 下翻 */
                {
                    s_cursor = (uint8_t)((s_cursor + 1u) % UI_ITEM_COUNT);
                    s_needRedraw = 1;
                }

                if ((events & KEY_EVENT_SW3) != 0)          /* 进入 */
                {
                    s_mode = MODE_PAGE;
                    State_EnterPage(s_cursor, now);
                    s_needRedraw = 1;
                }

                /* SW4 已经在顶层，退回无处可去，忽略 */
            }
            else if (BP_Busy())
            {
                /*
                 * 血压测量中：SW1/SW2/SW3 全部锁死，只有 SW4 能中止。
                 * 袖带正加压到 180+ mmHg，误按翻页会让人以为测量停了，
                 * 而实际还在加压 —— 宁可"按键没反应"也不要误操作。
                 */
                if ((events & KEY_EVENT_SW4) != 0)
                {
                    BP_Abort();                         /* 发 A1，让血压计立刻放气 */
                    s_needRedraw = 1;
                }
            }
            else
            {
                if ((events & KEY_EVENT_SW4) != 0)          /* 退回菜单 */
                {
                    s_mode = MODE_MENU;
                    s_cursor = s_page;
                    s_needRedraw = 1;
                }
                else if ((events & KEY_EVENT_SW1) != 0)     /* 上一个子项 */
                {
                    uint8_t prev = (uint8_t)((s_page + UI_ITEM_COUNT - 1u) % UI_ITEM_COUNT);

                    s_cursor = prev;
                    State_EnterPage(prev, now);
                    s_needRedraw = 1;
                }
                else if ((events & KEY_EVENT_SW2) != 0)     /* 下一个子项 */
                {
                    uint8_t next = (uint8_t)((s_page + 1u) % UI_ITEM_COUNT);

                    s_cursor = next;
                    State_EnterPage(next, now);
                    s_needRedraw = 1;
                }
                else if ((events & KEY_EVENT_SW3) != 0)     /* 血压页：开始测量 */
                {
                    if (s_page == UI_PAGE_BP)
                    {
                        (void)BP_Begin(now);
                        s_needRedraw = 1;
                    }
                }
            }
        }

        /* ---- 2. 采集：只跑当前页面对应的传感器 ---- */
        State_Update(now);

        /* ---- 3. 刷屏：200ms 一次，换页/翻选时立刻重画 ---- */
        if (s_needRedraw || Tick_Elapsed(&LastUiMs, UI_PERIOD_MS))
        {
            s_needRedraw = 0;
            State_Render();
        }
    }
}
