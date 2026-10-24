#!/usr/bin/env python3
"""Read-only UART monitor. Requires pyserial; never sends game commands."""
import argparse
from datetime import datetime
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="e.g. /dev/cu.usbmodem123; auto-selects one STM32 VCP")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log", type=Path, help="save received text here")
    args = parser.parse_args()
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        parser.exit(1, "Install monitor dependency in a virtual environment: pip install pyserial\n")
    candidates = [p.device for p in list_ports.comports()
                  if p.vid == 0x0483 and (p.device.startswith("/dev/cu.") or sys.platform != "darwin")]
    port = args.port
    if not port:
        if len(candidates) != 1:
            parser.exit(1, f"Select --port explicitly. STM32 candidates: {candidates}\n")
        port = candidates[0]
    log = args.log.open("a", encoding="utf-8") if args.log else None
    try:
        with serial.Serial(port=port, baudrate=args.baud, bytesize=8, parity="N",
                           stopbits=1, timeout=0.2, rtscts=False, dsrdtr=False) as console:
            print(f"Listening on {port}, {args.baud} 8N1. Ctrl-C to exit; reset board for boot message.")
            while True:
                data = console.readline()
                if not data:
                    continue
                line = data.decode("utf-8", errors="replace").rstrip("\r\n")
                # Host's timestamp labels receipt, not the sensor acquisition time.
                text = f"[{datetime.now().astimezone().isoformat(timespec='milliseconds')}] {line}"
                print(text, flush=True)
                if log:
                    log.write(text + "\n")
                    log.flush()
    except KeyboardInterrupt:
        return 0
    except (serial.SerialException, OSError) as exc:
        print(f"monitor: {exc}", file=sys.stderr)
        return 1
    finally:
        if log:
            log.close()


if __name__ == "__main__":
    sys.exit(main())
