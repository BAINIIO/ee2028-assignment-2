/* Exercise board_io.c's actual queue and polling UART with scripted HAL
 * progress. Unrelated sensor/clock setup is omitted by the narrow test guard. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
typedef struct {
    uint32_t BaudRate, WordLength, StopBits, Parity, Mode, HwFlowCtl;
    uint32_t OverSampling, OneBitSampling;
} FakeUartInit;
typedef struct {
    void *Instance;
    FakeUartInit Init;
    struct { uint32_t AdvFeatureInit; } AdvancedInit;
    uint16_t TxXferCount;
} UART_HandleTypeDef;

#define GPIOB ((void *)(uintptr_t)0x48000400u)
#define USART1 ((void *)(uintptr_t)0x40013800u)
#define USART1_IRQn 37
#define GPIO_PIN_6 (1u << 6)
#define GPIO_PIN_7 (1u << 7)
#define GPIO_MODE_AF_PP 2u
#define GPIO_PULLUP 1u
#define GPIO_SPEED_FREQ_VERY_HIGH 3u
#define GPIO_AF7_USART1 7u
#define UART_WORDLENGTH_8B 8u
#define UART_STOPBITS_1 1u
#define UART_PARITY_NONE 0u
#define UART_MODE_TX_RX 3u
#define UART_HWCONTROL_NONE 0u
#define UART_OVERSAMPLING_16 16u
#define UART_ONE_BIT_SAMPLE_DISABLE 0u
#define UART_ADVFEATURE_NO_INIT 0u
#define UART_FLAG_TC 1u
#define RESET 0u

static bool fake_clock_enabled;
static bool fake_complete;
static uint32_t fake_irq_mask;
static unsigned fake_calls;
static unsigned fake_aborts;
static unsigned fake_deinits;
static unsigned fake_disabled_irqs;
static unsigned fake_cleared_irqs;
static uint64_t fake_elapsed_us;
static HAL_StatusTypeDef fake_status;
static uint16_t fake_partial;
static uint8_t fake_wire[16384];
static size_t fake_wire_length;

#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_USART1_CLK_ENABLE() ((void)(fake_clock_enabled = true))
#define __HAL_RCC_USART1_CLK_DISABLE() ((void)(fake_clock_enabled = false))
#define __HAL_UART_GET_FLAG(handle, flag) ((void)(handle), (void)(flag), fake_complete)

static uint32_t __get_PRIMASK(void);
static void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *pins);
static void HAL_GPIO_DeInit(void *port, uint32_t pins);
static void HAL_NVIC_DisableIRQ(int interrupt);
static void HAL_NVIC_ClearPendingIRQ(int interrupt);
static HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *handle);
static HAL_StatusTypeDef HAL_UART_DeInit(UART_HandleTypeDef *handle);
static HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *handle);
static HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *handle,
                                         const uint8_t *bytes, uint16_t length,
                                         uint32_t timeout);

#define BOARD_UART_HOST_TEST
#include "../Core/Src/board_io.c"

static uint32_t __get_PRIMASK(void) { return fake_irq_mask; }
static void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *pins)
{
    assert(port == GPIOB && pins->Pin == (GPIO_PIN_6 | GPIO_PIN_7));
    assert(pins->Mode == GPIO_MODE_AF_PP && pins->Alternate == GPIO_AF7_USART1);
}
static void HAL_GPIO_DeInit(void *port, uint32_t pins)
{
    assert(port == GPIOB && pins == (GPIO_PIN_6 | GPIO_PIN_7));
}
static void HAL_NVIC_DisableIRQ(int interrupt)
{
    assert(interrupt == USART1_IRQn); ++fake_disabled_irqs;
}
static void HAL_NVIC_ClearPendingIRQ(int interrupt)
{
    assert(interrupt == USART1_IRQn); ++fake_cleared_irqs;
}
static HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *handle)
{
    assert(handle->Instance == USART1 && handle->Init.BaudRate == 115200u);
    assert(handle->Init.WordLength == UART_WORDLENGTH_8B);
    assert(handle->Init.StopBits == UART_STOPBITS_1 && handle->Init.Parity == UART_PARITY_NONE);
    assert(handle->Init.HwFlowCtl == UART_HWCONTROL_NONE);
    fake_complete = true;
    return HAL_OK;
}
static HAL_StatusTypeDef HAL_UART_DeInit(UART_HandleTypeDef *handle)
{
    assert(handle == &console_uart); ++fake_deinits; return HAL_OK;
}
static HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *handle)
{
    assert(handle == &console_uart && fake_irq_mask == 0u);
    handle->TxXferCount = 0;
    ++fake_aborts;
    return HAL_OK;
}
static HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *handle,
                                         const uint8_t *bytes, uint16_t length,
                                         uint32_t timeout)
{
    assert(handle == &console_uart && fake_clock_enabled);
    assert(fake_irq_mask == 0u); /* A ticking SysTick must enforce HAL's timeout. */
    assert(length > 0u && length <= 16u && timeout == 2u);
    ++fake_calls;
    HAL_StatusTypeDef status = fake_status;
    fake_status = HAL_OK;
    uint16_t accepted = status == HAL_OK ? length :
        status == HAL_TIMEOUT ? fake_partial : 0u;
    assert(accepted <= length && fake_wire_length + accepted <= sizeof fake_wire);
    memcpy(fake_wire + fake_wire_length, bytes, accepted);
    fake_wire_length += accepted;
    if (status == HAL_OK || status == HAL_TIMEOUT) {
        handle->TxXferCount = (uint16_t)(length - accepted);
    }
    fake_complete = status == HAL_OK;
    fake_elapsed_us += status == HAL_OK
        ? ((uint64_t)length * 10000000u + 115199u) / 115200u : 3000u;
    return status;
}

