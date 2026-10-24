# System flowcharts

These Mermaid diagrams describe the implemented application. Source functions are in `Core/Src/main.c`, `game.c`, `button.c`, `telemetry.c`, `board_io.c` and `external_io.c`. Render the diagrams with a Mermaid-capable Markdown viewer or copy the rendered figures into presentation slides.

## Operating states

```mermaid
stateDiagram-v2
    [*] --> Standby: reset / emit standby entry
    Standby --> Standby: single ignored / LED solid
    Standby --> Battle: double / emit battle entry
    Battle --> Battle: single / charge +3 then auto-fire if energy >=5
    Battle --> Standby: double / LED solid
    Battle --> Attacked: gyro corroboration + sustained inverted gravity
    Attacked --> Attacked: single ignored / LED 2 Hz / attacked log every 1 s
    Attacked --> Battle: double second edge before 10000 ms / rescue
    Attacked --> Terminated: elapsed >=10000 ms
    Terminated --> [*]: final queued output drain, UART off, wait for reset
    note right of Battle
      LED 1 Hz; six-sensor report every 1 s
      Target game: 6 zombie hits wins; 3 lost lives loses
      Round result retains battle controls and attack monitoring
    end note
    note right of Attacked
      Target timeout paused
      Rescue restarts target's 4-second window
      Return upright after rescue to rearm attacks
    end note
    note right of Terminated
      No button or sensor can restart firmware
      Hardware reset begins a new standby session
    end note
```

## Interrupt input and strict double-press timing

```mermaid
flowchart TD
    IRQ[PC13 press-edge EXTI] --> Q[Queue HAL tick timestamp only]
    Q --> R[Return from interrupt]
    Main[Foreground drains queued edges] --> Overflow{Queue overflow?}
    Overflow -- yes --> Drop[Discard ambiguous input and reset decoder; log warning]
    Overflow -- no --> Edge{Edge available?}
    Drop --> Edge
    Edge -- yes --> Debounce{Less than 30 ms since accepted edge?}
    Debounce -- yes --> Edge
    Debounce -- no --> Pending{First press pending?}
    Pending -- no --> Store[Store first press; defer all single actions]
    Store --> Edge
    Pending -- yes --> Window{Second edge minus first is less than 500 ms?}
    Window -- yes --> Double[Emit DOUBLE at second-edge timestamp; clear pending]
    Window -- no --> SingleOld[Emit SINGLE at first+500; retain new edge as pending]
    Double --> Dispatch[Dispatch event to current game state]
    SingleOld --> Dispatch
    Dispatch --> Edge
    Edge -- no --> Atomic[Brief critical section: recheck queue empty]
    Atomic --> Arrived{New captured edge queued?}
    Arrived -- yes --> Edge
    Arrived -- no --> Timeout{Pending age at least 500 ms?}
    Timeout -- yes --> Single[Emit SINGLE at first+500; clear pending]
    Timeout -- no --> Done[Continue foreground loop]
    Single --> Handle[Restore interrupts, then dispatch event]
    Handle --> Done
```

Timestamps use unsigned tick arithmetic across rollover. Captured edges are processed before current-time expiry checks. The game ignores obsolete contextual events older than the current mode, attack or target boundary so an ISR timestamp cannot underflow a newly created deadline. The 499 ms and 500 ms cases and stale-event races are covered by host tests; real button/interrupt latency still needs bench verification.

## Foreground scheduling and telemetry

```mermaid
flowchart TD
    Boot[HAL and 80 MHz clock; board UART/sensors; optional modules] --> Init[Initialize standby and decoder; emit boot messages; capture stable reference]
    Init --> Inputs[Drain EXTI timestamps and classify button events]
    Inputs --> Tick[Advance game deadlines; handle queued events]
    Tick --> Dead{Terminal?}
    Dead -- yes --> Stop[Final message and OLED frame; bounded 600 ms drain budget]
    Stop --> Quiet[UART deinit; buzzer stop; EXTI/tick/interrupts off; WFI until reset]
    Dead -- no --> MotionDue{50 ms motion update due?}
    MotionDue -- yes --> Motion[Read acceleration and gyro; drain button edges; update orientation]
    MotionDue -- no --> SensorsDue{1-second sweep due?}
    Motion --> SensorsDue
    SensorsDue -- yes --> Read[Read all six sensors; drain inputs; recheck terminal deadline]
    Read --> StillAlive{Terminal?}
    StillAlive -- yes --> Stop
    StillAlive -- no --> Limits[Evaluate every threshold and validity flag]
    Limits --> Abnormal{Any abnormal or unavailable sensor?}
    Abnormal -- yes --> Warnings[Queue all warning lines; suppress normal telemetry]
    Abnormal -- no --> Telemetry[Queue complete physical-unit telemetry]
    Warnings --> Events[Handle game events and attacked 1-second message]
    Telemetry --> Events
    SensorsDue -- no --> Events
    Events --> TerminalAgain{Terminal?}
    TerminalAgain -- yes --> Stop
    TerminalAgain -- no --> Optional[Poll IR, optional colour, one OLED chunk and buzzer notes]
    Optional --> Outputs[Update aim/colour hint; refresh display every 200 ms; write LED; poll UART]
    Outputs --> Inputs
```

The one-second sensor schedule continues in standby, battle and attacked battle. Warnings replace normal telemetry only while a violation or unavailable reading remains. UART mode/charge/fire/attack messages are independent event notifications. Optional I2C uses a different bus from on-board sensors. No interrupt handler waits for I2C, UART, a double-press window or a melody.

## Target game within ordinary battle

```mermaid
flowchart TD
    Target[Display next reproducible zombie or human target] --> Wait[Accept compulsory battle controls for up to 4 s]
    Wait --> Cause{Shot or target expiry?}
    Cause -- auto-fire --> Identity{Displayed zombie?}
    Identity -- yes --> Hit[Score +10; add +5 if IR aim locked; zombie hit count +1]
    Identity -- no --> HumanHit[Score -5; life -1]
    Cause -- 4-second expiry --> Expired{Displayed human?}
    Expired -- yes --> Saved[Score +5; no life lost]
    Expired -- no --> Escaped[Score -5; life -1]
    Hit --> Result{Lives zero or zombie hits six?}
    HumanHit --> Result
    Saved --> Result
    Escaped --> Result
    Result -- neither --> Target
    Result -- six hits --> Win[Victory text and sound; retain mandatory battle operations]
    Result -- zero lives --> Lose[Defeat text and sound; retain mandatory battle operations]
    Win --> Restart[Double to standby, then double to battle for a new round]
    Lose --> Restart
```

An attacked state pauses target expiry and ignores singles. Rescue resumes the same target with a fresh four-second window. No external sensor gates a mandatory shot, and a reflective IR output never determines target colour.
