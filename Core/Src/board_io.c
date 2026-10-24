#include "board_io.h"
#include "app_config.h"
#ifndef BOARD_UART_HOST_TEST
#include "stm32l4xx_hal.h"
#include "stm32l4s5i_iot01.h"
#include "stm32l4s5i_iot01_tsensor.h"
#include "stm32l4s5i_iot01_hsensor.h"
#include "stm32l4s5i_iot01_psensor.h"
#include "stm32l4s5i_iot01_accelero.h"
#include "stm32l4s5i_iot01_gyro.h"
#include "stm32l4s5i_iot01_magneto.h"
#include <math.h>
#endif
#include <stddef.h>

#define UART_QUEUE_SIZE 4096u
#define UART_CHUNK_SIZE 16u
#define UART_TIMEOUT_MS 2u
#define UART_NO_PROGRESS_LIMIT 3u

static UART_HandleTypeDef console_uart;
static uint8_t uart_queue[UART_QUEUE_SIZE];
static uint16_t uart_head;
static uint16_t uart_tail;
static uint8_t uart_no_progress;
/* Retained for read-only diagnostics; a broken UART cannot reliably report
 * its own failures over that same UART. */
static volatile uint32_t uart_transmit_failures;
static volatile uint32_t uart_dropped_bytes;
static bool uart_enabled;

/* Host tests compile the actual UART implementation below with a small HAL
 * contract, without needing to emulate unrelated clocks and sensor buses. */
#ifndef BOARD_UART_HOST_TEST
/* The BSP owns I2C2 on PB10/PB11. Its getters discard HAL status and use a
 * one-second timeout; checked, bounded HAL reads below use the same register
 * maps and physical sensitivities as the supplied ST component drivers. */
extern I2C_HandleTypeDef hI2cHandler;

#define SENSOR_TIMEOUT_MS 5u
#define SENSOR_I2C_TIMING_80MHZ 0x10909CECu
#define HTS_ADDRESS HTS221_I2C_ADDRESS
#define LPS_ADDRESS LPS22HB_I2C_ADDRESS
#define IMU_ADDRESS LSM6DSL_ACC_GYRO_I2C_ADDRESS_LOW
#define MAG_ADDRESS LIS3MDL_MAG_I2C_ADDRESS_HIGH

static uint32_t initialized_sensors;
static uint32_t sensor_health;
static uint32_t sensors_with_conversion;
static uint32_t acceleration_fresh_ms;
static uint32_t gyro_fresh_ms;

typedef struct {
    float humidity_zero;
    float humidity_scale;
    float temperature_zero;
    float temperature_scale;
    int16_t humidity_raw_zero;
    int16_t temperature_raw_zero;
} HtsCalibration;

static HtsCalibration hts_cal;

static bool Clock_Init(void)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clocks = {0};
    RCC_PeriphCLKInitTypeDef peripheral = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
        return false;

    /* Internal 4 MHz MSI -> /1 *40 /2 = 80 MHz. No ST-LINK clock required. */
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_MSI;
    oscillator.MSIState = RCC_MSI_ON;
    oscillator.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
    oscillator.MSIClockRange = RCC_MSIRANGE_6;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_MSI;
    oscillator.PLL.PLLM = 1;
    oscillator.PLL.PLLN = 40;
    oscillator.PLL.PLLP = RCC_PLLP_DIV7;
    oscillator.PLL.PLLQ = RCC_PLLQ_DIV2;
    oscillator.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
        return false;

    clocks.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clocks.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clocks.APB1CLKDivider = RCC_HCLK_DIV1;
    clocks.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_4) != HAL_OK)
        return false;

    peripheral.PeriphClockSelection = RCC_PERIPHCLK_USART1 | RCC_PERIPHCLK_I2C2;
    peripheral.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
    peripheral.I2c2ClockSelection = RCC_I2C2CLKSOURCE_PCLK1;
    return HAL_RCCEx_PeriphCLKConfig(&peripheral) == HAL_OK;
}
#endif

