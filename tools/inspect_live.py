#!/usr/bin/env python3
"""Read converted sensor/game state over SWD HOTPLUG without halting or reset."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import board


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9]+", args.serial):
        parser.error("Invalid ST-Link serial")
    args.output.mkdir(parents=True, exist_ok=True)
    symbols = subprocess.run(["sh", "tools/arm-tool.sh", "objdump", "-t",
                              "build/firmware/zombie_game.elf"], cwd=board.ROOT,
                             text=True, stdout=subprocess.PIPE, check=True).stdout
    locations = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) >= 6 and parts[-1] in {
            "sensors", "game", "uwTick", "ir_present", "ir_raw",
            "oled_online", "oled_offset", "oled_sending", "oled_dirty",
            "oled_buffer", "oled_send_buffer", "external_i2c",
            "oled_failures", "oled_busy_deferrals", "oled_recoveries",
            "oled_last_status", "oled_last_error"
        }:
            locations[parts[-1]] = (int(parts[0], 16), int(parts[-2], 16))
    # ARM builds can use byte-sized enums while host enums occupy four bytes.
    # Ask the exact target ELF for field offsets and sizes; do not assume the
    # host ABI or hard-code a C structure layout for a live-memory read.
    fields = ["mode", "round", "target", "colour", "aim_locked", "attacked",
              "energy", "lives", "zombie_hits", "score", "has_reference", "attack_armed"]
    debugger = ["sh", "tools/arm-tool.sh", "gdb", "-batch", "build/firmware/zombie_game.elf"]
    for field in fields:
        debugger.extend(["-ex", f'printf "FIELD {field} %u %u\\n", '
                         f'(unsigned int)&((Game*)0)->{field}, '
                         f'(unsigned int)sizeof(((Game*)0)->{field})'])
    debugger.extend(["-ex", 'printf "I2C_ERROR_OFFSET %u\\n", '
                     '(unsigned int)&((I2C_HandleTypeDef*)0)->ErrorCode'])
    metadata = subprocess.run(debugger, cwd=board.ROOT, text=True,
                               stdout=subprocess.PIPE, check=True).stdout
    layout = {name: (int(offset), int(size)) for name, offset, size in
              re.findall(r"FIELD (\w+) (\d+) (\d+)", metadata)}
    if set(layout) != set(fields):
        raise RuntimeError("Could not resolve target structure layout")
    arguments = [board.programmer(), "-c", "port=SWD", f"sn={args.serial}",
                 "freq=4000", "mode=HOTPLUG"]
    for name, (address, size) in locations.items():
        arguments.extend(["-u", hex(address), hex(size), str((args.output / f"{name}.bin").resolve())])
    arguments.extend(["-u", "0x48000800", "24", str((args.output / "gpioc.bin").resolve())])
    arguments.extend(["-u", "0x40005400", "28", str((args.output / "i2c1.bin").resolve()),
                      "-u", "0x48000410", "4", str((args.output / "gpiob_inputs.bin").resolve())])
    result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=30, check=True)
    (args.output / "read.log").write_text(re.sub(r"\x1b\[[0-9;]*m", "", result.stdout))
    values = struct.unpack("<12fI", (args.output / "sensors.bin").read_bytes())
    game = (args.output / "game.bin").read_bytes()
    game_state = {name: int.from_bytes(game[offset:offset + size], "little",
                                     signed=name == "score")
                  for name, (offset, size) in layout.items()}
    for field in ("aim_locked", "attacked", "has_reference", "attack_armed"):
        game_state[field] = bool(game_state[field])
    mode, _, _, pull, inputs, _ = struct.unpack("<6I", (args.output / "gpioc.bin").read_bytes())
    ir_state = {
        "A0_PC5_level": (inputs >> 5) & 1,
        "A0_PC5_mode": (mode >> 10) & 3,
        "A0_PC5_pull": (pull >> 10) & 3,
        "A1_PC4_level": (inputs >> 4) & 1,
        "A1_PC4_mode": (mode >> 8) & 3,
        "gpio_modes": "0=input, 1=output, 2=alternate, 3=analog (digital input disabled)"
    }
    for name in ("ir_present", "ir_raw"):
        if name in locations:
            ir_state[name] = bool(int.from_bytes((args.output / f"{name}.bin").read_bytes(), "little"))
    report = {
        "sensor_readings": {
            "temperature_c": values[0], "pressure_hpa": values[1], "humidity_pct": values[2],
            "acceleration_ms2": values[3:6], "angular_rate_dps": values[6:9],
            "magnetic_ut": values[9:12], "valid_mask": hex(values[12])},
        "game": game_state,
        "ir_diagnostics": ir_state,
        "tick_ms": struct.unpack("<I", (args.output / "uwTick.bin").read_bytes())[0],
        "method": "read-only SWD HOTPLUG; no input injection, halt or reset",
        "layout": "Game field offsets and sizes resolved from target ELF debug information"
    }
    oled_state = {}
    for name in ("oled_online", "oled_offset", "oled_sending", "oled_dirty",
                 "oled_failures", "oled_busy_deferrals", "oled_recoveries",
                 "oled_last_status", "oled_last_error"):
        if name in locations:
            value = int.from_bytes((args.output / f"{name}.bin").read_bytes(), "little")
            oled_state[name] = bool(value) if name in {
                "oled_online", "oled_sending", "oled_dirty"} else value
    for name in ("oled_buffer", "oled_send_buffer"):
        if name in locations:
            data = (args.output / f"{name}.bin").read_bytes()
            oled_state[f"{name}_sha256"] = hashlib.sha256(data).hexdigest()
    if oled_state:
        if "external_i2c" in locations:
            match = re.search(r"I2C_ERROR_OFFSET (\d+)", metadata)
            if match:
                offset = int(match.group(1))
                data = (args.output / "external_i2c.bin").read_bytes()
                oled_state["hal_i2c_error"] = hex(int.from_bytes(data[offset:offset + 4], "little"))
        registers = struct.unpack("<7I", (args.output / "i2c1.bin").read_bytes())
        oled_state["i2c1_cr1"] = hex(registers[0])
        oled_state["i2c1_isr"] = hex(registers[6])
        inputs = int.from_bytes((args.output / "gpiob_inputs.bin").read_bytes(), "little")
        oled_state["SCL_PB8"] = (inputs >> 8) & 1
        oled_state["SDA_PB9"] = (inputs >> 9) & 1
        report["oled_diagnostics"] = oled_state
    (args.output / "snapshot.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
