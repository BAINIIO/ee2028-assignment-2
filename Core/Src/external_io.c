#include "external_io.h"
#include "game_display.h"
#include "app_config.h"
#include "stm32l4xx_hal.h"
#include <stdatomic.h>
#include <string.h>

#ifndef APP_ENABLE_IR
#define APP_ENABLE_IR 1
#endif
#ifndef APP_IR_ACTIVE_LOW
#define APP_IR_ACTIVE_LOW 1
#endif
#ifndef APP_BUZZER_ACTIVE
#define APP_BUZZER_ACTIVE 0
#endif

/* PB8/PB9 = Arduino D15/D14, PB1 = D6, PC5 = A0. No on-board sensor pins. */
#define EXTERNAL_I2C_TIMEOUT_MS 5u
#define EXTERNAL_OLED_TIMEOUT_MS 8u
#define EXTERNAL_OLED_RETRY_MS 1000u
#define EXTERNAL_OLED_CHUNK 32u
#define TCS_COMMAND 0x80u
#define TCS_AUTO_INCREMENT 0x20u
#define TCS_REG_ENABLE 0x00u
#define TCS_REG_ATIME 0x01u
#define TCS_REG_CONTROL 0x0fu
#define TCS_REG_ID 0x12u
#define TCS_REG_STATUS 0x13u
#define TCS_REG_CLEAR 0x14u
#define TCS_PON 0x01u
#define TCS_AEN 0x02u

static bool oled_online;
static bool colour_online;
static bool buzzer_online;
static ExternalColourSample colour_sample;

#if APP_ENABLE_LAPTOP_DISPLAY
/* The laptop reads these symbols without stopping the MCU. Odd means a
 * render is in progress; matching even sequence reads bracket a full frame. */
volatile uint32_t laptop_display_sequence;
volatile uint32_t laptop_display_terminal;
_Alignas(4) uint8_t laptop_display_buffer[128 * 64 / 8];
#endif

#if APP_ENABLE_OLED || APP_ENABLE_COLOUR
static I2C_HandleTypeDef external_i2c;
static bool i2c_online;

static bool i2c_available(void)
{
    if (!i2c_online) return false;
    /* A previous STOP or another device can briefly leave BUSY asserted.
     * Defer instead of entering HAL's separate 25 ms BUSY wait or disabling
     * every external module for the remainder of the run. */
    return __HAL_I2C_GET_FLAG(&external_i2c, I2C_FLAG_BUSY) == RESET;
}

