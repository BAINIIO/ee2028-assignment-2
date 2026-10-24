# Connected-board test - 22 October 2026

Partial hardware verification on the user's STM32L4S5 Discovery board through its ST-Link connection. Times below are Singapore time (UTC+08:00). This records observed results separately from host simulations and uncompleted bench checks.

This snapshot covers the firmware identified below and evidence through 10:18. The user subsequently requested a Plants vs Zombies inspired animated OLED game. That upcoming revision requires its own build, flash and visible-game verification; the checks here must not be read as verification of that revision.

## Firmware and retained evidence

- The original 2 MiB flash image was backed up before programming: [original-flash.bin](../output/hardware-tests/20261022-100145/original-flash.bin). Preserve this first backup; the later smoke-test backup contains the firmware already installed at that later time.
- The initial game image was programmed and verified; see the [first flash log](../output/hardware-tests/20261022-100145/flash.log) and [boot UART](../output/hardware-tests/20261022-100145/uart.json).
- After correcting the external module pins, the updated firmware was programmed and verified: [flash log](../output/hardware-tests/20261022-101328/flash.log), [summary](../output/hardware-tests/20261022-101328/summary.json), [boot UART](../output/hardware-tests/20261022-101328/uart.json). Image SHA-256: `9a3f4cb3f3a09b2fa476aa418598e1a79b558a371c6d958037b8a0c6077f4e91`. Linked size: 46,692 bytes flash; 20,792 bytes RAM including reserved heap/stack.
- Real manual controls and game events are in [controls-uart.log](../output/hardware-tests/20261022-100145/controls-uart.log). Startup/reset transitions can include partial serial bytes; use complete timestamped lines as evidence.
- After connecting the OLED, a remote reset without rewriting flash reprobed the display. The [OLED check UART](../output/hardware-tests/20261022-101328/oled-check/uart.log) reported `OLED=online` and `sensors=0x3f/0x3f`; the user confirmed the visible standby score/energy/lives HUD.

## Observed results

| Check | Result and evidence | Limit |
| --- | --- | --- |
| Flash and boot | Programming verification passed. UART reported standby and `sensors=0x3f/0x3f`. | Repeated cold starts and power robustness not characterized. |
| Six compulsory sensor readings | Read-only SWD HOTPLUG snapshot, without halt/reset/input injection: [snapshot.json](../output/hardware-tests/20261022-100145/live-snapshot/snapshot.json). All six valid (`0x3f`); values below. | Plausible converted values do not establish reference accuracy or dynamic response. |
| Manual start | Blue USER/B2 double press logged `Entering BATTLE MODE` at 10:06:40.094. | Strict 499/500 ms boundary and GPIO edge timing remain host-tested only. |
| LED feedback | User confirmed the LED was blinking after battle entry. | No measured 1 Hz/2 Hz waveform or physical attacked state. |
| Charging and automatic firing | Separate manual singles: 3/10 at 10:07:13.322; 6/10 at 10:07:17.438; automatic shot at 10:07:17.443 consumed 5/10 and left 1/10. | This occurred after round defeat, with `target=NONE`; it does not demonstrate a zombie hit or a winning round. |
| Game progression | Real UART showed human avoidance increasing score by 5, escaped zombies deducting 5 and a life, and `DEFEAT! No lives left.` at 10:07:03.933. | Ordinary round defeat leaves compulsory battle controls running. Rescue-timeout termination is a separate path and was not physically verified. |
| Threshold UART and recovery | Magnetic magnitude roughly 213–223 µT exceeded the configured 200 µT limit, giving warning-only UART at roughly one-second intervals. At 10:17:58.410, full normal six-sensor UART resumed with M=(13.59, -196.21, -23.28) µT, magnitude below 200 µT; subsequent normal lines remained roughly one second apart. | Simultaneous faults and controlled repeated recovery tests not captured. Magnetic baseline/calibration remains unresolved; the cause of the field change was not established. |
| Buzzer | Initially silent. After changing output to D7/PA4 and reflashing, user confirmed audible sound. | Buzzer type, electrical waveform, exact pitch and each individual sound sequence are not instrumentally verified. |
| IR | Reported wiring is A0; firmware now reads PC5 as a digital input with active-low presence and debounce. Module identified by the user as Infrared Reflective Sensor v1.2; its red indicator responds to a white card, but the first present/remove check produced no reported OLED indicator change. | End-to-end presence detection is unresolved; verify signal behavior, range and aim bonus. `IR=enabled` in the boot log means configured, not a successful physical probe. |
| OLED | Absent in the first two smoke tests. After connection and reset, UART reported `OLED=online`; user confirmed visible standby text with score/energy/lives. | Target animation, rescue countdown, result screens and display orientation across the full game remain unverified. |

The live sensor snapshot contained:

| Quantity | Reading |
| --- | --- |
| Temperature | 30.20 °C |
| Pressure | 1010.20 hPa |
| Relative humidity | 59.96 %RH |
| Acceleration (x, y, z) | (-0.122, -0.078, 9.997) m/s² |
| Angular rate (x, y, z) | (-0.28, -1.96, 0.28) °/s |
| Magnetic field (x, y, z) | (-30.338, -217.658, -23.562) µT |

The snapshot also showed a valid upright reference and attack detection armed. No physical inversion/rescue is claimed from this state alone.

The later full normal UART report at 10:17:58.410 was:

```text
T: 30.51 degC, P: 1010.14 hPa, H: 58.45 %RH, A: [-0.12,-0.04,9.98] m/s^2, G: [-0.35,-1.96,0.28] deg/s, M: [13.59,-196.21,-23.28] uT
```

This confirms recovery from the previously captured warning-only output to physical-unit telemetry for all six sensors. It does not calibrate those readings or establish precisely when the field crossed the threshold between captures.

## Wiring finding and correction

The actual connector differs from the initial assumed pin assignment:

| Device | Initial assumption | Reported actual connection / current firmware |
| --- | --- | --- |
| Buzzer signal | Arduino D1 / PA0 | Arduino D7 / PA4 |
| Reflective IR signal | Arduino D0 / PA1 | Arduino A0 / PC5 |
| OLED | I2C1 | Arduino D15 / PB8 SCL; D14 / PB9 SDA; compatible 3.3 V supply and common ground |

PA4 does not provide the originally assumed direct TIM2 channel output. The passive buzzer now toggles its GPIO from TIM2 update interrupts; active modules use configurable on/off output. The A0 connector can read a reflective module's digital comparator signal as GPIO; an analog-only sensor would need an ADC implementation. See [external hardware](external-hardware.md) for configuration and wiring.

## Remaining test work

1. Check OLED target/energy/rescue/result screens, then test IR present/absent response and aim bonus.
2. Capture in-round firing against both target types and a complete winning round.
3. Perform a smooth physical inversion; confirm attacked UART, 2 Hz LED, rescue before 10 s, upright rearm, and ignored attacked singles.
4. Let a rescue expire; confirm final output, communication silence, ignored presses and reset recovery.
5. Measure LED/button timing and sensor acquisition cadence; compare sensor readings to references, justify the magnetic threshold and repeat controlled warning/recovery tests.

Host verification remains 239 game/button checks, telemetry tests and six application scheduler scenarios. Additional external-I/O sanitizer checks covered configuration variants, the revised buzzer GPIO/timer behavior and IR debounce. These automated results supplement the partial physical evidence; they do not complete the remaining bench tests.