static bool Uart_Init(void)
{
    GPIO_InitTypeDef pins = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();
    pins.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    pins.Mode = GPIO_MODE_AF_PP;
    pins.Pull = GPIO_PULLUP;
    pins.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    pins.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOB, &pins);

    console_uart.Instance = USART1;
    console_uart.Init.BaudRate = APP_UART_BAUD;
    console_uart.Init.WordLength = UART_WORDLENGTH_8B;
    console_uart.Init.StopBits = UART_STOPBITS_1;
    console_uart.Init.Parity = UART_PARITY_NONE;
    console_uart.Init.Mode = UART_MODE_TX_RX;
    console_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    console_uart.Init.OverSampling = UART_OVERSAMPLING_16;
    console_uart.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
#if defined(USART_PRESC_PRESCALER)
    console_uart.Init.ClockPrescaler = UART_PRESCALER_DIV1;
#endif
    console_uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&console_uart) != HAL_OK)
        return false;
    /* Lab 5 polling UART uses no TX interrupt. SysTick and button interrupts
     * stay enabled while the foreground transmits one small chunk. */
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    HAL_NVIC_ClearPendingIRQ(USART1_IRQn);
    uart_head = uart_tail = 0;
    uart_no_progress = 0;
    uart_transmit_failures = uart_dropped_bytes = 0;
    uart_enabled = true;
    return true;
}

#ifndef BOARD_UART_HOST_TEST
static bool ReadRegisters(uint16_t address, uint8_t reg, uint8_t *data,
                          uint16_t length)
{
    /* HAL's initial BUSY wait uses its own 25 ms limit, irrespective of our
     * transaction timeout. A held bus must not stall every scheduler pass. */
    if (__HAL_I2C_GET_FLAG(&hI2cHandler, I2C_FLAG_BUSY) != RESET)
        return false;
    return HAL_I2C_Mem_Read(&hI2cHandler, address, reg, I2C_MEMADD_SIZE_8BIT,
                            data, length, SENSOR_TIMEOUT_MS) == HAL_OK;
}

static bool WriteRegister(uint16_t address, uint8_t reg, uint8_t value)
{
    if (__HAL_I2C_GET_FLAG(&hI2cHandler, I2C_FLAG_BUSY) != RESET)
        return false;
    return HAL_I2C_Mem_Write(&hI2cHandler, address, reg, I2C_MEMADD_SIZE_8BIT,
                             &value, 1, SENSOR_TIMEOUT_MS) == HAL_OK;
}

static bool RegisterMatches(uint16_t address, uint8_t reg, uint8_t mask,
                            uint8_t expected)
{
    uint8_t value;
    return ReadRegisters(address, reg, &value, 1) &&
           (value & mask) == expected;
}

static int16_t Signed16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | (uint16_t)data[1] << 8);
}

static bool Hts_ReadCalibration(void)
{
    uint8_t raw[16];
    if (!ReadRegisters(HTS_ADDRESS, HTS221_H0_RH_X2 | 0x80u, raw, sizeof raw))
        return false;
    const int16_t h0 = Signed16(raw + 6);
    const int16_t h1 = Signed16(raw + 10);
    const int16_t t0 = Signed16(raw + 12);
    const int16_t t1 = Signed16(raw + 14);
    const float h0_pct = raw[0] / 2.0f;
    const float h1_pct = raw[1] / 2.0f;
    const float t0_c = ((uint16_t)raw[2] |
                         ((uint16_t)(raw[5] & 3u) << 8)) / 8.0f;
    const float t1_c = ((uint16_t)raw[3] |
                         ((uint16_t)(raw[5] & 12u) << 6)) / 8.0f;
    if (h1 == h0 || t1 == t0 || h1_pct <= h0_pct || t1_c <= t0_c)
        return false;
    hts_cal.humidity_zero = h0_pct;
    hts_cal.humidity_scale = (h1_pct - h0_pct) / (float)(h1 - h0);
    hts_cal.temperature_zero = t0_c;
    hts_cal.temperature_scale = (t1_c - t0_c) / (float)(t1 - t0);
    hts_cal.humidity_raw_zero = h0;
    hts_cal.temperature_raw_zero = t0;
    return true;
}