static bool i2c_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_PeriphCLKInitTypeDef clock = {0};
    /* Timing below assumes the application's documented 80 MHz SYSCLK. */
    if (HAL_RCC_GetSysClockFreq() != 80000000u) return false;
    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_NOPULL;     /* External 3.3 V pull-ups are required. */
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &gpio);
    clock.PeriphClockSelection = RCC_PERIPHCLK_I2C1;
    clock.I2c1ClockSelection = RCC_I2C1CLKSOURCE_SYSCLK;
    if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK) return false;
    __HAL_RCC_I2C1_CLK_ENABLE();
    external_i2c.Instance = I2C1;
    /* ST's CubeL4 I2C_TwoBoards_MasterTx_SlaveRx example uses this
     * CubeMX timing at 80 MHz / 400 kHz, analog filter on, digital filter 0
     * (100 ns rise / 10 ns fall). External pull-ups must support Fast-mode. */
    external_i2c.Init.Timing = APP_OLED_I2C_TIMING_80MHZ;
    external_i2c.Init.OwnAddress1 = 0;
    external_i2c.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    external_i2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    external_i2c.Init.OwnAddress2 = 0;
    external_i2c.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    external_i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    external_i2c.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&external_i2c) != HAL_OK) return false;
    if (HAL_I2CEx_ConfigAnalogFilter(&external_i2c, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
        return false;
    return HAL_I2CEx_ConfigDigitalFilter(&external_i2c, 0) == HAL_OK;
}

static bool i2c_probe(uint16_t address)
{
    return i2c_available() &&
        HAL_I2C_IsDeviceReady(&external_i2c, (uint16_t)(address << 1), 1,
                             EXTERNAL_I2C_TIMEOUT_MS) == HAL_OK;
}
#endif

#if APP_ENABLE_COLOUR
typedef enum { COLOUR_OFF, COLOUR_POWER_WAIT, COLOUR_INTEGRATING } ColourState;
static ColourState colour_state;
static uint32_t colour_due_ms;

static bool colour_write(uint8_t reg, uint8_t value)
{
    return i2c_available() && HAL_I2C_Mem_Write(&external_i2c,
        APP_COLOUR_ADDRESS << 1, TCS_COMMAND | reg, I2C_MEMADD_SIZE_8BIT,
        &value, 1, EXTERNAL_I2C_TIMEOUT_MS) == HAL_OK;
}

static bool colour_read(uint8_t reg, uint8_t *data, uint16_t length)
{
    uint8_t command = TCS_COMMAND | reg;
    if (length > 1) command |= TCS_AUTO_INCREMENT;
    return i2c_available() && HAL_I2C_Mem_Read(&external_i2c,
        APP_COLOUR_ADDRESS << 1, command, I2C_MEMADD_SIZE_8BIT, data,
        length, EXTERNAL_I2C_TIMEOUT_MS) == HAL_OK;
}

static void colour_init(uint32_t now_ms)
{
    uint8_t id = 0;
    if (!i2c_probe(APP_COLOUR_ADDRESS) ||
        !colour_read(TCS_REG_ID, &id, 1) || (id != 0x44 && id != 0x4d)) return;
    /* 21 x 2.4 ms = 50.4 ms integration, 4x gain, power oscillator first. */
    if (!colour_write(TCS_REG_ATIME, 0xeb) ||
        !colour_write(TCS_REG_CONTROL, 0x01) ||
        !colour_write(TCS_REG_ENABLE, TCS_PON)) return;
    colour_online = true;
    colour_state = COLOUR_POWER_WAIT;
    colour_due_ms = now_ms + 3u;
}

static uint16_t little_endian16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void colour_poll(uint32_t now_ms)
{
    uint8_t status, data[8];
    if (!colour_online || (int32_t)(now_ms - colour_due_ms) < 0) return;
    if (colour_state == COLOUR_POWER_WAIT) {
        if (!colour_write(TCS_REG_ENABLE, TCS_PON | TCS_AEN)) goto failed;
        colour_state = COLOUR_INTEGRATING;
        colour_due_ms = now_ms + 55u;
        return;
    }
    colour_due_ms = now_ms + APP_EXTERNAL_PERIOD_MS;
    if (!colour_read(TCS_REG_STATUS, &status, 1)) goto failed;
    if ((status & 1u) == 0) {
        colour_sample.valid = false;
        return;
    }
    if (!colour_read(TCS_REG_CLEAR, data, sizeof data)) goto failed;
    colour_sample.clear = little_endian16(data);
    colour_sample.red = little_endian16(data + 2);
    colour_sample.green = little_endian16(data + 4);
    colour_sample.blue = little_endian16(data + 6);
    colour_sample.timestamp_ms = now_ms;
    colour_sample.colour = EXTERNAL_COLOUR_NONE;
    /* Saturation is 21 x 1024 counts at the selected integration time. */
    colour_sample.valid = colour_sample.clear >= APP_COLOUR_MIN_CLEAR &&
                          colour_sample.clear < 21504u;
    if (colour_sample.valid) {
        float clear = (float)colour_sample.clear;
        float red = (float)colour_sample.red / clear;
        float green = (float)colour_sample.green / clear;
        float blue = (float)colour_sample.blue / clear;
        if (red > green * APP_COLOUR_RATIO && red > blue * APP_COLOUR_RATIO)
            colour_sample.colour = EXTERNAL_COLOUR_RED;
        else if (green > red * APP_COLOUR_RATIO && green > blue * APP_COLOUR_RATIO)
            colour_sample.colour = EXTERNAL_COLOUR_GREEN;
    }
    return;
failed:
    colour_online = false;
    colour_state = COLOUR_OFF;
    colour_sample.valid = false;
    colour_sample.colour = EXTERNAL_COLOUR_NONE;
}
#endif

#if APP_ENABLE_OLED
static uint8_t oled_buffer[128 * 64 / 8];
/* An immutable transfer snapshot prevents drawing into a partially sent frame. */
static uint8_t oled_send_buffer[128 * 64 / 8];
static bool oled_dirty;
static bool oled_sending;
static uint16_t oled_offset;
static bool oled_recovery_pending;
static uint32_t oled_retry_ms;
/* Retained for a read-only SWD diagnostic snapshot. These count transport
 * events, not animation frames; the most recent failure survives recovery. */
static volatile uint32_t oled_busy_deferrals;
static volatile uint32_t oled_failures;
static volatile uint32_t oled_recoveries;
static volatile uint32_t oled_last_status;
static volatile uint32_t oled_last_error;
/* Count only successfully completed physical frames, independently of the
 * renderer / USB mirror. Read these over SWD to measure real OLED throughput. */
static volatile uint32_t oled_frames_completed;
static volatile uint32_t oled_last_frame_ms;
static volatile uint32_t oled_last_transfer_ms;
static volatile uint32_t oled_longest_transfer_ms;
static uint32_t oled_transfer_started_ms;

_Static_assert(sizeof oled_send_buffer % EXTERNAL_OLED_CHUNK == 0,
               "OLED chunks must divide the complete framebuffer");

static HAL_StatusTypeDef oled_write(uint8_t *bytes, uint16_t length)
{
    if (!i2c_available()) {
        ++oled_busy_deferrals;
        return HAL_BUSY;
    }
    HAL_StatusTypeDef status = HAL_I2C_Master_Transmit(&external_i2c,
        APP_OLED_ADDRESS << 1, bytes, length, EXTERNAL_OLED_TIMEOUT_MS);
    if (status == HAL_BUSY) {
        ++oled_busy_deferrals;
    } else if (status != HAL_OK) {
        ++oled_failures;
        oled_last_status = (uint32_t)status;
        oled_last_error = HAL_I2C_GetError(&external_i2c);
    }
    return status;
}

static HAL_StatusTypeDef oled_configure(void)
{
    /* Control byte 00, display off, 128x64, internal charge pump, horizontal RAM. */
    uint8_t commands[] = {0x00, 0xae, 0xd5,0x80, 0xa8,0x3f, 0xd3,0x00,
        0x40, 0x8d,0x14, 0x20,0x00, 0xa1, 0xc8, 0xda,0x12,
        0x81,0x7f, 0xd9,0xf1, 0xdb,0x40, 0xa4, 0xa6, 0xaf};
    return oled_write(commands, sizeof commands);
}

static void oled_init(void)
{
    if (!i2c_probe(APP_OLED_ADDRESS) || oled_configure() != HAL_OK) return;
    oled_online = true;
    oled_dirty = true;
}

static void oled_poll(uint32_t now_ms, bool allow_recovery)
{
    HAL_StatusTypeDef status;
    if (!oled_online) {
        /* Only a previously working display is retried. An absent boot-time
         * module stays absent. A retry is one bounded command transaction and
         * only runs on an idle bus; shutdown drains never initiate recovery. */
        if (!allow_recovery || !oled_recovery_pending ||
            (int32_t)(now_ms - oled_retry_ms) < 0) return;
        status = oled_configure();
        if (status == HAL_BUSY) return;
        oled_retry_ms = now_ms + EXTERNAL_OLED_RETRY_MS;
        if (status != HAL_OK) return;
        oled_online = true;
        oled_recovery_pending = false;
        oled_sending = false;
        oled_dirty = true;
        ++oled_recoveries;
        return;
    }
    if (!oled_sending) {
        if (!oled_dirty) return;
        uint8_t address[] = {0x00,0x21,0x00,0x7f,0x22,0x00,0x07};
        status = oled_write(address, sizeof address);
        if (status == HAL_BUSY) return;
        if (status != HAL_OK) goto failed;
        memcpy(oled_send_buffer, oled_buffer, sizeof oled_send_buffer);
        oled_sending = true;
        oled_dirty = false;
        oled_offset = 0;
        oled_transfer_started_ms = now_ms;
        return;                 /* At most one bounded OLED transaction per poll. */
    }
    uint8_t packet[EXTERNAL_OLED_CHUNK + 1];
    packet[0] = 0x40;           /* Data, continuing horizontal GDDRAM address. */
    memcpy(packet + 1, oled_send_buffer + oled_offset, EXTERNAL_OLED_CHUNK);
    status = oled_write(packet, sizeof packet);
    if (status == HAL_BUSY) return;
    if (status != HAL_OK) goto failed;
    oled_offset += EXTERNAL_OLED_CHUNK;
    if (oled_offset >= sizeof oled_buffer) {
        oled_sending = false;
        ++oled_frames_completed;
        oled_last_frame_ms = HAL_GetTick();
        oled_last_transfer_ms = oled_last_frame_ms - oled_transfer_started_ms;
        if (oled_last_transfer_ms > oled_longest_transfer_ms)
            oled_longest_transfer_ms = oled_last_transfer_ms;
    }
    return;
failed:
    oled_online = false;
    oled_sending = false;
    /* A failed transaction may have sent some bytes. Reconfigure and start
     * the whole frame at column/page zero instead of resending at an unknown
     * SSD1306 RAM address. Preserve the latest drawing buffer while offline. */
    oled_dirty = true;
    oled_recovery_pending = true;
    oled_retry_ms = now_ms + EXTERNAL_OLED_RETRY_MS;
}
#endif

#if APP_ENABLE_BUZZER
typedef struct {uint16_t hz, duration_ms;} Note;
typedef struct {const Note *notes; uint8_t length, priority;} Tune;
static const Note charge_notes[] = {{1200,70},{0,30},{1600,80}};
static const Note shot_notes[] = {{2400,35},{1800,35},{1000,70}};
static const Note warning_notes[] = {{700,120},{0,100},{700,120}};
static const Note attack_notes[] = {{420,200},{0,80},{420,200},{0,80},{330,250}};
static const Note rescue_notes[] = {{880,120},{1100,120},{1320,180}};
static const Note win_notes[] = {{523,120},{659,120},{784,120},{1047,320}};
static const Note lose_notes[] = {{784,150},{622,150},{523,180},{392,350}};
#define TUNE(notes, priority) {notes, (uint8_t)(sizeof notes / sizeof notes[0]), priority}
static const Tune tunes[] = {
    TUNE(charge_notes,1), TUNE(shot_notes,2), TUNE(warning_notes,3),
    TUNE(attack_notes,5), TUNE(rescue_notes,5), TUNE(win_notes,4), TUNE(lose_notes,6)
};
static const Tune *active_tune;
static uint8_t note_index;
static uint32_t note_due_ms;
#if !APP_BUZZER_ACTIVE
static TIM_HandleTypeDef buzzer_timer;
static bool buzzer_timer_running;
#endif

static void buzzer_output(bool on)
{
    HAL_GPIO_WritePin(APP_BUZZER_GPIO_PORT, APP_BUZZER_GPIO_PIN,
        (on != (APP_BUZZER_ACTIVE_LOW != 0)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

#if !APP_BUZZER_ACTIVE
/* A bounded timer interrupt toggles the configured GPIO at twice the
 * requested pitch without requiring a TIM2 alternate-function pin. No
 * foreground polling, HAL delays or other peripheral work occurs here. */
void TIM2_IRQHandler(void)
{
    if (__HAL_TIM_GET_FLAG(&buzzer_timer, TIM_FLAG_UPDATE) != RESET &&
        __HAL_TIM_GET_IT_SOURCE(&buzzer_timer, TIM_IT_UPDATE) != RESET) {
        __HAL_TIM_CLEAR_IT(&buzzer_timer, TIM_IT_UPDATE);
        HAL_GPIO_TogglePin(APP_BUZZER_GPIO_PORT, APP_BUZZER_GPIO_PIN);
    }
}
#endif

static void buzzer_frequency(uint16_t hz)
{
    if (!buzzer_online) return;
#if APP_BUZZER_ACTIVE
    buzzer_output(hz != 0u);
#else
    /* Mask first so an old half-cycle cannot race a note change or silence. */
    HAL_NVIC_DisableIRQ(TIM2_IRQn);
    if (buzzer_timer_running) {
        (void)HAL_TIM_Base_Stop_IT(&buzzer_timer);
        buzzer_timer_running = false;
    }
    buzzer_output(false);
    HAL_NVIC_ClearPendingIRQ(TIM2_IRQn);
    if (hz == 0u) return;

    uint32_t period = 1000000u / (2u * (uint32_t)hz);
    __HAL_TIM_SET_AUTORELOAD(&buzzer_timer, period - 1u);
    __HAL_TIM_SET_COUNTER(&buzzer_timer, 0);
    /* Reload prescaler/counter and clear the software-generated update before
     * unmasking. The first GPIO edge follows one full half-period. */
    buzzer_timer.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&buzzer_timer, TIM_FLAG_UPDATE);
    HAL_NVIC_ClearPendingIRQ(TIM2_IRQn);
    if (HAL_TIM_Base_Start_IT(&buzzer_timer) == HAL_OK) {
        buzzer_timer_running = true;
        HAL_NVIC_EnableIRQ(TIM2_IRQn);
    } else {
        buzzer_online = false;
    }
#endif
}

static void buzzer_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    APP_BUZZER_GPIO_CLOCK_ENABLE();
    gpio.Pin = APP_BUZZER_GPIO_PIN;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    buzzer_output(false);
    HAL_GPIO_Init(APP_BUZZER_GPIO_PORT, &gpio);
#if APP_BUZZER_ACTIVE
    buzzer_online = true;
#else
    RCC_ClkInitTypeDef clocks;
    uint32_t flash_latency;
    HAL_RCC_GetClockConfig(&clocks, &flash_latency);
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq();
    if (clocks.APB1CLKDivider != RCC_HCLK_DIV1) timer_clock *= 2u;
    if (timer_clock < 1000000u || timer_clock % 1000000u != 0) return;
    __HAL_RCC_TIM2_CLK_ENABLE();
    HAL_NVIC_DisableIRQ(TIM2_IRQn);
    HAL_NVIC_ClearPendingIRQ(TIM2_IRQn);
    HAL_NVIC_SetPriority(TIM2_IRQn, 6, 0);
    buzzer_timer.Instance = TIM2;
    buzzer_timer.Init.Prescaler = timer_clock / 1000000u - 1u;
    buzzer_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
    buzzer_timer.Init.Period = 999u;
    buzzer_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    buzzer_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&buzzer_timer) != HAL_OK) return;
    __HAL_TIM_CLEAR_FLAG(&buzzer_timer, TIM_FLAG_UPDATE);
    buzzer_online = true;
#endif
}

