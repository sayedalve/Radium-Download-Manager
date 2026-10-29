#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run repeatable Radium HTTP tests against the local benchmark fixture.

The result JSON retains per-request engine events and per-handler fixture
records. This runner performs no external network requests.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))
import benchmark_server  # noqa: E402


def _default_executable() -> Path:
    candidates = [
        ROOT / "build_msvc" / "Release" / "radium.exe",
        ROOT / "build" / "win-x64" / "Release" / "radium.exe",
        ROOT / "dist" / "Radium.exe",
    ]
    return next((candidate for candidate in candidates if candidate.is_file()), candidates[0])


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _parse_range(value: str) -> tuple[int, int] | None:
    match = re.fullmatch(r"bytes=(\d+)-(\d+)", value.strip())
    return (int(match.group(1)), int(match.group(2))) if match else None


def _trace_from_log(log_text: str) -> tuple[list[dict], list[dict]]:
    events: list[dict] = []
    phases: list[dict] = []
    for line in log_text.splitlines():
        event_match = re.search(r"HTTP_RUNTIME_TRACE (\{.*\})", line)
        if event_match:
            try:
                events.append(json.loads(event_match.group(1)))
            except json.JSONDecodeError:
                continue
        phase_match = re.search(r"HTTP_PHASE_TRACE phase=([^ ]+) elapsed_us=(\d+)", line)
        if phase_match:
            phases.append({"phase": phase_match.group(1), "elapsed_us": int(phase_match.group(2))})
    return events, phases


def _active_request_metrics(events: list[dict]) -> dict:
    starts = {event["request_id"]: event for event in events if event.get("event") == "request_start"}
    ends = {event["request_id"]: event for event in events if event.get("event") == "request_end"}
    intervals = []
    for request_id, start in starts.items():
        end = ends.get(request_id)
        if end is None:
            continue
        start_tick = int(start.get("steady_tick_us", 0))
        end_tick = int(end.get("steady_tick_us", start_tick))
        if end_tick >= start_tick:
            intervals.append((start_tick, 1))
            intervals.append((end_tick, -1))
    active = peak = 0
    for _, delta in sorted(intervals, key=lambda item: (item[0], item[1])):
        active += delta
        peak = max(peak, active)
    return {"request_interval_count": len(intervals) // 2, "peak_active_client_requests": peak}


def _phase_durations(phases: list[dict]) -> dict:
    points = {phase["phase"]: phase["elapsed_us"] for phase in phases}
    def duration(start: str, end: str):
        if start not in points or end not in points:
            return None
        return max(0, points[end] - points[start]) / 1000.0
    return {
        "download_initialization": duration("initialization_start", "transfer_start"),
        "transfer": duration("transfer_start", "transfer_end"),
        "finalization": duration("finalization_start", "finalization_end"),
        "engine_total_to_finalization_ms": points.get("finalization_end", 0) / 1000.0 if "finalization_end" in points else None,
    }


def _percentile(values: list[int], fraction: float) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int((len(ordered) - 1) * fraction)))
    return int(ordered[index])


def _lock_timing_summary(events: list[dict]) -> dict:
    grouped: dict[str, list[dict]] = {}
    for event in events:
        if event.get("event") == "lock_timing":
            grouped.setdefault(str(event.get("lock", "unknown")), []).append(event)
    summary = {}
    for lock_name, records in grouped.items():
        waits = [int(record.get("wait_us", 0)) for record in records]
        holds = [int(record.get("hold_us", 0)) for record in records]
        operations = sorted({str(record.get("operation", "unknown")) for record in records})
        summary[lock_name] = {
            "acquisitions": len(records),
            "wait_total_us": sum(waits),
            "wait_mean_us": sum(waits) / len(waits),
            "wait_p95_us": _percentile(waits, 0.95),
            "wait_max_us": max(waits),
            "hold_total_us": sum(holds),
            "hold_mean_us": sum(holds) / len(holds),
            "hold_p95_us": _percentile(holds, 0.95),
            "hold_max_us": max(holds),
            "by_operation": {
                operation: {
                    "acquisitions": sum(1 for record in records if record.get("operation") == operation),
                    "wait_total_us": sum(int(record.get("wait_us", 0)) for record in records if record.get("operation") == operation),
                    "hold_total_us": sum(int(record.get("hold_us", 0)) for record in records if record.get("operation") == operation),
                }
                for operation in operations
            },
        }
    return summary


