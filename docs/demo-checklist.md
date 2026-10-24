# Bench verification and demonstration

This is a checklist to execute, not a record of completed hardware tests. Record dates, firmware configuration, measured observations, log filenames and actual team contributions as work is performed. Start with the [README commands](../README.md), [hardware wiring](external-hardware.md) and [requirements matrix](requirements.md).

## Before connecting and flashing

- [ ] Confirm the board is B-L4S5I-IOT01A / STM32L4S5 and the USB cable carries data through CN7/ST-LINK.
- [ ] Identify the OLED controller, resolution and address; the driver expects I2C SSD1306, 128×64, `0x3c`.
- [ ] Confirm 3.3 V module logic, common ground, I2C pull-ups, IR output polarity and buzzer type/driver requirements.
- [ ] Record the selected `EXTRA_DEFINES`. Build with `make` using those options, then run `make host-test`.
- [ ] Use `python3 tools/board.py detect` to choose the intended probe. Use `flash` only when ready to replace its firmware; the tool flashes the existing ELF without rebuilding.
- [ ] Open the monitor at 115200 8N1, start a UART log, reset and keep the board still/upright for reference capture.
- [ ] Confirm USART1 PB6/PB7 AF7, no flow control, and the current queued polling backend: exact `HAL_UART_Transmit()` calls of at most 16 bytes, 2 ms HAL timeout, with interrupts and SysTick enabled.
- [ ] Check sensor health and actual values; an initialized mask is not proof that later readings are valid.
- [ ] If using the laptop display, close other ST-Link sessions first and verify the viewer is reading the matching flashed firmware. Use the [player guide](how-to-play.md) for controls and the [code guidebook](code-guidebook.md) for the implementation trace.

## Compulsory demonstration sequence

| Step | Action | Expected observation | Evidence to save |
| --- | --- | --- | --- |
| 1 | Reset with terminal open | One `Entering STANDBY MODE`; LED solid; one-second reports include all six quantities and units | Boot log and short video |
| 2 | Single press; wait beyond 500 ms | Remains standby, no charging | UART and display |
| 3 | Double press with 100–300 ms spacing | Battle entry on second edge; LED full cycle 1 s | Mode line and LED timing |
| 4 | On a zombie, press once; wait >500 ms; press once again | First resolved single yields 3/10; second charges to 6/10 and fires automatically, leaving 1/10 | Charge/fire lines, energy and sound |
| 5 | Double press in normal battle | Returns standby with solid LED; no extra charge from the first press | Mode/energy log |
| 6 | Re-enter battle; smoothly invert over 0.5–2 s and hold | Attacked state, rescue countdown, 2 Hz LED and attacked message every second | Sensor trace, countdown and LED video |
| 7 | Single press while attacked | Ignored; no charging | UART/energy |
| 8 | Double press comfortably before 10 s | Rescue to normal battle, 1 Hz LED; rotate upright before testing another attack | Rescue line and state transition |
| 9 | Attack again and provide no rescue | At 10 s terminal state; final notification then silence after bounded output drain; LED off; laptop accepts final frame, closes its debugger connection and shows SESSION ENDED | Continuous UART log through expiry; final viewer status and connection-close evidence |
| 10 | Press single/double after termination | No response and no further telemetry; no automatic viewer polling/reconnect; reset restarts standby, then explicit Reconnect after reset resumes viewing | Several seconds of UART silence, stopped viewer connection, reset/reconnect log |

The final output drain ends early when both UART and OLED are idle. `APP_SHUTDOWN_DRAIN_MS` gives a 600 ms loop budget for serialized UART chunks and the OLED frame, with a deadline check between their work. One already-started bounded UART or OLED call can finish slightly beyond that budget; this is not an exact 600 ms physical-cutoff guarantee. No new acquisition, game action or button handling occurs during this drain. UART idle includes the hardware transmission-complete flag before deinitialization.

