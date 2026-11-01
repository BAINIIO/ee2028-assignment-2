/* ISM43362 SPI firmware transport. Wire commands follow ST's exact-board
 * es_wifi.c/es_wifi_io.c examples, but reset, DRDY waits and AT responses are
 * advanced by foreground polls instead of HAL_Delay or blocking AT calls.
 * UART1, button/LED/buzzer pins, and the sensor buses are not configured here. */
#include "network_io.h"
#ifndef NETWORK_HOST_TEST
#include "stm32l4xx_hal.h"
#endif
#include <string.h>

#define SPI_WORDS_PER_POLL 16u
#define SPI_TIMEOUT_MS 1u
#define RESET_HOLD_MS 10u
#define BOOT_WAIT_MS 500u
#define COMMAND_TIMEOUT_MS 3000u
#define JOIN_TIMEOUT_MS 30000u
#define CONNECT_TIMEOUT_MS 12000u
#define RETRY_MS 5000u
#define READ_INTERVAL_MS 200u
#define SOCKET_FRAGMENT_MAX 1024u
#define RESPONSE_CAPACITY 1536u
#define COMMAND_CAPACITY 128u
#define MISSION_LINE_MAX 24u

typedef enum {
    IO_IDLE = 0, IO_TX_GUARD, IO_TX, IO_PAYLOAD_READY,
    IO_RESPONSE_READY, IO_RX_GUARD, IO_RX, IO_DONE
} IoState;

enum { STAGE_BOOT = 0, STAGE_INFO, STAGE_SSID, STAGE_PASSWORD, STAGE_SECURITY,
       STAGE_JOIN, STAGE_SOCKET, STAGE_TCP, STAGE_PORT, STAGE_ADDRESS,
       STAGE_CONNECT, STAGE_SEND_TIMEOUT, STAGE_READ_LENGTH,
       STAGE_READ_TIMEOUT, STAGE_READY, STAGE_SEND, STAGE_RECEIVE };

typedef struct { uint16_t length; uint8_t bytes[NETWORK_PACKET_MAX]; } Packet;

static SPI_HandleTypeDef network_spi;
static const NetworkConfig *config;
static NetworkStatus status;
static bool hardware_ready;
static bool draining;
static bool reset_released;
static uint32_t state_due;
static uint8_t stage;
static Packet packets[NETWORK_QUEUE_CAPACITY];
static uint8_t packet_read, packet_write, packet_count;
static uint16_t packet_offset, fragment_length;
static uint32_t next_read;
static bool mission_pending;
static uint8_t mission_latest;
static char mission_line[MISSION_LINE_MAX];
static uint8_t mission_length;
static bool mission_overflow;

static IoState io_state;
static uint32_t io_due, io_started, io_timeout;
static volatile uint32_t ready_edges;
static uint32_t response_edge;
static bool response_went_low;
static _Alignas(4) uint8_t transmit[COMMAND_CAPACITY + SOCKET_FRAGMENT_MAX + 2u];
static uint16_t transmit_length, transmit_offset, header_length;
static _Alignas(4) uint8_t response[RESPONSE_CAPACITY + 1u];
static uint16_t response_length;

#ifndef NETWORK_CONFIG_EXTERNAL
__attribute__((weak)) const NetworkConfig *Network_GetConfig(void) { return NULL; }
#endif

static bool reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool module_ready(void)
{
    return HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_1) == GPIO_PIN_SET;
}

