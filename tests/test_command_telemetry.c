/* Real C serialization and command lifecycle with fake transports. No hardware. */
#include "command_telemetry.h"
#include "network_io.h"
#include "nfc_io.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../Core/Src/command_telemetry.c"

#define LINES 64u
static char lines[LINES][NETWORK_PACKET_MAX];
static unsigned line_count;
static char fixtures[4][NETWORK_PACKET_MAX];
static bool fake_connected, fake_network_idle, fake_begin_drain, fake_network_stopped;
static bool fake_nfc_stopped, fake_mission_pending, fake_tap_pending;
static bool fake_nfc_stop_done, fake_nfc_ready;
static bool fake_queue_full;
static uint8_t fake_requested, fake_nfc_mission;
static uint32_t fake_tap_ms, fake_taps, fake_drop;
static unsigned network_polls, mission_reads, nfc_polls, tap_reads, mission_sets;
static unsigned nfc_finish_calls;
static unsigned final_queues, uart_messages, tap_sounds;
static char last_uart[256];
static bool last_nfc_allow;
static char nfc_url[180];
static const char *nfc_borrowed_url;
static const NetworkConfig *fake_config;
static char trace[512];
static unsigned trace_used;

static void mark(char value)
{
    assert(trace_used + 1u < sizeof trace);
    trace[trace_used++] = value;
    trace[trace_used] = '\0';
}

const NetworkConfig *Network_GetConfig(void) { return fake_config; }
void Network_Init(uint32_t now_ms) { (void)now_ms; mark('I'); }
void Network_Poll(uint32_t now_ms)
{
    (void)now_ms;
    assert(!fake_network_stopped);
    ++network_polls;
    mark('P');
}
static bool capture_line(const char *line, size_t length, char marker)
{
    assert(!fake_begin_drain && !fake_network_stopped);
    assert(line != NULL && length > 0u && length < NETWORK_PACKET_MAX);
    assert(line[length - 1u] == '\n' && line_count < LINES);
    memcpy(lines[line_count], line, length);
    lines[line_count++][length] = '\0';
    mark(marker);
    return true;
}
bool Network_Queue(const char *line, size_t length)
{
    if (!fake_connected || fake_queue_full) {
        ++fake_drop;
        return false;
    }
    return capture_line(line, length, 'Q');
}
bool Network_QueueFinal(const char *line, size_t length)
{
    ++final_queues;
    bool result = capture_line(line, length, 'F');
    Network_BeginDrain();
    return result;
}
bool Network_Connected(void) { return fake_connected; }
uint32_t Network_Dropped(void) { return fake_drop; }
bool Network_ReadMission(uint8_t *id)
{
    assert(!fake_begin_drain);
    ++mission_reads;
    if (!fake_mission_pending) return false;
    *id = fake_requested;
    fake_mission_pending = false;
    return true;
}
void Network_BeginDrain(void)
{
    if (!fake_begin_drain) mark('D');
    fake_begin_drain = true;
}
bool Network_Idle(void) { return fake_network_idle; }
void Network_Stop(void) { fake_network_stopped = true; mark('S'); }

void Nfc_Init(const char *url, uint32_t now_ms)
{
    (void)now_ms;
    assert(url != NULL && strlen(url) < sizeof nfc_url);
    strcpy(nfc_url, url);
    nfc_borrowed_url = url;
    mark('i');
}
void Nfc_Poll(uint32_t now_ms, bool allow_update)
{
    (void)now_ms;
    assert(!fake_nfc_stopped);
    ++nfc_polls;
    last_nfc_allow = allow_update;
}
bool Nfc_SetMission(uint8_t id)
{
    assert(!fake_nfc_stopped && id <= 2u);
    ++mission_sets;
    fake_nfc_mission = id;
    return true;
}
bool Nfc_Ready(void) { return fake_nfc_ready && !fake_nfc_stopped; }
uint32_t Nfc_Taps(void) { return fake_taps; }
bool Nfc_PopTap(uint32_t *time_ms)
{
    assert(!fake_nfc_stopped);
    ++tap_reads;
    if (!fake_tap_pending) return false;
    *time_ms = fake_tap_ms;
    fake_tap_pending = false;
    return true;
}
void Nfc_BeginStop(void)
{
    assert(!fake_nfc_stopped);
    fake_nfc_stopped = true;
    mark('N');
}
void Nfc_FinishStop(uint32_t now_ms)
{
    (void)now_ms;
    assert(fake_nfc_stopped);
    if (++nfc_finish_calls >= 2u) fake_nfc_stop_done = true;
}
bool Nfc_StopDone(void) { return fake_nfc_stop_done; }

