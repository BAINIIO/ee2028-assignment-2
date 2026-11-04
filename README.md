# EE2028 Assignment 2 — Garden Guard

Firmware for the **B-L4S5I-IOT01A / STM32L4S5** combines the compulsory scouting/battle drone controls with **Garden Guard**, an original plant-versus-zombies game displayed on the 128×64 monochrome OLED or in a native laptop window. A pea-shooting plant defends a lawn against walking zombies while friendly humans must pass unharmed. The six on-board sensors, one-second UART monitoring, interrupt pushbutton, LED patterns, energy rules and attacked/rescue state remain active alongside the game.

**Status:** automated host tests pass. Recorded firmware revisions have been programmed and verified on the connected STM32L4S5; use each log's image identity when assessing the current build. All six sensors produce readings, the physical blue USER/B2 button enters battle, UART confirms charging/automatic firing, and the user previously confirmed blinking LED, audible buzzer and visible Garden Guard animation. A white-card test verified IR detection and release at PC5/A0 through to the game's aim-lock state. The display is a **128×64 I2C SSD1306-compatible OLED at `0x3c`**. On 24 October the buzzer was confirmed audible after correcting its pin to D6/PB1. Moving the OLED cable from D3 to a Grove I2C socket restored detection, with no communication failures during an eight-second capture, but the user reports damaged or distorted pixels; physical panel damage is suspected, not confirmed. See the [latest OLED test](output/hardware-tests/20261024-oled-i2c-retry/summary.json) and the earlier [animation test report](docs/animation-test-2026-10-22.md). Complete physical rescue, timeout and winning-round verification remain pending. Reflective IR detects proximity, not red/green colour; OLED targets determine identity and IR presence gives an aim bonus.

The [24 October compliance update](docs/compliance-update-2026-10-24.md) records the revised polling-UART and terminal-aware viewer build. The programmed board reported all six sensors valid and OLED online, continued approximately one-second UART warnings, and supplied 175 coherent laptop frames in six seconds (about 29.18 fps) without recorded UART transmit failures or dropped bytes. An explicitly debugger-injected timeout exercised the final frame/connection close and final UART notification followed by three seconds of silence. This verifies that hardware-executed path; it does not establish physical inversion or a successful manual rescue.

**Current wiring, 1 November 2026:** OLED in a Grove I2C socket, reflective IR in A0/PC5, and buzzer moved to D2/PD14. Firmware defaults now match D2; audible operation on this new connection has not yet been verified. The D6 result above is dated evidence for the previous wiring.

## Build and run

The [Wi-Fi/NFC command-centre upgrade](docs/command-centre.md) adds a responsive
phone dashboard, native board Wi-Fi transport, NFC mission links and field
check-ins. On 1 November, NFC URL programming/readback and opening the website
on the iPhone were verified. Native board Wi-Fi telemetry is still being
checked; opening the website alone does not establish live sensor delivery.
The current wiring is listed above. The dashboard source is in
`web/command-centre/dist/`.

