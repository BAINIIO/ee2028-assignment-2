/* Exercise the actual NFC state machine against an EEPROM whose I2C address
 * NACKs for 5 ms per touched 4-byte page, as specified by ST DS10925. */
#include "../Core/Src/nfc_io.c"
#include <assert.h>

I2C_HandleTypeDef hI2cHandler;
uint32_t fake_nfc_bus_busy;
static uint32_t fake_tick, fake_ready_at;
static unsigned fake_calls, fake_writes, fake_eeprom_writes;
static unsigned fake_busy_nacks, fake_commits;
static bool fake_session, fake_irq_enabled, fake_fail_write;
static bool fake_fail_all;
static uint8_t fake_memory[512], fake_gpo, fake_static_rf, fake_dynamic_rf;
static uint8_t expected[256];
static size_t expected_size;

uint32_t HAL_GetTick(void) { return fake_tick; }
void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *gpio)
{
    assert(port == GPIOE && gpio->Pin == GPIO_PIN_4);
    assert(gpio->Mode == GPIO_MODE_IT_FALLING && gpio->Pull == GPIO_PULLUP);
}
void HAL_GPIO_EXTI_IRQHandler(uint16_t pin) { assert(pin == GPIO_PIN_4); }
void HAL_NVIC_SetPriority(int irq, unsigned priority, unsigned sub)
{ assert(irq == EXTI4_IRQn && priority == 6u && sub == 0u); }
void HAL_NVIC_EnableIRQ(int irq)
{ assert(irq == EXTI4_IRQn); fake_irq_enabled = true; }
void HAL_NVIC_DisableIRQ(int irq)
{ assert(irq == EXTI4_IRQn); fake_irq_enabled = false; }