bool Board_UartWrite(const char *message)
{
    assert(message != NULL && message[0] != '{' && strlen(message) < sizeof last_uart);
    strcpy(last_uart, message);
    ++uart_messages;
    return true;
}

void External_Play(ExternalSound sound, uint32_t now_ms)
{
    (void)now_ms;
    assert(sound == EXTERNAL_SOUND_CHARGE);
    ++tap_sounds;
}

static SensorSample sample(void)
{
    const SensorSample result = {
        .temperature_c = 29.5f, .pressure_hpa = 1010.25f, .humidity_pct = 60.5f,
        .acceleration_ms2 = {0.0f, -0.125f, 9.81f},
        .angular_rate_dps = {1.0f, -2.0f, 3.5f},
        .magnetic_ut = {10.0f, -20.0f, 30.0f}, .valid = SENSOR_ALL
    };
    return result;
}

static void reset(uint32_t now)
{
    memset(lines, 0, sizeof lines);
    line_count = 0;
    fake_connected = true;
    fake_network_idle = fake_begin_drain = fake_network_stopped = false;
    fake_nfc_stopped = fake_mission_pending = fake_tap_pending = false;
    fake_nfc_stop_done = false;
    fake_nfc_ready = false;
    fake_queue_full = false;
    fake_requested = fake_nfc_mission = 0;
    fake_tap_ms = fake_taps = fake_drop = 0;
    network_polls = mission_reads = nfc_polls = tap_reads = mission_sets = 0;
    nfc_finish_calls = 0;
    final_queues = uart_messages = tap_sounds = 0;
    last_uart[0] = '\0';
    last_nfc_allow = false;
    trace_used = 0;
    trace[0] = '\0';
    fake_config = NULL;
    Command_Init(now);
}

static unsigned sequence_of(const char *line)
{
    const char *where = strstr(line, "\"seq\":");
    unsigned value;
    assert(where && sscanf(where, "\"seq\":%u", &value) == 1);
    return value;
}

static void assert_dashboard_url(const char *derived)
{
    (void)derived;
#if defined(TEST_NFC_URL_OVERRIDE)
    assert(strcmp(nfc_url, "https://garden.example/drone/GG01") == 0);
#elif defined(TEST_NFC_URL_LENGTH)
    if (TEST_NFC_URL_LENGTH >= 180) assert(nfc_url[0] == '\0');
    else {
        static const char prefix[] = "https://example.test/";
        assert(strlen(nfc_url) == TEST_NFC_URL_LENGTH);
        assert(strncmp(nfc_url, prefix, sizeof prefix - 1u) == 0);
        for (size_t i = sizeof prefix - 1u; i < strlen(nfc_url); ++i)
            assert(nfc_url[i] == 'x');
    }
#else
    assert(strcmp(nfc_url, derived) == 0);
#endif
    /* A caller retaining the URL must not observe Command_Init stack storage. */
    assert(strcmp(nfc_borrowed_url, nfc_url) == 0);
}

