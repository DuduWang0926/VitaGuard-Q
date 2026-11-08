#include "stm32f10x.h"
#include "Delay.h"
#include "OLED.h"
#include "BP.h"

/* 血压计最小验证程序
 * 上电 -> 检测血压计 -> 开始一次测量（阻塞，最长 60s） -> 结果留在屏幕上
 * 需要连续测量时，把下面 while(1) 里的注释打开即可。 */

int main(void)
{
	BP_Result result;
	BP_Status status;

	BP_Init();								/* USART2(PA2/PA3) + OLED(PB8/PB9) */

	OLED_ShowString(1, 1, "BP Sensor Test  ");
	Delay_ms(800);

	status = BP_Start(&result);				/* 检测 1s + 测量最长 60s */

	while (1)
	{
		/* 连续测量：
		   Delay_s(5);
		   status = BP_Start(&result); */

		(void)status;
		(void)result;
	}
}
