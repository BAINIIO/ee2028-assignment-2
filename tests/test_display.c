#include "game_display.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { FRAME_BYTES = 128 * 64 / 8, GUARD_BYTES = 32 };

typedef struct {
    uint8_t before[GUARD_BYTES];
    uint8_t pixels[FRAME_BYTES];
    uint8_t after[GUARD_BYTES];
} GuardedFrame;

static unsigned checks;

#define CHECK(condition) do { assert(condition); ++checks; } while (0)

static ExternalDisplayFrame battle_frame(void)
{
    const ExternalDisplayFrame frame = {
        .mode = "BATTLE", .status = "ZOMBIE", .hint = "1x CHARGE 2x MODE",
        .score = 25, .energy = 3, .lives = 3,
        .target = EXTERNAL_COLOUR_RED,
        .scene = EXTERNAL_SCENE_BATTLE, .now_ms = 1000,
        .target_age_ms = 1800, .target_period_ms = 4000,
        .effect_age_ms = 1000, .effect = EXTERNAL_EFFECT_NONE,
        .zombie_hits = 2, .goal_hits = 6
    };
    return frame;
}

static void render_checked(const ExternalDisplayFrame *frame,
                           uint8_t output[FRAME_BYTES], uint8_t initial)
{
    GuardedFrame guarded;
    uint8_t original[sizeof *frame];
    memcpy(original, frame, sizeof original);
    memset(&guarded, 0xa5, sizeof guarded);
    memset(guarded.pixels, initial, sizeof guarded.pixels);
    GameDisplay_Render(guarded.pixels, frame);
    for (size_t i = 0; i < GUARD_BYTES; ++i) {
        CHECK(guarded.before[i] == 0xa5);
        CHECK(guarded.after[i] == 0xa5);
    }
    CHECK(memcmp(original, frame, sizeof original) == 0);
    memcpy(output, guarded.pixels, FRAME_BYTES);
}

static unsigned lit_pixels(const uint8_t frame[FRAME_BYTES])
{
    unsigned count = 0;
    for (size_t i = 0; i < FRAME_BYTES; ++i) {
        uint8_t value = frame[i];
        while (value != 0u) {
            count += value & 1u;
            value >>= 1u;
        }
    }
    return count;
}

static bool region_differs(const uint8_t a[FRAME_BYTES],
                           const uint8_t b[FRAME_BYTES],
                           unsigned left, unsigned top,
                           unsigned right, unsigned bottom)
{
    /* Check pixel coordinates in SSD1306 page-addressed byte order. */
    for (unsigned y = top; y < bottom; ++y) {
        for (unsigned x = left; x < right; ++x) {
            const size_t index = (size_t)(y / 8u) * 128u + x;
            if (((a[index] ^ b[index]) & (1u << (y % 8u))) != 0u)
                return true;
        }
    }
    return false;
}

static void test_scenes_and_replacement(void)
{
    uint8_t scenes[6][FRAME_BYTES];
    uint8_t repeated[FRAME_BYTES];
    ExternalDisplayFrame frame = battle_frame();
    for (unsigned scene = 0; scene < 6u; ++scene) {
        frame.scene = (ExternalDisplayScene)scene;
        frame.attacked = scene == EXTERNAL_SCENE_ATTACKED;
        frame.rescue_seconds = 7;
        render_checked(&frame, scenes[scene], 0x00);
        render_checked(&frame, repeated, 0xff);
        CHECK(memcmp(scenes[scene], repeated, FRAME_BYTES) == 0);
        CHECK(lit_pixels(repeated) > 40u);
        CHECK(lit_pixels(repeated) < 128u * 64u - 40u);
        for (unsigned previous = 0; previous < scene; ++previous)
            CHECK(memcmp(scenes[scene], scenes[previous], FRAME_BYTES) != 0);
    }

    /* Visiting another scene cannot leave pixels behind in the next frame. */
    memcpy(repeated, scenes[EXTERNAL_SCENE_WON], FRAME_BYTES);
    frame = battle_frame();
    GameDisplay_Render(repeated, &frame);
    render_checked(&frame, scenes[0], 0x00);
    CHECK(memcmp(repeated, scenes[0], FRAME_BYTES) == 0);
}