static void reset_uart(void)
{
    fake_irq_mask = 0;
    fake_calls = fake_aborts = fake_deinits = 0;
    fake_disabled_irqs = fake_cleared_irqs = 0;
    fake_elapsed_us = 0;
    fake_wire_length = 0;
    fake_status = HAL_OK;
    fake_partial = 0;
    assert(Uart_Init());
    assert(fake_disabled_irqs == 1u && fake_cleared_irqs == 1u);
    assert(Board_UartIdle());
}

static void drain(void)
{
    for (unsigned polls = 0; !Board_UartIdle(); ++polls) {
        assert(polls < 1024u);
        Board_UartPoll();
    }
}

static void test_chunks_and_wrap(void)
{
    reset_uart();
    char message[4091];
    memset(message, 'A', sizeof message - 1u);
    message[sizeof message - 1u] = '\0';
    assert(Board_UartWrite(message));
    assert(fake_calls == 1u && fake_wire_length == 16u);
    drain();
    assert(fake_wire_length == 4090u && memcmp(fake_wire, message, 4090u) == 0);
    const char *crossing = "0123456789012345678901234567890123456789";
    assert(Board_UartWrite(crossing));
    drain();
    assert(fake_wire_length == 4130u);
    assert(memcmp(fake_wire + 4090u, crossing, 40u) == 0);
    assert(uart_transmit_failures == 0u && uart_dropped_bytes == 0u);
}

static void test_full_queue_and_masked_interrupts(void)
{
    reset_uart();
    char full[4096], oversized[4097];
    memset(full, 'B', sizeof full - 1u); full[sizeof full - 1u] = '\0';
    memset(oversized, 'C', sizeof oversized - 1u); oversized[sizeof oversized - 1u] = '\0';
    assert(!Board_UartWrite(NULL));
    assert(!Board_UartWrite(oversized));
    fake_irq_mask = 1;
    assert(Board_UartWrite(full));
    assert(!Board_UartWrite("extra"));
    assert(fake_calls == 0u);
    Board_UartPoll();
    assert(fake_calls == 0u);
    fake_irq_mask = 0;
    drain();
    assert(fake_wire_length == 4095u && memcmp(fake_wire, full, 4095u) == 0);
    assert(fake_elapsed_us < 356000u);
}

static void test_partial_timeout_does_not_replay_prefix(void)
{
    reset_uart();
    const char *message = "abcdefghijklmnopqrst";
    fake_status = HAL_TIMEOUT;
    fake_partial = 6;
    assert(Board_UartWrite(message));
    assert(fake_wire_length == 6u && uart_tail == 6u);
    drain();
    assert(fake_wire_length == strlen(message));
    assert(memcmp(fake_wire, message, strlen(message)) == 0);
    assert(uart_transmit_failures == 1u && uart_dropped_bytes == 0u);
}

static void test_final_complete_timeout_does_not_replay_bytes(void)
{
    reset_uart();
    fake_status = HAL_TIMEOUT;
    fake_partial = 10;
    assert(Board_UartWrite("0123456789"));
    assert(uart_head == uart_tail && !Board_UartIdle());
    Board_UartPoll();
    assert(fake_calls == 1u && fake_wire_length == 10u);
    fake_complete = true;
    assert(Board_UartIdle());
}

static void test_stuck_uart_has_bounded_loss_and_recovers(void)
{
    reset_uart();
    const char *message = "abcdefghijklmnopqrst";
    fake_status = HAL_BUSY;
    assert(Board_UartWrite(message));
    assert(uart_tail == 0u && fake_aborts == 1u);
    fake_status = HAL_ERROR;
    Board_UartPoll();
    assert(uart_tail == 0u && fake_aborts == 2u);
    fake_status = HAL_TIMEOUT;
    fake_partial = 0;
    Board_UartPoll();
    assert(uart_tail == 16u && uart_dropped_bytes == 16u);
    drain();
    assert(fake_wire_length == 4u && memcmp(fake_wire, message + 16, 4u) == 0);
    assert(uart_transmit_failures == 3u);
}

static void test_busy_is_retried_without_losing_data(void)
{
    reset_uart();
    fake_status = HAL_BUSY;
    assert(Board_UartWrite("retry me"));
    assert(fake_wire_length == 0u && uart_tail == 0u);
    drain();
    assert(fake_wire_length == 8u && memcmp(fake_wire, "retry me", 8u) == 0);
    assert(uart_dropped_bytes == 0u);
    Board_UartStop();
    assert(Board_UartIdle() && !Board_UartWrite("stopped"));
    assert(fake_deinits == 1u && !fake_clock_enabled);
    Board_UartStop();
    assert(fake_deinits == 1u);
}

int main(void)
{
    test_chunks_and_wrap();
    test_full_queue_and_masked_interrupts();
    test_partial_timeout_does_not_replay_prefix();
    test_final_complete_timeout_does_not_replay_bytes();
    test_stuck_uart_has_bounded_loss_and_recovers();
    test_busy_is_retried_without_losing_data();
    puts("board UART bounded polling tests passed");
    return 0;
}
