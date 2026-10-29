#ifndef COMMAND_TELEMETRY_H
#define COMMAND_TELEMETRY_H
#include "game.h"
#include "telemetry.h"
void Command_Init(uint32_t now_ms);
void Command_Event(const GameEvent *event);
void Command_Poll(const Game *game, const SensorSample *sample,
                  TelemetryStatus status, uint32_t now_ms);
void Command_Final(const Game *game, const SensorSample *sample,
                   TelemetryStatus status, uint32_t now_ms);
void Command_Finish(uint32_t now_ms);
bool Command_Idle(void);
void Command_Stop(void);
#endif
