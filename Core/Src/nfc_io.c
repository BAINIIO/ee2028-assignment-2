/* ST25DV04K: bounded I2C2 service; EEPROM writes commit NDEF length last. */
#include "nfc_io.h"
#include "nfc_ndef.h"
#include "app_config.h"
#include "stm32l4xx_hal.h"
#include <stdio.h>
#include <string.h>

#ifndef APP_ENABLE_NFC
#define APP_ENABLE_NFC 1
#endif
#if APP_ENABLE_NFC
extern I2C_HandleTypeDef hI2cHandler;
#define NFC_DATA 0xa6u
#define NFC_SYSTEM 0xaeu
#define NFC_TIMEOUT_MS 2u
#define NFC_CHUNK 16u
#define NFC_IMAGE_CAPACITY 256u
#define NFC_RF_MANAGEMENT 0x2003u
#define NFC_INTERRUPT_STATUS 0x2005u
#define NFC_FIELD_STATUS 0x2002u
#define NFC_SLEEP 0x02u

typedef enum {
    NFC_PROBE, NFC_PASSWORD, NFC_SESSION, NFC_GPO_READ, NFC_GPO_WRITE,
    NFC_RF_DEFAULT_READ, NFC_RF_DEFAULT_WRITE, NFC_SLEEP_WRITE,
    NFC_COMPARE, NFC_INVALIDATE, NFC_WRITE, NFC_COMMIT, NFC_VERIFY,
    NFC_WAKE, NFC_IDLE
} NfcStep;
static NfcStep step;
static bool online, ready, halted, have_tap, field_on, gpo_configured;
static volatile bool edge_pending;
static volatile uint32_t edge_time;
static uint32_t next_ms, last_check_ms, last_tap_ms, tap_time, tap_count, errors;
static uint32_t wanted_generation, active_generation;
static uint8_t desired[NFC_IMAGE_CAPACITY], image[NFC_IMAGE_CAPACITY];
static uint16_t desired_size, image_size, offset;
static char base_url[180];
static bool have_previous_tap;
static uint8_t stop_phase, stop_attempts;
static bool quiet;
static uint32_t stop_due_ms;

static bool bus_free(void)
{
    return __HAL_I2C_GET_FLAG(&hI2cHandler, I2C_FLAG_BUSY) == RESET;
}

static bool read_bytes(uint16_t device, uint16_t address, uint8_t *data, uint16_t size)
{
    return bus_free() && HAL_I2C_Mem_Read(&hI2cHandler, device, address,
        I2C_MEMADD_SIZE_16BIT, data, size, NFC_TIMEOUT_MS) == HAL_OK;
}

static bool write_bytes(uint16_t device, uint16_t address, const uint8_t *data, uint16_t size)
{
    return bus_free() && HAL_I2C_Mem_Write(&hI2cHandler, device, address,
        I2C_MEMADD_SIZE_16BIT, (uint8_t *)data, size, NFC_TIMEOUT_MS) == HAL_OK;
}

static void retry(uint32_t now)
{
    ++errors;
    ready = false;
    step = NFC_PROBE;
    next_ms = now + 1000u;
}

static void record_tap(uint32_t now)
{
    if (have_previous_tap && (uint32_t)(now - last_tap_ms) < 1500u) return;
    have_previous_tap = true;
    last_tap_ms = tap_time = now;
    ++tap_count;
    have_tap = true;
}

void Nfc_SignalEdge(uint32_t now_ms)
{
    edge_time = now_ms;
    edge_pending = true;
}

void EXTI4_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_4);
}

bool Nfc_SetMission(uint8_t mission)
{
    if (mission > 2u || base_url[0] == '\0' || halted) return false;
    char url[210];
    int length = snprintf(url, sizeof url, "%s?mission=%u", base_url, (unsigned)mission);
    if (length <= 0 || (size_t)length >= sizeof url) return false;
    size_t size = NfcNdef_BuildUri(url, desired, sizeof desired);
    if (size == 0u) return false;
    desired_size = (uint16_t)size;
    ++wanted_generation;
    ready = false;
    return true;
}

