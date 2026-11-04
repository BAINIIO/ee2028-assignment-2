/* Exercise the real OLED transport, including the observed ACK failure, with
 * an emulated SSD1306 RAM address and scripted HAL/bus results. */
#define APP_ENABLE_OLED 1
#define APP_ENABLE_COLOUR 0
#define APP_ENABLE_BUZZER 0
#define APP_ENABLE_IR 0
#include "../Core/Src/external_io.c"

#include <assert.h>
#include <stdio.h>

uint32_t fake_i2c_busy;
static uint32_t fake_tick;
static bool fake_present = true;
static HAL_StatusTypeDef fake_next_status;
static unsigned fake_transmits;
static unsigned fake_probes;
static unsigned fake_configures;
static unsigned fake_addresses;
static uint8_t fake_ram[1024];
static size_t fake_ram_offset;

uint32_t HAL_GetTick(void) { return fake_tick; }
uint32_t HAL_RCC_GetSysClockFreq(void) { return 80000000u; }
void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *gpio)
{
    assert(port == GPIOB && gpio->Pin == (GPIO_PIN_8 | GPIO_PIN_9));
}
HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(const RCC_PeriphCLKInitTypeDef *clock)
{
    assert(clock->I2c1ClockSelection == RCC_I2C1CLKSOURCE_SYSCLK);
    return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *handle)
{
    assert(handle->Instance == I2C1 && handle->Init.Timing == 0x00f02b86u);
    return HAL_OK;
}
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef *handle, uint32_t setting)
{
    (void)handle; assert(setting == I2C_ANALOGFILTER_ENABLE); return HAL_OK;
}
HAL_StatusTypeDef HAL_I2CEx_ConfigDigitalFilter(I2C_HandleTypeDef *handle, uint32_t setting)
{
    (void)handle; assert(setting == 0u); return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *handle, uint16_t address,
                                     uint32_t trials, uint32_t timeout)
{
    (void)handle;
    assert(address == APP_OLED_ADDRESS << 1 && trials == 1u && timeout == 5u);
    ++fake_probes;
    return fake_present ? HAL_OK : HAL_ERROR;
}
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *handle, uint16_t address,
                                       uint8_t *bytes, uint16_t length, uint32_t timeout)
{
    assert(fake_i2c_busy == 0u && address == APP_OLED_ADDRESS << 1);
    assert(timeout == 8u && length <= 33u);
    ++fake_transmits;
    HAL_StatusTypeDef status = fake_next_status;
    fake_next_status = HAL_OK;
    if (status != HAL_OK) {
        handle->ErrorCode = status == HAL_ERROR ? HAL_I2C_ERROR_AF : 0u;
        /* Failed data may have partly changed the display's RAM pointer. */
        if (status == HAL_ERROR && bytes[0] == 0x40u) ++fake_ram_offset;
        return status;
    }
    handle->ErrorCode = 0u;
    if (bytes[0] == 0u && bytes[1] == 0xaeu) {
        ++fake_configures;
    } else if (bytes[0] == 0u && bytes[1] == 0x21u) {
        assert(length == 7u && bytes[2] == 0u && bytes[3] == 0x7fu);
        fake_ram_offset = 0u;
        ++fake_addresses;
    } else {
        assert(bytes[0] == 0x40u && length == EXTERNAL_OLED_CHUNK + 1u);
        assert(fake_ram_offset + EXTERNAL_OLED_CHUNK <= sizeof fake_ram);
        memcpy(fake_ram + fake_ram_offset, bytes + 1, EXTERNAL_OLED_CHUNK);
        fake_ram_offset += EXTERNAL_OLED_CHUNK;
    }
    return HAL_OK;
}
uint32_t HAL_I2C_GetError(const I2C_HandleTypeDef *handle) { return handle->ErrorCode; }

static void poll(uint32_t now)
{
    unsigned previous = fake_transmits;
    fake_tick = now;
    External_Poll(now);
    assert(fake_transmits - previous <= 1u);
}

