#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager — Controlled HTTP Benchmark Server
# Serves deterministic datasets with RFC 7233 byte-range support,
# per-connection bandwidth throttling, connection instrumentation,
# non-range fallback endpoints, and strict localhost isolation.

import argparse
import hashlib
import http.server
import json
import logging
import os
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] [%(threadName)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
logger = logging.getLogger("BenchmarkServer")

DEFAULT_THROTTLE_BPS = 1048576  # 1 MiB/s per connection default for throttled endpoints


class ServerMetrics:
    """Thread-safe metrics collector for benchmark instrumentation."""

    def __init__(self):
        self._lock = threading.Lock()
        self.active_connections = 0
        self.peak_connections = 0
        self.active_handlers = 0
        self.peak_handlers = 0
        self.total_requests = 0
        self.total_bytes_served = 0
        self.request_failures = 0
        self.history = []

    def on_request_start(self):
        with self._lock:
            self.active_handlers += 1
            self.peak_handlers = max(self.peak_handlers, self.active_handlers)
            return self.active_handlers, self.peak_handlers

    def on_request_end(self, record: dict):
        with self._lock:
            self.active_handlers = max(0, self.active_handlers - 1)
            self.total_requests += 1
            self.total_bytes_served += int(record.get("server_bytes_offered", 0))
            if not record.get("body_complete", False):
                self.request_failures += 1
            self.history.append(record)

    def on_connection_start(self):
        with self._lock:
            self.active_connections += 1
            if self.active_connections > self.peak_connections:
                self.peak_connections = self.active_connections
            return self.active_connections, self.peak_connections

    def on_connection_end(self, bytes_sent: int, success: bool, duration_s: float, path: str, status_code: int):
        with self._lock:
            self.active_connections = max(0, self.active_connections - 1)

    def reset(self):
        with self._lock:
            self.active_connections = 0
            self.peak_connections = 0
            self.active_handlers = 0
            self.peak_handlers = 0
            self.total_requests = 0
            self.total_bytes_served = 0
            self.request_failures = 0
            self.history.clear()

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "active_connections": self.active_connections,
                "peak_connections": self.peak_connections,
                "active_handlers": self.active_handlers,
                "peak_handlers": self.peak_handlers,
                "total_requests": self.total_requests,
                "total_bytes_served": self.total_bytes_served,
                "request_failures": self.request_failures,
            }


class ThreadingBenchmarkServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        # Ignore client disconnects without cluttering logs
        exc_type, exc_val, _ = sys.exc_info()
        if exc_type in (ConnectionResetError, BrokenPipeError, socket.error):
            return
        super().handle_error(request, client_address)

    def __init__(self, server_address, RequestHandlerClass, data_dir: Path, throttle_bps: int):
        self.data_dir = data_dir
        self.throttle_bps = throttle_bps
        self.metrics = ServerMetrics()
        self.datasets = {}
        self.is_running = True
        super().__init__(server_address, RequestHandlerClass)


def generate_deterministic_dataset(file_path: Path, size_bytes: int) -> str:
    """Generates a deterministic file on disk and returns its SHA256 hex digest."""
    file_path.parent.mkdir(parents=True, exist_ok=True)
    if file_path.exists() and file_path.stat().st_size == size_bytes:
        # Verify existing file
        hasher = hashlib.sha256()
        with open(file_path, "rb") as f:
            while chunk := f.read(1048576):
                hasher.update(chunk)
        digest = hasher.hexdigest()
        logger.info("Found existing verified dataset %s (SHA256: %s)", file_path.name, digest)
        return digest

    logger.info("Generating deterministic dataset %s (%d bytes)...", file_path.name, size_bytes)
    t0 = time.perf_counter()
    hasher = hashlib.sha256()

    # Pre-generate 64 KiB base pattern
    pattern = bytearray(65536)
    for i in range(len(pattern)):
        pattern[i] = ((i * 37) ^ 0x5A) & 0xFF

    written = 0
    block_idx = 0
    with open(file_path, "wb") as f:
        while written < size_bytes:
            chunk_len = min(len(pattern), size_bytes - written)
            chunk = bytearray(pattern[:chunk_len])
            # Embed block index to avoid identical adjacent blocks
            chunk[0] = block_idx & 0xFF
            chunk[1] = (block_idx >> 8) & 0xFF
            chunk[2] = (block_idx >> 16) & 0xFF
            chunk[3] = (block_idx >> 24) & 0xFF
            f.write(chunk)
            hasher.update(chunk)
            written += chunk_len
            block_idx += 1

    digest = hasher.hexdigest()
    elapsed = time.perf_counter() - t0
    logger.info("Generated %s in %.2fs (SHA256: %s)", file_path.name, elapsed, digest)
    return digest


class BenchmarkRequestHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError, socket.error):
            pass

    def log_message(self, format, *args):
        # Route through standard Python logging at DEBUG level to avoid noisy stdout
        logger.debug("%s - - [%s] %s", self.client_address[0], self.log_date_time_string(), format % args)

    @property
    def server_instance(self) -> ThreadingBenchmarkServer:
        return self.server

    def do_HEAD(self):
        self._run_instrumented_request(send_body=False)

    def do_GET(self):
        self._run_instrumented_request(send_body=True)

    def _run_instrumented_request(self, send_body: bool):
        start_tick = time.perf_counter_ns()
        start_epoch_ns = time.time_ns()
        self._bench_status = 0
        self._bench_bytes_offered = 0
        self._bench_response_range = None
        self._bench_body_complete = False
        self._bench_send_error = ""
        self._bench_headers_tick_ns = None
        self._bench_body_tick_ns = None
        active_handlers, peak_handlers = self.server_instance.metrics.on_request_start()
        active_payload = self.server_instance.metrics.snapshot()["active_connections"]
        try:
            self._handle_request(send_body=send_body)
        except (ConnectionResetError, BrokenPipeError, socket.error) as exc:
            self._bench_send_error = type(exc).__name__
        finally:
            end_tick = time.perf_counter_ns()
            end_epoch_ns = time.time_ns()
            active_payload_end = self.server_instance.metrics.snapshot()["active_connections"]
            self.server_instance.metrics.on_request_end({
                "method": self.command,
                "path": self.path,
                "requested_range": self.headers.get("Range", ""),
                "response_range": self._bench_response_range,
                "status": self._bench_status,
                "server_bytes_offered": self._bench_bytes_offered,
                "body_complete": self._bench_body_complete,
                "send_error": self._bench_send_error,
                "start_epoch_ns": start_epoch_ns,
                "end_epoch_ns": end_epoch_ns,
                "start_tick_ns": start_tick,
                "end_tick_ns": end_tick,
                "duration_us": (end_tick - start_tick) // 1000,
                "headers_tick_ns": self._bench_headers_tick_ns,
                "body_tick_ns": self._bench_body_tick_ns,
                "response_headers_latency_us": ((self._bench_headers_tick_ns - start_tick) // 1000
                                                if self._bench_headers_tick_ns is not None else None),
                "post_header_body_wait_us": ((self._bench_body_tick_ns - self._bench_headers_tick_ns) // 1000
                                              if self._bench_body_tick_ns is not None and self._bench_headers_tick_ns is not None else None),
                "body_transfer_handler_us": ((end_tick - self._bench_body_tick_ns) // 1000
                                             if self._bench_body_tick_ns is not None else None),
                "active_handlers_at_start": active_handlers,
                "peak_handlers_at_start": peak_handlers,
                "active_payload_handlers_at_start": active_payload,
                "active_payload_handlers_at_end": active_payload_end,
            })

    def do_POST(self):
        path = self.path.split("?")[0]
        if path == "/reset-metrics":
            self.server_instance.metrics.reset()
            self._send_json_response(200, {"status": "ok", "message": "Metrics reset successfully"})
            return
        if path == "/shutdown":
            self._send_json_response(200, {"status": "ok", "message": "Server shutting down"})
            threading.Thread(target=self.server_instance.shutdown).start()
            return
        self.send_error(404, "Not Found")

    def _send_json_response(self, code: int, data: dict):
        body = json.dumps(data, indent=2).encode("utf-8")
        self._bench_status = code
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self._bench_headers_tick_ns = time.perf_counter_ns()
        self.wfile.write(body)
        self._bench_bytes_offered += len(body)
        self._bench_body_complete = True

    def send_error(self, code, message=None, explain=None):
        self._bench_status = code
        self._bench_send_error = message or "HTTP error"
        return super().send_error(code, message, explain)

    def _handle_request(self, send_body: bool):
        path = self.path.split("?")[0]

        if path in ("/health", "/ping"):
            self._send_json_response(200, {
                "status": "healthy",
                "active_connections": self.server_instance.metrics.active_connections,
                "peak_connections": self.server_instance.metrics.peak_connections,
            })
            return

        if path == "/metrics":
            snap = self.server_instance.metrics.snapshot()
            snap["datasets"] = self.server_instance.datasets
            snap["throttle_bps"] = self.server_instance.throttle_bps
            self._send_json_response(200, snap)
            return

        if path == "/reset-metrics":
            self.server_instance.metrics.reset()
            self._send_json_response(200, {"status": "ok", "message": "Metrics reset successfully"})
            return

        # Determine route type:
        # /files/<filename>     -> Normal range-capable
        # /throttled/<filename> -> Range-capable with per-connection bandwidth throttling
        # /no-range/<filename>  -> Single-stream fallback (Accept-Ranges: none, ignores Range header)
        is_throttled = False
        is_range_capable = True

        if path.startswith("/files/"):
            filename = path[len("/files/"):]
        elif path.startswith("/throttled/"):
            filename = path[len("/throttled/"):]
            is_throttled = True
        elif path.startswith("/no-range/"):
            filename = path[len("/no-range/"):]
            is_range_capable = False
        else:
            self.send_error(404, "Endpoint Not Found")
            return

        target_file = self.server_instance.data_dir / filename
        if not target_file.exists() or not target_file.is_file():
            self.send_error(404, f"Dataset file not found: {filename}")
            return

        file_size = target_file.stat().st_size
        range_header = self.headers.get("Range", "")

        latency_ms = int(getattr(self.server_instance, "response_latency_ms", 0))
        if latency_ms > 0:
            time.sleep(latency_ms / 1000.0)

        limit = int(getattr(self.server_instance, "max_payload_handlers", 0))
        if limit > 0 and self.server_instance.metrics.snapshot()["active_connections"] >= limit:
            self._bench_status = 429
            self.send_response(429)
            self.send_header("Retry-After", "0")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        # Non-range endpoint behavior: Always return 200 OK with full file
        if not is_range_capable or not range_header:
            self._serve_full_content(target_file, file_size, is_range_capable, is_throttled, send_body)
            return

        # Range-capable endpoint with Range header
        self._serve_range_content(target_file, file_size, range_header, is_throttled, send_body)

    def _serve_full_content(self, target_file: Path, file_size: int, is_range_capable: bool, is_throttled: bool, send_body: bool):
        t0 = time.perf_counter()
        active, peak = self.server_instance.metrics.on_connection_start()
        logger.debug("Active connection +1 (now %d, peak %d) for full %s", active, peak, target_file.name)

        bytes_sent = 0
        success = False
        try:
            self.send_response(200)
            self._bench_status = 200
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(file_size))
            if is_range_capable:
                self.send_header("Accept-Ranges", "bytes")
            else:
                self.send_header("Accept-Ranges", "none")
            self.send_header("ETag", f'"{target_file.name}-{file_size}"')
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            self._bench_headers_tick_ns = time.perf_counter_ns()

            if send_body:
                self._bench_body_tick_ns = time.perf_counter_ns()
                first_body_delay_ms = int(getattr(self.server_instance, "initial_body_delay_ms", 0))
                if first_body_delay_ms > 0:
                    time.sleep(first_body_delay_ms / 1000.0)
                bytes_sent = self._stream_file_bytes(target_file, 0, file_size - 1, is_throttled)
                self._bench_body_complete = bytes_sent == file_size
            else:
                self._bench_body_complete = True
                self._bench_body_tick_ns = None
            self._bench_response_range = [0, file_size - 1]
            success = True
        except (ConnectionResetError, BrokenPipeError, socket.error):
            logger.debug("Client reset connection during full stream of %s", target_file.name)
        finally:
            dur = time.perf_counter() - t0
            self.server_instance.metrics.on_connection_end(bytes_sent, success, dur, self.path, 200)

    def _serve_range_content(self, target_file: Path, file_size: int, range_header: str, is_throttled: bool, send_body: bool):
        t0 = time.perf_counter()

        # Parse byte range (e.g. "bytes=0-1048575", "bytes=1048576-", "bytes=-524288")
        if not range_header.startswith("bytes="):
            self.send_error(400, "Bad Request: Invalid Range specifier")
            return

        range_spec = range_header[len("bytes="):].strip()
        parts = range_spec.split("-")
        if len(parts) != 2:
            self.send_error(400, "Bad Request: Multipart ranges not supported")
            return

        start_str, end_str = parts[0].strip(), parts[1].strip()
        try:
            if not start_str and end_str:
                # Suffix byte range: bytes=-500 (last 500 bytes)
                length = int(end_str)
                start = max(0, file_size - length)
                end = file_size - 1
            elif start_str and not end_str:
                # Open-ended: bytes=500- (from 500 to end)
                start = int(start_str)
                end = file_size - 1
            else:
                start = int(start_str)
                end = int(end_str)
        except ValueError:
            self.send_error(400, "Bad Request: Non-integer byte range")
            return

        if start > end or start >= file_size:
            # 416 Range Not Satisfiable
            self.send_response(416)
            self._bench_status = 416
            self.send_header("Content-Range", f"bytes */{file_size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        end = min(end, file_size - 1)
        content_length = end - start + 1
        self._bench_response_range = [start, end]

        active, peak = self.server_instance.metrics.on_connection_start()
        logger.debug("Active connection +1 (now %d, peak %d) range %d-%d for %s",
                     active, peak, start, end, target_file.name)

        bytes_sent = 0
        success = False
        try:
            self.send_response(206)
            self._bench_status = 206
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(content_length))
            self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", f'"{target_file.name}-{file_size}"')
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            self._bench_headers_tick_ns = time.perf_counter_ns()

            if send_body:
                self._bench_body_tick_ns = time.perf_counter_ns()
                first_body_delay_ms = int(getattr(self.server_instance, "initial_body_delay_ms", 0))
                if first_body_delay_ms > 0:
                    time.sleep(first_body_delay_ms / 1000.0)
                bytes_sent = self._stream_file_bytes(target_file, start, end, is_throttled)
            self._bench_body_complete = (not send_body) or bytes_sent == content_length
            success = True
        except (ConnectionResetError, BrokenPipeError, socket.error):
            logger.debug("Client reset connection during range [%d-%d] of %s", start, end, target_file.name)
        finally:
            dur = time.perf_counter() - t0
            self.server_instance.metrics.on_connection_end(bytes_sent, success, dur, self.path, 206)

    def _stream_file_bytes(self, target_file: Path, start: int, end: int, is_throttled: bool) -> int:
        chunk_size = int(getattr(self.server_instance, "response_chunk_bytes", 262144))
        bytes_to_send = end - start + 1
        total_sent = 0
        throttle_rate = self.server_instance.throttle_bps if is_throttled else 0

        with open(target_file, "rb") as f:
            f.seek(start)
            while total_sent < bytes_to_send:
                chunk_to_read = min(chunk_size, bytes_to_send - total_sent)
                chunk_t0 = time.perf_counter()
                data = f.read(chunk_to_read)
                if not data:
                    break
                self.wfile.write(data)
                total_sent += len(data)
                self._bench_bytes_offered += len(data)

                # Bandwidth throttling
                if throttle_rate > 0:
                    expected_time = len(data) / float(throttle_rate)
                    actual_time = time.perf_counter() - chunk_t0
                    sleep_time = expected_time - actual_time
                    if sleep_time > 0.001:
                        time.sleep(sleep_time)

                chunk_delay_ms = int(getattr(self.server_instance, "response_chunk_delay_ms", 0))
                if chunk_delay_ms > 0:
                    time.sleep(chunk_delay_ms / 1000.0)

        return total_sent