void Nfc_Init(const char *dashboard_url, uint32_t now_ms)
{
    online = ready = halted = have_tap = field_on = gpo_configured = false;
    edge_pending = false;
    tap_count = errors = 0;
    have_previous_tap = false;
    stop_phase = stop_attempts = 0;
    quiet = false;
    desired_size = image_size = offset = 0;
    wanted_generation = active_generation = 0;
    base_url[0] = '\0';
    if (dashboard_url && strlen(dashboard_url) < sizeof base_url)
        memcpy(base_url, dashboard_url, strlen(dashboard_url) + 1u);
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOE_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_IT_FALLING;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &gpio);
    HAL_NVIC_SetPriority(EXTI4_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);
    step = NFC_PROBE;
    next_ms = now_ms;
    last_check_ms = now_ms;
    (void)Nfc_SetMission(0);
}

void Nfc_Poll(uint32_t now, bool allow_update)
{
    if (halted || (int32_t)(now - next_ms) < 0 || !bus_free()) return;
    uint8_t data[NFC_CHUNK];
    uint8_t value;
    switch (step) {
    case NFC_PROBE:
        if (!read_bytes(NFC_DATA, 0, data, 4)) { online = false; retry(now); return; }
        online = true;
        step = NFC_PASSWORD;
        break;
    case NFC_PASSWORD: {
        /* Present the documented factory I2C password; never change or lock it. */
        uint8_t password[17] = {0};
        password[8] = 0x09;
        if (!write_bytes(NFC_SYSTEM, 0x0900, password, sizeof password)) {
            ++errors; step = NFC_SLEEP_WRITE; break;
        }
        step = NFC_SESSION;
        next_ms = now + 1u;
        break;
    }
    case NFC_SESSION:
        if (!read_bytes(NFC_DATA, 0x2004, &value, 1)) { retry(now); break; }
        step = (value & 1u) != 0u ? NFC_GPO_READ : NFC_SLEEP_WRITE;
        break;
    case NFC_GPO_READ:
        if (!read_bytes(NFC_SYSTEM, 0, &value, 1)) { retry(now); break; }
        if ((value & 0x88u) == 0x88u) { gpo_configured = true; step = NFC_RF_DEFAULT_READ; }
        else step = NFC_GPO_WRITE;
        break;
    case NFC_GPO_WRITE:
        value = 0x88; /* Enable GPO with RF-field changes. */
        if (!write_bytes(NFC_SYSTEM, 0, &value, 1)) { retry(now); break; }
        gpo_configured = true;
        next_ms = now + 7u;
        step = NFC_RF_DEFAULT_READ;
        break;
    case NFC_RF_DEFAULT_READ:
        if (!read_bytes(NFC_SYSTEM, 3, &value, 1)) { retry(now); break; }
        step = (value & 3u) == NFC_SLEEP ? NFC_SLEEP_WRITE : NFC_RF_DEFAULT_WRITE;
        break;
    case NFC_RF_DEFAULT_WRITE:
        /* Default RF silence on loss of MCU power; boot explicitly wakes it. */
        value = NFC_SLEEP;
        if (!write_bytes(NFC_SYSTEM, 3, &value, 1)) { retry(now); break; }
        next_ms = now + 7u;
        step = NFC_SLEEP_WRITE;
        break;
    case NFC_SLEEP_WRITE:
        value = NFC_SLEEP;
        if (!write_bytes(NFC_DATA, NFC_RF_MANAGEMENT, &value, 1)) { retry(now); break; }
        if (desired_size == 0u) { step = NFC_IDLE; break; }
        memcpy(image, desired, desired_size);
        image_size = desired_size;
        active_generation = wanted_generation;
        offset = 0;
        step = NFC_COMPARE;
        break;
    case NFC_COMPARE:
    case NFC_VERIFY: {
        uint16_t count = (uint16_t)(image_size - offset);
        if (count > NFC_CHUNK) count = NFC_CHUNK;
        if (!read_bytes(NFC_DATA, offset, data, count)) { retry(now); break; }
        if (memcmp(data, image + offset, count) != 0) {
            if (step == NFC_VERIFY) { retry(now); break; }
            step = NFC_INVALIDATE;
            break;
        }
        offset = (uint16_t)(offset + count);
        if (offset == image_size) step = NFC_WAKE;
        break;
    }
    case NFC_INVALIDATE:
        data[0] = 0x03; data[1] = 0;
        if (!write_bytes(NFC_DATA, 4, data, 2)) { retry(now); break; }
        offset = 0;
        step = NFC_WRITE;
        next_ms = now + 7u;
        break;
    case NFC_WRITE: {
        uint16_t count = (uint16_t)(image_size - offset);
        if (count > NFC_CHUNK) count = NFC_CHUNK;
        memcpy(data, image + offset, count);
        if (offset <= 5u && (uint16_t)(offset + count) > 5u) data[5u - offset] = 0;
        if (!write_bytes(NFC_DATA, offset, data, count)) { retry(now); break; }
        offset = (uint16_t)(offset + count);
        if (offset == image_size) step = NFC_COMMIT;
        next_ms = now + 7u;
        break;
    }
    case NFC_COMMIT:
        if (!write_bytes(NFC_DATA, 5, image + 5, 1)) { retry(now); break; }
        offset = 0;
        step = NFC_VERIFY;
        next_ms = now + 7u;
        break;
    case NFC_WAKE:
        value = 0;
        if (!write_bytes(NFC_DATA, NFC_RF_MANAGEMENT, &value, 1)) { retry(now); break; }
        ready = active_generation == wanted_generation;
        step = NFC_IDLE;
        break;
    case NFC_IDLE:
        if (active_generation != wanted_generation && allow_update && !field_on) {
            step = NFC_SLEEP_WRITE;
            break;
        }
        if (!edge_pending && (uint32_t)(now - last_check_ms) < 50u) return;
        last_check_ms = now;
        if (gpo_configured) {
            if (!read_bytes(NFC_DATA, NFC_INTERRUPT_STATUS, &value, 1)) { retry(now); break; }
            uint32_t timestamp = edge_pending ? edge_time : now;
            edge_pending = false;
            if ((value & 0x10u) != 0u) record_tap(timestamp);
            if ((value & 0x10u) != 0u) field_on = true;
            if ((value & 0x08u) != 0u) field_on = false;
        } else {
            if (!read_bytes(NFC_DATA, NFC_FIELD_STATUS, &value, 1)) { retry(now); break; }
            bool field = (value & 0x04u) != 0u;
            if (field && !field_on) record_tap(now);
            field_on = field;
            edge_pending = false;
        }
        break;
    }
}

