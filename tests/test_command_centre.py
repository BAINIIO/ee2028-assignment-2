"""Exercise real local HTTP/SSE/TCP sockets; no STM32 or physical UART required."""

import http.client
import importlib.util
import json
from pathlib import Path
import socket
import sys
import tempfile
import time
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "tools/command_centre.py"
SPEC = importlib.util.spec_from_file_location("garden_command_centre", SOURCE)
backend = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = backend
SPEC.loader.exec_module(backend)


def snapshot(seq=1, **changes):
    message = {
        "type": "snapshot", "id": "GG01", "seq": seq, "tick": seq & 0xffffffff,
        "mode": "standby", "attacked": False, "round": "active", "target": "none",
        "energy": 0, "lives": 3, "hits": 0, "score": 0, "rescue_ms": 0,
        "target_ms": 4000, "valid": 63, "abnormal": 0, "unavailable": 0,
        "t": 29.5, "p": 1010.0, "h": 60.0, "a": [0, 0, 9.81],
        "g": [0, 0, 0], "m": [10, 20, 30], "ir": False, "mission": 0,
        "nfc": True, "nfc_taps": 0, "wifi": True, "wifi_drop": 0,
    }
    message.update(changes)
    return message


class CommandCentreTests(unittest.TestCase):
    def setUp(self):
        self.files = tempfile.TemporaryDirectory(prefix="garden-http-test-")
        root = Path(self.files.name)
        (root / "index.html").write_text("<!doctype html><title>Real dashboard fixture</title>")
        (root / "app.js").write_text("console.log('fixture');")
        self.now = 1700000000.0
        self.app = backend.CommandCentre(
            host="127.0.0.1", http_port=0, ingest_port=0, static_root=root,
            allow_uart_ingest=True, clock=lambda: self.now).start()
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            client.close()
        self.app.close()
        self.files.cleanup()

    def request(self, method="GET", path="/api/state", payload=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.app.http.server_port, timeout=2)
        chosen = {"Content-Type": "application/json"} if payload is not None else {}
        chosen.update(headers or {})
        data = json.dumps(payload).encode() if payload is not None else None
        connection.request(method, path, body=data, headers=chosen)
        response = connection.getresponse()
        status, content_type, body = response.status, response.getheader("Content-Type"), response.read()
        connection.close()
        return status, json.loads(body) if "application/json" in content_type else body

    def board(self):
        client = socket.create_connection(self.app.tcp.server_address, timeout=2)
        client.settimeout(2)
        self.clients.append(client)
        return client

    def send(self, client, message):
        client.sendall(json.dumps(message, separators=(",", ":")).encode() + b"\n")

    def wait_state(self, predicate):
        end = time.monotonic() + 2
        while time.monotonic() < end:
            status, state = self.request()
            self.assertEqual(status, 200)
            if predicate(state):
                return state
            time.sleep(0.01)
        self.fail("Expected real server state did not arrive")

    def test_empty_state_has_no_simulated_readings_and_spa_routes(self):
        status, state = self.request()
        self.assertEqual(status, 200)
        self.assertIsNone(state["telemetry"])
        self.assertEqual(state["link"]["source"], "none")
        self.assertEqual(state["link"]["status"], "offline")
        self.assertEqual(state["events"], [])
        self.assertEqual(len(state["missions"]), 3)
        self.assertEqual(state["server"]["board_ingest_port"], self.app.tcp.server_address[1])
        for path in ("/", "/drone/GG01", "/drone/GG01/"):
            self.assertIn(b"Real dashboard fixture", self.request(path=path)[1])
        self.assertEqual(self.request(path="/app.js")[0], 200)
        self.assertEqual(self.request(path="/%2e%2e/command_centre.py")[0], 404)
        self.assertEqual(self.request(path="/api/missing")[0], 404)

    def test_snapshot_revision_instance_and_generation_timestamp(self):
        before = self.request()[1]
        self.assertEqual(before["revision"], 0)
        self.assertEqual(before["generated_at"], self.now)
        self.assertEqual(len(before["server_instance"]), 32)
        self.assertIsInstance(before["server_instance"], str)
        self.now += 0.5
        client = self.board()
        self.send(client, snapshot())
        after = self.wait_state(lambda state: state["telemetry"] is not None)
        self.assertGreater(after["revision"], before["revision"])
        self.assertEqual(after["server_instance"], before["server_instance"])
        self.assertGreater(after["generated_at"], before["generated_at"])
        self.now += 1.0
        heartbeat = self.request()[1]
        self.assertEqual(heartbeat["revision"], after["revision"])
        self.assertEqual(heartbeat["server_instance"], after["server_instance"])
        self.assertGreater(heartbeat["generated_at"], after["generated_at"])
        other = backend.StateStore(clock=lambda: self.now)
        self.assertNotEqual(other.server_instance, before["server_instance"])

    def test_tcp_sse_and_standby_mission_use_actual_socket(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.app.http.server_port, timeout=2)
        connection.request("GET", "/api/stream")
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(response.readline(), b"event: snapshot\n")
        initial = json.loads(response.readline()[6:])
        self.assertIsNone(initial["telemetry"])
        self.assertEqual(response.readline(), b"\n")
        client = self.board()
        self.send(client, snapshot())
        while True:
            self.assertEqual(response.readline(), b"event: snapshot\n")
            update = json.loads(response.readline()[6:])
            self.assertEqual(response.readline(), b"\n")
            if update["telemetry"]:
                break
        self.assertEqual(update["link"]["status"], "online")
        self.assertEqual(update["telemetry"]["received_at"], self.now)
        status, result = self.request("POST", "/api/mission", {"id": 2})
        self.assertEqual((status, result["requested"]), (202, 2))
        self.assertEqual(client.recv(100), b"MISSION 2\n")
        self.assertEqual(self.request()[1]["telemetry"]["mission"], 0)
        self.send(client, snapshot(2, mode="battle", target="zombie"))
        self.wait_state(lambda state: state["telemetry"]["mode"] == "battle")
        self.assertEqual(self.request("POST", "/api/mission", {"id": 1})[0], 409)
        response.close()
        connection.close()

    def test_stale_status_and_explicit_uart_fallback_never_enable_mission(self):
        client = self.board()
        self.send(client, snapshot())
        self.wait_state(lambda state: state["link"]["status"] == "online")
        self.now += 4
        state = self.request()[1]
        self.assertEqual((state["link"]["status"], state["link"]["age_ms"]), ("stale", 4000))
        self.assertEqual(self.request("POST", "/api/mission", {"id": 0})[0], 409)
        status, result = self.request("POST", "/api/ingest",
                                      {"source": "uart", "session": "boot1", "message": snapshot()})
        self.assertEqual((status, result["source"]), (202, "uart"))
        state = self.request()[1]
        self.assertEqual((state["link"]["source"], state["link"]["status"]), ("uart", "online"))
        self.assertTrue(state["link"]["uart"])
        self.assertFalse(state["link"]["wifi"])
        self.assertEqual(self.request("POST", "/api/mission", {"id": 0})[0], 409)
        self.now += 11
        self.assertEqual(self.request()[1]["link"]["status"], "offline")

    def test_sequence_rollover_duplicates_and_tick_wrap(self):
        client = self.board()
        self.send(client, snapshot(0xfffffffe, tick=0xfffffffe))
        self.wait_state(lambda state: state["telemetry"] is not None)
        self.send(client, snapshot(0xffffffff, tick=0xffffffff, energy=3))
        self.wait_state(lambda state: state["telemetry"]["energy"] == 3)
        self.send(client, snapshot(0, tick=0, energy=1))
        self.wait_state(lambda state: state["telemetry"]["seq"] == 0)
        self.send(client, snapshot(0, energy=10))
        self.send(client, snapshot(0xffffffff, energy=9))
        time.sleep(0.05)
        self.assertEqual(self.request()[1]["telemetry"]["energy"], 1)

    def test_terminal_latches_until_new_session_standby_and_old_socket_cannot_revive(self):
        first = self.board()
        self.send(first, snapshot(1, mode="battle", target="zombie"))
        self.wait_state(lambda state: state["telemetry"] is not None)
        self.send(first, snapshot(2, mode="terminated", round="lost"))
        self.wait_state(lambda state: state["link"]["status"] == "terminated")
        self.send(first, snapshot(3))
        time.sleep(0.03)
        self.assertEqual(self.request()[1]["telemetry"]["mode"], "terminated")
        second = self.board()
        self.send(second, snapshot(0))
        self.wait_state(lambda state: state["link"]["status"] == "online")
        self.send(first, snapshot(4, mode="terminated", round="lost"))
        time.sleep(0.03)
        self.assertEqual(self.request()[1]["telemetry"]["mode"], "standby")
        second.close()
        self.wait_state(lambda state: state["link"]["status"] == "offline")

    def test_event_termination_prevents_uart_fallback_revival(self):
        client = self.board()
        self.send(client, snapshot())
        self.wait_state(lambda state: state["telemetry"] is not None)
        self.send(client, {"type": "event", "id": "GG01", "seq": 2, "tick": 123,
                           "event": "terminated", "energy": 0, "score": 0, "target": "none"})
        self.wait_state(lambda state: state["link"]["status"] == "terminated")
        status, value = self.request("POST", "/api/ingest",
                                     {"source": "uart", "message": snapshot(100)})
        self.assertEqual(status, 202)
        self.assertFalse(value["accepted"])
        self.assertEqual(self.request()[1]["link"]["source"], "wifi")
        self.assertEqual(self.request()[1]["link"]["status"], "terminated")

    def test_invalid_protocol_does_not_replace_previous_sample(self):
        client = self.board()
        self.send(client, snapshot())
        self.wait_state(lambda state: state["telemetry"] is not None)
        self.send(client, snapshot(2, t=float("nan")))
        client.sendall(b'{"id":"GG01","id":"GG01"}\n')
        self.send(client, snapshot(3, id="other"))
        self.send(client, snapshot(4, energy=True))
        self.send(client, snapshot(5, g=[0, 0]))
        self.send(client, snapshot(6, t=None, valid=62, unavailable=1))
        state = self.wait_state(lambda item: item["telemetry"]["seq"] == 6)
        self.assertIsNone(state["telemetry"]["t"])
        self.assertEqual(state["telemetry"]["energy"], 0)
        self.assertEqual(self.app.store.invalid_messages, 5)
        client.sendall(b"x" * (backend.MAX_WIRE_BYTES + 1))
        self.assertEqual(client.recv(1), b"")

    def test_mission_request_origin_content_and_bounds_checks(self):
        self.assertEqual(self.request("POST", "/api/mission", {"id": 0})[0], 409)
        self.assertEqual(self.request("POST", "/api/mission", {"id": 0},
                                      {"Origin": "http://evil.example"})[0], 403)
        self.assertEqual(self.request("POST", "/api/mission", {"id": 0},
                                      {"Host": "evil.example"})[0], 403)
        self.assertEqual(self.request("POST", "/api/mission", {"id": 0},
                                      {"Content-Type": "text/plain"})[0], 415)
        for bad in (-1, 3, True, "1"):
            self.assertEqual(self.request("POST", "/api/mission", {"id": bad})[0], 400)
        self.assertEqual(self.request("POST", "/api/mission", {"id": 0, "padding": "x" * 5000})[0], 413)
        self.assertEqual(self.request("OPTIONS", "/api/mission")[0], 403)

    def test_history_is_bounded_without_changing_frontend_contract(self):
        peer = self.app.store.register(source="uart")
        for seq in range(350):
            self.app.store.ingest(peer, snapshot(seq))
        for seq in range(350, 600):
            self.app.store.ingest(peer, {"type": "event", "id": "GG01", "seq": seq,
                                        "tick": seq, "event": "charged", "energy": 3,
                                        "score": 0, "target": "none"})
        state = self.request()[1]
        self.assertEqual(set(state), {"telemetry", "link", "events", "missions", "server",
                                      "revision", "generated_at", "server_instance"})
        self.assertEqual(len(state["events"]), 200)
        self.assertEqual(len(self.app.store.history), 300)
        self.assertLess(state["events"][0]["id"], state["events"][-1]["id"])


if __name__ == "__main__":
    unittest.main()
