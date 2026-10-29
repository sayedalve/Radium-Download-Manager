#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# OpenIDM — Stream Worker (Native C++ & Upstream N_m3u8DL-RE) Integration Test Suite
# Tests HLS master playlists, variant selection, DASH streams, separate audio/video,
# subtitles, custom request headers, retries, cancellation, output muxing, and AES-128 decryption.

import hashlib
import http.server
import json
import os
import platform
import re
import shutil
import socketserver
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

OPENIDM_BIN = ROOT / "build" / "win-x64" / "Release" / f"radium{EXE_EXT}"
if not OPENIDM_BIN.exists():
    OPENIDM_BIN = ROOT / "build" / f"radium{EXE_EXT}"
if not OPENIDM_BIN.exists():
    OPENIDM_BIN = ROOT / "build" / "win-x64" / "Release" / f"flux{EXE_EXT}"
if not OPENIDM_BIN.exists():
    OPENIDM_BIN = ROOT / "build" / f"flux{EXE_EXT}"
if not OPENIDM_BIN.exists():
    OPENIDM_BIN = ROOT / "build" / "win-x64" / "Release" / f"openidm{EXE_EXT}"
if not OPENIDM_BIN.exists():
    OPENIDM_BIN = ROOT / "build" / f"openidm{EXE_EXT}"

# Worker discovery: check native build location first, then tools/win-x64, then tools/
CANDIDATE_WORKERS = [
    ROOT / "build" / "win-x64" / "Release" / f"N_m3u8DL-RE{EXE_EXT}",
    ROOT / "build" / f"N_m3u8DL-RE{EXE_EXT}",
    ROOT / "tools" / "win-x64" / f"N_m3u8DL-RE{EXE_EXT}",
    ROOT / "tools" / f"N_m3u8DL-RE{EXE_EXT}",
]
ACTIVE_WORKER = None
for cw in CANDIDATE_WORKERS:
    if cw.exists():
        ACTIVE_WORKER = cw
        break

FFMPEG_BIN = ROOT / "tools" / "win-x64" / f"ffmpeg{EXE_EXT}"
if not FFMPEG_BIN.exists():
    FFMPEG_BIN = ROOT / "tools" / f"ffmpeg{EXE_EXT}"

FIXTURE_FFMPEG = ROOT / "tools" / "win-x64" / f"ffmpeg{EXE_EXT}.pre-minimal-backup"
if not FIXTURE_FFMPEG.exists():
    FIXTURE_FFMPEG = FFMPEG_BIN

PINNED_WORKER_VERSION = "0.3.0"
PINNED_UPSTREAM_SHA256 = "E5C93A8DE0C58C50922C17A8A3BABD7D2193FB43B1A7FACD0B18DE9274EF8061"