static void buzzer_poll(uint32_t now_ms)
{
    if (!active_tune || (int32_t)(now_ms - note_due_ms) < 0) return;
    if (++note_index >= active_tune->length) {
        active_tune = NULL;
        buzzer_frequency(0);
        return;
    }
    const Note *note = &active_tune->notes[note_index];
    buzzer_frequency(note->hz);
    note_due_ms = now_ms + note->duration_ms;
}
#endif

#if APP_ENABLE_IR
static bool ir_present, ir_raw;
static uint32_t ir_changed_ms;

static bool ir_read(void)
{
    bool high = HAL_GPIO_ReadPin(APP_IR_GPIO_PORT, APP_IR_GPIO_PIN) == GPIO_PIN_SET;
    return APP_IR_ACTIVE_LOW ? !high : high;
}

static void ir_init(uint32_t now_ms)
{
    GPIO_InitTypeDef gpio = {0};
    APP_IR_GPIO_CLOCK_ENABLE();
    gpio.Pin = APP_IR_GPIO_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = APP_IR_ACTIVE_LOW ? GPIO_PULLUP : GPIO_PULLDOWN;
    HAL_GPIO_Init(APP_IR_GPIO_PORT, &gpio);
    ir_raw = ir_read();
    ir_present = false;
    ir_changed_ms = now_ms;
}

