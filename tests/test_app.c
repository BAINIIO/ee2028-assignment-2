/* Exercise the actual scheduler and EXTI queue, with simulated peripherals.
 * This proves application integration behavior, not physical hardware timing. */
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define main firmware_main
#include "../Core/Src/main.c"
#undef main

#define TRACE_CAPACITY 256u
/* Fourteen-second rescue scenarios now record about 420 display frames. */
#define DISPLAY_TRACE_CAPACITY 1024u
#define UART_CAPACITY 4096u
#define WIRE_CAPACITY 65536u
#define EDGE_CAPACITY 32u
#define MOTION_TRACE_CAPACITY 512u

typedef struct {
    uint32_t at_ms;
    uint16_t pin;
} ScriptedEdge;

typedef struct {
    uint32_t at_ms;
    char text[1024];
} UartTrace;

typedef struct {
    bool flip;
    bool rescue;
    bool uart_jammed;
    bool optional_present;
    bool threshold_changes;
    bool boot_failure;
    size_t terminal_backlog;
    uint32_t stop_ms;
} Scenario;

enum { SIM_LIMIT = 1, SIM_HALTED = 2 };
static jmp_buf fake_escape;
static Scenario fake_scenario;
static uint32_t fake_tick;
static uint32_t fake_irq_mask;
static bool fake_tick_suspended;
static bool fake_button_irq_disabled;
static bool fake_uart_stopped;
static bool fake_external_stopped;
static bool fake_inside_sensor_read;
static unsigned fake_edges_during_reads;
static bool fake_led;
static unsigned fake_external_polls;
static unsigned fake_external_polls_at_terminal;
static unsigned fake_external_inits;
static unsigned fake_finish_display_calls;
static uint32_t fake_display_ready_ms;
static char fake_display_mode[24];
static ExternalDisplayFrame fake_display_frames[DISPLAY_TRACE_CAPACITY];
static size_t fake_display_count;
static unsigned fake_sound_count[EXTERNAL_SOUND_LOSE + 1u];
static uint32_t fake_sensor_reads[TRACE_CAPACITY];
static size_t fake_sensor_count;
static uint32_t fake_motion_reads[MOTION_TRACE_CAPACITY];
static size_t fake_motion_count;
static size_t fake_sensor_count_at_terminal;
static size_t fake_motion_count_at_terminal;
static ScriptedEdge fake_edges[EDGE_CAPACITY];
static size_t fake_edge_count;
static size_t fake_edge_next;
static UartTrace fake_trace[TRACE_CAPACITY];
static size_t fake_trace_count;
static char fake_uart_queue[UART_CAPACITY];
static size_t fake_uart_head;
static size_t fake_uart_tail;
static size_t fake_uart_bytes;
static uint32_t fake_uart_credit;
static char fake_wire[WIRE_CAPACITY];
static size_t fake_wire_length;
static unsigned fake_uart_idle_checks;
static unsigned fake_uart_idle_checks_at_terminal;
static uint32_t fake_first_attack_ms;
static uint32_t fake_terminal_ms;
static uint32_t fake_uart_stop_ms;
static uint32_t fake_rescue_first_ms;
static uint32_t fake_rescue_second_ms;
static bool fake_rescue_first_sent;
static bool fake_rescue_second_sent;
static bool fake_attacked_single_sent;
static unsigned fake_late_edges;
static bool fake_race_armed;
static unsigned fake_nfc_inits, fake_nfc_polls, fake_nfc_polls_at_terminal;
static unsigned fake_nfc_finish_calls, fake_nfc_edges;
static uint32_t fake_nfc_last_edge_ms, fake_nfc_stop_ready_ms;
static bool fake_nfc_stopping;

static void send_edge(uint16_t pin)
{
    assert(fake_irq_mask == 0u);
    if (fake_inside_sensor_read) ++fake_edges_during_reads;
    HAL_GPIO_EXTI_Callback(pin);
}

