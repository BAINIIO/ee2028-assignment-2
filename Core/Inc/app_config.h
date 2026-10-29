#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Override on the compiler command line, e.g. -DAPP_ENABLE_OLED=0. */
#ifndef APP_ENABLE_OLED
#define APP_ENABLE_OLED 1
#endif
/* A read-only ST-Link viewer can display frames even without an OLED. */
#ifndef APP_ENABLE_LAPTOP_DISPLAY
#define APP_ENABLE_LAPTOP_DISPLAY 1
#endif
#ifndef APP_ENABLE_COLOUR
#define APP_ENABLE_COLOUR 0
#endif
#ifndef APP_ENABLE_NFC
#define APP_ENABLE_NFC 1
#endif
#define APP_COMMAND_HTTP_PORT 8080u
#ifndef APP_ENABLE_IR
#define APP_ENABLE_IR 1
#endif
#ifndef APP_IR_ACTIVE_LOW
#define APP_IR_ACTIVE_LOW 1
#endif
/* Grove A0 connector signal is PC5, used as a digital comparator input. */
#ifndef APP_IR_GPIO_PORT
#define APP_IR_GPIO_PORT GPIOC
#endif
#ifndef APP_IR_GPIO_PIN
#define APP_IR_GPIO_PIN GPIO_PIN_5
#endif
#ifndef APP_IR_GPIO_CLOCK_ENABLE
#define APP_IR_GPIO_CLOCK_ENABLE() __HAL_RCC_GPIOC_CLK_ENABLE()
#endif
#ifndef APP_ENABLE_BUZZER
#define APP_ENABLE_BUZZER 1
#endif
#ifndef APP_BUZZER_ACTIVE
#define APP_BUZZER_ACTIVE 0
#endif
#ifndef APP_BUZZER_ACTIVE_LOW
#define APP_BUZZER_ACTIVE_LOW 0
#endif
/* User's buzzer is connected to Arduino D6 / PB1. Timer-driven GPIO also
 * supports pins without a hardware PWM alternate function. */
#ifndef APP_BUZZER_GPIO_PORT
#define APP_BUZZER_GPIO_PORT GPIOB
#endif
#ifndef APP_BUZZER_GPIO_PIN
#define APP_BUZZER_GPIO_PIN GPIO_PIN_1
#endif
#ifndef APP_BUZZER_GPIO_CLOCK_ENABLE
#define APP_BUZZER_GPIO_CLOCK_ENABLE() __HAL_RCC_GPIOB_CLK_ENABLE()
#endif

#define APP_UART_BAUD 115200u
#define APP_SENSOR_PERIOD_MS 1000u
#define APP_MOTION_PERIOD_MS 50u
#define APP_EXTERNAL_PERIOD_MS 150u
/* About 30 fps for the laptop. The OLED sends complete snapshots at the
 * slower rate supported by its 100 kHz bus, skipping superseded frames. */
#define APP_DISPLAY_PERIOD_MS 33u
#define APP_DEBOUNCE_MS 30u
#define APP_DOUBLE_PRESS_MS 500u
#define APP_RESCUE_MS 10000u
/* Drain queued UART bytes and the final OLED frame before shutting down. */
#define APP_SHUTDOWN_DRAIN_MS 600u

/* Empirical starting points: calibrate and justify these for the demo. */
#define APP_TEMP_MIN_C 10.0f
#define APP_TEMP_MAX_C 45.0f
#define APP_PRESSURE_MIN_HPA 850.0f
#define APP_PRESSURE_MAX_HPA 1100.0f
#define APP_HUMIDITY_MIN_PCT 20.0f
#define APP_HUMIDITY_MAX_PCT 85.0f
#define APP_ACCEL_MAX_MS2 30.0f
#define APP_GYRO_MAX_DPS 250.0f
#define APP_MAG_MAX_UT 200.0f

/* External I2C modules use I2C1 on PB8/PB9, separate from on-board I2C2. */
#define APP_OLED_ADDRESS 0x3cu
#define APP_COLOUR_ADDRESS 0x29u
#define APP_COLOUR_MIN_CLEAR 40u
#define APP_COLOUR_RATIO 1.35f

#endif
