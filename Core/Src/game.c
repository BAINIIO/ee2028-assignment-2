#include "game.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Timestamps may be captured in EXTI while an I2C read is in progress, then
 * delivered after that read creates a new state. A historical edge must not
 * be mistaken for almost 2^32 ms elapsed. Ordering is unambiguous when events
 * and state boundaries are separated by less than 2^31 ms (24.8 days). */
static bool time_precedes(uint32_t candidate, uint32_t boundary)
{
    return (int32_t)(candidate - boundary) < 0;
}

static void emit(Game *game, GameEventType type, uint32_t now_ms)
{
    uint8_t index;
    if (game->event_count == GAME_EVENT_CAPACITY) {
        /* Prefer the latest state, particularly the terminal event. The main
         * loop drains every iteration; dropped_events exposes a stalled loop. */
        game->event_read = (uint8_t)((game->event_read + 1u) % GAME_EVENT_CAPACITY);
        --game->event_count;
        ++game->dropped_events;
    }
    index = (uint8_t)((game->event_read + game->event_count) % GAME_EVENT_CAPACITY);
    game->events[index].type = type;
    game->events[index].time_ms = now_ms;
    game->events[index].energy = game->energy;
    game->events[index].score = game->score;
    game->events[index].target = game->target;
    ++game->event_count;
}

bool game_pop_event(Game *game, GameEvent *event)
{
    if (game->event_count == 0u || event == NULL) {
        return false;
    }
    *event = game->events[game->event_read];
    game->event_read = (uint8_t)((game->event_read + 1u) % GAME_EVENT_CAPACITY);
    --game->event_count;
    return true;
}

static void add_score(Game *game, int points)
{
    int value = (int)game->score + points;
    if (value > INT16_MAX) value = INT16_MAX;
    if (value < INT16_MIN) value = INT16_MIN;
    game->score = (int16_t)value;
}

static void next_target(Game *game, uint32_t now_ms)
{
    game->random_state = game->random_state * 1664525u + 1013904223u;
    /* Upper bits avoid the alternating low bit of a linear congruential RNG.
     * Two thirds of targets are zombies. This is a reproducible game sequence,
     * not a security or statistically scientific random number generator. */
    game->target = ((game->random_state >> 16) % 3u == 0u)
        ? GAME_TARGET_HUMAN : GAME_TARGET_ZOMBIE;
    game->target_since_ms = now_ms;
    emit(game, GAME_EVENT_TARGET, now_ms);
}

static void finish_round_if_needed(Game *game, uint32_t now_ms)
{
    if (game->lives == 0u) {
        game->round = GAME_ROUND_LOST;
        game->target = GAME_TARGET_NONE;
        emit(game, GAME_EVENT_DEFEAT, now_ms);
    } else if (game->zombie_hits >= GAME_WIN_HITS) {
        game->round = GAME_ROUND_WON;
        game->target = GAME_TARGET_NONE;
        emit(game, GAME_EVENT_VICTORY, now_ms);
    } else {
        next_target(game, now_ms);
    }
}

static void enter_battle(Game *game, uint32_t now_ms)
{
    game->mode = GAME_BATTLE;
    game->mode_since_ms = now_ms;
    game->round = GAME_ROUND_ACTIVE;
    game->score = 0;
    game->lives = GAME_INITIAL_LIVES;
    game->zombie_hits = 0;
    game->attacked = false;
    game->inversion_pending = false;
    emit(game, GAME_EVENT_BATTLE, now_ms);
    next_target(game, now_ms);
}

void game_init(Game *game, uint32_t now_ms)
{
    memset(game, 0, sizeof(*game));
    game->mode = GAME_STANDBY;
    game->round = GAME_ROUND_ACTIVE;
    game->lives = GAME_INITIAL_LIVES;
    game->mode_since_ms = now_ms;
    game->random_state = 0x2028a11eu;
    game->attack_armed = true;
    emit(game, GAME_EVENT_STANDBY, now_ms);
}

