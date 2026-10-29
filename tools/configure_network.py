#!/usr/bin/env python3
"""Compile a private Wi-Fi configuration object without a plaintext source file.

Use hidden prompts, or --stdin-json with ssid, password, server_ipv4 and
optional server_port (8081). Credentials are never accepted as CLI arguments,
printed, saved as JSON/C, or included in compiler diagnostics.
"""

from __future__ import annotations

import argparse
import getpass
import ipaddress
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import warnings

ROOT = Path(__file__).resolve().parent.parent


def validate(configuration: dict) -> tuple[str, str, tuple[int, ...], int]:
    if not isinstance(configuration, dict):
        raise ValueError("Configuration must be a JSON object")
    values = []
    for field, minimum, maximum in (("ssid", 1, 32), ("password", 8, 63)):
        value = configuration.get(field)
        if not isinstance(value, str):
            raise ValueError(f"{field} must be a string")
        encoded = value.encode("utf-8")
        if not minimum <= len(encoded) <= maximum or any(byte < 32 or byte == 127 for byte in encoded):
            raise ValueError(f"{field} must contain {minimum}–{maximum} UTF-8 bytes and no ASCII controls")
        values.append(value)
    try:
        address = ipaddress.IPv4Address(configuration.get("server_ipv4"))
    except (ipaddress.AddressValueError, TypeError) as exc:
        raise ValueError("server_ipv4 must be a numeric IPv4 address") from exc
    octets = tuple(address.packed)
    if octets[0] == 0 or octets[0] >= 224:
        raise ValueError("server_ipv4 must be a unicast address")
    port = configuration.get("server_port", 8081)
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
        raise ValueError("server_port must be an integer from 1 to 65535")
    return values[0], values[1], octets, port


def c_string(value: str) -> str:
    # Fixed-width octal escapes cannot absorb following hex digits and safely
    # preserve quotes, backslashes and multibyte SSIDs in the generated C.
    return '"' + "".join(f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'


def source(configuration: dict) -> str:
    ssid, password, address, port = validate(configuration)
    return ("#include \"network_io.h\"\n"
            "static const NetworkConfig private_network = {\n"
            f"    .ssid = {c_string(ssid)},\n"
            f"    .password = {c_string(password)},\n"
            "    .server_ipv4 = {" + ",".join(str(octet) for octet in address) + "},\n"
            f"    .server_port = {port}u\n"
            "};\n"
            "const NetworkConfig *Network_GetConfig(void) { return &private_network; }\n")


def compile_configuration(configuration: dict, output: Path) -> None:
    generated = source(configuration)
    output = output.expanduser().resolve()
    if not output.is_relative_to((ROOT / "build").resolve()):
        raise ValueError("Private credential objects must stay inside this project's ignored build directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".network-private-", suffix=".o",
                                         dir=output.parent, delete=False) as stream:
            temporary = Path(stream.name)
        command = ["sh", str(ROOT / "tools/arm-tool.sh"), "gcc",
                   "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
                   "-std=c11", "-Os", "-pipe", "-ffunction-sections", "-fdata-sections",
                   "-I", str(ROOT / "Core/Inc"), "-x", "c", "-c", "-o", str(temporary), "-"]
        result = subprocess.run(command, input=generated, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=30, check=False, cwd=ROOT)
        if result.returncode:
            # Compiler diagnostics can quote source lines containing credentials.
            # Never echo them, including on failure.
            raise RuntimeError(f"Configuration compilation failed (exit {result.returncode}); diagnostics withheld")
        temporary.chmod(0o600)
        os.replace(temporary, output)
        temporary = None
    finally:
        generated = ""
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdin-json", action="store_true", help="Read one private JSON object from standard input")
    parser.add_argument("--output", type=Path, default=ROOT / "build/firmware/network_credentials.o",
                        help="Private object inside build/ (default: build/firmware/network_credentials.o)")
    args = parser.parse_args()
    try:
        if args.stdin_json:
            try:
                configuration = json.load(sys.stdin)
            except (json.JSONDecodeError, UnicodeError) as exc:
                raise ValueError("Could not read configuration JSON") from exc
        else:
            if not sys.stdin.isatty():
                raise ValueError("Use --stdin-json when no interactive terminal is available")
            with warnings.catch_warnings():
                # getpass normally falls back to an echoed input prompt when
                # terminal hiding is unavailable. Fail instead of exposing it.
                warnings.simplefilter("error", getpass.GetPassWarning)
                configuration = {
                    "ssid": getpass.getpass("Hotspot SSID (hidden): "),
                    "password": getpass.getpass("Hotspot password (hidden): "),
                    "server_ipv4": input("Mac server IPv4: ").strip(),
                    "server_port": 8081,
                }
        compile_configuration(configuration, args.output)
        print("Private network configuration object compiled. Run make to link it; no board was changed.")
        return 0
    except KeyboardInterrupt:
        return 130
    except (EOFError, ValueError, OSError, RuntimeError, getpass.GetPassWarning,
            subprocess.SubprocessError):
        # Do not expose arbitrary exception strings: input/diagnostics may
        # contain credentials. Field validation is available through validate().
        print("Network configuration failed. Check input fields, server address and Arm toolchain; no credentials were printed.",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
