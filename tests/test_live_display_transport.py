"""Host-only checks for the live display protocol; never connect to a board."""

from pathlib import Path
import queue
import socket
import sys
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from live_display_transport import (FRAME_BYTES, LiveDisplayTransport, TransportError,
                                    _Symbols, _parse_flash_address, _parse_frame,
                                    _parse_symbols)


class FirmwareMetadataTests(unittest.TestCase):
    def test_sizes_and_addresses_come_from_elf_symbols(self):
        output = ("200002a0 g O .bss 00000004 laptop_display_sequence\n"
                  "200002a4 g O .bss 00000400 laptop_display_buffer\n")
        output += "200006a4 g O .bss 00000004 laptop_display_terminal\n"
        self.assertEqual(_parse_symbols(output), _Symbols(0x200002A0, 0x200002A4, 0x200006A4))
        with self.assertRaises(TransportError):
            _parse_symbols(output.replace("00000400", "00000040"))
        with self.assertRaises(TransportError):
            _parse_symbols(output.replace("200002a4", "080002a4"))

    def test_mirror_without_terminal_publication_is_rejected(self):
        output = ("200002a0 g O .bss 00000004 laptop_display_sequence\n"
                  "200002a4 g O .bss 00000400 laptop_display_buffer\n")
        with self.assertRaises(TransportError):
            _parse_symbols(output)

    def test_old_firmware_is_rejected_before_reading_ram(self):
        with self.assertRaisesRegex(TransportError, "no laptop display mirror"):
            _parse_symbols("20000000 g O .bss 00000004 old_buffer\n")

    def test_flash_upload_starts_at_load_address(self):
        output = ("  0 .isr_vector 000001bc 08000000 08000000 00001000 2**0\n"
                  "    CONTENTS, ALLOC, LOAD, READONLY, DATA\n"
                  "  1 .data 000002a0 20000000 0800c99c 0000e000 2**2\n"
                  "    CONTENTS, ALLOC, LOAD, DATA\n"
                  "  2 .bss 00001000 200002a0 200002a0 0000e2a0 2**2\n"
                  "    ALLOC\n")
        self.assertEqual(_parse_flash_address(output), 0x08000000)