static bool SensorBus_ConfigureTiming(void)
{
    /* The bundled BSP reinstates its default timing during each ReadID/init.
     * Set an explicit ~100 kHz timing for our 80 MHz PCLK1 after those calls. */
    __HAL_I2C_DISABLE(&hI2cHandler);
    hI2cHandler.Init.Timing = SENSOR_I2C_TIMING_80MHZ;
    if (HAL_I2C_Init(&hI2cHandler) != HAL_OK)
        return false;
    HAL_NVIC_DisableIRQ(I2C2_EV_IRQn);
    HAL_NVIC_DisableIRQ(I2C2_ER_IRQn);
    return HAL_I2CEx_ConfigAnalogFilter(&hI2cHandler, I2C_ANALOGFILTER_ENABLE)
               == HAL_OK;
}

static void Sensors_Init(void)
{
    bool temperature_ok = false, humidity_ok = false;
    bool pressure_ok = false, acceleration_ok = false;
    bool gyro_ok = false, magnet_ok = false;
    initialized_sensors = sensor_health = sensors_with_conversion = 0;
    acceleration_fresh_ms = gyro_fresh_ms = 0;
    SENSOR_IO_Init();
    if (!SensorBus_ConfigureTiming())
        return;

    /* BSP_TSENSOR_Init always returns OK even if the device is absent, so
     * verify the actual WHO_AM_I value before calling each BSP initializer. */
    if (RegisterMatches(HTS_ADDRESS, HTS221_WHO_AM_I_REG, 0xffu,
                         HTS221_WHO_AM_I_VAL)) {
        temperature_ok = BSP_TSENSOR_Init() == TSENSOR_OK;
        humidity_ok = BSP_HSENSOR_Init() == HSENSOR_OK;
    }
    if (RegisterMatches(LPS_ADDRESS, LPS22HB_WHO_AM_I_REG, 0xffu,
                         LPS22HB_WHO_AM_I_VAL))
        pressure_ok = BSP_PSENSOR_Init() == PSENSOR_OK;
    if (RegisterMatches(IMU_ADDRESS, LSM6DSL_ACC_GYRO_WHO_AM_I_REG, 0xffu,
                         LSM6DSL_ACC_GYRO_WHO_AM_I)) {
        acceleration_ok = BSP_ACCELERO_Init() == ACCELERO_OK;
        gyro_ok = BSP_GYRO_Init() == GYRO_OK;
    }
    if (RegisterMatches(MAG_ADDRESS, LIS3MDL_MAG_WHO_AM_I_REG, 0xffu,
                         I_AM_LIS3MDL))
        magnet_ok = BSP_MAGNETO_Init() == MAGNETO_OK;
    if (!SensorBus_ConfigureTiming())
        return;

    /* Run HTS221 conversions at 7 Hz and acquire at 1 Hz, ensuring there is a
     * fresh conversion even when scheduler jitter straddles a 1 Hz boundary.
     * PD=1, BDU=1, ODR=10. Preserve fractional factory calibration endpoints. */
    if ((temperature_ok || humidity_ok) &&
        WriteRegister(HTS_ADDRESS, HTS221_CTRL_REG1, 0x86u) &&
        RegisterMatches(HTS_ADDRESS, HTS221_CTRL_REG1, 0x87u, 0x86u) &&
        Hts_ReadCalibration()) {
        if (temperature_ok) initialized_sensors |= SENSOR_TEMPERATURE;
        if (humidity_ok) initialized_sensors |= SENSOR_HUMIDITY;
    }
    if (pressure_ok &&
        WriteRegister(LPS_ADDRESS, LPS22HB_CTRL_REG2, 0x10u) &&
        RegisterMatches(LPS_ADDRESS, LPS22HB_CTRL_REG1, 0x72u, 0x32u) &&
        RegisterMatches(LPS_ADDRESS, LPS22HB_CTRL_REG2, 0x10u, 0x10u))
        initialized_sensors |= SENSOR_PRESSURE;
    if (RegisterMatches(IMU_ADDRESS, LSM6DSL_ACC_GYRO_CTRL3_C, 0x44u, 0x44u)) {
        if (acceleration_ok &&
            RegisterMatches(IMU_ADDRESS, LSM6DSL_ACC_GYRO_CTRL1_XL, 0xfcu, 0x30u))
            initialized_sensors |= SENSOR_ACCELEROMETER;
        if (gyro_ok &&
            RegisterMatches(IMU_ADDRESS, LSM6DSL_ACC_GYRO_CTRL2_G, 0xfeu, 0x3cu))
            initialized_sensors |= SENSOR_GYROSCOPE;
    }
    if (magnet_ok &&
        RegisterMatches(MAG_ADDRESS, LIS3MDL_MAG_CTRL_REG1, 0xfcu, 0x58u) &&
        RegisterMatches(MAG_ADDRESS, LIS3MDL_MAG_CTRL_REG2, 0x60u, 0x00u) &&
        RegisterMatches(MAG_ADDRESS, LIS3MDL_MAG_CTRL_REG3, 0x03u, 0x00u) &&
        RegisterMatches(MAG_ADDRESS, LIS3MDL_MAG_CTRL_REG5, 0x40u, 0x40u))
        initialized_sensors |= SENSOR_MAGNETOMETER;
    sensor_health = initialized_sensors;
}