The [hosted page](https://garden-guard-gg01.spunkybroom4.chatgpt.site/drone/GG01)
shows just the actual **OLED image** and **UART messages**. See the
[cloud hosting guide](docs/cloud-hosting.md) for the capture command and NFC
URL. The page remains available when the Mac is off; live outputs require
the board and capture relay. The public page is read-only, and native board
Wi-Fi association remains unverified.

Clone the private GitHub repository with an account that has access:

```sh
git clone https://github.com/BAINIIO/ee2028-assignment-2.git
cd ee2028-assignment-2
```

Run these commands in this `Assignment 2` directory. Prerequisites are `make`, Python 3 and either an Arm GNU toolchain on `PATH` or macOS STM32CubeIDE in `/Applications/STM32CubeIDE.app`. The wrapper scripts discover CubeIDE's bundled compiler, programmer and debugger without changing your editor profile.

```sh
make
make host-test
```

`make` produces `build/firmware/zombie_game.elf`, `.bin`, `.hex` and `.map`; it does not touch a board. `make host-test` uses Clang, AddressSanitizer and UndefinedBehaviorSanitizer. The game/button suite passes **239 checks** and the renderer suite **8,346 checks**. Telemetry tests cover units, simultaneous violations, missing/non-finite data, recovery and output bounds. Application scenarios run the actual foreground loop with simulated hardware to check EXTI timing, sensor schedules, warning recovery, rescue, visual-event ordering, tick wrap, UART draining and irreversible shutdown. A fake-HAL test reproduces partial OLED NACK, BUSY and timeout failures and checks complete-frame recovery. The UART backend test exercises bounded polling, queue wrap/full, partial timeouts, stuck/recovery behavior and transmission-complete idle. These checks do not verify physical hardware.

In the existing parent **C 2028** VS Code workspace, **⌘⇧B** runs the default **STM32: build zombie game** task. **Terminal → Run Task** also offers the host tests and board detection/flash tasks. The command-line workflow below works when this project directory is opened by itself or exported separately.

Connect a data-capable USB cable to the board's **ST-LINK connector CN7**, then:

```sh
python3 tools/board.py detect
python3 tools/board.py flash
```

`detect` lists interfaces without connecting to or changing firmware. `flash` explicitly programs and verifies the **existing** `build/firmware/zombie_game.elf`, then resets the board. It does not rebuild, preserving the options used for that ELF. Run `make` with the intended options immediately before flashing. With multiple probes, use `--serial SERIAL_FROM_DETECT`.

For a UART terminal, install the optional monitor dependency in a local virtual environment:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install pyserial
.venv/bin/python tools/monitor.py --port /dev/cu.usbmodemYOUR_PORT --log uart-demo.log
```

Replace the port with your ST-Link virtual COM port; omitting `--port` auto-selects one STM32 port when unambiguous. The monitor is receive-only, uses **115200 baud, 8 data bits, no parity, 1 stop bit, no flow control**, and timestamps receipt on the computer. Reset the board with the monitor open to capture the boot message. The physical user button controls the game.

Firmware formats messages with `snprintf`/`vsnprintf`, copies them into a 4,096-byte queue, and sends at most 16 contiguous bytes per `Board_UartPoll()` using the exact `HAL_UART_Transmit()` function with a 2 ms HAL timeout. Interrupts and SysTick remain enabled during this bounded foreground polling. Normal wire time is about 1.39 ms per 16-byte chunk; millisecond tick quantization can make a timeout take roughly 3 ms. It does not use `printf` retargeting, semihosting or UART TX interrupts. The queue's idle check includes the hardware transmission-complete flag.

For debugging, start these commands in two terminals:

```sh
python3 tools/board.py debug
```

```sh
python3 tools/board.py gdb
```

The debug command starts ST's OpenOCD and halts/resets the board; GDB connects on port 3333 and breaks at `main`. Use `continue` to run the firmware already present. Use `load` only when deliberately programming the selected ELF. The board tool supports `STM32_PROGRAMMER`, `STM32_OPENOCD` and `STM32_OPENOCD_SCRIPTS` overrides when the tools are installed elsewhere.

To use CubeIDE, choose **File → New → Makefile Project with Existing Code**, select this directory and configure the external `make` command. Use the root `Makefile`. Historical `Debug/makefile`, `Assignment2.launch` and starter examples are retained only in the original local workspace and are excluded from Git. They do not describe this application. Semihosting is not used for telemetry.

## Connections and configuration

See [wiring and module identification](docs/external-hardware.md) before connecting external modules. The short pin map is:

| Function | Connection |
| --- | --- |
| OLED SCL / SDA | PB8 / PB9, Arduino D15 / D14, I2C1 |
| Reflective IR digital output | PC5, Arduino A0 header, active-low by default |
| Buzzer signal | PD14, Arduino D2 (CN3 pin 3), timer-driven GPIO tones |
| External supply/reference | Suitable 3.3 V supply and common ground |
| Six on-board sensors | Existing I2C2, PB10 / PB11; no extra wiring |
| UART telemetry | USART1 PB6 / PB7 through ST-Link VCP |
| User button / status LED | PC13 EXTI / PB14 LED2 |

The D2/PD14 mapping is specified in [ST UM2708, table 6, page 23](https://www.st.com/resource/en/user_manual/dm00698410-.pdf). Use 3.3 V logic and I2C pull-ups. Identify the buzzer's type and driver/current needs; D2 is a control signal. The reflective module uses a digital signal even though it is connected to the A0 header. Optional modules can be disabled independently:

```sh
make EXTRA_DEFINES="-DAPP_ENABLE_OLED=0 -DAPP_ENABLE_IR=0 -DAPP_ENABLE_BUZZER=0"
make EXTRA_DEFINES="-DAPP_BUZZER_ACTIVE=1"
```

The second example selects active-buzzer on/off patterns; passive square-wave melodies are the default. TIM2 interrupts toggle D2/PD14 without requiring a hardware PWM alternate function. The buzzer defaults follow the user's D2 wiring reported on 1 November 2026; audible operation on D2 is pending verification. Dated hardware reports retain the earlier D6 and D7 results. Port/pin/clock macros and active-low polarity are configurable in `app_config.h`. Include all desired overrides in each build command. The configuration stamp tracks flags and automatically rebuilds affected objects when `EXTRA_DEFINES` changes, including a return to plain `make`. Defaults and empirical thresholds are in [app_config.h](Core/Inc/app_config.h); game constants are in [game_config.h](Core/Inc/game_config.h).

## Assignment and Lab 5 references

Lab 5 supports the implementation framework: an interrupt-driven pushbutton, SysTick timing without application `HAL_Delay()`, and messages sent through UART. Its serial setup is USART1 on PB6/PB7, AF7, 115200 8N1 without flow control; this firmware uses alternate-function push-pull with pull-ups and bounded `HAL_UART_Transmit()` calls.

The original Assignment 2 brief specifies a **20-minute presentation and demonstration**, an appendix declaring actual joint and individual contributions, and accepted slide formats including PPT/PPTX/PDF/KEY. Lab 5 page 18 instead describes **15 minutes**, an optional contribution appendix and PPT. Follow the original assignment for the full preparation, retain the required contribution appendix, and rehearse a compressed 15-minute variant as well. Confirm a newer teaching-team instruction and the actual lab-class deadline before submission. HTML appears in one assignment paragraph but is omitted from its later accepted-format list; use a consistently listed format unless confirmed otherwise. These references do not supply this group's identifiers or establish its members' completed contributions.

## Controls and game rules

Boot enters standby with the LED continuously on. Double press to enter battle. Two accepted press edges must be **strictly less than 500 ms** apart; 500 ms is two singles. A 30 ms debounce rejects bounce. A single is resolved only after its 500 ms window expires, so the first half of a double never charges.

| State | Single press | Double press | LED |
| --- | --- | --- | --- |
| Standby | Ignored | Enter battle | Always on |
| Battle | Charge +3/10, capped at 10/10; automatically fire whenever the resulting charge reaches 5/10, consuming 5/10 | Return to standby | 1 complete blink per second |
| Attacked battle | Ignored | Rescue if the second edge occurs before the 10-second deadline | 2 complete blinks per second |
| Terminated | Ignored | Ignored | Off; physical reset restarts |

There is no separate shoot button. From empty, resolved single presses leave energy at **3, 1, 4, 2, 0, …** because firing is automatic. Aim the charging/firing decision at the displayed target:

| Event | Effect |
| --- | --- |
| Shoot a zombie | +10 points; +5 additional points when IR aim lock is present |
| Avoid a human for its full 4-second target period | +5 points |
| Shoot a human | −5 points and lose one life |
| Let a zombie's 4-second target period expire | −5 points and lose one life |
| Defeat 6 zombies | Victory message and sound |
| Lose all 3 lives | Defeat message and sound |

Target sequences are deterministic and reproducible. The monochrome OLED distinguishes humans and zombies by silhouette and labels. Reflective IR cannot classify colours; it never blocks mandatory auto-fire. A round result leaves compulsory battle controls, sensors and rescue monitoring active. Double to standby, then double to battle to start a fresh round. Energy persists across mode switches; score, lives and hit count reset on battle entry.

## Animated OLED game

### Live laptop display

The original game can be shown in a laptop window while the STM32 runs the
game and reads the physical USER button and sensors. The buzzer is configured
for D2/PD14. The OLED can be disconnected: the laptop frame updates independently.

After building and flashing the current firmware, double-click
`tools/live_display.command` on macOS, or run:

```sh
sh tools/live_display.command
```

The launcher uses an available Python with Tk 8.6 or later; on this Mac it
selects `/opt/anaconda3/bin/python`. Keep USB connected to ST-Link/CN7.
Double-press the blue USER button to start, then make single presses to charge
and automatically fire. Hold fire when a human appears. Escape or the window
close button closes the viewer. The board continues running after it closes.

The viewer reads the actual 128×64 framebuffer over ST-Link with a 30 fps
target, enlarging each pixel without replacing the game renderer. Its status
shows the measured received frame rate. Aligned 32-bit reads reduce USB/debug
overhead, and deadline-based polling accounts for time spent reading a frame.
It verifies that the board's firmware matches the local ELF/BIN, then attaches
without halting or resetting. Sequence checks discard frames that changed
during a read. A disconnected board is shown as disconnected, with the last
frame paused. Close the viewer before using another ST-Link debugger or
programmer. `APP_ENABLE_LAPTOP_DISPLAY=0` removes the mirror from a build.

The final terminated image publishes a terminal flag inside the same coherent snapshot. After reading it, the viewer closes its owned OpenOCD/ST-Link connection and stops debugger polling automatically. It retains the final image with **SESSION ENDED** and does not reconnect on its own. Reset the board, then explicitly choose **Reconnect after reset** for a new session. A final picture remains visible after the connection closes; it is not fresh board activity.

For an explicitly labelled offline animation of the actual renderer, use
`sh tools/live_display.command --preview`. This mode does not connect to a board.

### OLED animation

The display uses original pixel artwork, with a swaying plant, walking targets, flying peas, hit bursts and result scenes. The HUD shows score, hearts, zombie progress and energy. `ZOMBIE` means charge to shoot; `HUMAN` / `HOLD FIRE` means let the friendly target pass. The energy meter follows the compulsory +3 charge / −5 automatic shot rules. An aim marker shows the reflective IR state and a small warning indicator shows abnormal or unavailable sensor readings.

Frames are rendered every 33 ms (about 30 per second) for the laptop. The physical OLED remains limited by its 100 kHz I2C bus and sends the latest complete frame when it can, skipping superseded frames. Its driver sends one bounded 16-byte chunk per foreground iteration, so animation does not wait for a whole screen transfer or alter button deadlines. Each transfer uses an immutable snapshot. The display follows game events; projectile flight is a visual effect after the shot has been scored. Attacked mode takes priority and shows the rescue countdown; termination queues a static reset screen and drains pending output within its shutdown budget.

A previously working OLED recovers from a failed transfer by retrying after one second and redrawing a complete current frame. Bus BUSY defers work. Recovery does not run during terminal shutdown; a missing display at boot is not repeatedly probed.

Generate a preview from the actual firmware renderer on your Mac:

```sh
python3 tools/preview_display.py
```

The tool writes an animated GIF and a contact sheet under `output/game-preview/`. It needs Clang and Pillow; no board is needed. See [animation implementation and verification](docs/animated-game.md) for the rendering interface and remaining physical checks.

Keep the board still in its intended upright orientation after reset so the first stable acceleration/gyro sample establishes a gravity reference. To demonstrate an attack, flip it smoothly over about **0.5–2 seconds**, then hold inverted. Acceleration supplies absolute tilt relative to the reference; integrated gyro motion corroborates rotation. Inversion must persist at least 350 ms. A gyro alone cannot identify absolute upside-down orientation. After rescue, return upright before the next attack can arm.

The second rescue press must arrive **before 10,000 ms**; exactly 10,000 ms is too late. Terminal state cannot be reversed by another press or sensor update. The application emits its final notification and drains queued UART/display/network output plus bounded NFC shutdown attempts, ending as soon as they finish. `APP_SHUTDOWN_DRAIN_MS` gives a **600 ms** loop budget, with a deadline check between their work. One already-started bounded peripheral call can finish slightly beyond that budget. No further acquisition, game actions or button handling occurs during the drain. The Wi-Fi module is stopped, UART is deinitialized, buzzer/interrupt processing stops and the firmware waits for hardware reset. The laptop stops its own debugger polling after its coherent terminal frame; UART silence remains the direct application-cutoff evidence. See the command-centre guide for the NFC shutdown verification limits.

## Evidence and handover

- [Player guide PDF](output/pdf/Garden_Guard_Player_Guide.pdf) and [editable player guide source](docs/how-to-play.md)
- [Wi-Fi/NFC command-centre setup and current verification](docs/command-centre.md)
- [Code guidebook PDF](output/pdf/Garden_Guard_Code_Guidebook.pdf) and [editable code guidebook source](docs/code-guidebook.md)
- [Requirement-to-code matrix, units and threshold rationale](docs/requirements.md)
- [State, interrupt-input and foreground flowcharts](docs/flowcharts.md)
- [Bench verification, demonstration and submission checklist](docs/demo-checklist.md)
- [External hardware details](docs/external-hardware.md)
- [Animated game and IR hardware test](docs/animation-test-2026-10-22.md)
- [Current UART/viewer compliance and hardware verification](docs/compliance-update-2026-10-24.md)
- [15-slide presentation draft](output/pdf/Assignment2_DRAFT_Slides.pdf)

To create a source archive and a separate slides PDF:

```sh
python3 tools/package.py
```

Files go into `output/submission/` with `DRAFT` names. After entering the actual team information in the slides, use `python3 tools/package.py --group YOUR_GROUP --members YOUR_MATRIC PARTNER_MATRIC` to name the files. Substitute the real identifiers. The package includes application source, drivers, linker/startup files, tools, tests, documentation, both guide PDFs and their illustrations, and the current compliance logs. Historical starter examples in `reference/`, generated `Debug/` artifacts and flash backups are excluded. Nothing is uploaded or submitted.

The guide generator is [tools/make_guides.py](tools/make_guides.py), using ReportLab, markdown-it-py and Pillow. With the Python environment from the terminal setup above, regenerate both PDFs and the presentation from their editable sources:

```sh
.venv/bin/python -m pip install reportlab markdown-it-py Pillow
.venv/bin/python tools/make_guides.py
.venv/bin/python tools/make_slides.py
```

`make_guides.py` writes both guides to `output/pdf/`; `--output DIRECTORY` selects another destination. The presentation source is [tools/make_slides.py](tools/make_slides.py). Its default run sheet is 20 minutes. Generate a separately named compressed variant with:

```sh
.venv/bin/python tools/make_slides.py --duration 15 \
  --output output/pdf/Assignment2_DRAFT_Slides_15min.pdf
```

Rebuild after editing content, group/contribution placeholders or physical evidence. The player and code guides explain use and implementation; they do not replace the separately submitted slides or establish completed bench tests. Slides remain separate from the code ZIP, as the brief requires.

The current code is an AI-assisted implementation draft. Team members must inspect, understand, calibrate, physically test and accurately declare their actual individual and joint work before submission. The proposed 50–50 work split is a planning allocation, not evidence that those contributions have already occurred.
