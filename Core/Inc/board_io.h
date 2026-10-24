#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdbool.h>
#include <stdint.h>
#include "sensor_sample.h"

/* Call HAL_Init() first. False means clock/UART setup failed. Missing sensors
 * are reported individually in Board_SensorHealth() and SensorSample.valid. */
bool Board_Init(void);
uint32_t Board_SensorHealth(void);
void Board_ReadSensors(SensorSample *sample);
/* Refreshes only acceleration and angular rate; preserves other readings. */
void Board_ReadMotion(SensorSample *sample);
void Board_LedWrite(bool on);

/* Main-context only. Queues a copy; false means queue full or UART stopped.
 * Poll() transmits a bounded chunk with HAL_UART_Transmit while interrupts
 * and SysTick remain enabled. Keep polling until Idle() before Stop(). */
bool Board_UartWrite(const char *message);
void Board_UartPoll(void);
bool Board_UartIdle(void);
void Board_UartStop(void);

#endif