def _number_summary(values: list[int]) -> dict:
    if not values:
        return {"count": 0, "total_us": 0, "mean_us": 0, "median_us": 0, "p95_us": 0, "max_us": 0}
    ordered = sorted(values)
    return {
        "count": len(ordered),
        "total_us": sum(ordered),
        "mean_us": sum(ordered) / len(ordered),
        "median_us": ordered[len(ordered) // 2],
        "p95_us": _percentile(ordered, 0.95),
        "max_us": max(ordered),
    }


def run_benchmark(
    size_mb: int = 16,
    connections: int = 8,
    latency_ms: int = 0,
    initial_body_delay_ms: int = 0,
    chunk_delay_ms: int = 0,
    simulate_429_limit: int = 0,
    exe: str | Path | None = None,
    timeout_s: int = 180,
    result_path: str | Path | None = None,
) -> dict:
    executable = Path(exe) if exe else _default_executable()
    if not executable.is_file():
        raise FileNotFoundError(f"Radium executable not found: {executable}")
    if size_mb <= 0 or connections not in (1, 2, 4, 8, 16, 32):
        raise ValueError("size_mb must be positive and connections must be 1, 2, 4, 8, 16, or 32")

    expected_bytes = size_mb * 1024 * 1024
    with tempfile.TemporaryDirectory(prefix="radium_local_fixture_") as temp_root:
        root = Path(temp_root)
        data_dir = root / "fixture"
        data_dir.mkdir()
        dataset = data_dir / "payload.bin"
        expected_hash = benchmark_server.generate_deterministic_dataset(dataset, expected_bytes)
        server = benchmark_server.ThreadingBenchmarkServer(
            ("127.0.0.1", 0), benchmark_server.BenchmarkRequestHandler, data_dir, 0
        )
        server.datasets = {dataset.name: {"size": expected_bytes, "sha256": expected_hash}}
        server.response_latency_ms = max(0, latency_ms)
        server.initial_body_delay_ms = max(0, initial_body_delay_ms)
        server.response_chunk_delay_ms = max(0, chunk_delay_ms)
        server.response_chunk_bytes = 256 * 1024
        server.max_payload_handlers = max(0, simulate_429_limit)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()

        db_path = root / "state.db"
        output_dir = root / "output"
        output_dir.mkdir()
        url = f"http://127.0.0.1:{server.server_port}/files/{dataset.name}"
        cmd = [
            str(executable), "--download", url, "--db", str(db_path),
            "--out", str(output_dir), "--connections", str(connections), "--allow-loopback",
        ]
        env = os.environ.copy()
        env["RADIUM_HTTP_RANGE_TRACE"] = "1"
        wall_start = time.perf_counter_ns()
        wall_start_epoch_ns = time.time_ns()
        try:
            completed = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s, env=env)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"Radium timed out after {timeout_s}s; stdout={exc.stdout!r}; stderr={exc.stderr!r}") from exc
        wall_end_epoch_ns = time.time_ns()
        wall_end = time.perf_counter_ns()
        deadline = time.perf_counter() + 3.0
        while server.metrics.snapshot()["active_handlers"] and time.perf_counter() < deadline:
            time.sleep(0.01)
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=2)

        log_path = root / "radium.log"
        log_text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        events, phases = _trace_from_log(log_text)
        server_records = list(server.metrics.history)
        server_metrics = server.metrics.snapshot()
        output_files = [path for path in output_dir.iterdir() if path.is_file() and not path.name.endswith(".part")]
        output_path = output_files[0] if len(output_files) == 1 else None
        file_size = output_path.stat().st_size if output_path else 0
        actual_hash = _sha256(output_path) if output_path else ""

    starts = [event for event in events if event.get("event") == "request_start"]
    ends = [event for event in events if event.get("event") == "request_end"]
    payload_starts = [event for event in starts if event.get("request_kind") != "probe"]
    payload_ends = [event for event in ends if event.get("request_kind") != "probe"]
    client_received = sum(int(event.get("client_bytes_received", 0)) for event in payload_ends)
    client_written = sum(int(event.get("file_bytes_written", 0)) for event in payload_ends)
    server_offered = sum(int(record.get("server_bytes_offered", 0)) for record in server_records)

    client_pairs = collections.Counter(
        (_parse_range(f"bytes={event.get('range_start')}-{event.get('range_end')}"), int(end.get("status", 0)))
        for event in starts
        for end in ends
        if end.get("request_id") == event.get("request_id")
    )
    server_pairs = collections.Counter(
        (_parse_range(record.get("requested_range", "")), int(record.get("status", 0)))
        for record in server_records
    )
    range_status_match = client_pairs == server_pairs
    client_responses = collections.Counter(
        ((int(end.get("response_start", 0)), int(end.get("response_end", 0))) if int(end.get("status", 0)) == 206 else None,
         int(end.get("status", 0)))
        for end in ends
    )
    server_responses = collections.Counter(
        (tuple(record["response_range"]) if record.get("response_range") else None, int(record.get("status", 0)))
        for record in server_records
    )
    response_range_match = client_responses == server_responses
    request_count_match = len(starts) == len(server_records)
    phase_durations = _phase_durations(phases)
    transfer_ms = phase_durations["transfer"] or 0.0
    worker_starts = [event for event in events if event.get("event") == "worker_start"]
    worker_stops = [event for event in events if event.get("event") == "worker_stop"]
    worker_stop_by_id = {event.get("worker"): event for event in worker_stops}
    worker_lifetime_us = 0
    for start in worker_starts:
        stop = worker_stop_by_id.get(start.get("worker"))
        if stop:
            worker_lifetime_us += max(0, int(stop.get("steady_tick_us", 0)) - int(start.get("steady_tick_us", 0)))
    retry_events = [event for event in events if event.get("event") == "retry_wait"]
    coordinator_wait_events = [event for event in events if event.get("event") == "coordinator_wait_summary"]
    split_events = [event for event in events if event.get("event") == "range_split"]
    canceled_requests = [event for event in payload_ends
                         if event.get("cancel_reason") and event.get("cancel_reason") != "segment_complete"]
    completed_segment_stops = [event for event in payload_ends if event.get("cancel_reason") == "segment_complete"]
    file_sync_events = [event for event in events if event.get("event") == "file_sync"]
    request_non_callback_wait_us = sum(max(0, int(event.get("duration_us", 0)) - int(event.get("callback_processing_us", 0)))
                                       for event in payload_ends)
    file_write_total_us = sum(int(event.get("file_write_duration_us", 0)) for event in payload_ends)
    file_write_count = sum(int(event.get("file_write_count", 0)) for event in payload_ends)
    file_write_max_us = max((int(event.get("file_write_max_duration_us", 0)) for event in payload_ends), default=0)
    server_header_us = [int(record["response_headers_latency_us"]) for record in server_records
                        if record.get("response_headers_latency_us") is not None]
    server_post_header_us = [int(record["post_header_body_wait_us"]) for record in server_records
                             if record.get("post_header_body_wait_us") is not None]
    server_body_handler_us = [int(record["body_transfer_handler_us"]) for record in server_records
                              if record.get("body_transfer_handler_us") is not None]
    consistency = {
        "request_count_matches_server": request_count_match,
        "range_and_status_multisets_match_server": range_status_match,
        "response_ranges_and_statuses_match_server": response_range_match,
        "client_received_does_not_exceed_server_socket_offers": client_received <= server_offered,
        "received_bytes_cover_all_successful_client_writes": client_received >= client_written,
        "written_bytes_equal_expected_size": client_written == expected_bytes,
        "output_size_matches_expected": file_size == expected_bytes,
        "output_sha256_matches_fixture": actual_hash == expected_hash,
    }
    telemetry_consistent = all(consistency.values())
    status = "COMPLETED" if completed.returncode == 0 and output_path and actual_hash == expected_hash else "FAILED"
    result = {
        "status": status,
        "exit_code": completed.returncode,
        "executable": str(executable.resolve()),
        "size_bytes": expected_bytes,
        "configured_connections": connections,
        "fixture": {"latency_ms": latency_ms, "initial_body_delay_ms": initial_body_delay_ms,
                    "chunk_delay_ms_per_256KiB": chunk_delay_ms, "simulated_payload_handler_limit": simulate_429_limit},
        "url": url,
        "wall_start_epoch_ns": wall_start_epoch_ns,
        "wall_end_epoch_ns": wall_end_epoch_ns,
        "process_duration_ms": (wall_end - wall_start) / 1_000_000.0,
        "engine_phase_durations_ms": phase_durations,
        "useful_bytes": client_written,
        "useful_transfer_mib_s": (client_written / (1024 * 1024)) / (transfer_ms / 1000.0) if transfer_ms else None,
        "requests_started": len(payload_starts),
        "client_request_count": len(payload_starts),
        "client_total_request_count_including_probe": len(starts),
        "client_request_end_count": len(ends),
        "client_bytes_received": client_received,
        "file_bytes_written_by_client": client_written,
        "server_bytes_offered_to_socket": server_offered,
        "server_payload_handler_peak": server_metrics.get("peak_connections", 0),
        "server_all_handler_peak": server_metrics.get("peak_handlers", 0),
        "server_request_count": len(server_records),
        "server_request_status_counts": dict(collections.Counter(str(r.get("status", 0)) for r in server_records)),
        "server_payload_bytes_offered": server_offered,
        "server_timing_us": {
            "request_to_headers": _number_summary(server_header_us),
            "headers_to_body_start": _number_summary(server_post_header_us),
            "body_start_to_handler_end": _number_summary(server_body_handler_us),
        },
        "peak_runtime_active_requests": _active_request_metrics(events)["peak_active_client_requests"],
        "runtime_request_intervals": _active_request_metrics(events)["request_interval_count"],
        "runtime_worker_start_count": len(worker_starts),
        "runtime_worker_stop_count": len(worker_stops),
        "runtime_worker_lifetime_us": worker_lifetime_us,
        "runtime_worker_idle_wait_us": sum(int(event.get("idle_wait_us", 0)) for event in worker_stops),
        "runtime_worker_queue_wait_us": sum(int(event.get("queue_wait_us", 0)) for event in worker_stops),
        "runtime_worker_queue_wait_count": sum(int(event.get("queue_wait_count", 0)) for event in worker_stops),
        "runtime_coordinator_poll_wait_us": sum(int(event.get("poll_wait_us", 0)) for event in coordinator_wait_events),
        "runtime_coordinator_poll_count": sum(int(event.get("poll_count", 0)) for event in coordinator_wait_events),
        "runtime_worker_slot_occupancy_percent": (worker_lifetime_us / 1000.0) / (transfer_ms * connections) * 100.0 if transfer_ms else None,
        "runtime_range_split_count": len(split_events),
        "runtime_range_split_cancel_count": sum(event.get("cancel_reason") == "range_split" for event in payload_ends),
        "runtime_cancelled_request_count": len(canceled_requests),
        "runtime_received_bytes_in_cancelled_requests": sum(int(event.get("client_bytes_received", 0)) for event in canceled_requests),
        "runtime_completed_segment_stop_count": len(completed_segment_stops),
        "runtime_discarded_client_bytes": max(0, client_received - client_written),
        "runtime_retry_wait_count": len(retry_events),
        "runtime_retry_wait_us": sum(int(event.get("actual_wait_us", 0)) for event in retry_events),
        "runtime_limiter_wait_us": sum(int(event.get("limiter_wait_us", 0)) for event in payload_ends),
        "runtime_callback_processing_us": sum(int(event.get("callback_processing_us", 0)) for event in payload_ends),
        "runtime_request_non_callback_wait_us": request_non_callback_wait_us,
        "runtime_lock_timing": _lock_timing_summary(events),
        "runtime_file_write_count": file_write_count,
        "runtime_file_write_total_us": file_write_total_us,
        "runtime_file_write_mean_us": file_write_total_us / file_write_count if file_write_count else 0,
        "runtime_file_write_max_us": file_write_max_us,
        "runtime_file_sync_total_us": sum(int(event.get("duration_us", 0)) for event in file_sync_events),
        "file_size": file_size,
        "sha256_match": actual_hash == expected_hash,
        "expected_sha256": expected_hash,
        "computed_sha256": actual_hash,
        "telemetry_consistent": telemetry_consistent,
        "telemetry_consistency": consistency,
        "client_runtime_events": events,
        "client_phase_events": phases,
        "server_request_log": server_records,
        "app_trace_lines": [line for line in log_text.splitlines() if "HTTP_RUNTIME_TRACE " in line or "HTTP_RANGE_TRACE " in line or "HTTP_PHASE_TRACE " in line],
        "app_stdout": completed.stdout,
        "app_stderr": completed.stderr,
    }
    if result_path:
        destination = Path(result_path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(result, indent=2), encoding="utf-8")
        result["result_file"] = str(destination.resolve())
    return result


