#include "BP.h"
#include "UART2.h"
#include "OLED.h"
#include "Delay.h"

/* ------------------------- 协议常量 ------------------------- */
#define BP_CMD_CHECK	0xAA	/* 检测状态 */
#define BP_CMD_START	0xA0	/* 开始测量 */
#define BP_CMD_ABORT	0xA1	/* 中断测量 */

#define BP_STA_RUNNING	0xA0	/* 测量中 */
#define BP_STA_RESULT	0xAC	/* 测量结果 */
#define BP_STA_FAIL		0xAD	/* 测量失败 */

#define BP_FRAME_MAX	16		/* 帧最大长度（LEN 最大 12 -> 14 字节） */

/* ------------------------- 时序参数 ------------------------- */
#define BP_CHECK_POLL_MS	10		/* 检测轮询间隔 */
#define BP_CHECK_RETRY		100		/* 100 * 10ms  = 1s   */
#define BP_START_POLL_MS	500		/* 过程帧 0.5s 一个，轮询节拍与之一致 */
#define BP_START_RETRY		120		/* 120 * 500ms = 60s  */

/* ------------------------- 帧类型 ------------------------- */
typedef enum
{
	BP_T_NONE = 0,
	BP_T_ACK_CHECK,
	BP_T_ACK_ABORT,
	BP_T_PROGRESS,
	BP_T_RESULT,
	BP_T_FAIL
} BP_FrameType;

/* ------------------------- 解析状态机 ------------------------- */
#define BP_ST_FF	0
#define BP_ST_C0	1
#define BP_ST_LEN	2
#define BP_ST_BODY	3

static uint8_t s_state = BP_ST_FF;
static uint8_t s_len;			/* 当前帧的 LEN */
static uint8_t s_cnt;			/* 已收到的载荷字节数 */
static uint8_t s_body[12];		/* 载荷暂存 */

static uint8_t s_frame[BP_FRAME_MAX];	/* 完整帧 */
static uint8_t s_frameLen;
static uint8_t s_frameReady;

static void BP_ShowProgress(uint8_t heartBeat, uint8_t pressure);
static void BP_ShowResult(const BP_Result *res);
static void BP_ShowFail(uint8_t x);
static BP_Status BP_MapFail(uint8_t x);

/* ------------------------- 字节解析 ------------------------- */
/**
  * @brief  把一个字节喂进帧状态机
  * @note   帧格式：FF C0 LEN CKSUM P0 P1 ...，LEN = 总长 - 2
  */
static void BP_ParseByte(uint8_t byte)
{
	uint8_t i;

	switch (s_state)
	{
	case BP_ST_FF:
		if (byte == 0xFF)
		{
			s_state = BP_ST_C0;
		}
		break;

	case BP_ST_C0:
		if (byte == 0xC0)
		{
			s_state = BP_ST_LEN;
		}
		else if (byte != 0xFF)		/* 连续 FF 时停在等 C0，不丢帧头 */
		{
			s_state = BP_ST_FF;
		}
		break;

	case BP_ST_LEN:
		s_len = byte;
		s_cnt = 0;
		if (byte >= 3 && byte <= 12)
		{
			s_state = BP_ST_BODY;
		}
		else
		{
			s_state = BP_ST_FF;
		}
		break;

	default:					/* BP_ST_BODY */
		s_body[s_cnt] = byte;
		s_cnt++;
		if (s_cnt >= (uint8_t)(s_len - 1))		/* 收满 CKSUM + 载荷 */
		{
			if (!s_frameReady)					/* 上一帧没取走就丢掉，不覆盖 */
			{
				s_frame[0] = 0xFF;
				s_frame[1] = 0xC0;
				s_frame[2] = s_len;
				for (i = 0; i < (uint8_t)(s_len - 1); i++)
				{
					s_frame[3 + i] = s_body[i];
				}
				s_frameLen   = (uint8_t)(s_len + 2);
				s_frameReady = 1;
			}
			s_state = BP_ST_FF;
		}
		break;
	}
}

/**
  * @brief  把环形缓冲里现有的字节全部解析一遍
  */
