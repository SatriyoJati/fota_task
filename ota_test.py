#!/usr/bin/env python3
"""
ota_chaos_server.py
====================

Standalone HTTP/HTTPS server for testing ESP32 (esp_https_ota / advanced_https_ota
style) OTA clients against real-world network misbehavior: dropped connections,
stalls, throttling, corrupted bytes, and servers that don't support Range
requests.

No pytest, no DUT, no serial console needed. Point it at a directory, put your
.bin in there, give the ESP32 the printed URL, and pull the network out from
under it however you like.

Quick start
-----------
    # plain HTTP, serve everything in ./firmware as-is, no chaos
    python3 ota_chaos_server.py --dir ./firmware

    # HTTPS (self-signed cert auto-generated on first run) + random mid-download drops
    python3 ota_chaos_server.py --dir ./firmware --https --chaos drop

    # stall for 8s partway through every download (simulate a frozen AP)
    python3 ota_chaos_server.py --dir ./firmware --https --chaos stall --stall-seconds 8

    # throttle to 20 KB/s so you have time to unplug Wi-Fi / power-cycle by hand
    python3 ota_chaos_server.py --dir ./firmware --https --throttle-kbps 20

    # server that refuses Range requests, to test your fallback-to-full-download path
    python3 ota_chaos_server.py --dir ./firmware --https --no-range

    # only break every 3rd request, everything else serves normally
    python3 ota_chaos_server.py --dir ./firmware --https --chaos drop --every 3

    # flip the coin each time: could be a clean drop, a stall, or bit corruption
    python3 ota_chaos_server.py --dir ./firmware --https --chaos flaky --probability 0.5

Swapping the firmware
----------------------
The file is read fresh from disk on every single request (nothing is cached
in memory), so to test a new build you can just overwrite the .bin in --dir
in place -- no server restart needed. If you want to point at a totally
different directory, that's when you re-run the script with a new --dir.

Then on the ESP32 side, just point advanced_https_ota / esp_https_ota at:
    https://<this-machine-ip>:<port>/<your-file>.bin
(or http:// if you didn't pass --https)
"""

from __future__ import annotations

import argparse
import http.server
import os
import random
import re
import socket
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path

# --------------------------------------------------------------------------
# Chaos decision logic
# --------------------------------------------------------------------------

CHAOS_MODES = ["none", "drop", "stall", "throttle", "corrupt", "truncate-headers", "flaky"]
_FLAKY_POOL = ["drop", "stall", "corrupt"]


def resolve_chaos(args: argparse.Namespace, request_id: int) -> str:
    """Decide what, if anything, should go wrong with this particular request."""
    if args.chaos == "none":
        return "none"
    if args.every and request_id % args.every != 0:
        return "none"
    if random.random() > args.probability:
        return "none"
    if args.chaos == "flaky":
        return random.choice(_FLAKY_POOL)
    return args.chaos


# --------------------------------------------------------------------------
# Self-signed cert generation (only used if --https and no --cert/--key given)
# --------------------------------------------------------------------------