static void chip_select(bool selected)
{
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, selected ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static size_t text_length(const char *text, size_t limit)
{
    size_t length = 0;
    if (text == NULL) return limit + 1u;
    while (length <= limit && text[length] != '\0') ++length;
    return length;
}

static bool credential_valid(const char *text, size_t minimum, size_t maximum)
{
    size_t length = text_length(text, maximum);
    if (length < minimum || length > maximum) return false;
    for (size_t index = 0; index < length; ++index)
        if ((uint8_t)text[index] < 0x20u || (uint8_t)text[index] == 0x7fu) return false;
    return true;
}

static bool config_valid(const NetworkConfig *candidate)
{
    return candidate != NULL && credential_valid(candidate->ssid, 1u, 32u) &&
        credential_valid(candidate->password, 8u, 63u) && candidate->server_port != 0u &&
        candidate->server_ipv4[0] != 0u && candidate->server_ipv4[0] < 224u;
}

static void clear_input(void)
{
    mission_pending = false;
    mission_length = 0;
    mission_overflow = false;
}

static void mission_bytes(const uint8_t *bytes, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        uint8_t byte = bytes[index];
        if (byte == '\n') {
            size_t used = mission_length;
            if (used && mission_line[used - 1u] == '\r') --used;
            if (!mission_overflow && used == 9u &&
                memcmp(mission_line, "MISSION ", 8u) == 0 &&
                mission_line[8] >= '0' && mission_line[8] <= '2') {
                mission_latest = (uint8_t)(mission_line[8] - '0');
                mission_pending = true;
                ++status.received_missions;
            }
            mission_length = 0;
            mission_overflow = false;
        } else if (mission_length < sizeof mission_line) {
            mission_line[mission_length++] = (char)byte;
        } else {
            mission_overflow = true;
        }
    }
}

static bool hardware_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_SPI3_CLK_ENABLE();
    HAL_NVIC_DisableIRQ(EXTI1_IRQn);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_8, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_8;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_Init(GPIOE, &gpio);
    gpio.Pin = GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_IT_RISING;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &gpio);
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_1);
    HAL_NVIC_ClearPendingIRQ(EXTI1_IRQn);
    HAL_NVIC_SetPriority(EXTI1_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(EXTI1_IRQn);
    gpio.Pin = GPIO_PIN_10 | GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_MEDIUM;
    gpio.Alternate = GPIO_AF6_SPI3;
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = GPIO_PIN_11;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &gpio);
    network_spi.Instance = SPI3;
    network_spi.Init.Mode = SPI_MODE_MASTER;
    network_spi.Init.Direction = SPI_DIRECTION_2LINES;
    network_spi.Init.DataSize = SPI_DATASIZE_16BIT;
    network_spi.Init.CLKPolarity = SPI_POLARITY_LOW;
    network_spi.Init.CLKPhase = SPI_PHASE_1EDGE;
    network_spi.Init.NSS = SPI_NSS_SOFT;
    network_spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    network_spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    network_spi.Init.TIMode = SPI_TIMODE_DISABLE;
    network_spi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    network_spi.Init.CRCPolynomial = 7;
    network_spi.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
    network_spi.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    return HAL_SPI_Init(&network_spi) == HAL_OK;
}

/* DRDY events only; unlike HAL_GPIO_EXTI_Callback this never replaces the
 * application's independent PC13 button callback. No SPI work in the ISR. */
void EXTI1_IRQHandler(void)
{
    if (__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_1) != RESET) {
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_1);
        ++ready_edges;
    }
}

static void retry_later(uint32_t now, NetworkError error)
{
    /* Classify before clearing/resetting, retaining constants only. A fault
     * can interrupt a response, so never search beyond its bounded buffer. */
    size_t length = response_length;
    if (length > RESPONSE_CAPACITY) length = RESPONSE_CAPACITY;
    response[length] = '\0';
    status.failure_stage = stage;
    status.last_response_flags = 0;
    if (strstr((const char *)response, "\r\nOK\r\n> ") != NULL)
        status.last_response_flags |= NETWORK_RESPONSE_OK;
    if (strstr((const char *)response, "ERROR") != NULL)
        status.last_response_flags |= NETWORK_RESPONSE_ERROR;
    if (strstr((const char *)response, "[JOIN ] Failed") != NULL)
        status.last_response_flags |= NETWORK_RESPONSE_JOIN_FAILED;
    if (strstr((const char *)response, "Usage") != NULL)
        status.last_response_flags |= NETWORK_RESPONSE_USAGE;
    if (hardware_ready) {
        chip_select(false);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_8, GPIO_PIN_RESET);
    }
    status.state = NETWORK_RETRY;
    status.last_error = error;
    ++status.retries;
    state_due = now + RETRY_MS;
    io_state = IO_IDLE;
    /* The failed TCP stream may contain a partial line. Its backlog belongs
     * to that old session: never replay it as apparently fresh telemetry on
     * a new connection. The reporter publishes current state on reconnection. */
    status.dropped += packet_count;
    packet_read = packet_write = packet_count = 0;
    packet_offset = 0;
    clear_input();
    memset(transmit, 0, sizeof transmit);
    memset(response, 0, sizeof response);
}

