#include "stm32f10x.h"
#include "OLED.h"
#include "Delay.h"

/* Pin assignment:
 *   USART1: PA10 <- sensor TX, PA9 -> sensor RX (115200, 8N1)
 *   OLED:   PB8 = SCL, PB9 = SDA (implemented by OLED.c)
 *   KEY:    PA0, active low, internal pull-up
 */
#define KEY_PORT        GPIOA
#define KEY_PIN         GPIO_Pin_0

static void Key_Init(void)
{
	GPIO_InitTypeDef gpio;
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	gpio.GPIO_Pin = KEY_PIN;
	gpio.GPIO_Mode = GPIO_Mode_IPU;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(KEY_PORT, &gpio);
}

static void Sensor_UART_Init(void)
{
	GPIO_InitTypeDef gpio;
	USART_InitTypeDef usart;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1, ENABLE);

	/* PA9 is optional for this sensor, but is configured for a complete UART. */
	gpio.GPIO_Pin = GPIO_Pin_9;
	gpio.GPIO_Mode = GPIO_Mode_AF_PP;
	gpio.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &gpio);

	gpio.GPIO_Pin = GPIO_Pin_10;
	gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(GPIOA, &gpio);

	USART_StructInit(&usart);
	usart.USART_BaudRate = 115200;
	usart.USART_WordLength = USART_WordLength_8b;
	usart.USART_StopBits = USART_StopBits_1;
	usart.USART_Parity = USART_Parity_No;
	usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
	USART_Init(USART1, &usart);
	USART_Cmd(USART1, ENABLE);
}

/* Return one byte if available, otherwise 0. */
static uint8_t Sensor_ReadByte(uint8_t *byte)
{
	if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) == RESET)
		return 0;
	*byte = (uint8_t)USART_ReceiveData(USART1);
	return 1;
}

/* Protocol: A+XX.XB\r\n or A-XX.XB\r\n. Value is returned in tenths of a degree. */
static uint8_t Sensor_ParseByte(uint8_t byte, int16_t *value)
{
	static uint8_t frame[9];
	static uint8_t index = 0;
	uint8_t i;
	int16_t integer_part;

	if (index == 0 && byte != 'A')
		return 0;
	frame[index++] = byte;
	if (index < 9)
		return 0;
	index = 0;

	if (frame[0] != 'A' || (frame[1] != '+' && frame[1] != '-') ||
		frame[4] != '.' || frame[6] != 'B' || frame[7] != '\r' || frame[8] != '\n')
		return 0;
	for (i = 2; i <= 3; i++)
		if (frame[i] < '0' || frame[i] > '9') return 0;
	if (frame[5] < '0' || frame[5] > '9')
		return 0;
	integer_part = (int16_t)((frame[2] - '0') * 10 + frame[3] - '0');
	*value = (int16_t)(integer_part * 10 + frame[5] - '0');
	if (frame[1] == '-') *value = -*value;
	return 1;
}

static void OLED_ShowTemperature(int16_t value)
{
	uint16_t magnitude;
	if (value < 0) {
		OLED_ShowChar(2, 1, '-');
		magnitude = (uint16_t)(-value);
	} else {
		OLED_ShowChar(2, 1, '+');
		magnitude = (uint16_t)value;
	}
	OLED_ShowNum(2, 2, magnitude / 10, 2);
	OLED_ShowChar(2, 4, '.');
	OLED_ShowNum(2, 5, magnitude % 10, 1);
	OLED_ShowChar(2, 6, 'C');
}

int main(void)
{
	uint8_t byte;
	uint8_t pressed = 0;
	uint8_t stable_count = 0;
	int16_t sample = 0;
	int16_t stable_value = 0;

	Key_Init();
	Sensor_UART_Init();
	OLED_Init();
	OLED_ShowString(1, 1, "Temperature:");
	OLED_ShowString(3, 1, "Press KEY");

	while (1) {
		/* Detect one press, with debounce. */
		if (GPIO_ReadInputDataBit(KEY_PORT, KEY_PIN) == Bit_RESET) {
			if (pressed == 0) {
				pressed = 1;
				stable_count = 0;
				/* Discard an incomplete/old frame before this measurement. */
				while (Sensor_ReadByte(&byte)) { (void)byte; }
			}
		} else {
			pressed = 0;
		}

		if (pressed == 1 && Sensor_ReadByte(&byte) && Sensor_ParseByte(byte, &sample)) {
			if (stable_count == 0 || sample == stable_value) {
				stable_value = sample;
				if (stable_count < 3) stable_count++;
			} else {
				stable_count = 1;
				stable_value = sample;
			}
			/* Three identical complete frames constitute one stable result. */
			if (stable_count >= 3) {
				OLED_ShowTemperature(stable_value);
				/* State 2 prevents retriggering while the key is held down. */
				pressed = 2;
				stable_count = 0;
			}
		}
	}
}
