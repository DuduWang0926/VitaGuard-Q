#include "stm32f10x.h"
#include "Board.h"

void Board_Init(void)
{
	/* AFIO 必须最先开，后面的重映射全靠它 */
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

	/* ---- 第 1 步：普通重映射（会把 SWJ_CFG 置成 111，所以必须放在前面） ---- */

	/* USART1: PA9/PA10 -> PB6/PB7（PA9/PA10 要留给按钮） */
	GPIO_PinRemapConfig(GPIO_Remap_USART1, ENABLE);

	/* 本项目屏幕暂时仍用软件 I2C（PB8/PB9），不启用 I2C1 重映射。
	 * 将来换成硬件 I2C1 时，在这里加一行：
	 *     GPIO_PinRemapConfig(GPIO_Remap_I2C1, ENABLE);   // PB8=SCL, PB9=SDA
	 * 但要注意：那会和软件 I2C 抢同一对引脚，必须同时改 OLED.c。 */

	/* ---- 第 2 步：最后处理 SWJ_CFG ---- */

	/* PA15 要当 SoftwareI2C 的 SCL，而它复位后是 JTDI。
	 * GPIO_Remap_SWJ_JTAGDisable => SWJ_CFG = 010：关 JTAG，保留 SWD。
	 * 所以 PA15 可用，PA13/PA14 仍能下载和调试。
	 *
	 * 注意：调用里那个 "ENABLE" 会被忽略(见 StdPeriph 源码)，
	 *       SWJ_CFG 的写入是在分支外无条件执行的。 */
	GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
}