static void advance_time(uint32_t duration_ms)
{
    for (uint32_t step = 0; step < duration_ms; ++step) {
        assert(!fake_tick_suspended);
        ++fake_tick;
        if (!fake_uart_stopped &&
            !(fake_scenario.uart_jammed && fake_terminal_ms != 0u)) {
            /* 115200 baud, 8N1: 11520 complete characters per second. */
            fake_uart_credit += 11520u;
            size_t budget = fake_uart_credit / 1000u;
            fake_uart_credit %= 1000u;
            while (budget != 0u && fake_uart_bytes != 0u) {
                assert(fake_wire_length + 1u < WIRE_CAPACITY);
                fake_wire[fake_wire_length++] = fake_uart_queue[fake_uart_tail];
                fake_uart_tail = (fake_uart_tail + 1u) % UART_CAPACITY;
                --fake_uart_bytes;
                --budget;
            }
            fake_wire[fake_wire_length] = '\0';
        }
        while (fake_edge_next < fake_edge_count &&
               fake_edges[fake_edge_next].at_ms == fake_tick) {
            send_edge(fake_edges[fake_edge_next].pin);
            ++fake_edge_next;
        }
        if (fake_first_attack_ms != 0u) {
            if (!fake_attacked_single_sent &&
                fake_tick == fake_first_attack_ms + 1000u) {
                send_edge(BUTTON_EXTI13_Pin);
                fake_attacked_single_sent = true;
            }
            if (!fake_rescue_first_sent && fake_tick == fake_rescue_first_ms) {
                send_edge(BUTTON_EXTI13_Pin);
                fake_rescue_first_sent = true;
                if (!fake_scenario.rescue) ++fake_late_edges;
            }
            if (!fake_rescue_second_sent && fake_tick == fake_rescue_second_ms) {
                send_edge(BUTTON_EXTI13_Pin);
                fake_rescue_second_sent = true;
                if (!fake_scenario.rescue) ++fake_late_edges;
            }
        }
        if (fake_tick >= fake_scenario.stop_ms) longjmp(fake_escape, SIM_LIMIT);
    }
}

static void enqueue_uart(const char *bytes, size_t length)
{
    assert(length <= UART_CAPACITY - 1u - fake_uart_bytes);
    for (size_t index = 0; index < length; ++index) {
        fake_uart_queue[fake_uart_head] = bytes[index];
        fake_uart_head = (fake_uart_head + 1u) % UART_CAPACITY;
    }
    fake_uart_bytes += length;
}

void HAL_Init(void) {}
uint32_t HAL_GetTick(void) { return fake_tick; }
uint32_t __get_PRIMASK(void) { return fake_irq_mask; }
void __set_PRIMASK(uint32_t mask) { fake_irq_mask = mask; }
void __DMB(void) {}

void __disable_irq(void)
{
    if (fake_race_armed && fake_irq_mask == 0u) {
        /* A second edge is captured at 499 ms, immediately before the main
         * thread masks interrupts and considers the 500 ms single timeout. */
        fake_race_armed = false;
        const uint32_t resume_ms = fake_tick;
        fake_tick = 1499u;
        send_edge(BUTTON_EXTI13_Pin);
        fake_tick = resume_ms;
    }
    fake_irq_mask = 1u;
}

void HAL_NVIC_DisableIRQ(int irq)
{
    assert(irq == EXTI15_10_IRQn);
    fake_button_irq_disabled = true;
}

void HAL_SuspendTick(void) { fake_tick_suspended = true; }

void __WFI(void)
{
    assert(fake_irq_mask == 1u);
    if (!fake_scenario.boot_failure) {
        assert(fake_uart_stopped && fake_external_stopped);
        assert(fake_tick_suspended && fake_button_irq_disabled);
    }
    longjmp(fake_escape, SIM_HALTED);
}

bool Board_Init(void)
{
    fake_led = true;
    return !fake_scenario.boot_failure;
}

uint32_t Board_SensorHealth(void) { return SENSOR_ALL; }
void Board_LedWrite(bool on) { fake_led = on; }

