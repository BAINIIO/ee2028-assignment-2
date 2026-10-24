# Garden Guard animated OLED game

Garden Guard is an original pixel-art plant shooter inspired by the lawn-defense idea of Plants vs. Zombies. It runs on the existing 128×64 monochrome SSD1306 display. It uses code-drawn artwork and no downloaded game assets.

## Play

1. Double-press the **blue USER/B2** button to start.
2. On `ZOMBIE`, make single presses at least one second apart. Each resolved single charges +3/10; reaching 5/10 automatically fires a pea and consumes 5/10.
3. On `HUMAN`, hold fire until the friendly target leaves. Shooting a human costs a life.
4. Defeat six zombies to win. Three lost lives end the round. Double to standby, then double again for a new round.
5. If the board is flipped and the attacked screen appears, double-press within the ten-second countdown to rescue it. After termination, use the **black RESET/B1** button.

The plant and walking sprites animate between input events. Peas and impact bursts show resolved shots; the scoring decision occurs immediately under the existing game rules. Inputs keep the assignment's 500 ms single/double recognition delay. Targets keep their existing four-second lifetime, including while a previous shot effect is shown. A target's position reflects its remaining lifetime. Visuals never extend a target or rescue deadline.

## Code and timing

`Core/Src/game_display.c` is a portable renderer. `GameDisplay_Render()` consumes an `ExternalDisplayFrame` and writes exactly one 1,024-byte SSD1306 page-packed frame. It does not read GPIO, query clocks, modify game state, allocate memory or wait. Rendering depends only on the supplied snapshot and timestamps.

`main.c` maps resolved game events to presentation effects. It captures the previous target's age before the following target event, allowing a projectile and hit effect to refer to the target actually shot. The next target continues using the game clock. Standby/battle entry clears previous effects; attacked, win/loss and terminated scenes take priority.

The foreground loop renders a frame every **33 ms**, about **30 frames/s** for the laptop viewer. The external I2C bus remains at its working **100 kHz** setting, so the physical OLED displays fewer frames and skips superseded ones. `external_io.c` copies each frame into an immutable transfer buffer and sends at most a 16-byte chunk per poll, with an 8 ms OLED timeout. Rendering while a transfer is active queues the newest complete screen. Actual frame delivery depends on bus and sensor traffic. After a failed transfer, a previously working OLED retries after one second and restarts a complete current frame. BUSY defers work; terminal draining never initiates recovery. UART records display failures and recovery transitions for observation without a live SWD connection.

The existing EXTI queue, 30 ms debounce, 500 ms double-press boundary, 50 ms motion sampling, one-second six-sensor telemetry, buzzer scheduling, LED rules and terminal shutdown are independent of the animation timeline. Hardware failures still allow the compulsory controls and UART to continue.

## Preview and checks

```sh
make host-test
make
python3 tools/preview_display.py
```

The preview compiles and runs the actual C renderer on the host and converts its one-bit frames to a GIF and PNG contact sheet. It demonstrates drawing and animation, not physical I2C timing. The renderer tests exercise clipping, extreme field values, repeatability, distinct characters and changing animation frames. Application integration tests cover the control/sensor schedules and event-to-display wiring.

The [earlier hardware report](hardware-test-2026-10-22.md) records the text-display revision. The [animation test report](animation-test-2026-10-22.md) records visible animation, a real OLED acknowledgement failure, recovery verification, and successful IR card detection/release. Correct live zombie hits, rescue and timeout still require observation on the board.
