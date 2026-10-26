# SPDX-License-Identifier: GPL-3.0-or-later
"""Runtime HTTP telemetry consistency checks against the loopback fixture."""

import importlib.util
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "benchmarks" / "scripts" / "run_local_benchmark.py"
SPEC = importlib.util.spec_from_file_location("radium_local_benchmark", SCRIPT)
benchmark = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(benchmark)


def main():
    result = benchmark.run_benchmark(
        size_mb=16,
        connections=8,
        latency_ms=2,
        initial_body_delay_ms=200,
    )
    assert result["status"] == "COMPLETED", result
    assert result["telemetry_consistent"], result["telemetry_consistency"]
    assert result["telemetry_consistency"]["request_count_matches_server"]
    assert result["telemetry_consistency"]["range_and_status_multisets_match_server"]
    assert result["telemetry_consistency"]["response_ranges_and_statuses_match_server"]
    assert result["runtime_worker_start_count"] == result["runtime_worker_stop_count"]
    assert result["runtime_worker_start_count"] >= 1
    assert result["runtime_range_split_count"] >= 1
    assert result["runtime_range_split_cancel_count"] <= result["runtime_range_split_count"]
    assert result["runtime_completed_segment_stop_count"] >= 1
    assert result["peak_runtime_active_requests"] >= 1
    assert result["client_request_count"] == result["requests_started"]
    assert result["client_bytes_received"] >= result["file_bytes_written_by_client"]
    assert result["file_bytes_written_by_client"] == result["file_size"]
    assert result["runtime_discarded_client_bytes"] == result["client_bytes_received"] - result["file_bytes_written_by_client"]
    assert result["server_request_count"] == result["client_total_request_count_including_probe"]
    assert all("requested_range" in request and "start_epoch_ns" in request and "end_epoch_ns" in request
               for request in result["server_request_log"])
    assert result["file_size"] == 16 * 1024 * 1024
    assert result["sha256_match"]
    assert result["engine_phase_durations_ms"]["download_initialization"] is not None
    assert result["engine_phase_durations_ms"]["transfer"] > 0
    assert result["engine_phase_durations_ms"]["finalization"] is not None
    assert result["runtime_lock_timing"]["scheduler_state"]["acquisitions"] > 0
    assert result["runtime_lock_timing"]["segment_write"]["acquisitions"] > 0
    assert result["runtime_file_write_count"] > 0
    assert result["runtime_file_write_total_us"] >= 0
    assert result["runtime_worker_queue_wait_count"] >= 0
    assert result["runtime_coordinator_poll_count"] > 0
    assert result["runtime_file_sync_total_us"] >= 0
    assert result["server_timing_us"]["request_to_headers"]["count"] > 0

    retry_result = benchmark.run_benchmark(size_mb=4, connections=4, simulate_429_limit=1)
    assert retry_result["status"] == "COMPLETED", retry_result
    assert retry_result["telemetry_consistent"], retry_result["telemetry_consistency"]
    assert retry_result["server_request_status_counts"].get("429", "0") != "0"
    assert retry_result["runtime_retry_wait_count"] >= 1
    assert retry_result["runtime_retry_wait_us"] > 0
    assert retry_result["runtime_worker_start_count"] == retry_result["runtime_worker_stop_count"]
    assert retry_result["sha256_match"]

    latency_only = benchmark.run_benchmark(size_mb=4, connections=4, latency_ms=10, chunk_delay_ms=0)
    assert latency_only["status"] == "COMPLETED", latency_only
    assert latency_only["telemetry_consistent"], latency_only["telemetry_consistency"]
    assert latency_only["fixture"]["latency_ms"] == 10
    assert latency_only["fixture"]["chunk_delay_ms_per_256KiB"] == 0
    assert latency_only["server_timing_us"]["request_to_headers"]["median_us"] >= 10000

    pacing_only = benchmark.run_benchmark(size_mb=4, connections=4, latency_ms=0, chunk_delay_ms=1)
    assert pacing_only["status"] == "COMPLETED", pacing_only
    assert pacing_only["telemetry_consistent"], pacing_only["telemetry_consistency"]
    assert pacing_only["fixture"]["latency_ms"] == 0
    assert pacing_only["fixture"]["chunk_delay_ms_per_256KiB"] == 1
    print(
        "PASS: runtime telemetry matched fixture request count/ranges, "
        f"{result['client_bytes_received']} received bytes, "
        f"{result['file_bytes_written_by_client']} written bytes, and SHA-256; "
        f"{result['peak_runtime_active_requests']} peak active requests, "
        f"{result['runtime_range_split_count']} range splits, "
        f"{result['runtime_lock_timing']['scheduler_state']['acquisitions']} scheduler lock samples, "
        f"{result['runtime_file_write_count']} file-write samples. Simulated-429 retry "
        f"telemetry matched {retry_result['requests_started']} payload GETs plus the probe and "
        f"recorded {retry_result['runtime_retry_wait_us']} us waiting."
    )


if __name__ == "__main__":
    main()
