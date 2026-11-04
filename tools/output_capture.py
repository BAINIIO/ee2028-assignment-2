"""Optional real OLED framebuffer and UART capture for the hosted output viewer.

Importing this module does no I/O. OLED uses the existing verified read-only
ST-Link transport. UART opens only an explicitly selected POSIX port, or tails
an existing log without opening serial. No game/sensor output is synthesized.
"""

from __future__ import annotations

import base64
from collections import deque
from datetime import datetime
import os
from pathlib import Path
import re
import threading
import time


MAX_UART_LINES = 100
MAX_UART_LINE_BYTES = 2048
MAX_UART_TEXT_BYTES = 24000
MAX_LOG_READ = 64 * 1024
TERMINAL_UART_LINE = "TERMINATED: rescue timed out. Communication ending."


def output_status(received_at, now, *, active=True, terminal=False):
    if terminal:
        return "terminated"
    if not active or received_at is None or now - received_at > 10:
        return "offline"
    return "stale" if now - received_at > 3 else "online"


class OledCapture:
    def __init__(self, *, serial=None, elf=None, transport_factory=None, clock=time.time):
        self.serial, self.elf = serial, elf
        self.factory, self.clock = transport_factory, clock
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.thread = None
        self.transport = None
        self.frame = None
        self.active = False
        self.terminal = False
        self.error = None

    def start(self):
        self.thread = threading.Thread(target=self._run, name="cloud-oled-capture", daemon=True)
        self.thread.start()

    def _run(self):
        try:
            factory = self.factory
            if factory is None:
                from live_display_transport import LiveDisplayTransport
                factory = LiveDisplayTransport
            transport = factory(serial=self.serial, elf_path=self.elf)
            with self.lock:
                self.transport = transport
            if self.stopping.is_set():
                return
            transport.connect()  # Mandatory ELF/BIN/flash match; never flash/reset.
            with self.lock:
                self.active = True
            while not self.stopping.is_set():
                pixels = transport.read_frame()
                if pixels is not None:
                    # The reader assigns this only after a coherent seqlock read.
                    sequence = transport._last_sequence
                    if (len(pixels) != 1024 or type(sequence) is not int
                            or not 0 < sequence <= 0xffffffff or sequence & 1):
                        raise ValueError("Invalid coherent OLED frame")
                    with self.lock:
                        self.frame = {"width": 128, "height": 64, "format": "ssd1306-pages",
                                      "data": base64.b64encode(pixels).decode("ascii"),
                                      "received_at": self.clock(), "sequence": sequence}
                        self.terminal = bool(transport.terminal)
                if transport.terminal:
                    break
                self.stopping.wait(0.2)  # Latest real frame; hosted uploads remain ~1 Hz.
        except Exception as error:
            with self.lock:
                self.error = str(error)
        finally:
            with self.lock:
                transport = self.transport
                self.active = False
            if transport is not None:
                transport.close()

    def snapshot(self):
        with self.lock:
            if self.frame is None:
                return None
            return {**self.frame, "status": output_status(self.frame["received_at"], self.clock(),
                    active=self.active, terminal=self.terminal)}

    def close(self):
        self.stopping.set()
        with self.lock:
            transport = self.transport
        if transport is not None:
            transport.close()  # Interrupt a pending discovery/read on exit.
        if self.thread is not None:
            self.thread.join(timeout=2)


class UartBuffer:
    def __init__(self, *, clock=time.time, parse_timestamps=False):
        self.clock = clock
        self.parse_timestamps = parse_timestamps
        self.lock = threading.Lock()
        self.lines = deque()
        self.text_bytes = 0
        self.pending = bytearray()
        self.next_id = 0
        self.received_at = None
        self.active = False
        self.terminal = False
        self.error = None

    def feed(self, data: bytes, received_at: float):
        with self.lock:
            if self.terminal:
                return
            for byte in data:
                if byte == 10:
                    line = bytes(self.pending).removesuffix(b"\r").decode("utf-8", errors="replace")
                    self.pending.clear()
                    stamp = re.match(r"^\[([^\]]+)\] (.*)$", line)
                    at = received_at
                    if stamp and self.parse_timestamps:
                        try:
                            parsed = datetime.fromisoformat(stamp[1])
                            if parsed.tzinfo is not None:
                                at, line = parsed.timestamp(), stamp[2]
                        except ValueError:
                            pass
                    # Preserve line bodies/whitespace; only transport CR/LF and
                    # recognized monitor-added timestamp prefixes are removed.
                    encoded = line.encode("utf-8")[:MAX_UART_LINE_BYTES]
                    line = encoded.decode("utf-8", errors="ignore")
                    size = len(line.encode("utf-8"))
                    self.next_id += 1
                    self.lines.append({"id": self.next_id, "at": at, "text": line})
                    self.text_bytes += size
                    self.received_at = at
                    while len(self.lines) > MAX_UART_LINES or self.text_bytes > MAX_UART_TEXT_BYTES:
                        self.text_bytes -= len(self.lines.popleft()["text"].encode("utf-8"))
                    if line == TERMINAL_UART_LINE:
                        self.terminal = True
                        break
                elif len(self.pending) < MAX_UART_LINE_BYTES + 128:
                    self.pending.append(byte)  # Bound an unterminated/oversized line.

    def snapshot(self):
        with self.lock:
            return {"lines": [dict(line) for line in self.lines], "received_at": self.received_at,
                    "status": output_status(self.received_at, self.clock(),
                              active=self.active, terminal=self.terminal)}


