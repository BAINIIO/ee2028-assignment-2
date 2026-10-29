#ifndef NETWORK_IO_H
#define NETWORK_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_PACKET_MAX 2048u
#define NETWORK_QUEUE_CAPACITY 8u

/* Supply a strong Network_GetConfig definition in a private build object.
 * Strings must remain valid for the program lifetime. NULL disables Wi-Fi.
 * Credentials are never logged or included in status diagnostics. */
typedef struct {
    const char *ssid;
    const char *password;
    uint8_t server_ipv4[4];
    uint16_t server_port;
} NetworkConfig;

typedef enum {
    NETWORK_DISABLED = 0, NETWORK_RESETTING, NETWORK_JOINING,
    NETWORK_CONNECTING, NETWORK_ONLINE, NETWORK_RETRY, NETWORK_STOPPED
} NetworkState;

typedef enum {
    NETWORK_ERROR_NONE = 0, NETWORK_ERROR_CONFIG, NETWORK_ERROR_SPI,
    NETWORK_ERROR_TIMEOUT, NETWORK_ERROR_RESPONSE, NETWORK_ERROR_OVERFLOW
} NetworkError;

typedef struct {
    NetworkState state;
    NetworkError last_error;
    uint32_t retries;
    uint32_t commands;
    uint32_t sent_packets;
    uint32_t sent_bytes;
    uint32_t received_missions;
    uint32_t dropped;
    uint32_t spi_errors;
    uint32_t at_errors;
    uint8_t stage;               /* Command index; never command/credential text. */
    uint8_t queued;
} NetworkStatus;

const NetworkConfig *Network_GetConfig(void);
void Network_Init(uint32_t now_ms);
/* One short SPI operation per call, at most 16 words and a 1 ms HAL timeout.
 * The caller must leave SysTick/button interrupts enabled. No delay waits. */
void Network_Poll(uint32_t now_ms);
/* Main-context only. Copies one complete newline-terminated JSON packet.
 * Returns false and counts a drop if not online, invalid, or full. */
bool Network_Queue(const char *line, size_t length);
/* Terminal priority packet: preserve a partly sent/in-flight packet, discard
 * older unsent backlog, admit this final packet, then begin drain. Returns
 * false if invalid or not connected; begins drain in those cases too. */
bool Network_QueueFinal(const char *line, size_t length);
bool Network_Connected(void);
uint32_t Network_Dropped(void);
/* Only an exact MISSION <0..2> line from this TCP connection is accepted. */
bool Network_ReadMission(uint8_t *mission_id);
NetworkStatus Network_GetStatus(void);
/* After the final packet is queued, allow existing replies and queued TX only.
 * No new connection attempts, receive polls, missions, or queue admission. */
void Network_BeginDrain(void);
bool Network_Idle(void);
/* Immediate reset/deinit, no AT handshake or output drain. Safe to repeat. */
void Network_Stop(void);

#endif
