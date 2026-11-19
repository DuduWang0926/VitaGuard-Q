#include "BP.h"
#include "UART2.h"

/* ------------------------- 协议常量 ------------------------- */
#define BP_CMD_CHECK	0xAA	/* 检测状态 */
#define BP_CMD_START	0xA0	/* 开始测量 */
#define BP_CMD_ABORT	0xA1	/* 中断测量 */

#define BP_STA_RUNNING	0xA0	/* 测量中 */
#define BP_STA_RESULT	0xAC	/* 测量结果 */
#define BP_STA_FAIL		0xAD	/* 测量失败 */

#define BP_FRAME_MAX	16		/* 帧最大长度（LEN 最大 12 -> 14 字节） */

/* ------------------------- 时序参数 ------------------------- */
#define BP_CHECK_TIMEOUT_MS		1000	/* 发 AA 后等回显的上限 */
#define BP_MEASURE_TIMEOUT_MS	120000	/* 发 A0 后等终帧的上限（袖带加压 + 放气，留足 120s） */

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
#define BP_PST_FF		0
#define BP_PST_C0		1
#define BP_PST_LEN		2
#define BP_PST_BODY		3

static uint8_t s_pst = BP_PST_FF;	/* 解析状态（别和模块状态 s_state 混） */
static uint8_t s_len;				/* 当前帧的 LEN */
static uint8_t s_cnt;				/* 已收到的载荷字节数 */
static uint8_t s_body[12];			/* 载荷暂存（含 CKSUM） */

static uint8_t s_frame[BP_FRAME_MAX];	/* 完整帧 */
static uint8_t s_frameLen;
static uint8_t s_frameReady;

/* ------------------------- 模块状态 ------------------------- */
static BP_State s_state = BP_IDLE;
static uint32_t s_cmdMs;			/* 发命令的时刻，用于超时 */
static uint16_t s_cuff;				/* 过程帧 QYL：袖带气压 */
static uint8_t  s_pulse;			/* 过程帧 QYH：1=检测到心跳 */
static uint8_t  s_hasProg;			/* 本次测量是否已收到过过程帧 */
static BP_Result s_result;
static uint8_t  s_failCode;

/* ------------------------- 内部函数 ------------------------- */

/**
  * @brief  把一个字节喂进帧状态机
  * @note   帧格式：FF C0 LEN CKSUM P0 P1 ...，LEN = 总长 - 2
  */
static void BP_ParseByte(uint8_t byte)
{
	uint8_t i;

	switch (s_pst)
	{
	case BP_PST_FF:
		if (byte == 0xFF)
		{
			s_pst = BP_PST_C0;
		}
		break;

	case BP_PST_C0:
		if (byte == 0xC0)
		{
			s_pst = BP_PST_LEN;
		}
		else if (byte != 0xFF)		/* 连续 FF 时停在等 C0，不丢帧头 */
		{
			s_pst = BP_PST_FF;
		}
		break;

	case BP_PST_LEN:
		s_len = byte;
		s_cnt = 0;
		if (byte >= 3 && byte <= 12)
		{
			s_pst = BP_PST_BODY;
		}
		else
		{
			s_pst = BP_PST_FF;
		}
		break;

	default:					/* BP_PST_BODY */
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
			s_pst = BP_PST_FF;
		}
		break;
	}
}

/**
  * @brief  丢弃残留字节并复位解析状态机（发命令前必须调用）
  */
