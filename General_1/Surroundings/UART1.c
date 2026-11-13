#include "UART1.h"

#define UART1_RX_SIZE   128     /* 环形缓冲字节数 */

static volatile uint8_t  s_rx[UART1_RX_SIZE];
static volatile uint16_t s_head = 0;    /* 写指针（中断里推进） */
static volatile uint16_t s_tail = 0;    /* 读指针（主循环里推进） */
static volatile uint16_t s_overrun = 0; /* 缓冲满导致丢字节的次数 */

/**
  * @brief  USART1 初始化：重映射到 PB6/PB7、115200 8N1、开 RXNE 中断
  * @note   PA9/PA10 是 USART1 的默认脚，但本项目这两脚给了按钮，
  *         所以必须先开 AFIO 再 GPIO_Remap_USART1。
  */
void UART1_Init(void)
{
	GPIO_InitTypeDef  gpio;
	USART_InitTypeDef usart;
	NVIC_InitTypeDef  nvic;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);	/* USART1 挂在 APB2 */

	/* 注意：这里【绝对不能】调 GPIO_PinRemapConfig(GPIO_Remap_USART1)。
	 * 那个函数会把 AFIO->MAPR 的 SWJ_CFG 位写成 111，也就是重新打开 JTAG，
	 * 之后 PA15（软件 I2C 的 SCL）立刻变回 JTDI、彻底失效。
	 * USART1 的重映射统一在 Board_Init() 里做，且顺序必须在关 JTAG 之前。
	 * 调用顺序：Board_Init() -> UART1_Init() */

	/* PB6 -> USART1_TX */
	gpio.GPIO_Pin   = GPIO_Pin_6;
	gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB, &gpio);

	/* PB7 -> USART1_RX */
	gpio.GPIO_Pin  = GPIO_Pin_7;
	gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(GPIOB, &gpio);

	usart.USART_BaudRate            = 115200;
	usart.USART_WordLength          = USART_WordLength_8b;
	usart.USART_StopBits            = USART_StopBits_1;
	usart.USART_Parity              = USART_Parity_No;
	usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
	USART_Init(USART1, &usart);

	USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

	nvic.NVIC_IRQChannel                   = USART1_IRQn;
	nvic.NVIC_IRQChannelPreemptionPriority = 2;
	nvic.NVIC_IRQChannelSubPriority        = 1;
	nvic.NVIC_IRQChannelCmd                = ENABLE;
	NVIC_Init(&nvic);

	USART_Cmd(USART1, ENABLE);
}

void UART1_SendBuf(const uint8_t *buf, uint16_t len)
{
	uint16_t i;

	for (i = 0; i < len; i++)
	{
		while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
		USART_SendData(USART1, buf[i]);
	}
	while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
}

void UART1_SendStr(const char *str)
{
	while (*str != '\0')
	{
		while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
		USART_SendData(USART1, (uint8_t)(*str));
		str++;
	}
	while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
}

uint8_t UART1_Pop(uint8_t *byte)
{
	if (s_head == s_tail)
	{
		return 0;
	}
	*byte  = s_rx[s_tail];
	s_tail = (uint16_t)((s_tail + 1) % UART1_RX_SIZE);
	return 1;
}

uint16_t UART1_Count(void)
{
	return (uint16_t)((s_head + UART1_RX_SIZE - s_tail) % UART1_RX_SIZE);
}

void UART1_Flush(void)
{
	s_tail = s_head;
}

/**
  * @brief  USART1 中断：只搬运字节，不做协议解析
  * @note   缓冲满时丢弃新字节，但必须把 DR 读走，否则 RXNE 永久置位
  */
void USART1_IRQHandler(void)
{
	uint8_t  byte;
	uint16_t next;

	if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
	{
		byte = (uint8_t)USART_ReceiveData(USART1);
		next = (uint16_t)((s_head + 1) % UART1_RX_SIZE);
		if (next != s_tail)
		{
			s_rx[s_head] = byte;
			s_head = next;
		}
		else
		{
			s_overrun++;
		}
	}

	/* 溢出错误：读 SR 后读 DR 即可清除 */
	if (USART_GetFlagStatus(USART1, USART_FLAG_ORE) != RESET)
	{
		(void)USART_ReceiveData(USART1);
	}
}
