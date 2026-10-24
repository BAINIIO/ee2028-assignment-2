#!/usr/bin/env python3
"""Back up the connected STM32L4S5, flash the built game, and capture boot UART.

This explicitly changes firmware on the selected board. It does not inject game
inputs, change option bytes, or claim physical display/button/buzzer validation.
Uses POSIX serial APIs, so no third-party Python serial package is needed.
"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import select
import subprocess
import termios
import threading
import time
from zoneinfo import ZoneInfo

import board


class Capture:
    def __init__(self, port):
        self.fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        self.original = termios.tcgetattr(self.fd)
        attributes = termios.tcgetattr(self.fd)
        attributes[0] = attributes[1] = attributes[3] = 0
        attributes[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attributes[4] = attributes[5] = termios.B115200
        attributes[6][termios.VMIN] = attributes[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attributes)
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.raw = bytearray()
        self.lines = []
        self.stop = threading.Event()
        self.started = time.monotonic()
        self.worker = threading.Thread(target=self.read, daemon=True)
        self.worker.start()

    def read(self):
        pending = bytearray()
        while not self.stop.is_set():
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            try:
                data = os.read(self.fd, 16384)
            except BlockingIOError:
                continue
            self.raw.extend(data)
            pending.extend(data)
            while b"\n" in pending:
                line, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                self.lines.append({"seconds": round(time.monotonic() - self.started, 3),
                                   "text": line.decode(errors="replace").rstrip("\r")})

    def close(self):
        self.stop.set()
        self.worker.join(timeout=2)
        try:
            termios.tcsetattr(self.fd, termios.TCSANOW, self.original)
        finally:
            os.close(self.fd)


def command(arguments, output, timeout=60):
    result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=timeout, check=False)
    cleaned = re.sub(r"\x1b\[[0-9;]*m", "", result.stdout)
    output.write_text(cleaned)
    if result.returncode != 0 or re.search(r"(?:^|\n)Error:", cleaned):
        raise RuntimeError(f"Programmer failed; see {output}\n{cleaned[-1800:]}")
    return cleaned


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--seconds", type=float, default=8)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9]+", args.serial):
        parser.error("Invalid ST-Link serial")
    if not 1 <= args.seconds <= 45:
        parser.error("Capture duration must be between 1 and 45 seconds")
    root = Path(__file__).resolve().parent.parent
    elf = root / "build/firmware/zombie_game.elf"
    binary = elf.with_suffix(".bin")
    if not elf.is_file() or not binary.is_file():
        parser.error("Build first with make; the existing ELF is flashed without rebuilding")
    timestamp = datetime.now(ZoneInfo("Asia/Singapore")).strftime("%Y%m%d-%H%M%S")
    output = root / "output/hardware-tests" / timestamp
    output.mkdir(parents=True, exist_ok=False)
    cli = board.programmer()
    connection = [cli, "-c", "port=SWD", f"sn={args.serial}", "freq=4000"]
    print("Checking chip identity without a reset...", flush=True)
    identity = command(connection + ["mode=HOTPLUG", "-r32", "0xE0042000", "4"],
                       output / "identity.log")
    if not re.search(r"Device\s+ID\s*:\s*0x470\b", identity, flags=re.IGNORECASE):
        raise RuntimeError(f"Expected STM32L4R/S device ID 0x470. Nothing flashed; see {output}")
    print("Backing up all 2 MiB of existing flash...", flush=True)
    backup = output / "original-flash.bin"
    command(connection + ["mode=HOTPLUG", "-u", "0x08000000", "0x200000", str(backup)],
            output / "backup.log")
    if backup.stat().st_size != 0x200000:
        raise RuntimeError("Backup size mismatch; refusing to flash")
    capture = Capture(args.port)
    try:
        print("Programming and verifying the built game, then resetting...", flush=True)
        command(connection + ["mode=UR", "-w", str(elf), "-v", "-rst"],
                output / "flash.log")
        print(f"Capturing {args.seconds:g} seconds of live UART...", flush=True)
        time.sleep(args.seconds)
    finally:
        capture.close()
        (output / "uart-raw.log").write_bytes(capture.raw)
        (output / "uart.json").write_text(json.dumps(capture.lines, indent=2) + "\n")
        (output / "uart.log").write_text("\n".join(
            f"[{line['seconds']:.3f}] {line['text']}" for line in capture.lines) + "\n")
    summary = {
        "firmware_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "original_flash_backup": str(backup),
        "startup_seen": any("Entering STANDBY MODE" in line["text"] for line in capture.lines),
        "telemetry_lines": sum(line["text"].startswith("T:") for line in capture.lines),
        "warning_lines": sum(line["text"].startswith("WARNING") for line in capture.lines),
        "hardware_display_button_buzzer": "not observed by this script",
        "logs": str(output)
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    for line in capture.lines[:12]:
        print(f"[{line['seconds']:.3f}] {line['text']}")


if __name__ == "__main__":
    main()