bool Board_Init(void)
{
    if (!Clock_Init()) return false;
    BSP_LED_Init(LED2);
    BSP_LED_On(LED2);
    BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);
    if (!Uart_Init()) return false;
    Sensors_Init();
    return true;
}

uint32_t Board_SensorHealth(void)
{
    return sensor_health;
}

void Board_LedWrite(bool on)
{
    if (on) BSP_LED_On(LED2);
    else BSP_LED_Off(LED2);
}

static void InvalidateVector(float vector[3])
{
    vector[0] = vector[1] = vector[2] = NAN;
}

void Board_ReadMotion(SensorSample *sample)
{
    const uint32_t motion = SENSOR_ACCELEROMETER | SENSOR_GYROSCOPE;
    uint8_t raw[12], control[3], status;
    if (sample == NULL) return;
    sample->valid &= ~motion;
    InvalidateVector(sample->acceleration_ms2);
    InvalidateVector(sample->angular_rate_dps);
    if ((initialized_sensors & motion) &&
        ReadRegisters(IMU_ADDRESS, LSM6DSL_ACC_GYRO_CTRL1_XL, control, 3) &&
        (control[2] & 0x44u) == 0x44u &&
        ReadRegisters(IMU_ADDRESS, 0x1eu, &status, 1) &&
        ReadRegisters(IMU_ADDRESS, LSM6DSL_ACC_GYRO_OUTX_L_G, raw, sizeof raw)) {
        const uint32_t now = HAL_GetTick();
        /* A 1 Hz all-sensor read can immediately follow a 50 ms motion read.
         * Data-ready is then cleared but output registers still contain the
         * real most recent conversion. Accept that repeat for one motion
         * interval only; a frozen data-ready signal must become invalid. */
        if ((initialized_sensors & SENSOR_ACCELEROMETER) &&
            (control[0] & 0xfcu) == 0x30u &&
            ((status & 1u) ||
             ((sensors_with_conversion & SENSOR_ACCELEROMETER) &&
              now - acceleration_fresh_ms <= APP_MOTION_PERIOD_MS))) {
            /* +/-2g: 0.061 mg/LSB, 1 mg = 0.00980665 m/s^2. */
            for (unsigned axis = 0; axis < 3; ++axis)
                sample->acceleration_ms2[axis] =
                    Signed16(raw + 6 + axis * 2) *
                    (LSM6DSL_ACC_SENSITIVITY_2G * 0.00980665f);
            sample->valid |= SENSOR_ACCELEROMETER;
            sensors_with_conversion |= SENSOR_ACCELEROMETER;
            if (status & 1u) acceleration_fresh_ms = now;
        }
        if ((initialized_sensors & SENSOR_GYROSCOPE) &&
            (control[1] & 0xfeu) == 0x3cu &&
            ((status & 2u) ||
             ((sensors_with_conversion & SENSOR_GYROSCOPE) &&
              now - gyro_fresh_ms <= APP_MOTION_PERIOD_MS))) {
            /* +/-2000 dps: BSP sensitivity is 70 millidegrees/s per LSB. */
            for (unsigned axis = 0; axis < 3; ++axis)
                sample->angular_rate_dps[axis] =
                    Signed16(raw + axis * 2) *
                    (LSM6DSL_GYRO_SENSITIVITY_2000DPS / 1000.0f);
            sample->valid |= SENSOR_GYROSCOPE;
            sensors_with_conversion |= SENSOR_GYROSCOPE;
            if (status & 2u) gyro_fresh_ms = now;
        }
    }
    sensor_health = (sensor_health & ~motion) | (sample->valid & motion);
}