The laptop reads the terminal flag inside the same coherent sequence-checked snapshot as the final image, then closes its owned OpenOCD/ST-Link connection and refuses further polling. It retains the final picture and requires board reset plus an explicit reconnect. This stops the viewer's debugger communication; it does not disable every possible external debugger electrically. Preserve the separate UART silence recording as direct evidence of application cutoff.

The queue uses bounded foreground polling rather than a UART TX interrupt. A 16-byte chunk normally occupies about 1.39 ms at 115200 8N1; HAL's 2 ms timeout can occupy roughly 3 ms because it uses millisecond ticks. Verify the current build rather than applying older interrupt-driven UART captures to this backend.

Use host tests to establish exact 499/500 ms and 9,999/10,000 ms boundaries; manual press timing alone cannot substantiate millisecond accuracy. A logic analyzer or controlled switch stimulus can add physical timing evidence. Tests also cover tick rollover, stale edge timestamps and continuous inversion/rearm behavior.

## Game and optional hardware

- [ ] Play a winning round: shoot zombies, wait out humans' four-second periods, and defeat six zombies without losing all three lives. Check +10 per zombie, +5 for each human avoided, victory screen and melody.
- [ ] Demonstrate reflective IR presence/absence and adjust its potentiometer. Aim lock adds +5 to a zombie hit; it never decides red/green identity or prevents automatic firing. An unplugged input should show no aim lock with default polarity.
- [ ] Shoot a human to show −5 and one lost life. Let zombies escape, or make further wrong shots, to reach zero lives and show defeat.
- [ ] After an ordinary victory/defeat, demonstrate that the compulsory battle button/LED/monitoring behavior remains available. Double to standby and double to battle for a new round.
- [ ] Verify charge, shot, warning, attack, rescue, victory and defeat sounds. An active buzzer produces on/off patterns rather than different pitches; select the matching build option.
- [ ] Boot with each optional module absent in turn. The compulsory state machine and UART must continue. An OLED absent at boot is probed again after reset; a previously working OLED retries failed transfers after one second. The colour module has separate reset recovery behavior.
- [ ] If an actual TCS34725 is added, enable and calibrate it separately; report it as an extra sensor. It is not the supplied reflective IR module.

## Threshold warning and recovery demonstration

First record a quiet baseline for every sensor and compare with reference instruments where available. Demonstrate simultaneous violations and the suppression of normal telemetry. Restore every value to its accepted range and show that full telemetry resumes at the next one-second report.

Physical examples include a nearby magnet moved gradually toward and away from the magnetometer or a controlled board rotation exceeding the gyro warning threshold. Keep such a demonstration separate from rescue-timeout testing so the intended transitions remain clear. Use measured, justified thresholds for the actual room; do not damage or wet the board to produce an environmental change.

Where a physical environmental excursion is impractical, explicitly label a **debugger-injected sensor-value demonstration**. It verifies software behavior, not sensor accuracy. One reproducible procedure is:

1. Start OpenOCD/GDB using the README commands; run to `main`, then continue until initialized. Add `break Telemetry_Check` and `continue`.
2. When stopped in `Telemetry_Check`, inspect `print *sample` and confirm all six physical readings/validity bits first. The startup call can occur immediately; continue to the desired scheduled sweep.
3. Inject known values in the debugger, for example:

   ```gdb
   set variable sensors.temperature_c = 46.0
   set variable sensors.humidity_pct = 10.0
   ```

   These examples exceed the default 45 °C maximum and fall below 20 %RH minimum. If those sensors were unavailable, do not describe an injected validity bit as a real reading. To exercise the formatter deliberately with a complete synthetic sample, declare that explicitly before changing `sensors.valid`.
4. Disable that breakpoint and continue. The current reporting cycle should contain both warning lines and suppress the ordinary `T:..., P:...` telemetry line. Normal hardware values overwrite the injected ones at the next sensor sweep, allowing recovery to be demonstrated.
5. Preserve the UART log and label exactly which values were injected. A debugger halt stops foreground timing while the MCU tick can behave differently under debug; use a separate reset and ordinary operation for rescue timing evidence.