void game_tick(Game *game, uint32_t now_ms)
{
    if (game->mode != GAME_BATTLE) return;

    if (game->attacked) {
        if (time_precedes(now_ms, game->attacked_since_ms)) return;
        if ((uint32_t)(now_ms - game->attacked_since_ms) >= APP_RESCUE_MS) {
            game->mode = GAME_TERMINATED;
            game->round = GAME_ROUND_LOST;
            game->target = GAME_TARGET_NONE;
            /* Drop queued pre-termination chatter; these are the only final
             * notifications. Board code emits its final line then silences
             * UART and stops acquisition/interrupts until a physical reset. */
            game->event_read = 0;
            game->event_count = 0;
            emit(game, GAME_EVENT_DEFEAT, now_ms);
            emit(game, GAME_EVENT_TERMINATED, now_ms);
        }
        return;
    }

    if (game->round == GAME_ROUND_ACTIVE &&
        !time_precedes(now_ms, game->target_since_ms) &&
        (uint32_t)(now_ms - game->target_since_ms) >= GAME_TARGET_PERIOD_MS) {
        if (game->target == GAME_TARGET_HUMAN) {
            add_score(game, GAME_HUMAN_SAVED_POINTS);
            emit(game, GAME_EVENT_HUMAN_SAVED, now_ms);
        } else {
            if (game->lives > 0u) --game->lives;
            add_score(game, -GAME_PENALTY_POINTS);
            emit(game, GAME_EVENT_MISSED, now_ms);
        }
        finish_round_if_needed(game, now_ms);
    }
}

void game_button(Game *game, ButtonEvent event, uint32_t now_ms)
{
    /* Contextual commands cannot be replayed into a newer mode/attack/target.
     * Main drains the EXTI queue before fresh sensor updates, but an edge can
     * still arrive between the drain and an attacked-state transition. */
    if (time_precedes(now_ms, game->mode_since_ms) ||
        (game->attacked && time_precedes(now_ms, game->attacked_since_ms)) ||
        (game->mode == GAME_BATTLE && game->round == GAME_ROUND_ACTIVE &&
         time_precedes(now_ms, game->target_since_ms))) return;
    game_tick(game, now_ms);
    if (game->mode == GAME_TERMINATED || event == BUTTON_NONE) return;

    if (event == BUTTON_DOUBLE) {
        if (game->mode == GAME_STANDBY) {
            enter_battle(game, now_ms);
        } else if (game->attacked) {
            game->attacked = false;
            game->mode_since_ms = now_ms;
            game->target_since_ms = now_ms;
            game->inversion_pending = false;
            game->rotation_degrees = 0.0f;
            emit(game, GAME_EVENT_RESCUED, now_ms);
        } else {
            game->mode = GAME_STANDBY;
            game->mode_since_ms = now_ms;
            game->target = GAME_TARGET_NONE;
            game->inversion_pending = false;
            emit(game, GAME_EVENT_STANDBY, now_ms);
        }
        return;
    }

    if (game->mode != GAME_BATTLE || game->attacked) return;
    game->energy = (uint8_t)(game->energy + GAME_CHARGE_ENERGY);
    if (game->energy > GAME_ENERGY_CAPACITY) game->energy = GAME_ENERGY_CAPACITY;
    emit(game, GAME_EVENT_CHARGED, now_ms);
    if (game->energy < GAME_SHOT_ENERGY) return;

    game->energy = (uint8_t)(game->energy - GAME_SHOT_ENERGY);
    emit(game, GAME_EVENT_FIRED, now_ms);
    /* Mandatory auto-fire continues after a round result. A fresh round starts
     * by returning to standby then entering battle again. */
    if (game->round != GAME_ROUND_ACTIVE) return;

    if ((game->colour == GAME_COLOUR_RED && game->target == GAME_TARGET_HUMAN) ||
        (game->colour == GAME_COLOUR_GREEN && game->target == GAME_TARGET_ZOMBIE)) {
        emit(game, GAME_EVENT_COLOUR_MISMATCH, now_ms);
    }
    if (game->target == GAME_TARGET_ZOMBIE) {
        ++game->zombie_hits;
        add_score(game, GAME_ZOMBIE_POINTS);
        if (game->aim_locked) add_score(game, GAME_AIM_BONUS_POINTS);
        emit(game, GAME_EVENT_HIT, now_ms);
    } else {
        if (game->lives > 0u) --game->lives;
        add_score(game, -GAME_PENALTY_POINTS);
        emit(game, GAME_EVENT_HUMAN_HIT, now_ms);
    }
    finish_round_if_needed(game, now_ms);
}

void game_set_colour(Game *game, GameColour colour)
{
    if (game->mode != GAME_TERMINATED) {
        game->colour = (colour == GAME_COLOUR_RED || colour == GAME_COLOUR_GREEN)
            ? colour : GAME_COLOUR_UNKNOWN;
    }
}

void game_set_aim_locked(Game *game, bool locked)
{
    if (game->mode != GAME_TERMINATED) game->aim_locked = locked;
}

uint32_t game_rescue_remaining_ms(const Game *game, uint32_t now_ms)
{
    uint32_t elapsed;
    if (!game->attacked || game->mode != GAME_BATTLE) return 0;
    elapsed = (uint32_t)(now_ms - game->attacked_since_ms);
    return elapsed >= APP_RESCUE_MS ? 0u : APP_RESCUE_MS - elapsed;
}