static void reset_begin(uint32_t now)
{
    chip_select(false);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_8, GPIO_PIN_RESET);
    reset_released = false;
    state_due = now + RESET_HOLD_MS;
    status.state = NETWORK_RESETTING;
    stage = STAGE_BOOT;
    status.stage = stage;
    io_state = IO_IDLE;
    response_length = 0;
}

static void receive_begin(uint32_t now)
{
    response_length = 0;
    chip_select(true);
    io_due = now + 1u; /* Exceeds the module's 15 us CS setup requirement. */
    io_state = IO_RX_GUARD;
}

static bool command_begin(const char *command, const uint8_t *payload,
                          uint16_t payload_length, uint32_t now)
{
    size_t length = text_length(command, COMMAND_CAPACITY - 2u);
    if (length > COMMAND_CAPACITY - 2u || payload_length > SOCKET_FRAGMENT_MAX) return false;
    memcpy(transmit, command, length);
    /* ST's 16-bit SPI command framing pads odd commands with LF. */
    if (length & 1u) transmit[length++] = '\n';
    header_length = (uint16_t)length;
    if (payload_length) memcpy(transmit + length, payload, payload_length);
    length += payload_length;
    if (length & 1u) transmit[length++] = '\n';
    transmit_length = (uint16_t)length;
    transmit_offset = 0;
    response_edge = ready_edges;
    response_went_low = false;
    io_started = now;
    io_timeout = stage == STAGE_JOIN ? JOIN_TIMEOUT_MS :
                 stage == STAGE_CONNECT ? CONNECT_TIMEOUT_MS : COMMAND_TIMEOUT_MS;
    chip_select(true);
    io_due = now + 1u;
    io_state = IO_TX_GUARD;
    status.stage = stage;
    ++status.commands;
    return true;
}

static bool io_poll(uint32_t now)
{
    if (io_state == IO_IDLE || io_state == IO_DONE) return io_state == IO_DONE;
    if ((uint32_t)(now - io_started) >= io_timeout) {
        retry_later(now, NETWORK_ERROR_TIMEOUT);
        return false;
    }
    switch (io_state) {
    case IO_TX_GUARD:
        if (reached(now, io_due)) io_state = IO_TX;
        break;
    case IO_PAYLOAD_READY:
        if (reached(now, io_due) && module_ready()) {
            response_edge = ready_edges;
            response_went_low = false;
            io_state = IO_TX;
        }
        break;
    case IO_TX: {
        uint16_t boundary = transmit_offset < header_length ? header_length : transmit_length;
        uint16_t bytes = (uint16_t)(boundary - transmit_offset);
        if (bytes > SPI_WORDS_PER_POLL * 2u) bytes = SPI_WORDS_PER_POLL * 2u;
        if (HAL_SPI_Transmit(&network_spi, transmit + transmit_offset,
                             (uint16_t)(bytes / 2u), SPI_TIMEOUT_MS) != HAL_OK) {
            ++status.spi_errors;
            retry_later(now, NETWORK_ERROR_SPI);
            break;
        }
        transmit_offset = (uint16_t)(transmit_offset + bytes);
        if (transmit_offset == transmit_length) {
            chip_select(false);
            io_due = now + 1u; /* Also exceeds ST's 3 us inter-transfer gap. */
            io_state = IO_RESPONSE_READY;
        } else if (transmit_offset == header_length) {
            io_due = now + 1u; /* ST also applies CS setup time before the payload. */
            io_state = IO_PAYLOAD_READY;
        }
        break;
    }
    case IO_RESPONSE_READY:
        if (!module_ready()) response_went_low = true;
        if (reached(now, io_due) && module_ready() &&
            (ready_edges != response_edge || response_went_low)) receive_begin(now);
        break;
    case IO_RX_GUARD:
        if (reached(now, io_due)) io_state = IO_RX;
        break;
    case IO_RX: {
        if (!module_ready()) {
            chip_select(false);
            response[response_length] = '\0';
            io_state = IO_DONE;
            return true;
        }
        if (response_length + 2u > RESPONSE_CAPACITY) {
            retry_later(now, NETWORK_ERROR_OVERFLOW);
            break;
        }
        /* Read one16-bit word then recheck DRDY on the next poll, as required
         * by the SPI firmware. This never clocks an entire empty buffer. */
        if (HAL_SPI_Receive(&network_spi, response + response_length, 1u,
                            SPI_TIMEOUT_MS) != HAL_OK) {
            ++status.spi_errors;
            retry_later(now, NETWORK_ERROR_SPI);
            break;
        }
        response_length = (uint16_t)(response_length + 2u);
        break;
    }
    default: break;
    }
    return false;
}