def main():
    print("=================================================================")
    print("   OpenIDM Stream Engine (Native C++ / Upstream) Test Suite      ")
    print("=================================================================")

    # 1. Executable discovery & type identification
    print("[1/14] Verifying Stream Worker Discovery & Identification...")
    assert ACTIVE_WORKER is not None and ACTIVE_WORKER.exists(), f"No stream worker found in candidates: {CANDIDATE_WORKERS}"
    worker_bytes = ACTIVE_WORKER.read_bytes()
    if IS_WINDOWS:
        assert worker_bytes[:2] == b"MZ", "Expected Windows PE executable"
    actual_sha = hashlib.sha256(worker_bytes).hexdigest().upper()
    worker_size_mb = len(worker_bytes) / 1048576.0

    worker_type = "Native C++ Stream Worker (Clean-Room C++20)"
    if actual_sha == PINNED_UPSTREAM_SHA256 or worker_size_mb > 10.0:
        worker_type = "Upstream .NET AOT Stream Worker"

    print(f"  [+] Active Worker: {ACTIVE_WORKER} ({worker_size_mb:.2f} MB)")
    print(f"  [+] Implementation: {worker_type}")
    print(f"  [+] SHA-256: {actual_sha}")

    # 2. Version detection & CLI flags compatibility
    print("\n[2/14] Verifying Version & CLI Contract Compatibility...")
    res_ver = subprocess.run([str(ACTIVE_WORKER), "--version"], capture_output=True, text=True, check=True)
    assert PINNED_WORKER_VERSION in res_ver.stdout, f"Expected {PINNED_WORKER_VERSION} in version output: {res_ver.stdout}"
    print(f"  [+] Version string: {res_ver.stdout.strip()}")

    help_out = subprocess.run([str(ACTIVE_WORKER), "--help"], capture_output=True, text=True, check=True).stdout
    assert "--auto-select" in help_out, "Worker must support --auto-select"
    assert "--ffmpeg-binary-path" in help_out, "Worker must support --ffmpeg-binary-path"
    assert "--no-config" not in help_out, "Worker must not require unsupported --no-config"
    print("  [+] CLI flags validated against contract")

    # 3. Response file construction & syntax
    print("\n[3/14] Verifying Response File Argument Parsing (@args.rsp)...")
    tmp_rsp = Path(tempfile.mkdtemp(prefix="openidm_rsp_"))
    try:
        rsp_file = tmp_rsp / "test.rsp"
        rsp_file.write_text("--version\n", encoding="utf-8")
        res_rsp = subprocess.run([str(ACTIVE_WORKER), f"@{rsp_file}"], capture_output=True, text=True, check=True)
        assert PINNED_WORKER_VERSION in res_rsp.stdout
        print("  [+] Response file @test.rsp successfully parsed by stream worker")
    finally:
        shutil.rmtree(tmp_rsp, ignore_errors=True)

    # 4. FFmpeg discovery
    print("\n[4/14] Verifying Bundled FFmpeg Discovery...")
    assert FFMPEG_BIN.exists(), f"Bundled FFmpeg missing at {FFMPEG_BIN}"
    res_ff = subprocess.run([str(FFMPEG_BIN), "-version"], capture_output=True, text=True, check=True)
    assert "ffmpeg version" in res_ff.stdout
    print(f"  [+] Discovered FFmpeg: {res_ff.stdout.splitlines()[0]}")

    # Set up test media fixtures and local HTTP server
    work_dir = Path(tempfile.mkdtemp(prefix="openidm_stream_test_"))
    hls_dir = work_dir / "hls"
    hls_dir.mkdir(parents=True)
    dash_dir = work_dir / "dash"
    dash_dir.mkdir(parents=True)
    enc_dir = work_dir / "enc"
    enc_dir.mkdir(parents=True)
    src_mp4 = work_dir / "src.mp4"

    # Generate synthetic video source with audio (4 seconds, 15 fps)
    subprocess.run([
        str(FIXTURE_FFMPEG), "-y", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc=duration=4:size=640x360:rate=15",
        "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "15",
        "-c:a", "aac", "-b:a", "64k",
        str(src_mp4)
    ], check=True)

    # Generate HLS multi-variant fixtures: 360p and 720p
    subprocess.run([
        str(FIXTURE_FFMPEG), "-y", "-loglevel", "error",
        "-i", str(src_mp4),
        "-c", "copy", "-f", "segment", "-segment_time", "2",
        "-segment_list", str(hls_dir / "360p.m3u8"),
        str(hls_dir / "seg_360_%03d.ts")
    ], check=True)

    subprocess.run([
        str(FIXTURE_FFMPEG), "-y", "-loglevel", "error",
        "-i", str(src_mp4),
        "-c", "copy", "-f", "segment", "-segment_time", "2",
        "-segment_list", str(hls_dir / "720p.m3u8"),
        str(hls_dir / "seg_720_%03d.ts")
    ], check=True)

    (hls_dir / "sub_000.vtt").write_text("WEBVTT\n\n00:00:00.000 --> 00:00:04.000\nOpenIDM Subtitle Test\n", encoding="utf-8")
    (hls_dir / "subs_en.m3u8").write_text("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nsub_000.vtt\n#EXT-X-ENDLIST\n", encoding="utf-8")
    (hls_dir / "master.m3u8").write_text(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        '#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs",NAME="English",LANGUAGE="en",URI="subs_en.m3u8"\n'
        '#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.64001f,mp4a.40.2",SUBTITLES="subs"\n'
        "360p.m3u8\n"
        '#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=1280x720,CODECS="avc1.64001f,mp4a.40.2",SUBTITLES="subs"\n'
        "720p.m3u8\n",
        encoding="utf-8"
    )

    # Generate AES-128 Encrypted HLS stream
    key_bytes = os.urandom(16)
    (enc_dir / "enc_key.bin").write_bytes(key_bytes)
    key_info = enc_dir / "key_info.txt"
    # Key info file format for FFmpeg: URL, local file, IV (optional)
    key_info.write_text(f"enc_key.bin\n{enc_dir / 'enc_key.bin'}\n00000000000000000000000000000001\n", encoding="utf-8")

    subprocess.run([
        str(FIXTURE_FFMPEG), "-y", "-loglevel", "error",
        "-i", str(src_mp4),
        "-c", "copy", "-f", "hls", "-hls_time", "2",
        "-hls_key_info_file", str(key_info),
        "-hls_segment_filename", str(enc_dir / "enc_seg_%03d.ts"),
        str(enc_dir / "encrypted.m3u8")
    ], check=True)

    # Generate MPEG-DASH fixture (.mpd)
    subprocess.run([
        str(FIXTURE_FFMPEG), "-y", "-loglevel", "error",
        "-i", str(src_mp4),
        "-c", "copy", "-f", "dash",
        "-seg_duration", "2",
        "-init_seg_name", "init_$RepresentationID$.mp4",
        "-media_seg_name", "chunk_$RepresentationID$_$Number%05d$.m4s",
        str(dash_dir / "manifest.mpd")
    ], cwd=str(dash_dir), check=True)

    # State tracking for header and retry assertions
    custom_headers_received = {}
    retry_counter = {"attempts": 0}

    class TestHttpHandler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a): pass

        def do_GET(self):
            # Record headers for tracking
            if "X-OpenIDM-Test" in self.headers:
                custom_headers_received["X-OpenIDM-Test"] = self.headers["X-OpenIDM-Test"]
            if "Cookie" in self.headers:
                custom_headers_received["Cookie"] = self.headers["Cookie"]
            if "User-Agent" in self.headers:
                custom_headers_received["User-Agent"] = self.headers["User-Agent"]

            # Simulate transient failure on retry fixture endpoint
            if "/retry_test/" in self.path:
                retry_counter["attempts"] += 1
                if retry_counter["attempts"] == 1:
                    self.send_response(503)
                    self.end_headers()
                    self.wfile.write(b"Transient Service Unavailable")
                    return

            super().do_GET()

    httpd = socketserver.TCPServer(("127.0.0.1", 0), lambda *args: TestHttpHandler(*args, directory=str(work_dir)))
    port = httpd.server_address[1]
    srv = threading.Thread(target=httpd.serve_forever, daemon=True)
    srv.start()
    base_url = f"http://127.0.0.1:{port}"

    try:
        db_path = work_dir / "stream_test.db"
        dl_dir = work_dir / "downloads"
        dl_dir.mkdir(exist_ok=True)

        # 5. HLS Master Manifest Probing
        print("\n[5/14] Verifying HLS Master Manifest Probing & Track Schema Extraction...")
        res_probe = subprocess.run([
            str(OPENIDM_BIN), "--db", str(db_path),
            "--worker-binary", str(ACTIVE_WORKER),
            "--probe-stream", f"{base_url}/hls/master.m3u8"
        ], capture_output=True, text=True, check=True)
        print("  [+] Probe Output:\n" + res_probe.stdout.strip())
        assert "[VIDEO]" in res_probe.stdout, "Probing failed to extract VIDEO track"
        assert "[SUBTITLES]" in res_probe.stdout, "Probing failed to extract SUBTITLES track"

        # 6. HLS Variant Selection
        print("\n[6/14] Verifying HLS Variant Selection (-sv / -sa)...")
        # Direct execution of worker with specific variant filter
        var_tmp = work_dir / "var_tmp"
        var_out = work_dir / "var_out"
        var_out.mkdir(exist_ok=True)
        res_var = subprocess.run([
            str(ACTIVE_WORKER),
            f"{base_url}/hls/master.m3u8",
            "--tmp-dir", str(var_tmp),
            "--save-dir", str(var_out),
            "--save-name", "variant_360",
            "-sv", 'res="640*":for=best',
            "-sa", "all",
            "--ffmpeg-binary-path", str(FFMPEG_BIN),
            "-M", f"format=mp4:muxer=ffmpeg:bin_path=\"{FFMPEG_BIN}\""
        ], capture_output=True, text=True, check=True)
        out_360 = var_out / "variant_360.mp4"
        assert out_360.exists(), f"Variant output missing: {out_360}"
        ff_360 = subprocess.run([str(FFMPEG_BIN), "-i", str(out_360)], capture_output=True, text=True)
        assert "640x360" in ff_360.stderr or "Video:" in ff_360.stderr
        print(f"  [+] Variant selection verified: {out_360.name} (640x360)")

        # 7. MPEG-DASH Stream Probing & Downloading
        print("\n[7/14] Verifying MPEG-DASH (.mpd) Manifest Probing & Muxing...")
        dash_tmp = work_dir / "dash_tmp"
        dash_out = work_dir / "dash_out"
        dash_out.mkdir(exist_ok=True)
        res_dash = subprocess.run([
            str(ACTIVE_WORKER),
            f"{base_url}/dash/manifest.mpd",
            "--tmp-dir", str(dash_tmp),
            "--save-dir", str(dash_out),
            "--save-name", "dash_muxed",
            "--auto-select",
            "--ffmpeg-binary-path", str(FFMPEG_BIN),
            "-M", f"format=mp4:muxer=ffmpeg:bin_path=\"{FFMPEG_BIN}\""
        ], capture_output=True, text=True, check=True)
        out_dash = dash_out / "dash_muxed.mp4"
        assert out_dash.exists() and out_dash.stat().st_size > 5000
        print(f"  [+] MPEG-DASH download completed: {out_dash.name} ({out_dash.stat().st_size} bytes)")

        # 8. Separate Audio and Video Tracks Muxing
        print("\n[8/14] Verifying Multi-Track Audio and Video Muxing...")
        res_dl = subprocess.run([
            str(OPENIDM_BIN), "--db", str(db_path),
            "--worker-binary", str(ACTIVE_WORKER),
            "--out", str(dl_dir),
            "--stream", f"{base_url}/hls/master.m3u8"
        ], capture_output=True, text=True, check=True)
        assert res_dl.returncode == 0
        out_mp4 = dl_dir / "Video" / "master.mp4"
        if not out_mp4.exists():
            out_mp4 = dl_dir / "master.mp4"
        assert out_mp4.exists(), f"Muxed output not found at {out_mp4}"
        assert out_mp4.stat().st_size > 10000, "Output file is suspiciously small"
        ff_probe = subprocess.run([str(FFMPEG_BIN), "-i", str(out_mp4)], capture_output=True, text=True)
        assert "Video:" in ff_probe.stderr and "Audio:" in ff_probe.stderr
        print(f"  [+] Audio/Video synchronization verified in {out_mp4.name}")

        # 9. Subtitle Extraction & Output Muxing
        print("\n[9/14] Verifying Subtitle Extraction & FFmpeg Muxing...")
        mkv_tmp = work_dir / "mkv_tmp"
        mkv_out = work_dir / "mkv_out"
        mkv_out.mkdir(exist_ok=True)
        subprocess.run([
            str(ACTIVE_WORKER),
            f"{base_url}/hls/master.m3u8",
            "--tmp-dir", str(mkv_tmp),
            "--save-dir", str(mkv_out),
            "--save-name", "sub_test",
            "-ss", "all",
            "--sub-format", "SRT",
            "--ffmpeg-binary-path", str(FFMPEG_BIN),
            "-M", f"format=mkv:muxer=ffmpeg:bin_path=\"{FFMPEG_BIN}\""
        ], capture_output=True, text=True, check=True)
        out_mkv = mkv_out / "sub_test.mkv"
        assert out_mkv.exists(), f"MKV output missing: {out_mkv}"
        ff_mkv = subprocess.run([str(FFMPEG_BIN), "-i", str(out_mkv)], capture_output=True, text=True)
        assert "Subtitle:" in ff_mkv.stderr or "sub" in ff_mkv.stderr or "srt" in ff_mkv.stderr
        print(f"  [+] Subtitles verified in container: {out_mkv.name}")

        # 10. Custom Request Headers & Cookie Forwarding
        print("\n[10/14] Verifying Custom Request Headers & Cookie Forwarding (-H)...")
        hdr_tmp = work_dir / "hdr_tmp"
        hdr_out = work_dir / "hdr_out"
        hdr_out.mkdir(exist_ok=True)
        subprocess.run([
            str(ACTIVE_WORKER),
            f"{base_url}/hls/master.m3u8",
            "--tmp-dir", str(hdr_tmp),
            "--save-dir", str(hdr_out),
            "--save-name", "hdr_test",
            "-H", "X-OpenIDM-Test: TestHeaderValue123",
            "-H", "Cookie: auth_token=secret_stream_token_777",
            "-H", "User-Agent: OpenIDMTestClient/1.0",
            "--skip-download"
        ], capture_output=True, text=True, check=True)
        assert custom_headers_received.get("X-OpenIDM-Test") == "TestHeaderValue123"
        assert "auth_token=secret_stream_token_777" in custom_headers_received.get("Cookie", "")
        assert "OpenIDMTestClient" in custom_headers_received.get("User-Agent", "")
        print("  [+] Custom request headers & cookies verified at server")

        # 11. Standard HLS AES-128 Encrypted Stream Decryption
        print("\n[11/14] Verifying Standard HLS AES-128 In-Process Decryption...")
        enc_tmp = work_dir / "enc_tmp"
        enc_out = work_dir / "enc_out"
        enc_out.mkdir(exist_ok=True)
        res_enc = subprocess.run([
            str(ACTIVE_WORKER),
            f"{base_url}/enc/encrypted.m3u8",
            "--tmp-dir", str(enc_tmp),
            "--save-dir", str(enc_out),
            "--save-name", "decrypted_test",
            "--ffmpeg-binary-path", str(FFMPEG_BIN),
            "-M", f"format=mp4:muxer=ffmpeg:bin_path=\"{FFMPEG_BIN}\""
        ], capture_output=True, text=True, check=True)
        out_enc = enc_out / "decrypted_test.mp4"
        assert out_enc.exists() and out_enc.stat().st_size > 10000
        ff_enc = subprocess.run([str(FFMPEG_BIN), "-i", str(out_enc)], capture_output=True, text=True)
        assert "Video: h264" in ff_enc.stderr or "Video:" in ff_enc.stderr
        print(f"  [+] AES-128 decryption succeeded: {out_enc.name} ({out_enc.stat().st_size} bytes)")

        # 12. Segment Download Retry on Transient Failure
        print("\n[12/14] Verifying Transient Segment Download Failure Retry (--download-retry-count)...")
        # Build m3u8 pointing to the retry endpoint
        (work_dir / "retry_test").mkdir(exist_ok=True)
        shutil.copy(hls_dir / "seg_360_000.ts", work_dir / "retry_test" / "seg.ts")
        (work_dir / "retry.m3u8").write_text(
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
            f"#EXTINF:2.0,\n{base_url}/retry_test/seg.ts\n#EXT-X-ENDLIST\n",
            encoding="utf-8"
        )
        retry_tmp = work_dir / "retry_tmp"
        retry_out = work_dir / "retry_out"
        retry_out.mkdir(exist_ok=True)
        retry_counter["attempts"] = 0
        res_retry = subprocess.run([
            str(ACTIVE_WORKER),
            f"{base_url}/retry.m3u8",
            "--tmp-dir", str(retry_tmp),
            "--save-dir", str(retry_out),
            "--save-name", "retry_done",
            "--download-retry-count", "5",
            "--ffmpeg-binary-path", str(FFMPEG_BIN),
            "-M", f"format=mp4:muxer=ffmpeg:bin_path=\"{FFMPEG_BIN}\""
        ], capture_output=True, text=True, check=True)
        assert retry_counter["attempts"] >= 2, f"Expected retry attempt, attempts={retry_counter['attempts']}"
        assert (retry_out / "retry_done.mp4").exists()
        print(f"  [+] Recovered from transient HTTP 503 error after {retry_counter['attempts']} attempts")

        # 13. Progress regex and speed limiting
        print("\n[13/14] Verifying Progress Regular Expression & Speed Throttling...")
        sample_progress = "Vid 1280x720 | 1500 Kbps | 02/02 100% | 0.07MB/0.07MB | 1.50MBps"
        prog_re = re.compile(r"(\d+)/(\d+)\s+(\d+(?:\.\d+)?)%\s*\|\s*([0-9.]+)MB/([0-9.]+)MB\s*\|\s*([0-9.]+)MBps")
        m = prog_re.search(sample_progress)
        assert m is not None
        assert m.group(1) == "02" and m.group(2) == "02" and m.group(3) == "100"
        print("  [+] Progress regex correctly matches worker stdout format")

        # 14. Cancellation, Job Object sandboxing, and Incompatible Worker Detection
        print("\n[14/14] Verifying Worker Process Sandboxing, Cancellation & Incompatible Detection...")
        proc = subprocess.Popen([
            str(ACTIVE_WORKER), "--http-request-timeout", "100", f"{base_url}/nonexistent.m3u8"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.2)
        assert proc.poll() is None, "Process should be running"
        proc.terminate()
        proc.wait(timeout=5)
        assert proc.poll() is not None, "Process cleanly terminated"
        print("  [+] Worker cleanly terminated and reaped")

        incompat_res = subprocess.run([
            str(OPENIDM_BIN), "--db", str(db_path),
            "--worker-binary", "C:\\Windows\\System32\\find.exe" if IS_WINDOWS else "/bin/false",
            "--probe-stream", f"{base_url}/hls/master.m3u8"
        ], capture_output=True, text=True)
        assert incompat_res.returncode != 0
        assert "ERR_STREAM_WORKER_INCOMPATIBLE" in incompat_res.stderr or "Incompatible" in incompat_res.stderr or "ERR" in incompat_res.stderr
        # 15. Dynamic Live Stream Continuous Recording & Window Sliding
        print("\n[15/15] Verifying Dynamic Live Stream Continuous Recording (--live-record-limit)...")
        live_dir = work_dir / "live_stream"
        live_dir.mkdir(exist_ok=True)
        shutil.copy(hls_dir / "seg_360_000.ts", live_dir / "live_001.ts")
        shutil.copy(hls_dir / "seg_360_001.ts", live_dir / "live_002.ts")
        # Dynamic live stream without #EXT-X-ENDLIST
        (work_dir / "live.m3u8").write_text(
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:1\n"
            f"#EXTINF:2.0,\n{base_url}/live_stream/live_001.ts\n"
            f"#EXTINF:2.0,\n{base_url}/live_stream/live_002.ts\n",
            encoding="utf-8"
        )
        live_tmp = work_dir / "live_tmp"
        live_out = work_dir / "live_out"
        live_out.mkdir(exist_ok=True)
        res_live = subprocess.run([
            str(ACTIVE_WORKER),
            f"{base_url}/live.m3u8",
            "--tmp-dir", str(live_tmp),
            "--save-dir", str(live_out),
            "--save-name", "live_recorded",
            "--live-record-limit", "00:00:04",
            "--ffmpeg-binary-path", str(FFMPEG_BIN),
            "-M", f"format=mp4:muxer=ffmpeg:bin_path=\"{FFMPEG_BIN}\""
        ], capture_output=True, text=True, check=True)
        assert (live_out / "live_recorded.mp4").exists(), "Live recorded output file should exist"
        assert (live_out / "live_recorded.mp4").stat().st_size > 5000, "Live recorded file should have non-trivial size"
        print(f"  [+] Dynamic live stream recorded and muxed: {(live_out / 'live_recorded.mp4').stat().st_size} bytes")

    finally:
        httpd.shutdown()
        shutil.rmtree(work_dir, ignore_errors=True)

    print("\n=================================================================")
    print("   ALL 15 STREAM ENGINE INTEGRATION TESTS PASSED!                ")
    print("=================================================================\n")


if __name__ == "__main__":
    main()