void Board_ReadSensors(SensorSample *sample)
{
    uint8_t raw[6], status;
    if (sample == NULL) return;
    sample->valid = 0;
    sample->temperature_c = sample->pressure_hpa = sample->humidity_pct = NAN;
    InvalidateVector(sample->magnetic_ut);
    if ((initialized_sensors & (SENSOR_TEMPERATURE | SENSOR_HUMIDITY)) &&
        RegisterMatches(HTS_ADDRESS, HTS221_CTRL_REG1, 0x87u, 0x86u) &&
        ReadRegisters(HTS_ADDRESS, 0x27u, &status, 1) &&
        ReadRegisters(HTS_ADDRESS, HTS221_HR_OUT_L_REG | 0x80u, raw, 4)) {
        if ((initialized_sensors & SENSOR_HUMIDITY) && (status & 1u)) {
            const float humidity = hts_cal.humidity_zero +
                (Signed16(raw) - hts_cal.humidity_raw_zero) *
                hts_cal.humidity_scale;
            if (isfinite(humidity) && humidity >= 0.0f && humidity <= 100.0f) {
                sample->humidity_pct = humidity;
                sample->valid |= SENSOR_HUMIDITY;
            }
        }
        if ((initialized_sensors & SENSOR_TEMPERATURE) && (status & 2u)) {
            const float temperature = hts_cal.temperature_zero +
                (Signed16(raw + 2) - hts_cal.temperature_raw_zero) *
                hts_cal.temperature_scale;
            if (isfinite(temperature) && temperature >= -40.0f &&
                temperature <= 120.0f) {
                sample->temperature_c = temperature;
                sample->valid |= SENSOR_TEMPERATURE;
            }
        }
    }
    if ((initialized_sensors & SENSOR_PRESSURE) &&
        RegisterMatches(LPS_ADDRESS, LPS22HB_CTRL_REG1, 0x72u, 0x32u) &&
        ReadRegisters(LPS_ADDRESS, 0x27u, &status, 1) && (status & 1u) &&
        ReadRegisters(LPS_ADDRESS, LPS22HB_PRESS_OUT_XL_REG, raw, 3)) {
        int32_t value = (int32_t)((uint32_t)raw[0] |
                           (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16);
        if (value & 0x00800000) value |= (int32_t)0xff000000u;
        /* LPS22HB data sheet: 4096 LSB per hPa. */
        const float pressure = value / 4096.0f;
        if (isfinite(pressure) && pressure >= 260.0f && pressure <= 1260.0f) {
            sample->pressure_hpa = pressure;
            sample->valid |= SENSOR_PRESSURE;
        }
    }
    if ((initialized_sensors & SENSOR_MAGNETOMETER) &&
        RegisterMatches(MAG_ADDRESS, LIS3MDL_MAG_CTRL_REG2, 0x60u, 0x00u) &&
        RegisterMatches(MAG_ADDRESS, LIS3MDL_MAG_CTRL_REG3, 0x03u, 0x00u) &&
        ReadRegisters(MAG_ADDRESS, LIS3MDL_MAG_STATUS_REG, &status, 1) &&
        (status & 8u) &&
        ReadRegisters(MAG_ADDRESS, LIS3MDL_MAG_OUTX_L | 0x80u, raw, 6)) {
        /* +/-4 gauss: 0.14 milligauss/LSB (BSP), 1 mgauss = 0.1 uT. */
        for (unsigned axis = 0; axis < 3; ++axis)
            sample->magnetic_ut[axis] = Signed16(raw + axis * 2) *
                (LIS3MDL_MAG_SENSITIVITY_FOR_FS_4GA * 0.1f);
        sample->valid |= SENSOR_MAGNETOMETER;
    }
    Board_ReadMotion(sample);
    sensor_health = sample->valid;
}
#endif

bool Board_UartWrite(const char *message)
{
    size_t length = 0;
    if (!uart_enabled || message == NULL) return false;
    while (length < UART_QUEUE_SIZE && message[length] != '\0') ++length;
    if (length >= UART_QUEUE_SIZE) return false;
    /* The public UART API is main-context only; no ISR modifies this queue. */
    const uint16_t free_bytes = (uint16_t)((uart_tail + UART_QUEUE_SIZE -
                                          uart_head - 1u) % UART_QUEUE_SIZE);
    if (length > free_bytes) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        uart_queue[uart_head] = (uint8_t)message[i];
        uart_head = (uint16_t)((uart_head + 1u) % UART_QUEUE_SIZE);
    }
    Board_UartPoll();
    return true;
}

