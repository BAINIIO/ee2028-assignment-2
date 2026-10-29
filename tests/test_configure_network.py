"""Credential encoding/validation checks with invented data only."""
from pathlib import Path
import re
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import configure_network as setup


class PrivateConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.config = {"ssid": "Someone’s Phone", "password": 'invented"\\password',
                       "server_ipv4": "192.0.2.10", "server_port": 8081}

    def test_utf8_and_c_escaping_round_trip(self):
        for value in (self.config["ssid"], self.config["password"], "1abc9"):
            encoded = setup.c_string(value)
            decoded = bytes(int(digits, 8) for digits in re.findall(r"\\([0-7]{3})", encoded))
            self.assertEqual(decoded, value.encode("utf-8"))
            self.assertNotIn(value, encoded)
        generated = setup.source(self.config)
        self.assertNotIn(self.config["password"], generated)
        self.assertIn(".server_ipv4 = {192,0,2,10}", generated)
        self.assertIn("Network_GetConfig", generated)

    def test_lengths_are_bytes_and_command_controls_are_rejected(self):
        for field, value in (("ssid", "é" * 17), ("ssid", "bad\rnetwork"),
                             ("password", "short"), ("password", "bad\npassword")):
            candidate = dict(self.config, **{field: value})
            with self.assertRaises(ValueError):
                setup.validate(candidate)

    def test_server_address_and_port_are_checked(self):
        for changes in ({"server_ipv4": "host.example"}, {"server_ipv4": "224.0.0.1"},
                        {"server_port": 0}, {"server_port": True}, {"server_port": 65536}):
            with self.assertRaises(ValueError):
                setup.validate(dict(self.config, **changes))

    def test_compile_uses_stdin_pipe_and_never_forwards_diagnostics(self):
        output = setup.ROOT / "build" / "host" / "test-private-network.o"
        result = type("Result", (), {"returncode": 1, "stderr": self.config["password"]})()
        with patch.object(setup.subprocess, "run", return_value=result) as compiler:
            with self.assertRaisesRegex(RuntimeError, "diagnostics withheld") as error:
                setup.compile_configuration(self.config, output)
        self.assertNotIn(self.config["password"], str(error.exception))
        command = compiler.call_args.args[0]
        self.assertIn("-pipe", command)
        self.assertEqual(command[-1], "-")
        self.assertNotIn(self.config["password"], " ".join(command))
        self.assertEqual(compiler.call_args.kwargs["input"], setup.source(self.config))
        self.assertFalse(output.exists())

    def test_object_cannot_be_written_to_tracked_source_tree(self):
        with self.assertRaises(ValueError):
            setup.compile_configuration(self.config, setup.ROOT / "Core" / "credentials.o")


if __name__ == "__main__":
    unittest.main()