static void ir_poll(uint32_t now_ms)
{
    bool detected = ir_read();
    if (detected != ir_raw) {
        ir_raw = detected;
        ir_changed_ms = now_ms;
    }
    if ((uint32_t)(now_ms - ir_changed_ms) >= 30u) ir_present = ir_raw;
}
#endif

void External_Init(uint32_t now_ms)
{
    oled_online = false;
    colour_online = false;
    buzzer_online = false;
    memset(&colour_sample, 0, sizeof colour_sample);
#if APP_ENABLE_LAPTOP_DISPLAY
    laptop_display_sequence = 0;
    laptop_display_terminal = 0;
    memset(laptop_display_buffer, 0, sizeof laptop_display_buffer);
#endif
#if APP_ENABLE_OLED || APP_ENABLE_COLOUR
    i2c_online = i2c_init();
#endif
#if APP_ENABLE_OLED
    oled_sending = false;
    oled_dirty = false;
    oled_recovery_pending = false;
    oled_retry_ms = now_ms;
    oled_busy_deferrals = 0;
    oled_failures = 0;
    oled_recoveries = 0;
    oled_last_status = (uint32_t)HAL_OK;
    oled_last_error = 0;
    oled_frames_completed = 0;
    oled_last_frame_ms = 0;
    oled_last_transfer_ms = 0;
    oled_longest_transfer_ms = 0;
    oled_transfer_started_ms = now_ms;
    memset(oled_buffer, 0, sizeof oled_buffer);
    if (i2c_online) oled_init();
#endif
#if APP_ENABLE_COLOUR
    colour_state = COLOUR_OFF;
    if (i2c_online) colour_init(now_ms);
#endif
#if APP_ENABLE_BUZZER
    active_tune = NULL;
#if !APP_BUZZER_ACTIVE
    buzzer_timer_running = false;
#endif
    buzzer_init();
#endif
#if APP_ENABLE_IR
    ir_init(now_ms);
#endif
    (void)now_ms;
}