static void read_motion_values(SensorSample *sample)
{
    const float pi = 3.14159265358979323846f;
    float angle = 0.0f;
    float rate = 0.0f;
    if (fake_scenario.flip && fake_tick >= 1000u) {
        if (fake_tick < 2000u) {
            angle = pi * (float)(fake_tick - 1000u) / 1000.0f;
            rate = 180.0f;
        } else angle = pi;
    }
    sample->acceleration_ms2[0] = 0.0f;
    sample->acceleration_ms2[1] = 9.80665f * sinf(angle);
    sample->acceleration_ms2[2] = 9.80665f * cosf(angle);
    sample->angular_rate_dps[0] = rate;
    sample->angular_rate_dps[1] = sample->angular_rate_dps[2] = 0.0f;
    sample->valid |= SENSOR_ACCELEROMETER | SENSOR_GYROSCOPE;
}

void Board_ReadSensors(SensorSample *sample)
{
    assert(fake_terminal_ms == 0u && !fake_uart_stopped);
    assert(fake_sensor_count < TRACE_CAPACITY);
    fake_sensor_reads[fake_sensor_count++] = fake_tick;
    sample->valid = SENSOR_ALL;
    sample->temperature_c = fake_scenario.threshold_changes &&
        fake_tick >= 2000u && fake_tick < 3000u ? 52.0f : 25.0f;
    sample->pressure_hpa = 1013.25f;
    sample->humidity_pct = 55.0f;
    sample->magnetic_ut[0] = 12.0f;
    sample->magnetic_ut[1] = 20.0f;
    sample->magnetic_ut[2] = 30.0f;
    if (fake_scenario.threshold_changes && fake_tick >= 4000u &&
        fake_tick < 5000u) {
        sample->valid &= ~(uint32_t)SENSOR_HUMIDITY;
        sample->humidity_pct = NAN;
    }
    read_motion_values(sample);
    fake_inside_sensor_read = true;
    advance_time(3u);
    fake_inside_sensor_read = false;
}

void Board_ReadMotion(SensorSample *sample)
{
    assert(fake_terminal_ms == 0u && !fake_uart_stopped);
    assert(fake_motion_count < MOTION_TRACE_CAPACITY);
    fake_motion_reads[fake_motion_count++] = fake_tick;
    read_motion_values(sample);
    fake_inside_sensor_read = true;
    advance_time(2u);
    fake_inside_sensor_read = false;
}

bool Board_UartWrite(const char *message_text)
{
    assert(!fake_uart_stopped);
    const size_t length = strlen(message_text);
    assert(fake_trace_count < TRACE_CAPACITY);
    assert(length < sizeof fake_trace[0].text);
    fake_trace[fake_trace_count].at_ms = fake_tick;
    memcpy(fake_trace[fake_trace_count++].text, message_text, length + 1u);
    if (strncmp(message_text, "Drone Was Attacked!", 18u) == 0 &&
        fake_first_attack_ms == 0u) {
        fake_first_attack_ms = fake_tick;
        if (fake_scenario.rescue) {
            fake_rescue_first_ms = fake_tick + 9400u;
            fake_rescue_second_ms = fake_tick + 9600u;
            fake_scenario.stop_ms = fake_tick + 10500u;
        } else {
            fake_rescue_first_ms = fake_tick + APP_RESCUE_MS + 30u;
            fake_rescue_second_ms = fake_tick + APP_RESCUE_MS + 130u;
        }
    }
    if (strncmp(message_text, "TERMINATED:", 11u) == 0) {
        assert(fake_terminal_ms == 0u);
        fake_terminal_ms = fake_tick;
        fake_sensor_count_at_terminal = fake_sensor_count;
        fake_motion_count_at_terminal = fake_motion_count;
        fake_external_polls_at_terminal = fake_external_polls;
        fake_nfc_polls_at_terminal = fake_nfc_polls;
        fake_uart_idle_checks_at_terminal = fake_uart_idle_checks;
        for (size_t count = 0; count < fake_scenario.terminal_backlog; ++count)
            enqueue_uart("~", 1u);
    }
    enqueue_uart(message_text, length);
    return true;
}

void Board_UartPoll(void) { advance_time(1u); }

bool Board_UartIdle(void)
{
    ++fake_uart_idle_checks;
    return fake_uart_bytes == 0u;
}