class UartLogCapture(UartBuffer):
    def __init__(self, path, **options):
        super().__init__(parse_timestamps=True, **options)
        self.path = Path(path).expanduser()
        self.identity = None
        self.offset = 0

    def start(self):
        pass

    def poll(self):
        if self.terminal:
            return
        try:
            stat = self.path.stat()
            identity = stat.st_dev, stat.st_ino
            changed = identity != self.identity or stat.st_size < self.offset
            with self.path.open("rb") as stream:
                if changed:
                    self.pending.clear()
                    self.offset = max(0, stat.st_size - MAX_LOG_READ)
                    stream.seek(self.offset)
                    if self.offset:
                        stream.readline(MAX_LOG_READ)  # Skip a partial first line.
                        self.offset = stream.tell()
                else:
                    stream.seek(self.offset)
                data = stream.read(MAX_LOG_READ)
                self.offset = stream.tell()
            self.identity, self.active, self.error = identity, True, None
            if data:
                # Timestamped monitor logs retain their original receipts;
                # untimestamped raw logs use the actual file modification time.
                self.feed(data, stat.st_mtime)
        except OSError:
            self.active = False
            self.error = "UART log is unavailable"

    def snapshot(self):
        self.poll()
        return super().snapshot()

    def close(self):
        self.active = False


class SerialUartCapture(UartBuffer):
    def __init__(self, port, **options):
        super().__init__(**options)
        self.port = port
        self.stopping = threading.Event()
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self._run, name="cloud-uart-capture", daemon=True)
        self.thread.start()

    def _run(self):
        fd, original = None, None
        try:
            if os.name != "posix":
                raise OSError("Direct UART capture requires POSIX; use --uart-log on this platform")
            import fcntl
            import select
            import termios
            fd = os.open(self.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
            # An explicit port is required. Close Tera Term/other readers first;
            # a log source lets an existing terminal retain its connection.
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if hasattr(termios, "TIOCEXCL"):
                fcntl.ioctl(fd, termios.TIOCEXCL)
            original = termios.tcgetattr(fd)
            attributes = termios.tcgetattr(fd)
            attributes[0] = attributes[1] = attributes[3] = 0
            attributes[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            attributes[4] = attributes[5] = termios.B115200
            attributes[6][termios.VMIN] = attributes[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, attributes)
            self.active = True
            while not self.stopping.is_set() and not self.terminal:
                ready, _, _ = select.select([fd], [], [], 0.2)
                if ready:
                    try:
                        data = os.read(fd, 16384)
                    except BlockingIOError:
                        continue
                    if not data:
                        break
                    self.feed(data, self.clock())
        except Exception as error:
            self.error = str(error)
        finally:
            self.active = False
            if fd is not None:
                try:
                    if original is not None:
                        termios.tcsetattr(fd, termios.TCSANOW, original)
                    if hasattr(termios, "TIOCNXCL"):
                        fcntl.ioctl(fd, termios.TIOCNXCL)
                except OSError:
                    pass
                os.close(fd)

    def close(self):
        self.stopping.set()
        if self.thread is not None:
            self.thread.join(timeout=1)


class OutputCollector:
    def __init__(self, *, oled=False, uart_port=None, uart_log=None, oled_serial=None, elf=None):
        if uart_port and uart_log:
            raise ValueError("Choose a UART port or an existing log")
        self.oled = OledCapture(serial=oled_serial, elf=elf) if oled else None
        self.uart = (SerialUartCapture(uart_port) if uart_port else
                     UartLogCapture(uart_log) if uart_log else None)

    def start(self):
        for capture in (self.oled, self.uart):
            if capture is not None:
                capture.start()

    def snapshot(self):
        return {"oled": self.oled.snapshot() if self.oled else None,
                "uart": self.uart.snapshot() if self.uart else
                {"lines": [], "received_at": None, "status": "offline"}}

    def errors(self):
        return [f"{label} capture unavailable: {source.error}" for label, source in
                (("OLED", self.oled), ("UART", self.uart)) if source is not None and source.error]

    def close(self):
        for capture in (self.oled, self.uart):
            if capture is not None:
                capture.close()
