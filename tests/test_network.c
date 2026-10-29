/* Script a real ISM43362-style module around the actual async transport.
 * These are host-only fake credentials; no board or network is contacted. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "network_io.h"

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
typedef struct {
    uint32_t Mode, Direction, DataSize, CLKPolarity, CLKPhase, NSS;
    uint32_t BaudRatePrescaler, FirstBit, TIMode, CRCCalculation;
    uint32_t CRCPolynomial, CRCLength, NSSPMode;
} FakeSpiInit;
typedef struct { void *Instance; FakeSpiInit Init; } SPI_HandleTypeDef;
#define GPIOC ((void *)(uintptr_t)0x48000800u)
#define GPIOE ((void *)(uintptr_t)0x48001000u)
#define SPI3 ((void *)(uintptr_t)0x40003c00u)
#define GPIO_PIN_0 1u
#define GPIO_PIN_1 2u
#define GPIO_PIN_8 256u
#define GPIO_PIN_10 1024u
#define GPIO_PIN_11 2048u
#define GPIO_PIN_12 4096u
#define GPIO_PIN_RESET 0u
#define GPIO_PIN_SET 1u
#define GPIO_MODE_OUTPUT_PP 1u
#define GPIO_MODE_IT_RISING 2u
#define GPIO_MODE_AF_PP 3u
#define GPIO_NOPULL 0u
#define GPIO_PULLUP 1u
#define GPIO_SPEED_FREQ_LOW 0u
#define GPIO_SPEED_FREQ_MEDIUM 1u
#define GPIO_AF6_SPI3 6u
#define SPI_MODE_MASTER 1u
#define SPI_DIRECTION_2LINES 2u
#define SPI_DATASIZE_16BIT 16u
#define SPI_POLARITY_LOW 0u
#define SPI_PHASE_1EDGE 0u
#define SPI_NSS_SOFT 1u
#define SPI_BAUDRATEPRESCALER_8 8u
#define SPI_FIRSTBIT_MSB 0u
#define SPI_TIMODE_DISABLE 0u
#define SPI_CRCCALCULATION_DISABLE 0u
#define SPI_CRC_LENGTH_DATASIZE 0u
#define SPI_NSS_PULSE_DISABLE 0u
#define EXTI1_IRQn 7
#define RESET 0u
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOE_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SPI3_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SPI3_CLK_DISABLE() ((void)0)

static bool fake_exti_pending, fake_drdy, fake_selected, fake_rx_mode;
static bool fake_reset_high, fake_response_pending, fake_suppress_join;
static uint32_t fake_tick, fake_due, fake_irq_mask;
static unsigned fake_spi_calls, fake_command_count, fake_reset_count;
static HAL_StatusTypeDef fake_spi_status;
static uint8_t fake_tx[1200], fake_rx[1536], fake_wire[16384];
static size_t fake_tx_length, fake_rx_length, fake_rx_offset, fake_wire_length;
static char fake_last_command[128];
static const char *fake_downlink;
static const NetworkConfig fake_valid_config = {
    "host-test-network", "host-test-password", {192, 0, 2, 10}, 8081u
};
static const NetworkConfig *fake_config;

#define __HAL_GPIO_EXTI_GET_IT(pin) ((void)(pin), fake_exti_pending)
#define __HAL_GPIO_EXTI_CLEAR_IT(pin) ((void)(pin), (void)(fake_exti_pending = false))
static uint32_t __get_PRIMASK(void);
static unsigned HAL_GPIO_ReadPin(void *port, uint32_t pin);
static void HAL_GPIO_WritePin(void *port, uint32_t pin, unsigned level);
static void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *gpio);
static void HAL_GPIO_DeInit(void *port, uint32_t pins);
static void HAL_NVIC_DisableIRQ(int irq);
static void HAL_NVIC_ClearPendingIRQ(int irq);
static void HAL_NVIC_SetPriority(int irq, unsigned priority, unsigned sub);
static void HAL_NVIC_EnableIRQ(int irq);
static HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *handle);
static HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *handle);
static HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *handle, uint8_t *data,
                                        uint16_t words, uint32_t timeout);
static HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *handle, uint8_t *data,
                                       uint16_t words, uint32_t timeout);
#define NETWORK_HOST_TEST
#define NETWORK_CONFIG_EXTERNAL
#include "../Core/Src/network_io.c"

const NetworkConfig *Network_GetConfig(void) { return fake_config; }
static uint32_t __get_PRIMASK(void) { return fake_irq_mask; }
static unsigned HAL_GPIO_ReadPin(void *port, uint32_t pin)
{
    assert(port == GPIOE && pin == GPIO_PIN_1);
    return fake_drdy ? GPIO_PIN_SET : GPIO_PIN_RESET;
}
static void load_response(const uint8_t *bytes, size_t length)
{
    assert(length + 1u < sizeof fake_rx);
    memcpy(fake_rx, bytes, length);
    if (length & 1u) fake_rx[length++] = 0x15u;
    fake_rx_length = length;
    fake_rx_offset = 0;
    fake_response_pending = true;
    fake_drdy = false;
    fake_due = fake_tick + 2u;
}
static void process_command(void)
{
    const uint8_t *end = memchr(fake_tx, '\r', fake_tx_length);
    assert(end != NULL);
    size_t length = (size_t)(end - fake_tx);
    assert(length < sizeof fake_last_command);
    memcpy(fake_last_command, fake_tx, length);
    fake_last_command[length] = '\0';
    ++fake_command_count;
    static const char *const setup_commands[] = {
        "I?", "C1=host-test-network", "C2=host-test-password", "C3=3", "C0",
        "P0=0", "P1=0", "P4=8081", "P3=192.0.2.10", "P6=1",
        "S2=1000", "R1=128", "R2=1"
    };
    if (fake_command_count <= sizeof setup_commands / sizeof setup_commands[0])
        assert(strcmp(fake_last_command, setup_commands[fake_command_count - 1u]) == 0);
    if (strncmp(fake_last_command, "S3=", 3u) == 0) {
        unsigned payload = 0;
        assert(length == 7u);
        for (size_t index = 3u; index < length; ++index)
            payload = payload * 10u + (unsigned)(fake_last_command[index] - '0');
        assert(payload <= 1024u && fake_tx_length >= 8u + payload);
        assert(fake_wire_length + payload <= sizeof fake_wire);
        memcpy(fake_wire + fake_wire_length, fake_tx + 8u, payload);
        fake_wire_length += payload;
    }
    if (fake_suppress_join && strcmp(fake_last_command, "C0") == 0) {
        fake_drdy = false;
        fake_due = UINT32_MAX;
        fake_response_pending = false;
        return;
    }
    if (strcmp(fake_last_command, "R0") == 0) {
        char reply[512] = "\r\n";
        if (fake_downlink) {
            assert(strlen(fake_downlink) < 400u);
            strcat(reply, fake_downlink);
            fake_downlink = NULL;
        }
        strcat(reply, "\r\nOK\r\n> ");
        load_response((const uint8_t *)reply, strlen(reply));
    } else {
        static const uint8_t ok[] = "\r\nOK\r\n> ";
        load_response(ok, sizeof ok - 1u);
    }
}
static void HAL_GPIO_WritePin(void *port, uint32_t pin, unsigned level)
{
    assert(port == GPIOE);
    if (pin == GPIO_PIN_8) {
        if (level == GPIO_PIN_RESET) {
            fake_reset_high = false;
            fake_response_pending = false;
            fake_drdy = false;
            fake_due = UINT32_MAX;
            fake_tx_length = 0;
            ++fake_reset_count;
        } else if (!fake_reset_high) {
            fake_reset_high = true;
            static const uint8_t prompt[] = {0x15u, 0x15u, '\r', '\n', '>', ' '};
            load_response(prompt, sizeof prompt);
            fake_due = fake_tick + 500u;
        }
    } else {
        assert(pin == GPIO_PIN_0);
        bool selected = level == GPIO_PIN_RESET;
        if (selected && !fake_selected) {
            fake_rx_mode = fake_response_pending;
            if (!fake_rx_mode) fake_tx_length = 0;
        } else if (!selected && fake_selected) {
            if (fake_rx_mode) {
                assert(fake_rx_offset == fake_rx_length);
                fake_response_pending = false;
                fake_due = fake_tick + 1u;
            } else if (fake_tx_length) process_command();
        }
        fake_selected = selected;
    }
}
static void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *gpio)
{
    assert(port == GPIOC || port == GPIOE);
    if (port == GPIOC) assert((gpio->Pin & ~(GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12)) == 0u);
    else assert((gpio->Pin & ~(GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8)) == 0u);
}
static void HAL_GPIO_DeInit(void *port, uint32_t pins) { (void)port; (void)pins; }
static void HAL_NVIC_DisableIRQ(int irq) { assert(irq == EXTI1_IRQn); }
static void HAL_NVIC_ClearPendingIRQ(int irq) { assert(irq == EXTI1_IRQn); }
static void HAL_NVIC_SetPriority(int irq, unsigned priority, unsigned sub)
{
    assert(irq == EXTI1_IRQn && priority == 7u && sub == 0u);
}
static void HAL_NVIC_EnableIRQ(int irq) { assert(irq == EXTI1_IRQn); }
static HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *handle)
{
    assert(handle->Instance == SPI3 && handle->Init.DataSize == 16u);
    return HAL_OK;
}
static HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *handle) { (void)handle; return HAL_OK; }
static HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *handle, uint8_t *data,
                                        uint16_t words, uint32_t timeout)
{
    assert(handle == &network_spi && fake_irq_mask == 0u && timeout == 1u);
    assert(fake_selected && !fake_rx_mode && words > 0u && words <= 16u);
    ++fake_spi_calls;
    HAL_StatusTypeDef result = fake_spi_status;
    fake_spi_status = HAL_OK;
    if (result != HAL_OK) return result;
    assert(fake_tx_length + (size_t)words * 2u <= sizeof fake_tx);
    memcpy(fake_tx + fake_tx_length, data, (size_t)words * 2u);
    fake_tx_length += (size_t)words * 2u;
    return HAL_OK;
}
static HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *handle, uint8_t *data,
                                       uint16_t words, uint32_t timeout)
{
    assert(handle == &network_spi && fake_irq_mask == 0u && timeout == 1u);
    assert(fake_selected && fake_rx_mode && fake_drdy && words == 1u);
    ++fake_spi_calls;
    assert(fake_rx_offset + 2u <= fake_rx_length);
    memcpy(data, fake_rx + fake_rx_offset, 2u);
    fake_rx_offset += 2u;
    if (fake_rx_offset == fake_rx_length) fake_drdy = false;
    return HAL_OK;
}

static void poll_once(void)
{
    if (fake_due != UINT32_MAX && fake_tick >= fake_due && !fake_drdy &&
        (!fake_selected || fake_response_pending)) {
        fake_drdy = true;
        fake_due = UINT32_MAX;
        fake_exti_pending = true;
        EXTI1_IRQHandler();
    }
    unsigned before = fake_spi_calls;
    Network_Poll(fake_tick++);
    assert(fake_spi_calls - before <= 1u);
}
static void setup(void)
{
    if (hardware_ready) Network_Stop();
    fake_tick = fake_irq_mask = 0;
    fake_drdy = fake_selected = fake_rx_mode = fake_reset_high = false;
    fake_response_pending = fake_suppress_join = false;
    fake_exti_pending = false;
    fake_due = UINT32_MAX;
    fake_spi_calls = fake_command_count = fake_reset_count = 0;
    fake_tx_length = fake_rx_length = fake_rx_offset = fake_wire_length = 0;
    fake_spi_status = HAL_OK;
    fake_downlink = NULL;
    fake_config = &fake_valid_config;
    Network_Init(fake_tick);
}
static void connect_module(void)
{
    for (unsigned count = 0; !Network_Connected() && count < 2000u; ++count) poll_once();
    assert(Network_Connected());
    assert(Network_GetStatus().spi_errors == 0u && Network_GetStatus().at_errors == 0u);
}
static void test_join_queue_and_fragments(void)
{
    setup(); connect_module();
    char line[1501];
    memset(line, 'x', sizeof line); line[sizeof line - 1u] = '\n';
    assert(Network_Queue(line, sizeof line));
    for (unsigned count = 0; Network_GetStatus().sent_packets != 1u && count < 2000u; ++count) poll_once();
    NetworkStatus result = Network_GetStatus();
    assert(result.sent_packets == 1u && result.sent_bytes == sizeof line && result.queued == 0u);
    assert(fake_wire_length == sizeof line && memcmp(fake_wire, line, sizeof line) == 0);
    assert(result.commands >= 16u);
    Network_Stop();
}
static void test_mission_stream_is_strict_and_fragmented(void)
{
    setup(); connect_module();
    fake_downlink = "MISSION 3\nMISSION 1 extra\nxMISSION 2\nMISSION 2\r\n";
    uint8_t mission = 255;
    for (unsigned count = 0; !mission_pending && count < 500u; ++count) poll_once();
    assert(Network_ReadMission(&mission) && mission == 2u);
    assert(Network_GetStatus().received_missions == 1u);
    assert(!Network_ReadMission(&mission));
    fake_downlink = "MISS";
    for (unsigned count = 0; fake_downlink && count < 500u; ++count) poll_once();
    for (unsigned count = 0; stage != STAGE_READY && count < 100u; ++count) poll_once();
    assert(!Network_ReadMission(&mission));
    fake_downlink = "ION 0\n";
    for (unsigned count = 0; !mission_pending && count < 500u; ++count) poll_once();
    assert(Network_ReadMission(&mission) && mission == 0u);
    Network_Stop();
}
static void test_missing_hotspot_is_async_and_retry_is_bounded(void)
{
    setup(); fake_suppress_join = true;
    while (stage != STAGE_JOIN || io_state != IO_RESPONSE_READY) {
        assert(fake_tick < 2000u); poll_once();
    }
    uint32_t start = fake_tick;
    while (fake_tick - start < 29900u) {
        assert(!Network_Connected()); poll_once();
    }
    assert(Network_GetStatus().state == NETWORK_JOINING);
    while (Network_GetStatus().state != NETWORK_RETRY) {
        assert(fake_tick - start < 30100u); poll_once();
    }
    assert(Network_GetStatus().last_error == NETWORK_ERROR_TIMEOUT);
    assert(Network_GetStatus().retries == 1u);
    Network_Stop();
}
static void test_queue_capacity_masked_poll_and_stop(void)
{
    setup();
    fake_irq_mask = 1;
    for (unsigned index = 0; index < 1000u; ++index) poll_once();
    assert(fake_spi_calls == 0u);
    fake_irq_mask = 0;
    connect_module();
    for (unsigned index = 0; index < 8u; ++index) assert(Network_Queue("{}\n", 3u));
    assert(!Network_Queue("{}\n", 3u));
    assert(!Network_Queue("no newline", 10u));
    assert(Network_Dropped() == 2u);
    unsigned commands = fake_command_count;
    unsigned transfers = fake_spi_calls;
    Network_Stop();
    assert(fake_command_count == commands && fake_spi_calls == transfers);
    assert(!Network_Connected() && Network_GetStatus().state == NETWORK_STOPPED);
    for (unsigned index = 0; index < 1000u; ++index) poll_once();
    assert(fake_spi_calls == transfers);
    Network_Stop();
}
static void test_invalid_or_absent_config_does_not_touch_hardware(void)
{
    setup(); Network_Stop();
    unsigned resets = fake_reset_count;
    fake_config = NULL;
    Network_Init(0);
    assert(Network_GetStatus().state == NETWORK_DISABLED && fake_reset_count == resets);
    NetworkConfig invalid = fake_valid_config;
    invalid.ssid = "bad\rnetwork";
    fake_config = &invalid;
    Network_Init(0);
    assert(Network_GetStatus().last_error == NETWORK_ERROR_CONFIG);
    assert(fake_reset_count == resets);
    fake_config = &fake_valid_config;
}

static void test_final_drain_finishes_tx_and_never_reads_or_rejoins(void)
{
    setup(); connect_module();
    char final_packet[1501];
    memset(final_packet, 'f', sizeof final_packet);
    final_packet[sizeof final_packet - 1u] = '\n';
    assert(Network_Queue(final_packet, sizeof final_packet));
    Network_BeginDrain();
    assert(!Network_Idle() && !Network_Queue("{}\n", 3u));
    for (unsigned count = 0; !Network_Idle() && count < 1000u; ++count) {
        poll_once();
        assert(stage != STAGE_RECEIVE);
    }
    assert(Network_Idle() && Network_GetStatus().sent_packets == 1u);
    assert(fake_wire_length == sizeof final_packet);
    assert(memcmp(fake_wire, final_packet, sizeof final_packet) == 0);
    unsigned commands = fake_command_count;
    for (unsigned count = 0; count < 1000u; ++count) poll_once();
    assert(fake_command_count == commands);
    Network_Stop();

    setup();
    Network_BeginDrain();
    for (unsigned count = 0; count < 40000u; ++count) poll_once();
    assert(fake_command_count == 0u && !Network_Connected());
    Network_Stop();
}

static void test_drain_discards_inflight_mission_and_stops_on_fault(void)
{
    setup(); connect_module();
    fake_downlink = "MISSION 1\n";
    while (stage != STAGE_RECEIVE || io_state != IO_RX) poll_once();
    Network_BeginDrain();
    for (unsigned count = 0; !Network_Idle() && count < 100u; ++count) poll_once();
    assert(Network_Idle());
    uint8_t mission;
    assert(!Network_ReadMission(&mission));
    Network_Stop();

    setup(); connect_module();
    assert(Network_Queue("{}\n", 3u));
    Network_BeginDrain();
    fake_spi_status = HAL_TIMEOUT;
    while (Network_GetStatus().state != NETWORK_RETRY) poll_once();
    unsigned resets = fake_reset_count;
    unsigned commands = fake_command_count;
    for (unsigned count = 0; count < 40000u; ++count) poll_once();
    assert(fake_reset_count == resets && fake_command_count == commands);
    assert(Network_Idle()); /* The failed session's queue was dropped. */
    Network_Stop();
    assert(Network_Idle());
}

