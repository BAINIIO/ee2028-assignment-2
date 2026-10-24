#ifndef TEST_FAKE_MAIN_H
#define TEST_FAKE_MAIN_H

#include <stdbool.h>
#include <stdint.h>

#define BUTTON_EXTI13_Pin ((uint16_t)0x2000u)
#define EXTI15_10_IRQn 40

void HAL_Init(void);
uint32_t HAL_GetTick(void);
void HAL_NVIC_DisableIRQ(int irq);
void HAL_SuspendTick(void);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t mask);
void __DMB(void);
void __WFI(void);
void Error_Handler(void);

#endif
