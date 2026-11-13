#include "stm32f10x.h"
#include "UART1.h"
#include "Temp.h"

#define TEMP_FRAME_LEN      9
#define TEMP_STABLE_COUNT   3       /* 连续几帧一致才算有效 */
#define TEMP_VALID_MS       3000    /* 超过这么久没更新就判为无效 */
#define TEMP_ALIVE_MS       1000    /* 超过这么久没收到帧就认为传感器没在发 */

static uint8_t  s_frame[TEMP_FRAME_LEN];
static uint8_t  s_index;

static int16_t  s_stableValue;
static uint8_t  s_stableCount;
static uint8_t  s_valid;

static uint32_t s_lastFrameMs;
static uint32_t s_lastValidMs;
static uint8_t  s_alive;

/**
  * @brief  解析一个字节，凑满一帧就验证格式
  * @retval 1=解析出一个合法温度值（单位 0.1℃）
  * @note   任何位置收到 'A' 都重新同步，避免丢字节后永久错位
  */
static uint8_t Temp_ParseByte(uint8_t byte, int16_t *valueX10)
{
	uint8_t i;
	int16_t integerPart;

	if (byte == 'A')
	{
		s_index = 0;			/* 帧头永远是 'A'，见到就重来 */
	}
	else if (s_index == 0)
	{
		return 0;				/* 还没同步上，丢掉 */
	}

	s_frame[s_index++] = byte;

	if (s_index < TEMP_FRAME_LEN)
	{
		return 0;
	}

	s_index = 0;				/* 一帧收完，无论对错都重新开始 */

	if (s_frame[1] != '+' && s_frame[1] != '-') return 0;
	if (s_frame[4] != '.')                      return 0;
	if (s_frame[6] != 'B')                      return 0;
	if (s_frame[7] != '\r')                     return 0;
	if (s_frame[8] != '\n')                     return 0;

	for (i = 2; i <= 3; i++)
	{
		if (s_frame[i] < '0' || s_frame[i] > '9') return 0;
	}
	if (s_frame[5] < '0' || s_frame[5] > '9') return 0;

	integerPart = (int16_t)((s_frame[2] - '0') * 10 + (s_frame[3] - '0'));
	*valueX10   = (int16_t)(integerPart * 10 + (s_frame[5] - '0'));

	if (s_frame[1] == '-')
	{
		*valueX10 = (int16_t)(-*valueX10);
	}

	return 1;
}

void Temp_Init(void)
{
	s_index       = 0;
	s_stableValue = 0;
	s_stableCount = 0;
	s_valid       = 0;
	s_alive       = 0;
	s_lastFrameMs = 0;
	s_lastValidMs = 0;

	UART1_Init();
}

uint8_t Temp_Poll(uint32_t nowMs)
{
	uint8_t byte;
	uint8_t gotResult = 0;
	int16_t value;

	while (UART1_Pop(&byte))
	{
		if (!Temp_ParseByte(byte, &value))
		{
			continue;
		}

		s_lastFrameMs = nowMs;
		s_alive       = 1;

		if (s_stableCount == 0 || value == s_stableValue)
		{
			s_stableValue = value;
			if (s_stableCount < TEMP_STABLE_COUNT) s_stableCount++;
		}
		else
		{
			/* 数值跳变，重新计数 */
			s_stableValue = value;
			s_stableCount = 1;
		}

		if (s_stableCount >= TEMP_STABLE_COUNT)
		{
			s_stableCount = 0;
			s_valid       = 1;
			s_lastValidMs = nowMs;
			gotResult     = 1;
		}
	}

	/* 超时判据：借 nowMs 做，不需要额外的定时器 */
	if (s_valid && (uint32_t)(nowMs - s_lastValidMs) > TEMP_VALID_MS)
	{
		s_valid = 0;
	}

	if (s_alive && (uint32_t)(nowMs - s_lastFrameMs) > TEMP_ALIVE_MS)
	{
		s_alive = 0;
	}

	return gotResult;
}

uint8_t Temp_Get(int16_t *valueX10)
{
	if (valueX10 != 0)
	{
		*valueX10 = s_stableValue;
	}
	return s_valid;
}

uint8_t Temp_SensorAlive(void)
{
	return s_alive;
}
