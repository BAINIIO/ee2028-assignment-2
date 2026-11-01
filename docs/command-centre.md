# Local Garden Guard command centre

## Current verification — 1 November 2026

The NFC firmware was programmed and verified on the connected STM32L4S5I-DK
(probe `0667FF515652787187041646`). The tag committed and read back the 49-byte
record with zero NFC errors. The iPhone displayed an NFC notification and the
user confirmed the Garden Guard website opens. The current local link is
`http://172.20.10.5:8080/drone/GG01?mission=0`; it requires the Mac server to
remain running and the phone to reach that hotspot network.

Current wiring is OLED on Grove I2C, IR on A0/PC5 and buzzer on D2/PD14. The
board reports all six sensors valid and OLED online; PD14 is configured as an
output. Audible D2 operation is not yet independently confirmed. Native board
Wi-Fi telemetry is not yet connected: the module rejected its hotspot join
before reaching TCP setup. Its retained failure markers were `ERROR` and
`Usage` at join stage 5, with zero SPI errors in the controlled retry cycle.
The website opening through NFC does not establish live wireless sensor
delivery. Confirm the current hotspot details before further Wi-Fi testing.

The connected board's original 2 MiB flash was backed up locally before the
update under `output/hardware-tests/20261101-nfc/`. That directory also holds
the programming logs, UART NFC-ready message and initial live status reads.

The older 29 October D6 baseline ELF/BIN/HEX/MAP were preserved locally under
`build/known-working/`. Its BIN SHA-256 is
`4f18a88f7a81fcba3ac7b6df86cad44b9f04eaa7888f9d8140eb65b868682554`.
Use those files only for a board still running that baseline. For the current
flashed firmware, the viewer uses the matching build by default:

```sh
sh tools/live_display.command
```

## Prepare the board on the intended network

1. Enable the iPhone hotspot's **Allow Others to Join** and **Maximize
   Compatibility**, if available. The module needs a compatible 2.4 GHz WPA2
   personal network. Connect the Mac to that same hotspot and obtain its
   reachable address with `ipconfig getifaddr en0`.
2. Run `python3 tools/configure_network.py`. It asks for the SSID and password
   using hidden prompts, plus the Mac's numeric IPv4 address. It compiles a
   private `build/firmware/network_credentials.o` through compiler stdin;
   it does not save a plaintext C/JSON credentials file. Keep the ingestion
   port at 8081 and HTTP port at `APP_COMMAND_HTTP_PORT` (8080 by default).
3. Run `make` and `make host-test`, then start the server using the commands
   below. Flashing is a separate explicit step through `tools/board.py flash`.
   A build without the private object leaves Wi-Fi disabled. `make clean`
   removes the private object, so network setup must be repeated afterward.
4. Verify native Wi-Fi packets before testing NFC. The firmware programs a
   Type 5 NDEF URL such as `http://MAC_IP:8080/drone/GG01?mission=0`. After
   readback succeeds, the NFC-ready indicator is enabled. Tap the board's
   printed NFC antenna with the top of the iPhone, then tap its notification.
   The page opens in Safari; the phone must reach the Mac's local network.

For NFC setup without board Wi-Fi credentials, a build can instead supply
`APP_NFC_DASHBOARD_URL`. It overrides the URL derived from the network object:

```sh
make EXTRA_DEFINES='-DAPP_NFC_DASHBOARD_URL=\"http://MAC_IP:8080/drone/GG01\"'
```

Replace `MAC_IP` with the Mac's reachable numeric address. This programs the
phone link; live wireless telemetry still requires the board's network
configuration. Keep the same override on subsequent builds. The base URL must
be shorter than 180 bytes. A missing or invalid URL leaves the existing tag's
memory and RF settings untouched. Once the URL has been committed and read
back, UART emits `NFC READY` with the URL and the antenna instruction.

Mission selection is available only in standby. The STM32 validates the
request again, updates its mission identifier and NFC record, and reports the
result. A detected RF field creates an NFC check-in event and a short sound;
it is an observation of NFC activity, not proof of a person's identity. NFC
does not perform the compulsory rescue or modify the laser energy rules.