void Board_UartStop(void)
{
    assert(fake_terminal_ms != 0u && !fake_uart_stopped);
    assert(fake_uart_idle_checks > fake_uart_idle_checks_at_terminal);
    if (!fake_scenario.uart_jammed) assert(fake_uart_bytes == 0u);
    fake_uart_stopped = true;
    fake_uart_stop_ms = fake_tick;
}

void External_Init(uint32_t now_ms)
{
    (void)now_ms;
    ++fake_external_inits;
}

void External_Poll(uint32_t now_ms)
{
    (void)now_ms;
    assert(fake_terminal_ms == 0u && !fake_external_stopped);
    ++fake_external_polls;
    advance_time(1u);
}

void External_SetDisplay(const ExternalDisplayFrame *frame)
{
    assert(frame != NULL);
    assert(fake_display_count < DISPLAY_TRACE_CAPACITY);
    fake_display_frames[fake_display_count++] = *frame;
    assert(strlen(frame->mode) < sizeof fake_display_mode);
    memcpy(fake_display_mode, frame->mode, strlen(frame->mode) + 1u);
    fake_display_ready_ms = fake_tick + 90u;
    if (frame->attacked) {
        assert(frame->scene == EXTERNAL_SCENE_ATTACKED);
        assert(frame->target == EXTERNAL_COLOUR_NONE);
        assert(frame->rescue_seconds >= 1u && frame->rescue_seconds <= 10u);
    }
}

void External_FinishDisplay(void)
{
    assert(fake_terminal_ms != 0u);
    ++fake_finish_display_calls;
}

bool External_DisplayIdle(void)
{
    return !fake_scenario.optional_present ||
           fake_tick >= fake_display_ready_ms;
}

void External_Play(ExternalSound sound, uint32_t now_ms)
{
    (void)now_ms;
    assert((unsigned)sound <= (unsigned)EXTERNAL_SOUND_LOSE);
    ++fake_sound_count[sound];
}

void External_AllStop(void) { fake_external_stopped = true; }
bool External_DisplayOnline(void) { return fake_scenario.optional_present; }
bool External_BuzzerOnline(void) { return fake_scenario.optional_present; }
bool External_ColourOnline(void) { return false; }
bool External_IRPresent(void) { return false; }
bool External_IREnabled(void) { return fake_scenario.optional_present; }
ExternalColourSample External_GetColour(void)
{
    const ExternalColourSample result = {0};
    return result;
}

void Nfc_Init(const char *url, uint32_t now_ms)
{
    assert(strcmp(url, APP_NFC_DASHBOARD_URL) == 0);
    assert(strstr(url, "https://") == url && now_ms == fake_tick);
    ++fake_nfc_inits;
}

void Nfc_Poll(uint32_t now_ms, bool allow_update)
{
    assert(now_ms == fake_tick && !fake_nfc_stopping && fake_terminal_ms == 0u);
    assert(allow_update == (game.mode == GAME_STANDBY));
    ++fake_nfc_polls;
}

bool Nfc_Ready(void) { return fake_tick >= 100u && !fake_nfc_stopping; }
bool Nfc_PopTap(uint32_t *time_ms) { (void)time_ms; return false; }
uint32_t Nfc_Taps(void) { return 0u; }
void Nfc_SignalEdge(uint32_t now_ms)
{
    assert(!fake_tick_suspended);
    ++fake_nfc_edges;
    fake_nfc_last_edge_ms = now_ms;
}
void Nfc_BeginStop(void)
{
    assert(!fake_nfc_stopping && !fake_tick_suspended);
    fake_nfc_stopping = true;
    fake_nfc_stop_ready_ms = fake_tick + 40u;
}
void Nfc_FinishStop(uint32_t now_ms)
{
    assert(fake_nfc_stopping && !fake_tick_suspended && now_ms == fake_tick);
    ++fake_nfc_finish_calls;
}
bool Nfc_StopDone(void)
{
    assert(fake_nfc_stopping);
    return fake_tick >= fake_nfc_stop_ready_ms;
}

