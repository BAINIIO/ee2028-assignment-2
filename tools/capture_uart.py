#!/usr/bin/env python3
"""Capture real 115200-baud STM32 UART without third-party dependencies (POSIX)."""
import argparse
from datetime import datetime
import os
from pathlib import Path
import select
import termios
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 120:
        parser.error("Use a capture duration between 1 and 120 seconds")
    fd = os.open(args.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    original = termios.tcgetattr(fd)
    attributes = termios.tcgetattr(fd)
    attributes[0] = attributes[1] = attributes[3] = 0
    attributes[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attributes[4] = attributes[5] = termios.B115200
    attributes[6][termios.VMIN] = attributes[6][termios.VTIME] = 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    pending = bytearray()
    start = time.monotonic()
    try:
        termios.tcsetattr(fd, termios.TCSANOW, attributes)
        termios.tcflush(fd, termios.TCIFLUSH)
        with args.output.open("a", encoding="utf-8") as log:
            print(f"Listening on {args.port} for {args.seconds:g} seconds.", flush=True)
            while time.monotonic() - start < args.seconds:
                ready, _, _ = select.select([fd], [], [], 0.1)
                if not ready:
                    continue
                try:
                    pending.extend(os.read(fd, 16384))
                except BlockingIOError:
                    continue
                while b"\n" in pending:
                    line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    stamp = datetime.now().astimezone().isoformat(timespec="milliseconds")
                    text = f"[{stamp}] {line.decode(errors='replace').rstrip()}"
                    print(text, flush=True)
                    log.write(text + "\n")
                    log.flush()
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, original)
        os.close(fd)


if __name__ == "__main__":
    main()
