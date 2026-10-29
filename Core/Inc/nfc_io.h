#ifndef NFC_IO_H
#define NFC_IO_H
#include <stdbool.h>
#include <stdint.h>
void Nfc_Init(const char *dashboard_url, uint32_t now_ms);
void Nfc_Poll(uint32_t now_ms, bool allow_update);
bool Nfc_SetMission(uint8_t mission);
void Nfc_SignalEdge(uint32_t now_ms);
bool Nfc_Online(void);
bool Nfc_Ready(void);
uint32_t Nfc_Taps(void);
uint32_t Nfc_Errors(void);
bool Nfc_PopTap(uint32_t *time_ms);
void Nfc_BeginStop(void);
void Nfc_FinishStop(uint32_t now_ms);
bool Nfc_StopDone(void);
bool Nfc_Quiet(void);
/* RF_SLEEP keeps the powered tag silent until initialization after reset. */
void Nfc_Stop(void);
#endif
