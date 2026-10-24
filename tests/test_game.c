#include "button.h"
#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static unsigned drain_type(Game *game, GameEventType type)
{
    GameEvent event;
    unsigned count = 0;
    while (game_pop_event(game, &event)) {
        if (event.type == type) ++count;
    }
    return count;
}

static SensorSample motion(float x, float y, float z, float gx, float gy, float gz)
{
    SensorSample sample;
    memset(&sample, 0, sizeof(sample));
    sample.valid = SENSOR_ACCELEROMETER | SENSOR_GYROSCOPE;
    sample.acceleration_ms2[0] = x;
    sample.acceleration_ms2[1] = y;
    sample.acceleration_ms2[2] = z;
    sample.angular_rate_dps[0] = gx;
    sample.angular_rate_dps[1] = gy;
    sample.angular_rate_dps[2] = gz;
    return sample;
}

static uint32_t attack(Game *game, uint32_t base)
{
    SensorSample sample = motion(0, 0, 9.81f, 0, 0, 0);
    unsigned i;
    game_update_sensors(game, &sample, base);
    if (game->mode == GAME_STANDBY) game_button(game, BUTTON_DOUBLE, base);
    sample = motion(9.81f, 0, 0, 300, 0, 0);
    for (i = 1; i <= 6; ++i) game_update_sensors(game, &sample, base + 50u * i);
    sample = motion(0, 0, -9.81f, 0, 0, 0);
    for (i = 7; i < 14; ++i) {
        game_update_sensors(game, &sample, base + 50u * i);
        CHECK(!game->attacked);
    }
    game_update_sensors(game, &sample, base + 700u);
    CHECK(game->attacked);
    CHECK(!game->attack_armed);
    CHECK(game->attacked_since_ms == base + 700u);
    return base + 700u;
}

static void test_button_classification(void)
{
    Button button;
    button_init(&button, 0);
    CHECK(button_press(&button, 0) == BUTTON_NONE);
    CHECK(button_press(&button, 10) == BUTTON_NONE);
    CHECK(button_press(&button, 29) == BUTTON_NONE);
    CHECK(button_press(&button, 30) == BUTTON_DOUBLE);
    CHECK(button.event_ms == 30);
    CHECK(button_poll(&button, 1000) == BUTTON_NONE);

    button_init(&button, 0);
    CHECK(button_press(&button, 100) == BUTTON_NONE);
    CHECK(button_poll(&button, 599) == BUTTON_NONE);
    CHECK(button_press(&button, 599) == BUTTON_DOUBLE);
    CHECK(button.event_ms == 599);

    button_init(&button, 0);
    CHECK(button_press(&button, 0) == BUTTON_NONE);
    CHECK(button_press(&button, 500) == BUTTON_SINGLE);
    CHECK(button.event_ms == 500);
    CHECK(button_poll(&button, 999) == BUTTON_NONE);
    CHECK(button_poll(&button, 1500) == BUTTON_SINGLE);
    CHECK(button.event_ms == 1000);
    CHECK(button_poll(&button, 1501) == BUTTON_NONE);

    button_init(&button, UINT32_MAX - 100u);
    CHECK(button_press(&button, UINT32_MAX - 100u) == BUTTON_NONE);
    CHECK(button_press(&button, 100u) == BUTTON_DOUBLE);
    CHECK(button.event_ms == 100u);
    button_init(&button, UINT32_MAX - 100u);
    CHECK(button_press(&button, UINT32_MAX - 100u) == BUTTON_NONE);
    CHECK(button_poll(&button, 398u) == BUTTON_NONE);
    CHECK(button_poll(&button, 399u) == BUTTON_SINGLE);
    CHECK(button.event_ms == 399u);
}

static void apply_event(Game *game, Button *button, ButtonEvent event)
{
    if (event != BUTTON_NONE) game_button(game, event, button->event_ms);
}