static void test_initialization_and_canonical_snapshot(void)
{
    reset(1000u);
    assert_dashboard_url("");
    const NetworkConfig no_address = {NULL, NULL, {0, 0, 0, 0}, 8081};
    fake_config = &no_address;
    Command_Init(1000u);
    assert_dashboard_url("");
    const NetworkConfig known_address = {NULL, NULL, {10, 1, 2, 3}, 8081};
    fake_config = &known_address;
    Command_Init(1000u);
    assert_dashboard_url("http://10.1.2.3:8080/drone/GG01");
    fake_config = NULL;

    Game current;
    game_init(&current, 0u);
    SensorSample sensors = sample();
    fake_drop = 4u;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1000u);
    assert(line_count == 1u && sequence_of(lines[0]) == 0u);
    assert(uart_messages == 0u); /* JSON telemetry belongs to WiFi, not UART. */
    const char *required[] = {
        "\"type\":\"snapshot\"", "\"id\":\"GG01\"", "\"tick\":1000",
        "\"mode\":\"standby\"", "\"attacked\":false", "\"round\":\"active\"",
        "\"target\":\"none\"", "\"energy\":0", "\"lives\":3", "\"hits\":0",
        "\"score\":0", "\"rescue_ms\":0", "\"target_ms\":0", "\"valid\":63",
        "\"abnormal\":0", "\"unavailable\":0", "\"t\":29.500", "\"p\":1010.250",
        "\"h\":60.500", "\"a\":[0.000,-0.125,9.810]", "\"g\":[1.000,-2.000,3.500]",
        "\"m\":[10.000,-20.000,30.000]", "\"ir\":false", "\"mission\":0",
        "\"nfc\":false", "\"nfc_taps\":0", "\"wifi\":true", "\"wifi_drop\":4"
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i)
        assert(strstr(lines[0], required[i]) != NULL);
    strcpy(fixtures[0], lines[0]);
    assert(last_nfc_allow);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1999u);
    assert(line_count == 1u);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 2000u);
    assert(line_count == 2u && sequence_of(lines[1]) == 1u);
}

static void test_events_preserve_old_target_and_share_sequence(void)
{
    reset(100u);
    Game current;
    SensorSample sensors = sample();
    game_init(&current, 0u);
    game_button(&current, BUTTON_DOUBLE, 100u);
    GameEvent event;
    while (game_pop_event(&current, &event)) { /* Discard boot/entry events. */ }
    assert(current.target == GAME_TARGET_ZOMBIE);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 100u);
    game_button(&current, BUTTON_SINGLE, 600u);
    game_button(&current, BUTTON_SINGLE, 1200u);
    bool saw_fire = false;
    while (game_pop_event(&current, &event)) {
        Command_Event(&event);
        if (event.type == GAME_EVENT_FIRED) {
            const char *line = lines[line_count - 1u];
            assert(strstr(line, "\"event\":\"fired\"") != NULL);
            assert(strstr(line, "\"target\":\"zombie\"") != NULL);
            assert(strstr(line, "\"energy\":1") != NULL);
            strcpy(fixtures[1], line);
            saw_fire = true;
        }
    }
    assert(saw_fire && current.target == GAME_TARGET_HUMAN);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1200u);
    assert(strstr(lines[line_count - 1u], "\"target\":\"human\"") != NULL);
    assert(strstr(lines[line_count - 1u], "\"target_ms\":4000") != NULL);
    for (unsigned i = 0; i < line_count; ++i) assert(sequence_of(lines[i]) == i);
    unsigned before = line_count;
    Command_Event(NULL);
    const GameEvent invalid = {.type = (GameEventType)255};
    Command_Event(&invalid);
    assert(line_count == before);
}

static void test_finite_and_unavailable_values_are_json_null(void)
{
    reset(0u);
    Game current;
    game_init(&current, 0u);
    SensorSample sensors = sample();
    sensors.temperature_c = NAN;
    sensors.angular_rate_dps[1] = INFINITY;
    sensors.valid &= ~(uint32_t)(SENSOR_PRESSURE | SENSOR_HUMIDITY);
    Command_Poll(&current, &sensors, (TelemetryStatus){32u, 6u}, 0u);
    assert(strstr(lines[0], "\"t\":null,\"p\":null,\"h\":null") != NULL);
    assert(strstr(lines[0], "\"g\":[1.000,null,3.500]") != NULL);
    assert(strstr(lines[0], "\"valid\":57") != NULL);
    assert(strstr(lines[0], "\"abnormal\":32,\"unavailable\":6") != NULL);
    assert(strstr(lines[0], "nan") == NULL && strstr(lines[0], "inf") == NULL);
    strcpy(fixtures[2], lines[0]);
}

