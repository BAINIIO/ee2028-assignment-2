#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>
#include "button.h"
#include "game_config.h"
#include "sensor_sample.h"

typedef enum { GAME_STANDBY = 0, GAME_BATTLE, GAME_TERMINATED } GameMode;
typedef enum { GAME_TARGET_NONE = 0, GAME_TARGET_ZOMBIE, GAME_TARGET_HUMAN } GameTarget;
typedef enum { GAME_ROUND_ACTIVE = 0, GAME_ROUND_WON, GAME_ROUND_LOST } GameRound;
typedef enum { GAME_COLOUR_UNKNOWN = 0, GAME_COLOUR_RED, GAME_COLOUR_GREEN } GameColour;

typedef enum {
    GAME_EVENT_STANDBY = 0,
    GAME_EVENT_BATTLE,
    GAME_EVENT_CHARGED,
    GAME_EVENT_FIRED,
    GAME_EVENT_HIT,
    GAME_EVENT_HUMAN_HIT,
    GAME_EVENT_HUMAN_SAVED,
    GAME_EVENT_MISSED,
    GAME_EVENT_TARGET,
    GAME_EVENT_COLOUR_MISMATCH,
    GAME_EVENT_ATTACKED,
    GAME_EVENT_RESCUED,
    GAME_EVENT_VICTORY,
    GAME_EVENT_DEFEAT,
    GAME_EVENT_TERMINATED
} GameEventType;

/* Event values are snapshots, including a shot's target before it changes. */
typedef struct {
    GameEventType type;
    uint32_t time_ms;
    uint8_t energy;
    int16_t score;
    GameTarget target;
} GameEvent;

/* Public state is read-only to callers; initialize/change via functions below. */
typedef struct {
    GameMode mode;
    GameRound round;
    GameTarget target;
    GameColour colour;
    bool aim_locked;
    bool attacked;
    uint8_t energy;
    uint8_t lives;
    uint8_t zombie_hits;
    int16_t score;
    uint32_t mode_since_ms;
    uint32_t attacked_since_ms;
    uint32_t target_since_ms;

    /* Internal state: no dynamic allocation, no HAL dependencies. */
    uint32_t random_state;
    float upright_reference[3];
    float reference_norm_sq;
    float rotation_degrees;
    uint32_t motion_since_ms;
    uint32_t inverted_since_ms;
    bool has_reference;
    bool has_motion_time;
    bool inversion_pending;
    bool attack_armed;
    GameEvent events[GAME_EVENT_CAPACITY];
    uint8_t event_read;
    uint8_t event_count;
    uint32_t dropped_events;
} Game;

void game_init(Game *game, uint32_t now_ms);
void game_tick(Game *game, uint32_t now_ms);
void game_button(Game *game, ButtonEvent event, uint32_t now_ms);
void game_update_sensors(Game *game, const SensorSample *sample, uint32_t now_ms);
/* Optional colour is an external confirmation of the displayed target. A
 * mismatch gives a warning; OLED-generated targets remain authoritative so
 * installing/removing a colour module never changes the mandatory controls. */
void game_set_colour(Game *game, GameColour colour);
/* Optional reflective IR presence gives a zombie-hit bonus. It never decides
 * red/green identity and never gates the compulsory automatic firing. */
void game_set_aim_locked(Game *game, bool locked);
bool game_led(const Game *game, uint32_t now_ms);
uint32_t game_rescue_remaining_ms(const Game *game, uint32_t now_ms);
bool game_pop_event(Game *game, GameEvent *event);

#endif