static void test_button_game_integration(void)
{
    Game game;
    Button button;
    game_init(&game, 0);
    button_init(&button, 0);
    CHECK(game.mode == GAME_STANDBY);
    CHECK(game_led(&game, 0));
    CHECK(drain_type(&game, GAME_EVENT_STANDBY) == 1);

    apply_event(&game, &button, button_press(&button, 0));
    apply_event(&game, &button, button_poll(&button, 500));
    CHECK(game.energy == 0);
    CHECK(game.mode == GAME_STANDBY);
    apply_event(&game, &button, button_press(&button, 1000));
    CHECK(game.mode == GAME_STANDBY);
    apply_event(&game, &button, button_press(&button, 1499));
    CHECK(game.mode == GAME_BATTLE);
    CHECK(game.energy == 0);
    CHECK(drain_type(&game, GAME_EVENT_CHARGED) == 0);

    apply_event(&game, &button, button_press(&button, 2000));
    apply_event(&game, &button, button_poll(&button, 2499));
    CHECK(game.energy == 0);
    apply_event(&game, &button, button_poll(&button, 2500));
    CHECK(game.energy == 3);
    apply_event(&game, &button, button_press(&button, 3000));
    apply_event(&game, &button, button_poll(&button, 3500));
    CHECK(game.energy == 1); /* 3 + 3 - 5: fire is automatic. */
    CHECK(drain_type(&game, GAME_EVENT_FIRED) == 1);

    apply_event(&game, &button, button_press(&button, 4000));
    apply_event(&game, &button, button_press(&button, 4200));
    CHECK(game.mode == GAME_STANDBY);
    CHECK(game.energy == 1); /* A double never charges its first press. */
}

static void test_energy_and_led(void)
{
    Game game;
    uint32_t start = UINT32_MAX - 400u;
    game_init(&game, start);
    game_button(&game, BUTTON_DOUBLE, start);
    CHECK(game_led(&game, start));
    CHECK(game_led(&game, start + 499u));
    CHECK(!game_led(&game, start + 500u));
    CHECK(game_led(&game, start + 1000u));
    game_button(&game, BUTTON_SINGLE, start + 500u);
    CHECK(game.energy == 3);
    game_button(&game, BUTTON_SINGLE, start + 1000u);
    CHECK(game.energy == 1);
    game_button(&game, BUTTON_SINGLE, start + 1500u);
    CHECK(game.energy == 4);
    game_button(&game, BUTTON_SINGLE, start + 2000u);
    CHECK(game.energy == 2);
    game_button(&game, BUTTON_SINGLE, start + 2500u);
    CHECK(game.energy == 0);
    CHECK(drain_type(&game, GAME_EVENT_FIRED) == 3);
    /* The cap is defensive: ordinary automatic-fire operation never leaves
     * >=5 stored energy. Inject high initial energy to verify that cap. */
    game.energy = 9;
    game_button(&game, BUTTON_SINGLE, start + 3000u);
    CHECK(game.energy == 5);
}

static void test_rescue_deadline_and_terminal(void)
{
    Game game;
    Game before;
    GameEvent event;
    SensorSample sample = motion(0, 0, 9.81f, 0, 0, 0);
    uint32_t attacked_at;
    game_init(&game, 0);
    attacked_at = attack(&game, 0);
    CHECK(game_led(&game, attacked_at));
    CHECK(!game_led(&game, attacked_at + 250u));
    CHECK(game_led(&game, attacked_at + 500u));
    CHECK(game_rescue_remaining_ms(&game, attacked_at + 9999u) == 1);
    game_button(&game, BUTTON_SINGLE, attacked_at + 50u);
    CHECK(game.energy == 0);
    game_button(&game, BUTTON_DOUBLE, attacked_at + 9999u);
    CHECK(game.mode == GAME_BATTLE);
    CHECK(!game.attacked);
    CHECK(!game.attack_armed);
    CHECK(drain_type(&game, GAME_EVENT_RESCUED) == 1);

    game_init(&game, 0);
    attacked_at = attack(&game, 0);
    game_button(&game, BUTTON_DOUBLE, attacked_at + 10000u);
    CHECK(game.mode == GAME_TERMINATED);
    CHECK(game.round == GAME_ROUND_LOST);
    CHECK(!game_led(&game, attacked_at + 10000u));
    CHECK(game_rescue_remaining_ms(&game, attacked_at + 10000u) == 0);
    CHECK(game_pop_event(&game, &event));
    CHECK(event.type == GAME_EVENT_DEFEAT);
    CHECK(game_pop_event(&game, &event));
    CHECK(event.type == GAME_EVENT_TERMINATED);
    CHECK(!game_pop_event(&game, &event));
    before = game;
    game_button(&game, BUTTON_DOUBLE, attacked_at + 11000u);
    game_button(&game, BUTTON_SINGLE, attacked_at + 12000u);
    game_tick(&game, attacked_at + 13000u);
    game_update_sensors(&game, &sample, attacked_at + 14000u);
    game_set_colour(&game, GAME_COLOUR_RED);
    game_set_aim_locked(&game, true);
    CHECK(memcmp(&before, &game, sizeof(game)) == 0);

    game_init(&game, UINT32_MAX - 5000u);
    attacked_at = attack(&game, UINT32_MAX - 5000u);
    game_tick(&game, attacked_at + 9999u);
    CHECK(game.mode == GAME_BATTLE);
    game_tick(&game, attacked_at + 10000u);
    CHECK(game.mode == GAME_TERMINATED);
}