void External_Poll(uint32_t now_ms)
{
#if APP_ENABLE_IR
    ir_poll(now_ms);
#endif
#if APP_ENABLE_BUZZER
    buzzer_poll(now_ms);
#endif
#if APP_ENABLE_COLOUR
    colour_poll(now_ms);
#endif
#if APP_ENABLE_OLED
    oled_poll(now_ms, true);
#endif
    (void)now_ms;
}

ExternalColourSample External_GetColour(void)
{
    return colour_sample;
}

void External_SetDisplay(const ExternalDisplayFrame *frame)
{
    if (!frame) return;
#if APP_ENABLE_LAPTOP_DISPLAY
    ++laptop_display_sequence;
    atomic_thread_fence(memory_order_seq_cst);
    GameDisplay_Render(laptop_display_buffer, frame);
    laptop_display_terminal = frame->scene == EXTERNAL_SCENE_TERMINATED;
    atomic_thread_fence(memory_order_seq_cst);
    ++laptop_display_sequence;
#endif
#if APP_ENABLE_OLED
    if (!oled_online && !oled_recovery_pending) return;
#if APP_ENABLE_LAPTOP_DISPLAY
    memcpy(oled_buffer, laptop_display_buffer, sizeof oled_buffer);
#else
    GameDisplay_Render(oled_buffer, frame);
#endif
    /* If updated mid-transfer, finish it and send a whole new frame next. */
    oled_dirty = true;
#endif
}