class SnapshotTests(unittest.TestCase):
    @staticmethod
    def response(before=2, after=2, pixel="0x55", terminal=0):
        return f"frame {before} {after} {terminal} " + " ".join([pixel] * FRAME_BYTES)

    def test_complete_snapshot_preserves_page_bytes(self):
        self.assertEqual(_parse_frame(self.response()), (2, bytes([0x55] * FRAME_BYTES), False))

    def test_word_snapshot_uses_stm32_little_endian_byte_order(self):
        response = "frame32 8 8 0 " + " ".join(["0x12345678"] * (FRAME_BYTES // 4))
        self.assertEqual(_parse_frame(response), (8, b"\x78\x56\x34\x12" * 256, False))
        with self.assertRaises(TransportError):
            _parse_frame(response.replace("0x12345678", "0x100000000", 1))

    def test_active_or_changed_renders_are_discarded(self):
        for before, after in ((0, 0), (3, 3), (2, 4)):
            self.assertIsNone(_parse_frame(self.response(before, after)))
        self.assertIsNone(_parse_frame("pending 0"))
        self.assertIsNone(_parse_frame("pending 3"))

    def test_truncated_or_invalid_payloads_fail(self):
        for response in ("frame 2 2 0", self.response(pixel="256"), self.response(terminal=2),
                         "OpenOCD error"):
            with self.assertRaises(TransportError):
                _parse_frame(response)

    def test_unchanged_sequence_does_not_redraw(self):
        display = LiveDisplayTransport()
        display._symbols = _Symbols(0x200002A0, 0x200002A4, 0x200006A4)
        display._socket = Mock()
        display._rpc = Mock(return_value=self.response())
        self.assertEqual(display.read_frame(), bytes([0x55] * FRAME_BYTES))
        self.assertIsNone(display.read_frame())
        command = display._rpc.call_args.args[0]
        self.assertIn("read_memory 0x200002a0 32 1", command)
        self.assertIn("read_memory 0x200002a4 32 256", command)
        self.assertIn("read_memory 0x200006a4 32 1", command)
        self.assertNotIn("halt", command)
        self.assertNotIn("reset", command)
        display.close()

    def test_torn_snapshot_retries_and_returns_latest_coherent_frame(self):
        display = LiveDisplayTransport()
        display._symbols = _Symbols(0x200002A0, 0x200002A4, 0x200006A4)
        display._socket = Mock()
        display._rpc = Mock(side_effect=[self.response(2, 4), self.response(4, 4)])
        self.assertEqual(display.read_frame(), bytes([0x55] * FRAME_BYTES))
        self.assertEqual(display._last_sequence, 4)
        self.assertEqual(display._rpc.call_count, 2)
        self.assertGreaterEqual(display.read_seconds, 0)
        display.close()

    def test_torn_snapshot_retries_are_bounded(self):
        display = LiveDisplayTransport()
        display._symbols = _Symbols(0x200002A0, 0x200002A4, 0x200006A4)
        display._socket = Mock()
        display._rpc = Mock(return_value=self.response(2, 4))
        self.assertIsNone(display.read_frame())
        self.assertEqual(display._rpc.call_count, 3)
        display.close()

    def test_unchanged_frame_does_not_trigger_retries(self):
        display = LiveDisplayTransport()
        display._symbols = _Symbols(0x200002A0, 0x200002A4, 0x200006A4)
        display._socket = Mock()
        display._last_sequence = 2
        display._rpc = Mock(return_value=self.response())
        self.assertIsNone(display.read_frame())
        display._rpc.assert_called_once()
        display.close()

    def test_retry_does_not_start_when_expected_read_exceeds_time_budget(self):
        display = LiveDisplayTransport()
        display._symbols = _Symbols(0x200002A0, 0x200002A4, 0x200006A4)
        display._socket = Mock()
        display._rpc = Mock(return_value=self.response(2, 4))
        with patch("live_display_transport.time.monotonic", side_effect=[0, 0, 0.015, 0.015]):
            self.assertIsNone(display.read_frame())
        display._rpc.assert_called_once()
        self.assertEqual(display.read_seconds, 0.015)
        display.close()

    def test_final_frame_closes_connection_and_never_polls_again(self):
        display = LiveDisplayTransport()
        display._symbols = _Symbols(0x200002A0, 0x200002A4, 0x200006A4)
        client, server = socket.socketpair()
        server.settimeout(2)
        display._socket = client
        process = Mock()
        process.poll.return_value = None
        display._process = process
        display._rpc = Mock(return_value=self.response(terminal=1))
        try:
            self.assertEqual(display.read_frame(), bytes([0x55] * FRAME_BYTES))
            self.assertTrue(display.terminal)
            self.assertIn("Session ended", display.status)
            self.assertEqual(server.recv(1), b"")
            self.assertIsNone(display.read_frame())
            self.assertIsNone(display.read_frame())
            display._rpc.assert_called_once()
            display.close()
            process.terminate.assert_called_once()
        finally:
            display.close()
            server.close()

    def test_terminal_flag_in_torn_snapshot_is_not_trusted(self):
        display = LiveDisplayTransport()
        display._symbols = _Symbols(0x200002A0, 0x200002A4, 0x200006A4)
        display._socket = Mock()
        display._rpc = Mock(side_effect=[self.response(2, 4, pixel="0x11", terminal=1),
                                        self.response(4, 4, pixel="0x22", terminal=1)])
        self.assertEqual(display.read_frame(), bytes([0x22] * FRAME_BYTES))
        self.assertTrue(display.terminal)
        self.assertEqual(display._rpc.call_count, 2)
        self.assertIsNone(display.read_frame())
        self.assertEqual(display._rpc.call_count, 2)

    def test_terminal_flag_ends_even_an_unchanged_final_sequence(self):
        display = LiveDisplayTransport()
        display._symbols = _Symbols(0x200002A0, 0x200002A4, 0x200006A4)
        display._socket = Mock()
        display._last_sequence = 2
        display._rpc = Mock(return_value=self.response(terminal=1))
        self.assertEqual(display.read_frame(), bytes([0x55] * FRAME_BYTES))
        self.assertTrue(display.terminal)


class ConnectionLifecycleTests(unittest.TestCase):
    def test_tcl_response_can_arrive_in_fragments(self):
        display = LiveDisplayTransport()
        client, server = socket.socketpair()
        display._socket = client
        client.settimeout(2)

        def respond():
            self.assertEqual(server.recv(100), b"test\x1a")
            server.sendall(b"part ")
            server.sendall(b"two\x1a")

        worker = threading.Thread(target=respond)
        worker.start()
        try:
            self.assertEqual(display._rpc("test"), "part two")
        finally:
            display.close()
            server.close()
            worker.join(timeout=2)
        self.assertFalse(worker.is_alive())

    def test_close_interrupts_connection_and_only_stops_owned_process_once(self):
        display = LiveDisplayTransport()
        client, server = socket.socketpair()
        server.settimeout(2)
        display._socket = client
        process = Mock()
        process.poll.return_value = None
        display._process = process
        display.close()
        display.close()
        self.assertEqual(server.recv(1), b"")
        server.close()
        process.terminate.assert_called_once()
        process.wait.assert_called_once()
        with self.assertRaisesRegex(TransportError, "closed"):
            display.read_frame()


class WindowWorkerTests(unittest.TestCase):
    def test_final_screen_is_queued_before_ended_state_without_another_read(self):
        from live_display import DisplayWindow

        window = DisplayWindow.__new__(DisplayWindow)
        window.args = SimpleNamespace(serial=None, fps=30)
        window.stop = threading.Event()
        window.events = queue.Queue()
        window.transport = None
        display = Mock()
        display.terminal = True
        display.read_frame.return_value = bytes([0x42] * FRAME_BYTES)
        with patch("live_display_transport.LiveDisplayTransport", return_value=display):
            window.board_worker()
        events = []
        while not window.events.empty():
            events.append(window.events.get_nowait())
        self.assertEqual([event[0] for event in events], ["status", "frame", "ended"])
        self.assertEqual(events[1][1][0], bytes([0x42] * FRAME_BYTES))
        self.assertIn("Reset board", events[2][1])
        display.read_frame.assert_called_once()
        display.close.assert_called_once()
        self.assertIsNone(window.transport)

    def test_final_frame_remains_visible_in_ended_window(self):
        from live_display import DisplayWindow, FrameRate, framebuffer_ppm

        window = DisplayWindow.__new__(DisplayWindow)
        window.args = SimpleNamespace(preview=False)
        window.tk = Mock()
        window.root = Mock()
        window.canvas = Mock()
        window.badge = Mock()
        window.status = Mock()
        window.retry = Mock()
        window.placeholder = 1
        window.closing = False
        window.current_status = "connected"
        window.status_detail = "Board connected"
        window.frame = None
        window.frame_count = 0
        window.displayed_frame_count = 0
        window.session_ended = False
        window.frame_rate = FrameRate()
        window.displayed_rate = FrameRate()
        window.snapshot_written = True
        window.last_rate_update = 0.0
        window.last_diagnostic_write = time.time()
        window.paint = Mock()
        window.write_diagnostic = Mock()
        window.events = queue.Queue()
        pixels = bytes([0x73] * FRAME_BYTES)
        window.events.put(("frame", (pixels, time.monotonic(), time.time())))
        window.events.put(("ended", "Board terminated; reset and reconnect"))
        window.poll()
        self.assertEqual(window.frame, pixels)
        self.assertEqual(window.current_status, "ended")
        self.assertTrue(window.session_ended)
        self.assertEqual(window.displayed_frame_count, 1)
        window.tk.PhotoImage.assert_called_once_with(data=framebuffer_ppm(pixels), format="PPM")
        self.assertEqual(window.badge.configure.call_args.kwargs["text"], "SESSION ENDED")
        window.retry.configure.assert_called_with(text="Reconnect after reset")
        window.write_diagnostic.assert_called_with(force_snapshot=True)
        self.assertFalse(window.root.destroy.called)


if __name__ == "__main__":
    unittest.main()
