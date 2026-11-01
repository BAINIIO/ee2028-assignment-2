# External game hardware

The existing hardware is a 0.96-inch OLED, buzzer and **infrared reflective sensor**. Firmware assumes a **128x64, I2C SSD1306** OLED at seven-bit address `0x3c`. Confirm the module's controller, resolution, supply rating and pin labels before wiring; 0.96-inch describes the size, not the controller. SH1106 and SPI displays need a different driver.

The OLED is monochrome. Garden Guard uses different animated zombie and friendly-human sprites with explicit `ZOMBIE` / `HUMAN` labels and a `HOLD FIRE` instruction for humans. The reflective IR sensor detects the presence of a nearby object and supplies an aim marker. It **cannot distinguish red from green**. Targets come from the game state. The optional TCS34725 driver is disabled by default because that module is not currently available.

## Wiring

**Current wiring reported on 1 November 2026:** OLED in a Grove I2C socket, reflective IR in A0/PC5, and buzzer in D2/PD14. Firmware defaults match this connection; audible buzzer operation on D2 has not yet been verified. Earlier D6 and D7 results below remain dated test evidence.

Power down before connecting modules. Use a common ground and **3.3 V logic**, including I2C pull-ups and the IR digital output. A module powered from 5 V may drive a 5 V output even if its label says it is Arduino-compatible. Verify its output voltage or use an appropriate level shifter.

| Module signal | B-L4S5I-IOT01A connection | Firmware use |
| --- | --- | --- |
| OLED VCC / GND | 3V3 / GND, if module supports 3.3 V | Module supply |
| OLED SCL | Arduino D15 / PB8 | I2C1 SCL, nominal 100 kHz |
| OLED SDA | Arduino D14 / PB9 | I2C1 SDA |
| IR VCC / GND | 3V3 / GND, if module supports 3.3 V | Module supply |
| IR digital OUT / DO | Arduino A0 / PC5 | Digital object presence, active-low by default |
| Buzzer control / signal | Arduino D2 / PD14 (CN3 pin 3) | Timer-driven square wave or digital on/off |
| Buzzer ground | GND | Common reference |
| Optional TCS34725 SCL / SDA | PB8 / PB9, shared with OLED | I2C address `0x29` |
| Optional TCS34725 supply | 3V3 / GND | Confirm breakout supply requirements |

