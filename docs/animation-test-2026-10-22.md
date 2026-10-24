# Garden Guard animation and IR test — 22 October 2026

The user requested a Plants vs. Zombies inspired OLED game. Garden Guard adds original animated plant, zombie and friendly-human sprites, pea flight and impact effects, a lawn scene, score/hearts/energy HUD, rescue countdown and result scenes. The compulsory game controls and sensor monitoring remain in the existing state machine.

## Software verification

- ARM build succeeds with the installed STM32 toolchain.
- All host suites pass with AddressSanitizer, UndefinedBehaviorSanitizer and warnings treated as errors: 239 game/button checks, 8,346 renderer checks, telemetry checks, and application scheduling/event scenarios.
- Application tests verify that a shot's old target and position survive the following target event, the new target advances independently, standby clears effects, rescue/terminal scenes follow the real state machine, and visual ages survive tick wrap.
- A fake-HAL OLED test uses the real transport and emulates a partial NACK at byte offset 544, matching the physical observation. It verifies one-second backoff, complete-frame addressing after recovery, newest-frame preservation, BUSY deferral, absent-module behavior and no recovery during terminal draining.
- Five external-I/O ARM configurations compile with warnings as errors: default, active buzzer, optional modules disabled, colour enabled and colour without OLED.

The preview tool compiles and executes the actual C renderer. Its [animated GIF](../output/game-preview/garden-game.gif) and [contact sheet](../output/game-preview/contact-sheet.png) are host-generated previews, not photographs of the OLED. The 16 scene previews were visually inspected.

## First animation deployment

The first animation image was programmed and verified in [20261022-102414](../output/hardware-tests/20261022-102414/summary.json), SHA-256 `0de07211afc9b64769424ed627e0b3cfb991e67f3abb92d01d2d623321751c39`. Its boot capture reported `OLED=online`, all six sensors valid (`0x3f`), and eight full normal telemetry lines during the capture. The user explicitly confirmed seeing the Garden Guard animation.

A later read-only SWD snapshot found the display offline at tick 35,912 ms, with a partial transfer offset of 544. The application and sensors continued running. A [diagnostic snapshot](../output/hardware-tests/20261022-102414/oled-failure-diagnostics/snapshot.json) confirmed HAL I2C error `0x4` (`HAL_I2C_ERROR_AF`, acknowledgement failure), with both SDA and SCL high. A subsequent reset also encountered an acknowledgement failure at another transfer offset. These observations establish the failure mode but not its electrical cause.

The driver previously disabled OLED output permanently after a failed transaction. The updated transport defers BUSY work, retains the newest drawing frame, and retries a previously working display after one second. Recovery reissues initialization and full-frame addressing because a partial failed transmission can leave the display's RAM pointer uncertain. OLED transactions now have an 8 ms timeout; the bus remains 100 kHz and data chunks remain 32 bytes. A missing module at boot is not repeatedly probed. Terminal output never initiates recovery.

Manual activity in the first animation revision's [controls log](../output/hardware-tests/20261022-102414/animation-controls.log) includes battle entry, target changes, charging, human avoidance, zombie escapes and ordinary defeat. This does not establish a correct zombie shot or winning round on hardware.

## IR detection and release

The module is labelled **infrared reflective sensor v1.2**, connected to A0/PC5. The user held a white card steadily with its red indicator on, then removed it on request. Read-only SWD snapshots recorded:

| Condition | PC5 digital level | Debounced IR presence | Game aim lock | Evidence |
| --- | --- | --- | --- | --- |
| White card held steadily | 0 | true | true | [held snapshot](../output/hardware-tests/20261022-102414/ir-card-held/snapshot.json) |
| Card removed | 1 | false | false | [removed snapshot](../output/hardware-tests/20261022-102414/ir-card-removed/snapshot.json) |

This confirms the physical signal and debounced game-state path in both directions. The earlier lack of a visible indication does not establish a wiring fault. The IR detects nearby reflected infrared light, not target identity or colour. OLED game state determines zombie/human identity; aim lock supplies the existing +5 zombie-hit bonus. The bonus calculation is host-tested; an actual IR-assisted zombie hit has not yet been captured.

## Recovery revision

The recovery revision was programmed and verified in [20261022-102951](../output/hardware-tests/20261022-102951/summary.json). SHA-256: `37d00980ddf2784d1b92a08b9e290c29c07fd611eb022370cacf1fe60d5869db`. The linker reports **52,180 bytes flash** and **20,832 bytes RAM**, including reserved heap/stack.

Its boot UART reported standby, OLED online and all six sensors valid. Eight magnetic-threshold warnings were captured at roughly one-second intervals (approximately 201 µT versus the configured 200 µT limit), demonstrating continued monitoring. That configured threshold and sensor calibration still need justification for the demonstration.

The [recovery snapshot](../output/hardware-tests/20261022-102951/recovery-state-1/snapshot.json) reported 23 failures and 23 recoveries by the sampled tick near 30 seconds. This demonstrates the recovery path but does not establish stable animation. Live snapshots read separate variables at different times, can observe intermediate sensor/transport states and may perturb execution; they are not atomic or a measurement of panel refresh. For example, a snapshot's temporary missing motion bits are not by themselves evidence of a permanent sensor failure.

## Current installed image and unresolved OLED stability

The final diagnostic revision uses **16-byte OLED chunks**, retaining **100 kHz**, **8 ms OLED timeout**, **125 ms requested frame updates**, and one-second recovery. The smaller chunks were a targeted experiment, not a confirmed controller requirement. UART now records every OLED offline/recovered transition so failures can be observed without a live SWD session.

This image was programmed and verified in [20261022-103350](../output/hardware-tests/20261022-103350/summary.json), SHA-256 `1cd75f3837a0233db4425051162ccfe6494173d6328c52deef8c7f62b9179fdf`. Linked memory is **52,276 bytes flash** and **20,832 bytes RAM**, including heap/stack reservation. All host suites pass with this chunk size.

The [boot UART capture](../output/hardware-tests/20261022-103350/uart.log), collected without a concurrent live SWD session, contains eight full normal sensor telemetry lines, six `DISPLAY: OLED transfer failed; retry pending` events and six `DISPLAY: OLED recovered` events. Thus the smaller chunks did **not** eliminate the acknowledgement failures. The software recovers, but continuous OLED stability is **unresolved**. The user could not reseat the OLED connections before the lab ended and reported that it works for now. Hardware testing stopped at the user's departure; no post-reseat result is claimed. The electrical cause has not been established. The next bench step is to power down and reseat GND/3V3/SCL/SDA, then observe animation and receive-only UART without a debugger connection.

## Remaining physical checks

Capture a correct zombie hit with and without IR bonus, a complete winning round, smooth inversion/attacked detection, successful rescue, and rescue-timeout shutdown. Measure LED cadence and button boundaries if required for demonstration evidence. Check OLED wiring/pull-ups and observe extended animation stability; software retry cannot repair a disconnected or electrically faulty module.
