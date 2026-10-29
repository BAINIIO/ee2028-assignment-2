#!/usr/bin/env python3
"""Show the Garden Guard board's actual OLED framebuffer in a laptop window.

The blue USER button, sensors and buzzer on the STM32 still run the game.
Use --preview to view the existing C renderer without connecting a board.
If your Python has no Tk support, use /opt/anaconda3/bin/python on this Mac.
"""

from __future__ import annotations

import argparse
from collections import deque
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import sys
import threading
import time


WIDTH, HEIGHT, FRAME_BYTES = 128, 64, 1024
BACKGROUND = "#0c1713"
PANEL = "#12251c"
SCREEN = "#07120d"
PIXEL = "#bcf4a5"
TEXT = "#e4efdf"
MUTED = "#99ad9c"
GREEN = "#a8e88b"
AMBER = "#f2c785"
PIXEL_RGB = bytes.fromhex(PIXEL[1:])
SCREEN_RGB = bytes.fromhex(SCREEN[1:])


class FrameRate:
    """Measure received frames over the last two seconds, including pauses."""

    def __init__(self):
        self.samples: deque[float] = deque()

    def add(self, timestamp: float) -> None:
        self.samples.append(timestamp)

    def value(self, now: float) -> float:
        while self.samples and self.samples[0] < now - 2.0:
            self.samples.popleft()
        if len(self.samples) < 2:
            return 0.0
        elapsed = now - self.samples[0]
        return (len(self.samples) - 1) / elapsed if elapsed >= 0.25 else 0.0


