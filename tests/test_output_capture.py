"""Bounded raw-output collector tests; fake transport and temporary UART logs only."""

import base64
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest


SPEC = importlib.util.spec_from_file_location(
    "garden_output_capture", Path(__file__).resolve().parents[1] / "tools/output_capture.py")
capture = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = capture
SPEC.loader.exec_module(capture)


class OutputCaptureTests(unittest.TestCase):
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