static void BP_Poll(void)
{
	uint8_t byte;

	while (UART2_Pop(&byte))
	{
		BP_ParseByte(byte);
	}
}

/**
  * @brief  丢弃残留字节并复位状态机（发命令前必须调用）
  */
static void BP_ResetParser(void)
{
	uint8_t byte;

	while (UART2_Pop(&byte))
	{
		/* 丢弃 */
	}
	s_state      = BP_ST_FF;
	s_cnt        = 0;
	s_frameLen   = 0;
	s_frameReady = 0;
}

/**
  * @brief  取出一帧
  * @retval 帧长度，0 表示暂无完整帧
  */
static uint8_t BP_GetFrame(uint8_t *out)
{
	uint8_t i;
	uint8_t n = s_frameLen;

	if (!s_frameReady)
	{
		return 0;
	}
	for (i = 0; i < n; i++)
	{
		out[i] = s_frame[i];
	}
	s_frameReady = 0;
	return n;
}

/**
  * @brief  按 LEN + 载荷首字节判定帧类型（不校验 CKSUM）
  */
static BP_FrameType BP_Classify(const uint8_t *f, uint8_t n)
{
	if (n < 5 || f[0] != 0xFF || f[1] != 0xC0)
	{
		return BP_T_NONE;
	}
	if ((uint8_t)(f[2] + 2) != n)			/* 长度自洽性检查 */
	{
		return BP_T_NONE;
	}

	switch (f[2])
	{
	case 0x03:								/* 命令回显：FF C0 03 CK AA / A1 */
		if (f[4] == BP_CMD_CHECK)
		{
			return BP_T_ACK_CHECK;
		}
		if (f[4] == BP_CMD_ABORT)
		{
			return BP_T_ACK_ABORT;
		}
		return BP_T_NONE;

	case 0x05:								/* 过程帧：FF C0 05 CK A0 QYH QYL */
		return (f[4] == BP_STA_RUNNING) ? BP_T_PROGRESS : BP_T_NONE;

	case 0x08:								/* 结果帧：FF C0 08 CK AC SSYH SSYL SZYH SZYL XL */
		return (f[4] == BP_STA_RESULT) ? BP_T_RESULT : BP_T_NONE;

	case 0x04:								/* 失败帧：FF C0 04 CK AD X */
		return (f[4] == BP_STA_FAIL) ? BP_T_FAIL : BP_T_NONE;

	default:
		return BP_T_NONE;
	}
}

/**
  * @brief  发送单字节载荷的命令帧：FF C0 03 CKSUM payload
  */
static void BP_SendCmd(uint8_t payload)
{
	uint8_t f[5];

	f[0] = 0xFF;
	f[1] = 0xC0;
	f[2] = 0x03;
	f[3] = (uint8_t)(0x03 + payload);	/* CKSUM = LEN + 载荷，取低字节 */
	f[4] = payload;

	UART2_SendBuf(f, 5);
}

/* ------------------------- 显示 ------------------------- */
/**
  * @brief  测量中界面：Running / Catched(Catching) / 气压
  */
static void BP_ShowProgress(uint8_t heartBeat, uint8_t pressure)
{
	OLED_ShowString(1, 1, "Running         ");
	OLED_ShowString(2, 1, heartBeat ? "Catched         " : "Catching        ");
	OLED_ShowString(3, 1, "Pressure:   mmHg");
	OLED_ShowNum(3, 10, pressure, 3);
}

/**
  * @brief  测量完成界面：sys / dia / 心率 + 心律不齐标志
  */
static void BP_ShowResult(const BP_Result *res)
{
	OLED_ShowString(1, 1, "Finished        ");

	OLED_ShowString(2, 1, "sys:   mmHg     ");
	OLED_ShowNum(2, 5, res->Systolic, 3);

	OLED_ShowString(3, 1, "dia:   mmHg     ");
	OLED_ShowNum(3, 5, res->Diastolic, 3);

	OLED_ShowString(4, 1, "HR:   IR:       ");
	OLED_ShowNum(4, 4, res->HeartRate, 3);
	OLED_ShowNum(4, 10, res->Irregular, 1);
}

