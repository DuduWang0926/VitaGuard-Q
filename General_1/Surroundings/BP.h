#ifndef __BP_H
#define __BP_H

#include "stm32f10x.h"

/* 血压计模块（USART2 / PA2-TX PA3-RX，115200 8N1）
 *
 * 协议：FF C0 LEN CKSUM 载荷...      LEN = 帧总长 - 2
 *   命令  AA=检测   A0=开始测量   A1=中断
 *   回显  FF C0 03 CK AA  /  FF C0 03 CK A1
 *   过程  FF C0 05 CK A0 QYH QYL     0.5s 一帧；QYH=1 有心跳，QYL=袖带气压
 *   结果  FF C0 08 CK AC SSYH SSYL SZYH SZYL XL
 *   失败  FF C0 04 CK AD X           X=0 无脉搏 1 气压过低 2 结果异常 3 超压 4 干预过多
 *
 * 【本版按项目要求「不校验 CKSUM」】帧只做长度自洽 + 命令字判定。
 *   风险：坏帧可能被当成有效结果。真遇到误报再把 CKSUM 加上（只需一个函数）。
 *
 * 【全部接口非阻塞】
 *   旧版 BP_Start() 用 Delay_ms(500) 循环 120 次，最长阻塞 60 秒，
 *   期间按键、体温串口、刷屏全部停摆。现在改成：
 *       BP_Begin() 只负责发命令，
 *       BP_Task()  由主循环每轮调用，把环形缓冲里的字节解析掉。
 *   因为它永不阻塞，MAX30102 的 FIFO（32 样本 = 320ms）也不会被饿死。
 *
 * 典型用法：
 *      BP_Init();
 *      BP_Begin(Tick_GetMs());                 // 用户按 SW3
 *      while (1) { BP_Task(Tick_GetMs()); ... } // 主循环
 *      if (BP_GetState() == BP_DONE) BP_GetResult(&r);
 */

typedef enum
{
	BP_IDLE = 0,		/* 空闲，等用户按开始 */
	BP_CHECKING,		/* 已发 AA，等检测回显（1s 超时） */
	BP_MEASURING,		/* 已发 A0，等过程帧 / 终帧（120s 超时） */
	BP_DONE,			/* 拿到结果帧 */
	BP_FAILED,			/* 拿到失败帧，原因见 BP_GetFailCode() */
	BP_OFFLINE,			/* 1s 内没有检测回显：设备不在或没接线 */
	BP_TIMEOUT,			/* 120s 没有终帧 */
	BP_ABORTED			/* 用户主动中止（已发 A1） */
} BP_State;

typedef struct
{
	uint8_t Systolic;		/* 收缩压 mmHg，取 SSYL */
	uint8_t Diastolic;		/* 舒张压 mmHg，取 SZYL */
	uint8_t HeartRate;		/* 心率 bpm，取 XL */
	uint8_t Irregular;		/* SSYH：1=心律不齐，0=正常 */
} BP_Result;

/* 初始化 USART2 并清掉上电噪声。不碰屏幕。 */
void		BP_Init(void);

/* 开始一次测量（先发检测命令）。已在测量中则返回 0。 */
uint8_t		BP_Begin(uint32_t nowMs);

/* 主循环每轮调用一次：解析串口、推进状态、处理超时。永不阻塞。 */
void		BP_Task(uint32_t nowMs);

/* 中止测量：立即发 A1，状态置 BP_ABORTED */
void		BP_Abort(void);

/* 清结果、回 BP_IDLE（进入页面时调用）。不影响串口配置。 */
void		BP_Reset(void);

BP_State	BP_GetState(void);

/* 1 = 正在检测或测量（调用方应锁住翻页/重复启动） */
uint8_t		BP_Busy(void);

/* 过程帧（0.5s 一帧）。返回 1 表示本状态有可用气压值 */
uint8_t		BP_GetProgress(uint16_t *cuffMmHg, uint8_t *pulsePresent);

/* 返回 1 表示有结果可读（state == BP_DONE） */
uint8_t		BP_GetResult(BP_Result *res);

/* 失败帧里的 X（仅 BP_FAILED 时有意义） */
uint8_t		BP_GetFailCode(void);

#endif
