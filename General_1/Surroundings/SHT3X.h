#ifndef __SHT3X_H
#define __SHT3X_H
      void SHT3X_Init(void);
	  uint8_t SHT3X_STMeasure(void);
	  uint8_t SHT3X_ReadData(uint8_t *Data);
	  uint8_t SHT3X_CRC(uint8_t *Data, uint8_t Length);
	   uint8_t SHT3X_GTMeasure(float *Tmp,float *hum);
#endif