static bool response_ok(void)
{
    while (response_length && response[response_length - 1u] == 0x15u) --response_length;
    response[response_length] = '\0';
    static const char success[] = "\r\nOK\r\n> ";
    return response_length >= sizeof success - 1u &&
        memcmp(response + response_length - (sizeof success - 1u), success, sizeof success - 1u) == 0 &&
        strstr((const char *)response, "\r\nERROR") == NULL &&
        strstr((const char *)response, "\r\n-1\r\n") == NULL;
}

static size_t append_decimal(char *text, size_t used, unsigned value)
{
    char digits[10];
    size_t count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (count) text[used++] = digits[--count];
    text[used] = '\0';
    return used;
}

static void stage_command(uint32_t now)
{
    char command[COMMAND_CAPACITY] = {0};
    switch (stage) {
    case STAGE_INFO: memcpy(command, "I?\r", 4u); break;
    case STAGE_SSID:
    case STAGE_PASSWORD: {
        const char *secret = stage == STAGE_SSID ? config->ssid : config->password;
        size_t length = strlen(secret);
        command[0] = 'C'; command[1] = stage == STAGE_SSID ? '1' : '2'; command[2] = '=';
        memcpy(command + 3u, secret, length);
        command[3u + length] = '\r';
        break;
    }
    case STAGE_SECURITY: memcpy(command, "C3=3\r", 6u); break;
    case STAGE_JOIN: memcpy(command, "C0\r", 4u); break;
    case STAGE_SOCKET: memcpy(command, "P0=0\r", 6u); break;
    case STAGE_TCP: memcpy(command, "P1=0\r", 6u); break;
    case STAGE_PORT: {
        memcpy(command, "P4=", 3u);
        size_t used = append_decimal(command, 3u, config->server_port);
        command[used] = '\r';
        break;
    }
    case STAGE_ADDRESS: {
        memcpy(command, "P3=", 3u);
        size_t used = 3u;
        for (unsigned index = 0; index < 4u; ++index) {
            used = append_decimal(command, used, config->server_ipv4[index]);
            command[used++] = index == 3u ? '\r' : '.';
        }
        break;
    }
    case STAGE_CONNECT: memcpy(command, "P6=1\r", 6u); break;
    case STAGE_SEND_TIMEOUT: memcpy(command, "S2=1000\r", 9u); break;
    case STAGE_READ_LENGTH: memcpy(command, "R1=128\r", 8u); break;
    case STAGE_READ_TIMEOUT: memcpy(command, "R2=1\r", 6u); break;
    case STAGE_RECEIVE: memcpy(command, "R0\r", 4u); break;
    case STAGE_SEND: {
        uint16_t remaining = (uint16_t)(packets[packet_read].length - packet_offset);
        fragment_length = remaining > SOCKET_FRAGMENT_MAX ? SOCKET_FRAGMENT_MAX : remaining;
        memcpy(command, "S3=0000\r", 9u);
        unsigned value = fragment_length;
        for (unsigned index = 7u; index > 3u; --index) {
            command[index - 1u] = (char)('0' + value % 10u);
            value /= 10u;
        }
        (void)command_begin(command, packets[packet_read].bytes + packet_offset,
                            fragment_length, now);
        memset(command, 0, sizeof command);
        return;
    }
    default: return;
    }
    (void)command_begin(command, NULL, 0, now);
    memset(command, 0, sizeof command);
}

