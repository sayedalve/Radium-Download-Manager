#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Radium Download Manager - Crash Recovery, Resume Integrity & Database Corruption Test Suite
Verifies:
  1. Safe recovery of interrupted tasks across simulated application crashes or OS restarts.
  2. Restoration of unfinished and queued tasks on startup without duplicate creation or completed restart.
  3. Pre-resume detection of missing, replaced, or externally truncated partial files.
  4. Bit-exact SHA-256 / validator verification and corruption rejection.
  5. SQLite transaction safety, WAL durability, and automatic schema migrations.
  6. Download history, file paths, size, and timestamp preservation across restarts.
  7. Resilient error handling under database file corruption and malformed data scenarios.
"""

import argparse
import hashlib
import http.server
import json
import os
import platform
import shutil
import socketserver
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
        ROOT / "build" / "win-x64" / "Release" / f"{name}{EXE_EXT}",
        ROOT / "build" / "Release" / f"{name}{EXE_EXT}",
        ROOT / "dist" / f"{name}{EXE_EXT}",
    ])
    for c in candidates:
        if c.is_file():
            return c
    return ROOT / f"{name}{EXE_EXT}"


# Deterministic test payloads
PAYLOAD_RECOVERY = b"RADIUM_RECOVERY_TEST_STREAM_DATA_CHUNK_" * 100000  # ~4 MB
HASH_RECOVERY = hashlib.sha256(PAYLOAD_RECOVERY).hexdigest()

class RecoveryHttpHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/files/recovery_sample.bin":
            range_header = self.headers.get("Range")
            total_size = len(PAYLOAD_RECOVERY)
            if range_header and range_header.startswith("bytes="):
                range_spec = range_header[6:].strip()
                start_s, end_s = range_spec.split("-")
                start = int(start_s) if start_s else 0
                end = int(end_s) if end_s else total_size - 1
                end = min(end, total_size - 1)
                content_len = end - start + 1

                self.send_response(206)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Range", f"bytes {start}-{end}/{total_size}")
                self.send_header("Content-Length", str(content_len))
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("ETag", '"radium-recovery-etag-v1"')
                self.send_header("Last-Modified", "Wed, 21 Oct 2026 06:00:00 GMT")
                self.end_headers()
                self.wfile.write(PAYLOAD_RECOVERY[start:end+1])
            else:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(total_size))
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("ETag", '"radium-recovery-etag-v1"')
                self.send_header("Last-Modified", "Wed, 21 Oct 2026 06:00:00 GMT")
                self.end_headers()
                self.wfile.write(PAYLOAD_RECOVERY)
        else:
            self.send_response(404)
            self.end_headers()


def run_recovery_test_suite(radium_bin: Path) -> dict:
    results = {
        "suite": "Radium Crash Recovery & Download History Integrity Suite",
        "total_tests": 8,
        "passed_tests": 0,
        "failed_tests": 0,
        "details": []
    }

    def record(test_name: str, passed: bool, msg: str):
        if passed:
            results["passed_tests"] += 1
            status = "PASS"
        else:
            results["failed_tests"] += 1
            status = "FAIL"
        results["details"].append({"test": test_name, "status": status, "message": msg})
        print(f"  [{status}] {test_name} - {msg}")

    print("=================================================================")
    print(" Radium Crash Recovery, Resume Integrity & History Verification ")
    print(f" Target Binary : {radium_bin}")
    print("=================================================================")

    # Start local deterministic range-supporting HTTP server
    server = socketserver.TCPServer(("127.0.0.1", 0), RecoveryHttpHandler)
    server_port = server.server_address[1]
    server_thread = threading.Thread(target=server.serve_forever)
    server_thread.daemon = True
    server_thread.start()
    base_url = f"http://127.0.0.1:{server_port}"

    tmp_dir = Path(tempfile.mkdtemp(prefix="radium_rec_test_")).resolve()

    try:
        # -------------------------------------------------------------
        # Test 1: Crash Recovery - Interrupted State Rollback
        # -------------------------------------------------------------
        try:
            db_path = tmp_dir / "crash_recovery.db"
            # Initialize schema
            subprocess.run([str(radium_bin), "--db", str(db_path), "--list"], capture_output=True, text=True, check=True)

            part_file = tmp_dir / "crash_sample.bin.part"
            # Write 1.5 MiB partial file
            part_file.write_bytes(PAYLOAD_RECOVERY[:1572864])

            conn = sqlite3.connect(str(db_path))
            conn.execute(
                "INSERT INTO tasks ("
                "  task_id, task_type, state, url, file_name, save_dir, part_path, "
                "  total_bytes, downloaded_bytes, max_connections, created_at_ms, updated_at_ms"
                ") VALUES ("
                "  'task-interrupted-crash', 'HTTP_FILE', 'DOWNLOADING', ?, 'crash_sample.bin', ?, ?, "
                "  ?, 1572864, 4, 1700000000000, 1700000000000"
                ");",
                (f"{base_url}/files/recovery_sample.bin", str(tmp_dir), str(part_file), len(PAYLOAD_RECOVERY))
            )
            # Add segments to test the 64 KiB safety margin rollback
            conn.execute(
                "INSERT INTO task_segments (task_id, segment_id, start_offset, current_offset, end_offset, completed) "
                "VALUES ('task-interrupted-crash', 0, 0, 1572864, 3999999, 0);"
            )
            conn.commit()
            conn.close()

            # Restart Radium against this database
            subprocess.run([str(radium_bin), "--db", str(db_path), "--list"], capture_output=True, text=True, check=True)

            conn = sqlite3.connect(str(db_path))
            row = conn.execute("SELECT state, downloaded_bytes FROM tasks WHERE task_id='task-interrupted-crash';").fetchone()
            seg_row = conn.execute("SELECT current_offset FROM task_segments WHERE task_id='task-interrupted-crash' AND segment_id=0;").fetchone()
            conn.close()

            expected_offset = 1572864 - 65536
            state_ok = (row and row[0] == "PAUSED")
            rollback_ok = (seg_row and seg_row[0] == expected_offset)
            record(
                "TC-REC-01: Interrupted Tasks Recovered to PAUSED with Safety Rollback",
                state_ok and rollback_ok,
                f"State={row[0] if row else 'None'}, Segment offset={seg_row[0] if seg_row else 0} (expected {expected_offset})"
            )
        except Exception as e:
            record("TC-REC-01: Interrupted Tasks Recovered to PAUSED with Safety Rollback", False, str(e))

        # -------------------------------------------------------------
        # Test 2: Startup Restoration Without Duplicates or Restarting Completed
        # -------------------------------------------------------------
        try:
            db_path2 = tmp_dir / "startup_restore.db"
            subprocess.run([str(radium_bin), "--db", str(db_path2), "--list"], capture_output=True, text=True, check=True)

            conn = sqlite3.connect(str(db_path2))
            conn.execute(
                "INSERT INTO tasks (task_id, task_type, state, url, file_name, save_dir, total_bytes, downloaded_bytes, created_at_ms, updated_at_ms, completed_at_ms) "
                "VALUES ('task-comp-preserve', 'HTTP_FILE', 'COMPLETED', 'https://example.com/done.zip', 'done.zip', ?, 1048576, 1048576, 1000, 1000, 1000);",
                (str(tmp_dir),)
            )
            conn.execute(
                "INSERT INTO tasks (task_id, task_type, state, url, file_name, save_dir, total_bytes, downloaded_bytes, created_at_ms, updated_at_ms) "
                "VALUES ('task-queued-preserve', 'HTTP_FILE', 'QUEUED', 'https://example.com/queued.zip', 'queued.zip', ?, 2097152, 0, 2000, 2000);",
                (str(tmp_dir),)
            )
            conn.commit()
            conn.close()

            # Multiple startups in succession
            for _ in range(3):
                subprocess.run([str(radium_bin), "--db", str(db_path2), "--list"], capture_output=True, text=True, check=True)

            conn = sqlite3.connect(str(db_path2))
            tasks = conn.execute("SELECT task_id, state, downloaded_bytes FROM tasks ORDER BY created_at_ms;").fetchall()
            conn.close()

            no_dups = (len(tasks) == 2)
            comp_preserved = (tasks[0][0] == "task-comp-preserve" and tasks[0][1] == "COMPLETED" and tasks[0][2] == 1048576)
            queued_preserved = (tasks[1][0] == "task-queued-preserve" and tasks[1][1] == "QUEUED" and tasks[1][2] == 0)

            record(
                "TC-REC-02: Queued & Completed Tasks Restored Idempotently on Startup",
                no_dups and comp_preserved and queued_preserved,
                f"Total tasks={len(tasks)}, Completed={tasks[0][1]}, Queued={tasks[1][1]}"
            )
        except Exception as e:
            record("TC-REC-02: Queued & Completed Tasks Restored Idempotently on Startup", False, str(e))

        # -------------------------------------------------------------
        # Test 3: Missing or Deleted Part File Detection Before Resume
        # -------------------------------------------------------------
        try:
            db_path3 = tmp_dir / "missing_file.db"
            subprocess.run([str(radium_bin), "--db", str(db_path3), "--list"], capture_output=True, text=True, check=True)

            non_existent_part = tmp_dir / "deleted_outside.part"
            if non_existent_part.exists():
                non_existent_part.unlink()

            conn = sqlite3.connect(str(db_path3))
            conn.execute(
                "INSERT INTO tasks (task_id, task_type, state, url, file_name, save_dir, part_path, total_bytes, downloaded_bytes, created_at_ms, updated_at_ms) "
                "VALUES ('task-lost-part', 'HTTP_FILE', 'DOWNLOADING', 'https://example.com/lost.bin', 'lost.bin', ?, ?, 5000000, 2500000, 1000, 1000);",
                (str(tmp_dir), str(non_existent_part))
            )
            conn.execute(
                "INSERT INTO task_segments (task_id, segment_id, start_offset, current_offset, end_offset, completed) "
                "VALUES ('task-lost-part', 0, 0, 2500000, 4999999, 0);"
            )
            conn.commit()
            conn.close()

            # Startup triggers reconciliation
            subprocess.run([str(radium_bin), "--db", str(db_path3), "--list"], capture_output=True, text=True, check=True)

            conn = sqlite3.connect(str(db_path3))
            row = conn.execute("SELECT state, downloaded_bytes FROM tasks WHERE task_id='task-lost-part';").fetchone()
            seg_row = conn.execute("SELECT current_offset FROM task_segments WHERE task_id='task-lost-part';").fetchone()
            conn.close()

            detected_ok = (row and row[0] == "PAUSED" and row[1] == 0)
            seg_ok = (seg_row and seg_row[0] == 0)
            record(
                "TC-REC-03: Missing Partial File Cleanly Detected and Re-zeroed",
                detected_ok and seg_ok,
                f"State={row[0] if row else 'None'}, Downloaded bytes reset to {row[1] if row else -1}"
            )
        except Exception as e:
            record("TC-REC-03: Missing Partial File Cleanly Detected and Re-zeroed", False, str(e))

        # -------------------------------------------------------------
        # Test 4: Externally Modified / Truncated Partial File Clamping
        # -------------------------------------------------------------
        try:
            db_path4 = tmp_dir / "truncated_file.db"
            subprocess.run([str(radium_bin), "--db", str(db_path4), "--list"], capture_output=True, text=True, check=True)

            trunc_part = tmp_dir / "externally_truncated.part"
            # Write 300,000 bytes on disk, while database had recorded 800,000
            trunc_part.write_bytes(b"Z" * 300000)

            conn = sqlite3.connect(str(db_path4))
            conn.execute(
                "INSERT INTO tasks (task_id, task_type, state, url, file_name, save_dir, part_path, total_bytes, downloaded_bytes, created_at_ms, updated_at_ms) "
                "VALUES ('task-trunc-part', 'HTTP_FILE', 'DOWNLOADING', 'https://example.com/t.bin', 't.bin', ?, ?, 1000000, 800000, 1000, 1000);",
                (str(tmp_dir), str(trunc_part))
            )
            conn.commit()
            conn.close()

            subprocess.run([str(radium_bin), "--db", str(db_path4), "--list"], capture_output=True, text=True, check=True)

            conn = sqlite3.connect(str(db_path4))
            row = conn.execute("SELECT state, downloaded_bytes FROM tasks WHERE task_id='task-trunc-part';").fetchone()
            conn.close()

            # Should clamp downloaded_bytes to actual on-disk size (300000) minus safety margin if applicable
            clamped_ok = (row and row[0] == "PAUSED" and row[1] <= 300000)
            record(
                "TC-REC-04: Externally Modified/Truncated File Reconciled to Disk Bounds",
                clamped_ok,
                f"State={row[0] if row else 'None'}, Reconciled bytes={row[1] if row else -1} (disk size=300000)"
            )
        except Exception as e:
            record("TC-REC-04: Externally Modified/Truncated File Reconciled to Disk Bounds", False, str(e))

        # -------------------------------------------------------------
        # Test 5: Full Resume & Bit-Exact SHA-256 Checksum Verification
        # -------------------------------------------------------------
        try:
            db_path5 = tmp_dir / "resume_sha.db"
            subprocess.run([str(radium_bin), "--db", str(db_path5), "--list"], capture_output=True, text=True, check=True)

            resume_dl_dir = tmp_dir / "resume_out"
            resume_dl_dir.mkdir(parents=True, exist_ok=True)
            task_part = resume_dl_dir / "recovery_sample.bin.part"

            # Pre-populate partial download with 1 MiB of real payload
            split_offset = 1048576
            task_part.write_bytes(PAYLOAD_RECOVERY[:split_offset])

            conn = sqlite3.connect(str(db_path5))
            conn.execute(
                "INSERT INTO tasks ("
                "  task_id, task_type, state, url, file_name, save_dir, part_path, "
                "  total_bytes, downloaded_bytes, max_connections, expected_sha256, created_at_ms, updated_at_ms"
                ") VALUES ("
                "  'task-sha-resume', 'HTTP_FILE', 'PAUSED', ?, 'recovery_sample.bin', ?, ?, "
                "  ?, ?, 4, ?, 1700000000000, 1700000000000"
                ");",
                (
                    f"{base_url}/files/recovery_sample.bin",
                    str(resume_dl_dir),
                    str(task_part),
                    len(PAYLOAD_RECOVERY),
                    split_offset,
                    HASH_RECOVERY
                )
            )
            conn.commit()
            conn.close()

            # Resume and complete download via Radium CLI
            res = subprocess.run([
                str(radium_bin),
                "--db", str(db_path5),
                "--out", str(resume_dl_dir),
                "--allow-loopback",
                "--download", f"{base_url}/files/recovery_sample.bin"
            ], capture_output=True, text=True)

            out_file = resume_dl_dir / "recovery_sample.bin"
            if not out_file.exists():
                out_file = resume_dl_dir / "General" / "recovery_sample.bin"

            downloaded_ok = (out_file.is_file() and out_file.stat().st_size == len(PAYLOAD_RECOVERY))
            actual_sha = hashlib.sha256(out_file.read_bytes()).hexdigest() if downloaded_ok else ""
            sha_ok = (actual_sha == HASH_RECOVERY)

            record(
                "TC-REC-05: Resumed Download Completes with Bit-Exact SHA-256 Match",
                downloaded_ok and sha_ok,
                f"File size={out_file.stat().st_size if downloaded_ok else 0}, SHA-256 verified={sha_ok}"
            )
        except Exception as e:
            record("TC-REC-05: Resumed Download Completes with Bit-Exact SHA-256 Match", False, str(e))

        # -------------------------------------------------------------
        # Test 6: Download History, File Paths, Timestamps & Metadata Preservation
        # -------------------------------------------------------------
        try:
            db_path6 = tmp_dir / "history_preserve.db"
            subprocess.run([str(radium_bin), "--db", str(db_path6), "--list"], capture_output=True, text=True, check=True)

            t_created = 1700000000000
            t_updated = 1700000050000
            t_completed = 1700000055000

            conn = sqlite3.connect(str(db_path6))
            conn.execute(
                "INSERT INTO tasks ("
                "  task_id, task_type, state, url, page_url, file_name, save_dir, part_path, "
                "  total_bytes, downloaded_bytes, created_at_ms, updated_at_ms, completed_at_ms, "
                "  etag, last_modified, category"
                ") VALUES ("
                "  'hist-task-1', 'HTTP_FILE', 'COMPLETED', 'https://example.com/archive.zip', 'https://example.com/page', "
                "  'archive.zip', ?, '', 9876543, 9876543, ?, ?, ?, "
                "  '\"etag-hist-77\"', 'Thu, 22 Oct 2026 12:00:00 GMT', 'Compressed'"
                ");",
                (str(tmp_dir), t_created, t_updated, t_completed)
            )
            conn.commit()
            conn.close()

            # Execute list / restart
            subprocess.run([str(radium_bin), "--db", str(db_path6), "--list"], capture_output=True, text=True, check=True)

            conn = sqlite3.connect(str(db_path6))
            row = conn.execute(
                "SELECT task_id, state, file_name, save_dir, total_bytes, downloaded_bytes, "
                "       created_at_ms, updated_at_ms, completed_at_ms, etag, last_modified, category "
                "FROM tasks WHERE task_id='hist-task-1';"
            ).fetchone()
            conn.close()

            fields_preserved = (
                row is not None and
                row[0] == "hist-task-1" and
                row[1] == "COMPLETED" and
                row[2] == "archive.zip" and
                row[3] == str(tmp_dir) and
                row[4] == 9876543 and
                row[5] == 9876543 and
                row[6] == t_created and
                row[7] == t_updated and
                row[8] == t_completed and
                row[9] == '"etag-hist-77"' and
                row[10] == "Thu, 22 Oct 2026 12:00:00 GMT" and
                row[11] == "Compressed"
            )

            record(
                "TC-REC-06: Task History, Timestamps, Paths & Metadata Preserved Unaltered",
                fields_preserved,
                f"History record verified intact: {row[2] if row else 'None'} ({row[4] if row else 0} bytes)"
            )
        except Exception as e:
            record("TC-REC-06: Task History, Timestamps, Paths & Metadata Preserved Unaltered", False, str(e))

        # -------------------------------------------------------------
        # Test 7: SQLite Schema Migrations & WAL Mode Pragma Integrity
        # -------------------------------------------------------------
        try:
            db_path7 = tmp_dir / "migration_wal.db"
            # Simulate an earlier schema database (Phase 1) missing the subsequent 9 migration columns
            legacy_conn = sqlite3.connect(str(db_path7))
            legacy_conn.execute("PRAGMA journal_mode = WAL;")
            legacy_conn.execute("""
                CREATE TABLE tasks (
                    task_id                 TEXT PRIMARY KEY,
                    task_type               TEXT NOT NULL,
                    state                   TEXT NOT NULL,
                    queue_id                TEXT NOT NULL DEFAULT 'main',
                    queue_order             INTEGER NOT NULL DEFAULT 0,
                    category                TEXT NOT NULL DEFAULT 'General',
                    url                     TEXT NOT NULL,
                    page_url                TEXT,
                    file_name               TEXT NOT NULL,
                    save_dir                TEXT NOT NULL,
                    part_path               TEXT,
                    total_bytes             INTEGER NOT NULL DEFAULT 0,
                    downloaded_bytes        INTEGER NOT NULL DEFAULT 0,
                    range_supported         INTEGER NOT NULL DEFAULT 0,
                    max_connections         INTEGER NOT NULL DEFAULT 8,
                    etag                    TEXT,
                    last_modified           TEXT,
                    headers_json            TEXT NOT NULL DEFAULT '{}',
                    encrypted_auth_blob     BLOB,
                    stream_options_json     TEXT,
                    expected_sha256         TEXT,
                    error_code              TEXT,
                    error_message           TEXT,
                    created_at_ms           INTEGER NOT NULL,
                    updated_at_ms           INTEGER NOT NULL,
                    completed_at_ms         INTEGER
                );
            """)
            legacy_conn.execute("""
                CREATE TABLE queues (
                    queue_id                TEXT PRIMARY KEY,
                    name                    TEXT NOT NULL,
                    max_concurrent_tasks    INTEGER NOT NULL DEFAULT 3,
                    speed_limit_bps         INTEGER NOT NULL DEFAULT 0
                );
            """)
            legacy_conn.execute("INSERT INTO tasks (task_id, task_type, state, url, file_name, save_dir, total_bytes, downloaded_bytes, created_at_ms, updated_at_ms) VALUES ('legacy-task', 'HTTP_FILE', 'PAUSED', 'https://example.com/l.zip', 'l.zip', ?, 100, 50, 1000, 1000);", (str(tmp_dir),))
            legacy_conn.commit()
            legacy_conn.close()

            # Start Radium: should execute migrations idempotently without failing or losing legacy records
            proc = subprocess.run([str(radium_bin), "--db", str(db_path7), "--list"], capture_output=True, text=True)

            check_conn = sqlite3.connect(str(db_path7))
            check_cur = check_conn.cursor()
            check_cur.execute("PRAGMA journal_mode;")
            j_mode = check_cur.fetchone()[0].lower()
            # Verify new columns added
            check_cur.execute("PRAGMA table_info(tasks);")
            columns = {col[1] for col in check_cur.fetchall()}
            check_cur.execute("SELECT task_id, state, downloaded_bytes FROM tasks WHERE task_id='legacy-task';")
            legacy_row = check_cur.fetchone()
            check_conn.close()

            required_cols = {"speed_limit_bps", "alternate_urls_json", "schedule_stop_time_local", "pause_reason", "uploaded_bytes", "conflict_policy", "scheduled"}
            cols_added = required_cols.issubset(columns)
            legacy_preserved = (legacy_row and legacy_row[0] == "legacy-task" and legacy_row[2] == 50)
            wal_ok = (j_mode in ("wal", "memory"))

            record(
                "TC-REC-07: Safe Schema Migration & WAL Journal Durability",
                proc.returncode == 0 and cols_added and legacy_preserved and wal_ok,
                f"WAL mode={j_mode}, Legacy record preserved, {len(columns)} columns present"
            )
        except Exception as e:
            record("TC-REC-07: Safe Schema Migration & WAL Journal Durability", False, str(e))

        # -------------------------------------------------------------
        # Test 8: Resilient Error Handling under Database Corruption
        # -------------------------------------------------------------
        try:
            db_path8 = tmp_dir / "corrupted.db"
            # Write invalid binary data into the database file
            db_path8.write_bytes(b"INVALID_HEADER_GARBAGE_PAYLOAD" * 50)

            # Execution must reject corruption with descriptive error and without hanging or crashing
            proc = subprocess.run([str(radium_bin), "--db", str(db_path8), "--list"], capture_output=True, text=True)

            has_error_reported = (proc.returncode != 0)
            descriptive_err = ("Failed to initialize Radium controller" in proc.stderr or "file is not a database" in proc.stderr)

            # Test backup and recovery: verify non-corrupted database recovers if restored
            backup_db = tmp_dir / "restored.db"
            subprocess.run([str(radium_bin), "--db", str(backup_db), "--list"], capture_output=True, text=True)
            bconn = sqlite3.connect(str(backup_db))
            bconn.execute("INSERT INTO tasks (task_id, task_type, state, url, file_name, save_dir, created_at_ms, updated_at_ms) VALUES ('res-1', 'HTTP_FILE', 'PAUSED', 'https://example.com/r.zip', 'r.zip', ?, 1, 1);", (str(tmp_dir),))
            bconn.commit()
            bconn.close()

            proc2 = subprocess.run([str(radium_bin), "--db", str(backup_db), "--list"], capture_output=True, text=True)
            clean_recovered = (proc2.returncode == 0)

            record(
                "TC-REC-08: Corrupted Database Gracefully Handled Without Hanging",
                has_error_reported and descriptive_err and clean_recovered,
                f"Handled error code {proc.returncode}, restored db operational"
            )
        except Exception as e:
            record("TC-REC-08: Corrupted Database Gracefully Handled Without Hanging", False, str(e))

    finally:
        server.shutdown()
        shutil.rmtree(tmp_dir, ignore_errors=True)

    print("=================================================================")
    print(f" Summary: {results['passed_tests']} PASSED, {results['failed_tests']} FAILED")
    print("=================================================================")
    return results


def main():
    parser = argparse.ArgumentParser(description="Radium Crash Recovery & History Verification Suite")
    parser.add_argument("--bin-dir", type=str, default=None, help="Directory containing radium.exe")
    parser.add_argument("--output-json", type=str, default=None, help="Path to write JSON test results")
    args = parser.parse_args()

    bin_dir = Path(args.bin_dir).resolve() if args.bin_dir else None
    radium_bin = find_binary("radium", bin_dir)
    if not radium_bin.is_file():
        radium_bin = find_binary("flux", bin_dir)

    assert radium_bin.is_file(), f"Cannot locate radium executable: {radium_bin}"

    results = run_recovery_test_suite(radium_bin)
    if args.output_json:
        out_p = Path(args.output_json).resolve()
        out_p.parent.mkdir(parents=True, exist_ok=True)
        out_p.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"Test results saved to: {out_p}")

    sys.exit(0 if results["failed_tests"] == 0 else 1)


if __name__ == "__main__":
    main()
