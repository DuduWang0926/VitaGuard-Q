#include "stm32f10x.h"
#include "Max30102.h"
#include "SpO2.h"

/*
 * SpO2 算法（移植自 血氧检测仪/User/main.c，并做了三处修正）
 *
 * 相对原版的改动：
 *   1) 手指移开/放上时重置统计窗口，避免"拔手指那一下"产生一个假 SpO2；
 *   2) 无手指时不累计样本，窗口从干净状态开始；
 *   3) 顺带输出心率（窗内峰数 * 30），原版数了峰却丢掉了。
 *
 * 注意：本算法的"2 秒窗"是按 200 个样本算的，不是按时间算的。
 * 主循环被 SHT3X（约 20~50ms）或 OLED 刷新占住时，实际窗口会略长于 2s，
 * 心率因此会偏低几个 bpm。要精确就得上 TIM 时间戳，属于后续优化。
 */

#define SPO2_WINDOW_SIZE      200       /* 100Hz 下约 2s */
#define SPO2_FINGER_IR_MIN    10000     /* IR 直流低于此值视为没放手指 */
#define SPO2_AC_MIN           100       /* 交流峰峰值下限，滤掉无脉搏的弱信号 */

#define SPO2_HR_MIN           30
#define SPO2_HR_MAX           220

static uint32_t s_redRaw;
static uint32_t s_irRaw;

static uint32_t s_redFilt;
static uint32_t s_irFilt;
static uint32_t s_irPrev;
static int32_t  s_irOldChange;
static uint8_t  s_filtInit;

static uint32_t s_redMin;
static uint32_t s_redMax;
static uint32_t s_irMin;
static uint32_t s_irMax;
static uint32_t s_redSum;
static uint32_t s_irSum;
static uint16_t s_count;
static uint8_t  s_peakCount;

static uint8_t s_spo2;
static uint8_t s_hr;
static uint8_t s_valid;
static uint8_t s_finger;
static uint8_t s_fingerPrev;
static uint8_t s_fifoErr;

static void SpO2_ResetWindow(void)
{
    s_redMin = 0xFFFFFFFFu;
    s_redMax = 0;
    s_irMin  = 0xFFFFFFFFu;
    s_irMax  = 0;
    s_redSum = 0;
    s_irSum  = 0;
    s_count  = 0;
    s_peakCount = 0;
}

/*
 * 处理一个样本。返回 1 表示"一个窗口结束、有新结果"。
 */
