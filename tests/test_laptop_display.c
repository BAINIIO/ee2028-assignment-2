/* The laptop mirror must work when every physical external module is off. */
#define APP_ENABLE_OLED 0
#define APP_ENABLE_COLOUR 0
#define APP_ENABLE_BUZZER 0
#define APP_ENABLE_IR 0
#include "../Core/Src/external_io.c"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint8_t expected[1024];
    ExternalDisplayFrame frame = {
        .scene = EXTERNAL_SCENE_BATTLE, .now_ms = 1000,
        .target = EXTERNAL_COLOUR_RED, .target_period_ms = 4000,
        .target_age_ms = 500, .lives = 3, .energy = 3, .goal_hits = 6
    };
    External_Init(0);
    assert(!External_DisplayOnline() && laptop_display_sequence == 0);
    assert(laptop_display_terminal == 0u);
    External_SetDisplay(&frame);
    GameDisplay_Render(expected, &frame);
    assert(laptop_display_sequence == 2);
    assert(laptop_display_terminal == 0u);
    assert(memcmp(expected, laptop_display_buffer, sizeof expected) == 0);

    External_SetDisplay(NULL);
    assert(laptop_display_sequence == 2);
    assert(memcmp(expected, laptop_display_buffer, sizeof expected) == 0);

    frame.scene = EXTERNAL_SCENE_TERMINATED;
    External_SetDisplay(&frame);
    GameDisplay_Render(expected, &frame);
    assert(laptop_display_sequence == 4);
    assert(laptop_display_terminal == 1u);
    assert(memcmp(expected, laptop_display_buffer, sizeof expected) == 0);
    External_FinishDisplay();
    assert(External_DisplayIdle() && laptop_display_sequence == 4);
    External_SetDisplay(NULL);
    assert(laptop_display_terminal == 1u);
    External_Init(0);
    assert(laptop_display_terminal == 0u && laptop_display_sequence == 0u);
    puts("Laptop display tests passed: no external modules, coherent frame publication, NULL input, terminal frame.");
    return 0;
}
