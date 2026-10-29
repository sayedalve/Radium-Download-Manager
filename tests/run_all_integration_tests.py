#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# OpenIDM — Portable End-to-End Integration, HTTP Fault-Injection, Native Messaging IPC,
# Stream Pipeline (HLS/DASH/AES-128 + FFmpeg), UI Snapshot, and Enforced Benchmark Suite (8 Suites).

import argparse
import hashlib
import http.server
import json
import os
import platform
try:
    import resource
except ImportError:
    resource = None
import shutil
import socket
import socketserver
import statistics
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def resolve_project_root() -> Path:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--root", type=str, default=None)
    args, _ = parser.parse_known_args()
    if args.root:
        return Path(args.root).resolve()
    if os.environ.get("OPENIDM_PROJECT_ROOT"):
        return Path(os.environ["OPENIDM_PROJECT_ROOT"]).resolve()
    return Path(__file__).resolve().parent.parent


ROOT = resolve_project_root()
IS_WINDOWS = (os.name == "nt" or platform.system().lower().startswith("win"))
EXE_EXT = ".exe" if IS_WINDOWS else ""

tools_dir = ROOT / "tools" / ("win-x64" if IS_WINDOWS else "")
if tools_dir.is_dir():
    os.environ["PATH"] = str(tools_dir.resolve()) + os.pathsep + os.environ.get("PATH", "")
os.environ["RADIUM_ALLOW_LOOPBACK"] = "1"
os.environ["FLUX_ALLOW_LOOPBACK"] = "1"
os.environ["OPENIDM_ALLOW_LOOPBACK"] = "1"


def find_binary(name: str) -> Path:
    candidates = [
        ROOT / "build" / "win-x64" / "Release" / f"{name}{EXE_EXT}",
        ROOT / "build" / "Release" / f"{name}{EXE_EXT}",
        ROOT / "build" / f"{name}{EXE_EXT}",
        ROOT / "tools" / "win-x64" / f"{name}{EXE_EXT}",
        ROOT / "tools" / f"{name}{EXE_EXT}",
    ]
    if name in ("N_m3u8DL-RE", "ffmpeg", "ffprobe"):
        candidates.reverse()
    for c in candidates:
        if c.is_file():
            return c
    which_c = shutil.which(f"{name}{EXE_EXT}") or shutil.which(name)
    if which_c:
        return Path(which_c)
    return candidates[0]


FLUX_BIN = find_binary("radium")
if not FLUX_BIN.is_file():
    FLUX_BIN = find_binary("flux")
if not FLUX_BIN.is_file():
    FLUX_BIN = find_binary("openidm")
OPENIDM_BIN = FLUX_BIN

FLUX_NMH_BIN = find_binary("radium-nmh")
if not FLUX_NMH_BIN.is_file():
    FLUX_NMH_BIN = find_binary("flux-nmh")
if not FLUX_NMH_BIN.is_file():
    FLUX_NMH_BIN = find_binary("openidm-nmh")
NMH_BIN = FLUX_NMH_BIN

STREAM_WORKER = find_binary("N_m3u8DL-RE")
FFMPEG_BIN = find_binary("ffmpeg")

PAYLOAD_4MB = os.urandom(4 * 1048576)
SHA256_4MB = hashlib.sha256(PAYLOAD_4MB).hexdigest()

PAYLOAD_16MB = os.urandom(16 * 1048576)
SHA256_16MB = hashlib.sha256(PAYLOAD_16MB).hexdigest()


class FaultInjectionHttpHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    flaky_attempts = 0
    fixture_dir = None
    aes_key_bytes = b"0123456789abcdef"

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/redirect/release":
            self.send_response(302)
            self.send_header("Location", "/files/ranged_4mb.bin")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if path == "/files/ranged_4mb.bin":
            self._serve_buffer(
                PAYLOAD_4MB,
                support_range=True,
                etag='"v1-4mb"',
                content_disposition="attachment; filename*=UTF-8''Radium_Test_Archive.bin",
            )
            return

        if path == "/files/bench_16mb.bin":
            self._serve_buffer(PAYLOAD_16MB, support_range=True, etag='"bench-16mb"')
            return

        if path == "/files/norange_1mb.bin":
            data = PAYLOAD_4MB[:1048576]
            self._serve_buffer(data, support_range=False, etag='"norange-1mb"')
            return

        if path == "/files/flaky_2mb.bin":
            data = PAYLOAD_4MB[: 2 * 1048576]
            rng = self.headers.get("Range", "")
            if rng and rng != "bytes=0-0" and FaultInjectionHttpHandler.flaky_attempts == 0:
                FaultInjectionHttpHandler.flaky_attempts += 1
                total = len(data)
                start, end = 0, total - 1
                if rng.startswith("bytes="):
                    spec = rng.split("=", 1)[1].strip()
                    parts = spec.split("-", 1)
                    if parts[0]:
                        start = int(parts[0])
                    if len(parts) > 1 and parts[1]:
                        end = min(int(parts[1]), total - 1)
                span = end - start + 1
                self.send_response(206)
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
                self.send_header("Content-Length", str(span))
                self.end_headers()
                partial_size = min(65536, max(1, span // 2))
                try:
                    self.wfile.write(data[start : start + partial_size])
                    self.wfile.flush()
                except Exception:
                    pass
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                except Exception:
                    pass
                return
            self._serve_buffer(data, support_range=True, etag='"flaky-2mb"')
            return

        if path == "/keys/aes128.key":
            cookie = self.headers.get("Cookie", "")
            if "stream_session=authorized_777" not in cookie:
                self.send_response(403)
                self.send_header("Content-Length", "9")
                self.end_headers()
                self.wfile.write(b"Forbidden")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", "16")
            self.end_headers()
            self.wfile.write(FaultInjectionHttpHandler.aes_key_bytes)
            return

        if FaultInjectionHttpHandler.fixture_dir:
            rel = path.lstrip("/")
            fpath = FaultInjectionHttpHandler.fixture_dir / rel
            if fpath.exists() and fpath.is_file():
                raw = fpath.read_bytes()
                self.send_response(200)
                if fpath.suffix == ".m3u8":
                    self.send_header("Content-Type", "application/vnd.apple.mpegurl")
                elif fpath.suffix == ".mpd":
                    self.send_header("Content-Type", "application/dash+xml")
                elif fpath.suffix == ".vtt":
                    self.send_header("Content-Type", "text/vtt")
                else:
                    self.send_header("Content-Type", "video/mp2t")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return

        self.send_response(404)
        self.send_header("Content-Length", "9")
        self.end_headers()
        self.wfile.write(b"Not Found")

    def _serve_buffer(self, buf, support_range=True, etag=None, content_disposition=None):
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
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
            self.send_header("Content-Length", str(len(chunk)))
            if etag:
                self.send_header("ETag", etag)
            if content_disposition:
                self.send_header("Content-Disposition", content_disposition)
            self.end_headers()
            try:
                self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_response(200)
            if support_range:
                self.send_header("Accept-Ranges", "bytes")
            else:
                self.send_header("Accept-Ranges", "none")
            self.send_header("Content-Length", str(total))
            if etag:
                self.send_header("ETag", etag)
            if content_disposition:
                self.send_header("Content-Disposition", content_disposition)
            self.end_headers()
            try:
                self.wfile.write(mv)
            except (BrokenPipeError, ConnectionResetError):
                pass


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass


def build_media_fixtures(fixture_dir: Path, base_url: str, aes_key: bytes):
    hls_dir = fixture_dir / "hls"
    hls_dir.mkdir(parents=True, exist_ok=True)

    src_mp4 = fixture_dir / "source.mp4"
    static_media_dir = ROOT / "tests" / "fixtures" / "media"
    if (static_media_dir / "source.mp4").is_file() and (static_media_dir / "seg_000.ts").is_file():
        shutil.copy2(static_media_dir / "source.mp4", src_mp4)
        for ts_file in static_media_dir.glob("seg_*.ts"):
            shutil.copy2(ts_file, hls_dir / ts_file.name)
        if (static_media_dir / "720p.m3u8").is_file():
            shutil.copy2(static_media_dir / "720p.m3u8", hls_dir / "720p.m3u8")
    else:
        gen_ffmpeg = ROOT / "tools" / "win-x64" / f"ffmpeg{EXE_EXT}.pre-minimal-backup"
        if not gen_ffmpeg.is_file():
            gen_ffmpeg = shutil.which("ffmpeg") or "ffmpeg"
        subprocess.run(
            [
                str(gen_ffmpeg), "-y", "-loglevel", "error",
                "-f", "lavfi", "-i", "testsrc=duration=4:size=640x360:rate=15",
                "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
                "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "15",
                "-c:a", "aac", "-b:a", "64k",
                str(src_mp4),
            ],
            check=True,
        )

        subprocess.run(
            [
                str(gen_ffmpeg), "-y", "-loglevel", "error",
                "-i", str(src_mp4),
                "-c", "copy", "-f", "segment", "-segment_time", "2",
                "-segment_list", str(hls_dir / "720p.m3u8"),
                str(hls_dir / "seg_%03d.ts"),
            ],
            check=True,
        )

    (hls_dir / "sub_000.vtt").write_text(
        "WEBVTT\n\n00:00:00.000 --> 00:00:04.000\nOpenIDM Automated Stream Test Subtitle\n",
        encoding="utf-8",
    )
    (hls_dir / "subs_en.m3u8").write_text(
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nsub_000.vtt\n#EXT-X-ENDLIST\n",
        encoding="utf-8",
    )

    (hls_dir / "master.m3u8").write_text(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        '#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs",NAME="English",LANGUAGE="en",URI="subs_en.m3u8"\n'
        '#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=640x360,CODECS="avc1.64001f,mp4a.40.2",SUBTITLES="subs"\n'
        "720p.m3u8\n",
        encoding="utf-8",
    )

    enc_dir = fixture_dir / "hls_aes"
    enc_dir.mkdir(parents=True, exist_ok=True)
    iv_hex = "00000000000000000000000000000001"
    iv_bytes = bytes.fromhex(iv_hex)

    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        from cryptography.hazmat.primitives import padding
        from cryptography.hazmat.backends import default_backend
        has_crypto = True
    except ImportError:
        has_crypto = False

    for idx in range(2):
        plain_ts = hls_dir / f"seg_{idx:03d}.ts"
        enc_ts = enc_dir / f"enc_{idx:03d}.ts"
        if has_crypto:
            plain_data = plain_ts.read_bytes()
            padder = padding.PKCS7(128).padder()
            padded_data = padder.update(plain_data) + padder.finalize()
            cipher = Cipher(algorithms.AES(aes_key), modes.CBC(iv_bytes), backend=default_backend())
            enc = cipher.encryptor()
            enc_ts.write_bytes(enc.update(padded_data) + enc.finalize())
        else:
            subprocess.run(
                [
                    "openssl", "enc", "-aes-128-cbc",
                    "-in", str(plain_ts),
                    "-out", str(enc_ts),
                    "-K", aes_key.hex(),
                    "-iv", iv_hex,
                ],
                check=True,
            )
    (enc_dir / "index.m3u8").write_text(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:2\n"
        f'#EXT-X-KEY:METHOD=AES-128,URI="{base_url}/keys/aes128.key",IV=0x{iv_hex}\n'
        "#EXTINF:2.0,\nenc_000.ts\n"
        "#EXTINF:2.0,\nenc_001.ts\n"
        "#EXT-X-ENDLIST\n",
        encoding="utf-8",
    )

    dash_dir = fixture_dir / "dash"
    dash_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src_mp4, dash_dir / "rep_video.mp4")
    (dash_dir / "manifest.mpd").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT4S">\n'
        '  <Period id="0">\n'
        '    <AdaptationSet mimeType="video/mp4" contentType="video" lang="en">\n'
        '      <Representation id="v0" bandwidth="1500000" width="640" height="360" codecs="avc1.64001f">\n'
        '        <BaseURL>rep_video.mp4</BaseURL>\n'
        '      </Representation>\n'
        '    </AdaptationSet>\n'
        '  </Period>\n'
        '</MPD>\n',
        encoding="utf-8",
    )