static void reset_simulation(Scenario setup)
{
    fake_scenario = setup;
    fake_tick = fake_irq_mask = 0;
    fake_tick_suspended = fake_button_irq_disabled = false;
    fake_uart_stopped = fake_external_stopped = false;
    fake_inside_sensor_read = false;
    fake_edges_during_reads = 0;
    fake_led = false;
    fake_external_polls = fake_external_polls_at_terminal = 0;
    fake_external_inits = fake_finish_display_calls = 0;
    fake_display_ready_ms = 0;
    fake_display_count = 0;
    memset(fake_display_mode, 0, sizeof fake_display_mode);
    memset(fake_sound_count, 0, sizeof fake_sound_count);
    fake_sensor_count = fake_motion_count = 0;
    fake_sensor_count_at_terminal = fake_motion_count_at_terminal = 0;
    fake_edge_count = fake_edge_next = 0;
    fake_trace_count = 0;
    fake_uart_head = fake_uart_tail = fake_uart_bytes = 0;
    fake_uart_credit = 0;
    fake_wire_length = 0;
    fake_wire[0] = '\0';
    fake_uart_idle_checks = fake_uart_idle_checks_at_terminal = 0;
    fake_first_attack_ms = fake_terminal_ms = fake_uart_stop_ms = 0;
    fake_rescue_first_ms = fake_rescue_second_ms = 0;
    fake_rescue_first_sent = fake_rescue_second_sent = false;
    fake_attacked_single_sent = false;
    fake_late_edges = 0;
    fake_race_armed = false;
    fake_nfc_inits = fake_nfc_polls = fake_nfc_polls_at_terminal = 0u;
    fake_nfc_finish_calls = fake_nfc_edges = 0u;
    fake_nfc_last_edge_ms = fake_nfc_stop_ready_ms = 0u;
    fake_nfc_stopping = false;

    /* Reset firmware static storage as a physical reset would. */
    memset(&game, 0, sizeof game);
    memset(&button, 0, sizeof button);
    memset(&sensors, 0, sizeof sensors);
    memset(&telemetry_status, 0, sizeof telemetry_status);
    edge_write = edge_read = 0;
    edge_overflow = false;
    last_attack_log = dropped_uart_messages = 0;
    terminal_event = false;
}

static void add_edge(uint32_t at_ms, uint16_t pin)
{
    assert(fake_edge_count < EDGE_CAPACITY);
    assert(fake_edge_count == 0u ||
           fake_edges[fake_edge_count - 1u].at_ms < at_ms);
    fake_edges[fake_edge_count++] = (ScriptedEdge){at_ms, pin};
}

static int run_firmware(void)
{
    const int reason = setjmp(fake_escape);
    if (reason == 0) {
        (void)firmware_main();
        assert(false); /* Firmware must run continuously or halt. */
    }
    return reason;
}

static size_t count_messages(const char *prefix)
{
    size_t count = 0;
    const size_t length = strlen(prefix);
    for (size_t index = 0; index < fake_trace_count; ++index)
        if (strncmp(fake_trace[index].text, prefix, length) == 0) ++count;
    return count;
}

static const UartTrace *find_message(const char *prefix, size_t occurrence)
{
    const size_t length = strlen(prefix);
    for (size_t index = 0; index < fake_trace_count; ++index) {
        if (strncmp(fake_trace[index].text, prefix, length) == 0) {
            if (occurrence == 0u) return &fake_trace[index];
            --occurrence;
        }
    }
    assert(false); /* Expected UART message missing. */
    return NULL;
}

static void start_battle_script(void)
{
    add_edge(100u, BUTTON_EXTI13_Pin);
    add_edge(260u, BUTTON_EXTI13_Pin);
}