def framebuffer_ppm(frame: bytes) -> bytes:
    """Decode the SSD1306 page layout; preserve every original 1-bit pixel."""
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"Expected {FRAME_BYTES} framebuffer bytes, got {len(frame)}")
    pixels = bytearray(WIDTH * HEIGHT * 3)
    offset = 0
    for y in range(HEIGHT):
        page = (y // 8) * WIDTH
        mask = 1 << (y % 8)
        for x in range(WIDTH):
            pixels[offset:offset + 3] = PIXEL_RGB if frame[page + x] & mask else SCREEN_RGB
            offset += 3
    return f"P6\n{WIDTH} {HEIGHT}\n255\n".encode("ascii") + pixels


def self_test() -> None:
    """Check the board's vertical page-bit layout without needing Tk or USB."""
    frame = bytearray(FRAME_BYTES)
    points = ((0, 0), (127, 0), (0, 7), (1, 8), (127, 63))
    for x, y in points:
        frame[x + (y // 8) * WIDTH] |= 1 << (y % 8)
    ppm = framebuffer_ppm(bytes(frame))
    pixels = ppm.split(b"\n", 3)[3]
    assert len(pixels) == WIDTH * HEIGHT * 3
    for y in range(HEIGHT):
        for x in range(WIDTH):
            offset = (y * WIDTH + x) * 3
            expected = PIXEL_RGB if (x, y) in points else SCREEN_RGB
            assert pixels[offset:offset + 3] == expected, (x, y)
    try:
        framebuffer_ppm(bytes(FRAME_BYTES - 1))
    except ValueError:
        pass
    else:
        raise AssertionError("Incomplete frames must be rejected")
    rate = FrameRate()
    for index in range(61):
        rate.add(10.0 + index / 30.0)
    assert abs(rate.value(12.0) - 30.0) < 0.001
    assert 14.0 < rate.value(13.0) < 16.0
    assert rate.value(15.0) == 0.0
    print("Laptop display self-test passed: framebuffer layout and measured frame rates.")


class DisplayWindow:
    """Tk is confined to the main thread; a worker owns all board reads."""

    def __init__(self, tk, root, args: argparse.Namespace):
        self.tk, self.root, self.args = tk, root, args
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.stop = threading.Event()
        self.worker: threading.Thread | None = None
        self.transport = None
        self.closing = False
        self.frame: bytes | None = None
        self.image = None
        self.zoomed_image = None
        self.zoom = args.scale
        self.current_status = "preview" if args.preview else "connecting"
        self.status_detail = "Starting…"
        self.frame_count = 0
        self.session_ended = False
        self.displayed_frame_count = 0
        self.frame_rate = FrameRate()
        self.displayed_rate = FrameRate()
        self.last_rate_update = 0.0
        self.last_frame_unix: float | None = None
        self.last_diagnostic_write = 0.0
        self.last_snapshot_write = 0.0
        self.snapshot_path = args.status_file.with_suffix(".ppm") if args.status_file else None
        self.snapshot_written = False
        self.diagnostic_warning = False
        self.last_geometry = ""

        root.title("Garden Guard — laptop display")
        root.configure(background=BACKGROUND)
        root.minsize(660, 570)
        root.geometry(f"{max(760, WIDTH * args.scale + 80)}x{HEIGHT * args.scale + 286}")
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.bind("<Escape>", lambda _event: self.close())
        root.grid_columnconfigure(0, weight=1)
        root.grid_rowconfigure(1, weight=1)

        header = tk.Frame(root, bg=BACKGROUND)
        header.grid(row=0, column=0, sticky="ew", padx=28, pady=(22, 18))
        header.grid_columnconfigure(0, weight=1)
        tk.Label(header, text="GARDEN GUARD", bg=BACKGROUND, fg=TEXT,
                 font=("Helvetica Neue", 24, "bold"), anchor="w").grid(row=0, column=0, sticky="w")
        tk.Label(header, text="Laptop display  /  Play with your board", bg=BACKGROUND,
                 fg=MUTED, font=("Helvetica Neue", 12), anchor="w").grid(
                     row=1, column=0, sticky="w", pady=(5, 0))
        self.badge = tk.Label(header, text="PREVIEW" if args.preview else "CONNECTING",
                              bg=PANEL, fg=AMBER, font=("Menlo", 10, "bold"),
                              padx=12, pady=8)
        self.badge.grid(row=0, column=1, rowspan=2, sticky="e")

        screen_frame = tk.Frame(root, bg=PANEL, highlightbackground="#34503a",
                                highlightthickness=1, padx=12, pady=12)
        screen_frame.grid(row=1, column=0, sticky="nsew", padx=28)
        screen_frame.grid_columnconfigure(0, weight=1)
        screen_frame.grid_rowconfigure(0, weight=1)
        self.canvas = tk.Canvas(screen_frame, bg=SCREEN, highlightthickness=0,
                                width=WIDTH * args.scale, height=HEIGHT * args.scale)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.canvas.bind("<Configure>", self.resize)
        self.image_item = self.canvas.create_image(0, 0, anchor="center")
        self.placeholder = self.canvas.create_text(
            WIDTH * args.scale // 2, HEIGHT * args.scale // 2,
            text="Loading firmware artwork…" if args.preview else "Connecting to your board…",
            fill=MUTED, font=("Menlo", 12), justify="center")

        status_row = tk.Frame(root, bg=BACKGROUND)
        status_row.grid(row=2, column=0, sticky="ew", padx=29, pady=(11, 15))
        status_row.grid_columnconfigure(0, weight=1)
        self.status = tk.Label(status_row, text="Starting…", bg=BACKGROUND, fg=MUTED,
                               font=("Helvetica Neue", 11), anchor="w", justify="left",
                               wraplength=650)
        self.status.grid(row=0, column=0, sticky="w")
        self.retry = tk.Button(status_row, text="Reconnect", command=self.start_worker,
                               bg=PANEL, fg=TEXT, font=("Helvetica Neue", 11),
                               highlightthickness=0, padx=8)

        controls = tk.Frame(root, bg=PANEL, padx=16, pady=15)
        controls.grid(row=3, column=0, sticky="ew", padx=28, pady=(0, 16))
        hints = (("START / STANDBY", "Double press"),
                 ("CHARGE + AUTO FIRE", "Single press"),
                 ("HUMAN ON SCREEN", "Hold fire · wait"),
                 ("RESCUE", "Double press < 10s"))
        for column, (title, hint) in enumerate(hints):
            controls.grid_columnconfigure(column, weight=1, uniform="hint")
            tk.Label(controls, text=title, bg=PANEL, fg=MUTED,
                     font=("Menlo", 8, "bold"), anchor="w").grid(
                         row=0, column=column, sticky="w", padx=(0, 12))
            tk.Label(controls, text=hint, bg=PANEL, fg=TEXT,
                     font=("Helvetica Neue", 12), anchor="w").grid(
                         row=1, column=column, sticky="w", padx=(0, 12), pady=(6, 0))
        tk.Label(root, text="Use the blue USER button on your board. Keep USB connected.",
                 bg=BACKGROUND, fg=MUTED, font=("Helvetica Neue", 10)).grid(
                     row=4, column=0, sticky="w", padx=29, pady=(0, 19))

        self.start_worker()
        root.after(8, self.poll)
        root.after(100, self.bring_to_front)

    def bring_to_front(self) -> None:
        if not self.closing:
            self.root.lift()
            self.root.focus_force()

    def start_worker(self) -> None:
        if self.closing or (self.worker and self.worker.is_alive()):
            return
        self.stop.clear()
        self.session_ended = False
        self.frame_rate.samples.clear()
        self.displayed_rate.samples.clear()
        self.retry.configure(text="Reconnect")
        self.retry.grid_remove()
        self.set_status("preview" if self.args.preview else "connecting",
                        "Offline preview · actual firmware artwork" if self.args.preview
                        else "Connecting to the STM32 over USB…")
        self.worker = threading.Thread(target=self.preview_worker if self.args.preview
                                       else self.board_worker, daemon=True,
                                       name="garden-guard-display")
        self.worker.start()

    def board_worker(self) -> None:
        transport = None
        failure = None
        ended = False
        try:
            from live_display_transport import LiveDisplayTransport

            transport = LiveDisplayTransport(serial=self.args.serial,
                elf_path=getattr(self.args, "elf", None),
                bin_path=getattr(self.args, "bin", None))
            self.transport = transport
            if self.stop.is_set():
                return
            transport.connect()
            if not self.stop.is_set():
                self.events.put(("status", ("connected", "Board connected · waiting for its screen…")))
            period = 1.0 / self.args.fps
            deadline = time.monotonic()
            while not self.stop.is_set():
                frame = transport.read_frame()
                if frame is not None:
                    if len(frame) != FRAME_BYTES:
                        raise RuntimeError("The board returned an incomplete screen image")
                    self.events.put(("frame", (frame, time.monotonic(), time.time())))
                if transport.terminal:
                    ended = True
                    break
                # Account for USB/SWD read time instead of sleeping a full period
                # after every read. An overrun resumes immediately without a burst.
                now = time.monotonic()
                deadline = max(deadline + period, now)
                self.stop.wait(max(0.0, deadline - now))
        except Exception as error:
            if not self.stop.is_set():
                failure = str(error) or type(error).__name__
        finally:
            if transport is not None:
                transport.close()
            self.transport = None
            if failure is not None:
                self.events.put(("error", failure))
            elif ended and not self.stop.is_set():
                self.events.put(("ended", "Board terminated · polling stopped. Reset board, then Reconnect."))

    def preview_worker(self) -> None:
        try:
            from preview_display import compile_and_render, scenarios

            compiler = shutil.which("clang") or shutil.which("cc")
            if not compiler:
                raise RuntimeError("Preview needs a C compiler (clang or cc)")
            frames = scenarios()
            data = compile_and_render(frames, compiler)
            while not self.stop.is_set():
                for index, frame in enumerate(frames):
                    if self.stop.is_set():
                        return
                    self.events.put(("frame", (data[index * FRAME_BYTES:(index + 1) * FRAME_BYTES],
                                               time.monotonic(), time.time())))
                    self.events.put(("status", ("preview", f"Offline preview · {frame.label}")))
                    if self.stop.wait(frame.duration_ms / 1000):
                        return
        except Exception as error:
            if not self.stop.is_set():
                self.events.put(("error", str(error) or type(error).__name__))

    def set_status(self, state: str, detail: str) -> None:
        changed = self.current_status != state or self.status_detail != detail
        self.current_status = state
        self.status_detail = detail
        labels = {"connecting": "CONNECTING", "connected": "CONNECTED", "live": "LIVE · USB",
                  "preview": "OFFLINE PREVIEW", "error": "DISCONNECTED", "closing": "CLOSING",
                  "ended": "SESSION ENDED"}
        self.badge.configure(text=labels[state], fg=GREEN if state in ("live", "connected") else AMBER)
        self.status.configure(text=detail, fg=AMBER if state == "error" else MUTED)
        if changed:
            self.write_diagnostic()

    def write_diagnostic(self, *, force_snapshot: bool = False) -> None:
        """Optional local diagnostics, separate from the game and its window."""
        destination = self.args.status_file
        if destination is None:
            return
        now = time.time()
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            if self.frame is not None and (force_snapshot or now - self.last_snapshot_write >= 0.5):
                temporary = self.snapshot_path.with_name(f".{self.snapshot_path.name}.{os.getpid()}.tmp")
                temporary.write_bytes(framebuffer_ppm(self.frame))
                temporary.replace(self.snapshot_path)
                self.last_snapshot_write = now
                self.snapshot_written = True
            exists = False
            title = "Garden Guard — laptop display"
            try:
                exists = bool(self.root.winfo_exists())
                if exists:
                    self.last_geometry = self.root.geometry()
                    title = self.root.title()
            except self.tk.TclError:
                pass
            record = {
                "state": self.current_status,
                "detail": self.status_detail,
                "pid": os.getpid(),
                "preview": self.args.preview,
                "frame_count": self.frame_count,
                "session_ended": self.session_ended,
                "received_fps": round(self.frame_rate.value(time.monotonic()), 2),
                "displayed_frame_count": self.displayed_frame_count,
                "displayed_fps": round(self.displayed_rate.value(time.monotonic()), 2),
                "target_fps": self.args.fps,
                "last_frame_unix": self.last_frame_unix,
                "last_frame_timestamp": datetime.fromtimestamp(self.last_frame_unix, timezone.utc).isoformat()
                if self.last_frame_unix is not None else None,
                "updated_at": datetime.fromtimestamp(now, timezone.utc).isoformat(),
                "window": {"exists": exists, "title": title, "geometry": self.last_geometry},
                "snapshot": str(self.snapshot_path) if self.snapshot_written else None,
            }
            temporary = destination.with_name(f".{destination.name}.{os.getpid()}.tmp")
            temporary.write_text(json.dumps(record, indent=2) + "\n")
            temporary.replace(destination)
            self.last_diagnostic_write = now
        except OSError as error:
            if not self.diagnostic_warning:
                print(f"Could not write display diagnostics: {error}", file=sys.stderr)
                self.diagnostic_warning = True

    def poll(self) -> None:
        if self.closing:
            return
        newest_frame = None
        while True:
            try:
                event, value = self.events.get_nowait()
            except queue.Empty:
                break
            if event == "frame":
                newest_frame, received_at, received_unix = value
                self.frame_count += 1
                self.last_frame_unix = received_unix
                self.frame_rate.add(received_at)
                if not self.args.preview and self.current_status != "live":
                    self.set_status("live", "Live from your board · measuring frame rate…")
            elif event == "status":
                state, detail = value
                self.set_status(state, detail)
            elif event == "ended":
                self.session_ended = True
                self.set_status("ended", str(value))
                self.retry.configure(text="Reconnect after reset")
                self.retry.grid(row=0, column=1, sticky="e", padx=(12, 0))
            elif event == "error":
                suffix = " · last image paused" if self.frame or newest_frame else ""
                self.set_status("error", str(value) + suffix)
                self.retry.grid(row=0, column=1, sticky="e", padx=(12, 0))
                if self.frame is None and newest_frame is None:
                    self.canvas.itemconfigure(self.placeholder,
                                              text="Board display unavailable\nCheck USB, then reconnect.")
        if newest_frame is not None:
            self.frame = newest_frame
            self.image = self.tk.PhotoImage(data=framebuffer_ppm(self.frame), format="PPM")
            self.canvas.itemconfigure(self.placeholder, state="hidden")
            self.paint()
            self.displayed_frame_count += 1
            self.displayed_rate.add(time.monotonic())
            if not self.snapshot_written or self.session_ended:
                self.write_diagnostic(force_snapshot=True)
        now = time.monotonic()
        if self.current_status == "live" and now - self.last_rate_update >= 0.5:
            fps = self.frame_rate.value(now)
            self.set_status("live", f"Live from your board · {fps:.1f} fps · physical controls active")
            self.last_rate_update = now
        if time.time() - self.last_diagnostic_write >= 0.5:
            self.write_diagnostic()
        self.root.after(8, self.poll)

    def resize(self, event) -> None:
        self.zoom = max(1, min(event.width // WIDTH, event.height // HEIGHT))
        self.canvas.coords(self.placeholder, event.width // 2, event.height // 2)
        self.paint()

    def paint(self) -> None:
        if self.image is None:
            return
        self.zoomed_image = self.image.zoom(self.zoom, self.zoom)
        self.canvas.itemconfigure(self.image_item, image=self.zoomed_image)
        self.canvas.coords(self.image_item, self.canvas.winfo_width() // 2,
                           self.canvas.winfo_height() // 2)

    def close(self) -> None:
        if self.closing:
            return
        self.closing = True
        self.stop.set()
        self.retry.configure(state="disabled")
        self.set_status("closing", "Closing the laptop display…")
        # Transport.close interrupts a pending socket read and owns its OpenOCD cleanup.
        # Keep that cleanup off Tk's event thread as well.
        if self.transport is not None:
            threading.Thread(target=self.transport.close, daemon=True,
                             name="garden-guard-close").start()
        self.wait_for_close()

    def wait_for_close(self) -> None:
        if self.worker and self.worker.is_alive():
            self.root.after(40, self.wait_for_close)
        else:
            self.root.destroy()
            self.current_status = "closed"
            self.status_detail = "Laptop display closed"
            self.write_diagnostic(force_snapshot=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="ST-Link probe serial when multiple boards are connected")
    parser.add_argument("--elf", type=Path, help="Exact ELF currently flashed, when testing a separate candidate build")
    parser.add_argument("--bin", type=Path, help="Matching BIN for --elf (defaults to its .bin sibling)")
    parser.add_argument("--scale", type=int, choices=range(3, 17), default=6,
                        help="initial integer pixel magnification (default: 6)")
    parser.add_argument("--fps", type=int, choices=range(1, 121), metavar="N", default=30,
                        help="target board polling rate; actual rate is measured (default: 30)")
    parser.add_argument("--preview", action="store_true",
                        help="animate real firmware renderer scenes offline; do not connect USB")
    parser.add_argument("--self-test", action="store_true",
                        help="check framebuffer decoding without opening a window or board")
    parser.add_argument("--status-file", type=lambda value: Path(value).expanduser().resolve(),
                        help="write local diagnostic JSON and a sibling .ppm frame snapshot")
    args = parser.parse_args()
    if args.status_file and args.status_file.suffix.lower() == ".ppm":
        parser.error("--status-file must use a different suffix from its .ppm snapshot")
    if args.self_test:
        self_test()
        return
    try:
        import tkinter as tk
    except ImportError:
        launcher = Path("/opt/anaconda3/bin/python")
        hint = f'\nTry: "{launcher}" "{Path(__file__).resolve()}"' if launcher.exists() else ""
        parser.exit(1, "This Python does not include Tkinter. Use a Python with Tk 8.6+." + hint + "\n")
    try:
        root = tk.Tk()
    except tk.TclError as error:
        parser.exit(1, f"Could not open a laptop window: {error}\n")
    window = DisplayWindow(tk, root, args)
    previous_handlers = {}

    def request_close(_signum, _frame) -> None:
        root.after(0, window.close)

    for signum in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.signal(signum, request_close)
    try:
        root.mainloop()
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)


if __name__ == "__main__":
    main()