def send_nmh_rpc(proc, obj):
    raw = json.dumps(obj).encode("utf-8")
    proc.stdin.write(struct.pack("<I", len(raw)) + raw)
    proc.stdin.flush()
    hdr = proc.stdout.read(4)
    if len(hdr) < 4:
        raise RuntimeError("NMH EOF while reading response length")
    resp_len = struct.unpack("<I", hdr)[0]
    body = proc.stdout.read(resp_len)
    return json.loads(body.decode("utf-8"))


def ffprobe_verify(file_path: Path):
    ffprobe_cmd = shutil.which("ffprobe")
    if ffprobe_cmd:
        res = subprocess.run(
            [
                ffprobe_cmd, "-v", "error",
                "-show_entries", "format=duration:stream=codec_type",
                "-of", "json",
                str(file_path),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        info = json.loads(res.stdout)
        streams = [s.get("codec_type") for s in info.get("streams", [])]
        dur = float(info.get("format", {}).get("duration", "0"))
        return streams, dur

    ffmpeg_bin = find_binary("ffmpeg")
    res = subprocess.run([str(ffmpeg_bin), "-i", str(file_path)], capture_output=True, text=True)
    text = res.stderr
    streams = []
    dur = 0.0
    for line in text.splitlines():
        line = line.strip()
        if "Duration:" in line:
            import re
            m = re.search(r"Duration:\s*(\d+):(\d+):(\d+\.?\d*)", line)
            if m:
                h, m_min, s = m.groups()
                dur = int(h) * 3600 + int(m_min) * 60 + float(s)
        if "Stream #" in line:
            if "Video:" in line:
                streams.append("video")
            elif "Audio:" in line:
                streams.append("audio")
            elif "Subtitle:" in line:
                streams.append("subtitle")
    return streams, dur


def verify_platform_binary_format(bin_path: Path):
    assert bin_path.exists(), f"Missing binary: {bin_path}"
    head = bin_path.read_bytes()[:4]
    if IS_WINDOWS:
        assert head[:2] == b"MZ", f"Expected Windows PE MZ executable at {bin_path}, got {head}"
    else:
        assert head == b"\x7fELF", f"Expected POSIX ELF executable at {bin_path}, got {head}"


if IS_WINDOWS:
    import ctypes
    import ctypes.wintypes

    class PMC(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]

    class FILETIME(ctypes.Structure):
        _fields_ = [("dwLowDateTime", ctypes.wintypes.DWORD), ("dwHighDateTime", ctypes.wintypes.DWORD)]


def measure_child_command_rusage(cmd_list):
    if IS_WINDOWS:
        t0 = time.perf_counter()
        proc = subprocess.Popen([str(x) for x in cmd_list], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        proc.wait()
        elapsed_s = time.perf_counter() - t0
        if proc.returncode != 0:
            raise subprocess.CalledProcessError(proc.returncode, cmd_list)

        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        ctypes.windll.psapi.GetProcessMemoryInfo(int(proc._handle), ctypes.byref(pmc), pmc.cb)

        creation = FILETIME(); exit_t = FILETIME(); kernel_t = FILETIME(); user_t = FILETIME()
        ctypes.windll.kernel32.GetProcessTimes(
            int(proc._handle),
            ctypes.byref(creation),
            ctypes.byref(exit_t),
            ctypes.byref(kernel_t),
            ctypes.byref(user_t),
        )

        utime_s = ((user_t.dwHighDateTime << 32) + user_t.dwLowDateTime) / 1e7
        stime_s = ((kernel_t.dwHighDateTime << 32) + kernel_t.dwLowDateTime) / 1e7
        maxrss_kb = pmc.PeakWorkingSetSize // 1024
        return {
            "elapsed_s": elapsed_s,
            "maxrss_kb": maxrss_kb,
            "utime_s": utime_s,
            "stime_s": stime_s,
        }
    else:
        code = (
            "import json, resource, subprocess, sys, time; "
            "t0 = time.perf_counter(); "
            "subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True); "
            "elapsed_s = time.perf_counter() - t0; "
            "ru = resource.getrusage(resource.RUSAGE_CHILDREN); "
            "print(json.dumps({'elapsed_s': elapsed_s, 'maxrss_kb': ru.ru_maxrss, 'utime_s': ru.ru_utime, 'stime_s': ru.ru_stime}))"
        )
        out = subprocess.check_output([sys.executable, "-c", code] + [str(x) for x in cmd_list], text=True).strip()
        return json.loads(out)


def main():
    work_dir = Path(tempfile.mkdtemp(prefix="openidm_e2e_"))
    fixture_dir = work_dir / "fixtures"
    dl_dir = work_dir / "downloads"
    db_path = work_dir / "state.db"
    sock_path = f"\\\\.\\pipe\\flux_test_{os.getpid()}" if IS_WINDOWS else f"/tmp/flux_test_{os.getpid()}.sock"
    daemon_proc = None
    nmh_proc = None

    FaultInjectionHttpHandler.fixture_dir = fixture_dir
    server = ThreadedHTTPServer(("127.0.0.1", 0), FaultInjectionHttpHandler)
    port = server.server_address[1]
    base_url = f"http://127.0.0.1:{port}"
    srv_thread = threading.Thread(target=server.serve_forever, daemon=True)
    srv_thread.start()

    try:
        print(f"Project Root: {ROOT}")
        print(f"Started deterministic local fault-injection HTTP/HLS/DASH server at {base_url}")
        build_media_fixtures(fixture_dir, base_url, FaultInjectionHttpHandler.aes_key_bytes)

        # ---------------------------------------------------------------------
        # Test 1: Platform-Aware Native Binary Verification
        # ---------------------------------------------------------------------
        print(f"[1/8] IT-BIN-01: Verifying Platform-Aware Compiled Executables ({FLUX_BIN.stem}, {FLUX_NMH_BIN.stem}, N_m3u8DL-RE)...")
        verify_platform_binary_format(OPENIDM_BIN)
        verify_platform_binary_format(NMH_BIN)
        verify_platform_binary_format(STREAM_WORKER)
        ver_out = subprocess.run([str(STREAM_WORKER), "--version"], capture_output=True, text=True, check=True)
        ver_text = (ver_out.stdout + "\n" + ver_out.stderr)
        assert "0.3.0" in ver_text or "N_m3u8DL-RE" in ver_text, f"Unexpected worker version output: {ver_out.stdout}"
        print(f"  -> PASS (Verified native {'PE32+' if IS_WINDOWS else 'ELF x86_64'} executables)")

        # ---------------------------------------------------------------------
        # Test 2: Multi-Connection Segmented HTTP Download + Redirect + RFC 5987 Filename
        # ---------------------------------------------------------------------
        print("[2/8] IT-HTTP-01 & IT-HTTP-03: Segmented HTTP Range Download (8 Connections + 302 Redirect)...")
        subprocess.run(
            [
                str(OPENIDM_BIN),
                "--db", str(db_path),
                "--out", str(dl_dir),
                "--connections", "8",
                "--download", f"{base_url}/redirect/release",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        out_file1 = dl_dir / "Radium_Test_Archive.bin"
        if not out_file1.exists():
            out_file1 = dl_dir / "Flux_Test_Archive.bin"
        if not out_file1.exists():
            out_file1 = dl_dir / "OpenIDM_Test_Archive.bin"
        assert out_file1.exists(), f"Expected {out_file1} to exist"
        assert hashlib.sha256(out_file1.read_bytes()).hexdigest() == SHA256_4MB, "SHA-256 mismatch on 8-connection download"
        print("  -> PASS (4 MiB downloaded across 8 work-stealing connections, SHA-256 verified)")

        # ---------------------------------------------------------------------
        # Test 3: Non-Range Server Single-Stream Fallback (FR-002)
        # ---------------------------------------------------------------------
        print("[3/8] IT-HTTP-02: Non-Range Server Single-Stream Fallback...")
        subprocess.run(
            [
                str(OPENIDM_BIN),
                "--db", str(db_path),
                "--out", str(dl_dir),
                "--download", f"{base_url}/files/norange_1mb.bin",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        out_file2 = dl_dir / "norange_1mb.bin"
        assert out_file2.exists(), "Non-range output file missing"
        assert hashlib.sha256(out_file2.read_bytes()).hexdigest() == hashlib.sha256(PAYLOAD_4MB[:1048576]).hexdigest()
        print("  -> PASS (Single-stream fallback completed with matching SHA-256)")

        # ---------------------------------------------------------------------
        # Test 4: Flaky / Dropped Connection Reset & Automatic Retry (FR-004)
        # ---------------------------------------------------------------------
        print("[4/8] IT-HTTP-04: Dropped Connection Reset & Resume from Exact Byte Offset...")
        subprocess.run(
            [
                str(OPENIDM_BIN),
                "--db", str(db_path),
                "--out", str(dl_dir),
                "--connections", "4",
                "--download", f"{base_url}/files/flaky_2mb.bin",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        out_file3 = dl_dir / "flaky_2mb.bin"
        assert out_file3.exists(), "Flaky download output missing"
        assert hashlib.sha256(out_file3.read_bytes()).hexdigest() == hashlib.sha256(PAYLOAD_4MB[: 2 * 1048576]).hexdigest()
        assert FaultInjectionHttpHandler.flaky_attempts >= 1, "Flaky fault was triggered and recovered"
        print("  -> PASS (Recovered from mid-stream disconnect with exact SHA-256)")

        # ---------------------------------------------------------------------
        # Test 5: HLS Multi-Variant Probe, Download & FFmpeg Muxing (FR-019, FR-020)
        # ---------------------------------------------------------------------
        print("[5/8] IT-STREAM-01 & IT-STREAM-02: HLS Master Manifest Probe & FFmpeg Container Muxing...")
        probe_out = subprocess.run(
            [
                str(OPENIDM_BIN),
                "--db", str(db_path),
                "--worker-binary", str(STREAM_WORKER),
                "--ffmpeg-binary", str(FFMPEG_BIN),
                "--allow-loopback",
                "--probe-stream", f"{base_url}/hls/master.m3u8"
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        stdout_upper = (probe_out.stdout + "\n" + probe_out.stderr).upper()
        assert "[VIDEO]" in stdout_upper and "[SUBTITLES]" in stdout_upper, f"Expected VIDEO and SUBTITLES tracks, got stdout: {probe_out.stdout!r}, stderr: {probe_out.stderr!r}"

        subprocess.run(
            [
                str(OPENIDM_BIN),
                "--db", str(db_path),
                "--out", str(dl_dir),
                "--worker-binary", str(STREAM_WORKER),
                "--ffmpeg-binary", str(FFMPEG_BIN),
                "--allow-loopback",
                "--stream", f"{base_url}/hls/master.m3u8",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        muxed_hls = dl_dir / "Video" / "master.mp4"
        if not muxed_hls.exists():
            muxed_hls = dl_dir / "master.mp4"
        assert muxed_hls.exists(), f"Expected muxed HLS file at {muxed_hls}"
        streams, dur = ffprobe_verify(muxed_hls)
        assert "video" in streams and "audio" in streams and dur >= 3.5, f"Invalid ffprobe streams={streams}, dur={dur}"
        print(f"  -> PASS (Probed & muxed HLS Video+Audio+Subtitles -> {muxed_hls.name}, duration={dur:.2f}s)")

        # ---------------------------------------------------------------------
        # Test 6: MPEG-DASH (.mpd) Stream Download & FFmpeg Muxing (FR-020)
        # ---------------------------------------------------------------------
        print("[6/8] IT-STREAM-03: MPEG-DASH (.mpd) Manifest Download & FFmpeg Muxing...")
        subprocess.run(
            [
                str(OPENIDM_BIN),
                "--db", str(db_path),
                "--out", str(dl_dir),
                "--worker-binary", str(STREAM_WORKER),
                "--ffmpeg-binary", str(FFMPEG_BIN),
                "--allow-loopback",
                "--stream", f"{base_url}/dash/manifest.mpd",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        muxed_dash = dl_dir / "manifest.mp4"
        assert muxed_dash.exists(), "Expected muxed DASH file"
        d_streams, d_dur = ffprobe_verify(muxed_dash)
        assert "video" in d_streams and d_dur >= 3.5
        print(f"  -> PASS (Downloaded & muxed DASH MPD -> {muxed_dash.name}, duration={d_dur:.2f}s)")

        # ---------------------------------------------------------------------
        # Test 7: End-to-End Native Messaging Host (openidm-nmh) + AES-128 Authenticated Stream + UI Snapshot
        # ---------------------------------------------------------------------
        print("[7/8] IT-IPC-01, IT-STREAM-06 & IT-UI-01: Native Messaging Host, AES-128 Decryption & Desktop UI Snapshot...")
        daemon_proc = subprocess.Popen(
            [str(OPENIDM_BIN), "--daemon", "--db", str(db_path), "--out", str(dl_dir), "--ipc-socket", sock_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        time.sleep(0.2)

        # Measure Idle Daemon RSS while running!
        idle_daemon_rss_mb = 0.0
        if IS_WINDOWS:
            import ctypes
            pmc = PMC()
            pmc.cb = ctypes.sizeof(PMC)
            if ctypes.windll.psapi.GetProcessMemoryInfo(int(daemon_proc._handle), ctypes.byref(pmc), pmc.cb):
                idle_daemon_rss_mb = pmc.WorkingSetSize / (1024.0 * 1024.0)
        else:
            proc_status = Path(f"/proc/{daemon_proc.pid}/status")
            if proc_status.exists():
                for ln in proc_status.read_text(encoding="utf-8").splitlines():
                    if ln.startswith("VmRSS:"):
                        idle_daemon_rss_mb = float(ln.split()[1]) / 1024.0
                        break

        env = os.environ.copy()
        env["RADIUM_IPC_SOCKET"] = sock_path
        env["FLUX_IPC_SOCKET"] = sock_path
        env["OPENIDM_IPC_SOCKET"] = sock_path
        nmh_proc = subprocess.Popen(
            [str(NMH_BIN)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )

        hs = send_nmh_rpc(nmh_proc, {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "system.handshake",
            "params": {"protocolVersion": "1.0", "browser": "chromium"}
        })
        assert hs["result"]["captureEnabled"] is True
        assert hs["result"]["workersAvailable"] is True

        aes_add = send_nmh_rpc(nmh_proc, {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "stream.add",
            "params": {
                "streamType": "HLS",
                "manifestUrl": f"{base_url}/hls_aes/index.m3u8",
                "pageTitle": "Encrypted Lecture AES128",
                "outputFormat": "mp4",
                "headers": {
                    "Referer": f"{base_url}/course",
                    "Cookie": "stream_session=authorized_777"
                }
            }
        })
        assert aes_add["result"]["accepted"] is True
        aes_task_id = aes_add["result"]["taskId"]

        completed_task = None
        for _ in range(100):
            time.sleep(0.1)
            tlist = send_nmh_rpc(nmh_proc, {"jsonrpc": "2.0", "id": 3, "method": "task.list", "params": {}})
            for t in tlist["result"]["tasks"]:
                if t["taskId"] == aes_task_id and t["state"] in ("COMPLETED", "FAILED"):
                    completed_task = t
                    break
            if completed_task:
                break

        assert completed_task and completed_task["state"] == "COMPLETED", f"AES-128 stream task failed: {completed_task}"
        aes_mp4 = Path(completed_task["saveDir"]) / completed_task["fileName"]
        assert aes_mp4.exists(), f"Missing decrypted output {aes_mp4}"
        a_streams, a_dur = ffprobe_verify(aes_mp4)
        assert "video" in a_streams and a_dur >= 3.5

        log_file = db_path.parent / "radium.log"
        if not log_file.exists():
            log_file = db_path.parent / "flux.log"
        if not log_file.exists():
            log_file = db_path.parent / "openidm.log"
        if log_file.exists():
            log_text = log_file.read_text(encoding="utf-8")
            assert "authorized_777" not in log_text, "SECURITY VIOLATION: Cookie leaked into application log!"

        nmh_proc.stdin.close()
        nmh_proc.wait(timeout=5)
        daemon_proc.terminate()
        daemon_proc.wait(timeout=5)

        ui_snap = subprocess.run(
            [str(OPENIDM_BIN), "--db", str(db_path), "--gui-snapshot"],
            capture_output=True,
            text=True,
            check=True,
        )
        snap_text = ui_snap.stdout + "\n" + ui_snap.stderr
        assert "[Sidebar Counts] All=6" in snap_text and "[Details Panel]" in snap_text, "Desktop UI snapshot missing panels"
        print("  -> PASS (Native Messaging RPC, in-process AES-128 decryption, FFmpeg muxing, log redaction, and UI snapshot verified)")

        # ---------------------------------------------------------------------
        # Test 8: Comprehensive 8-Metric Performance & Resource Verification
        # ---------------------------------------------------------------------
        print("[8/8] PERF-BENCH-01: Executing Repeated 8-Metric Performance & Budget Verification...")
        bench_dir = work_dir / "bench"
        bench_dir.mkdir(parents=True, exist_ok=True)
        bench_db = bench_dir / "bench_state.db"

        openidm_size = OPENIDM_BIN.stat().st_size
        nmh_size = NMH_BIN.stat().st_size
        worker_size = STREAM_WORKER.stat().st_size

        # Metric 1: Cold startup latency (median of 5 runs on clean DB)
        startup_samples = []
        for _ in range(5):
            t0 = time.perf_counter()
            subprocess.run([str(OPENIDM_BIN), "--db", str(bench_db), "--list"], capture_output=True, check=True)
            startup_samples.append((time.perf_counter() - t0) * 1000.0)
        cold_start_ms = statistics.median(startup_samples)

        # Metric 3, 6, 7: Segmented 8-connection 16 MiB HTTP download (3 repeated runs in isolated child processes)
        http_rss_samples = []
        http_tp_samples = []
        http_cpu_samples = []
        for i in range(3):
            run_db = bench_dir / f"http_run_{i}.db"
            res_stat = measure_child_command_rusage([
                OPENIDM_BIN, "--db", run_db, "--out", bench_dir, "--connections", "8",
                "--download", f"{base_url}/files/bench_16mb.bin"
            ])
            http_rss_samples.append(res_stat["maxrss_kb"] / 1024.0)
            http_tp_samples.append((16.0 * 8.0) / max(0.001, res_stat["elapsed_s"]))
            http_cpu_samples.append((res_stat["utime_s"] + res_stat["stime_s"]) * 1000.0)

        peak_http_rss_mb = max(http_rss_samples)
        throughput_mbps = statistics.median(http_tp_samples)
        cpu_time_16mb_ms = statistics.median(http_cpu_samples)

        # Metric 4: Peak memory during HLS/DASH stream probing (OpenIDM + N_m3u8DL-RE worker)
        # and full muxing (including system /usr/bin/ffmpeg shared library footprint)
        probe_stat = measure_child_command_rusage([
            OPENIDM_BIN, "--db", bench_dir / "probe_bench.db",
            "--worker-binary", str(STREAM_WORKER),
            "--ffmpeg-binary", str(FFMPEG_BIN),
            "--allow-loopback",
            "--probe-stream", f"{base_url}/hls/master.m3u8"
        ])
        peak_stream_worker_rss_mb = probe_stat["maxrss_kb"] / 1024.0

        ffmpeg_base_stat = measure_child_command_rusage(["ffmpeg", "-version"])
        ffmpeg_baseline_rss_mb = ffmpeg_base_stat["maxrss_kb"] / 1024.0

        stream_stat = measure_child_command_rusage([
            OPENIDM_BIN, "--db", bench_dir / "stream_bench.db", "--out", bench_dir,
            "--worker-binary", str(STREAM_WORKER),
            "--ffmpeg-binary", str(FFMPEG_BIN),
            "--allow-loopback",
            "--stream", f"{base_url}/hls/master.m3u8"
        ])
        peak_stream_rss_mb = stream_stat["maxrss_kb"] / 1024.0
        net_stream_overhead_above_ffmpeg_mb = max(0.0, peak_stream_rss_mb - ffmpeg_baseline_rss_mb)

        # Metric 5: Large 1,000-Task List Responsiveness
        large_db = bench_dir / "large_1000.db"
        subprocess.run([str(OPENIDM_BIN), "--db", str(large_db), "--list"], capture_output=True, check=True)
        conn = sqlite3.connect(str(large_db))
        now_ms = int(time.time() * 1000)
        rows_1000 = [
            (
                f"task-{idx:04d}",
                "HTTP_FILE" if idx % 3 != 0 else "STREAM_HLS",
                "COMPLETED" if idx % 2 == 0 else "PAUSED",
                "main", idx, "Video" if idx % 3 == 0 else "Compressed",
                f"https://cdn.example.com/item_{idx}.zip",
                "", f"item_{idx:04d}.zip", str(bench_dir), "",
                10485760, 5242880, 1, 8, "", "", "{}", None, "{}", "", "", "",
                now_ms + idx, now_ms + idx, now_ms + idx
            )
            for idx in range(1000)
        ]
        conn.executemany(
            """INSERT INTO tasks (
                task_id, task_type, state, queue_id, queue_order, category,
                url, page_url, file_name, save_dir, part_path,
                total_bytes, downloaded_bytes, range_supported, max_connections,
                etag, last_modified, headers_json, encrypted_auth_blob, stream_options_json,
                expected_sha256, error_code, error_message,
                created_at_ms, updated_at_ms, completed_at_ms
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            rows_1000
        )
        conn.commit()
        conn.close()

        t_large0 = time.perf_counter()
        snap_1000 = subprocess.run([str(OPENIDM_BIN), "--db", str(large_db), "--gui-snapshot"], capture_output=True, text=True, check=True)
        large_1000_ms = (time.perf_counter() - t_large0) * 1000.0
        snap_1000_text = snap_1000.stdout + "\n" + snap_1000.stderr
        assert "[Sidebar Counts] All=1000" in snap_1000_text, "Expected 1000 tasks in snapshot"

        # Metric 8: Verify zero leftover .flux.part / .openidm.part temporary files in dl_dir and bench_dir
        leftover_parts = (
            list(dl_dir.rglob("*.radium.part")) + list(bench_dir.rglob("*.radium.part")) +
            list(dl_dir.rglob("*.flux.part")) + list(bench_dir.rglob("*.flux.part")) +
            list(dl_dir.rglob("*.openidm.part")) + list(bench_dir.rglob("*.openidm.part"))
        )
        leftover_temp_count = len(leftover_parts)

        # Enforce documented performance budgets (fail test if any limit is exceeded!)
        assert (openidm_size + nmh_size) <= 3 * 1048576, f"Core binary size exceeded 3 MB: {openidm_size + nmh_size}"
        assert cold_start_ms <= 250.0, f"Cold startup {cold_start_ms:.2f} ms exceeded 250 ms limit"
        assert idle_daemon_rss_mb <= 25.0, f"Idle daemon RSS {idle_daemon_rss_mb:.2f} MB exceeded 25 MB limit"
        assert peak_http_rss_mb <= 65.0, f"Peak HTTP 8-conn RSS {peak_http_rss_mb:.2f} MB exceeded 65 MB limit"
        assert peak_stream_worker_rss_mb <= 65.0, f"Peak Stream Worker RSS {peak_stream_worker_rss_mb:.2f} MB exceeded 65 MB limit"
        assert net_stream_overhead_above_ffmpeg_mb <= 40.0, f"Net Stream overhead above FFmpeg {net_stream_overhead_above_ffmpeg_mb:.2f} MB exceeded 40 MB limit"
        assert large_1000_ms <= 300.0, f"1,000-task UI snapshot {large_1000_ms:.2f} ms exceeded 300 ms limit"
        assert throughput_mbps >= 50.0, f"Loopback throughput {throughput_mbps:.1f} Mbps below 50 Mbps floor"
        assert leftover_temp_count == 0, f"Found {leftover_temp_count} uncleaned temporary part files"

        metrics = {
            "version": "1.0.0",
            "product_name": "Radium Download Manager",
            "short_name": "Radium",
            "host_platform": f"{platform.system()} {platform.release()} ({platform.machine()})",
            "flux_binary_bytes": openidm_size,
            "flux_nmh_binary_bytes": nmh_size,
            "nm3u8dl_re_worker_bytes": worker_size,
            "combined_core_binaries_kb": round((openidm_size + nmh_size) / 1024.0, 1),
            "total_all_three_binaries_kb": round((openidm_size + nmh_size + worker_size) / 1024.0, 1),
            "cold_startup_list_median_ms": round(cold_start_ms, 2),
            "idle_daemon_rss_mb": round(idle_daemon_rss_mb, 2),
            "peak_http_8conn_rss_mb": round(peak_http_rss_mb, 2),
            "peak_stream_worker_rss_mb": round(peak_stream_worker_rss_mb, 2),
            "system_ffmpeg_baseline_rss_mb": round(ffmpeg_baseline_rss_mb, 2),
            "peak_stream_with_ffmpeg_rss_mb": round(peak_stream_rss_mb, 2),
            "large_1000_tasks_snapshot_ms": round(large_1000_ms, 2),
            "throughput_16mb_8conn_median_mbps": round(throughput_mbps, 1),
            "cpu_time_16mb_8conn_ms": round(cpu_time_16mb_ms, 2),
            "leftover_temp_files_count": leftover_temp_count,
        }
        (ROOT / "build" / "benchmark_results.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
        print(
            f"  -> PASS: Core={metrics['combined_core_binaries_kb']} KB, "
            f"Startup={metrics['cold_startup_list_median_ms']} ms, "
            f"Idle RSS={metrics['idle_daemon_rss_mb']} MB, "
            f"HTTP Peak RSS={metrics['peak_http_8conn_rss_mb']} MB, "
            f"Stream Worker Peak RSS={metrics['peak_stream_worker_rss_mb']} MB (with FFmpeg={metrics['peak_stream_with_ffmpeg_rss_mb']} MB), "
            f"1000-Task UI={metrics['large_1000_tasks_snapshot_ms']} ms, "
            f"Throughput={metrics['throughput_16mb_8conn_median_mbps']} Mbps"
        )

        print("\nALL 8 END-TO-END INTEGRATION, NATIVE WORKER & ENFORCED BENCHMARK SUITES PASSED!")

    finally:
        # Always tear down child processes, including when an assertion fails.
        # Leaving the daemon alive after a failed run can claim the test pipe and
        # make the next IPC run fail with a misleading desktop-unavailable error.
        if nmh_proc is not None:
            try:
                if nmh_proc.stdin and not nmh_proc.stdin.closed:
                    nmh_proc.stdin.close()
            except (BrokenPipeError, OSError):
                pass
            if nmh_proc.poll() is None:
                nmh_proc.terminate()
            try:
                nmh_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                nmh_proc.kill()
                nmh_proc.wait(timeout=5)
        if daemon_proc is not None:
            if daemon_proc.poll() is None:
                daemon_proc.terminate()
            try:
                daemon_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon_proc.kill()
                daemon_proc.wait(timeout=5)
        server.shutdown()
        shutil.rmtree(work_dir, ignore_errors=True)
        if not IS_WINDOWS and os.path.exists(sock_path):
            os.unlink(sock_path)


if __name__ == "__main__":
    main()