static void test_timestamped_rescue(void)
{
    Game game;
    Button button;
    uint32_t attacked_at;
    ButtonEvent event;
    game_init(&game, 0);
    button_init(&button, 0);
    attacked_at = attack(&game, 0);
    event = button_press(&button, attacked_at + 9700u);
    CHECK(event == BUTTON_NONE);
    event = button_press(&button, attacked_at + 9999u);
    CHECK(event == BUTTON_DOUBLE);
    /* Foreground may run after the deadline: process captured edges first,
     * using event_ms, so a physical rescue at 9999 ms is still accepted. */
    apply_event(&game, &button, event);
    game_tick(&game, attacked_at + 10050u);
    CHECK(game.mode == GAME_BATTLE && !game.attacked);

    game_init(&game, 0);
    button_init(&button, 0);
    attacked_at = attack(&game, 0);
    (void)button_press(&button, attacked_at + 9700u);
    event = button_press(&button, attacked_at + 10000u);
    apply_event(&game, &button, event);
    CHECK(game.mode == GAME_TERMINATED);
}

static void test_historical_input(void)
{
    Game game;
    Game before;
    uint32_t attacked_at;
    unsigned i;
    const uint32_t starts[] = {0u, UINT32_MAX - 500u};
    for (i = 0; i < sizeof starts / sizeof starts[0]; ++i) {
        game_init(&game, starts[i]);
        attacked_at = attack(&game, starts[i]);
        (void)drain_type(&game, GAME_EVENT_ATTACKED);
        before = game;
        /* An edge captured just before the sensor read detected attack can
         * reach the foreground afterward. It cannot expire or rescue a state
         * that had not started at its physical timestamp. */
        game_button(&game, BUTTON_DOUBLE, attacked_at - 1u);
        CHECK(memcmp(&before, &game, sizeof(game)) == 0);
        game_button(&game, BUTTON_SINGLE, attacked_at - 1u);
        CHECK(memcmp(&before, &game, sizeof(game)) == 0);
        game_tick(&game, attacked_at - 1u);
        CHECK(memcmp(&before, &game, sizeof(game)) == 0);
        game_button(&game, BUTTON_DOUBLE, attacked_at + 1u);
        CHECK(game.mode == GAME_BATTLE && !game.attacked);
    }
    game_init(&game, 1000);
    game_button(&game, BUTTON_DOUBLE, 1200);
    before = game;
    game_button(&game, BUTTON_DOUBLE, 1100); /* Old standby-era command. */
    CHECK(memcmp(&before, &game, sizeof(game)) == 0);
    game_tick(&game, 5200);
    before = game;
    game_button(&game, BUTTON_SINGLE, 5199); /* Target has since changed. */
    CHECK(memcmp(&before, &game, sizeof(game)) == 0);
}

