#include "stm32f10x.h"
#include "Tick.h"

/*
 * TIM2 1ms 心跳
 *   APB1 = 36MHz，定时器时钟 = 72MHz
 *   PSC = 72-1   -> 1MHz 计数
 *   ARR = 1000-1 -> 1kHz 更新中断 = 1ms
 *
 * 中断里只做一次自增，别再往里塞任何逻辑：
 * 软件 I2C 的半周期只有 10us，ISR 越长，时序抖动越大。
 */

static volatile uint32_t s_ms = 0;

void Tick_Init(void)
{
    TIM_TimeBaseInitTypeDef tim;
    NVIC_InitTypeDef nvic;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    tim.TIM_Prescaler         = 72 - 1;
    tim.TIM_Period            = 1000 - 1;
    tim.TIM_ClockDivision     = TIM_CKD_DIV1;
    tim.TIM_CounterMode       = TIM_CounterMode_Up;
    tim.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM2, &tim);

    TIM_ClearFlag(TIM2, TIM_FLAG_Update);
    TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);

    nvic.NVIC_IRQChannel                   = TIM2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);

    TIM_Cmd(TIM2, ENABLE);
}

uint32_t Tick_GetMs(void)
{
    return s_ms;
}

uint8_t Tick_Elapsed(uint32_t *lastMs, uint32_t periodMs)
{
    uint32_t now = s_ms;

    if ((uint32_t)(now - *lastMs) >= periodMs)
    {
        *lastMs = now;
        return 1;
    }
    return 0;
}

void TIM2_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
        s_ms++;
    }
}
