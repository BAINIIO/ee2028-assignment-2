#ifndef TESTS_FAKE_STM32L4XX_HAL_H
#define TESTS_FAKE_STM32L4XX_HAL_H
/* Minimal external-I2C HAL contract used by test_external_oled.c. */
#include <stdint.h>

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
typedef struct { uint32_t PeriphClockSelection, I2c1ClockSelection; } RCC_PeriphCLKInitTypeDef;
typedef struct {
    uint32_t Timing, OwnAddress1, AddressingMode, DualAddressMode;
    uint32_t OwnAddress2, OwnAddress2Masks, GeneralCallMode, NoStretchMode;
} FakeI2cInit;
typedef struct { void *Instance; FakeI2cInit Init; uint32_t ErrorCode; } I2C_HandleTypeDef;

#define GPIOB ((void *)(uintptr_t)0x48000400u)
#define I2C1 ((void *)(uintptr_t)0x40005400u)
#define GPIO_PIN_8 0x0100u
#define GPIO_PIN_9 0x0200u
#define GPIO_MODE_AF_OD 1u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_HIGH 3u
#define GPIO_AF4_I2C1 4u
#define RCC_PERIPHCLK_I2C1 1u
#define RCC_I2C1CLKSOURCE_SYSCLK 1u
#define I2C_ADDRESSINGMODE_7BIT 1u
#define I2C_DUALADDRESS_DISABLE 0u
#define I2C_OA2_NOMASK 0u
#define I2C_GENERALCALL_DISABLE 0u
#define I2C_NOSTRETCH_DISABLE 0u
#define I2C_ANALOGFILTER_ENABLE 1u
#define I2C_FLAG_BUSY 1u
#define HAL_I2C_ERROR_AF 4u
#define RESET 0u
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_I2C1_CLK_ENABLE() ((void)0)
extern uint32_t fake_i2c_busy;
#define __HAL_I2C_GET_FLAG(handle,flag) ((void)(handle),(void)(flag),fake_i2c_busy)

uint32_t HAL_GetTick(void);
uint32_t HAL_RCC_GetSysClockFreq(void);
void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *gpio);
HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(const RCC_PeriphCLKInitTypeDef *clock);
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *handle);
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef *handle, uint32_t setting);
HAL_StatusTypeDef HAL_I2CEx_ConfigDigitalFilter(I2C_HandleTypeDef *handle, uint32_t setting);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *handle, uint16_t address,
                                     uint32_t trials, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *handle, uint16_t address,
                                       uint8_t *bytes, uint16_t length, uint32_t timeout);
uint32_t HAL_I2C_GetError(const I2C_HandleTypeDef *handle);
#endif
