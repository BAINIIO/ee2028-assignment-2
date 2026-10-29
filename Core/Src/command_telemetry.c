/* One authoritative game, two output transports. Formatting never sends UART. */
#include "command_telemetry.h"
#include "network_io.h"
#include "nfc_io.h"
#include "external_io.h"
#include "board_io.h"
#include "app_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t sequence, last_snapshot;
static uint8_t mission;
static bool dirty, draining, was_connected;
static char packet[NETWORK_PACKET_MAX];

static const char *target_text(GameTarget target)
{
    return target == GAME_TARGET_ZOMBIE ? "zombie" :
           target == GAME_TARGET_HUMAN ? "human" : "none";
}

static void queue_event(const char *name, uint32_t tick, unsigned energy,
                        int score, GameTarget target)
{
    int count = snprintf(packet, sizeof packet,
        "{\"type\":\"event\",\"id\":\"GG01\",\"seq\":%lu,\"tick\":%lu,"
        "\"event\":\"%s\",\"energy\":%u,\"score\":%d,\"target\":\"%s\"}\n",
        (unsigned long)sequence++, (unsigned long)tick, name, energy, score,
        target_text(target));
    if (count > 0 && (size_t)count < sizeof packet)
        (void)Network_Queue(packet, (size_t)count);
}

void Command_Event(const GameEvent *event)
{
    static const char *const names[] = {
        "standby", "battle", "charged", "fired", "hit", "human_hit",
        "human_saved", "missed", "target", "colour_mismatch", "attacked",
        "rescued", "victory", "defeat", "terminated"
    };
    if (!event || draining || (unsigned)event->type >= sizeof names / sizeof names[0]) return;
    queue_event(names[event->type], event->time_ms, event->energy,
                event->score, event->target);
    dirty = true;
}

static void quantity(char output[24], float value, bool valid)
{
    if (!valid || !isfinite(value)) memcpy(output, "null", 5);
    else (void)snprintf(output, 24, "%.3f", (double)value);
}

static void snapshot(const Game *game, const SensorSample *sample,
                     TelemetryStatus status, uint32_t now, bool final_packet)
{
    char values[12][24];
    quantity(values[0], sample->temperature_c, (sample->valid & SENSOR_TEMPERATURE) != 0u);
    quantity(values[1], sample->pressure_hpa, (sample->valid & SENSOR_PRESSURE) != 0u);
    quantity(values[2], sample->humidity_pct, (sample->valid & SENSOR_HUMIDITY) != 0u);
    for (unsigned i = 0; i < 3u; ++i) {
        quantity(values[3u + i], sample->acceleration_ms2[i], (sample->valid & SENSOR_ACCELEROMETER) != 0u);
        quantity(values[6u + i], sample->angular_rate_dps[i], (sample->valid & SENSOR_GYROSCOPE) != 0u);
        quantity(values[9u + i], sample->magnetic_ut[i], (sample->valid & SENSOR_MAGNETOMETER) != 0u);
    }
    bool terminated = game->mode == GAME_TERMINATED;
    bool target_active = game->mode == GAME_BATTLE && !game->attacked &&
        game->round == GAME_ROUND_ACTIVE && game->target != GAME_TARGET_NONE;
    uint32_t age = now - game->target_since_ms;
    uint32_t target_ms = target_active && age < GAME_TARGET_PERIOD_MS ? GAME_TARGET_PERIOD_MS - age : 0u;
    const char *mode = game->mode == GAME_STANDBY ? "standby" : terminated ? "terminated" : "battle";
    const char *round = game->round == GAME_ROUND_WON ? "won" : game->round == GAME_ROUND_LOST ? "lost" : "active";
    int count = snprintf(packet, sizeof packet,
        "{\"type\":\"snapshot\",\"id\":\"GG01\",\"seq\":%lu,\"tick\":%lu,"
        "\"mode\":\"%s\",\"attacked\":%s,\"round\":\"%s\",\"target\":\"%s\","
        "\"energy\":%u,\"lives\":%u,\"hits\":%u,\"score\":%d,"
        "\"rescue_ms\":%lu,\"target_ms\":%lu,\"valid\":%lu,\"abnormal\":%lu,\"unavailable\":%lu,"
        "\"t\":%s,\"p\":%s,\"h\":%s,\"a\":[%s,%s,%s],\"g\":[%s,%s,%s],\"m\":[%s,%s,%s],"
        "\"ir\":%s,\"mission\":%u,\"nfc\":%s,\"nfc_taps\":%lu,\"wifi\":%s,\"wifi_drop\":%lu}\n",
        (unsigned long)sequence++, (unsigned long)now, mode,
        game->attacked && !terminated ? "true" : "false", round, target_text(game->target),
        (unsigned)game->energy, (unsigned)game->lives, (unsigned)game->zombie_hits,
        (int)game->score, (unsigned long)game_rescue_remaining_ms(game, now),
        (unsigned long)target_ms, (unsigned long)(sample->valid & SENSOR_ALL),
        (unsigned long)(status.abnormal & SENSOR_ALL), (unsigned long)(status.unavailable & SENSOR_ALL),
        values[0], values[1], values[2], values[3], values[4], values[5],
        values[6], values[7], values[8], values[9], values[10], values[11],
        game->aim_locked ? "true" : "false", (unsigned)mission,
        !terminated && Nfc_Ready() ? "true" : "false", (unsigned long)Nfc_Taps(),
        Network_Connected() ? "true" : "false", (unsigned long)Network_Dropped());
    if (count > 0 && (size_t)count < sizeof packet) {
        if (final_packet) (void)Network_QueueFinal(packet, (size_t)count);
        else (void)Network_Queue(packet, (size_t)count);
    }
    last_snapshot = now;
    dirty = false;
}

