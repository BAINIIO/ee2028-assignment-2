#!/usr/bin/env python3
"""Local Garden Guard HTTP/SSE dashboard and native board WiFi receiver.

This program never opens the physical UART or connects to ST-Link. The board
pushes bounded newline-delimited JSON to the TCP port; browsers use HTTP/SSE.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
from collections import deque
from dataclasses import dataclass, field
import json
import math
import mimetypes
from pathlib import Path
import re
import signal
import socket
import socketserver
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parent.parent
DRONE_ID = "GG01"
MAX_WIRE_BYTES = 2048
MAX_HTTP_BODY = 4096
MAX_CLIENTS = 16
MISSIONS = [
    {"id": 0, "name": "Orchard Perimeter",
     "description": "Defend the orchard approaches and track perimeter incursions."},
    {"id": 1, "name": "Evacuation Corridor",
     "description": "Protect friendly evacuees while defending the escape route."},
    {"id": 2, "name": "Greenhouse Shelter",
     "description": "Monitor shelter conditions and defend the greenhouse perimeter."},
]
EVENT_MESSAGES = {
    "standby": "Drone entered standby.", "battle": "Drone entered battle.",
    "charged": "Laser energy charged.", "fired": "Laser fired automatically.",
    "hit": "Zombie hit.", "human_hit": "Friendly human hit: penalty.",
    "human_saved": "Friendly human passed safely.",
    "missed": "Zombie escaped: penalty.", "target": "New target selected.",
    "attacked": "Drone attacked: rescue countdown started.",
    "rescued": "Drone rescued by the physical control.",
    "victory": "Garden round won.", "defeat": "Garden round lost.",
    "terminated": "Drone terminated; communication is ending.",
    "nfc_tap": "NFC handover detected.",
    "mission_assigned": "Board accepted a mission assignment.",
    "mission_rejected": "Board rejected a mission assignment.",
}


class ProtocolError(ValueError):
    """A message is malformed, oversized or outside the agreed wire contract."""


def _no_constant(value):
    raise ProtocolError(f"Non-finite JSON constant is not allowed: {value}")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError("Duplicate JSON object key")
        result[key] = value
    return result


def parse_json(raw: bytes, maximum=MAX_WIRE_BYTES):
    if not raw or len(raw) > maximum:
        raise ProtocolError("Message exceeds its size limit or is empty")
    try:
        value = json.loads(raw.decode("utf-8"), parse_constant=_no_constant,
                           object_pairs_hook=_unique_object)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise ProtocolError("Invalid JSON message") from error
    if not isinstance(value, dict):
        raise ProtocolError("JSON message must be an object")
    return value


def _integer(message, name, minimum=0, maximum=0xffffffff):
    value = message.get(name)
    if type(value) is not int or not minimum <= value <= maximum:
        raise ProtocolError(f"Invalid integer field: {name}")
    return value


def _boolean(message, name):
    value = message.get(name)
    if type(value) is not bool:
        raise ProtocolError(f"Invalid boolean field: {name}")
    return value


def _choice(message, name, choices):
    value = message.get(name)
    if not isinstance(value, str) or value not in choices:
        raise ProtocolError(f"Invalid choice field: {name}")
    return value


def _quantity(value, name, low, high):
    if value is None:
        return None
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ProtocolError(f"Non-finite or non-numeric field: {name}")
    if not low <= value <= high:
        raise ProtocolError(f"Implausible field: {name}")
    return value


def validate_message(message):
    """Return a sanitized flat wire message; never retain arbitrary fields."""
    if message.get("id") != DRONE_ID:
        raise ProtocolError("Unknown drone identifier")
    result = {"id": DRONE_ID,
              "type": _choice(message, "type", {"snapshot", "event"}),
              "seq": _integer(message, "seq"),
              "tick": _integer(message, "tick"),
              "energy": _integer(message, "energy", 0, 10),
              "score": _integer(message, "score", -32768, 32767),
              "target": _choice(message, "target", {"none", "zombie", "human"})}
    if result["type"] == "event":
        event = message.get("event")
        if not isinstance(event, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,47}", event):
            raise ProtocolError("Invalid event name")
        result["event"] = event
        return result
    result.update({
        "mode": _choice(message, "mode", {"standby", "battle", "terminated"}),
        "round": _choice(message, "round", {"active", "won", "lost"}),
        "attacked": _boolean(message, "attacked"),
        "lives": _integer(message, "lives", 0, 3),
        "hits": _integer(message, "hits", 0, 255),
        "rescue_ms": _integer(message, "rescue_ms", 0, 10000),
        "target_ms": _integer(message, "target_ms", 0, 65535),
        "valid": _integer(message, "valid", 0, 63),
        "abnormal": _integer(message, "abnormal", 0, 63),
        "unavailable": _integer(message, "unavailable", 0, 63),
        "ir": _boolean(message, "ir"),
        "mission": _integer(message, "mission", 0, 2),
        "nfc": _boolean(message, "nfc"),
        "nfc_taps": _integer(message, "nfc_taps"),
        "wifi": _boolean(message, "wifi"),
        "wifi_drop": _integer(message, "wifi_drop"),
    })
    for name, low, high in (("t", -273.15, 1000), ("p", 0, 2000), ("h", 0, 100)):
        if name not in message:
            raise ProtocolError(f"Missing field: {name}")
        result[name] = _quantity(message[name], name, low, high)
    for name, bound in (("a", 1000), ("g", 10000), ("m", 100000)):
        values = message.get(name)
        if not isinstance(values, list) or len(values) != 3:
            raise ProtocolError(f"Invalid three-axis field: {name}")
        result[name] = [_quantity(value, name, -bound, bound) for value in values]
    return result


def _forward_sequence(candidate, previous):
    return previous is None or 0 < ((candidate - previous) & 0xffffffff) < 0x80000000


@dataclass
class BoardPeer:
    generation: int
    connection: socket.socket | None
    source: str = "wifi"
    sequence: int | None = None
    first: bool = True
    connected: bool = True
    send_lock: threading.Lock = field(default_factory=threading.Lock)

    def send_mission(self, mission):
        if self.connection is None or not self.connected:
            raise OSError("Board connection is unavailable")
        with self.send_lock:
            self.connection.sendall(f"MISSION {mission}\n".encode("ascii"))


class StateStore:
    """One drone, bounded history, and a condition for SSE subscribers."""

    def __init__(self, *, stale_after=3.0, offline_after=10.0, clock=time.time):
        self.condition = threading.Condition(threading.RLock())
        self.clock = clock
        self.stale_after, self.offline_after = stale_after, offline_after
        self.events = deque(maxlen=200)
        self.history = deque(maxlen=300)
        self.records = {}
        self.active_wifi = None
        self.uart_sessions = {}
        self.generation = 0
        self.event_id = 0
        self.version = 0
        self.server_instance = uuid.uuid4().hex
        self.terminal_peer = None
        self.invalid_messages = 0

    def _changed(self):
        self.version += 1
        self.condition.notify_all()

    def _event(self, kind, message, source, level="info"):
        self.event_id += 1
        self.events.append({"id": self.event_id, "at": self.clock(), "kind": kind,
                            "message": message, "level": level, "source": source})

    def register(self, connection=None, source="wifi"):
        with self.condition:
            self.generation += 1
            return BoardPeer(self.generation, connection, source)

    def disconnect(self, peer):
        with self.condition:
            peer.connected = False
            if self.active_wifi is peer:
                self._event("link_disconnected", "Board WiFi connection closed.", "wifi", "warning")
                self._changed()

    def ingest(self, peer, message):
        message = validate_message(message)
        with self.condition:
            if not _forward_sequence(message["seq"], peer.sequence):
                return False
            # An older socket must never overwrite the replacement connection.
            if peer.source == "wifi" and self.active_wifi is not None:
                if peer.generation < self.active_wifi.generation:
                    return False
            if self.terminal_peer is not None and message.get("mode") != "terminated":
                reset = (peer.source == "wifi" and peer.first
                         and peer.generation > self.terminal_peer.generation
                         and message["type"] == "snapshot" and message["mode"] == "standby")
                if reset:
                    self.terminal_peer = None
                    self._event("reset", "A new board session entered standby.", peer.source)
                elif message.get("event") != "terminated":
                    return False
            if peer.source == "wifi" and self.active_wifi is not peer:
                if message["type"] != "snapshot":
                    return False  # Establish a new session with a complete snapshot.
                self.active_wifi = peer
                self._event("link_connected", "Native board WiFi telemetry connected.", "wifi")
            peer.sequence, peer.first = message["seq"], False
            now = self.clock()
            if message["type"] == "snapshot":
                message["received_at"] = now
                self.records[peer.source] = (message, peer, now)
                self.history.append(dict(message))
                if message["mode"] == "terminated":
                    self.terminal_peer = peer
            else:
                kind = message["event"]
                level = ("critical" if kind in {"attacked", "terminated"} else
                         "warning" if kind in {"human_hit", "missed", "defeat", "mission_rejected"}
                         else "info")
                self._event(kind, EVENT_MESSAGES.get(kind, kind.replace("_", " ").capitalize()),
                            peer.source, level)
                if kind == "terminated":
                    self.terminal_peer = peer
            self._changed()
            return True

    def ingest_uart(self, message, session):
        with self.condition:
            peer = self.uart_sessions.get(session)
            if peer is None:
                if len(self.uart_sessions) >= 8:
                    self.uart_sessions.pop(next(iter(self.uart_sessions)))
                peer = self.register(source="uart")
                self.uart_sessions[session] = peer
        return self.ingest(peer, message)

    def snapshot(self, server):
        with self.condition:
            now = self.clock()
            wifi = self.records.get("wifi")
            uart = self.records.get("uart")

            def fresh(record, source):
                return bool(record and now - record[2] <= self.stale_after
                            and (source != "wifi" or record[1].connected))

            if self.terminal_peer is not None:
                record = self.records.get(self.terminal_peer.source)
                source = self.terminal_peer.source if record else "none"
            elif fresh(wifi, "wifi"):
                record, source = wifi, "wifi"
            elif fresh(uart, "uart"):
                record, source = uart, "uart"
            elif wifi or uart:
                source = "wifi" if wifi and (not uart or wifi[2] >= uart[2]) else "uart"
                record = self.records[source]
            else:
                record, source = None, "none"
            telemetry = dict(record[0]) if record else None
            age = max(0, int((now - record[2]) * 1000)) if record else None
            connected = bool(record and (source != "wifi" or record[1].connected))
            if self.terminal_peer is not None:
                status = "terminated"
            elif record is None or not connected or age > self.offline_after * 1000:
                status = "offline"
            elif age > self.stale_after * 1000:
                status = "stale"
            else:
                status = "online"
            return {"revision": self.version, "generated_at": now,
                    "server_instance": self.server_instance, "telemetry": telemetry,
                    "link": {"source": source, "status": status,
                             "wifi": fresh(wifi, "wifi") and bool(wifi[0].get("wifi")),
                             "uart": fresh(uart, "uart"),
                             "last_seen": record[2] if record else None, "age_ms": age},
                    "events": list(self.events), "missions": MISSIONS,
                    "server": dict(server)}

    def request_mission(self, mission):
        with self.condition:
            record = self.records.get("wifi")
            if not record or self.terminal_peer is not None:
                raise ValueError("A live native WiFi board in standby is required")
            telemetry, peer, received = record
            if (not peer.connected or peer is not self.active_wifi
                    or self.clock() - received > self.stale_after
                    or not telemetry["wifi"] or telemetry["mode"] != "standby"
                    or telemetry["attacked"]):
                raise ValueError("Mission assignment requires live WiFi and standby")
            try:
                peer.send_mission(mission)
            except OSError as error:
                peer.connected = False
                self._changed()
                raise ValueError("The board WiFi connection closed before assignment") from error
            self._event("mission_requested", f"Requested mission: {MISSIONS[mission]['name']}.", "wifi")
            self._changed()


class _TCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False

    def __init__(self, address, application):
        self.application = application
        self.slots = threading.BoundedSemaphore(MAX_CLIENTS)
        super().__init__(address, _BoardHandler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class _BoardHandler(socketserver.BaseRequestHandler):
    def handle(self):
        app = self.server.application
        peer = app.store.register(self.request)
        self.request.settimeout(1.0)
        pending, invalid = bytearray(), 0
        try:
            while not app.stopping.is_set():
                try:
                    chunk = self.request.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                pending.extend(chunk)
                while b"\n" in pending:
                    line, _, rest = pending.partition(b"\n")
                    pending = bytearray(rest)
                    try:
                        app.store.ingest(peer, parse_json(bytes(line).rstrip(b"\r")))
                    except ProtocolError:
                        invalid += 1
                        with app.store.condition:
                            app.store.invalid_messages += 1
                        if invalid >= 8:
                            return
                if len(pending) > MAX_WIRE_BYTES:
                    with app.store.condition:
                        app.store.invalid_messages += 1
                    break
        except OSError:
            pass
        finally:
            app.store.disconnect(peer)


class _HTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, address, handler):
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(address, handler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class _HTTPHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "GardenGuard/1"

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, _format, *_args):
        pass  # No telemetry, IP addresses or arbitrary request values in logs.

    @property
    def app(self):
        return self.server.application

    def _headers(self, status, content_type, length=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "same-origin")
        if length is not None:
            self.send_header("Content-Length", str(length))
        self.end_headers()

    def _json(self, status, value):
        data = json.dumps(value, separators=(",", ":"), allow_nan=False).encode()
        self._headers(status, "application/json; charset=utf-8", len(data))
        self.wfile.write(data)

    def _allowed_host(self):
        host = self.headers.get("Host", "")
        try:
            parsed = urlsplit("http://" + host)
            return (not parsed.username and not parsed.password and not parsed.path
                    and not parsed.query and not parsed.fragment
                    and parsed.hostname in self.app.allowed_hosts
                    and (parsed.port or 80) == self.app.http.server_port)
        except ValueError:
            return False

    def _same_origin(self):
        if not self._allowed_host():
            return False
        origin = self.headers.get("Origin")
        if not origin:
            return True  # JSON POST is not a browser simple/form request.
        try:
            parsed = urlsplit(origin)
            target = urlsplit("http://" + self.headers["Host"])
            return (not parsed.username and not parsed.password and not parsed.path
                    and parsed.scheme == "http" and parsed.hostname == target.hostname
                    and (parsed.port or 80) == (target.port or 80))
        except ValueError:
            return False

    def do_OPTIONS(self):
        self._json(403, {"error": "Cross-origin access is not enabled"})

    def do_GET(self):
        if not self._allowed_host():
            self._json(400, {"error": "Unexpected Host header"})
            return
        path = urlsplit(self.path).path
        if path == "/api/state":
            self._json(200, self.app.store.snapshot(self.app.server_info))
        elif path == "/api/stream":
            if self._same_origin():
                self._stream()
            else:
                self._json(403, {"error": "Same-origin dashboard requests are required"})
        elif path.startswith("/api/"):
            self._json(404, {"error": "Unknown API endpoint"})
        else:
            self._static(path)

    def _stream(self):
        if not self.app.stream_slots.acquire(blocking=False):
            self._json(503, {"error": "Too many live dashboard connections"})
            return
        try:
            self.connection.settimeout(5)
            self._headers(200, "text/event-stream; charset=utf-8")
            while not self.app.stopping.is_set():
                with self.app.store.condition:
                    version = self.app.store.version
                    payload = self.app.store.snapshot(self.app.server_info)
                data = json.dumps(payload, separators=(",", ":"), allow_nan=False)
                self.wfile.write(f"event: snapshot\ndata: {data}\n\n".encode())
                self.wfile.flush()
                with self.app.store.condition:
                    self.app.store.condition.wait_for(
                        lambda: self.app.store.version != version or self.app.stopping.is_set(),
                        timeout=1.0)
        except (OSError, ValueError):
            pass
        finally:
            self.close_connection = True
            self.app.stream_slots.release()

    def _static(self, path):
        try:
            decoded = unquote(path)
            if "\x00" in decoded:
                raise ValueError
            if decoded in {"/", "/drone/GG01", "/drone/GG01/"}:
                decoded = "/index.html"
            candidate = (self.app.static_root / decoded.lstrip("/")).resolve()
            if not candidate.is_relative_to(self.app.static_root) or not candidate.is_file():
                self._json(404, {"error": "Dashboard asset not found"})
                return
            content = candidate.read_bytes()
        except (ValueError, OSError):
            self._json(404, {"error": "Dashboard asset not found"})
            return
        content_type = mimetypes.guess_type(candidate.name)[0] or "application/octet-stream"
        self._headers(200, content_type, len(content))
        self.wfile.write(content)

    def do_POST(self):
        self.close_connection = True
        path = urlsplit(self.path).path
        if path not in {"/api/mission", "/api/ingest"}:
            self._json(404, {"error": "Unknown API endpoint"})
            return
        if not self._same_origin():
            self._json(403, {"error": "Same-origin dashboard requests are required"})
            return
        if self.headers.get("Content-Type", "").split(";", 1)[0].strip() != "application/json":
            self._json(415, {"error": "Use application/json"})
            return
        if self.headers.get("Transfer-Encoding"):
            self._json(400, {"error": "Chunked request bodies are not accepted"})
            return
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        if not 0 < length <= MAX_HTTP_BODY:
            self._json(413 if length > MAX_HTTP_BODY else 411,
                       {"error": "A bounded Content-Length is required"})
            return
        try:
            self.connection.settimeout(3)
            payload = parse_json(self.rfile.read(length), MAX_HTTP_BODY)
            if path == "/api/mission":
                mission = _integer(payload, "id", 0, 2)
                self.app.store.request_mission(mission)
                self._json(202, {"ok": True, "requested": mission})
            else:
                if not self.app.allow_uart_ingest or self.client_address[0] not in {"127.0.0.1", "::1"}:
                    self._json(403, {"error": "Optional UART ingest is loopback-only and disabled by default"})
                    return
                session = payload.get("session", "default")
                if (payload.get("source") != "uart" or not isinstance(session, str)
                        or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", session)
                        or not isinstance(payload.get("message"), dict)):
                    raise ProtocolError("Use an explicit UART source, session and message object")
                accepted = self.app.store.ingest_uart(payload["message"], session)
                self._json(202, {"ok": True, "accepted": accepted, "source": "uart"})
        except ProtocolError as error:
            self._json(400, {"error": str(error)})
        except ValueError as error:
            self._json(409, {"error": str(error)})
        except (OSError, RecursionError):
            self.close_connection = True


def local_address():
    """Ask the OS routing table for an address; UDP connect sends no packet."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect(("192.0.2.1", 9))
            return route.getsockname()[0]
    except OSError:
        return "127.0.0.1"


