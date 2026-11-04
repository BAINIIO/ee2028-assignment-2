#!/usr/bin/env python3
"""Relay real local Garden Guard state to its hosted HTTPS dashboard.

Read-only: this program never opens a board/UART, flashes firmware or forwards
mission commands. Supply GARDEN_GUARD_RELAY_TOKEN or use --token-prompt; the
secret stays in process memory and is never included in a URL, file or log.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import getpass
import http.client
import json
import math
import os
import signal
import ssl
import sys
import threading
import time
from urllib.parse import urlsplit
import uuid
import warnings


DEFAULT_CLOUD = "https://garden-guard-gg01.spunkybroom4.chatgpt.site"
MAX_SYNC_BYTES = 64 * 1024
MAX_LOCAL_BYTES = 256 * 1024


class RelayError(ValueError):
    """A bounded, credential-free error suitable for the local console."""


@dataclass(frozen=True)
class Endpoint:
    scheme: str
    host: str
    port: int

    @classmethod
    def parse(cls, value: str, *, local: bool = False):
        try:
            parsed = urlsplit(value)
            expected = "http" if local else "https"
            if (parsed.scheme != expected or not parsed.hostname or parsed.username
                    or parsed.password or parsed.path not in {"", "/"}
                    or parsed.query or parsed.fragment):
                raise ValueError
            port = parsed.port or (80 if local else 443)
            if not 1 <= port <= 65535:
                raise ValueError
            host = parsed.hostname.encode("idna").decode("ascii")
            if local and host not in {"127.0.0.1", "localhost", "::1"}:
                raise ValueError
            return cls(expected, host, port)
        except (ValueError, UnicodeError, AttributeError) as error:
            message = ("Local URL must be a loopback HTTP origin" if local else
                       "Cloud URL must be an HTTPS origin without credentials or a path")
            raise RelayError(message) from error


def _reject_constant(_value):
    raise RelayError("JSON contains a non-finite number")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise RelayError("JSON contains duplicate object keys")
        result[key] = value
    return result


def decode_object(raw: bytes, maximum: int):
    if not raw or len(raw) > maximum:
        raise RelayError("JSON response is empty or exceeds its size limit")
    try:
        value = json.loads(raw.decode("utf-8"), parse_constant=_reject_constant,
                           object_pairs_hook=_unique_object)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise RelayError("Invalid JSON response") from error
    if not isinstance(value, dict):
        raise RelayError("JSON response must be an object")
    return value


def encode_object(value: dict, maximum: int):
    try:
        raw = json.dumps(value, separators=(",", ":"), allow_nan=False).encode("utf-8")
    except (TypeError, ValueError, RecursionError) as error:
        raise RelayError("State cannot be encoded as finite JSON") from error
    if len(raw) > maximum:
        raise RelayError("State exceeds the cloud relay size limit")
    return raw


class JsonHTTP:
    """Direct HTTP client: verified TLS, finite timeouts, no redirects/proxies."""

    def __init__(self, timeout: float = 4.0):
        self.timeout = timeout
        self.context = ssl.create_default_context()

    def request(self, endpoint: Endpoint, method: str, path: str, *, body=None,
                token=None, maximum=MAX_SYNC_BYTES, accepted=(200,)):
        headers = {"Accept": "application/json", "User-Agent": "GardenGuardRelay/1"}
        if body is not None:
            headers["Content-Type"] = "application/json"
        if token is not None:
            if endpoint.scheme != "https":
                raise RelayError("Relay authentication requires HTTPS")
            headers["Authorization"] = "Bearer " + token
        connection = (http.client.HTTPSConnection(endpoint.host, endpoint.port,
                      timeout=self.timeout, context=self.context)
                      if endpoint.scheme == "https" else
                      http.client.HTTPConnection(endpoint.host, endpoint.port, timeout=self.timeout))
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            # http.client does not follow redirects, so credentials cannot be
            # forwarded to a Location destination or downgraded to HTTP.
            if response.status not in accepted:
                raise RelayError(f"HTTP request rejected (status {response.status})")
            if response.getheader("Content-Encoding", "identity") != "identity":
                raise RelayError("Compressed API responses are not accepted")
            if response.getheader("Content-Type", "").split(";", 1)[0].strip() != "application/json":
                raise RelayError("API response must use application/json")
            return decode_object(response.read(maximum + 1), maximum)
        except (OSError, http.client.HTTPException) as error:
            raise RelayError("Connection unavailable or TLS verification failed") from error
        finally:
            connection.close()


def validate_snapshot(snapshot: dict):
    """Check the existing local envelope without changing data or timestamps."""
    link = snapshot.get("link")
    if (not isinstance(link, dict) or link.get("source") not in {"none", "wifi", "uart"}
            or link.get("status") not in {"online", "stale", "offline", "terminated"}
            or type(link.get("wifi")) is not bool or type(link.get("uart")) is not bool):
        raise RelayError("Local state has an invalid link description")
    if (type(snapshot.get("revision")) is not int or snapshot["revision"] < 0
            or not isinstance(snapshot.get("server_instance"), str)
            or not 1 <= len(snapshot["server_instance"]) <= 64
            or type(snapshot.get("generated_at")) not in {int, float}
            or not math.isfinite(snapshot["generated_at"])):
        raise RelayError("Local state has invalid ordering metadata")
    if snapshot.get("telemetry") is not None and not isinstance(snapshot["telemetry"], dict):
        raise RelayError("Local state has invalid telemetry")
    if not isinstance(snapshot.get("events"), list) or len(snapshot["events"]) > 200:
        raise RelayError("Local state has invalid bounded event history")
    if not isinstance(snapshot.get("missions"), list) or not isinstance(snapshot.get("server"), dict):
        raise RelayError("Local state has invalid dashboard metadata")
    return snapshot


def validate_token(token):
    if (not isinstance(token, str) or not 24 <= len(token) <= 1017
            or any(not 33 <= ord(character) <= 126 for character in token)):
        raise RelayError("A valid relay token is required")
    return token


class CloudRelay:
    def __init__(self, cloud_url: str, token: str, *, local_url="http://127.0.0.1:8080",
                 http=None, relay_id=None):
        self.cloud = Endpoint.parse(cloud_url)
        self.local = Endpoint.parse(local_url, local=True)
        self.token = validate_token(token)
        self.http = http if http is not None else JsonHTTP()
        self.relay_id = relay_id or uuid.uuid4().hex

    def sync_once(self):
        snapshot = validate_snapshot(self.http.request(
            self.local, "GET", "/api/state", maximum=MAX_LOCAL_BYTES))
        body = encode_object({"version": 1, "relay_id": self.relay_id,
                              "snapshot": snapshot, "acknowledgements": []}, MAX_SYNC_BYTES)
        reply = self.http.request(self.cloud, "POST", "/api/relay/sync", body=body,
                                  token=self.token, accepted=(200, 202))
        if reply.get("ok") is not True or reply.get("commands") != []:
            raise RelayError("Cloud response violates the read-only relay contract")
        return snapshot["link"]

    def run(self, stopping: threading.Event, *, interval=1.0, report=print):
        previous = None
        failures = 0
        while not stopping.is_set():
            started = time.monotonic()
            try:
                link = self.sync_once()
                message = f"Cloud relay connected; board source={link['source']}, status={link['status']}."
                failures = 0
            except RelayError as error:
                message = f"Cloud relay waiting: {error}."
                failures += 1
            if message != previous:
                report(message)
                previous = message
            delay = min(15.0, interval * 2 ** min(failures, 4)) if failures else interval
            stopping.wait(max(0.0, delay - (time.monotonic() - started)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cloud-url", default=os.environ.get("GARDEN_GUARD_CLOUD_URL", DEFAULT_CLOUD))
    parser.add_argument("--local-url", default="http://127.0.0.1:8080")
    parser.add_argument("--token-prompt", action="store_true", help="Read the token silently from a terminal")
    parser.add_argument("--interval", type=float, default=1.0, help="Sync interval in seconds (0.5–5)")
    parser.add_argument("--once", action="store_true", help="Send one real local snapshot, then exit")
    args = parser.parse_args(argv)
    stopping = threading.Event()
    try:
        if not math.isfinite(args.interval) or not 0.5 <= args.interval <= 5:
            raise RelayError("Sync interval must be between 0.5 and 5 seconds")
        if args.token_prompt:
            if not sys.stdin.isatty():
                raise RelayError("Hidden token entry requires an interactive terminal")
            with warnings.catch_warnings():
                warnings.simplefilter("error", getpass.GetPassWarning)
                token = getpass.getpass("Cloud relay token (hidden): ")
        else:
            token = os.environ.get("GARDEN_GUARD_RELAY_TOKEN")
        relay = CloudRelay(args.cloud_url, token, local_url=args.local_url)
        if args.once:
            link = relay.sync_once()
            print(f"State relayed; board source={link['source']}, status={link['status']}.", flush=True)
            return 0
        signal.signal(signal.SIGINT, lambda *_: stopping.set())
        signal.signal(signal.SIGTERM, lambda *_: stopping.set())
        relay.run(stopping, interval=args.interval, report=lambda value: print(value, flush=True))
        return 0
    except (RelayError, getpass.GetPassWarning) as error:
        print(f"Cloud relay unavailable: {error}.", file=sys.stderr)
        return 1
    except (EOFError, KeyboardInterrupt):
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
