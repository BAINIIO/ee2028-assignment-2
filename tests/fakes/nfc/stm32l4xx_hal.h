#ifndef TESTS_NFC_FAKE_STM32L4XX_HAL_H
#define TESTS_NFC_FAKE_STM32L4XX_HAL_H
#include <stdint.h>
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed; } GPIO_InitTypeDef;
typedef struct { uint32_t unused; } I2C_HandleTypeDef;
#define GPIOE ((void *)(uintptr_t)0x48001000u)
#define GPIO_PIN_4 0x0010u
#define GPIO_MODE_IT_FALLING 1u
#define GPIO_PULLUP 1u
#define GPIO_SPEED_FREQ_LOW 0u
#define EXTI4_IRQn 10
#define I2C_FLAG_BUSY 1u
#define I2C_MEMADD_SIZE_16BIT 2u
#define RESET 0u
#define __HAL_RCC_GPIOE_CLK_ENABLE() ((void)0)
extern uint32_t fake_nfc_bus_busy;
#define __HAL_I2C_GET_FLAG(handle, flag) \
    ((void)(handle), (void)(flag), fake_nfc_bus_busy)
uint32_t HAL_GetTick(void);
void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *gpio);
void HAL_GPIO_EXTI_IRQHandler(uint16_t pin);
void HAL_NVIC_SetPriority(int irq, unsigned priority, unsigned sub);
void HAL_NVIC_EnableIRQ(int irq);
void HAL_NVIC_DisableIRQ(int irq);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *handle,
    uint16_t device, uint16_t address, uint16_t address_size,
    uint8_t *data, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *handle,
    uint16_t device, uint16_t address, uint16_t address_size,
    uint8_t *data, uint16_t size, uint32_t timeout);
#endif
