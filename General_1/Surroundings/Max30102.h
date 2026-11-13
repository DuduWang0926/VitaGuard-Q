#ifndef __MAX30102_H
#define __MAX30102_H

#include "stm32f10x.h"

extern volatile uint8_t Max30102_ErrorStep;

uint8_t Max30102_Reset(void);

uint8_t Max30102_ClearFIFO(void);

uint8_t Max30102_STMeasure_SPO2(void);

uint8_t Max30102_GetFIFOCount(void);

uint8_t Max30102_ReadFIFO(uint32_t *RED, uint32_t *IR);

#endif