static uint8_t SpO2_ProcessSample(uint32_t redRaw, uint32_t irRaw)
{
    int32_t  change;
    uint32_t redDC, irDC, redAC, irAC;
    uint32_t ratioX1000, calc;
    unsigned long long num, den;

    /* ---- 一阶 IIR：filtered = 0.75*old + 0.25*new（整数实现） ---- */
    if (!s_filtInit)
    {
        s_redFilt   = redRaw;
        s_irFilt    = irRaw;
        s_irPrev    = s_irFilt;
        s_irOldChange = 0;
        s_filtInit  = 1;
    }
    else
    {
        s_redFilt = (3u * s_redFilt + redRaw) / 4u;
        s_irFilt  = (3u * s_irFilt  + irRaw ) / 4u;

        change = (int32_t)s_irFilt - (int32_t)s_irPrev;

        /* 斜率由正变负 = 一个脉搏峰 */
        if (change < 0 && s_irOldChange > 0 && s_peakCount < 0xFF)
            s_peakCount++;

        s_irPrev = s_irFilt;

        /* 整数滤波在峰顶可能出现 0 斜率，记住最近一次非零方向以免漏峰 */
        if (change != 0)
            s_irOldChange = change;
    }

    /* ---- 手指检测（用滤波后的瞬时值，比按窗口判断快得多） ---- */
    s_finger = (s_irFilt >= SPO2_FINGER_IR_MIN) ? 1 : 0;
    if (s_finger != s_fingerPrev)
    {
        s_fingerPrev = s_finger;
        SpO2_ResetWindow();         /* 手放上 / 移开，都从干净窗口重新开始 */
        s_valid = 0;
        s_hr    = 0;
    }
    if (!s_finger)
        return 0;

    /* ---- 窗口统计 ---- */
    if (s_redFilt < s_redMin) s_redMin = s_redFilt;
    if (s_redFilt > s_redMax) s_redMax = s_redFilt;
    if (s_irFilt  < s_irMin ) s_irMin  = s_irFilt;
    if (s_irFilt  > s_irMax ) s_irMax  = s_irFilt;

    s_redSum += s_redFilt;
    s_irSum  += s_irFilt;
    s_count++;

    if (s_count < SPO2_WINDOW_SIZE)
        return 0;

    /* ---- 窗口结束，算一次结果 ---- */
    redDC = s_redSum / SPO2_WINDOW_SIZE;
    irDC  = s_irSum  / SPO2_WINDOW_SIZE;
    redAC = s_redMax - s_redMin;
    irAC  = s_irMax  - s_irMin;

    if (redDC > 0 && irDC > 0 && redAC >= SPO2_AC_MIN && irAC >= SPO2_AC_MIN)
    {
        num = (unsigned long long)redAC * irDC * 1000u;
        den = (unsigned long long)irAC * redDC;

        if (den != 0)
        {
            ratioX1000 = (uint32_t)(num / den);

            if (ratioX1000 > 0 && ratioX1000 < 2000u)
            {
                calc = 110u - (25u * ratioX1000 + 500u) / 1000u;
                if (calc > 100u) calc = 100u;

                s_spo2  = (uint8_t)calc;
                s_valid = 1;
            }
        }
    }

    /* 200 样本 = 2s，峰数 * 30 即 bpm；超出 30~180 认为是噪声 */
    if (s_valid && s_peakCount >= 1 && s_peakCount <= 6)
        s_hr = (uint8_t)(s_peakCount * 30u);
    else
        s_hr = 0;

    SpO2_ResetWindow();
    return 1;
}

uint8_t SpO2_Init(void)
{
    s_filtInit   = 0;
    s_finger     = 0;
    s_fingerPrev = 0;
    s_spo2       = 0;
    s_hr         = 0;
    s_valid      = 0;
    s_fifoErr    = 0;
    SpO2_ResetWindow();

    return Max30102_STMeasure_SPO2();
}

uint8_t SpO2_Task(void)
{
    uint8_t count;
    uint8_t newResult = 0;

    count = Max30102_GetFIFOCount();

    if (count == 0xFF)
    {
        s_fifoErr = 1;
        return 0;
    }

    s_fifoErr = 0;

    /*
     * 一次把积压样本读空。这里用本地 count 递减，不再每读一个就重读一次
     * 指针寄存器 —— 每重读一次多花约 2.4ms，100Hz 下预算只有 10ms/样本，
     * 省掉它才能留出余量。
     */
    while (count > 0)
    {
        if (!Max30102_ReadFIFO(&s_redRaw, &s_irRaw))
        {
            /* ErrorStep 41 = FIFO 恰好被取空，属于正常，不算错误 */
            if (Max30102_ErrorStep != 41)
                s_fifoErr = 1;
            break;
        }

        if (SpO2_ProcessSample(s_redRaw, s_irRaw))
            newResult = 1;

        count--;
    }

    return newResult;
}

void SpO2_Get(uint8_t *spo2, uint8_t *hr, uint8_t *valid)
{
    if (spo2  != 0) *spo2  = s_spo2;
    if (hr    != 0) *hr    = s_hr;
    if (valid != 0) *valid = s_valid;
}

uint8_t SpO2_FingerPresent(void)
{
    return s_finger;
}

uint8_t SpO2_FifoError(void)
{
    return s_fifoErr;
}