void Board_UartPoll(void)
{
    if (!uart_enabled || uart_tail == uart_head || __get_PRIMASK() != 0u) return;
    uint16_t length = uart_head > uart_tail ? (uint16_t)(uart_head - uart_tail)
                         : (uint16_t)(UART_QUEUE_SIZE - uart_tail);
    if (length > UART_CHUNK_SIZE) length = UART_CHUNK_SIZE;

    /* At 115200 8N1, 16 bytes normally occupy 1.39 ms. HAL's 2 ms timeout
     * bounds a faulty poll to roughly 3 ms (its check is elapsed > timeout).
     * Interrupts remain enabled so SysTick can enforce that timeout and EXTI
     * can capture button timestamps throughout the bounded foreground wait. */
    console_uart.TxXferCount = length;
    HAL_StatusTypeDef status = HAL_UART_Transmit(&console_uart,
        uart_queue + uart_tail, length, UART_TIMEOUT_MS);
    uint16_t submitted = status == HAL_OK ? length :
        (status == HAL_TIMEOUT && console_uart.TxXferCount <= length
            ? (uint16_t)(length - console_uart.TxXferCount) : 0u);
    if (status != HAL_OK) ++uart_transmit_failures;
    if (submitted != 0u) {
        /* HAL decrements TxXferCount after each TDR write. Do not resend that
         * prefix, including when only the final transmission-complete wait
         * timed out; retry only bytes HAL has not handed to the peripheral. */
        uart_tail = (uint16_t)((uart_tail + submitted) % UART_QUEUE_SIZE);
        uart_no_progress = 0;
    } else if (++uart_no_progress >= UART_NO_PROGRESS_LIMIT) {
        /* A permanently stuck peripheral must not pin the queue forever.
         * After three no-progress calls, explicitly discard this small chunk. */
        uart_tail = (uint16_t)((uart_tail + length) % UART_QUEUE_SIZE);
        uart_dropped_bytes += length;
        uart_no_progress = 0;
    }
    if (status == HAL_BUSY || status == HAL_ERROR) {
        (void)HAL_UART_AbortTransmit(&console_uart);
    }
}

bool Board_UartIdle(void)
{
    return !uart_enabled || (uart_head == uart_tail &&
        __HAL_UART_GET_FLAG(&console_uart, UART_FLAG_TC) != RESET);
}

void Board_UartStop(void)
{
    if (!uart_enabled) return;
    uart_enabled = false;
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    HAL_NVIC_ClearPendingIRQ(USART1_IRQn);
    (void)HAL_UART_AbortTransmit(&console_uart);
    HAL_UART_DeInit(&console_uart);
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_6 | GPIO_PIN_7);
    __HAL_RCC_USART1_CLK_DISABLE();
    uart_head = uart_tail = 0;
    uart_no_progress = 0;
}