void Command_Init(uint32_t now)
{
    sequence = 0;
    mission = 0;
    dirty = true;
    draining = false;
    was_connected = false;
    last_snapshot = now - 1000u;
    Network_Init(now);
    char url[150] = "";
    const NetworkConfig *config = Network_GetConfig();
    if (config && config->server_ipv4[0] != 0u)
        (void)snprintf(url, sizeof url, "http://%u.%u.%u.%u:%u/drone/GG01",
            config->server_ipv4[0], config->server_ipv4[1],
            config->server_ipv4[2], config->server_ipv4[3], (unsigned)APP_COMMAND_HTTP_PORT);
    Nfc_Init(url, now);
}

void Command_Poll(const Game *game, const SensorSample *sample,
                  TelemetryStatus status, uint32_t now)
{
    if (draining || !game || !sample) return;
    Network_Poll(now);
    bool connected = Network_Connected();
    if (connected && !was_connected) dirty = true;
    was_connected = connected;
    uint8_t requested;
    if (Network_ReadMission(&requested)) {
        bool accepted = game->mode == GAME_STANDBY && requested <= 2u;
        if (accepted) {
            mission = requested;
            (void)Nfc_SetMission(mission);
        }
        static const char *const mission_names[] = {
            "Orchard Perimeter", "Evacuation Corridor", "Greenhouse Shelter"
        };
        char notice[100];
        if (accepted)
            (void)snprintf(notice, sizeof notice, "MISSION ASSIGNED: %s\r\n", mission_names[mission]);
        else
            (void)snprintf(notice, sizeof notice, "MISSION REJECTED: return to standby first\r\n");
        (void)Board_UartWrite(notice);
        queue_event(accepted ? "mission_assigned" : "mission_rejected", now,
                    game->energy, game->score, game->target);
        dirty = true;
    }
    Nfc_Poll(now, game->mode == GAME_STANDBY);
    uint32_t tap_ms;
    if (Nfc_PopTap(&tap_ms)) {
        char notice[80];
        (void)snprintf(notice, sizeof notice, "NFC CHECK-IN: field technician contact #%lu\r\n",
            (unsigned long)Nfc_Taps());
        (void)Board_UartWrite(notice);
        External_Play(EXTERNAL_SOUND_CHARGE, now);
        queue_event("nfc_tap", tap_ms, game->energy, game->score, game->target);
        dirty = true;
    }
    if (dirty || (uint32_t)(now - last_snapshot) >= 1000u)
        snapshot(game, sample, status, now, false);
}

void Command_Final(const Game *game, const SensorSample *sample,
                   TelemetryStatus status, uint32_t now)
{
    if (draining) return;
    snapshot(game, sample, status, now, true);
    draining = true;
    Network_BeginDrain();
    Nfc_BeginStop();
}
void Command_Finish(uint32_t now)
{
    if (!draining) return;
    Nfc_FinishStop(now);
    if (Network_Connected() && !Network_Idle()) Network_Poll(now);
}
bool Command_Idle(void) { return (!Network_Connected() || Network_Idle()) && Nfc_StopDone(); }
void Command_Stop(void) { Network_Stop(); }
