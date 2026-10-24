#!/usr/bin/env python3
"""Read the running game's 128x64 framebuffer through ST-Link, without resetting it.

Importing this module does no hardware I/O. Call connect() from a worker, then
read_frame() at the desired display rate. Frames use the OLED's page layout:
pixel (x, y) is bit (y % 8) of frame[x + 128 * (y // 8)].
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time

import board


WIDTH = 128
HEIGHT = 64
FRAME_BYTES = WIDTH * HEIGHT // 8
ROOT = Path(__file__).resolve().parent.parent
_MAX_SNAPSHOT_ATTEMPTS = 3
_SNAPSHOT_BUDGET_SECONDS = 0.027


class TransportError(RuntimeError):
    """A missing tool, incompatible firmware, or unavailable board connection."""


@dataclass(frozen=True)
class _Symbols:
    sequence: int
    buffer: int
    terminal: int


def _parse_symbols(output: str) -> _Symbols:
    found = {}
    expected = {"laptop_display_sequence": 4, "laptop_display_buffer": FRAME_BYTES,
                "laptop_display_terminal": 4}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 5 or fields[-1] not in expected:
            continue
        try:
            address, size = int(fields[0], 16), int(fields[-2], 16)
        except ValueError as exc:
            raise TransportError("Invalid framebuffer symbol table in firmware ELF") from exc
        if size != expected[fields[-1]] or address % 4 or not 0x20000000 <= address < 0x200A0000:
            raise TransportError(f"Unexpected size or RAM address for {fields[-1]}")
        if address + size > 0x200A0000:
            raise TransportError(f"Framebuffer symbol {fields[-1]} exceeds board RAM")
        found[fields[-1]] = address
    if set(found) != set(expected):
        if {"laptop_display_sequence", "laptop_display_buffer"} <= set(found):
            raise TransportError("This laptop mirror cannot signal termination; rebuild and flash the current game first")
        raise TransportError("This firmware has no laptop display mirror; rebuild the game first")
    return _Symbols(found["laptop_display_sequence"], found["laptop_display_buffer"],
                    found["laptop_display_terminal"])


def _parse_flash_address(output: str) -> int:
    addresses = []
    lines = output.splitlines()
    for index, line in enumerate(lines[:-1]):
        fields = line.split()
        if len(fields) >= 7 and fields[0].isdigit() and "LOAD" in lines[index + 1]:
            try:
                size, address = int(fields[2], 16), int(fields[4], 16)
            except ValueError:
                continue
            if size:
                if not 0x08000000 <= address < address + size <= 0x08200000:
                    raise TransportError("Firmware contains a load section outside board flash")
                addresses.append(address)
    if not addresses:
        raise TransportError("Cannot determine firmware flash address from ELF")
    return min(addresses)


def _number(value: str) -> int:
    return int(value, 16 if value.lower().startswith("0x") else 10)


def _parse_frame(response: str) -> tuple[int, bytes, bool] | None:
    values = response.split()
    try:
        if len(values) == 2 and values[0] == "pending":
            _number(values[1])
            return None
        if not values or values[0] not in ("frame", "frame32"):
            raise ValueError("invalid response format")
        words = values[0] == "frame32"
        if len(values) != (FRAME_BYTES // 4 if words else FRAME_BYTES) + 4:
            raise ValueError("invalid response length")
        before, after = _number(values[1]), _number(values[2])
        terminal = _number(values[3])
        if terminal not in (0, 1):
            raise ValueError("invalid terminal flag")
        if before == 0 or before & 1 or before != after:
            return None
        pixels = (struct.pack("<256I", *(_number(value) for value in values[4:]))
                  if words else bytes(_number(value) for value in values[4:]))
        return before, pixels, bool(terminal)
    except (ValueError, OverflowError, struct.error) as exc:
        raise TransportError(f"Unexpected framebuffer response: {response[:120]}") from exc


class LiveDisplayTransport:
    """One owned, read-only board connection; close() may interrupt its worker.

    ``serial`` selects an ST-Link probe, as with board.py --serial. It is not a
    USB UART path. Firmware verification is mandatory before any RAM is read.
    A transport instance is single-use after close(), including failed connects.
    """

    def __init__(self, elf_path=None, bin_path=None, serial=None):
        self.elf_path = Path(elf_path or ROOT / "build/firmware/zombie_game.elf")
        self.bin_path = Path(bin_path or self.elf_path.with_suffix(".bin"))
        self.serial = serial
        self.status = "Disconnected"
        self.terminal = False
        self.read_seconds = 0.0
        self._closed = threading.Event()
        self._lock = threading.Lock()
        self._process = None
        self._socket = None
        self._log_path = None
        self._symbols = None
        self._last_sequence = None
        self._received = b""

    def _check_open(self):
        if self._closed.is_set():
            raise TransportError("Display connection closed")

    def _register_process(self, process):
        with self._lock:
            if self._closed.is_set():
                process.terminate()
                process.wait(timeout=3)
                raise TransportError("Display connection closed")
            self._process = process

    def _run(self, command, timeout=15):
        """Run a discovery/read tool with cancellation when the UI closes."""
        self._check_open()
        process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        self._register_process(process)
        deadline = time.monotonic() + timeout
        try:
            while True:
                self._check_open()
                try:
                    output, _ = process.communicate(timeout=0.1)
                    break
                except subprocess.TimeoutExpired:
                    if time.monotonic() >= deadline:
                        raise TransportError(f"Timed out running {Path(command[0]).name}")
            if process.returncode:
                detail = re.sub(r"\x1b\[[0-9;]*m", "", output).strip()
                raise TransportError(detail[-1800:] or f"Tool exited with {process.returncode}")
            return output
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
            if process.stdout is not None:
                process.stdout.close()
            with self._lock:
                if self._process is process:
                    self._process = None

    def _verify_firmware(self):
        if not self.elf_path.is_file() or not self.bin_path.is_file():
            raise TransportError("Build the game firmware first: its ELF and BIN are required")
        arm_tool = str(ROOT / "tools/arm-tool.sh")
        self._symbols = _parse_symbols(self._run(["sh", arm_tool, "objdump", "-t",
                                                  str(self.elf_path)]))
        flash_address = _parse_flash_address(self._run(["sh", arm_tool, "objdump", "-h",
                                                       str(self.elf_path)]))
        with tempfile.TemporaryDirectory(prefix="garden-guard-verify-") as directory:
            generated = Path(directory) / "expected.bin"
            uploaded = Path(directory) / "board.bin"
            self._run(["sh", arm_tool, "objcopy", "-O", "binary", str(self.elf_path),
                       str(generated)])
            expected = self.bin_path.read_bytes()
            if not expected or generated.read_bytes() != expected:
                raise TransportError("Firmware ELF and BIN differ; rebuild before displaying the game")
            programmer = board.programmer()
            if self.serial:
                # Reuse board.py's explicit serial validation; this performs no I/O.
                self.serial = board.select_probe(self.serial)
            else:
                output = self._run([programmer, "--list"], timeout=30)
                probes = sorted(set(re.findall(r"ST-LINK\s+SN\s*:\s*([A-Za-z0-9]+)",
                                              output, flags=re.IGNORECASE)))
                if not probes:
                    raise TransportError("No ST-Link found. Connect the board's CN7 USB data port")
                if len(probes) != 1:
                    raise TransportError("Multiple ST-Link probes found; select one with --serial")
                self.serial = probes[0]
            self.status = "Checking the running firmware…"
            # HOTPLUG attaches to the running CPU. Upload reads flash into a
            # temporary host file; no halt, reset, write, or start command is used.
            self._run([programmer, "-c", "port=SWD", f"sn={self.serial}", "freq=4000",
                       "mode=HOTPLUG", "-u", f"0x{flash_address:08x}", hex(len(expected)),
                       str(uploaded)], timeout=30)
            if not uploaded.is_file() or hashlib.sha256(uploaded.read_bytes()).digest() != \
                    hashlib.sha256(expected).digest():
                raise TransportError("The board is running different firmware. Flash the current build first")

    def _log_tail(self):
        try:
            return Path(self._log_path).read_text(errors="replace")[-1800:]
        except (OSError, TypeError):
            return ""

    def connect(self):
        """Verify flash, then attach OpenOCD without halting or resetting the CPU."""
        self._check_open()
        if self._socket is not None:
            return
        self.status = "Finding the connected board…"
        try:
            self._verify_firmware()
            self._check_open()
            executable = board.tool("openocd", "*openocd*/tools/bin/openocd", "STM32_OPENOCD")
            configured = os.environ.get("STM32_OPENOCD_SCRIPTS")
            scripts = ([Path(configured).expanduser()] if configured else
                       sorted(board.PLUGINS.glob("*debug.openocd*/resources/openocd/st_scripts")))
            if not scripts or not scripts[-1].is_dir():
                raise TransportError("Set STM32_OPENOCD_SCRIPTS to ST's OpenOCD st_scripts directory")
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            self.status = "Connecting the live display…"
            with tempfile.NamedTemporaryFile(prefix="garden-guard-openocd-", suffix=".log",
                                             delete=False) as log:
                self._log_path = log.name
                process = subprocess.Popen([
                    executable, "-s", str(scripts[-1]), "-f", str(ROOT / "tools/openocd.cfg"),
                    "-c", f"adapter serial {self.serial}",
                    "-c", "gdb_port disabled", "-c", "telnet_port disabled",
                    "-c", f"tcl_port {port}",
                    # ST's examine-end hook normally writes power/debug
                    # registers. A live mirror needs only target examination.
                    "-c", "STM32L4S5VITx.cpu configure -event examine-end {}",
                    "-c", "init",
                ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                self._register_process(process)
            deadline = time.monotonic() + 12
            while True:
                self._check_open()
                if process.poll() is not None:
                    raise TransportError("OpenOCD could not attach: " + self._log_tail().strip())
                try:
                    connection = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                    connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    connection.settimeout(2)
                    with self._lock:
                        if self._closed.is_set():
                            connection.close()
                            raise TransportError("Display connection closed")
                        self._socket = connection
                    break
                except (ConnectionRefusedError, socket.timeout):
                    if time.monotonic() >= deadline:
                        raise TransportError("Timed out attaching the display: " + self._log_tail().strip())
                    self._closed.wait(0.1)
            self.status = "Connected — waiting for the game frame…"
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            self.status = str(exc)
            self.close()
            if isinstance(exc, TransportError):
                raise
            raise TransportError(str(exc)) from exc

    def _rpc(self, command):
        self._check_open()
        connection = self._socket
        if connection is None:
            raise TransportError("Connect the display before reading a frame")
        connection.sendall(command.encode("ascii") + b"\x1a")
        while b"\x1a" not in self._received:
            chunk = connection.recv(8192)
            if not chunk:
                raise TransportError("Board disconnected from the live display")
            self._received += chunk
            if len(self._received) > 32768:
                raise TransportError("Unexpectedly large response from OpenOCD")
        response, self._received = self._received.split(b"\x1a", 1)
        return response.decode("ascii").strip()

    def read_frame(self):
        """Return a stable 1024-byte frame, or None if busy/unchanged/not ready.

        The sequence is read before and after the block read in one Tcl request.
        Changed or odd sequences are retried within a small time budget, so a
        partial render is never shown. The terminal flag is part of that same
        coherent snapshot. A final frame closes the owned connection and sets
        terminal=True; subsequent calls return None without touching the board.
        """
        if self.terminal:
            return None
        self._check_open()
        if self._symbols is None or self._socket is None:
            raise TransportError("Connect the display before reading a frame")
        sequence = f"0x{self._symbols.sequence:08x}"
        buffer = f"0x{self._symbols.buffer:08x}"
        terminal = f"0x{self._symbols.terminal:08x}"
        command = (
            f"set before [lindex [read_memory {sequence} 32 1] 0]; "
            "if {$before == 0 || ($before & 1)} {list pending $before} else {"
            # The framebuffer is four-byte aligned in the ELF. A word block
            # needs one quarter as many SWD accesses as byte-width reads.
            f"set pixels [read_memory {buffer} 32 {FRAME_BYTES // 4}]; "
            f"set terminal [lindex [read_memory {terminal} 32 1] 0]; "
            f"set after [lindex [read_memory {sequence} 32 1] 0]; "
            "join [concat [list frame32 $before $after $terminal] $pixels]}"
        )
        started = time.monotonic()
        try:
            for _ in range(_MAX_SNAPSHOT_ATTEMPTS):
                attempt_started = time.monotonic()
                response = self._rpc(command)
                snapshot = _parse_frame(response)
                if snapshot is not None:
                    sequence_value, pixels, terminal_value = snapshot
                    if terminal_value:
                        # Only a coherent, completed publication can end the
                        # session. Stop before returning its final screen, so
                        # even callers that forget to check terminal cannot
                        # keep SWD or an owned OpenOCD session active afterward.
                        self._last_sequence = sequence_value
                        self.terminal = True
                        self.status = "Session ended — board terminated; reset and reconnect to play"
                        self.close()
                        return pixels
                    if sequence_value == self._last_sequence:
                        return None
                    self._last_sequence = sequence_value
                    self.status = "Live — board controls and buzzer are active"
                    return pixels
                if response == "pending 0":
                    return None
                now = time.monotonic()
                # Reserve the observed cost of another block read instead of
                # beginning a retry that would likely overrun this UI frame.
                if now - started + (now - attempt_started) >= _SNAPSHOT_BUDGET_SECONDS:
                    break
            return None
        except (OSError, UnicodeError, TransportError) as exc:
            self.status = f"Display disconnected: {exc}"
            self.close()
            raise TransportError(self.status) from exc
        finally:
            self.read_seconds = time.monotonic() - started

    def close(self):
        """Interrupt pending work and stop only this transport's OpenOCD process."""
        self._closed.set()
        with self._lock:
            connection, self._socket = self._socket, None
            process, self._process = self._process, None
        if connection is not None:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if self._log_path:
            try:
                Path(self._log_path).unlink(missing_ok=True)
            except OSError:
                pass
        if self.status.startswith(("Connected", "Live", "Finding", "Checking", "Connecting")):
            self.status = "Disconnected"

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="ST-Link serial, automatic if one probe is connected")
    parser.add_argument("--elf", type=Path, help="Exact ELF currently flashed to the board")
    parser.add_argument("--bin", type=Path, help="Matching firmware BIN")
    parser.add_argument("--frames", type=int, default=0,
                        help="Exit after this many updated frames (default: run until Ctrl-C)")
    parser.add_argument("--fps", type=float, default=30,
                        help="Requested polling rate in frames per second (default: 30)")
    parser.add_argument("--timeout", type=float, default=20,
                        help="Seconds to collect a finite --frames diagnostic (default: 20)")
    parser.add_argument("--output", type=Path,
                        help="Save the final raw page-layout framebuffer to this file")
    args = parser.parse_args()
    if args.frames < 0 or args.timeout <= 0 or not 1 <= args.fps <= 120:
        parser.error("--frames must be nonnegative, --timeout positive, and --fps between 1 and 120")
    if args.output and not args.frames:
        parser.error("--output requires a finite --frames count")
    try:
        with LiveDisplayTransport(args.elf, args.bin, args.serial) as display:
            print(display.status, flush=True)
            count = 0
            hashes = set()
            started = time.monotonic()
            deadline = started + args.timeout
            while True:
                read_started = time.monotonic()
                frame = display.read_frame()
                if frame is not None:
                    count += 1
                    digest = hashlib.sha256(frame).hexdigest()
                    hashes.add(digest)
                    print(f"Frame {count}: sha256={digest} read={display.read_seconds * 1000:.1f}ms"
                          f" — {display.status}", flush=True)
                    if display.terminal:
                        if args.output:
                            args.output.write_bytes(frame)
                        print("Board terminated. Final screen saved; debugger polling stopped."
                              if args.output else "Board terminated. Debugger polling stopped.", flush=True)
                        return 0
                    if args.frames and count >= args.frames:
                        if args.output:
                            args.output.write_bytes(frame)
                        elapsed = time.monotonic() - started
                        print(f"Received {count} frames in {elapsed:.2f}s ({count / max(elapsed, 0.001):.1f}fps); "
                              f"{len(hashes)} distinct framebuffer hashes",
                              flush=True)
                        return 0
                if args.frames and time.monotonic() >= deadline:
                    raise TransportError(f"Timed out after {count}/{args.frames} frames: {display.status}")
                time.sleep(max(0, 1 / args.fps - (time.monotonic() - read_started)))
    except KeyboardInterrupt:
        return 130
    except TransportError as exc:
        parser.exit(1, f"live display: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