bool game_led(const Game *game, uint32_t now_ms)
{
    uint32_t origin;
    uint32_t half_period;
    if (game->mode == GAME_STANDBY) return true;
    if (game->mode == GAME_TERMINATED) return false;
    origin = game->attacked ? game->attacked_since_ms : game->mode_since_ms;
    half_period = game->attacked ? 250u : 500u;
    return (((uint32_t)(now_ms - origin) / half_period) & 1u) == 0u;
}

static bool vector_finite(const float vector[3])
{
    return isfinite(vector[0]) && isfinite(vector[1]) && isfinite(vector[2]);
}

static float norm_squared(const float vector[3])
{
    return vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2];
}

void game_update_sensors(Game *game, const SensorSample *sample, uint32_t now_ms)
{
    float acceleration_norm_sq;
    float gyro_norm_sq;
    float dot;
    float norm_product;
    uint32_t elapsed;
    bool upright;
    bool inverted;
    const uint32_t motion_mask = SENSOR_ACCELEROMETER | SENSOR_GYROSCOPE;

    game_tick(game, now_ms);
    if (game->mode == GAME_TERMINATED || sample == NULL) return;
    if ((sample->valid & motion_mask) != motion_mask ||
        !vector_finite(sample->acceleration_ms2) ||
        !vector_finite(sample->angular_rate_dps)) {
        game->inversion_pending = false;
        game->has_motion_time = false;
        return;
    }
    acceleration_norm_sq = norm_squared(sample->acceleration_ms2);
    gyro_norm_sq = norm_squared(sample->angular_rate_dps);
    if (acceleration_norm_sq < GAME_GRAVITY_MIN_MS2 * GAME_GRAVITY_MIN_MS2 ||
        acceleration_norm_sq > GAME_GRAVITY_MAX_MS2 * GAME_GRAVITY_MAX_MS2 ||
        gyro_norm_sq > GAME_GYRO_PHYSICAL_MAX_DPS * GAME_GYRO_PHYSICAL_MAX_DPS) {
        game->inversion_pending = false;
        game->has_motion_time = false;
        return;
    }

    if (!game->has_reference) {
        if (gyro_norm_sq <= GAME_REFERENCE_MAX_DPS * GAME_REFERENCE_MAX_DPS) {
            memcpy(game->upright_reference, sample->acceleration_ms2,
                   sizeof(game->upright_reference));
            game->reference_norm_sq = acceleration_norm_sq;
            game->has_reference = true;
        }
        game->motion_since_ms = now_ms;
        game->has_motion_time = true;
        return;
    }

    elapsed = game->has_motion_time ? (uint32_t)(now_ms - game->motion_since_ms) : 0u;
    game->motion_since_ms = now_ms;
    game->has_motion_time = true;
    if (elapsed > GAME_MAX_MOTION_GAP_MS) {
        /* Long gaps cannot substantiate continuous inversion or rotation. */
        game->inversion_pending = false;
        game->rotation_degrees = 0.0f;
        elapsed = 0u;
    }
    game->rotation_degrees += sqrtf(gyro_norm_sq) * ((float)elapsed / 1000.0f);
    if (game->rotation_degrees > 360.0f) game->rotation_degrees = 360.0f;
    dot = sample->acceleration_ms2[0] * game->upright_reference[0] +
          sample->acceleration_ms2[1] * game->upright_reference[1] +
          sample->acceleration_ms2[2] * game->upright_reference[2];
    norm_product = game->reference_norm_sq * acceleration_norm_sq;
    upright = dot > 0.0f && dot * dot >= GAME_UPRIGHT_COSINE_SQUARED * norm_product;
    inverted = dot < 0.0f && dot * dot >= GAME_UPSIDE_COSINE_SQUARED * norm_product;

    if (upright) {
        game->inversion_pending = false;
        game->rotation_degrees = 0.0f;
        if (!game->attacked) game->attack_armed = true;
        return;
    }
    if (game->mode != GAME_BATTLE || game->attacked || !game->attack_armed ||
        !inverted || game->rotation_degrees < GAME_FLIP_MIN_DEGREES) {
        game->inversion_pending = false;
        return;
    }
    if (!game->inversion_pending) {
        game->inversion_pending = true;
        game->inverted_since_ms = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - game->inverted_since_ms) >= GAME_UPSIDE_HOLD_MS) {
        game->attacked = true;
        game->attack_armed = false;
        game->inversion_pending = false;
        game->attacked_since_ms = now_ms;
        emit(game, GAME_EVENT_ATTACKED, now_ms);
    }
}