static bool accept_call(I2C_HandleTypeDef *handle, uint16_t address_size,
                        uint32_t timeout)
{
    assert(handle == &hI2cHandler && address_size == I2C_MEMADD_SIZE_16BIT);
    assert(timeout == 2u && fake_nfc_bus_busy == 0u);
    ++fake_calls;
    if (fake_fail_all) return false;
    if ((int32_t)(fake_tick - fake_ready_at) < 0) {
        ++fake_busy_nacks;
        return false;
    }
    return true;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *handle,
    uint16_t device, uint16_t address, uint16_t address_size,
    uint8_t *data, uint16_t size, uint32_t timeout)
{
    if (!accept_call(handle, address_size, timeout)) return HAL_ERROR;
    if (device == 0xa6u && address < sizeof fake_memory) {
        assert((size_t)address + size <= sizeof fake_memory);
        memcpy(data, fake_memory + address, size);
    } else {
        assert(size == 1u);
        if (device == 0xaeu && address == 0u) *data = fake_gpo;
        else if (device == 0xaeu && address == 3u) *data = fake_static_rf;
        else if (device == 0xa6u && address == 0x2004u)
            *data = fake_session ? 1u : 0u;
        else if (device == 0xa6u && address == 0x2003u)
            *data = fake_dynamic_rf;
        else { assert(device == 0xa6u &&
                       (address == 0x2002u || address == 0x2005u));
               *data = 0u; }
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *handle,
    uint16_t device, uint16_t address, uint16_t address_size,
    uint8_t *data, uint16_t size, uint32_t timeout)
{
    ++fake_writes;
    if (!accept_call(handle, address_size, timeout)) return HAL_ERROR;
    if (device == 0xaeu && address == 0x0900u) {
        assert(size == 17u && data[8] == 0x09u);
        for (unsigned i = 0; i < 17u; ++i)
            assert(data[i] == (i == 8u ? 0x09u : 0u));
        fake_session = true;
    } else if (device == 0xaeu) {
        assert(fake_session && size == 1u);
        if (address == 0u) fake_gpo = *data;
        else { assert(address == 3u); fake_static_rf = *data;
               fake_dynamic_rf = *data; }
        fake_ready_at = fake_tick + 5u;
    } else if (device == 0xa6u && address == 0x2003u) {
        assert(size == 1u);
        fake_dynamic_rf = *data;
    } else {
        assert(device == 0xa6u && (size_t)address + size <= sizeof fake_memory);
        assert(fake_dynamic_rf == 2u); /* Hide an incomplete record from RF. */
        if (fake_fail_write && address == 8u) {
            fake_fail_write = false;
            return HAL_ERROR;
        }
        /* A TLV length can become nonzero only after all other desired bytes
         * are in EEPROM. The final one-byte commit must be separate. */
        if (address == 5u && size == 1u && data[0] != 0u) {
            assert(expected_size > 6u && fake_memory[5] == 0u);
            for (size_t i = 0; i < expected_size; ++i)
                if (i != 5u) assert(fake_memory[i] == expected[i]);
            ++fake_commits;
        }
        memcpy(fake_memory + address, data, size);
        ++fake_eeprom_writes;
        unsigned pages = ((unsigned)address % 4u + size + 3u) / 4u;
        fake_ready_at = fake_tick + 5u * pages;
    }
    return HAL_OK;
}

static void reset_fake(const char *url)
{
    fake_tick = fake_ready_at = fake_nfc_bus_busy = 0u;
    fake_calls = fake_writes = fake_eeprom_writes = 0u;
    fake_busy_nacks = fake_commits = 0u;
    fake_session = fake_irq_enabled = fake_fail_write = fake_fail_all = false;
    memset(fake_memory, 0xa5, sizeof fake_memory);
    fake_gpo = 0x88u;
    fake_static_rf = fake_dynamic_rf = 0u;
    expected_size = 0u;
    if (url && url[0]) {
        char mission_url[210];
        int length = snprintf(mission_url, sizeof mission_url,
                              "%s?mission=0", url);
        assert(length > 0 && (size_t)length < sizeof mission_url);
        expected_size = NfcNdef_BuildUri(mission_url, expected, sizeof expected);
    }
    Nfc_Init(url, fake_tick);
}

static void poll_next(bool allow_update)
{
    unsigned previous = fake_calls;
    ++fake_tick;
    Nfc_Poll(fake_tick, allow_update);
    assert(fake_calls - previous <= 1u); /* One bounded transaction per poll. */
}

static void run_until_ready(uint32_t limit)
{
    while (!Nfc_Ready() && fake_tick < limit) poll_next(true);
    assert(Nfc_Ready() && fake_irq_enabled);
    assert(memcmp(fake_memory, expected, expected_size) == 0);
    assert(fake_static_rf == 2u && fake_dynamic_rf == 0u);
    assert(fake_commits == 1u && fake_busy_nacks == 0u);
}

static void test_pages_commit_and_readback(void)
{
    reset_fake("http://192.0.2.10:8080/drone/GG01");
    run_until_ready(1000u);
    assert(Nfc_Online() && Nfc_Errors() == 0u);
    assert(fake_eeprom_writes > 5u);
    /* Busy shared sensor bus defers work without a HAL call or lost state. */
    unsigned previous = fake_calls;
    fake_nfc_bus_busy = 1u;
    for (unsigned i = 0; i < 100u; ++i) poll_next(true);
    assert(fake_calls == previous && Nfc_Ready());
    fake_nfc_bus_busy = 0u;
    Nfc_BeginStop();
    assert(!fake_irq_enabled && !Nfc_Ready() && !Nfc_StopDone());
    for (unsigned i = 0; !Nfc_StopDone() && i < 10u; ++i) {
        ++fake_tick; Nfc_FinishStop(fake_tick);
    }
    assert(Nfc_StopDone() && Nfc_Quiet() && fake_dynamic_rf == 2u);
    previous = fake_calls;
    for (unsigned i = 0; i < 100u; ++i) poll_next(true);
    assert(fake_calls == previous);
}

static void test_retry_keeps_desired_image(void)
{
    reset_fake("http://192.0.2.20:8080/drone/GG01");
    fake_fail_write = true;
    run_until_ready(2500u);
    assert(!fake_fail_write && Nfc_Errors() == 1u);
    assert(fake_tick >= 1000u);
    /* An unchanged record is compared and reused on reset, without EEPROM
     * wear or another length commit. */
    unsigned previous = fake_eeprom_writes;
    Nfc_Init("http://192.0.2.20:8080/drone/GG01", fake_tick);
    while (!Nfc_Ready() && fake_tick < 3000u) poll_next(true);
    assert(Nfc_Ready() && fake_eeprom_writes == previous);
}

static void test_hosted_url_is_programmed_and_reused_after_reset(void)
{
    reset_fake(APP_NFC_DASHBOARD_URL);
    assert(strlen(APP_NFC_DASHBOARD_URL) == 56u);
    assert(expected_size == 70u); /* 66-byte URI, compressed HTTPS prefix. */
    run_until_ready(1000u);
    assert(Nfc_Errors() == 0u && fake_memory[5] == 63u);
    assert(fake_memory[8] == 59u && fake_memory[10] == 0x04u);
    unsigned previous = fake_eeprom_writes;
    Nfc_Init(APP_NFC_DASHBOARD_URL, fake_tick);
    run_until_ready(fake_tick + 1000u);
    assert(fake_eeprom_writes == previous); /* EEPROM survives MCU reset. */
    assert(Nfc_Errors() == 0u);
}

static void test_empty_url_is_read_only(void)
{
    const char *const invalid[] = {NULL, "", "not-a-url"};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        reset_fake(invalid[i]);
        uint8_t original[sizeof fake_memory];
        memcpy(original, fake_memory, sizeof original);
        for (unsigned j = 0; j < 100u; ++j) poll_next(true);
        assert(Nfc_Online() && !Nfc_Ready());
        assert(fake_writes == 0u && fake_static_rf == 0u && fake_dynamic_rf == 0u);
        assert(memcmp(original, fake_memory, sizeof original) == 0);
        Nfc_BeginStop();
        Nfc_FinishStop(++fake_tick);
        assert(Nfc_StopDone() && !Nfc_Quiet() && fake_writes == 0u);
    }
}

static void test_shutdown_failure_is_bounded(void)
{
    reset_fake("http://192.0.2.10:8080/drone/GG01");
    run_until_ready(1000u);
    fake_fail_all = true;
    unsigned previous = fake_calls;
    Nfc_BeginStop();
    for (unsigned i = 0; !Nfc_StopDone() && i < 20u; ++i) {
        ++fake_tick; Nfc_FinishStop(fake_tick);
    }
    assert(Nfc_StopDone() && !Nfc_Quiet());
    assert(fake_calls - previous == 3u && Nfc_Errors() == 3u);
}

int main(void)
{
    test_pages_commit_and_readback();
    test_retry_keeps_desired_image();
    test_hosted_url_is_programmed_and_reused_after_reset();
    test_empty_url_is_read_only();
    test_shutdown_failure_is_bounded();
    puts("NFC driver tests passed");
    return 0;
}
