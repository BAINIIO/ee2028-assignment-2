#include "telemetry.h"
#include "app_config.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static float magnitude(const float vector[3])
{
    return sqrtf(vector[0] * vector[0] + vector[1] * vector[1] +
                 vector[2] * vector[2]);
}

static void check_value(TelemetryStatus *status, uint32_t valid, uint32_t bit,
                        float value, float minimum, float maximum)
{
    if ((valid & bit) == 0u || !isfinite(value)) {
        status->unavailable |= bit;
    } else if (value < minimum || value > maximum) {
        status->abnormal |= bit;
    }
}

TelemetryStatus Telemetry_Check(const SensorSample *sample)
{
    TelemetryStatus status = {0u, 0u};
    check_value(&status, sample->valid, SENSOR_TEMPERATURE,
                sample->temperature_c, APP_TEMP_MIN_C, APP_TEMP_MAX_C);
    check_value(&status, sample->valid, SENSOR_PRESSURE,
                sample->pressure_hpa, APP_PRESSURE_MIN_HPA, APP_PRESSURE_MAX_HPA);
    check_value(&status, sample->valid, SENSOR_HUMIDITY,
                sample->humidity_pct, APP_HUMIDITY_MIN_PCT, APP_HUMIDITY_MAX_PCT);
    check_value(&status, sample->valid, SENSOR_ACCELEROMETER,
                magnitude(sample->acceleration_ms2), 0.0f, APP_ACCEL_MAX_MS2);
    check_value(&status, sample->valid, SENSOR_GYROSCOPE,
                magnitude(sample->angular_rate_dps), 0.0f, APP_GYRO_MAX_DPS);
    check_value(&status, sample->valid, SENSOR_MAGNETOMETER,
                magnitude(sample->magnetic_ut), 0.0f, APP_MAG_MAX_UT);
    return status;
}

static bool append(char *buffer, size_t capacity, size_t *used,
                    const char *format, ...)
{
    if (*used >= capacity) {
        return false;
    }
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer + *used, capacity - *used, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity - *used) {
        buffer[0] = '\0';
        return false;
    }
    *used += (size_t)count;
    return true;
}

bool Telemetry_Format(const SensorSample *sample, char *buffer, size_t capacity)
{
    if (capacity == 0u) {
        return false;
    }
    buffer[0] = '\0';
    size_t used = 0u;
    TelemetryStatus status = Telemetry_Check(sample);
    if ((status.abnormal | status.unavailable) == 0u) {
        return append(buffer, capacity, &used,
            "T: %.2f degC, P: %.2f hPa, H: %.2f %%RH, "
            "A: [%.2f,%.2f,%.2f] m/s^2, G: [%.2f,%.2f,%.2f] deg/s, "
            "M: [%.2f,%.2f,%.2f] uT\r\n",
            (double)sample->temperature_c, (double)sample->pressure_hpa,
            (double)sample->humidity_pct,
            (double)sample->acceleration_ms2[0], (double)sample->acceleration_ms2[1],
            (double)sample->acceleration_ms2[2],
            (double)sample->angular_rate_dps[0], (double)sample->angular_rate_dps[1],
            (double)sample->angular_rate_dps[2],
            (double)sample->magnetic_ut[0], (double)sample->magnetic_ut[1],
            (double)sample->magnetic_ut[2]);
    }

    static const struct {
        uint32_t bit;
        const char *name;
        const char *unit;
        float minimum;
        float maximum;
    } limits[] = {
        {SENSOR_TEMPERATURE, "T", "degC", APP_TEMP_MIN_C, APP_TEMP_MAX_C},
        {SENSOR_PRESSURE, "P", "hPa", APP_PRESSURE_MIN_HPA, APP_PRESSURE_MAX_HPA},
        {SENSOR_HUMIDITY, "H", "%RH", APP_HUMIDITY_MIN_PCT, APP_HUMIDITY_MAX_PCT},
        {SENSOR_ACCELEROMETER, "A magnitude", "m/s^2", 0.0f, APP_ACCEL_MAX_MS2},
        {SENSOR_GYROSCOPE, "G magnitude", "deg/s", 0.0f, APP_GYRO_MAX_DPS},
        {SENSOR_MAGNETOMETER, "M magnitude", "uT", 0.0f, APP_MAG_MAX_UT}
    };
    const float values[] = {sample->temperature_c, sample->pressure_hpa,
        sample->humidity_pct, magnitude(sample->acceleration_ms2),
        magnitude(sample->angular_rate_dps), magnitude(sample->magnetic_ut)};
    for (size_t i = 0u; i < sizeof limits / sizeof limits[0]; ++i) {
        if ((status.unavailable & limits[i].bit) != 0u) {
            if (!append(buffer, capacity, &used,
                "WARNING %s: sensor unavailable; telemetry paused\r\n",
                limits[i].name)) {
                return false;
            }
        } else if ((status.abnormal & limits[i].bit) != 0u) {
            if (!append(buffer, capacity, &used,
                "WARNING %s: %.2f %s, outside [%.2f, %.2f] %s\r\n",
                limits[i].name, (double)values[i], limits[i].unit,
                (double)limits[i].minimum, (double)limits[i].maximum,
                limits[i].unit)) {
                return false;
            }
        }
    }
    return true;
}
