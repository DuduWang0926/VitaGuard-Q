#include "stm32f10x.h"
#include "Delay.h"
#include "OLED.h"
#include "SHT3X.h"

float Tep, Hum;
uint16_t a, b;
uint8_t result;

int main(void)
{
    OLED_Init();
    SHT3X_Init();

    while (1)
    {
        result = SHT3X_GTMeasure(&Tep, &Hum);

        if (result == 1)
        {
            a = (uint16_t)(Tep * 100);
            b = (uint16_t)(Hum * 100);

            OLED_ShowString(1, 1, "T*100:");
            OLED_ShowNum(1, 7, a, 5);

            OLED_ShowString(2, 1, "H*100:");
            OLED_ShowNum(2, 7, b, 5);
        }
        else
        {
            OLED_ShowNum(4, 1, result, 1);
        }

        Delay_ms(500);
    }
}