static void test_final_priority_discards_unsent_but_finishes_partial_line(void)
{
    setup(); connect_module();
    char first[1501];
    memset(first, 'p', sizeof first); first[sizeof first - 1u] = '\n';
    assert(Network_Queue(first, sizeof first));
    while (packet_offset == 0u) poll_once(); /* First1024-byte fragment acknowledged. */
    for (unsigned index = 0; index < 7u; ++index) assert(Network_Queue("old\n", 4u));
    assert(Network_GetStatus().queued == 8u);
    assert(Network_QueueFinal("final\n", 6u));
    assert(Network_GetStatus().queued == 2u && Network_Dropped() == 7u);
    while (!Network_Idle()) poll_once();
    assert(fake_wire_length == sizeof first + 6u);
    assert(memcmp(fake_wire, first, sizeof first) == 0);
    assert(memcmp(fake_wire + sizeof first, "final\n", 6u) == 0);
    Network_Stop();

    setup(); connect_module();
    for (unsigned index = 0; index < 8u; ++index) assert(Network_Queue("old\n", 4u));
    assert(Network_QueueFinal("only final\n", 11u));
    assert(Network_GetStatus().queued == 1u && Network_Dropped() == 8u);
    while (!Network_Idle()) poll_once();
    assert(fake_wire_length == 11u && memcmp(fake_wire, "only final\n", 11u) == 0);
    Network_Stop();
}

