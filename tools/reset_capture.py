#!/usr/bin/env python3
"""Reset the selected STM32 without rewriting flash and capture its boot UART."""
import argparse
import json
from pathlib import Path
import re
import time

import board
from hardware_smoke import Capture, command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=8)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9]+", args.serial):
        parser.error("Invalid ST-Link serial")
    if not 1 <= args.seconds <= 45:
        parser.error("Capture duration must be between 1 and 45 seconds")
    args.output.mkdir(parents=True, exist_ok=True)
    capture = Capture(args.port)
    try:
        command([board.programmer(), "-c", "port=SWD", f"sn={args.serial}",
                 "freq=4000", "mode=UR", "-rst"], args.output / "reset.log")
        print(f"Board reset; capturing {args.seconds:g} seconds...", flush=True)
        time.sleep(args.seconds)
    finally:
        capture.close()
        (args.output / "uart.json").write_text(json.dumps(capture.lines, indent=2) + "\n")
        (args.output / "uart.log").write_text("\n".join(
            f"[{line['seconds']:.3f}] {line['text']}" for line in capture.lines) + "\n")
    for line in capture.lines:
        print(f"[{line['seconds']:.3f}] {line['text']}")


if __name__ == "__main__":
    main()
