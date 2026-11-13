#ifndef __SPO2_H
#define __SPO2_H

#include "stm32f10x.h"

/* 血氧 / 心率（MAX30102 + 红光/红外 PPG）
 *
 * 用法：
 *   SpO2_Init();                     // 1=成功，0=失败（原因看 Max30102_ErrorStep）
 *   while (1) {
 *       SpO2_Task();                 // 每次主循环调用一次，内部排空 FIFO
 *       SpO2_Get(&spo2, &hr, &valid);
 *   }
 *
 * 算法：200 个样本（100Hz 下约 2s）为一窗
 *       R = (Red_AC/Red_DC) / (IR_AC/IR_DC)
 *       SpO2 = 110 - 25R   （线性近似，非临床精度）
 *       HR   = 窗内脉搏峰数 * 30
 */

uint8_t SpO2_Init(void);            /* 1=初始化成功 */
uint8_t SpO2_Task(void);            /* 返回 1 表示本窗算出了新结果 */
void    SpO2_Get(uint8_t *spo2, uint8_t *hr, uint8_t *valid);
uint8_t SpO2_FingerPresent(void);   /* 1=检测到手指（IR 直流足够大） */
uint8_t SpO2_FifoError(void);       /* 1=上一次 Task 遇到 FIFO/I2C 错误 */

#endif
