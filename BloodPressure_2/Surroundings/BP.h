#ifndef __BP_H
#define __BP_H

#include "stm32f10x.h"
#include <stdint.h>

/* 血压计模块（USART2）
 * 协议：FF C0 LEN CKSUM 载荷...   LEN = 帧总长 - 2
 * 命令：AA=检测  A0=开始测量  A1=中断
 * 状态：AA=检测回显  A1=中断回显  A0=测量中  AC=结果  AD=失败
 * 本模块不校验 CKSUM。 */

typedef enum
{
	BP_OK = 0,
	BP_ERR_COMM,			/* 超时 / 通信失败 */
	BP_ERR_OFFLINE,			/* 检测不到血压计 */
	BP_ERR_NO_PULSE,		/* X=0 找不到脉搏 */
	BP_ERR_LOW_PRESSURE,	/* X=1 气压过低 */
	BP_ERR_ABNORMAL,		/* X=2 结果异常 */
	BP_ERR_OVERP,			/* X=3 超压保护 */
	BP_ERR_INTERFERE		/* X=4 干预过多 */
} BP_Status;

typedef struct
{
	uint8_t Systolic;		/* 收缩压 mmHg，取 SSYL */
	uint8_t Diastolic;		/* 舒张压 mmHg，取 SZYL */
	uint8_t HeartRate;		/* 心率 bpm，取 XL */
	uint8_t Irregular;		/* SSYH：1=心律不齐，0=正常 */
} BP_Result;

void      BP_Init(void);					/* USART2 + OLED 初始化，并清掉上电噪声 */
uint8_t   BP_Check(void);					/* 发 AA，1s 内收到回显返回 1，否则 0 */
BP_Status BP_Start(BP_Result *res);		/* 发 A0，阻塞到结果/失败帧（最长 60s） */
void      BP_Abort(void);					/* 发 A1，中断测量 */

#endif
