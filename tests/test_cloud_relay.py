"""Read-only relay contract tests; no board, credentials or Internet involved."""

import contextlib
import base64
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import ssl
import sys
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "garden_cloud_relay", Path(__file__).resolve().parents[1] / "tools/cloud_relay.py")
relay = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = relay
SPEC.loader.exec_module(relay)
TEST_TOKEN = "host-test-relay-token-never-a-real-secret"


def snapshot(source="wifi"):
    return {"revision": 12, "generated_at": 1700000001.5, "server_instance": "local-test-session",
            "telemetry": {"id": "GG01", "received_at": 1700000000.0, "seq": 2, "wifi": source == "wifi"},
            "link": {"source": source, "status": "online", "wifi": source == "wifi",
                     "uart": source == "uart", "age_ms": 1500, "last_seen": 1700000000.0},
            "events": [], "missions": [{"id": 0, "name": "Test mission"}],
            "server": {"url": "http://127.0.0.1:8080", "drone_id": "GG01", "board_ingest_port": 8081}}


class FakeHTTP:
    def __init__(self, state=None, reply=None):
        self.state = state if state is not None else snapshot()
        self.reply = reply if reply is not None else {"ok": True, "commands": []}
        self.calls = []

    def request(self, endpoint, method, path, **options):
        self.calls.append((endpoint, method, path, options))
        if path == "/api/state":
            return copy.deepcopy(self.state)
        if path == "/api/relay/sync":
            return copy.deepcopy(self.reply)
        raise AssertionError("The relay must never send local commands")