static void test_controls_telemetry_and_sensor_recovery(void)
{
    reset_simulation((Scenario){.threshold_changes = true, .stop_ms = 5600u});
    add_edge(70u, 1u); /* Unrelated EXTI does not become a user press. */
    add_edge(80u, 0x0010u); /* NFC GPO belongs to NFC, not the user button. */
    add_edge(100u, BUTTON_EXTI13_Pin);
    add_edge(108u, BUTTON_EXTI13_Pin); /* Contact bounce. */
    add_edge(260u, BUTTON_EXTI13_Pin);
    add_edge(1003u, BUTTON_EXTI13_Pin); /* Arrives during a bounded sensor read. */
    add_edge(1010u, BUTTON_EXTI13_Pin);
    add_edge(2003u, BUTTON_EXTI13_Pin);
    add_edge(3000u, BUTTON_EXTI13_Pin);
    add_edge(3200u, BUTTON_EXTI13_Pin);
    add_edge(4000u, BUTTON_EXTI13_Pin); /* Single ignored in standby. */
    assert(run_firmware() == SIM_LIMIT);
    assert(fake_edge_next == fake_edge_count);
    assert(fake_nfc_inits == 1u && fake_nfc_polls > 0u);
    assert(fake_nfc_edges == 1u && fake_nfc_last_edge_ms == 80u);
    assert(count_messages("NFC READY:") == 1u);
    assert(find_message("NFC READY:", 0u)->at_ms >= 100u);
    assert(strstr(find_message("NFC READY:", 0u)->text,
                  APP_NFC_DASHBOARD_URL "?mission=0") != NULL);
    assert(fake_edges_during_reads != 0u);
    assert(count_messages("Entering STANDBY MODE") == 2u);
    assert(count_messages("Entering BATTLE MODE") == 1u);
    assert(count_messages("LASERGUN_CHARGING:") == 2u);
    assert(count_messages("LASERGUN_FIRED:") == 1u);
    const UartTrace *first = find_message("LASERGUN_CHARGING: 3/10", 0u);
    const UartTrace *second = find_message("LASERGUN_CHARGING: 6/10", 0u);
    assert(first->at_ms >= 1503u && first->at_ms <= 1510u);
    assert(second->at_ms >= 2503u && second->at_ms <= 2510u);
    assert(strstr(find_message("LASERGUN_FIRED:", 0u)->text,
                  "remaining 1/10") != NULL);
    assert(fake_sound_count[EXTERNAL_SOUND_SHOT] == 1u);
    assert(game.mode == GAME_STANDBY && game.energy == 1u && fake_led);

    /* The shot immediately replaces the zombie with a human. The renderer
     * still needs the resolved zombie and its old approach age to animate a
     * hit in the correct place, rather than firing at the incoming human. */
    size_t hit_frames = 0u;
    uint16_t last_effect_age = 0u;
    for (size_t index = 0u; index < fake_display_count; ++index) {
        const ExternalDisplayFrame *frame = &fake_display_frames[index];
        if (frame->scene != EXTERNAL_SCENE_BATTLE ||
            frame->effect != EXTERNAL_EFFECT_HIT) continue;
        assert(frame->target == EXTERNAL_COLOUR_GREEN);
        assert(frame->effect_target == EXTERNAL_COLOUR_RED);
        assert(frame->effect_target_age_ms == 2503u - 260u);
        assert(frame->target_age_ms == frame->effect_age_ms);
        assert(frame->now_ms - frame->effect_age_ms == 2503u);
        assert(frame->score == GAME_ZOMBIE_POINTS);
        assert(frame->zombie_hits == 1u && frame->energy == 1u);
        if (hit_frames != 0u) assert(frame->effect_age_ms > last_effect_age);
        last_effect_age = frame->effect_age_ms;
        ++hit_frames;
    }
    assert(hit_frames >= 2u);
    assert(fake_display_frames[fake_display_count - 1u].scene ==
           EXTERNAL_SCENE_STANDBY);
    assert(fake_display_frames[fake_display_count - 1u].effect ==
           EXTERNAL_EFFECT_NONE);

    assert(fake_sensor_count == 6u); /* Boot and one sample per second. */
    for (size_t index = 1u; index < fake_sensor_count; ++index) {
        const uint32_t ideal_ms = (uint32_t)index * APP_SENSOR_PERIOD_MS;
        assert(fake_sensor_reads[index] >= ideal_ms);
        assert(fake_sensor_reads[index] <= ideal_ms + 5u);
    }
    assert(count_messages("T:") == 3u);
    assert(count_messages("WARNING T:") == 1u);
    assert(count_messages("WARNING H:") == 1u);
    assert(find_message("WARNING T:", 0u)->at_ms >= 2000u);
    assert(find_message("WARNING T:", 0u)->at_ms < 2010u);
    assert(strstr(find_message("WARNING H:", 0u)->text,
                  "sensor unavailable; telemetry paused") != NULL);
    assert(find_message("T:", 1u)->at_ms >= 3000u);
    assert(find_message("T:", 2u)->at_ms >= 5000u);
    assert(fake_motion_count > 100u);
    assert(!fake_uart_stopped && !fake_external_stopped);
}