static void test_mission_gate_and_nfc_tap_publish_immediately(void)
{
    reset(1000u);
    Game current;
    SensorSample sensors = sample();
    game_init(&current, 0u);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1000u);
    fake_requested = 2;
    fake_mission_pending = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1001u);
    assert(mission_sets == 1u && fake_nfc_mission == 2u);
    assert(uart_messages == 1u && strstr(last_uart, "Greenhouse Shelter") != NULL);
    assert(strstr(lines[1], "\"event\":\"mission_assigned\"") != NULL);
    assert(strstr(lines[2], "\"mission\":2") != NULL);

    game_button(&current, BUTTON_DOUBLE, 1002u);
    fake_requested = 1;
    fake_mission_pending = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1003u);
    assert(mission_sets == 1u && !last_nfc_allow);
    assert(uart_messages == 2u && strstr(last_uart, "MISSION REJECTED") != NULL);
    assert(strstr(lines[3], "\"event\":\"mission_rejected\"") != NULL);
    assert(strstr(lines[4], "\"mission\":2") != NULL);

    current.attacked = true;
    current.attacked_since_ms = 1003u;
    fake_mission_pending = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1004u);
    assert(mission_sets == 1u && strstr(lines[6], "\"rescue_ms\":9999") != NULL);
    fake_taps = 1u;
    fake_tap_ms = 1005u;
    fake_tap_pending = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1005u);
    assert(strstr(lines[7], "\"event\":\"nfc_tap\"") != NULL);
    assert(strstr(lines[8], "\"nfc_taps\":1") != NULL);
    assert(tap_sounds == 1u && uart_messages == 4u);
    assert(strstr(last_uart, "NFC CHECK-IN") != NULL);
    assert(current.mode == GAME_BATTLE && current.attacked);

    current.mode = GAME_STANDBY;
    current.attacked = false;
    fake_requested = 9;
    fake_mission_pending = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1006u);
    assert(mission_sets == 1u);
    assert(strstr(lines[9], "\"event\":\"mission_rejected\"") != NULL);
}

static void test_nfc_readiness_publishes_and_announces_each_ready_edge_once(void)
{
    reset(1000u);
    const NetworkConfig config = {NULL, NULL, {10, 1, 2, 3}, 8081};
    fake_config = &config;
    Command_Init(1000u);
    assert_dashboard_url("http://10.1.2.3:8080/drone/GG01");
    Game current;
    SensorSample sensors = sample();
    game_init(&current, 0u);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1000u);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1001u);
    assert(line_count == 1u && uart_messages == 0u);
    assert(strstr(lines[0], "\"nfc\":false") != NULL);

    fake_nfc_ready = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1002u);
    assert(line_count == 2u && uart_messages == 1u);
    assert(strstr(lines[1], "\"nfc\":true") != NULL);
    assert(strstr(last_uart, "NFC READY: ") != NULL);
    assert(strstr(last_uart, nfc_url) != NULL);
    assert(strstr(last_uart, "?mission=0") != NULL);
    assert(strstr(last_uart, "tap the board NFC antenna") != NULL);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1003u);
    assert(line_count == 2u && uart_messages == 1u);

    fake_nfc_ready = false;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1004u);
    assert(line_count == 3u && uart_messages == 1u);
    assert(strstr(lines[2], "\"nfc\":false") != NULL);
    fake_requested = 2;
    fake_mission_pending = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1005u);
    assert(mission_sets == 1u && fake_nfc_mission == 2u && uart_messages == 2u);
    fake_nfc_ready = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1006u);
    assert(line_count == 6u && uart_messages == 3u);
    assert(strstr(last_uart, "NFC READY: ") != NULL);
    assert(strstr(last_uart, "?mission=2") != NULL);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1007u);
    assert(line_count == 6u && uart_messages == 3u);
}