class CloudRelayTests(unittest.TestCase):
    def test_token_limits_match_worker_authorization_header_bounds(self):
        for length in (24, 64, 1017):
            self.assertEqual(relay.validate_token("a" * length), "a" * length)
        for token in ("a" * 23, "a" * 1018, "a" * 24 + "\n", "a" * 24 + " ", None):
            with self.subTest(length=len(token) if isinstance(token, str) else None), \
                 self.assertRaises(relay.RelayError):
                relay.validate_token(token)

    def test_full_state_timestamps_and_source_are_unchanged(self):
        for source in ("wifi", "uart"):
            with self.subTest(source=source):
                client = FakeHTTP(snapshot(source))
                bridge = relay.CloudRelay("https://cloud.example", TEST_TOKEN, http=client, relay_id="test-relay")
                self.assertEqual(bridge.sync_once(), client.state["link"])
                self.assertEqual(len(client.calls), 2)
                self.assertNotIn("token", client.calls[0][3])
                cloud = client.calls[1]
                payload = json.loads(cloud[3]["body"])
                self.assertEqual(payload, {"version": 1, "relay_id": "test-relay",
                                          "snapshot": client.state, "acknowledgements": []})
                self.assertEqual(cloud[3]["token"], TEST_TOKEN)
                self.assertEqual(cloud[3]["accepted"], (200, 202))

    def test_no_readings_are_fabricated_while_board_is_offline(self):
        state = snapshot("none")
        state["telemetry"] = None
        state["link"].update(status="offline", age_ms=None, last_seen=None)
        client = FakeHTTP(state)
        relay.CloudRelay("https://cloud.example", TEST_TOKEN, http=client).sync_once()
        self.assertIsNone(json.loads(client.calls[-1][3]["body"])["snapshot"]["telemetry"])

    def test_outputs_only_uses_real_frame_and_lines_without_local_http(self):
        outputs = mock.Mock()
        outputs.snapshot.return_value = {"oled": {
            "width": 128, "height": 64, "format": "ssd1306-pages",
            "data": base64.b64encode(bytes(range(256)) * 4).decode(),
            "sequence": 2, "received_at": 1700000000.0, "status": "online"},
            "uart": {"lines": [{"id": 1, "at": 1700000000.5, "text": "  Actual UART message  "}],
                     "received_at": 1700000000.5, "status": "online"}}
        client = FakeHTTP()
        bridge = relay.CloudRelay("https://cloud.example", TEST_TOKEN, http=client,
            outputs=outputs, outputs_only=True, relay_id="real-capture-session", clock=lambda: 1700000001.0)
        bridge.sync_once()
        self.assertEqual(len(client.calls), 1)
        state = json.loads(client.calls[0][3]["body"])["snapshot"]
        self.assertEqual(state["outputs"], outputs.snapshot.return_value)
        self.assertIsNone(state["telemetry"])
        self.assertEqual(state["link"], {"source": "none", "status": "offline", "wifi": False,
                                         "uart": False, "last_seen": None, "age_ms": None})
        self.assertEqual((state["server_instance"], state["revision"]), ("real-capture-session", 1))
        self.assertEqual(state["generated_at"], 1700000001.0)
        bridge.sync_once()
        second = json.loads(client.calls[1][3]["body"])["snapshot"]
        self.assertEqual(second["revision"], 2)
        self.assertEqual(second["outputs"]["uart"]["received_at"], 1700000000.5)

    def test_output_json_growth_prunes_old_lines_without_creating_readings(self):
        outputs = mock.Mock()
        outputs.snapshot.return_value = {"oled": None, "uart": {
            "lines": [{"id": number, "at": 100.0, "text": "\x00" * 1000} for number in range(1, 25)],
            "received_at": 100.0, "status": "offline"}}
        client = FakeHTTP()
        relay.CloudRelay("https://cloud.example", TEST_TOKEN, http=client,
                         outputs=outputs, outputs_only=True).sync_once()
        raw = client.calls[0][3]["body"]
        self.assertLessEqual(len(raw), relay.MAX_SYNC_BYTES)
        state = json.loads(raw)["snapshot"]
        self.assertIsNone(state["telemetry"])
        lines = state["outputs"]["uart"]["lines"]
        self.assertLess(len(lines), 24)
        self.assertEqual(lines[-1], {"id": 24, "at": 100.0, "text": "\x00" * 1000})

    def test_missing_local_api_does_not_publish_an_old_snapshot(self):
        client = FakeHTTP()
        client.request = mock.Mock(side_effect=relay.RelayError("Local unavailable"))
        bridge = relay.CloudRelay("https://cloud.example", TEST_TOKEN, http=client)
        with self.assertRaises(relay.RelayError):
            bridge.sync_once()
        client.request.assert_called_once()

    def test_cloud_commands_are_never_forwarded(self):
        client = FakeHTTP(reply={"ok": True, "commands": [{"id": "x", "mission": 1}]})
        with self.assertRaisesRegex(relay.RelayError, "read-only"):
            relay.CloudRelay("https://cloud.example", TEST_TOKEN, http=client).sync_once()
        self.assertEqual([entry[2] for entry in client.calls], ["/api/state", "/api/relay/sync"])

    def test_cloud_requires_https_and_local_destination_requires_loopback(self):
        for value in ("http://cloud.example", "https://user:secret@cloud.example",
                      "https://cloud.example/api", "https://cloud.example/?token=secret"):
            with self.subTest(value=value), self.assertRaises(relay.RelayError):
                relay.Endpoint.parse(value)
        for value in ("http://192.168.1.2:8080", "http://example.com", "https://localhost:8080"):
            with self.subTest(value=value), self.assertRaises(relay.RelayError):
                relay.Endpoint.parse(value, local=True)
        self.assertEqual(relay.Endpoint.parse("http://[::1]:8080", local=True).host, "::1")

    def test_json_and_envelope_bounds(self):
        for raw in (b'{"a":NaN}', b'{"a":1,"a":2}', b'[]', b''):
            with self.subTest(raw=raw), self.assertRaises(relay.RelayError):
                relay.decode_object(raw, 1024)
        client = FakeHTTP()
        client.state["events"] = [{"message": "x" * relay.MAX_SYNC_BYTES}]
        with self.assertRaisesRegex(relay.RelayError, "size limit"):
            relay.CloudRelay("https://cloud.example", TEST_TOKEN, http=client).sync_once()
        self.assertEqual(len(client.calls), 1)

    def test_tls_verification_and_redirects_are_not_followed(self):
        response = mock.Mock(status=302)
        connection = mock.Mock()
        connection.getresponse.return_value = response
        with mock.patch.object(relay.http.client, "HTTPSConnection", return_value=connection) as constructor:
            http = relay.JsonHTTP()
            self.assertTrue(http.context.check_hostname)
            self.assertEqual(http.context.verify_mode, ssl.CERT_REQUIRED)
            with self.assertRaisesRegex(relay.RelayError, "302"):
                http.request(relay.Endpoint.parse("https://cloud.example"), "POST", "/api/relay/sync",
                             body=b'{}', token=TEST_TOKEN)
            constructor.assert_called_once()
            connection.request.assert_called_once()
            self.assertEqual(connection.request.call_args.kwargs["headers"]["Authorization"], "Bearer " + TEST_TOKEN)
            connection.close.assert_called_once()

    def test_network_failures_hide_untrusted_exception_details(self):
        connection = mock.Mock()
        connection.request.side_effect = OSError("untrusted value " + TEST_TOKEN)
        with mock.patch.object(relay.http.client, "HTTPSConnection", return_value=connection):
            with self.assertRaises(relay.RelayError) as caught:
                relay.JsonHTTP().request(relay.Endpoint.parse("https://cloud.example"), "POST", "/x", token=TEST_TOKEN)
            self.assertNotIn(TEST_TOKEN, str(caught.exception))

    def test_hidden_prompt_and_environment_do_not_log_token(self):
        for prompt in (False, True):
            with self.subTest(prompt=prompt):
                output, errors = io.StringIO(), io.StringIO()
                args = ["--once"] + (["--token-prompt"] if prompt else [])
                fake_bridge = mock.Mock()
                fake_bridge.sync_once.return_value = {"source": "uart", "status": "stale"}
                with mock.patch.dict(os.environ, {"GARDEN_GUARD_RELAY_TOKEN": TEST_TOKEN}), \
                     mock.patch.object(relay.sys.stdin, "isatty", return_value=True), \
                     mock.patch.object(relay.getpass, "getpass", return_value=TEST_TOKEN), \
                     mock.patch.object(relay, "CloudRelay", return_value=fake_bridge) as constructor, \
                     contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                    self.assertEqual(relay.main(args), 0)
                    self.assertEqual(constructor.call_args.args[1], TEST_TOKEN)
                self.assertNotIn(TEST_TOKEN, output.getvalue() + errors.getvalue())


if __name__ == "__main__":
    unittest.main()
