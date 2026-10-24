#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "sensor_sample.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t abnormal;
    uint32_t unavailable;
} TelemetryStatus;

TelemetryStatus Telemetry_Check(const SensorSample *sample);
/* Emits either a complete reading with units OR all violations, never both. */
bool Telemetry_Format(const SensorSample *sample, char *buffer, size_t capacity);

#endif
