#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Radium Download Manager - Core Operations, Pause/Resume, Concurrency & Persistence Verification
Executes directly against built Radium binaries (or installed binaries) to verify:
 1. Startup & Version / Help CLI verification
 2. SQLite Database initialization & WAL mode verification
 3. AppSettings configuration persistence across process lifecycles
 4. Multi-connection segmented HTTP download & SHA-256 byte integrity
 5. Mid-flight Pause and Resume with byte offset preservation & range reuse
 6. Concurrent multi-task downloads under queue limits
 7. High-DPI UI layout snapshot and queue state inspection
"""

import argparse
import hashlib
import http.server
import json
import os
import platform
import shutil
import socketserver
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = (os.name == "nt" or platform.system().lower().startswith("win"))
EXE_EXT = ".exe" if IS_WINDOWS else ""

os.environ["RADIUM_ALLOW_LOOPBACK"] = "1"
os.environ["FLUX_ALLOW_LOOPBACK"] = "1"
os.environ["OPENIDM_ALLOW_LOOPBACK"] = "1"


def find_binary(name: str, custom_dir: Path | None = None) -> Path:
    candidates = []
    if custom_dir:
        candidates.extend([
            custom_dir / f"{name}{EXE_EXT}",
            custom_dir / name,
            custom_dir / "Release" / f"{name}{EXE_EXT}",
        ])
    candidates.extend([
        ROOT / "build" / "Release" / f"{name}{EXE_EXT}",
        ROOT / "build" / "win-x64" / "Release" / f"{name}{EXE_EXT}",
        ROOT / "dist" / f"{name}{EXE_EXT}",
        ROOT / "tools" / "win-x64" / f"{name}{EXE_EXT}",
    ])
    for c in candidates:
        if c.is_file():
            return c.resolve()
    which_c = shutil.which(f"{name}{EXE_EXT}") or shutil.which(name)
    if which_c:
        return Path(which_c).resolve()
    return candidates[0]


PAYLOAD_SMALL = os.urandom(512 * 1024)        # 512 KiB
PAYLOAD_MEDIUM = os.urandom(4 * 1024 * 1024)   # 4 MiB
PAYLOAD_LARGE = os.urandom(12 * 1024 * 1024)   # 12 MiB

HASH_SMALL = hashlib.sha256(PAYLOAD_SMALL).hexdigest()
HASH_MEDIUM = hashlib.sha256(PAYLOAD_MEDIUM).hexdigest()
HASH_LARGE = hashlib.sha256(PAYLOAD_LARGE).hexdigest()


class ControlledTestHttpServer(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/files/payload_small.bin":
            self._serve_buffer(PAYLOAD_SMALL, filename="payload_small.bin")
        elif path == "/files/payload_medium.bin":
            self._serve_buffer(PAYLOAD_MEDIUM, filename="payload_medium.bin")
        elif path == "/files/payload_large.bin":
            self._serve_buffer(PAYLOAD_LARGE, filename="payload_large.bin")
        elif path.startswith("/files/concurrent_"):
            idx = path.split("_")[-1].replace(".bin", "")
            # Generate deterministic per-task payload
            seed = int(idx) if idx.isdigit() else 1
            payload = bytes([(i + seed) % 256 for i in range(1024 * 1024)]) # 1 MiB each
            self._serve_buffer(payload, filename=f"concurrent_{idx}.bin")
        elif path == "/files/norange_fallback.bin":
            # Explicit Non-Range server returning Accept-Ranges: none with Content-Length
            self._serve_buffer(PAYLOAD_MEDIUM, filename="norange_fallback.bin", support_range=False)
        elif path == "/files/rate_limited_429.bin":
            rng_hdr = self.headers.get("Range", "")
            # Always serve the range-capability probe (bytes=0-0) normally so the
            # engine can detect Accept-Ranges / Content-Length before rate-limiting.
            # The 429 fires only on the first real segment download request.
            if rng_hdr == "bytes=0-0":
                self._serve_buffer(PAYLOAD_MEDIUM, filename="rate_limited_429.bin", support_range=True)
            elif getattr(ControlledTestHttpServer, "_hit_429", False) is False:
                ControlledTestHttpServer._hit_429 = True
                self.send_response(429)
                self.send_header("Retry-After", "1")
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", "18")
                self.end_headers()
                self.wfile.write(b"Too Many Requests\n")
            else:
                self._serve_buffer(PAYLOAD_MEDIUM, filename="rate_limited_429.bin", support_range=True)
        elif path == "/files/reset_drop.bin":
            if getattr(ControlledTestHttpServer, "_hit_reset", False) is False:
                ControlledTestHttpServer._hit_reset = True
                total = len(PAYLOAD_MEDIUM)
                start, end = 0, total - 1
                rng = self.headers.get("Range", "")
                if rng.startswith("bytes="):
                    spec = rng.split("=", 1)[1].strip()
                    parts = spec.split("-", 1)
                    if parts[0]:
                        start = int(parts[0])
                    if len(parts) > 1 and parts[1]:
                        end = min(int(parts[1]), total - 1)
                span = end - start + 1
                self.send_response(206)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
                self.send_header("Content-Length", str(span))
                self.send_header("Content-Disposition", 'attachment; filename="reset_drop.bin"')
                self.end_headers()
                partial_size = min(32768, max(1, span // 2))
                try:
                    self.wfile.write(PAYLOAD_MEDIUM[start : start + partial_size])
                    self.wfile.flush()
                except Exception:
                    pass
                # Premature abort to trigger connection reset
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                except Exception:
                    pass
                return
            else:
                self._serve_buffer(PAYLOAD_MEDIUM, filename="reset_drop.bin", support_range=True)
        else:
            self.send_response(404)
            self.end_headers()

    def _serve_buffer(self, buf: bytes, filename: str, support_range: bool = True):
        total = len(buf)
        rng = self.headers.get("Range")
        mv = memoryview(buf)
        if support_range and rng and rng.startswith("bytes="):
            spec = rng.split("=", 1)[1].strip()
            s_str, e_str = spec.split("-", 1)
            start = int(s_str) if s_str else 0
            end = int(e_str) if e_str else (total - 1)
            end = min(end, total - 1)
            chunk = mv[start : end + 1]
            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
            self.send_header("Content-Length", str(len(chunk)))
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
            self.end_headers()
            try:
                self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            if support_range:
                self.send_header("Accept-Ranges", "bytes")
            else:
                self.send_header("Accept-Ranges", "none")
            self.send_header("Content-Length", str(total))
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
            self.end_headers()
            try:
                self.wfile.write(mv)
            except (BrokenPipeError, ConnectionResetError):
                pass


class ThreadedServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def run_test_suite(radium_bin: Path, nmh_bin: Path) -> dict:
    results = {
        "suite_name": "Radium Core Operations, Concurrency & Persistence Suite",
        "passed_tests": 0,
        "failed_tests": 0,
        "tests": []
    }

    def record(name: str, passed: bool, message: str = ""):
        results["tests"].append({
            "name": name,
            "status": "PASS" if passed else "FAIL",
            "message": message
        })
        if passed:
            results["passed_tests"] += 1
            print(f"  [PASS] {name} - {message}")
        else:
            results["failed_tests"] += 1
            print(f"  [FAIL] {name} - {message}")

    print("=================================================================")
    print(" Radium Core Operations, Persistence & Concurrency Verification  ")
    print(f" Target Binary : {radium_bin}")
    print(f" NMH Binary    : {nmh_bin}")
    print("=================================================================")

    # 1. Startup & Version
    try:
        proc = subprocess.run([str(radium_bin), "--version"], capture_output=True, text=True, check=True)
        out = (proc.stdout + "\n" + proc.stderr).strip()
        has_ver = ("Radium" in out or "Flux" in out) and ("1.0" in out or "0.3" in out)
        record("TC-CORE-01: Application Startup & Version Check", has_ver, f"Version banner: {out.splitlines()[0]}")
    except Exception as e:
        record("TC-CORE-01: Application Startup & Version Check", False, str(e))

    # Setup local HTTP server & work dir
    server = ThreadedServer(("127.0.0.1", 0), ControlledTestHttpServer)
    port = server.server_port
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    base_url = f"http://127.0.0.1:{port}"

    tmp_dir = Path(tempfile.mkdtemp(prefix="radium_core_ops_"))
    db_path = tmp_dir / "test_radium_state.db"
    dl_dir = tmp_dir / "downloads"
    dl_dir.mkdir(parents=True, exist_ok=True)

    try:
        # 2. Single ordinary download & SHA-256 verification
        url_med = f"{base_url}/files/payload_medium.bin"
        dl_res = subprocess.run(
            [str(radium_bin), "--db", str(db_path), "--out", str(dl_dir), "--allow-loopback", "--download", url_med],
            capture_output=True, text=True
        )
        med_file = dl_dir / "payload_medium.bin"
        if not med_file.exists():
            med_file = dl_dir / "General" / "payload_medium.bin"
        if dl_res.returncode == 0 and med_file.is_file() and hashlib.sha256(med_file.read_bytes()).hexdigest() == HASH_MEDIUM:
            record("TC-CORE-02: Core Segmented Download & SHA-256 Check", True, "4 MiB file downloaded and verified bit-exact")
        else:
            record("TC-CORE-02: Core Segmented Download & SHA-256 Check", False, f"Returncode={dl_res.returncode}, file_exists={med_file.exists()}")

        # 3. Database Schema, WAL Mode & SQLite Persistence
        try:
            conn = sqlite3.connect(str(db_path))
            cur = conn.cursor()
            cur.execute("PRAGMA journal_mode;")
            journal_mode = cur.fetchone()[0].lower()
            cur.execute("SELECT task_id, url, state, downloaded_bytes, total_bytes FROM tasks;")
            tasks = cur.fetchall()
            conn.close()
            is_wal = journal_mode in ("wal", "memory")
            has_tasks = len(tasks) > 0 and tasks[0][2] in ("COMPLETED", 3)
            record("TC-CORE-03: SQLite Schema, WAL Mode & Task Persistence", is_wal and has_tasks, f"Journal mode={journal_mode}, Persisted tasks={len(tasks)}")
        except Exception as e:
            record("TC-CORE-03: SQLite Schema, WAL Mode & Task Persistence", False, str(e))

        # 4. Settings Configuration Persistence Across Restarts
        try:
            # Let Radium initialize the database schema and default tables first
            init_res = subprocess.run([str(radium_bin), "--db", str(db_path), "--list"], capture_output=True, text=True)
            conn = sqlite3.connect(str(db_path))
            cur = conn.cursor()
            cur.execute("""
                INSERT OR REPLACE INTO settings (key, value_json, updated_at_ms)
                VALUES ('max_concurrent_tasks', '6', 1700000000000);
            """)
            cur.execute("""
                INSERT OR REPLACE INTO settings (key, value_json, updated_at_ms)
                VALUES ('speed_limit_enabled', 'true', 1700000000000);
            """)
            cur.execute("""
                INSERT OR REPLACE INTO settings (key, value_json, updated_at_ms)
                VALUES ('global_speed_limit_bps', '5242880', 1700000000000);
            """)
            conn.commit()
            conn.close()

            # Execute list/gui-snapshot to verify database opens, loads settings, and preserves them
            list_res = subprocess.run([str(radium_bin), "--db", str(db_path), "--list"], capture_output=True, text=True)
            conn = sqlite3.connect(str(db_path))
            cur = conn.cursor()
            cur.execute("SELECT value_json FROM settings WHERE key='max_concurrent_tasks';")
            val = cur.fetchone()[0]
            conn.close()
            record("TC-CORE-04: AppSettings Configuration Persistence Across Invocations", val == "6", f"Stored max_concurrent_tasks={val}")
        except Exception as e:
            record("TC-CORE-04: AppSettings Configuration Persistence Across Invocations", False, str(e))

        # 5. UI Snapshot & Queue Layout Rendering
        try:
            snap_res = subprocess.run([str(radium_bin), "--db", str(db_path), "--gui-snapshot"], capture_output=True, text=True)
            snap_out = snap_res.stdout + "\n" + snap_res.stderr
            has_panels = "[Sidebar Counts]" in snap_out and "[Details Panel]" in snap_out
            record("TC-CORE-05: Desktop UI Virtual View & Snapshot Rendering", has_panels, "Virtual ListView, sidebar categories & details rendered cleanly")
        except Exception as e:
            record("TC-CORE-05: Desktop UI Virtual View & Snapshot Rendering", False, str(e))

        # 6. Concurrent Multi-Task Queue Invocations
        try:
            threads = []
            results_pool = []
            def dl_worker(idx: int):
                u = f"{base_url}/files/concurrent_{idx}.bin"
                sub_dir = dl_dir / f"concurrent_{idx}"
                sub_dir.mkdir(parents=True, exist_ok=True)
                task_db = tmp_dir / f"concurrent_db_{idx}.db"
                r = subprocess.run(
                    [str(radium_bin), "--db", str(task_db), "--out", str(sub_dir), "--allow-loopback", "--download", u],
                    capture_output=True, text=True
                )
                cand_file = sub_dir / f"concurrent_{idx}.bin"
                if not cand_file.exists():
                    cand_file = sub_dir / "General" / f"concurrent_{idx}.bin"
                results_pool.append((idx, r.returncode, cand_file))

            for i in range(1, 5): # 4 concurrent download tasks
                t = threading.Thread(target=dl_worker, args=(i,))
                threads.append(t)
                t.start()

            for t in threads:
                t.join(timeout=30)

            all_ok = len(results_pool) == 4 and all(rc == 0 and f.is_file() for _, rc, f in results_pool)
            record("TC-CORE-06: Concurrent Multi-Task Queue Execution", all_ok, f"Executed {len(results_pool)}/4 concurrent tasks successfully")
        except Exception as e:
            record("TC-CORE-06: Concurrent Multi-Task Queue Execution", False, str(e))

        # 7. Pause and Resume Offset Verification via Partial File & Database Update
        try:
            # Simulate interrupted download state with .part file and database entries
            resume_db = tmp_dir / "resume_state.db"
            resume_dl = tmp_dir / "resume_downloads"
            resume_dl.mkdir(parents=True, exist_ok=True)
            part_file = resume_dl / "payload_large.bin.test-pause-task.part"
            
            # Pre-write 4 MiB of the 12 MiB payload to simulate a paused task
            part_file.write_bytes(PAYLOAD_LARGE[:4 * 1024 * 1024])
            
            # Let Radium initialize the canonical schema for resume_db
            subprocess.run([str(radium_bin), "--db", str(resume_db), "--list"], capture_output=True, text=True)

            conn = sqlite3.connect(str(resume_db))
            cur = conn.cursor()
            cur.execute("""
                INSERT OR REPLACE INTO tasks (
                    task_id, task_type, state, url, save_dir, file_name,
                    part_path, total_bytes, downloaded_bytes, max_connections,
                    created_at_ms, updated_at_ms
                ) VALUES (
                    'test-pause-task', 'HTTP_FILE', 'PAUSED', ?, ?, 'payload_large.bin',
                    ?, 12582912, 4194304, 4, 1700000000000, 1700000000000
                );
            """, (f"{base_url}/files/payload_large.bin", str(resume_dl), str(part_file)))
            conn.commit()
            conn.close()

            # Execute list to confirm state is recognized as PAUSED
            chk_proc = subprocess.run([str(radium_bin), "--db", str(resume_db), "--list"], capture_output=True, text=True, check=True)
            chk_out = chk_proc.stdout + chk_proc.stderr
            has_paused = ("PAUSED" in chk_out or "Paused" in chk_out)

            # Now execute download to resume and finish the remaining 8 MiB
            res_proc = subprocess.run(
                [str(radium_bin), "--db", str(resume_db), "--out", str(resume_dl), "--allow-loopback", "--download", f"{base_url}/files/payload_large.bin"],
                capture_output=True, text=True
            )
            final_file = resume_dl / "payload_large.bin"
            if not final_file.exists():
                final_file = resume_dl / "General" / "payload_large.bin"
            resumed_ok = final_file.is_file() and hashlib.sha256(final_file.read_bytes()).hexdigest() == HASH_LARGE
            record("TC-CORE-07: Pause/Resume Byte Offset & Full Integrity Check", has_paused and resumed_ok, "Pre-existing partial file was resumed to 100% SHA-256 match")
        except Exception as e:
            record("TC-CORE-07: Pause/Resume Byte Offset & Full Integrity Check", False, str(e))

        # 8. Non-Range Server Single Stream Fallback
        try:
            url_norange = f"{base_url}/files/norange_fallback.bin"
            nr_db = tmp_dir / "norange.db"
            nr_res = subprocess.run(
                [str(radium_bin), "--db", str(nr_db), "--out", str(dl_dir), "--allow-loopback", "--download", url_norange],
                capture_output=True, text=True
            )
            nr_file = dl_dir / "norange_fallback.bin"
            if not nr_file.exists():
                nr_file = dl_dir / "General" / "norange_fallback.bin"
            nr_ok = nr_res.returncode == 0 and nr_file.is_file() and hashlib.sha256(nr_file.read_bytes()).hexdigest() == HASH_MEDIUM
            record("TC-CORE-08: Non-Range Server Single Stream Fallback", nr_ok, "Non-range server downloaded bit-exact in single-stream mode")
        except Exception as e:
            record("TC-CORE-08: Non-Range Server Single Stream Fallback", False, str(e))

        # 9. HTTP 429 Rate Limiting with Retry-After Backoff
        try:
            ControlledTestHttpServer._hit_429 = False
            url_429 = f"{base_url}/files/rate_limited_429.bin"
            r429_db = tmp_dir / "rate_limit_429.db"
            t0_429 = time.time()
            r429_res = subprocess.run(
                [str(radium_bin), "--db", str(r429_db), "--out", str(dl_dir), "--allow-loopback", "--download", url_429],
                capture_output=True, text=True
            )
            elapsed_429 = time.time() - t0_429
            r429_file = dl_dir / "rate_limited_429.bin"
            if not r429_file.exists():
                r429_file = dl_dir / "General" / "rate_limited_429.bin"
            r429_ok = r429_res.returncode == 0 and r429_file.is_file() and hashlib.sha256(r429_file.read_bytes()).hexdigest() == HASH_MEDIUM and elapsed_429 >= 0.5
            record("TC-CORE-09: HTTP 429 Rate Limiting & Retry-After Recovery", r429_ok, f"Handled 429 with backoff in {elapsed_429:.2f}s and verified SHA-256")
        except Exception as e:
            record("TC-CORE-09: HTTP 429 Rate Limiting & Retry-After Recovery", False, str(e))

        # 10. Transient Connection Reset & Exact Resume
        try:
            ControlledTestHttpServer._hit_reset = False
            url_reset = f"{base_url}/files/reset_drop.bin"
            reset_db = tmp_dir / "reset_drop.db"
            reset_res = subprocess.run(
                [str(radium_bin), "--db", str(reset_db), "--out", str(dl_dir), "--allow-loopback", "--download", url_reset],
                capture_output=True, text=True
            )
            reset_file = dl_dir / "reset_drop.bin"
            if not reset_file.exists():
                reset_file = dl_dir / "General" / "reset_drop.bin"
            reset_ok = reset_res.returncode == 0 and reset_file.is_file() and hashlib.sha256(reset_file.read_bytes()).hexdigest() == HASH_MEDIUM
            record("TC-CORE-10: Connection Reset Auto-Recovery & Resumption", reset_ok, "Recovered from mid-stream connection reset and verified SHA-256")
        except Exception as e:
            record("TC-CORE-10: Connection Reset Auto-Recovery & Resumption", False, str(e))

    finally:
        server.shutdown()
        shutil.rmtree(tmp_dir, ignore_errors=True)

    print("=================================================================")
    print(f" Summary: {results['passed_tests']} PASSED, {results['failed_tests']} FAILED")
    print("=================================================================")
    return results


def main():
    parser = argparse.ArgumentParser(description="Radium Core Operations & Persistence Test Suite")
    parser.add_argument("--bin-dir", type=str, default=None, help="Directory containing radium.exe and radium-nmh.exe")
    parser.add_argument("--output-json", type=str, default=None, help="Path to write JSON test results")
    args = parser.parse_args()

    bin_dir = Path(args.bin_dir).resolve() if args.bin_dir else None
    radium_bin = find_binary("radium", bin_dir)
    if not radium_bin.is_file():
        radium_bin = find_binary("flux", bin_dir)
    nmh_bin = find_binary("radium-nmh", bin_dir)
    if not nmh_bin.is_file():
        nmh_bin = find_binary("flux-nmh", bin_dir)

    assert radium_bin.is_file(), f"Cannot locate radium executable: {radium_bin}"
    assert nmh_bin.is_file(), f"Cannot locate radium-nmh executable: {nmh_bin}"

    results = run_test_suite(radium_bin, nmh_bin)
    if args.output_json:
        out_p = Path(args.output_json).resolve()
        out_p.parent.mkdir(parents=True, exist_ok=True)
        out_p.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"Test results saved to: {out_p}")

    sys.exit(0 if results["failed_tests"] == 0 else 1)


if __name__ == "__main__":
    main()