static void test_final_snapshot_precedes_drain_and_no_new_acquisition(void)
{
    reset(0u);
    Game current;
    SensorSample sensors = sample();
    game_init(&current, 0u);
    current.mode = GAME_TERMINATED;
    current.round = GAME_ROUND_LOST;
    current.attacked = true;
    current.target = GAME_TARGET_NONE;
    trace_used = 0;
    trace[0] = '\0';
    fake_queue_full = true; /* The final status must survive ordinary backlog. */
    Command_Final(&current, &sensors, (TelemetryStatus){0u, 0u}, 10000u);
    assert(strcmp(trace, "FDN") == 0 && final_queues == 1u);
    assert(strstr(lines[0], "\"mode\":\"terminated\"") != NULL);
    assert(strstr(lines[0], "\"attacked\":false") != NULL);
    assert(strstr(lines[0], "\"nfc\":false") != NULL);
    strcpy(fixtures[3], lines[0]);
    assert(fake_begin_drain && fake_nfc_stopped && !Command_Idle());
    const unsigned old_mission_reads = mission_reads;
    const unsigned old_nfc_polls = nfc_polls, old_tap_reads = tap_reads;
    const unsigned old_lines = line_count;
    fake_mission_pending = fake_tap_pending = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 10001u);
    const GameEvent event = {.type = GAME_EVENT_CHARGED};
    Command_Event(&event);
    Command_Final(&current, &sensors, (TelemetryStatus){0u, 0u}, 10002u);
    Command_Finish(10003u);
    assert(network_polls == 1u && line_count == old_lines);
    assert(mission_reads == old_mission_reads && nfc_polls == old_nfc_polls);
    assert(tap_reads == old_tap_reads);
    fake_network_idle = true;
    assert(!Command_Idle()); /* Pending NFC quieting must also finish. */
    Command_Finish(10004u);
    assert(Command_Idle() && fake_nfc_stop_done);
    assert(network_polls == 1u);
    fake_network_idle = false;
    fake_connected = false;
    assert(Command_Idle());
    Command_Finish(10005u);
    assert(network_polls == 1u);
    Command_Stop();
    assert(fake_network_stopped);
}

static void test_snapshot_cadence_across_tick_wrap(void)
{
    reset(UINT32_MAX - 100u);
    Game current;
    SensorSample sensors = sample();
    game_init(&current, UINT32_MAX - 100u);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, UINT32_MAX - 100u);
    assert(line_count == 1u);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 898u);
    assert(line_count == 1u);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 899u);
    assert(line_count == 2u && sequence_of(lines[1]) == 1u);
}

static void test_reconnect_publishes_current_snapshot_without_waiting_one_second(void)
{
    reset(1000u);
    Game current;
    SensorSample sensors = sample();
    game_init(&current, 0u);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1000u);
    assert(line_count == 1u && sequence_of(lines[0]) == 0u);

    fake_connected = false;
    game_button(&current, BUTTON_DOUBLE, 1100u);
    game_button(&current, BUTTON_SINGLE, 1200u);
    const GameEvent charge = {.type = GAME_EVENT_CHARGED, .time_ms = 1200u,
        .energy = current.energy, .score = current.score, .target = current.target};
    Command_Event(&charge);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1201u);
    assert(line_count == 1u && fake_drop == 2u);

    sensors.temperature_c = 30.25f;
    fake_connected = true;
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1202u);
    assert(line_count == 2u && sequence_of(lines[1]) == 3u);
    assert(strstr(lines[1], "\"tick\":1202") != NULL);
    assert(strstr(lines[1], "\"mode\":\"battle\"") != NULL);
    assert(strstr(lines[1], "\"energy\":3") != NULL);
    assert(strstr(lines[1], "\"t\":30.250") != NULL);
    assert(strstr(lines[1], "\"wifi\":true,\"wifi_drop\":2") != NULL);
    Command_Poll(&current, &sensors, (TelemetryStatus){0u, 0u}, 1203u);
    assert(line_count == 2u); /* A steady connection does not publish every loop. */
}

int main(int argc, char **argv)
{
    test_initialization_and_canonical_snapshot();
    test_events_preserve_old_target_and_share_sequence();
    test_finite_and_unavailable_values_are_json_null();
    test_mission_gate_and_nfc_tap_publish_immediately();
    test_nfc_readiness_publishes_and_announces_each_ready_edge_once();
    test_final_snapshot_precedes_drain_and_no_new_acquisition();
    test_snapshot_cadence_across_tick_wrap();
    test_reconnect_publishes_current_snapshot_without_waiting_one_second();
    if (argc == 2 && strcmp(argv[1], "--dump") == 0) {
        for (unsigned i = 0; i < 4u; ++i) fputs(fixtures[i], stdout);
    } else {
        puts("Command telemetry tests passed: canonical JSON, events, nulls, missions, NFC, cadence and shutdown.");
    }
    return 0;
}
