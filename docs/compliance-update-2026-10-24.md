# Lab 5 compliance update and verification

This record covers the 24 October 2026 UART and laptop-display changes. The
original Assignment 2 brief supplies the compulsory behavior; `Lab5_B03.pdf`
pages 3, 6, 12 and 17 supply the additional UART, interrupt and timing guidance.
The [requirements matrix](requirements.md) maps the full assignment to code.

## Implemented changes

| Guidance | Current implementation |
| --- | --- |
| No `printf`; messages through UART | Bounded `snprintf`/`vsnprintf` format RAM buffers. `Board_UartWrite` copies messages into the UART queue. Application output does not use `printf` or semihosting. |
| `HAL_UART_Transmit` | `Board_UartPoll` calls that exact function on up to 16 contiguous bytes with a 2 ms timeout. USART1 is PB6/PB7 AF7, 115200 baud, eight data bits, no parity, one stop bit and no flow control. |
| SysTick timing; no `HAL_Delay` | SysTick advances the millisecond tick. Foreground timestamp comparisons schedule sensors, rendering, button classification and deadlines. Neither `printf`, `HAL_Delay` nor `HAL_UART_Transmit_IT` is linked in this firmware. |
| Interrupt pushbutton | PC13 EXTI records timestamped edges. Debouncing and single/double classification occur in foreground code. Interrupts stay enabled during UART polling. |
| Final notification and communication cutoff | Final queued output has a 600 ms drain loop budget, followed by UART/peripheral shutdown. A bounded transfer already in progress can finish slightly later. The laptop closes its debugger connection after a coherent terminal frame and retains the final picture. |

The UART queue preserves bytes already submitted when a HAL timeout interrupts
a chunk. Repeated no-progress failures have bounded drops recorded by diagnostic
counters. Idle requires both an empty queue and the UART transmission-complete
flag. These details prevent replaying partial messages or stopping before the
final byte finishes.

Historical starter examples remain under `reference/` locally but are not
compiled or included in the submission ZIP. Supplied vendor source contains an
unused BSP delay wrapper; it does not introduce a linked `HAL_Delay` call.

## Checks and observed results

- `make -j4 all host-test` passed: seven C suites under AddressSanitizer and
  UndefinedBehaviorSanitizer, including actual UART queue/polling tests.
- All 20 Python transport/lifecycle tests passed, as did the native viewer's
  framebuffer/frame-rate self-test and Python syntax checks.
- Arm symbol inspection confirmed linked `HAL_UART_Transmit`, SysTick and
  `laptop_display_terminal`; `printf`, `HAL_Delay` and `HAL_UART_Transmit_IT`
  were absent.
- The firmware was programmed and verified on the connected STM32L4S5.
  [Boot UART](../output/hardware-tests/20261024-153523-compliance/uart.json)
  reported `sensors=0x3f/0x3f` and `OLED=online`. The local magnetic field was
  about 241 microtesla, above the initial 200 microtesla limit, so one-second
  warnings correctly replaced ordinary telemetry.
- A six-second live mirror check received 175 frames, about **29.18 fps**.
  UART transmit-failure and dropped-byte counters were both zero. This is a
  short observed rate with a 30 fps target, not a guaranteed minimum.
- A [debugger-injected timeout check](../output/hardware-tests/20261024-153523-compliance/live-terminal-check.json)
  backdated the attacked timestamp by 9,500 ms. The final frame arrived about
  0.53 seconds later, the viewer closed its owned socket and OpenOCD process,
  and a subsequent frame request made no further board access. The
  [UART capture](../output/hardware-tests/20261024-153523-compliance/terminal-uart.json)
  ended with one termination notification followed by over three seconds of
  observed silence. The board was reset afterward.

The timeout was injected into firmware state. It does **not** establish a
physical inversion or a complete ten-second real-time countdown. A separate
full-countdown attempt was rescued by button input before expiry; its log
records roughly one-second attacked messages, rescue and subsequent charging.
The first backdated injection skipped normal attack-event log initialization
and caused artificial message catch-up; it is retained separately as
`initial-*` evidence and is not evidence of ordinary attacked-message cadence.

Firmware binary SHA-256:
`4f18a88f7a81fcba3ac7b6df86cad44b9f04eaa7888f9d8140eb65b868682554`.

## Remaining physical checks and submission details

Physical flip detection, a full physical attack/rescue/timeout sequence, a
manual winning round and calibrated LED/sensor timing remain pending. OLED
controller detection does not establish panel image quality; the user reports
distorted pixels, so the laptop mirror remains the usable game display.

The original brief calls for a 20-minute presentation/demo and contribution
appendix. Lab 5 describes 15 minutes and an optional appendix. The draft keeps
the original brief's fuller requirements and provides a 15-minute slide run
sheet option. Team identifiers and actual contributions remain editable
placeholders. The source ZIP and slides are prepared locally and have not been
submitted.