`tests/test_telemetry.c` provides an additional deterministic software-only demonstration of simultaneous warnings, missing/NaN readings, inclusive limits and automatic recovery. `make host-test` runs it without altering a board.

## Assessment references and timing plans

Use the original Assignment 2 brief for the full 20-minute presentation/demonstration and required actual contribution appendix. Lab 5 page 18 describes a 15-minute presentation and an optional appendix; keep a compressed variant ready without treating those instructions as identical. The original brief consistently lists PPT/PPTX/PDF/KEY formats, whereas Lab 5 lists PPT. Keep the code ZIP and slides separate, confirm the actual lab-class deadline, and preserve editable draft identity/contribution fields until accurate details are available.

Lab 5's technical framework remains applicable: interrupt pushbutton, SysTick timing without application `HAL_Delay()`, UART messages, and USART1 PB6/PB7 AF7 at 115200 8N1 with no flow control. The reference's receive/echo exercises are optional and do not imply this game's UART accepts commands.

### Full 20-minute plan

| Time | Content |
| --- | --- |
| 0–3 min | Requirements, hardware map, honest division of completed contributions |
| 3–6 min | Operating-state, interrupt-input and foreground flowcharts |
| 6–10 min | Standby/battle, energy auto-fire, 1 Hz/2 Hz LED, six-sensor UART and warning/recovery demonstration |
| 10–14 min | Native laptop/IR/buzzer game, correct/wrong shots and round results; assess OLED image separately |
| 14–17 min | Attack, successful rescue, second attack and irreversible timeout |
| 17–20 min | Validation evidence, real problems and fixes, limitations and questions |

Rehearse the route to a winning round and confirm the target/press timing. Prepare a short recorded fallback as supplementary evidence if allowed, while retaining the live compulsory demonstration.

### Compressed 15-minute variant

| Time | Content |
| --- | --- |
| 0–3 min | Requirements, architecture/state flowcharts and actual contributions |
| 3–6 min | Button classification, +3 charging/automatic fire and laptop game feedback |
| 6–9 min | Six physical quantities, one-second UART and warning/recovery |
| 9–12 min | Physical attack, 2 Hz LED, successful rescue and upright rearm |
| 12–15 min | Second attack, timeout, final frame/polling stop, UART silence/reset and evidence limits |

Keep Q&A preparation for both versions. `tools/make_slides.py` defaults to the full plan; `--duration 15` generates the compressed slide run sheet. These are rehearsal allocations, not evidence of completed tests or confirmation of the assessed slot.

## Submission and contribution record

- [ ] Confirm the actual group's lab deadline in Canvas. The brief has different deadlines by lab section.
- [ ] Archive the complete project, source, drivers, linker/startup files, Makefile and tools; use the actual group number and actual matriculation numbers in the assignment's ZIP naming template.
- [ ] Submit slides separately using an accepted format and the corresponding real group/member identifiers. Include flowcharts, implementation, enhancements, problems/solutions and contribution appendix.
- [ ] Do not fabricate group numbers, identifiers, measured values, screenshots, test outcomes or either student's contribution.
- [ ] Replace the planning table below with the work each member actually performed; disclose AI assistance as required by the course and be able to explain the submitted code.

| Planned allocation only | Intended work | Actual work and evidence to fill in |
| --- | --- | --- |
| Player/game member, planned 50% | Button interrupt controls, energy/auto-fire, LED, IR interaction, game scoring and buzzer | Pending team declaration |
| Sensor/display member, planned 50% | OLED, six sensors, UART, thresholds and monitoring | Pending team declaration |
| Joint | Integration, rescue/termination, debugging, calibration, flowcharts/slides and demonstration | Pending team declaration |

The current repository is an AI-assisted draft spanning both planned halves. The table is a coordination proposal, not a statement that either person implemented or verified those components. Record actual review, changes, experiments, integration and presentation work honestly.