def ensure_self_signed_cert(cert_dir: Path) -> tuple[Path, Path]:
    cert_dir.mkdir(parents=True, exist_ok=True)
    cert_path = cert_dir / "ota_chaos_cert.pem"
    key_path = cert_dir / "ota_chaos_key.pem"
    if cert_path.exists() and key_path.exists():
        return cert_path, key_path

    openssl = shutil_which("openssl")
    if not openssl:
        sys.exit(
            "No --cert/--key given and 'openssl' was not found on PATH to "
            "auto-generate a self-signed certificate. Install openssl or "
            "pass --cert/--key explicitly."
        )

    print(f"[*] Generating self-signed cert at {cert_path} (first run only)...")
    subprocess.run(
        [
            openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key_path), "-out", str(cert_path),
            "-days", "3650", "-subj", "/CN=ota-chaos-server",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return cert_path, key_path


def shutil_which(cmd: str) -> str | None:
    import shutil
    return shutil.which(cmd)


# --------------------------------------------------------------------------
# Handler factory
# --------------------------------------------------------------------------

def make_handler(directory: Path, args: argparse.Namespace, ssl_context: ssl.SSLContext | None):
    counter_lock = threading.Lock()
    counter = {"n": 0}

    class ChaosHandler(http.server.BaseHTTPRequestHandler):
        server_version = "OTAChaosServer/1.0"
        protocol_version = "HTTP/1.1"

        # --- TLS handshake done per-connection so the accept loop never blocks,
        # same trick esp-idf's own test server uses (matters when the device
        # hard-resets mid-connection). ---
        def setup(self) -> None:
            if ssl_context is not None:
                sock: socket.socket = self.request  # type: ignore[assignment]
                sock.settimeout(10)
                self.request = ssl_context.wrap_socket(sock, server_side=True)
                self.request.settimeout(None)
            super().setup()

        def finish(self) -> None:
            try:
                if not self.wfile.closed:
                    self.wfile.flush()
                    self.wfile.close()
            except OSError:
                pass
            try:
                self.rfile.close()
            except OSError:
                pass

        def handle(self) -> None:
            try:
                super().handle()
            except (OSError, ssl.SSLError):
                pass

        def log_message(self, fmt: str, *a) -> None:
            sys.stderr.write(f"[{self.log_date_time_string()}] {fmt % a}\n")

        # --- path safety ---
        def _resolve_path(self) -> Path | None:
            raw = self.path.split("?", 1)[0].split("#", 1)[0]
            raw = raw.lstrip("/")
            if not raw:
                raw = args.default_file or ""
            if not raw:
                return None
            candidate = (directory / raw).resolve()
            try:
                candidate.relative_to(directory.resolve())
            except ValueError:
                return None
            return candidate

        def do_HEAD(self) -> None:
            self._serve(send_body=False)

        def do_GET(self) -> None:
            self._serve(send_body=True)

        def _serve(self, send_body: bool) -> None:
            with counter_lock:
                counter["n"] += 1
                req_id = counter["n"]

            filepath = self._resolve_path()
            if filepath is None or not filepath.is_file():
                self.send_error(404, "File not found")
                return

            file_size = filepath.stat().st_size
            range_header = self.headers.get("Range")
            chaos = resolve_chaos(args, req_id)

            start, end, partial = 0, file_size - 1, False
            if range_header and not args.no_range:
                m = re.match(r"bytes=(\d+)-(\d*)", range_header)
                if m:
                    start = int(m.group(1))
                    end = int(m.group(2)) if m.group(2) else file_size - 1
                    end = min(end, file_size - 1)
                    partial = True
            elif range_header and args.no_range:
                # Simulate a server that doesn't understand Range at all.
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(file_size))
                self.end_headers()
                if send_body:
                    with open(filepath, "rb") as f:
                        self._stream(f, 0, file_size, "none")
                self.log_message(
                    "req#%d %s %s -> 200 (ignored Range, no-range mode) chaos=%s",
                    req_id, self.command, self.path, chaos,
                )
                return

            length = end - start + 1

            self.log_message(
                "req#%d %s %s range=%s chaos=%s size=%d",
                req_id, self.command, self.path, range_header, chaos, length,
            )

            if chaos == "truncate-headers":
                # Send a plausible-looking but incomplete header block, then vanish.
                try:
                    self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: ")
                    self.wfile.flush()
                except OSError:
                    pass
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                return

            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "none" if args.no_range else "bytes")
            self.send_header("Content-Length", str(length))
            if partial:
                self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
            self.end_headers()

            if not send_body:
                return

            with open(filepath, "rb") as f:
                self._stream(f, start, length, chaos)

        def _stream(self, f, start: int, length: int, chaos: str) -> None:
            f.seek(start)
            remaining = length
            sent = 0
            chunk_size = 4096

            drop_at = None
            if chaos == "drop":
                lo, hi = args.drop_min_percent / 100, args.drop_max_percent / 100
                drop_at = int(length * random.uniform(lo, hi))

            stalled_already = False

            while remaining > 0:
                data = f.read(min(chunk_size, remaining))
                if not data:
                    break

                if chaos == "corrupt" and random.random() < args.corrupt_chance:
                    data = bytearray(data)
                    for _ in range(min(args.corrupt_bytes, len(data))):
                        idx = random.randrange(len(data))
                        data[idx] = random.randrange(256)
                    data = bytes(data)

                try:
                    self.wfile.write(data)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, OSError):
                    self.log_message("connection died while sending (client gone)")
                    return

                sent += len(data)
                remaining -= len(data)

                if args.throttle_kbps:
                    time.sleep(len(data) / (args.throttle_kbps * 1024))

                if chaos == "stall" and not stalled_already and sent >= length * args.stall_at_percent / 100:
                    stalled_already = True
                    self.log_message(
                        "CHAOS: stalling %.1fs at %d/%d bytes", args.stall_seconds, sent, length
                    )
                    time.sleep(args.stall_seconds)

                if chaos == "drop" and drop_at is not None and sent >= drop_at:
                    self.log_message("CHAOS: dropping connection at %d/%d bytes", sent, length)
                    try:
                        self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    return

    return ChaosHandler


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def get_lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Chaos HTTP(S) server for exercising ESP32 OTA robustness.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("--dir", required=True, type=Path, help="Directory to serve (put your .bin here)")
    p.add_argument("--host", default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    p.add_argument("--port", type=int, default=8001, help="Port (default: 8001)")
    p.add_argument("--default-file", default=None, help="File to serve when request path is '/' (optional)")

    p.add_argument("--https", action="store_true", help="Serve over TLS")
    p.add_argument("--cert", type=Path, default=None, help="Path to cert PEM (auto-generated if omitted)")
    p.add_argument("--key", type=Path, default=None, help="Path to key PEM (auto-generated if omitted)")

    p.add_argument("--no-range", action="store_true",
                    help="Pretend Range requests aren't supported (200 full body every time, "
                         "tests OTA resumption fallback-to-full-download logic)")

    p.add_argument("--chaos", choices=CHAOS_MODES, default="none",
                    help="What kind of misbehavior to inject (default: none)")
    p.add_argument("--probability", type=float, default=1.0,
                    help="Chance [0-1] that a qualifying request gets the chaos treatment (default: 1.0)")
    p.add_argument("--every", type=int, default=0,
                    help="Only apply chaos to every Nth request (0 = every qualifying request, default: 0)")

    p.add_argument("--drop-min-percent", type=float, default=15,
                    help="For --chaos drop: min %% of body sent before dropping (default: 15)")
    p.add_argument("--drop-max-percent", type=float, default=85,
                    help="For --chaos drop: max %% of body sent before dropping (default: 85)")

    p.add_argument("--stall-seconds", type=float, default=10,
                    help="For --chaos stall: how long to pause mid-transfer (default: 10)")
    p.add_argument("--stall-at-percent", type=float, default=40,
                    help="For --chaos stall: %% of body sent before pausing (default: 40)")

    p.add_argument("--throttle-kbps", type=float, default=0,
                    help="Cap transfer speed in KB/s, 0 = unlimited (default: 0). "
                         "Useful combined with any --chaos mode to give yourself time to "
                         "physically interfere (unplug power, kill Wi-Fi, etc).")

    p.add_argument("--corrupt-chance", type=float, default=0.02,
                    help="For --chaos corrupt: chance per chunk that bytes get flipped (default: 0.02)")
    p.add_argument("--corrupt-bytes", type=int, default=4,
                    help="For --chaos corrupt: bytes flipped per corrupted chunk (default: 4)")

    return p


def main() -> None:
    args = build_arg_parser().parse_args()
    directory = args.dir.resolve()
    if not directory.is_dir():
        sys.exit(f"--dir {directory} does not exist or is not a directory")

    ssl_context = None
    scheme = "http"
    if args.https:
        scheme = "https"
        if args.cert and args.key:
            cert_path, key_path = args.cert, args.key
        else:
            cert_path, key_path = ensure_self_signed_cert(Path(__file__).resolve().parent / "certs")
        ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_context.load_cert_chain(certfile=str(cert_path), keyfile=str(key_path))

    handler_cls = make_handler(directory, args, ssl_context)
    httpd = http.server.ThreadingHTTPServer((args.host, args.port), handler_cls)
    httpd.daemon_threads = True

    lan_ip = get_lan_ip()
    print(f"[*] Serving {directory}")
    print(f"[*] Listening on {args.host}:{args.port}  (scheme: {scheme})")
    print(f"[*] Point your ESP32 at, e.g.: {scheme}://{lan_ip}:{args.port}/<your-file>.bin")
    print(f"[*] Chaos mode: {args.chaos}"
          + (f" (probability={args.probability}, every={args.every or 'all'})" if args.chaos != "none" else ""))
    if args.no_range:
        print("[*] Range requests are DISABLED (testing fallback-to-full-download path)")
    if args.throttle_kbps:
        print(f"[*] Throttled to {args.throttle_kbps} KB/s")
    print("[*] Overwrite the .bin in the served directory any time -- no restart needed.")
    print("[*] Ctrl+C to stop.\n")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[*] Shutting down.")
        httpd.shutdown()


if __name__ == "__main__":
    main()