class CommandCentre:
    def __init__(self, *, host="0.0.0.0", http_port=8080, ingest_port=8081,
                 public_url=None, static_root=None, allow_uart_ingest=False,
                 stale_after=3.0, offline_after=10.0, clock=time.time):
        self.stopping = threading.Event()
        self.store = StateStore(stale_after=stale_after, offline_after=offline_after, clock=clock)
        self.static_root = Path(static_root or ROOT / "web/command-centre/dist").resolve()
        self.allow_uart_ingest = allow_uart_ingest
        self.stream_slots = threading.BoundedSemaphore(MAX_CLIENTS)
        self.http = _HTTPServer((host, http_port), _HTTPHandler)
        self.http.application = self
        try:
            self.tcp = _TCPServer((host, ingest_port), self)
        except BaseException:
            self.http.server_close()
            raise
        address = local_address() if host in {"0.0.0.0", ""} else host
        if public_url:
            try:
                parsed = urlsplit(public_url)
                port_matches = (parsed.port or 80) == self.http.server_port
            except ValueError:
                parsed, port_matches = urlsplit(""), False
            if (parsed.scheme != "http" or not parsed.hostname or parsed.username
                    or parsed.password or parsed.path not in {"", "/"}
                    or parsed.query or parsed.fragment or not port_matches):
                self.http.server_close()
                self.tcp.server_close()
                raise ValueError("--public-url must be an HTTP origin on the HTTP server port, without credentials or a path")
            url = public_url.rstrip("/")
            address = parsed.hostname
        else:
            url = f"http://{address}:{self.http.server_port}"
        self.allowed_hosts = {address, "localhost", "127.0.0.1"}
        self.server_info = {"url": url, "drone_id": DRONE_ID,
                            "board_ingest_port": self.tcp.server_address[1]}
        self.threads = []

    def start(self):
        for server, name in ((self.http, "garden-http"), (self.tcp, "garden-wifi")):
            thread = threading.Thread(target=server.serve_forever,
                                      kwargs={"poll_interval": 0.1}, daemon=True, name=name)
            thread.start()
            self.threads.append(thread)
        return self

    def close(self):
        self.stopping.set()
        with self.store.condition:
            self.store.condition.notify_all()
        for server in (self.http, self.tcp):
            if self.threads:
                server.shutdown()
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)

    def __enter__(self):
        return self.start()

    def __exit__(self, *_args):
        self.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--ingest-port", type=int, default=8081)
    parser.add_argument("--public-url", help="Reachable HTTP origin, e.g. http://192.168.1.20:8080")
    parser.add_argument("--static-root", type=Path)
    parser.add_argument("--allow-uart-ingest", action="store_true",
                        help="Enable explicit UART JSON handoff on loopback /api/ingest")
    args = parser.parse_args()
    if not all(1 <= port <= 65535 for port in (args.http_port, args.ingest_port)):
        parser.error("Ports must be between 1 and 65535")
    try:
        with CommandCentre(host=args.host, http_port=args.http_port,
                           ingest_port=args.ingest_port, public_url=args.public_url,
                           static_root=args.static_root,
                           allow_uart_ingest=args.allow_uart_ingest) as app:
            def stop(_signum, _frame):
                app.stopping.set()
            signal.signal(signal.SIGTERM, stop)
            signal.signal(signal.SIGINT, stop)
            print(f"Command centre: {app.server_info['url']}/drone/{DRONE_ID}", flush=True)
            print(f"Board native WiFi TCP port: {app.tcp.server_address[1]}", flush=True)
            print("Waiting for real board telemetry; UART/Tera Term remain independent.", flush=True)
            app.stopping.wait()
    except (OSError, ValueError) as error:
        parser.exit(1, f"Command centre: {error}\n")


if __name__ == "__main__":
    main()