static void test_animation_and_game_feedback(void)
{
    uint8_t first[FRAME_BYTES], second[FRAME_BYTES];
    ExternalDisplayFrame frame = battle_frame();

    frame.target_age_ms = 0;
    render_checked(&frame, first, 0x00);
    frame.target_age_ms = 3600;
    render_checked(&frame, second, 0x00);
    CHECK(region_differs(first, second, 36, 18, 128, 50));

    frame.target_age_ms = 1800;
    render_checked(&frame, first, 0x00);
    frame.target = EXTERNAL_COLOUR_GREEN;
    render_checked(&frame, second, 0x00);
    CHECK(region_differs(first, second, 36, 18, 128, 50));

    frame = battle_frame();
    render_checked(&frame, first, 0x00);
    frame.effect = EXTERNAL_EFFECT_SHOT;
    frame.effect_age_ms = 100;
    frame.effect_target_age_ms = frame.target_age_ms;
    frame.effect_target = frame.target;
    render_checked(&frame, second, 0x00);
    CHECK(region_differs(first, second, 10, 18, 128, 50));

    frame = battle_frame();
    render_checked(&frame, first, 0x00);
    frame.energy = 8;
    frame.lives = 1;
    frame.score = 115;
    render_checked(&frame, second, 0x00);
    CHECK(region_differs(first, second, 0, 0, 128, 18));
    CHECK(region_differs(first, second, 0, 50, 128, 64));

    frame = battle_frame();
    render_checked(&frame, first, 0x00);
    frame.aim_locked = true;
    render_checked(&frame, second, 0x00);
    CHECK(memcmp(first, second, FRAME_BYTES) != 0);
    frame.aim_locked = false;
    frame.sensor_warning = true;
    render_checked(&frame, second, 0x00);
    CHECK(memcmp(first, second, FRAME_BYTES) != 0);

    frame = battle_frame();
    frame.scene = EXTERNAL_SCENE_ATTACKED;
    frame.attacked = true;
    frame.rescue_seconds = 10;
    render_checked(&frame, first, 0x00);
    frame.rescue_seconds = 3;
    render_checked(&frame, second, 0x00);
    CHECK(memcmp(first, second, FRAME_BYTES) != 0);

    frame = battle_frame();
    frame.scene = EXTERNAL_SCENE_STANDBY;
    frame.now_ms = 0;
    render_checked(&frame, first, 0x00);
    frame.now_ms = 500;
    render_checked(&frame, second, 0x00);
    CHECK(memcmp(first, second, FRAME_BYTES) != 0);
}

static void test_extreme_inputs(void)
{
    uint8_t first[FRAME_BYTES], second[FRAME_BYTES];
    ExternalDisplayFrame frame = battle_frame();
    /* Presentation inputs can briefly contain old event ages during transitions. */
    frame.now_ms = UINT32_MAX;
    frame.target_age_ms = UINT16_MAX;
    frame.target_period_ms = 0;
    frame.effect_age_ms = UINT16_MAX;
    frame.effect_target_age_ms = UINT16_MAX;
    frame.energy = UINT8_MAX;
    frame.lives = UINT8_MAX;
    frame.zombie_hits = UINT8_MAX;
    frame.goal_hits = 0;
    frame.rescue_seconds = UINT8_MAX;
    frame.score = INT16_MIN;
    frame.sensor_warning = true;
    frame.aim_locked = true;
    frame.mode = NULL;
    frame.status = NULL;
    frame.hint = NULL;
    for (unsigned scene = 0; scene < 6u; ++scene) {
        frame.scene = (ExternalDisplayScene)scene;
        for (unsigned effect = 0; effect < 8u; ++effect) {
            frame.effect = (ExternalDisplayEffect)effect;
            render_checked(&frame, first, 0x00);
            render_checked(&frame, second, 0xff);
            CHECK(memcmp(first, second, FRAME_BYTES) == 0);
        }
    }
    frame.target_period_ms = UINT16_MAX;
    frame.target_age_ms = 0;
    frame.score = INT16_MAX;
    frame.goal_hits = UINT8_MAX;
    frame.scene = EXTERNAL_SCENE_BATTLE;
    for (unsigned target = 0; target < 3u; ++target) {
        frame.target = (ExternalColour)target;
        frame.effect_target = (ExternalColour)target;
        render_checked(&frame, first, 0x00);
    }
}

int main(void)
{
    test_scenes_and_replacement();
    test_animation_and_game_feedback();
    test_extreme_inputs();
    printf("Display tests passed: %u checks; bounded drawing, deterministic replacement, "
           "six scenes, moving targets, human/zombie sprites, firing and HUD feedback.\n",
           checks);
    return 0;
}
