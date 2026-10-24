#ifndef EXTERNAL_IO_H
#define EXTERNAL_IO_H

#include <stdbool.h>
#include <stdint.h>

/* Optional, external modules; the six compulsory sensors are on-board. */
typedef enum {
    EXTERNAL_COLOUR_NONE = 0,
    EXTERNAL_COLOUR_RED,
    EXTERNAL_COLOUR_GREEN
} ExternalColour;

typedef struct {
    uint16_t clear, red, green, blue;
    ExternalColour colour;
    uint32_t timestamp_ms;
    bool valid;
} ExternalColourSample;

typedef enum {
    EXTERNAL_SOUND_CHARGE = 0,
    EXTERNAL_SOUND_SHOT,
    EXTERNAL_SOUND_WARNING,
    EXTERNAL_SOUND_ATTACK,
    EXTERNAL_SOUND_RESCUE,
    EXTERNAL_SOUND_WIN,
    EXTERNAL_SOUND_LOSE
} ExternalSound;

typedef enum {
    EXTERNAL_SCENE_STANDBY = 0,
    EXTERNAL_SCENE_BATTLE,
    EXTERNAL_SCENE_WON,
    EXTERNAL_SCENE_LOST,
    EXTERNAL_SCENE_ATTACKED,
    EXTERNAL_SCENE_TERMINATED
} ExternalDisplayScene;

typedef enum {
    EXTERNAL_EFFECT_NONE = 0,
    EXTERNAL_EFFECT_CHARGE,
    EXTERNAL_EFFECT_SHOT,
    EXTERNAL_EFFECT_HIT,
    EXTERNAL_EFFECT_HUMAN_HIT,
    EXTERNAL_EFFECT_HUMAN_SAVED,
    EXTERNAL_EFFECT_MISS,
    EXTERNAL_EFFECT_RESCUED
} ExternalDisplayEffect;

typedef struct {
    const char *mode;
    const char *status;
    const char *hint;            /* Context-specific controls, up to 21 chars. */
    int16_t score;
    uint8_t energy;              /* 0..10 */
    uint8_t lives;
    uint8_t rescue_seconds;      /* shown only while attacked */
    ExternalColour target;      /* red = zombie, green = human */
    bool attacked;
    bool sensor_warning;
    bool aim_locked;            /* digital reflective IR sees a nearby object */
    /* Visual state only: rendering never advances the rules or reads sensors. */
    ExternalDisplayScene scene;
    uint32_t now_ms;
    uint16_t target_age_ms;
    uint16_t target_period_ms;
    uint16_t effect_age_ms;
    uint16_t effect_target_age_ms;
    ExternalDisplayEffect effect;
    ExternalColour effect_target;
    uint8_t zombie_hits;
    uint8_t goal_hits;
} ExternalDisplayFrame;

/* Call after HAL_Init and clock configuration. No module is mandatory. */
void External_Init(uint32_t now_ms);
/* Call every main-loop iteration; advances sensor, OLED and melody state. */
void External_Poll(uint32_t now_ms);
ExternalColourSample External_GetColour(void);
/* Renders immediately; the read-only laptop mirror works without an OLED.
 * Physical OLED transmission is incremental. */
void External_SetDisplay(const ExternalDisplayFrame *frame);
/* Drain the final screen without acquiring further IR/colour sensor data. */
void External_FinishDisplay(void);
bool External_DisplayIdle(void);
void External_Play(ExternalSound sound, uint32_t now_ms);
/* Silences PWM and discards pending notes. Safe at terminal shutdown. */
void External_AllStop(void);
bool External_ColourOnline(void);
bool External_DisplayOnline(void);
bool External_BuzzerOnline(void);
/* Reflective IR reports proximity only; it cannot determine target colour. */
bool External_IRPresent(void);
bool External_IREnabled(void);

#endif
