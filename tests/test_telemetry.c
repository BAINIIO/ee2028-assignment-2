#include "telemetry.h"
#include "app_config.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    SensorSample sample = {
        .temperature_c = 25.0f, .pressure_hpa = 1013.25f, .humidity_pct = 55.0f,
        .acceleration_ms2 = {0.0f, 0.0f, 9.80665f},
        .angular_rate_dps = {0.0f, 0.0f, 0.0f},
        .magnetic_ut = {20.0f, 0.0f, 40.0f}, .valid = SENSOR_ALL
    };
    char text[1024];
    TelemetryStatus status = Telemetry_Check(&sample);
    assert(status.abnormal == 0u && status.unavailable == 0u);
    assert(Telemetry_Format(&sample, text, sizeof text));
    assert(strstr(text, "T: 25.00 degC") != NULL);
    assert(strstr(text, "1013.25 hPa") != NULL);
    assert(strstr(text, "%RH") != NULL && strstr(text, "m/s^2") != NULL);
    assert(strstr(text, "deg/s") != NULL && strstr(text, "uT\r\n") != NULL);

    sample.temperature_c = APP_TEMP_MAX_C + 1.0f;
    sample.humidity_pct = APP_HUMIDITY_MIN_PCT - 1.0f;
    status = Telemetry_Check(&sample);
    assert(status.abnormal == (SENSOR_TEMPERATURE | SENSOR_HUMIDITY));
    assert(Telemetry_Format(&sample, text, sizeof text));
    assert(strstr(text, "WARNING T:") != NULL);
    assert(strstr(text, "WARNING H:") != NULL);
    assert(strstr(text, "P:") == NULL); /* normal telemetry is suppressed */

    sample.valid &= ~(uint32_t)SENSOR_GYROSCOPE;
    sample.magnetic_ut[1] = NAN;
    status = Telemetry_Check(&sample);
    assert(status.unavailable == (SENSOR_GYROSCOPE | SENSOR_MAGNETOMETER));
    assert(Telemetry_Format(&sample, text, sizeof text));
    assert(strstr(text, "G magnitude: sensor unavailable") != NULL);
    assert(strstr(text, "M magnitude: sensor unavailable") != NULL);
    assert(strstr(text, "nan") == NULL);

    sample.temperature_c = APP_TEMP_MAX_C;
    sample.humidity_pct = APP_HUMIDITY_MIN_PCT;
    sample.valid = SENSOR_ALL;
    sample.magnetic_ut[1] = 0.0f;
    assert(Telemetry_Check(&sample).abnormal == 0u);
    assert(Telemetry_Format(&sample, text, sizeof text));
    assert(strstr(text, "WARNING") == NULL); /* automatically resumes */

    /* The replacement board's ~236 uT baseline must retain all six readings;
     * warning-only output begins strictly above the configured 300 uT limit. */
    sample.magnetic_ut[0] = 236.0f;
    sample.magnetic_ut[1] = sample.magnetic_ut[2] = 0.0f;
    assert(Telemetry_Check(&sample).abnormal == 0u);
    assert(Telemetry_Format(&sample, text, sizeof text));
    assert(strstr(text, "T:") != NULL && strstr(text, "M: [236.00,0.00,0.00] uT") != NULL);
    sample.magnetic_ut[0] = APP_MAG_MAX_UT;
    assert(Telemetry_Check(&sample).abnormal == 0u);
    sample.magnetic_ut[0] = APP_MAG_MAX_UT + 1.0f;
    assert(Telemetry_Check(&sample).abnormal == SENSOR_MAGNETOMETER);
    assert(Telemetry_Format(&sample, text, sizeof text));
    assert(strstr(text, "WARNING M magnitude: 301.00 uT") != NULL);
    assert(strstr(text, "T:") == NULL); /* Preserve compulsory suppression. */
    sample.magnetic_ut[0] = 236.0f;
    assert(Telemetry_Format(&sample, text, sizeof text));
    assert(strstr(text, "WARNING") == NULL && strstr(text, "T:") != NULL);

    sample.acceleration_ms2[0] = INFINITY;
    assert((Telemetry_Check(&sample).unavailable & SENSOR_ACCELEROMETER) != 0u);
    memset(text, 'x', sizeof text);
    assert(!Telemetry_Format(&sample, text, 4u));
    assert(text[0] == '\0');
    assert(!Telemetry_Format(&sample, text, 0u));
    puts("Telemetry tests passed: units, all violations, missing/NaN data, resume, bounds.");
    return 0;
}