static void test_busy_then_nack_recovery(void)
{
    ExternalDisplayFrame frame = { .scene = EXTERNAL_SCENE_BATTLE,
        .target = EXTERNAL_COLOUR_RED, .energy = 3u, .lives = 3u,
        .target_period_ms = 4000u, .goal_hits = 6u };
    uint8_t expected[1024];
    External_Init(0);
    assert(External_DisplayOnline() && fake_probes == 1u && fake_configures == 1u);
    External_SetDisplay(&frame);
    poll(1);
    assert(oled_offset == 0u && oled_sending);
    fake_i2c_busy = 1;
    unsigned previous = fake_transmits;
    for (uint32_t now = 2u; now <= 2000u; ++now) poll(now);
    assert(fake_transmits == previous && oled_offset == 0u);
    assert(External_DisplayOnline() && i2c_online && oled_busy_deferrals == 1999u);
    fake_i2c_busy = 0;
    fake_next_status = HAL_BUSY;
    poll(2001);
    assert(oled_offset == 0u && External_DisplayOnline());
    for (uint32_t now = 2002u; oled_offset < 544u; ++now) poll(now);
    assert(oled_offset == 544u);
    fake_next_status = HAL_ERROR; /* Actual board failure was NACK / AF=0x4. */
    poll(3000);
    assert(!External_DisplayOnline() && oled_recovery_pending && !oled_sending);
    assert(oled_failures == 1u && oled_last_status == HAL_ERROR);
    assert(oled_last_error == HAL_I2C_ERROR_AF && oled_retry_ms == 4000u);
    /* Keep the newest visual frame while communication is temporarily down. */
    frame.now_ms = 3300u; frame.energy = 1u; frame.score = 15;
    frame.target = EXTERNAL_COLOUR_GREEN;
    External_SetDisplay(&frame);
    GameDisplay_Render(expected, &frame);
    assert((laptop_display_sequence & 1u) == 0u);
    assert(memcmp(laptop_display_buffer, expected, sizeof expected) == 0);
    previous = fake_transmits;
    poll(3999);
    assert(fake_transmits == previous);
    fake_tick = 9000u;
    External_FinishDisplay(); /* Terminal drain must never initiate recovery. */
    assert(fake_transmits == previous && !External_DisplayOnline());
    fake_i2c_busy = 1u;
    poll(4000);
    assert(fake_transmits == previous);
    fake_i2c_busy = 0u;
    poll(4001);
    assert(External_DisplayOnline() && oled_recoveries == 1u && fake_configures == 2u);
    poll(4002);
    assert(oled_offset == 0u && fake_addresses == 2u);
    for (uint32_t now = 4003u; !External_DisplayIdle(); ++now) poll(now);
    assert(fake_ram_offset == sizeof fake_ram);
    assert(memcmp(fake_ram, expected, sizeof expected) == 0);
    assert(oled_last_error == HAL_I2C_ERROR_AF); /* Diagnostic survives recovery. */
}

static void test_retry_backoff_and_absent_boot(void)
{
    ExternalDisplayFrame frame = {0};
    External_SetDisplay(&frame);
    fake_next_status = HAL_TIMEOUT;
    poll(10000);
    assert(!External_DisplayOnline() && oled_failures == 2u);
    fake_next_status = HAL_ERROR;
    poll(11000);
    assert(oled_failures == 3u && oled_retry_ms == 12000u);
    unsigned previous = fake_transmits;
    for (uint32_t now = 11001u; now < 12000u; ++now) poll(now);
    assert(fake_transmits == previous);
    poll(12000);
    assert(External_DisplayOnline() && oled_recoveries == 2u);
    fake_present = false;
    External_Init(20000);
    assert(!External_DisplayOnline() && !oled_recovery_pending);
    previous = fake_transmits;
    unsigned probes = fake_probes;
    frame.scene = EXTERNAL_SCENE_BATTLE;
    frame.target = EXTERNAL_COLOUR_RED;
    frame.energy = 3u;
    frame.lives = 3u;
    frame.target_period_ms = 4000u;
    External_SetDisplay(&frame);
    uint8_t expected[1024];
    GameDisplay_Render(expected, &frame);
    assert(laptop_display_sequence == 2u && !oled_dirty);
    assert(memcmp(laptop_display_buffer, expected, sizeof expected) == 0);
    for (uint32_t now = 20001u; now < 40000u; ++now) poll(now);
    assert(fake_transmits == previous && fake_probes == probes);
}

static void test_fast_frames_keep_transfer_snapshot_immutable(void)
{
    fake_present = true;
    fake_i2c_busy = 0;
    fake_next_status = HAL_OK;
    External_Init(0);
    assert(oled_frames_completed == 0u);
    ExternalDisplayFrame first = { .scene = EXTERNAL_SCENE_BATTLE,
        .target = EXTERNAL_COLOUR_RED, .energy = 3u, .lives = 3u,
        .target_period_ms = 4000u, .goal_hits = 6u };
    ExternalDisplayFrame second = first;
    second.now_ms = 900u;
    second.energy = 1u;
    second.score = 15;
    second.target = EXTERNAL_COLOUR_GREEN;
    uint8_t expected_first[1024], expected_second[1024];
    GameDisplay_Render(expected_first, &first);
    GameDisplay_Render(expected_second, &second);
    External_SetDisplay(&first);
    unsigned before = fake_transmits;
    poll(1u); /* Address then exactly 32 data calls, still one call per poll. */
    for (uint32_t now = 2u; now <= 11u; ++now) poll(now);
    assert(oled_sending && oled_offset == 320u);
    External_SetDisplay(&second);
    for (uint32_t now = 12u; now <= 33u; ++now) poll(now);
    assert(fake_transmits - before == 33u && !oled_sending && oled_dirty);
    assert(memcmp(fake_ram, expected_first, sizeof fake_ram) == 0);
    assert(oled_frames_completed == 1u && oled_last_frame_ms == 33u);
    assert(oled_last_transfer_ms == 32u && oled_longest_transfer_ms == 32u);
    for (uint32_t now = 34u; !External_DisplayIdle(); ++now) poll(now);
    assert(fake_transmits - before == 66u);
    assert(memcmp(fake_ram, expected_second, sizeof fake_ram) == 0);
    assert(oled_frames_completed == 2u);
}

int main(void)
{
    test_busy_then_nack_recovery();
    test_retry_backoff_and_absent_boot();
    test_fast_frames_keep_transfer_snapshot_immutable();
    puts("OLED transport tests passed: Fast-mode, bounded 32-byte chunks, immutable frames, BUSY defer, partial NACK/full-frame recovery, backoff, terminal drain, absent module.");
}