void External_FinishDisplay(void)
{
#if APP_ENABLE_OLED
    oled_poll(HAL_GetTick(), false);
#endif
}

bool External_DisplayIdle(void)
{
#if APP_ENABLE_OLED
    return !oled_online || (!oled_dirty && !oled_sending);
#else
    return true;
#endif
}

void External_Play(ExternalSound sound, uint32_t now_ms)
{
#if APP_ENABLE_BUZZER
    if (!buzzer_online || (unsigned)sound >= sizeof tunes / sizeof tunes[0]) return;
    const Tune *next = &tunes[sound];
    if (active_tune && next->priority < active_tune->priority) return;
    active_tune = next;
    note_index = 0;
    buzzer_frequency(next->notes[0].hz);
    note_due_ms = now_ms + next->notes[0].duration_ms;
#else
    (void)sound;
    (void)now_ms;
#endif
}

void External_AllStop(void)
{
#if APP_ENABLE_BUZZER
    active_tune = NULL;
    buzzer_frequency(0);
#endif
}

bool External_ColourOnline(void) {return colour_online;}
bool External_DisplayOnline(void) {return oled_online;}
bool External_BuzzerOnline(void) {return buzzer_online;}
bool External_IRPresent(void)
{
#if APP_ENABLE_IR
    return ir_present;
#else
    return false;
#endif
}
bool External_IREnabled(void) {return APP_ENABLE_IR != 0;}