static void test_motion_and_rearming(void)
{
    Game game;
    SensorSample sample;
    uint32_t attacked_at;
    uint32_t now;
    unsigned i;
    game_init(&game, 0);
    sample = motion(0, 0, 9.81f, 0, 0, 0);
    sample.valid = SENSOR_ACCELEROMETER;
    game_update_sensors(&game, &sample, 0);
    CHECK(!game.has_reference); /* Gyro validity is mandatory. */
    sample = motion(0, 0, 9.81f, NAN, 0, 0);
    game_update_sensors(&game, &sample, 50);
    CHECK(!game.has_reference);
    sample = motion(0, 0, 9.81f, 100, 0, 0);
    game_update_sensors(&game, &sample, 100);
    CHECK(!game.has_reference); /* Don't calibrate a moving board. */
    sample = motion(0, 0, 9.81f, 0, 0, 0);
    game_update_sensors(&game, &sample, 150);
    CHECK(game.has_reference);
    game_button(&game, BUTTON_DOUBLE, 150);
    sample = motion(0, 0, -9.81f, 0, 0, 0);
    for (now = 200; now <= 700; now += 50) game_update_sensors(&game, &sample, now);
    CHECK(!game.attacked); /* An instantaneous accel sign flip lacks gyro support. */
    sample.angular_rate_dps[0] = 9000;
    game_update_sensors(&game, &sample, 750);
    CHECK(!game.attacked);
    CHECK(!game.has_motion_time);

    game_init(&game, 0);
    attacked_at = attack(&game, 0);
    game_button(&game, BUTTON_DOUBLE, attacked_at + 100u);
    sample = motion(0, 0, -9.81f, 300, 0, 0);
    for (i = 3; i <= 20; ++i) game_update_sensors(&game, &sample, attacked_at + 50u * i);
    CHECK(!game.attacked); /* Remaining inverted after rescue cannot retrigger. */
    sample = motion(0, 0, 9.81f, 0, 0, 0);
    now = attacked_at + 1050u;
    game_update_sensors(&game, &sample, now);
    CHECK(game.attack_armed);
    sample = motion(9.81f, 0, 0, 300, 0, 0);
    for (i = 1; i <= 6; ++i) game_update_sensors(&game, &sample, now + 50u * i);
    sample = motion(0, 0, -9.81f, 0, 0, 0);
    for (i = 7; i <= 14; ++i) game_update_sensors(&game, &sample, now + 50u * i);
    CHECK(game.attacked);

    /* A long sensor outage breaks the continuous-upside-down qualification. */
    game_init(&game, 0);
    (void)attack(&game, 0);
    game_button(&game, BUTTON_DOUBLE, 800);
    sample = motion(0, 0, 9.81f, 0, 0, 0);
    game_update_sensors(&game, &sample, 850);
    sample = motion(9.81f, 0, 0, 300, 0, 0);
    for (i = 1; i <= 6; ++i) game_update_sensors(&game, &sample, 850 + 50u * i);
    sample = motion(0, 0, -9.81f, 0, 0, 0);
    game_update_sensors(&game, &sample, 1200);
    CHECK(game.inversion_pending);
    game_update_sensors(&game, &sample, 1800);
    CHECK(!game.inversion_pending && !game.attacked);
}

static uint32_t play_to_victory(Game *game)
{
    uint32_t now = 0;
    unsigned iterations = 0;
    game_init(game, now);
    game_button(game, BUTTON_DOUBLE, now);
    game_set_aim_locked(game, true);
    while (game->round == GAME_ROUND_ACTIVE && ++iterations < 100u) {
        if (game->target == GAME_TARGET_HUMAN) {
            now += GAME_TARGET_PERIOD_MS;
            game_tick(game, now);
        } else {
            now += 600;
            game_button(game, BUTTON_SINGLE, now);
        }
    }
    CHECK(iterations < 100u);
    CHECK(game->round == GAME_ROUND_WON);
    CHECK(game->zombie_hits == GAME_WIN_HITS);
    CHECK(game->score >= (GAME_ZOMBIE_POINTS + GAME_AIM_BONUS_POINTS) * (int)GAME_WIN_HITS);
    CHECK(game->lives == GAME_INITIAL_LIVES);
    CHECK(game->mode == GAME_BATTLE);
    CHECK(game->target == GAME_TARGET_NONE);
    CHECK(drain_type(game, GAME_EVENT_VICTORY) == 1);
    return now;
}