static void complete_response(uint32_t now)
{
    io_state = IO_IDLE;
    if (stage == STAGE_BOOT) {
        if (response_length < 4u || strstr((const char *)response, "\r\n> ") == NULL) {
            ++status.at_errors;
            retry_later(now, NETWORK_ERROR_RESPONSE);
            return;
        }
        status.state = NETWORK_JOINING;
        stage = STAGE_INFO;
        return;
    }
    if (!response_ok()) {
        ++status.at_errors;
        retry_later(now, NETWORK_ERROR_RESPONSE);
        return;
    }
    if (stage == STAGE_SEND) {
        packet_offset = (uint16_t)(packet_offset + fragment_length);
        status.sent_bytes += fragment_length;
        if (packet_offset == packets[packet_read].length) {
            packet_read = (uint8_t)((packet_read + 1u) % NETWORK_QUEUE_CAPACITY);
            --packet_count;
            packet_offset = 0;
            ++status.sent_packets;
        }
        stage = STAGE_READY;
    } else if (stage == STAGE_RECEIVE) {
        const size_t trailer_length = sizeof "\r\nOK\r\n> " - 1u;
        if (!draining && response_length >= trailer_length + 2u && response[0] == '\r' && response[1] == '\n')
            mission_bytes(response + 2u, response_length - trailer_length - 2u);
        stage = STAGE_READY;
    } else {
        ++stage;
        if (stage == STAGE_SOCKET) status.state = NETWORK_CONNECTING;
        if (stage == STAGE_READY) {
            status.state = NETWORK_ONLINE;
            status.last_error = NETWORK_ERROR_NONE;
            next_read = now;
        }
    }
    status.stage = stage;
    memset(response, 0, sizeof response);
    memset(transmit, 0, sizeof transmit);
}

void Network_Init(uint32_t now_ms)
{
    if (hardware_ready) Network_Stop();
    memset(&status, 0, sizeof status);
    packet_read = packet_write = packet_count = 0;
    packet_offset = 0;
    draining = false;
    ready_edges = 0;
    io_state = IO_IDLE;
    stage = STAGE_BOOT;
    clear_input();
    config = Network_GetConfig();
    if (config == NULL) return;
    if (!config_valid(config)) {
        status.last_error = NETWORK_ERROR_CONFIG;
        return;
    }
    hardware_ready = true; /* Init configures reset/CS even if SPI fails. */
    if (!hardware_init()) {
        ++status.spi_errors;
        retry_later(now_ms, NETWORK_ERROR_SPI);
        return;
    }
    reset_begin(now_ms);
}

void Network_Poll(uint32_t now_ms)
{
    if (status.state == NETWORK_DISABLED || status.state == NETWORK_STOPPED ||
        __get_PRIMASK() != 0u) return;
    if (draining && status.state != NETWORK_ONLINE) return;
    if (status.state == NETWORK_RETRY) {
        if (!reached(now_ms, state_due)) return;
        (void)HAL_SPI_DeInit(&network_spi);
        memset(&network_spi, 0, sizeof network_spi);
        if (!hardware_init()) {
            ++status.spi_errors;
            retry_later(now_ms, NETWORK_ERROR_SPI);
        } else reset_begin(now_ms);
        return;
    }
    if (status.state == NETWORK_RESETTING) {
        if (!reached(now_ms, state_due)) return;
        if (!reset_released) {
            HAL_GPIO_WritePin(GPIOE, GPIO_PIN_8, GPIO_PIN_SET);
            reset_released = true;
            state_due = now_ms + BOOT_WAIT_MS;
            return;
        }
        if (io_state == IO_IDLE) {
            io_started = now_ms;
            io_timeout = COMMAND_TIMEOUT_MS;
            if (module_ready()) receive_begin(now_ms);
            else if ((uint32_t)(now_ms - state_due) >= COMMAND_TIMEOUT_MS)
                retry_later(now_ms, NETWORK_ERROR_TIMEOUT);
            return;
        }
    }
    if (io_state != IO_IDLE) {
        if (io_poll(now_ms)) complete_response(now_ms);
        return;
    }
    if (stage == STAGE_READY) {
        if (!draining && reached(now_ms, next_read)) {
            stage = STAGE_RECEIVE;
            next_read = now_ms + READ_INTERVAL_MS;
        } else if (packet_count) stage = STAGE_SEND;
        else return;
    }
    if (!module_ready()) {
        /* A ready idle module advertises a high DRDY. Never spin on it. */
        if ((uint32_t)(now_ms - io_started) >= COMMAND_TIMEOUT_MS)
            retry_later(now_ms, NETWORK_ERROR_TIMEOUT);
        return;
    }
    stage_command(now_ms);
}

