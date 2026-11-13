#include "stm32f10x.h"
#include "Key.h"

/*
 * 四个按键都在 GPIOA：PA8 / PA9 / PA10 / PA11
 * 注意 PA9/PA10 是 USART1 的默认引脚，本项目已把 USART1 重映射到 PB6/PB7
 * （见 Board_Init），所以这里可以放心当普通输入用。
 */

#define KEY_PORT            GPIOA
#define KEY_COUNT           4
#define KEY_DEBOUNCE_MS     20

static const uint16_t s_pins[KEY_COUNT] =
{
	GPIO_Pin_8, GPIO_Pin_9, GPIO_Pin_10, GPIO_Pin_11
};

static uint8_t  s_raw[KEY_COUNT];        /* 最近一次原始电平 1=松开 */
static uint8_t  s_stable[KEY_COUNT];     /* 消抖后的电平 */
static uint32_t s_changeMs[KEY_COUNT];   /* 原始电平最后一次变化的时刻 */

void Key_Init(void)
{
	GPIO_InitTypeDef gpio;
	uint8_t i;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

	gpio.GPIO_Mode  = GPIO_Mode_IPU;     /* 内部上拉，按键另一端接地 */
	gpio.GPIO_Speed = GPIO_Speed_50MHz;

	for (i = 0; i < KEY_COUNT; i++)
	{
		gpio.GPIO_Pin = s_pins[i];
		GPIO_Init(KEY_PORT, &gpio);

		s_raw[i]      = 1;
		s_stable[i]   = 1;
		s_changeMs[i] = 0;
	}
}

uint8_t Key_Scan(uint32_t nowMs)
{
	uint8_t events = 0;
	uint8_t i;
	uint8_t level;

	for (i = 0; i < KEY_COUNT; i++)
	{
		level = GPIO_ReadInputDataBit(KEY_PORT, s_pins[i]);

		if (level != s_raw[i])
		{
			/* 电平在抖，重新计时 */
			s_raw[i]      = level;
			s_changeMs[i] = nowMs;
		}
		else if ((uint32_t)(nowMs - s_changeMs[i]) >= KEY_DEBOUNCE_MS)
		{
			/* 稳定够久了，认可这个电平 */
			if (s_stable[i] != level)
			{
				s_stable[i] = level;

				/* 只有"松开 -> 按下"才产生事件，按住不会连发 */
				if (level == 0)
				{
					events |= (uint8_t)(1u << i);
				}
			}
		}
	}

	return events;
}

uint8_t Key_IsDown(uint8_t Index)
{
	if (Index >= KEY_COUNT) return 0;
	return (uint8_t)(s_stable[Index] == 0);
}
