#include "UART2.h"

#define UART2_RX_SIZE   128     /* 环形缓冲字节数，必须能被用于取模运算 */

static volatile uint8_t  s_rx[UART2_RX_SIZE];
static volatile uint16_t s_head = 0;    /* 写指针（中断里推进） */
static volatile uint16_t s_tail = 0;    /* 读指针（主循环里推进） */
static volatile uint16_t s_overrun = 0; /* 缓冲满导致丢字节的次数 */

/**
  * @brief  USART2 初始化：PA2 复用推挽输出、PA3 浮空输入、115200 8N1、开 RXNE 中断
  */
void UART2_Init(void)
{
	GPIO_InitTypeDef  gpio;
	USART_InitTypeDef usart;
	NVIC_InitTypeDef  nvic;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);	/* USART2 挂在 APB1 */

	/* PA2 -> USART2_TX */
	gpio.GPIO_Pin   = GPIO_Pin_2;
	gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &gpio);

	/* PA3 -> USART2_RX */
	gpio.GPIO_Pin  = GPIO_Pin_3;
	gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(GPIOA, &gpio);

	usart.USART_BaudRate            = 115200;
	usart.USART_WordLength          = USART_WordLength_8b;
	usart.USART_StopBits            = USART_StopBits_1;
	usart.USART_Parity              = USART_Parity_No;
	usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	usart.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
	USART_Init(USART2, &usart);

	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
	nvic.NVIC_IRQChannel                   = USART2_IRQn;
	nvic.NVIC_IRQChannelPreemptionPriority = 2;
	nvic.NVIC_IRQChannelSubPriority        = 2;
	nvic.NVIC_IRQChannelCmd                = ENABLE;
	NVIC_Init(&nvic);

	USART_Cmd(USART2, ENABLE);
}

/**
  * @brief  阻塞发送任意长度字节流
  * @note   每个字节等 TXE（发送数据寄存器空），最后再等 TC（发送完成）
  */
void UART2_SendBuf(const uint8_t *buf, uint16_t len)
{
	uint16_t i;

	for (i = 0; i < len; i++)
	{
		while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
		USART_SendData(USART2, buf[i]);
	}
	while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
}

/**
  * @brief  阻塞发送以 '\0' 结尾的字符串
  */
void UART2_SendStr(const char *str)
{
	while (*str != '\0')
	{
		while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
		USART_SendData(USART2, (uint8_t)(*str));
		str++;
	}
	while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
}

/**
  * @brief  从环形缓冲取一个字节
  * @retval 1=取到，0=缓冲为空
  */
uint8_t UART2_Pop(uint8_t *byte)
{
	if (s_head == s_tail)
	{
		return 0;
	}
	*byte  = s_rx[s_tail];
	s_tail = (uint16_t)((s_tail + 1) % UART2_RX_SIZE);
	return 1;
}

/**
  * @brief  缓冲中待读取的字节数
  */
uint16_t UART2_Count(void)
{
	return (uint16_t)((s_head + UART2_RX_SIZE - s_tail) % UART2_RX_SIZE);
}

/**
  * @brief  清空接收缓冲
  */
void UART2_Flush(void)
{
	s_tail = s_head;
}

/**
  * @brief  USART2 中断：只搬运字节，不做协议解析
  * @note   缓冲满时丢弃新字节，但必须把 DR 读走，否则 RXNE 永久置位并触发 ORE
  */
void USART2_IRQHandler(void)
{
	uint8_t  byte;
	uint16_t next;

	if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
	{
		byte = (uint8_t)USART_ReceiveData(USART2);
		next = (uint16_t)((s_head + 1) % UART2_RX_SIZE);
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

	/* 溢出错误：读 SR 后读 DR 即可清除，不处理会一直卡住接收 */
	if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET)
	{
		(void)USART_ReceiveData(USART2);
	}
}