static void BP_ResetParser(void)
{
	uint8_t byte;

	while (UART2_Pop(&byte))
	{
		/* 丢弃 */
	}
	s_pst        = BP_PST_FF;
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
  * @brief  按 LEN + 载荷首字节判定帧类型
  * @note   **故意不校验 CKSUM**（项目要求）。只做长度自洽 + 命令字判定。
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

/* ------------------------- 对外接口 ------------------------- */

void BP_Init(void)
{
	UART2_Init();
	BP_ResetParser();

	s_state    = BP_IDLE;
	s_cmdMs    = 0;
	s_cuff     = 0;
	s_pulse    = 0;
	s_hasProg  = 0;
	s_failCode = 0;
	s_result.Systolic  = 0;
	s_result.Diastolic = 0;
	s_result.HeartRate = 0;
	s_result.Irregular = 0;
}

uint8_t BP_Begin(uint32_t nowMs)
{
	if (BP_Busy())
	{
		return 0;						/* 已经在测了，别重入 */
	}

	BP_ResetParser();

	s_cuff     = 0;
	s_pulse    = 0;
	s_hasProg  = 0;
	s_failCode = 0;

	BP_SendCmd(BP_CMD_CHECK);			/* 先检测，回显到了才真正开始 */
	s_state = BP_CHECKING;
	s_cmdMs = nowMs;

	return 1;
}

void BP_Task(uint32_t nowMs)
{
	uint8_t byte;
	uint8_t f[BP_FRAME_MAX];
	uint8_t n;

	/* ① 把中断搬进环形缓冲的字节全部解析掉（微秒级，不阻塞） */
	while (UART2_Pop(&byte))
	{
		BP_ParseByte(byte);
	}

	/* ② 取空所有完整帧。必须是 while：一次调用里可能同时有过程帧和终帧 */
	while ((n = BP_GetFrame(f)) != 0)
	{
		switch (BP_Classify(f, n))
		{
		case BP_T_ACK_CHECK:
			if (s_state == BP_CHECKING)
			{
				/* 检测通过，立刻开始测量：FF C0 03 A3 A0 */
				BP_ResetParser();
				BP_SendCmd(BP_CMD_START);
				s_state   = BP_MEASURING;
				s_cmdMs   = nowMs;
				s_hasProg = 0;
			}
			break;

		case BP_T_ACK_ABORT:
			/* 中止回显，状态已由 BP_Abort() 置好，这里不用做事 */
			break;

		case BP_T_PROGRESS:
			if (s_state == BP_MEASURING)
			{
				s_pulse   = (f[5] != 0) ? 1u : 0u;
				s_cuff    = f[6];
				s_hasProg = 1;
			}
			break;

		case BP_T_RESULT:
			if (s_state == BP_MEASURING || s_state == BP_CHECKING)
			{
				s_result.Systolic  = f[6];						/* SSYL */
				s_result.Diastolic = f[8];						/* SZYL */
				s_result.HeartRate = f[9];						/* XL   */
				s_result.Irregular = (f[5] != 0) ? 1u : 0u;		/* SSYH */
				s_state = BP_DONE;
			}
			break;

		case BP_T_FAIL:
			if (s_state == BP_MEASURING || s_state == BP_CHECKING)
			{
				s_failCode = f[5];
				s_state    = BP_FAILED;
			}
			break;

		default:
			break;
		}
	}

	/* ③ 超时：用无符号减法，Tick 溢出（49.7 天）也不会误判 */
	if (s_state == BP_CHECKING &&
		(uint32_t)(nowMs - s_cmdMs) >= BP_CHECK_TIMEOUT_MS)
	{
		s_state = BP_OFFLINE;
	}
	if (s_state == BP_MEASURING &&
		(uint32_t)(nowMs - s_cmdMs) >= BP_MEASURE_TIMEOUT_MS)
	{
		s_state = BP_TIMEOUT;
	}
}

void BP_Abort(void)
{
	if (!BP_Busy())
	{
		return;
	}

	BP_ResetParser();
	BP_SendCmd(BP_CMD_ABORT);			/* 让血压计立刻放气 */
	s_state = BP_ABORTED;
}

void BP_Reset(void)
{
	BP_ResetParser();

	s_state    = BP_IDLE;
	s_cmdMs    = 0;
	s_cuff     = 0;
	s_pulse    = 0;
	s_hasProg  = 0;
	s_failCode = 0;
	s_result.Systolic  = 0;
	s_result.Diastolic = 0;
	s_result.HeartRate = 0;
	s_result.Irregular = 0;
}

BP_State BP_GetState(void)
{
	return s_state;
}

uint8_t BP_Busy(void)
{
	return (uint8_t)((s_state == BP_CHECKING) || (s_state == BP_MEASURING));
}

uint8_t BP_GetProgress(uint16_t *cuffMmHg, uint8_t *pulsePresent)
{
	if (s_state != BP_MEASURING || !s_hasProg)
	{
		return 0;
	}
	if (cuffMmHg != 0)
	{
		*cuffMmHg = s_cuff;
	}
	if (pulsePresent != 0)
	{
		*pulsePresent = s_pulse;
	}
	return 1;
}

uint8_t BP_GetResult(BP_Result *res)
{
	if (s_state != BP_DONE)
	{
		return 0;
	}
	if (res != 0)
	{
		*res = s_result;
	}
	return 1;
}

uint8_t BP_GetFailCode(void)
{
	return s_failCode;
}