static void test_round_results_and_optional_sensors(void)
{
    Game game;
    Game plain;
    GameEvent event;
    uint32_t now = play_to_victory(&game);
    uint8_t expected_energy;
    int16_t final_score = game.score;
    unsigned i;
    expected_energy = (uint8_t)(game.energy + GAME_CHARGE_ENERGY);
    if (expected_energy >= GAME_SHOT_ENERGY) expected_energy -= GAME_SHOT_ENERGY;
    game_button(&game, BUTTON_SINGLE, now + 600);
    CHECK(game.energy == expected_energy);
    CHECK(game.score == final_score);
    CHECK(game.mode == GAME_BATTLE);
    game_button(&game, BUTTON_DOUBLE, now + 800);
    CHECK(game.mode == GAME_STANDBY);
    game_button(&game, BUTTON_DOUBLE, now + 1000);
    CHECK(game.round == GAME_ROUND_ACTIVE);
    CHECK(game.score == 0 && game.lives == GAME_INITIAL_LIVES);

    game_init(&game, 0);
    game_button(&game, BUTTON_DOUBLE, 0);
    now = 0;
    /* Shoot every target; eventually humans cost all three lives. */
    for (i = 0; i < 200 && game.round == GAME_ROUND_ACTIVE; ++i) {
        now += 600;
        game_button(&game, BUTTON_SINGLE, now);
    }
    CHECK(i < 200);
    CHECK(game.round == GAME_ROUND_LOST);
    CHECK(game.mode == GAME_BATTLE);
    CHECK(game.lives == 0);
    CHECK(drain_type(&game, GAME_EVENT_DEFEAT) == 1);
    game_button(&game, BUTTON_DOUBLE, now + 100);
    CHECK(game.mode == GAME_STANDBY);

    /* Colour mismatches warn while the displayed target determines points.
     * No sensor is needed to shoot; IR aim adds points without gating fire. */
    game_init(&game, 0);
    game_button(&game, BUTTON_DOUBLE, 0);
    while (game.target == GAME_TARGET_HUMAN) game_tick(&game, game.target_since_ms + 4000u);
    now = game.target_since_ms;
    plain = game;
    game_set_colour(&game, GAME_COLOUR_GREEN);
    game_set_aim_locked(&game, true);
    for (i = 1; i <= 2; ++i) {
        game_button(&game, BUTTON_SINGLE, now + i * 600u);
        game_button(&plain, BUTTON_SINGLE, now + i * 600u);
    }
    CHECK(game.zombie_hits == 1 && plain.zombie_hits == 1);
    CHECK(game.energy == 1 && plain.energy == 1);
    CHECK(game.score == plain.score + GAME_AIM_BONUS_POINTS);
    CHECK(drain_type(&game, GAME_EVENT_COLOUR_MISMATCH) == 1);
    CHECK(!game_pop_event(&game, &event));
}

static void test_target_timeout_and_determinism(void)
{
    Game first;
    Game second;
    uint32_t now = 0;
    unsigned i;
    game_init(&first, now);
    game_init(&second, now);
    game_button(&first, BUTTON_DOUBLE, now);
    game_button(&second, BUTTON_DOUBLE, now);
    CHECK(first.target == second.target);
    for (i = 0; i < 30 && first.round == GAME_ROUND_ACTIVE; ++i) {
        GameTarget old_target = first.target;
        uint8_t old_lives = first.lives;
        int16_t old_score = first.score;
        now += GAME_TARGET_PERIOD_MS;
        game_tick(&first, now - 1u);
        CHECK(first.score == old_score && first.lives == old_lives);
        game_tick(&first, now);
        game_tick(&second, now);
        CHECK(first.target == second.target);
        if (old_target == GAME_TARGET_HUMAN) {
            CHECK(first.lives == old_lives);
            CHECK(first.score == old_score + GAME_HUMAN_SAVED_POINTS);
        } else {
            CHECK(first.lives == old_lives - 1u);
            CHECK(first.score == old_score - GAME_PENALTY_POINTS);
        }
    }
    CHECK(first.round == GAME_ROUND_LOST);
    CHECK(first.mode == GAME_BATTLE);
}

int main(void)
{
    test_button_classification();
    test_button_game_integration();
    test_energy_and_led();
    test_rescue_deadline_and_terminal();
    test_timestamped_rescue();
    test_historical_input();
    test_motion_and_rearming();
    test_round_results_and_optional_sensors();
    test_target_timeout_and_determinism();
    printf("PASS: %u game/button checks\n", checks);
    return EXIT_SUCCESS;
}