static void assert_attack_cadence(void)
{
    assert(fake_first_attack_ms >= 2000u && fake_first_attack_ms <= 2300u);
    const size_t count = count_messages("Drone Was Attacked!");
    assert(count >= 10u);
    for (size_t index = 1u; index < count; ++index) {
        const UartTrace *previous = find_message("Drone Was Attacked!", index - 1u);
        const UartTrace *current = find_message("Drone Was Attacked!", index);
        assert(current->at_ms - previous->at_ms >= 990u);
        assert(current->at_ms - previous->at_ms <= 1010u);
    }
    assert(count_messages("LASERGUN_CHARGING:") == 0u);
    assert(count_messages("LASERGUN_FIRED:") == 0u);
}

static void test_timeout_drains_then_silences_everything(bool jam_uart)
{
    reset_simulation((Scenario){.flip = true, .uart_jammed = jam_uart,
        .optional_present = true, .terminal_backlog = 3000u, .stop_ms = 14000u});
    start_battle_script();
    assert(run_firmware() == SIM_HALTED);
    assert_attack_cadence();
    assert(count_messages("TERMINATED:") == 1u);
    assert(count_messages("Drone rescued") == 0u);
    assert(game.mode == GAME_TERMINATED && !fake_led);
    assert(fake_terminal_ms - fake_first_attack_ms >= APP_RESCUE_MS);
    assert(fake_terminal_ms - fake_first_attack_ms <= APP_RESCUE_MS + 8u);
    assert(fake_uart_stopped && fake_external_stopped);
    assert(fake_uart_idle_checks > 0u && fake_finish_display_calls > 0u);
    assert(fake_sensor_count == fake_sensor_count_at_terminal);
    assert(fake_motion_count == fake_motion_count_at_terminal);
    assert(fake_external_polls == fake_external_polls_at_terminal);
    assert(fake_nfc_polls == fake_nfc_polls_at_terminal);
    assert(fake_nfc_stopping && fake_nfc_finish_calls > 0u);
    assert(fake_late_edges == 2u); /* Arriving after the deadline cannot rescue. */
    assert(strcmp(fake_display_mode, "TERMINATED") == 0);
    const ExternalDisplayFrame *final_frame =
        &fake_display_frames[fake_display_count - 1u];
    assert(final_frame->scene == EXTERNAL_SCENE_TERMINATED);
    assert(final_frame->target == EXTERNAL_COLOUR_NONE);
    assert(!final_frame->attacked && final_frame->rescue_seconds == 0u);
    const uint32_t drain_ms = fake_uart_stop_ms - fake_terminal_ms;
    assert(drain_ms <= APP_SHUTDOWN_DRAIN_MS);
    if (jam_uart) {
        assert(drain_ms == APP_SHUTDOWN_DRAIN_MS);
        assert(fake_uart_bytes != 0u);
    } else {
        assert(drain_ms > 200u && drain_ms < APP_SHUTDOWN_DRAIN_MS);
        assert(fake_uart_bytes == 0u);
        assert(strstr(fake_wire,
            "TERMINATED: rescue timed out. Communication ending.\r\n") != NULL);
    }
}