bool Network_Queue(const char *line, size_t length)
{
    if (line == NULL || length == 0u || length > NETWORK_PACKET_MAX ||
        line[length - 1u] != '\n' || packet_count == NETWORK_QUEUE_CAPACITY ||
        status.state != NETWORK_ONLINE || draining) {
        ++status.dropped;
        return false;
    }
    memcpy(packets[packet_write].bytes, line, length);
    packets[packet_write].length = (uint16_t)length;
    packet_write = (uint8_t)((packet_write + 1u) % NETWORK_QUEUE_CAPACITY);
    ++packet_count;
    return true;
}

bool Network_QueueFinal(const char *line, size_t length)
{
    if (line == NULL || length == 0u || length > NETWORK_PACKET_MAX ||
        line[length - 1u] != '\n' || !Network_Connected()) {
        ++status.dropped;
        Network_BeginDrain();
        return false;
    }
    /* A TCP line whose first fragment was already submitted must finish
     * before the final line, otherwise the server would see malformed JSON.
     * The current S3 transaction also owns its queue entry until its reply. */
    bool preserve = packet_count != 0u && (packet_offset != 0u ||
        (stage == STAGE_SEND && io_state != IO_IDLE));
    uint8_t kept = preserve ? 1u : 0u;
    status.dropped += (uint32_t)(packet_count - kept);
    packet_count = kept;
    packet_write = (uint8_t)((packet_read + kept) % NETWORK_QUEUE_CAPACITY);
    if (!preserve) packet_offset = 0;
    memcpy(packets[packet_write].bytes, line, length);
    packets[packet_write].length = (uint16_t)length;
    packet_write = (uint8_t)((packet_write + 1u) % NETWORK_QUEUE_CAPACITY);
    ++packet_count;
    Network_BeginDrain();
    return true;
}

bool Network_Connected(void) { return status.state == NETWORK_ONLINE; }
uint32_t Network_Dropped(void) { return status.dropped; }
NetworkStatus Network_GetStatus(void)
{
    NetworkStatus snapshot = status;
    snapshot.queued = packet_count;
    snapshot.stage = stage;
    return snapshot;
}

bool Network_ReadMission(uint8_t *mission_id)
{
    if (!mission_pending || mission_id == NULL) return false;
    *mission_id = mission_latest;
    mission_pending = false;
    return true;
}

void Network_BeginDrain(void)
{
    draining = true;
    clear_input();
    if (stage == STAGE_RECEIVE && io_state == IO_IDLE) stage = STAGE_READY;
}

bool Network_Idle(void)
{
    return packet_count == 0u && packet_offset == 0u && io_state == IO_IDLE;
}

void Network_Stop(void)
{
    if (hardware_ready) {
        chip_select(false);
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_8, GPIO_PIN_RESET);
        HAL_NVIC_DisableIRQ(EXTI1_IRQn);
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_1);
        HAL_NVIC_ClearPendingIRQ(EXTI1_IRQn);
        (void)HAL_SPI_DeInit(&network_spi);
        HAL_GPIO_DeInit(GPIOC, GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12);
        HAL_GPIO_DeInit(GPIOE, GPIO_PIN_1);
        __HAL_RCC_SPI3_CLK_DISABLE();
        hardware_ready = false;
    }
    status.dropped += packet_count;
    packet_read = packet_write = packet_count = 0;
    packet_offset = 0;
    clear_input();
    io_state = IO_IDLE;
    memset(transmit, 0, sizeof transmit);
    memset(response, 0, sizeof response);
    status.state = NETWORK_STOPPED;
}