def run_matrix(executable: Path, output_dir: Path, size_mb: int, repeats: int) -> list[dict]:
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = [
        ("fast", 0, 0),
        ("latency_only", 25, 0),
        ("pacing_only", 0, 5),
        ("latency_paced", 25, 5),
    ]
    all_results = []
    for scenario, latency_ms, chunk_delay_ms in cases:
        for connections in (4, 8):
            for repeat in range(1, repeats + 1):
                name = f"{scenario}_{connections}conn_repeat{repeat}.json"
                result = run_benchmark(
                    size_mb=size_mb,
                    connections=connections,
                    latency_ms=latency_ms,
                    chunk_delay_ms=chunk_delay_ms,
                    exe=executable,
                )
                result["scenario"] = scenario
                result["repeat"] = repeat
                destination = output_dir / name
                destination.write_text(json.dumps(result, indent=2), encoding="utf-8")
                result["result_file"] = str(destination.resolve())
                all_results.append(result)
                print(f"{scenario:13} {connections} conn run {repeat}: {result['status']} | "
                      f"transfer={result['engine_phase_durations_ms']['transfer']} ms | "
                      f"client peak={result['peak_runtime_active_requests']} | "
                      f"server payload peak={result['server_payload_handler_peak']} | "
                      f"sha256={result['sha256_match']} | consistent={result['telemetry_consistent']}")

    summary = []
    for scenario, _, _ in cases:
        for connections in (4, 8):
            group = [r for r in all_results if r.get("scenario") == scenario and r["configured_connections"] == connections]
            summary.append({
                "scenario": scenario,
                "configured_connections": connections,
                "runs": len(group),
                "transfer_ms_each": [r["engine_phase_durations_ms"]["transfer"] for r in group],
                "process_ms_each": [r["process_duration_ms"] for r in group],
                "client_peak_active_each": [r["peak_runtime_active_requests"] for r in group],
                "server_payload_handler_peak_each": [r["server_payload_handler_peak"] for r in group],
                "request_count_each_including_probe": [r["client_total_request_count_including_probe"] for r in group],
                "discarded_client_bytes_each": [r["runtime_discarded_client_bytes"] for r in group],
                "useful_mib_s_each": [r["useful_transfer_mib_s"] for r in group],
                "worker_slot_occupancy_percent_each": [r["runtime_worker_slot_occupancy_percent"] for r in group],
                "scheduler_lock_wait_us_each": [r["runtime_lock_timing"].get("scheduler_state", {}).get("wait_total_us", 0) for r in group],
                "scheduler_lock_hold_us_each": [r["runtime_lock_timing"].get("scheduler_state", {}).get("hold_total_us", 0) for r in group],
                "segment_write_lock_wait_us_each": [r["runtime_lock_timing"].get("segment_write", {}).get("wait_total_us", 0) for r in group],
                "segment_write_lock_hold_us_each": [r["runtime_lock_timing"].get("segment_write", {}).get("hold_total_us", 0) for r in group],
                "file_write_total_us_each": [r["runtime_file_write_total_us"] for r in group],
                "file_write_max_us_each": [r["runtime_file_write_max_us"] for r in group],
                "worker_queue_wait_us_each": [r["runtime_worker_queue_wait_us"] for r in group],
                "coordinator_poll_wait_us_each": [r["runtime_coordinator_poll_wait_us"] for r in group],
                "request_non_callback_wait_us_each": [r["runtime_request_non_callback_wait_us"] for r in group],
                "all_hashes_match": all(r["sha256_match"] for r in group),
                "all_telemetry_consistent": all(r["telemetry_consistent"] for r in group),
            })
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps({"summary": summary, "runs": all_results}, indent=2), encoding="utf-8")
    return all_results


def main() -> int:
    parser = argparse.ArgumentParser(description="Controlled local Radium telemetry benchmark")
    parser.add_argument("--exe", type=Path, default=_default_executable(), help="Radium executable to test")
    parser.add_argument("--size-mb", type=int, default=16)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--matrix", action="store_true", help="Run fast, latency-only, pacing-only, and combined 4/8 connection scenarios")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "benchmarks" / "results" / "local_telemetry_audit")
    args = parser.parse_args()
    if args.matrix:
        results = run_matrix(args.exe, args.out_dir, args.size_mb, args.repeats)
        return 0 if all(r["status"] == "COMPLETED" and r["telemetry_consistent"] for r in results) else 1
    result = run_benchmark(size_mb=args.size_mb, exe=args.exe)
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "COMPLETED" and result["telemetry_consistent"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
