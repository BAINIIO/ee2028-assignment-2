#ifndef GAME_CONFIG_H
#define GAME_CONFIG_H

#include "app_config.h"

#define GAME_ENERGY_CAPACITY 10u
#define GAME_CHARGE_ENERGY 3u
#define GAME_SHOT_ENERGY 5u
#define GAME_INITIAL_LIVES 3u
#define GAME_WIN_HITS 6u
#define GAME_TARGET_PERIOD_MS 4000u
#define GAME_ZOMBIE_POINTS 10
#define GAME_AIM_BONUS_POINTS 5
#define GAME_HUMAN_SAVED_POINTS 5
#define GAME_PENALTY_POINTS 5

/* Acceleration determines gravity direction; a gyro cannot measure absolute
 * orientation. Gyro integration corroborates rotation away from the captured
 * upright reference. Keep the board still briefly after reset to calibrate. */
#define GAME_GRAVITY_MIN_MS2 6.0f
#define GAME_GRAVITY_MAX_MS2 13.0f
#define GAME_REFERENCE_MAX_DPS 20.0f
#define GAME_GYRO_PHYSICAL_MAX_DPS 2000.0f
#define GAME_FLIP_MIN_DEGREES 60.0f
#define GAME_UPSIDE_COSINE_SQUARED 0.49f
#define GAME_UPRIGHT_COSINE_SQUARED 0.25f
#define GAME_UPSIDE_HOLD_MS 350u
#define GAME_MAX_MOTION_GAP_MS 250u
#define GAME_EVENT_CAPACITY 32u

#endif