bool Nfc_Online(void) { return online; }
bool Nfc_Ready(void) { return online && ready && !halted; }
uint32_t Nfc_Taps(void) { return tap_count; }
uint32_t Nfc_Errors(void) { return errors; }
bool Nfc_PopTap(uint32_t *time_ms)
{
    if (!have_tap) return false;
    if (time_ms) *time_ms = tap_time;
    have_tap = false;
    return true;
}
void Nfc_BeginStop(void)
{
    if (halted) return;
    halted = true;
    ready = false;
    HAL_NVIC_DisableIRQ(EXTI4_IRQn);
    stop_phase = 1;
    stop_attempts = 0;
    stop_due_ms = HAL_GetTick();
}
void Nfc_FinishStop(uint32_t now)
{
    if (!halted || stop_phase == 3u || (int32_t)(now - stop_due_ms) < 0) return;
    uint8_t value = NFC_SLEEP;
    bool ok;
    if (stop_phase == 1u) {
        ok = write_bytes(NFC_DATA, NFC_RF_MANAGEMENT, &value, 1);
        if (ok) { stop_phase = 2; return; }
    } else {
        ok = read_bytes(NFC_DATA, NFC_RF_MANAGEMENT, &value, 1);
        if (ok && (value & 3u) == NFC_SLEEP) {
            quiet = true;
            stop_phase = 3;
            return;
        }
    }
    ++errors;
    if (++stop_attempts >= 3u) stop_phase = 3;
    else { stop_phase = 1; stop_due_ms = now + 2u; }
}
bool Nfc_StopDone(void) { return halted && stop_phase == 3u; }
bool Nfc_Quiet(void) { return quiet; }
void Nfc_Stop(void)
{
    Nfc_BeginStop();
    Nfc_FinishStop(HAL_GetTick());
    Nfc_FinishStop(HAL_GetTick());
}
#else
void Nfc_Init(const char *url, uint32_t now) {(void)url; (void)now;}
void Nfc_Poll(uint32_t now, bool allow) {(void)now; (void)allow;}
bool Nfc_SetMission(uint8_t mission) {(void)mission; return false;}
void Nfc_SignalEdge(uint32_t now) {(void)now;}
bool Nfc_Online(void) {return false;}
bool Nfc_Ready(void) {return false;}
uint32_t Nfc_Taps(void) {return 0;}
uint32_t Nfc_Errors(void) {return 0;}
bool Nfc_PopTap(uint32_t *now) {(void)now; return false;}
void Nfc_BeginStop(void) {}
void Nfc_FinishStop(uint32_t now) {(void)now;}
bool Nfc_StopDone(void) {return true;}
bool Nfc_Quiet(void) {return true;}
void Nfc_Stop(void) {}
#endif