/**
  * @brief  测量失败界面：X=0/2/4 建议重试，X=1/3 建议联系厂家
  */
static void BP_ShowFail(uint8_t x)
{
	OLED_ShowString(1, 1, "Measure Failed  ");

	switch (x)
	{
	case 1:
	case 3:
		OLED_ShowString(2, 1, "Contact Factory ");
		break;

	default:								/* 0 找不到脉搏 / 2 结果异常 / 4 干预过多 */
		OLED_ShowString(2, 1, "Retry           ");
		break;
	}
}

/**
  * @brief  失败码 X 映射到返回状态
  */
static BP_Status BP_MapFail(uint8_t x)
{
	switch (x)
	{
	case 0:  return BP_ERR_NO_PULSE;
	case 1:  return BP_ERR_LOW_PRESSURE;
	case 2:  return BP_ERR_ABNORMAL;
	case 3:  return BP_ERR_OVERP;
	case 4:  return BP_ERR_INTERFERE;
	default: return BP_ERR_COMM;
	}
}

/* ------------------------- 对外接口 ------------------------- */
/**
  * @brief  初始化串口与屏幕，并清掉血压计上电时喷出的噪声
  */
void BP_Init(void)
{
	UART2_Init();
	OLED_Init();
	OLED_Clear();

	Delay_ms(100);		/* 等上电那串 "PDFLAG ON..." 发完 */
	BP_ResetParser();
}

/**
  * @brief  检测血压计是否在线
  * @retval 1=在线，0=1s 内无回显
  */
uint8_t BP_Check(void)
{
	uint8_t f[BP_FRAME_MAX];
	uint8_t n;
	uint8_t i;

	BP_ResetParser();
	BP_SendCmd(BP_CMD_CHECK);

	for (i = 0; i < BP_CHECK_RETRY; i++)
	{
		Delay_ms(BP_CHECK_POLL_MS);
		BP_Poll();

		n = BP_GetFrame(f);
		if (n != 0 && BP_Classify(f, n) == BP_T_ACK_CHECK)
		{
			return 1;			/* 收到检测回显 */
		}
	}

	return 0;
}

/**
  * @brief  开始一次测量（阻塞，最长 60s），过程中刷新屏幕
  * @param  res 结果缓存，可为 0
  * @retval BP_OK / BP_ERR_OFFLINE / BP_ERR_COMM / BP_ERR_xxx
  */
BP_Status BP_Start(BP_Result *res)
{
	uint8_t f[BP_FRAME_MAX];
	uint8_t n;
	uint8_t i;

	if (!BP_Check())
	{
		OLED_ShowString(1, 1, "BP Offline      ");
		return BP_ERR_OFFLINE;
	}

	BP_ResetParser();
	BP_SendCmd(BP_CMD_START);

	for (i = 0; i < BP_START_RETRY; i++)
	{
		Delay_ms(BP_START_POLL_MS);
		BP_Poll();

		while ((n = BP_GetFrame(f)) != 0)
		{
			switch (BP_Classify(f, n))
			{
			case BP_T_PROGRESS:
				BP_ShowProgress(f[5], f[6]);
				break;

			case BP_T_RESULT:						/* FF C0 08 CK AC SSYH SSYL SZYH SZYL XL */
				if (res != 0)
				{
					res->Systolic  = f[6];			/* SSYL */
					res->Diastolic = f[8];			/* SZYL */
					res->HeartRate = f[9];			/* XL   */
					res->Irregular = (f[5] != 0) ? 1 : 0;	/* SSYH */
					BP_ShowResult(res);
				}
				return BP_OK;

			case BP_T_FAIL:							/* FF C0 04 CK AD X */
				BP_ShowFail(f[5]);
				return BP_MapFail(f[5]);

			default:
				break;
			}
		}
	}

	OLED_ShowString(1, 1, "BP Timeout      ");
	return BP_ERR_COMM;
}

/**
  * @brief  中断当前测量
  */
void BP_Abort(void)
{
	BP_ResetParser();
	BP_SendCmd(BP_CMD_ABORT);
	Delay_ms(50);
	BP_Poll();
}