static void test_offline_packets_and_failed_session_are_not_replayed(void)
{
    setup();
    assert(!Network_Queue("old offline\n", 12u));
    assert(Network_GetStatus().queued == 0u && Network_Dropped() == 1u);
    connect_module();
    char partial[1501];
    memset(partial, 's', sizeof partial); partial[sizeof partial - 1u] = '\n';
    assert(Network_Queue(partial, sizeof partial));
    for (unsigned polls = 0; packet_offset == 0u; ++polls) {
        assert(polls < 2000u); poll_once();
    }
    assert(packet_offset == 1024u && fake_wire_length == 1024u);
    assert(Network_Queue("old queued\n", 11u));
    assert(Network_GetStatus().queued == 2u);
    fake_spi_status = HAL_TIMEOUT;
    for (unsigned polls = 0; Network_GetStatus().state != NETWORK_RETRY; ++polls) {
        assert(polls < 100u); poll_once();
    }
    assert(Network_GetStatus().queued == 0u && packet_offset == 0u);
    assert(Network_Dropped() == 3u);
    assert(!Network_Queue("offline\n", 8u));
    assert(Network_Dropped() == 4u);
    size_t previous_session_bytes = fake_wire_length;
    for (unsigned polls = 0; !Network_Connected(); ++polls) {
        assert(polls < 8000u); poll_once();
    }
    assert(Network_GetStatus().queued == 0u);
    assert(Network_Queue("fresh state\n", 12u));
    for (unsigned polls = 0; Network_GetStatus().sent_packets != 1u; ++polls) {
        assert(polls < 1000u); poll_once();
    }
    assert(fake_wire_length == previous_session_bytes + 12u);
    assert(memcmp(fake_wire + previous_session_bytes, "fresh state\n", 12u) == 0);
    Network_Stop();
}

int main(void)
{
    test_join_queue_and_fragments();
    test_mission_stream_is_strict_and_fragmented();
    test_missing_hotspot_is_async_and_retry_is_bounded();
    test_queue_capacity_masked_poll_and_stop();
    test_invalid_or_absent_config_does_not_touch_hardware();
    test_final_drain_finishes_tx_and_never_reads_or_rejoins();
    test_drain_discards_inflight_mission_and_stops_on_fault();
    test_final_priority_discards_unsent_but_finishes_partial_line();
    test_offline_packets_and_failed_session_are_not_replayed();
    puts("network async SPI/module tests passed");
    return 0;
}