NFC uses short I2C2 operations alongside the existing sensors. Accepted NDEF
writes finish in bounded steps even if the game changes modes. The length is
invalidated first and committed last, and the complete record is read back.
Each EEPROM transfer stays within one aligned four-byte page and waits seven
milliseconds through SysTick scheduling. This respects the ST25DV's per-page
programming time; a sixteen-byte transfer would need multiple write cycles.
See [ST DS10925, section 6.4.2](https://www.st.com/resource/en/datasheet/st25dv04k.pdf).
The driver attempts to set RF sleep as the static default when its factory
I2C configuration session is available, and explicitly wakes the tag only
after programming. A customized tag can reject that factory session; the
static power-loss policy must be checked on the actual hardware.

On termination, a priority final network packet supersedes unsent backlog.
The bounded output drain services only pending transmissions and NFC RF-sleep
write/readback attempts. The Wi-Fi module is then reset/deinitialized and
UART stops. NFC shutdown verification can fail under an I2C hardware fault;
`Nfc_Quiet()` distinguishes confirmed sleep from exhausted retries. Physical
RF silence, radio shutdown and deadline behavior remain bench checks.

The command centre serves the website from the Mac and receives real board
telemetry over the board's native WiFi connection. It uses Python's standard
library. It does not register cloud hosting, open the physical UART, start
ST-Link, reflash the board, or generate simulated startup readings.

The physical USER button still controls standby/battle, charging and rescue.
The website can request a mission only while the live native WiFi board is in
standby. The firmware must also check the mode when it receives that request;
a mode can change while a network command is in flight.

## Start the Mac server

Run from the `Assignment 2` directory:

```sh
python3 tools/command_centre.py
```

Or launch `tools/command_centre.command`. The launcher uses `python3` unless
`GARDEN_GUARD_PYTHON` selects another interpreter. No extra Python packages
are required. The defaults are:

| Connection | Address |
| --- | --- |
| Website and HTTP API | Mac, TCP port 8080 |
| Native board WiFi telemetry | Mac, TCP port 8081 |
| Drone page | `http://MAC_IP:8080/drone/GG01` |
| Files served | `web/command-centre/dist/` |

The server listens on `0.0.0.0`, so another device on the same local network
can reach it. Its automatic URL uses an address selected by the Mac routing
table; this lookup does not send an Internet request. If a VPN, multiple
interfaces or a changed hotspot selects an unsuitable address, supply the
actual reachable Mac address explicitly:

```sh
python3 tools/command_centre.py \
  --public-url http://192.168.1.20:8080
```

The example address is a placeholder. The HTTP URL's port must match
`--http-port`. Options also include `--host`, `--http-port`, `--ingest-port`
and `--static-root`. Changing an advertised URL does not configure the board's
destination address or rewrite its NFC record. Update those firmware settings
to the same reachable Mac address through the project's separate configuration
workflow. Never include WiFi passwords in website URLs or committed files.

Keep the Mac and phone on the same reachable network. With an iPhone hotspot,
use the Mac's address on that hotspot. Allow the local Python server through
the Mac firewall if requested. A website URL on another network will not
magically reach this local server. No Internet connection is required for the
HTTP/TCP data path itself.

## Data flow and UART independence

```text
STM32 native WiFi -> Mac TCP receiver :8081 -> bounded state/history
                                             |
                                             v
                                     HTTP + SSE :8080
                                             |
                                             v
                                      Mac/phone browser

STM32 USART1 -> ST-Link virtual COM port -> existing Tera Term/monitor
```

The backend never takes ownership of the physical serial port. The existing
Wine/Tera Term receiver or its serial relay can continue receiving compulsory
UART output independently. The browser's main source is the native board
WiFi transport. A laptop's WiFi carrying a UART gateway is a different path
and is labelled `uart` if the optional handoff below is enabled.

The dashboard starts with `telemetry: null` and an offline link. It shows
readings only after a valid board message arrives. Disconnecting the board
retains its last real snapshot with its original receipt time; those values
must not be presented as new measurements.

## Board wire protocol

The board opens a TCP connection to port 8081 and sends one UTF-8 JSON object
per line. Each line, excluding its newline, is limited to 2,048 bytes. The
receiver accepts CRLF as well as LF, rejects malformed UTF-8, duplicate JSON
keys, unknown drone IDs, NaN/Infinity and invalid field types/ranges, and
disconnects an overlong unfinished line. Repeated malformed lines also close
that connection. Sensor values that are unavailable use JSON `null`, not NaN.

Both message types carry `id: "GG01"`, an unsigned 32-bit `seq`, millisecond
`tick`, `energy`, `score` and `target`. One sequence counter is shared across
events and snapshots. A snapshot additionally carries mode, attacked/round
state, lives/hits, rescue and target milliseconds, sensor validity/warning
masks, scalar temperature/pressure/humidity, three-axis acceleration/gyro/
magnetic values, IR, mission, NFC state/tap count and WiFi diagnostics. These
fields follow the firmware/frontend contract; the backend adds `received_at`
in Unix epoch seconds. Events carry an `event` name instead of full readings.

Sequence comparison accepts unsigned rollover while dropping duplicate and
older messages within the same TCP connection. Tick rollover does not order
network messages; sequence numbers do. A new valid connection supersedes an
older socket, which cannot later overwrite its data.

The backend keeps at most 300 telemetry snapshots internally and 200 API
events. It limits board connections to 16, HTTP request workers to 32 and
SSE streams to 16. This is a local teaching/demo service, without account
authentication or Internet-facing deployment support.

## Browser API

`GET /api/state` returns the current complete state:

```text
revision: increasing state revision within this server instance
generated_at: snapshot generation time in Unix epoch seconds
server_instance: unique identity for this backend run
telemetry: latest flat board snapshot plus received_at, or null
link: source, status, wifi, uart, last_seen, age_ms
events: bounded entries with id, at, kind, message, level, source
missions: the three mission IDs, names and descriptions
server: url, drone_id, board_ingest_port
```

`received_at`, `last_seen` and event `at` are Unix epoch seconds. `age_ms` is
integer milliseconds. `GET /api/stream` sends Server-Sent Events named
`snapshot`, each containing that same entire API state. Changes trigger an
update; otherwise a snapshot is sent approximately once a second so stale
status and age remain honest without new board data.

The browser orders both HTTP and SSE updates by server instance, revision and
generation time so delayed responses cannot replace newer state. A backend
restart establishes a new ordering sequence.

The normal status is `online` for a fresh connected source. After three
seconds without a fresh snapshot it becomes `stale`; after ten seconds it is
`offline`. A closed native WiFi socket is offline immediately. The last
source and snapshot can remain visible even while offline. A fresh optional
UART snapshot can become the fallback source, but cannot enable remote
mission requests.

The presets are:

| ID | Mission |
| --- | --- |
| 0 | Orchard Perimeter |
| 1 | Evacuation Corridor |
| 2 | Greenhouse Shelter |

`POST /api/mission` accepts a JSON body containing integer `id` from zero to
two. It requires a connected, fresh native WiFi snapshot with `wifi: true`,
`mode: "standby"`, no active attack and no terminal latch. It writes exactly
`MISSION 0\n`, `MISSION 1\n` or `MISSION 2\n` back on the board's TCP socket.
HTTP 202 means the request was sent, not that the board accepted or applied
it. The UI must wait for the board's event/snapshot confirmation. The backend
never updates the mission optimistically.

POST bodies require `application/json`, a bounded Content-Length and the
dashboard's own origin. Foreign Origin/Host values are rejected and CORS is
not enabled. JSON content type prevents cross-site HTML forms from issuing
mission requests. Static file resolution cannot escape the dashboard folder;
`/drone/GG01` serves its `index.html`.

## Termination and reset

A `terminated` event immediately latches the link status, even if the final
complete snapshot has not arrived yet. The final terminated snapshot supplies
the final actual sensor/game fields. The server retains that final state
after the board closes its connection; no ordinary stale packet can return
the old session to online or re-enable mission assignment.

A newer native WiFi TCP connection must first publish a standby snapshot to
establish the reset session and clear the terminal latch. An older socket,
the same connection's stray standby message, or a UART fallback cannot revive
the terminated session. This is session handling, not cryptographic identity
verification: the service belongs on the trusted local demo network.

The Mac website can remain open showing retained terminal state. Its SSE
connection is between the Mac and browser, not continued traffic from the
terminated drone. Firmware termination must independently stop its native
WiFi, UART and other required communication paths.

## Optional UART handoff

The backend can accept an existing local relay's structured messages without
opening UART itself. This endpoint is disabled by default. Enable it with:

```sh
python3 tools/command_centre.py --allow-uart-ingest
```

Only a loopback client may then POST to `/api/ingest`, with an explicit source
and wire message:

```json
{
  "source": "uart",
  "session": "board-boot-1",
  "message": {"type": "snapshot", "id": "GG01"}
}
```

The example intentionally omits the remaining required snapshot fields;
send the full firmware wire object for an accepted reading. Use a new short
session identifier for a reset UART stream. This handoff does not parse the
existing human-readable UART lines automatically. It is an integration point
for a separate relay, not a claim that UART fallback is already connected.

## Verification

The 1 November update passed all 11 C suites, including a new actual-driver
fake-bus test that models EEPROM busy time per four-byte page. It covers
complete URI programming/readback, retries, unchanged records, an unconfigured
tag remaining untouched, shared-bus deferral and bounded RF shutdown. Reporter
tests also cover standalone URL precedence and one UART notice per readiness
transition, including accepted/rejected URL length boundaries.

The 29 October candidate passed the ARM build, all 10 C test suites with
AddressSanitizer/UndefinedBehaviorSanitizer, 35 Python tests and 30 browser
checks at desktop and iPhone sizes. Browser telemetry was explicitly synthetic.
The linked image contains `HAL_UART_Transmit` and `SysTick_Handler`, with no
linked `printf`, `HAL_Delay` or `HAL_UART_Transmit_IT`. Four packets emitted by
the actual C reporter also passed the Python receiver's strict validation.

Run the backend tests without a board:

```sh
python3 -B -m unittest discover -s tests -p 'test_command_centre.py' -v
```

The tests use real temporary local HTTP, SSE and TCP sockets. They cover
empty/offline startup, SPA routes/path traversal, coherent API updates,
actual mission command bytes, mission gating, stale/disconnected states,
explicit UART fallback, sequence/tick rollover, terminal/reset handling,
old-socket rejection, malformed/non-finite messages, request origins and
bounded history. They do not establish onboard WiFi performance, NFC phone
compatibility or a physical rescue/termination demonstration.