The header labels match `Core/Inc/main.h` and [ST UM2708, table 6, page 23](https://www.st.com/resource/en/user_manual/dm00698410-.pdf): Arduino D2 is **PD14** and Arduino A0 is **PC5**. The buzzer default follows the user's D2 wiring reported on 1 November 2026. The A0 label identifies the header pin; a reflective module with a digital comparator output can use it as a GPIO input. A module with a true analog-only output would require a separate ADC driver. PB6/PB7 remain available for USART1 telemetry; the on-board six sensors and NFC tag use separate I2C2 on PB10/PB11. D2 does not overlap the NFC tag or native Wi-Fi pins. Do not connect another output to the buzzer or IR signal pins.

I2C requires external pull-ups to **3.3 V**; many OLED breakouts already include them. Do not add multiple low-value pull-ups in parallel. A typical short connection uses approximately 4.7 kOhm if pull-ups are absent. Keep wires short. The firmware deliberately avoids enabling the MCU's weak internal pull-ups for I2C.

The buzzer output is a **control signal**. A three-pin buzzer module with a suitable driver can receive it at `SIG`. A small passive piezo element may be connected through a series resistor if its current is within the STM32 GPIO ratings. A magnetic or high-current buzzer needs a transistor driver and, where required, a flyback diode; do not drive it directly from the GPIO. Determine whether the module is passive or active before selecting the option below.

## Configuration

`Core/Inc/app_config.h` provides defaults; `make EXTRA_DEFINES="..."` can override the `APP_ENABLE_*`, `APP_IR_ACTIVE_LOW`, `APP_BUZZER_ACTIVE` and `APP_BUZZER_ACTIVE_LOW` switches.

| Define | Default | Meaning |
| --- | --- | --- |
| `APP_ENABLE_OLED` | `1` | SSD1306 on external I2C1 |
| `APP_ENABLE_IR` | `1` | Digital reflective sensor on Arduino A0 / PC5 |
| `APP_IR_GPIO_PORT` / `APP_IR_GPIO_PIN` | `GPIOC` / `GPIO_PIN_5` | Configurable IR digital GPIO; also update its clock-enable macro when changing port |
| `APP_IR_ACTIVE_LOW` | `1` | `0` means object detected; use `0` for active-high modules |
| `APP_ENABLE_BUZZER` | `1` | Buzzer signal on Arduino D2 / PD14 |
| `APP_BUZZER_GPIO_PORT` / `APP_BUZZER_GPIO_PIN` | `GPIOD` / `GPIO_PIN_14` | Configurable buzzer GPIO; clock-enable macro defaults to `__HAL_RCC_GPIOD_CLK_ENABLE()` |
| `APP_BUZZER_ACTIVE` | `0` | Passive piezo: pitch-changing square-wave melodies |
| `APP_BUZZER_ACTIVE` | set to `1` for active buzzer | On/off patterns; active buzzers cannot reproduce the melody pitches |
| `APP_BUZZER_ACTIVE_LOW` | `0` | Set to `1` for a module whose control input is active-low; also sets the correct idle level |
| `APP_ENABLE_COLOUR` | `0` | Set to `1` only after adding a TCS34725-compatible colour module |

For an active buzzer:

```sh
make EXTRA_DEFINES="-DAPP_BUZZER_ACTIVE=1"
```

For an active-low active module, build with `make EXTRA_DEFINES="-DAPP_BUZZER_ACTIVE=1 -DAPP_BUZZER_ACTIVE_LOW=1"`. The build configuration stamp rebuilds objects when these defines change. The defaults keep passive operation until the module's type is confirmed.

Tune the IR module's potentiometer with a nearby diffuse target. Confirm that the digital output changes and the OLED's aim-lock indication follows. A 30 ms stable input period rejects chatter. A digital input has no device identification, so the firmware cannot prove an IR module is connected: `External_IREnabled()` reports configuration, not a successful hardware probe. The default pull-up makes an unplugged active-low input read as no object.

## Driver operation and failure behavior

`External_Init()` initializes each module independently after the application establishes its 80 MHz system clock. Missing I2C modules are probed once; one absent module does not prevent the other from working. A later colour-sensor transaction failure marks that sensor offline until reset. A previously working OLED retries after a one-second backoff, reconfigures the SSD1306 and restarts a complete frame from column/page zero. A busy bus defers OLED work without waiting or discarding the current transfer position. The compulsory controls, on-board sensor monitoring and UART continue without external I2C modules.

The SSD1306 uses a 1,024-byte drawing framebuffer and a separate 1,024-byte immutable transfer snapshot. `External_SetDisplay()` draws immediately, including while a previously working display awaits recovery. `External_Poll()` sends at most one 16-byte data chunk or short addressing/configuration command per call. OLED transfers have an 8 ms HAL timeout; boot probes and colour transfers retain 5 ms. A bus-busy check avoids HAL's separate 25 ms busy wait. There are no delays, polling loops that wait for a module, or repeated absent-device probes in the main loop. Updating the drawing framebuffer during a transfer finishes the existing snapshot and schedules the newest complete frame next. The snapshot prevents changes to data still being transmitted. Inspectable counters retain failures, BUSY deferrals, recoveries and the last HAL status/error. UART records display offline/recovered transitions, allowing stability observation without a live SWD session.

When no target is displayed, the application supplies a context-specific control hint rather than presenting controls that are unavailable in standby, attacked or terminal states. During termination, `External_FinishDisplay()` advances only pending OLED transfers; it does not sample IR or the colour sensor or initiate OLED recovery. `External_DisplayIdle()` reports whether the screen has finished or is offline, allowing the application to bound its final screen/UART drain before shutdown.

The passive driver uses a 1 MHz TIM2 counter with update interrupts at twice the requested pitch, toggling D2/PD14 for a square wave without requiring a TIM2 alternate-function pin. The highest pitch (2400 Hz) requires 4800 short interrupts per second. Notes and gaps remain tick-driven; no foreground delays generate sound. Idle gaps and `External_AllStop()` disable timer interrupts and restore the inactive GPIO level. Active modules use GPIO on/off control without TIM2 interrupts. Attack/rescue sounds preempt routine sounds; defeat has the highest priority.

The optional TCS34725 driver checks device ID `0x44` or `0x4d`, selects 4x gain and 50.4 ms integration, powers the oscillator for at least 3 ms before enabling conversion, and reads valid RGBC samples after integration. Channels are normalized to clear intensity. Red or green must dominate both other colour channels by `APP_COLOUR_RATIO`; dark, saturated or ambiguous samples do not claim a colour. `APP_COLOUR_MIN_CLEAR` and the ratio need calibration for lighting and distance. Keep the sensing surface separate from the reflective sensor's output: they are different measurements.

## Bench checks still required

1. Confirm the OLED controller/address and check the display orientation.
2. Verify the IR module's supply and digital-output voltage with its datasheet or meter; test both present and absent states and adjust its potentiometer.
3. Identify the buzzer type, select passive square-wave or active on/off mode, and verify the driver/current requirements.
4. Demonstrate the game with each optional module unplugged in turn; boot must continue without hanging.
5. If a TCS34725 is added later, calibrate red/green classification under the actual demonstration lighting.

On 22 October 2026 the user confirmed audible buzzer tones on D7, visible OLED text and then the Garden Guard animation. The **infrared reflective sensor v1.2** was checked with a steadily held white card: PC5/A0 went low and the game set aim lock; removal returned PC5 high and cleared aim lock. The first animation revision also suffered a real I2C acknowledgement failure, motivating the recovery behavior above. See the [original hardware report](hardware-test-2026-10-22.md) and [animation test report](animation-test-2026-10-22.md) for revision-specific evidence and remaining checks.

On 24 October 2026 the user corrected the current buzzer connection to **D6/PB1**. The default port, pin and GPIO clock were updated, the ARM build and existing host tests passed, and the firmware was programmed and verified. Software button interrupts exercised the existing game sounds; live reads observed PB1 high and low with TIM2 running, followed by an idle low output. The user confirmed, **"Yes, it works now"**. See the [D6 test evidence](../output/hardware-tests/20261024-150023-buzzer-d6/summary.json). This confirmation concerns audible operation; pitch and electrical waveform were not measured.

The OLED initially remained absent with its Grove cable in D3. The user moved it to a Grove I2C socket, and the next reset detected it at `0x3c`. An eight-second UART capture recorded no display failures; the following live snapshot showed the display online with zero failure/recovery counters and no HAL error. The user reported **"Some pixels, but damaged or distorted"**. The connection is restored, but image quality remains impaired and physical panel damage has not been independently confirmed. See the [I2C retry evidence](../output/hardware-tests/20261024-oled-i2c-retry/summary.json).

## Device references

- [ST B-L4S5I-IOT01A user manual (UM2708)](https://www.st.com/resource/en/user_manual/um2708-discovery-kit-for-iot-node-multichannel-communication-with-stm32l4-series-stmicroelectronics.pdf)
- [ams OSRAM TCS3472 datasheet](https://look.ams-osram.com/m/7ec5bcc3e40679be/original/TCS3472-DS000390.pdf)
- [Solomon Systech SSD1306 datasheet, hosted by Adafruit](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)
