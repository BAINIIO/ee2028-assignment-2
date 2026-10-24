#ifndef SENSOR_SAMPLE_H
#define SENSOR_SAMPLE_H

#include <stdint.h>

enum {
    SENSOR_TEMPERATURE = 1u << 0,
    SENSOR_PRESSURE = 1u << 1,
    SENSOR_HUMIDITY = 1u << 2,
    SENSOR_ACCELEROMETER = 1u << 3,
    SENSOR_GYROSCOPE = 1u << 4,
    SENSOR_MAGNETOMETER = 1u << 5,
    SENSOR_ALL = 0x3fu
};

/* Physical units, never unscaled device register values. */
typedef struct {
    float temperature_c;
    float pressure_hpa;
    float humidity_pct;
    float acceleration_ms2[3];
    float angular_rate_dps[3];
    float magnetic_ut[3];
    uint32_t valid;
} SensorSample;

#endif
