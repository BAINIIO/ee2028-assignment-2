"""Bounded raw-output collector tests; fake transport and temporary UART logs only."""

import base64
import errno
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "garden_output_capture", Path(__file__).resolve().parents[1] / "tools/output_capture.py")
capture = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = capture
SPEC.loader.exec_module(capture)


class OutputCaptureTests(unittest.TestCase):
    def test_real_frame_batch_is_bounded_ordered_and_does_not_refresh_on_snapshot(self):
        class Transport:
            def __init__(self, **_options):
                self._last_sequence, self.terminal = 0, False
            def connect(self):
                pass
            def read_frame(self):
                self._last_sequence += 2
                self.terminal = self._last_sequence == 50
                return bytes([self._last_sequence]) * 1024
            def close(self):
                pass
        now = [100.0]
        source = capture.OledCapture(transport_factory=Transport, clock=lambda: now[0])
        with patch.object(source.stopping, "wait", return_value=False):
            source._run()
        result = source.snapshot()
        self.assertEqual(len(result["frames"]), 20)
        self.assertEqual([frame["sequence"] for frame in result["frames"]], list(range(12, 51, 2)))
        self.assertEqual(result["frames"][-1]["data"], result["data"])
        self.assertEqual(base64.b64decode(result["data"]), bytes([50]) * 1024)
        self.assertEqual(len(result["stream_id"]), 32)
        result["frames"][-1]["data"] = "changed by caller"
        now[0] = 200.0
        again = source.snapshot()
        self.assertEqual(again["received_at"], 100.0)
        self.assertNotEqual(again["frames"][-1]["data"], "changed by caller")
        self.assertEqual(again["status"], "terminated")
        self.assertNotEqual(source.stream_id, capture.OledCapture().stream_id)

    def test_oled_exact_page_bytes_terminal_and_single_connection(self):
        class Transport:
            def __init__(self, **_options):
                self._last_sequence, self.terminal, self.connects, self.closed = 2, True, 0, False
            def connect(self):
                self.connects += 1
            def read_frame(self):
                return bytes(range(256)) * 4
            def close(self):
                self.closed = True
        source = capture.OledCapture(transport_factory=Transport, clock=lambda: 100.0)
        source.start()
        source.thread.join(1)
        frame = source.snapshot()
        self.assertEqual(base64.b64decode(frame["data"]), bytes(range(256)) * 4)
        self.assertEqual((frame["sequence"], frame["received_at"], frame["status"]), (2, 100.0, "terminated"))
        self.assertEqual(source.transport.connects, 1)
        self.assertTrue(source.transport.closed)
        source.close()

    def test_oled_failed_verification_never_publishes_a_frame(self):
        class Transport:
            def __init__(self, **_options):
                pass
            def connect(self):
                raise ValueError("Firmware mismatch")
            def close(self):
                pass
        source = capture.OledCapture(transport_factory=Transport)
        source.start()
        source.thread.join(1)
        self.assertIsNone(source.snapshot())
        self.assertEqual(source.error, "Firmware mismatch")

    def test_uart_preserves_messages_and_original_host_receipts(self):
        source = capture.UartBuffer(clock=lambda: 1704067202.0, parse_timestamps=True)
        source.active = True
        source.feed(b"[2024-01-01T00:00:00+00:00]   UART message  \r\n", 1704067300.0)
        result = source.snapshot()
        self.assertEqual(result["lines"], [{"id": 1, "at": 1704067200.0, "text": "  UART message  "}])
        self.assertEqual(result["received_at"], 1704067200.0)
        self.assertEqual(result["status"], "online")
        source.feed(b"next partial", 1704067201.0)
        self.assertEqual(len(source.snapshot()["lines"]), 1)
        source.feed(b" complete\r\n", 1704067202.0)
        self.assertEqual(source.snapshot()["lines"][-1]["text"], "next partial complete")

    def test_direct_serial_keeps_timestamp_shaped_message_text_verbatim(self):
        source = capture.UartBuffer(clock=lambda: 1704067202.0)
        source.active = True
        source.feed(b"[2024-01-01T00:00:00+00:00]   Actual UART text  \r\n", 1704067202.0)
        line = source.snapshot()["lines"][0]
        self.assertEqual(line["text"], "[2024-01-01T00:00:00+00:00]   Actual UART text  ")
        self.assertEqual(line["at"], 1704067202.0)

    def test_uart_memory_is_bounded_and_terminal_is_latched(self):
        source = capture.UartBuffer(clock=lambda: 20.0)
        source.active = True
        for _ in range(200):
            source.feed(b"x" * 300 + b"\n", 20.0)
        result = source.snapshot()
        self.assertLessEqual(len(result["lines"]), 100)
        self.assertLessEqual(sum(len(line["text"].encode()) for line in result["lines"]), 24000)
        source.feed(b"x" * 100000, 20.0)
        self.assertLessEqual(len(source.pending), capture.MAX_UART_LINE_BYTES + 128)
        source.feed(b"\n" + capture.TERMINAL_UART_LINE.encode() + b"\r\nignored\r\n", 20.0)
        result = source.snapshot()
        self.assertEqual(result["status"], "terminated")
        self.assertEqual(result["lines"][-1]["text"], capture.TERMINAL_UART_LINE)
        source.feed(b"later\n", 21.0)
        self.assertEqual(source.snapshot()["lines"][-1]["text"], capture.TERMINAL_UART_LINE)

    def test_uart_log_uses_file_time_without_refreshing_old_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "uart.log"
            path.write_bytes(b"old raw UART\r\n")
            os.utime(path, (10.0, 10.0))
            now = [20.0]
            source = capture.UartLogCapture(path, clock=lambda: now[0])
            result = source.snapshot()
            self.assertEqual(result["received_at"], 10.0)
            self.assertEqual(result["status"], "stale")
            now[0] = 100.0
            self.assertEqual(source.snapshot()["status"], "offline")
            with path.open("ab") as stream:
                stream.write(b"new raw UART\n")
            os.utime(path, (100.0, 100.0))
            result = source.snapshot()
            self.assertEqual(result["lines"][-1]["text"], "new raw UART")
            self.assertEqual(result["status"], "online")
            path.unlink()
            self.assertEqual(source.snapshot()["status"], "offline")

    def test_no_capture_sources_means_no_frame_or_serial_data(self):
        source = capture.OutputCollector()
        source.start()
        self.assertEqual(source.snapshot(), {"oled": None, "uart": {
            "lines": [], "received_at": None, "status": "offline"}})
        source.close()

    def test_windows_uart_buffers_partial_reads_and_closes_on_terminal(self):
        chunks = [b"[2024-01-01T00:00:00+00:00] raw ", b"UART\r\npart",
                  b"ial\r\n" + capture.TERMINAL_UART_LINE.encode() + b"\r\nignored\n"]
        options = {}

        class Console:
            closed = False
            @property
            def in_waiting(self):
                return len(chunks[0]) if chunks else 0
            def read(self, count):
                self.assert_count = count
                if not chunks:
                    raise AssertionError("Capture must stop on terminal output")
                return chunks.pop(0)
            def close(self):
                self.closed = True

        console = Console()
        def serial_factory(**kwargs):
            options.update(kwargs)
            return console
        source = capture.SerialUartCapture("COM3", clock=lambda: 100.0)
        with patch.object(capture.os, "name", "nt"), patch.dict(
                sys.modules, {"serial": SimpleNamespace(Serial=serial_factory)}):
            source._run()
        result = source.snapshot()
        self.assertEqual([line["text"] for line in result["lines"]],
                         ["[2024-01-01T00:00:00+00:00] raw UART", "partial",
                          capture.TERMINAL_UART_LINE])
        self.assertEqual(result["status"], "terminated")
        self.assertEqual(result["received_at"], 100.0)
        self.assertEqual(options, {"port": "COM3", "baudrate": 115200, "bytesize": 8,
                                  "parity": "N", "stopbits": 1, "timeout": 0.2,
                                  "rtscts": False, "dsrdtr": False, "xonxoff": False})
        self.assertTrue(console.closed)
        self.assertFalse(source.active)
        self.assertIsNone(source.error)

    def test_windows_uart_read_error_closes_port_and_retains_real_lines(self):
        class Console:
            in_waiting = 3
            closed = False
            reads = 0
            def read(self, _count):
                self.reads += 1
                if self.reads == 1:
                    return b"ok\n"
                raise OSError("COM3 disconnected")
            def close(self):
                self.closed = True
        console = Console()
        source = capture.SerialUartCapture("COM3", clock=lambda: 100.0)
        with patch.object(capture.os, "name", "nt"), patch.dict(
                sys.modules, {"serial": SimpleNamespace(Serial=lambda **_: console)}):
            source._run()
        self.assertTrue(console.closed)
        self.assertEqual(source.error, "COM3 disconnected")
        self.assertEqual(source.snapshot()["lines"][0]["text"], "ok")
        self.assertEqual(source.snapshot()["status"], "offline")

    def test_windows_uart_close_stops_idle_reader(self):
        reading = threading.Event()
        class Console:
            in_waiting = 0
            closed = False
            def read(self, count):
                self.count = count
                reading.set()
                time.sleep(0.02)
                return b""
            def close(self):
                self.closed = True
        console = Console()
        source = capture.SerialUartCapture("COM3")
        with patch.object(capture.os, "name", "nt"), patch.dict(
                sys.modules, {"serial": SimpleNamespace(Serial=lambda **_: console)}):
            source.start()
            self.assertTrue(reading.wait(1))
            source.close()
        self.assertFalse(source.thread.is_alive())
        self.assertFalse(source.active)
        self.assertTrue(console.closed)
        self.assertEqual(console.count, 1)

    def test_windows_uart_missing_optional_dependency_is_reported(self):
        source = capture.SerialUartCapture("COM3")
        with patch.object(capture.os, "name", "nt"), patch.dict(sys.modules, {"serial": None}):
            source._run()
        self.assertIn("requires pyserial", source.error)
        self.assertEqual(source.snapshot()["status"], "offline")

    @unittest.skipUnless(os.name == "posix", "POSIX serial cleanup only")
    def test_posix_unplug_restore_error_still_releases_and_closes_descriptor(self):
        import fcntl
        import select
        import termios
        fd = 123
        for release_fails in (False, True):
            with self.subTest(release_fails=release_fails):
                original = [0, 0, 0, 0, 0, 0, [0] * 32]
                attributes = [0, 0, 0, 0, 0, 0, [0] * 32]
                def ioctl(_fd, request):
                    if release_fails and request == getattr(termios, "TIOCNXCL", None):
                        raise OSError(errno.ENXIO, "Device not configured")
                source = capture.SerialUartCapture("unused-test-port", clock=lambda: 100.0)
                with patch.object(capture.os, "open", return_value=fd), \
                        patch.object(fcntl, "flock"), \
                        patch.object(fcntl, "ioctl", side_effect=ioctl) as release, \
                        patch.object(termios, "tcgetattr", side_effect=[original, attributes]), \
                        patch.object(termios, "tcsetattr", side_effect=[
                            None, termios.error(errno.ENXIO, "Device not configured")]), \
                        patch.object(select, "select", return_value=([fd], [], [])), \
                        patch.object(capture.os, "read", side_effect=[
                            b"Actual UART before unplug  \r\n",
                            OSError(errno.ENXIO, "Device not configured")]), \
                        patch.object(capture.os, "close") as close:
                    source._run()
                close.assert_called_once_with(fd)
                if hasattr(termios, "TIOCNXCL"):
                    self.assertIn(unittest.mock.call(fd, termios.TIOCNXCL), release.call_args_list)
                self.assertIn("Device not configured", source.error)
                result = source.snapshot()
                self.assertEqual(result["status"], "offline")
                self.assertEqual(result["received_at"], 100.0)
                self.assertEqual(result["lines"], [{"id": 1, "at": 100.0,
                                                  "text": "Actual UART before unplug  "}])

    @unittest.skipUnless(os.name == "posix", "PTY serial capture is POSIX-only")
    def test_real_posix_uart_hangup_closes_descriptor_without_thread_error(self):
        master, slave = os.openpty()
        port = os.ttyname(slave)
        os.close(slave)
        opened = []
        thread_errors = []
        real_open = os.open
        def record_open(*args, **kwargs):
            fd = real_open(*args, **kwargs)
            opened.append(fd)
            return fd
        source = capture.SerialUartCapture(port, clock=lambda: 100.0)
        try:
            with patch.object(capture.os, "open", side_effect=record_open), \
                    patch.object(threading, "excepthook", side_effect=thread_errors.append):
                source.start()
                deadline = time.monotonic() + 2
                while not source.active and source.error is None and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(source.active, source.error)
                os.write(master, b"Last real PTY message\r\n")
                while not source.snapshot()["lines"] and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertEqual(source.snapshot()["lines"][0]["text"], "Last real PTY message")
                os.close(master)
                master = None
                source.thread.join(2)
                self.assertFalse(source.thread.is_alive())
                self.assertEqual(thread_errors, [])
                self.assertFalse(source.active)
                self.assertEqual(source.snapshot()["status"], "offline")
                self.assertEqual(source.snapshot()["received_at"], 100.0)
                self.assertEqual(len(opened), 1)
                with self.assertRaises(OSError) as error:
                    os.fstat(opened[0])
                self.assertEqual(error.exception.errno, errno.EBADF)
        finally:
            source.close()
            if master is not None:
                os.close(master)

    @unittest.skipUnless(os.name == "posix", "PTY serial capture is POSIX-only")
    def test_real_posix_uart_path_with_pty_and_exclusive_cleanup(self):
        import fcntl
        import termios
        master, slave = os.openpty()
        port = os.ttyname(slave)
        original = termios.tcgetattr(slave)
        os.close(slave)
        source = capture.SerialUartCapture(port, clock=lambda: 1704067202.0)
        try:
            source.start()
            deadline = time.monotonic() + 2
            while not source.active and source.error is None and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(source.active, source.error)
            os.write(master, b"[2024-01-01T00:00:00+00:00]   Real PTY UART  \r\nsecond\r\n")
            while len(source.snapshot()["lines"]) < 2 and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertEqual([line["text"] for line in source.snapshot()["lines"]],
                             ["[2024-01-01T00:00:00+00:00]   Real PTY UART  ", "second"])
            source.close()
            self.assertFalse(source.thread.is_alive())
            self.assertFalse(source.active)
            reopened = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
            try:
                fcntl.flock(reopened, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self.assertEqual(termios.tcgetattr(reopened), original)
            finally:
                os.close(reopened)
        finally:
            source.close()
            os.close(master)


if __name__ == "__main__":
    unittest.main()