static void test_rescue_just_before_deadline(void)
{
    reset_simulation((Scenario){.flip = true, .rescue = true,
        .optional_present = true, .stop_ms = 14000u});
    start_battle_script();
    assert(run_firmware() == SIM_LIMIT);
    assert_attack_cadence();
    assert(count_messages("Drone rescued") == 1u);
    assert(count_messages("TERMINATED:") == 0u);
    const UartTrace *rescued = find_message("Drone rescued", 0u);
    assert(rescued->at_ms - fake_first_attack_ms >= 9600u);
    assert(rescued->at_ms - fake_first_attack_ms < APP_RESCUE_MS);
    assert(game.mode == GAME_BATTLE && !game.attacked);
    assert(!fake_uart_stopped && !fake_external_stopped);
    assert(fake_sensor_reads[fake_sensor_count - 1u] > rescued->at_ms);
    size_t rescued_frames = 0u;
    bool saw_attack_frame = false;
    for (size_t index = 0u; index < fake_display_count; ++index) {
        const ExternalDisplayFrame *frame = &fake_display_frames[index];
        if (frame->scene == EXTERNAL_SCENE_ATTACKED) saw_attack_frame = true;
        if (frame->effect != EXTERNAL_EFFECT_RESCUED) continue;
        assert(saw_attack_frame);
        assert(frame->scene == EXTERNAL_SCENE_BATTLE && !frame->attacked);
        assert(frame->rescue_seconds == 0u);
        assert(frame->target != EXTERNAL_COLOUR_NONE);
        assert(frame->target_age_ms == frame->effect_age_ms);
        assert(frame->effect_target_age_ms == 0u);
        ++rescued_frames;
    }
    assert(rescued_frames >= 2u);
}

static void test_visual_event_ages_across_tick_wrap(void)
{
    reset_simulation((Scenario){.stop_ms = 1000u});
    const uint32_t battle_ms = UINT32_MAX - 800u;
    game_init(&game, UINT32_MAX - 1000u);
    game_button(&game, BUTTON_DOUBLE, battle_ms);
    handle_events(battle_ms);
    game_button(&game, BUTTON_SINGLE, UINT32_MAX - 400u);
    handle_events(UINT32_MAX - 400u);
    game_button(&game, BUTTON_SINGLE, 100u);
    handle_events(100u);
    fake_tick = 125u;
    update_display(fake_tick);
    assert(fake_display_count == 1u);
    const ExternalDisplayFrame *frame = &fake_display_frames[0];
    assert(frame->scene == EXTERNAL_SCENE_BATTLE);
    assert(frame->effect == EXTERNAL_EFFECT_HIT);
    assert(frame->effect_target == EXTERNAL_COLOUR_RED);
    assert(frame->target == EXTERNAL_COLOUR_GREEN);
    assert(frame->effect_age_ms == 25u && frame->target_age_ms == 25u);
    assert(frame->effect_target_age_ms == (uint32_t)(100u - battle_ms));
}

static void test_second_edge_racing_with_single_timeout(void)
{
    reset_simulation((Scenario){.stop_ms = 2000u});
    game_init(&game, 0u);
    game_button(&game, BUTTON_DOUBLE, 0u);
    button_init(&button, 0u);
    fake_tick = 1000u;
    send_edge(BUTTON_EXTI13_Pin);
    handle_buttons();
    fake_tick = 1500u;
    fake_race_armed = true;
    handle_buttons();
    assert(!fake_race_armed);
    assert(game.mode == GAME_STANDBY);
    assert(game.energy == 0u);
    assert(fake_irq_mask == 0u);
}

static void test_startup_failure_halts_without_claiming_success(void)
{
    reset_simulation((Scenario){.boot_failure = true, .stop_ms = 100u});
    assert(run_firmware() == SIM_HALTED);
    assert(fake_trace_count == 0u);
    assert(fake_sensor_count == 0u && fake_motion_count == 0u);
    assert(fake_external_inits == 0u);
    assert(fake_nfc_inits == 0u && fake_nfc_polls == 0u);
}

int main(void)
{
    test_controls_telemetry_and_sensor_recovery();
    test_timeout_drains_then_silences_everything(false);
    test_timeout_drains_then_silences_everything(true);
    test_rescue_just_before_deadline();
    test_visual_event_ages_across_tick_wrap();
    test_second_edge_racing_with_single_timeout();
    test_startup_failure_halts_without_claiming_success();
    puts("App integration tests passed: EXTI controls, telemetry, animation events, rescue, shutdown");
    return 0;
}