def run_benchmark_server(host: str = "127.0.0.1", port: int = 0, data_dir: Path = None,
                         throttle_bps: int = DEFAULT_THROTTLE_BPS, port_file: Path = None):
    if data_dir is None:
        data_dir = Path(__file__).resolve().parent.parent / "tools" / "benchmarks" / "data"
    data_dir.mkdir(parents=True, exist_ok=True)

    # Generate benchmark datasets
    datasets = {
        "bench_16mb.bin": {
            "size": 16 * 1048576,
            "sha256": generate_deterministic_dataset(data_dir / "bench_16mb.bin", 16 * 1048576)
        },
        "bench_64mb.bin": {
            "size": 64 * 1048576,
            "sha256": generate_deterministic_dataset(data_dir / "bench_64mb.bin", 64 * 1048576)
        },
        "bench_256mb.bin": {
            "size": 256 * 1048576,
            "sha256": generate_deterministic_dataset(data_dir / "bench_256mb.bin", 256 * 1048576)
        },
    }

    server = ThreadingBenchmarkServer((host, port), BenchmarkRequestHandler, data_dir, throttle_bps)
    server.datasets = datasets
    actual_port = server.server_address[1]
    logger.info("Benchmark Server running on http://%s:%d (Bound strictly to localhost)", host, actual_port)

    if port_file:
        port_file = Path(port_file)
        port_file.parent.mkdir(parents=True, exist_ok=True)
        port_file.write_text(str(actual_port), encoding="utf-8")
        logger.info("Wrote allocated port to %s", port_file)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("Server shutting down via KeyboardInterrupt")
    finally:
        server.server_close()
        logger.info("Server terminated.")


def main():
    parser = argparse.ArgumentParser(description="Radium Controlled HTTP Benchmark Server")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Host address (default: 127.0.0.1 loopback only)")
    parser.add_argument("--port", type=int, default=0, help="Port to listen on (0 for dynamic)")
    parser.add_argument("--data-dir", type=str, default=None, help="Directory for deterministic datasets")
    parser.add_argument("--throttle-rate", type=int, default=DEFAULT_THROTTLE_BPS, help="Per-connection throttle rate in bytes/sec (default: 1048576 = 1 MB/s)")
    parser.add_argument("--port-file", type=str, default=None, help="File path to write allocated port number")
    args = parser.parse_args()

    # Enforce loopback security rule
    if args.host not in ("127.0.0.1", "localhost", "::1"):
        raise PermissionError(f"Security policy violation: Server must bind only to localhost, got {args.host}")

    run_benchmark_server(
        host=args.host,
        port=args.port,
        data_dir=Path(args.data_dir).resolve() if args.data_dir else None,
        throttle_bps=args.throttle_rate,
        port_file=Path(args.port_file).resolve() if args.port_file else None,
    )


if __name__ == "__main__":
    main()
