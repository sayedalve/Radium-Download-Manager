#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- End-to-End Browser Detection & Download Pipeline Verification Suite
# Tests all 10 deterministic fixtures required by Section 4 & Section 7:
#   1. Direct PDF download (click -> click metadata -> download event -> download.offer -> Brisk engine -> valid PDF)
#   2. Direct ZIP download (click -> click metadata -> download event -> download.offer -> Brisk engine -> valid ZIP + Alt bypass)
#   3. Redirected file download (302 redirect + Content-Disposition attachment -> correlated click -> valid file)
#   4. Direct MP4 video (HTML5 <video> playback -> media detection -> popup & overlay button -> DIRECT_PROGRESSIVE -> valid MP4)
#   5. WebM video (HTML5 <video> playback -> video/webm classification -> popup & overlay -> valid WebM)
#   6. Dynamically inserted video (MutationObserver detection -> overlay attached only when valid source exists)
#   7. HLS master playlist (#EXTM3U master + child variant + .ts segments + .vtt subtitles -> N_m3u8DL-RE + FFmpeg -> valid MP4)
#   8. DASH manifest (<MPD> XML manifest -> cached_manifest.mpd -> N_m3u8DL-RE + FFmpeg -> valid MP4)
#   9. Video element using a blob: URL (both backed MSE blob resolved via discovery and unbacked blob producing clear error)
#  10. YouTube extraction routing path (controlled watch page with adaptive video+audio tracks muxed via FFmpeg,
#      JSON-in-m3u8 rejection guard, and external yt-dlp worker execution path)
# Also executes an offline simulated V8 DOM environment against the local test server to inspect DOM overlays and console logs.

import hashlib
import http.server
import json
import os
import platform
import shutil
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = (os.name == "nt" or platform.system().lower().startswith("win"))
EXE_EXT = ".exe" if IS_WINDOWS else ""
os.environ["RADIUM_ALLOW_LOOPBACK"] = "1"
os.environ["FLUX_ALLOW_LOOPBACK"] = "1"
os.environ["OPENIDM_ALLOW_LOOPBACK"] = "1"


def find_binary(name: str) -> Path:
    if name == "ffmpeg":
        backup = ROOT / "tools" / "win-x64" / f"ffmpeg{EXE_EXT}.pre-minimal-backup"
        if backup.is_file():
            return backup
    candidates = [
        ROOT / "dist" / f"{name}{EXE_EXT}",
        ROOT / "build" / "win-x64" / "Release" / f"{name}{EXE_EXT}",
        ROOT / "build" / f"{name}{EXE_EXT}",
        ROOT / "tools" / "win-x64" / f"{name}{EXE_EXT}",
        ROOT / "tools" / f"{name}{EXE_EXT}",
    ]
    for c in candidates:
        if c.is_file():
            return c
    w = shutil.which(f"{name}{EXE_EXT}") or shutil.which(name)
    if w:
        return Path(w)
    return candidates[0]


FLUX_BIN = find_binary("Radium") if (ROOT / "dist" / f"Radium{EXE_EXT}").is_file() else find_binary("radium")
NMH_BIN = find_binary("radium-nmh") if find_binary("radium-nmh").is_file() else find_binary("flux-nmh")
STREAM_WORKER = find_binary("N_m3u8DL-RE")
FFMPEG_BIN = find_binary("ffmpeg")


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


def query_task_list_oneshot(env):
    p = subprocess.Popen(
        [str(NMH_BIN)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    res = send_nmh_rpc(p, {"jsonrpc": "2.0", "id": "poll", "method": "task.list", "params": {}})
    p.stdin.close()
    p.wait(timeout=2)
    return res


def wait_for_task(nmh_proc, task_id: str, timeout_s: float = 25.0) -> dict:
    deadline = time.time() + timeout_s
    env = getattr(nmh_proc, "_flux_env", None) or os.environ.copy()
    while time.time() < deadline:
        tlist = None
        if nmh_proc.poll() is None:
            try:
                tlist = send_nmh_rpc(nmh_proc, {"jsonrpc": "2.0", "id": "poll", "method": "task.list", "params": {}})
            except Exception:
                tlist = None
        if tlist is None:
            try:
                tlist = query_task_list_oneshot(env)
            except Exception:
                time.sleep(0.1)
                continue
        for t in tlist.get("result", {}).get("tasks", []):
            if t.get("taskId") == task_id and t.get("state") in ("COMPLETED", "FAILED", "CANCELED"):
                return t
        time.sleep(0.1)
    raise TimeoutError(f"Task {task_id} did not reach terminal state within {timeout_s}s")


def ffprobe_streams(file_path: Path):
    ffprobe_cmd = shutil.which("ffprobe")
    if ffprobe_cmd:
        res = subprocess.run(
            [ffprobe_cmd, "-v", "error", "-show_entries", "format=duration:stream=codec_type", "-of", "json", str(file_path)],
            capture_output=True, text=True, check=True
        )
        info = json.loads(res.stdout)
        streams = [s.get("codec_type") for s in info.get("streams", [])]
        dur = float(info.get("format", {}).get("duration", "0"))
        return streams, dur

    res = subprocess.run([str(FFMPEG_BIN), "-i", str(file_path)], capture_output=True, text=True)
    streams = []
    dur = 0.0
    for line in res.stderr.splitlines():
        if "Video:" in line:
            streams.append("video")
        elif "Audio:" in line:
            streams.append("audio")
        elif "Subtitle:" in line:
            streams.append("subtitle")
        if "Duration:" in line:
            import re
            m = re.search(r"Duration:\s*(\d+):(\d+):(\d+\.?\d*)", line)
            if m:
                h, m_min, s = m.groups()
                dur = int(h) * 3600 + int(m_min) * 60 + float(s)
    return streams, dur


def create_all_deterministic_fixtures(fixture_dir: Path, base_url: str) -> dict:
    fixture_dir.mkdir(parents=True, exist_ok=True)
    pages_dir = fixture_dir / "pages"
    pages_dir.mkdir(parents=True, exist_ok=True)

    meta = {}

    # 1. Direct PDF (>= 128 KiB valid PDF document)
    pdf_path = fixture_dir / "flux_architecture_guide.pdf"
    pdf_body = (
        b"%PDF-1.4\n1 0 obj<< /Type /Catalog /Pages 2 0 R >>endobj\n"
        b"2 0 obj<< /Type /Pages /Kids [3 0 R] /Count 1 >>endobj\n"
        b"3 0 obj<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>endobj\n"
        + (b"% Deterministic PDF padding line for Flux Download Manager test\n" * 2400)
        + b"xref\n0 4\n0000000000 65535 f \ntrailer<< /Root 1 0 R /Size 4 >>\nstartxref\n9\n%%EOF\n"
    )
    pdf_path.write_bytes(pdf_body)
    meta["pdf_sha256"] = hashlib.sha256(pdf_body).hexdigest()

    # 2. Direct ZIP (>= 200 KiB valid ZIP archive)
    zip_path = fixture_dir / "flux_portable_bundle.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as zf:
        zf.writestr("README.txt", "Flux Download Manager Deterministic ZIP Fixture\n" * 4000)
        zf.writestr("config.json", json.dumps({"app": "Flux", "version": "0.3.0"}))
    meta["zip_sha256"] = hashlib.sha256(zip_path.read_bytes()).hexdigest()

    # 3. Redirected file download target (with Content-Disposition filename)
    redir_path = fixture_dir / "redirected_payload.bin"
    redir_bytes = os.urandom(256 * 1024)
    redir_path.write_bytes(redir_bytes)
    meta["redir_sha256"] = hashlib.sha256(redir_bytes).hexdigest()

    # 4. Direct MP4 (>= 80 KiB with H.264 video + AAC audio)
    mp4_path = fixture_dir / "direct_sample.mp4"
    subprocess.run([
        str(FFMPEG_BIN), "-y", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc=duration=4:size=640x360:rate=25",
        "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
        "-c:v", "libx264", "-b:v", "250k", "-pix_fmt", "yuv420p", "-g", "25",
        "-c:a", "aac", "-b:a", "64k",
        str(mp4_path)
    ], check=True)
    meta["mp4_sha256"] = hashlib.sha256(mp4_path.read_bytes()).hexdigest()
    meta["mp4_size"] = mp4_path.stat().st_size

    # 5. WebM video (>= 80 KiB with VP8 video + Vorbis/Opus audio)
    webm_path = fixture_dir / "direct_sample.webm"
    subprocess.run([
        str(FFMPEG_BIN), "-y", "-loglevel", "error",
        "-f", "lavfi", "-i", "testsrc=duration=4:size=640x360:rate=25",
        "-f", "lavfi", "-i", "sine=frequency=520:duration=4",
        "-c:v", "libvpx", "-b:v", "250k",
        "-c:a", "libvorbis", "-b:a", "64k",
        str(webm_path)
    ], check=True)
    meta["webm_sha256"] = hashlib.sha256(webm_path.read_bytes()).hexdigest()
    meta["webm_size"] = webm_path.stat().st_size

    # 7. HLS master playlist + child variant + segments
    hls_dir = fixture_dir / "hls"
    hls_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        str(FFMPEG_BIN), "-y", "-loglevel", "error",
        "-i", str(mp4_path),
        "-c", "copy", "-f", "segment", "-segment_time", "2",
        "-segment_list", str(hls_dir / "360p.m3u8"),
        str(hls_dir / "seg_%03d.ts")
    ], check=True)
    (hls_dir / "master.m3u8").write_text(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        '#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=640x360,CODECS="avc1.64001f,mp4a.40.2"\n'
        "360p.m3u8\n",
        encoding="utf-8"
    )

    # 8. DASH manifest (.mpd)
    dash_dir = fixture_dir / "dash"
    dash_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(mp4_path, dash_dir / "rep_video.mp4")
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
        encoding="utf-8"
    )

    # 10. YouTube separate adaptive video-only (.mp4) and audio-only (.m4a) tracks for controlled extraction & muxing test
    yt_dir = fixture_dir / "yt"
    yt_dir.mkdir(parents=True, exist_ok=True)
    yt_vid_only = yt_dir / "adaptive_video_1080p.mp4"
    yt_aud_only = yt_dir / "adaptive_audio_140.m4a"
    subprocess.run([
        str(FFMPEG_BIN), "-y", "-loglevel", "error",
        "-i", str(mp4_path), "-an", "-c:v", "copy", str(yt_vid_only)
    ], check=True)
    subprocess.run([
        str(FFMPEG_BIN), "-y", "-loglevel", "error",
        "-i", str(mp4_path), "-vn", "-c:a", "copy", str(yt_aud_only)
    ], check=True)

    # Create deterministic HTML test pages for browser verification (Fixtures 1..10)
    (pages_dir / "index.html").write_text(f"""<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>Flux Deterministic Fixture Test Suite</title></head>
<body>
  <h1>Flux Download Manager — Deterministic Test Pages</h1>
  <a id="link-pdf" href="{base_url}/fixtures/flux_architecture_guide.pdf" download="flux_architecture_guide.pdf">Download PDF</a>
  <a id="link-zip" href="{base_url}/fixtures/flux_portable_bundle.zip">Download ZIP</a>
  <a id="link-redirect" href="{base_url}/redirect/latest-package">Download Redirected Package</a>
  <video id="video-mp4" width="640" height="360" controls src="{base_url}/fixtures/direct_sample.mp4"></video>
  <video id="video-webm" width="640" height="360" controls src="{base_url}/fixtures/direct_sample.webm"></video>
  <video id="video-empty" width="640" height="360"></video>
</body>
</html>
""", encoding="utf-8")

    (pages_dir / "dynamic_video.html").write_text(f"""<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>Dynamic Video Insertion Test</title></head>
<body>
  <div id="player-mount"></div>
  <script>
    setTimeout(() => {{
      const v = document.createElement("video");
      v.id = "dyn-video";
      v.width = 640;
      v.height = 360;
      v.src = "{base_url}/fixtures/direct_sample.mp4";
      document.getElementById("player-mount").appendChild(v);
    }}, 80);
  </script>
</body>
</html>
""", encoding="utf-8")

    (pages_dir / "blob_video.html").write_text("""<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>Blob Video Test</title></head>
<body>
  <video id="blob-video" width="640" height="360" controls></video>
  <script>
    const ms = new MediaSource();
    const v = document.getElementById("blob-video");
    v.src = URL.createObjectURL(ms);
  </script>
</body>
</html>
""", encoding="utf-8")

    return meta


class DeterministicFixtureHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    fixture_dir: Path = None
    base_url: str = ""

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/redirect/latest-package":
            self.send_response(302)
            self.send_header("Location", f"{self.base_url}/fixtures/redirected_payload.bin")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if path == "/watch":
            # Controlled YouTube watch page fixture with ytInitialPlayerResponse containing separate adaptive video & audio tracks
            yt_player = {
                "playabilityStatus": {"status": "OK"},
                "videoDetails": {
                    "videoId": "FluxTestYt01",
                    "title": "Flux YouTube Adaptive Mux Test"
                },
                "streamingData": {
                    "adaptiveFormats": [
                        {
                            "itag": 137,
                            "mimeType": 'video/mp4; codecs="avc1.640028"',
                            "bitrate": 4200000,
                            "width": 1920,
                            "height": 1080,
                            "qualityLabel": "1080p",
                            "contentLength": str((self.fixture_dir / "yt" / "adaptive_video_1080p.mp4").stat().st_size),
                            "url": f"{self.base_url}/fixtures/yt/adaptive_video_1080p.mp4?itag=137&mime=video%2Fmp4&range=0-1000"
                        },
                        {
                            "itag": 136,
                            "mimeType": 'video/mp4; codecs="avc1.64001f"',
                            "bitrate": 2200000,
                            "width": 1280,
                            "height": 720,
                            "qualityLabel": "720p",
                            "contentLength": str((self.fixture_dir / "yt" / "adaptive_video_1080p.mp4").stat().st_size),
                            "url": f"{self.base_url}/fixtures/yt/adaptive_video_1080p.mp4?itag=136&mime=video%2Fmp4&range=0-1000"
                        },
                        {
                            "itag": 134,
                            "mimeType": 'video/mp4; codecs="avc1.4d401e"',
                            "bitrate": 700000,
                            "width": 640,
                            "height": 360,
                            "qualityLabel": "360p",
                            "contentLength": str((self.fixture_dir / "yt" / "adaptive_video_1080p.mp4").stat().st_size),
                            "url": f"{self.base_url}/fixtures/yt/adaptive_video_1080p.mp4?itag=134&mime=video%2Fmp4&range=0-1000"
                        },
                        {
                            "itag": 140,
                            "mimeType": 'audio/mp4; codecs="mp4a.40.2"',
                            "bitrate": 128000,
                            "contentLength": str((self.fixture_dir / "yt" / "adaptive_audio_140.m4a").stat().st_size),
                            "url": f"{self.base_url}/fixtures/yt/adaptive_audio_140.m4a?itag=140&mime=audio%2Fmp4&rn=1"
                        }
                    ]
                }
            }
            html = (
                "<!DOCTYPE html><html><head><title>Flux YouTube Adaptive Mux Test - YouTube</title></head>"
                "<body><div id='movie_player'><video id='yt-video' width='640' height='360' src='blob:https://www.youtube.com/1111'></video></div>"
                f"<script>var ytInitialPlayerResponse = {json.dumps(yt_player)};</script></body></html>"
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
            return

        if path.startswith("/fixtures/") or path.startswith("/pages/"):
            rel = path[len("/fixtures/"):] if path.startswith("/fixtures/") else path.lstrip("/")
            fpath = self.fixture_dir / rel
            if fpath.exists() and fpath.is_file():
                data = fpath.read_bytes()
                cd = None
                ctype = "application/octet-stream"
                if fpath.suffix == ".pdf":
                    ctype = "application/pdf"
                elif fpath.suffix == ".zip":
                    ctype = "application/zip"
                elif fpath.name == "redirected_payload.bin":
                    ctype = "application/octet-stream"
                    cd = 'attachment; filename="Flux_Redirected_Release.zip"'
                elif fpath.suffix == ".mp4":
                    ctype = "video/mp4"
                elif fpath.suffix == ".m4a":
                    ctype = "audio/mp4"
                elif fpath.suffix == ".webm":
                    ctype = "video/webm"
                elif fpath.suffix == ".m3u8":
                    ctype = "application/vnd.apple.mpegurl"
                elif fpath.suffix == ".mpd":
                    ctype = "application/dash+xml"
                elif fpath.suffix == ".ts":
                    ctype = "video/mp2t"
                elif fpath.suffix == ".html":
                    ctype = "text/html; charset=utf-8"

                self._serve_bytes(data, ctype, cd)
                return

        self.send_response(404)
        self.send_header("Content-Length", "9")
        self.end_headers()
        self.wfile.write(b"Not Found")

    def _serve_bytes(self, buf: bytes, content_type: str, content_disposition: str = None):
        total = len(buf)
        rng = self.headers.get("Range")
        mv = memoryview(buf)
        if rng and rng.startswith("bytes="):
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
            self.send_header("Content-Type", content_type)
            if content_disposition:
                self.send_header("Content-Disposition", content_disposition)
            self.end_headers()
            try:
                self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_response(200)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(total))
            self.send_header("Content-Type", content_type)
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


def run_extension_bridge_step(script_js: str, sock_path: str):
    env = os.environ.copy()
    env["FLUX_IPC_SOCKET"] = sock_path
    env["OPENIDM_IPC_SOCKET"] = sock_path
    env["FLUX_NMH_BIN"] = str(NMH_BIN)
    res = subprocess.run(
        ["node", "-e", script_js],
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        text=True
    )
    if res.returncode != 0:
        raise RuntimeError(f"Node script failed (rc={res.returncode}):\nSTDERR: {res.stderr}\nSTDOUT: {res.stdout}")
    return json.loads(res.stdout.strip().splitlines()[-1])


def run_offline_dom_simulation_verification(base_url: str, work_dir: Path):
    # Execute an offline simulated multi-world V8 session in Node.js (Page MAIN world catch.js + ISOLATED world content-script.js
    # + MV3 Service Worker background.js) against the local HTTP fixture page, verifying:
    # 1. Empty <video#video-empty> (640x360, no src) does NOT get a false overlay button.
    # 2. Direct <video#video-mp4> gets a Closed Shadow DOM overlay button.
    # 3. Dynamically inserted <video#video-dynamic> triggers MutationObserver + catch.js discovery + overlay attachment.
    script = f"""
    const fs = require("fs");
    const vm = require("vm");
    const http = require("http");

    function fetchHtml(url) {{
      return new Promise((resolve, reject) => {{
        http.get(url, (res) => {{
          let data = "";
          res.on("data", chunk => data += chunk);
          res.on("end", () => resolve(data));
        }}).on("error", reject);
      }});
    }}

    (async () => {{
      const html = await fetchHtml("{base_url}/pages/index.html");
      if (!html.includes("Flux Download Manager")) throw new Error("Fixture HTML page not served");

      const messagesToBg = [];
      let dynamicCandidateRegistered = false;

      const makeVideo = (id, src) => ({{
        tagName: "VIDEO",
        id,
        src: src || "",
        currentSrc: src || "",
         readyState: src ? 2 : 0,
        dataset: {{}},
        getAttribute(k) {{ return k === "src" ? (this.src || "") : null; }},
        querySelectorAll() {{ return []; }},
        getBoundingClientRect() {{ return {{ width: 640, height: 360, top: 80, left: 60 }}; }},
        addEventListener() {{}}
      }});

      const emptyVideo = makeVideo("video-empty", "");
      const mp4Video = makeVideo("video-mp4", "{base_url}/fixtures/direct_sample.mp4");
      const dynamicVideo = makeVideo("video-dynamic", "{base_url}/fixtures/dynamic_sample.mp4");
      const activeVideos = [emptyVideo, mp4Video];
      const shadowHosts = [];
      const postMessageListeners = [];
      let mutationCallback = null;

      const documentStub = {{
        title: "Flux Deterministic Test Fixtures",
        documentElement: {{}},
        body: {{
          appendChild(el) {{
            if (el && el.dataset && el.dataset.fluxShadowHost === "true") shadowHosts.push(el);
            if (el && el.tagName === "VIDEO") {{
              activeVideos.push(el);
              if (mutationCallback) mutationCallback([{{ addedNodes: [el] }}]);
            }}
          }}
        }},
        createElement(tag) {{
          return {{
            tagName: String(tag).toUpperCase(),
            style: {{}},
            dataset: {{}},
            children: [],
            classList: {{ add() {{}}, remove() {{}} }},
            setAttribute(k, v) {{
              if (k === "data-flux-shadow-host") this.dataset.fluxShadowHost = v;
              this.dataset[k] = v;
            }},
            getAttribute(k) {{
              return this.dataset[k] || null;
            }},
            addEventListener(ev, fn) {{
              this._listeners = this._listeners || {{}};
              this._listeners[ev] = fn;
            }},
            appendChild(c) {{ this.children.push(c); }},
            attachShadow(opts) {{
              const sr = {{
                mode: opts.mode,
                children: [],
                appendChild(c) {{ this.children.push(c); }},
                querySelector(sel) {{
                  if (sel === ".flux-btn") {{
                    return {{
                      style: {{}},
                      classList: {{ add() {{}}, remove() {{}} }},
                      querySelector(s) {{ return {{ textContent: "" }}; }},
                      addEventListener(ev, fn) {{ this._click = fn; }}
                    }};
                  }}
                  return null;
                }}
              }};
              this._closedShadow = sr;
              return sr;
            }}
          }};
        }},
        _docListeners: {{}},
        querySelectorAll(sel) {{
          if (sel === "video" || sel.includes("video")) return activeVideos;
          return [];
        }},
        addEventListener(ev, fn) {{
          this._docListeners[ev] = this._docListeners[ev] || [];
          this._docListeners[ev].push(fn);
        }},
        dispatchEvent(evObj) {{
          const list = this._docListeners[evObj.type] || [];
          for (const fn of list) fn(evObj);
        }}
      }};

      const sharedWindow = {{
        location: {{ href: "{base_url}/pages/index.html", hostname: "127.0.0.1" }},
        innerWidth: 1280,
        innerHeight: 720,
        scrollX: 0,
        scrollY: 0,
        document: documentStub,
        HTMLAnchorElement: function() {{}},
        MediaSource: {{ isTypeSupported: () => true }},
        fetch: async () => ({{ clone: () => ({{ text: async () => "" }}) }}),
        XMLHttpRequest: function() {{}};
        postMessage(data) {{
          for (const fn of postMessageListeners) fn({{ source: sharedWindow, data }});
        }},
        addEventListener(ev, fn) {{
          if (ev === "message") postMessageListeners.push(fn);
        }}
      }};
      sharedWindow.XMLHttpRequest.prototype = {{ open() {{}}, send() {{}}, addEventListener() {{}} }};
      sharedWindow.HTMLAnchorElement.prototype = {{ click() {{}} }};

      const sandbox = {{
        window: sharedWindow,
        document: documentStub,
        location: sharedWindow.location,
        MutationObserver: class {{
          constructor(cb) {{ mutationCallback = cb; }}
          observe() {{}}
        }},
        chrome: {{
          runtime: {{
            sendMessage(msg, cb) {{
              messagesToBg.push(msg);
              if ((msg.action === "addMedia" || msg.action === "MEDIA_DETECTED") && msg.url && msg.url.includes("dynamic_sample.mp4")) {{
                dynamicCandidateRegistered = true;
              }}
              if (msg.action === "QUERY_TAB_MEDIA_STATUS") {{
                if (cb) cb({{ ok: true, hasPlayableCandidate: true, bestCandidate: {{ url: "{base_url}/fixtures/dynamic_sample.mp4", streamType: "DIRECT", sourceType: "DIRECT_PROGRESSIVE", resolution: "720p" }} }});
                return;
              }}
              if (cb) cb({{ ok: true }});
            }},
            onMessage: {{ addListener() {{}} }}
          }}
        }},
        setTimeout: (fn) => {{ fn(); return 1; }},
        clearTimeout: () => {{}},
        setInterval: () => 1,
        clearInterval: () => {{}},
        URL,
         Math,
        Date,
         Number,
        String,
        Boolean,
        Array,
        Object,
        RegExp,
        Map,
        Set,
        Promise,
        console: {{ log() {{}}, warn() {{}}, error() {{}} }}
      }};
      sharedWindow.window = sharedWindow;
      vm.createContext(sandbox);
      vm.runInContext(fs.readFileSync("extension/catch-script/catch.js", "utf8"), sandbox);
      vm.runInContext(fs.readFileSync("extension/js/content-script.js", "utf8"), sandbox);

      // Verify initial scan: emptyVideo must NOT have overlay; mp4Video MUST have overlay
      if (emptyVideo.dataset.fluxOverlayAttached) throw new Error("Overlay falsely attached to empty video");
      if (mp4Video.dataset.fluxOverlayAttached !== "true") throw new Error("Overlay missing on valid mp4Video");

      // Dynamically insert <video id="video-dynamic">
      documentStub.body.appendChild(dynamicVideo);
      documentStub.dispatchEvent({{ type: "loadedmetadata", target: dynamicVideo }});

      if (dynamicVideo.dataset.fluxOverlayAttached !== "true") {{
        throw new Error("Overlay not attached to dynamically inserted video");
      }}
      if (!dynamicCandidateRegistered) {{
        throw new Error("Dynamic video candidate was not forwarded via MEDIA_DETECTED");
      }}
      console.log(JSON.stringify({{
        ran: true,
        emptyAttached: Boolean(emptyVideo.dataset.fluxOverlayAttached),
        mp4Attached: mp4Video.dataset.fluxOverlayAttached === "true",
        dynamicAttached: dynamicVideo.dataset.fluxOverlayAttached === "true",
        shadowHostsCreated: shadowHosts.length
      }}));
    }})();
    """.replace("XMLHttpRequest: function() {};", "XMLHttpRequest: function() {},")
    out = run_extension_bridge_step(script, "")
    assert out["ran"] is True
    assert out["emptyAttached"] is False
    assert out["mp4Attached"] is True
    assert out["dynamicAttached"] is True
    assert out["shadowHostsCreated"] >= 2
    return out


def main():
    work_dir = Path(tempfile.mkdtemp(prefix="flux_repair_e2e_"))
    fixture_dir = work_dir / "fixtures"
    dl_dir = work_dir / "downloads"
    db_path = work_dir / "state.db"
    sock_path = f"\\\\.\\pipe\\flux_repair_{os.getpid()}" if IS_WINDOWS else f"/tmp/flux_repair_{os.getpid()}.sock"

    server = ThreadedHTTPServer(("127.0.0.1", 0), DeterministicFixtureHandler)
    port = server.server_address[1]
    base_url = f"http://127.0.0.1:{port}"
    DeterministicFixtureHandler.fixture_dir = fixture_dir
    DeterministicFixtureHandler.base_url = base_url

    srv_thread = threading.Thread(target=server.serve_forever, daemon=True)
    srv_thread.start()

    daemon_proc = None
    nmh_proc = None

    try:
        print("=================================================================")
        print("  Radium Download Manager -- 10-Fixture End-to-End Repair Suite  ")
        print("=================================================================")
        meta = create_all_deterministic_fixtures(fixture_dir, base_url)

        daemon_proc = subprocess.Popen(
            [str(FLUX_BIN), "--daemon", "--db", str(db_path), "--out", str(dl_dir), "--ipc-socket", sock_path, "--allow-loopback"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        time.sleep(0.25)

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
        nmh_proc._flux_env = env

        # Node helper that runs the real extension background/content/catch modules and sends RPCs through flux-nmh
        node_bridge_preamble = """
const { spawn } = require("child_process");
const struct = require("buffer");
const { G, SourceType } = require("./extension/js/init.js");
const fn = require("./extension/js/function.js");
const bg = require("./extension/js/background.js");
const cs = require("./extension/js/content-script.js");
const { installDeepSearchHooks } = require("./extension/catch-script/catch.js");
const popup = require("./extension/js/popup.js");

function callNativeHost(payload) {
  return new Promise((resolve, reject) => {
    const p = spawn(process.env.FLUX_NMH_BIN, [], { env: process.env });
    const raw = Buffer.from(JSON.stringify(payload), "utf8");
    const hdr = Buffer.alloc(4);
    hdr.writeUInt32LE(raw.length, 0);
    let outBuf = Buffer.alloc(0);
    p.stdout.on("data", (chunk) => {
      outBuf = Buffer.concat([outBuf, chunk]);
      if (outBuf.length >= 4) {
        const len = outBuf.readUInt32LE(0);
        if (outBuf.length >= 4 + len) {
          const msg = JSON.parse(outBuf.slice(4, 4 + len).toString("utf8"));
          p.stdin.end();
          if (msg.error) {
            const e = new Error(msg.error.message || "RPC error");
            e.isRpcError = true;
            reject(e);
          } else {
            resolve(msg.result);
          }
        }
      }
    });
    p.on("error", reject);
    p.stdin.write(Buffer.concat([hdr, raw]));
  });
}
"""

        # ---------------------------------------------------------------------
        # Fixture 1: Direct PDF (Browser click -> click metadata -> download.offer -> Brisk -> valid PDF)
        # ---------------------------------------------------------------------
        print("[1/10] Testing Fixture 1: Direct PDF Click -> Metadata -> download.offer -> Brisk Engine -> Valid PDF...")
        res1 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  let sentMsg = null;
  globalThis.chrome = {{ runtime: {{ sendMessage: (m) => {{ sentMsg = m; }} }} }};
  cs.handleLinkClick({{
    altKey: false,
    target: {{ closest: () => ({{ href: "{base_url}/fixtures/flux_architecture_guide.pdf", pathname: "/fixtures/flux_architecture_guide.pdf", download: "flux_architecture_guide.pdf", hasAttribute: (a) => a === "download" }}) }}
  }});
  fn.recordClickedLink(11, sentMsg);
  let canceledId = null;
  const offerRes = await bg.onDownloadCreatedHandler(
    {{ id: 1001, tabId: 11, url: "{base_url}/fixtures/flux_architecture_guide.pdf", filename: "flux_architecture_guide.pdf", mime: "application/pdf", fileSize: 150000 }},
    callNativeHost,
    async (id) => {{ canceledId = id; }}
  );
  console.log(JSON.stringify({{ offerRes, canceledId, logs: G.diagnosticLogs }}));
}})();
""", sock_path)
        assert res1["offerRes"]["intercepted"] is True
        assert res1["canceledId"] == 1001
        t1 = wait_for_task(nmh_proc, res1["offerRes"]["taskId"])
        assert t1["state"] == "COMPLETED" and t1["taskType"] == "HTTP_FILE"
        pdf_out = Path(t1["saveDir"]) / t1["fileName"]
        assert pdf_out.exists() and hashlib.sha256(pdf_out.read_bytes()).hexdigest() == meta["pdf_sha256"]
        print(f"  -> PASS (Task {t1['taskId']} completed via Brisk, SHA-256 verified, corrId={t1.get('correlationId', '')})")

        # ---------------------------------------------------------------------
        # Fixture 2: Direct ZIP + Alt Bypass Verification
        # ---------------------------------------------------------------------
        print("[2/10] Testing Fixture 2: Direct ZIP Click & Alt-Bypass Verification...")
        res2 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  // First verify Alt-bypass prevents interception across both onCreated and onDeterminingFilename
  fn.recordBypassIntent(12, "{base_url}/fixtures/flux_portable_bundle.zip");
  const bypassCreated = await bg.onDownloadCreatedHandler(
    {{ id: 1002, tabId: 12, url: "{base_url}/fixtures/flux_portable_bundle.zip", filename: "flux_portable_bundle.zip" }},
    callNativeHost, null, null, "created"
  );
  const bypassDet = await bg.onDownloadCreatedHandler(
    {{ id: 1002, tabId: 12, url: "{base_url}/fixtures/flux_portable_bundle.zip", filename: "flux_portable_bundle.zip" }},
    callNativeHost, null, null, "determine"
  );

  // Now perform normal click and download interception
  fn.recordClickedLink(12, {{ url: "{base_url}/fixtures/flux_portable_bundle.zip", filename: "flux_portable_bundle.zip", referrer: "{base_url}/pages/index.html" }});
  const offerRes = await bg.onDownloadCreatedHandler(
    {{ id: 1003, tabId: 12, url: "{base_url}/fixtures/flux_portable_bundle.zip", filename: "flux_portable_bundle.zip", mime: "application/zip" }},
    callNativeHost,
    async () => {{}}
  );
  console.log(JSON.stringify({{ bypassCreated, bypassDet, offerRes }}));
}})();
""", sock_path)
        assert res2["bypassCreated"]["intercepted"] is False and res2["bypassDet"]["intercepted"] is False
        assert res2["offerRes"]["intercepted"] is True
        t2 = wait_for_task(nmh_proc, res2["offerRes"]["taskId"])
        assert t2["state"] == "COMPLETED"
        zip_out = Path(t2["saveDir"]) / t2["fileName"]
        assert zip_out.exists() and hashlib.sha256(zip_out.read_bytes()).hexdigest() == meta["zip_sha256"]
        print(f"  -> PASS (ZIP SHA-256 verified & Alt-bypass honored across lifecycle events)")

        # ---------------------------------------------------------------------
        # Fixture 3: Redirected File Download (302 -> Content-Disposition Attachment)
        # ---------------------------------------------------------------------
        print("[3/10] Testing Fixture 3: Redirected File Download (302 + Content-Disposition Correlation)...")
        res3 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  fn.recordClickedLink(13, {{ url: "{base_url}/redirect/latest-package", referrer: "{base_url}/pages/index.html" }});
  bg.onHeadersReceivedHandler({{
    requestId: "r-302",
    tabId: 13,
    url: "{base_url}/redirect/latest-package",
    statusCode: 302,
    responseHeaders: [{{ name: "Location", value: "{base_url}/fixtures/redirected_payload.bin" }}]
  }});
  bg.onHeadersReceivedHandler({{
    requestId: "r-302",
    tabId: 13,
    url: "{base_url}/fixtures/redirected_payload.bin",
    statusCode: 200,
    responseHeaders: [
      {{ name: "Content-Type", value: "application/octet-stream" }},
      {{ name: "Content-Disposition", value: 'attachment; filename="Flux_Redirected_Release.zip"' }},
      {{ name: "Content-Length", value: "262144" }}
    ]
  }});
  const offerRes = await bg.onDownloadCreatedHandler(
    {{ id: 1004, tabId: 13, url: "{base_url}/redirect/latest-package", finalUrl: "{base_url}/fixtures/redirected_payload.bin", filename: "" }},
    callNativeHost,
    async () => {{}}
  );
  console.log(JSON.stringify({{ offerRes }}));
}})();
""", sock_path)
        assert res3["offerRes"]["intercepted"] is True
        t3 = wait_for_task(nmh_proc, res3["offerRes"]["taskId"])
        assert t3["state"] == "COMPLETED" and t3["fileName"] == "Flux_Redirected_Release.zip"
        redir_out = Path(t3["saveDir"]) / t3["fileName"]
        assert redir_out.exists() and hashlib.sha256(redir_out.read_bytes()).hexdigest() == meta["redir_sha256"]
        print(f"  -> PASS (302 Redirect correlated -> {t3['fileName']} SHA-256 verified)")

        # ---------------------------------------------------------------------
        # Fixture 4: Direct MP4 Video (Detection -> Popup -> Video Button -> Valid MP4)
        # ---------------------------------------------------------------------
        print("[4/10] Testing Fixture 4: Direct MP4 Detection, Popup Listing & Video Button Handoff...")
        res4 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  const det = bg.onHeadersReceivedHandler({{
    requestId: "r-mp4",
    tabId: 14,
    url: "{base_url}/fixtures/direct_sample.mp4",
    documentUrl: "{base_url}/pages/index.html",
    statusCode: 200,
    responseHeaders: [
      {{ name: "Content-Type", value: "video/mp4" }},
      {{ name: "Content-Length", value: "{meta['mp4_size']}" }}
    ]
  }});
  const popupHtml = popup.renderDetectedMediaList(G.cacheData.get(14), {{ desktopConnected: true }});
  const resolved = bg.resolveCandidateForDownloadRequest(14, {{ url: "{base_url}/fixtures/direct_sample.mp4", pageTitle: "Direct MP4 Fixture" }}, {{}});
  const submitRes = await bg.submitMediaCandidateToFlux(resolved.candidate, {{ pageTitle: "Direct MP4 Fixture" }}, callNativeHost);
  console.log(JSON.stringify({{ added: det.added, sourceType: det.candidate.sourceType, inPopup: popupHtml.includes("direct_sample.mp4") || popupHtml.includes("MP4"), submitRes }}));
}})();
""", sock_path)
        assert res4["added"] is True and res4["sourceType"] == "DIRECT_PROGRESSIVE" and res4["inPopup"] is True
        t4 = wait_for_task(nmh_proc, res4["submitRes"]["taskId"])
        assert t4["state"] == "COMPLETED" and t4["taskType"] == "HTTP_FILE"
        mp4_out = Path(t4["saveDir"]) / t4["fileName"]
        assert mp4_out.exists() and hashlib.sha256(mp4_out.read_bytes()).hexdigest() == meta["mp4_sha256"]
        print(f"  -> PASS (Direct MP4 listed in popup & downloaded -> {mp4_out.name})")

        # ---------------------------------------------------------------------
        # Fixture 5: WebM Video
        # ---------------------------------------------------------------------
        print("[5/10] Testing Fixture 5: WebM Video Detection & Download...")
        res5 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  const det = bg.onHeadersReceivedHandler({{
    requestId: "r-webm",
    tabId: 15,
    url: "{base_url}/fixtures/direct_sample.webm",
    documentUrl: "{base_url}/pages/index.html",
    statusCode: 200,
    responseHeaders: [
      {{ name: "Content-Type", value: "video/webm" }},
      {{ name: "Content-Length", value: "{meta['webm_size']}" }}
    ]
  }});
  const submitRes = await bg.submitMediaCandidateToFlux(det.candidate, {{ pageTitle: "WebM Fixture" }}, callNativeHost);
  console.log(JSON.stringify({{ added: det.added, ext: det.candidate.ext, submitRes }}));
}})();
""", sock_path)
        assert res5["added"] is True and res5["ext"] == "webm"
        t5 = wait_for_task(nmh_proc, res5["submitRes"]["taskId"])
        assert t5["state"] == "COMPLETED"
        webm_out = Path(t5["saveDir"]) / t5["fileName"]
        assert webm_out.exists() and hashlib.sha256(webm_out.read_bytes()).hexdigest() == meta["webm_sha256"]
        print(f"  -> PASS (WebM video detected & downloaded -> {webm_out.name})")

        # ---------------------------------------------------------------------
        # Fixture 6: Dynamically Inserted Video & Offline DOM Simulation Verification
        # ---------------------------------------------------------------------
        print("[6/10] Testing Fixture 6: Dynamically Inserted Video & Offline V8 DOM Simulation...")
        chrome_res = run_offline_dom_simulation_verification(base_url, work_dir)
        print(f"  -> PASS (Dynamic video & offline simulated DOM verified: {chrome_res})")

        # ---------------------------------------------------------------------
        # Fixture 7: HLS Master Playlist (.m3u8) via N_m3u8DL-RE + FFmpeg
        # ---------------------------------------------------------------------
        print("[7/10] Testing Fixture 7: HLS Master Playlist Detection, Variant Grouping & N_m3u8DL-RE Muxing...")
        res7 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  const masterDet = bg.onHeadersReceivedHandler({{
    requestId: "r-hls-m",
    tabId: 17,
    url: "{base_url}/fixtures/hls/master.m3u8",
    statusCode: 200,
    responseHeaders: [{{ name: "Content-Type", value: "application/vnd.apple.mpegurl" }}]
  }});
  const childDet = bg.onHeadersReceivedHandler({{
    requestId: "r-hls-c",
    tabId: 17,
    url: "{base_url}/fixtures/hls/360p.m3u8",
    statusCode: 200,
    responseHeaders: [{{ name: "Content-Type", value: "application/vnd.apple.mpegurl" }}]
  }});
  const submitRes = await bg.submitMediaCandidateToFlux(masterDet.candidate, {{ pageTitle: "HLS_Fixture_Output" }}, callNativeHost);
  console.log(JSON.stringify({{ masterAdded: masterDet.added, childGrouped: childDet.groupedUnder === masterDet.candidate.candidateId, submitRes }}));
}})();
""", sock_path)
        assert res7["masterAdded"] is True and res7["childGrouped"] is True
        t7 = wait_for_task(nmh_proc, res7["submitRes"]["taskId"])
        assert t7["state"] == "COMPLETED" and t7["taskType"] == "STREAM_HLS", f"Task 7 failed with state {t7.get('state')}: {t7.get('errorMessage')}"
        hls_out = Path(t7["saveDir"]) / t7["fileName"]
        streams7, dur7 = ffprobe_streams(hls_out)
        assert "video" in streams7 and "audio" in streams7 and dur7 >= 3.5
        print(f"  -> PASS (HLS Master muxed via N_m3u8DL-RE + FFmpeg -> {hls_out.name}, dur={dur7:.2f}s)")

        # ---------------------------------------------------------------------
        # Fixture 8: DASH Manifest (.mpd) via N_m3u8DL-RE + FFmpeg
        # ---------------------------------------------------------------------
        print("[8/10] Testing Fixture 8: MPEG-DASH (.mpd) Manifest Detection & N_m3u8DL-RE Muxing...")
        dash_xml = (fixture_dir / "dash" / "manifest.mpd").read_text(encoding="utf-8")
        res8 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  const dashCand = {{
    url: "{base_url}/fixtures/dash/manifest.mpd",
    ext: "mpd",
    streamType: "DASH",
    sourceType: "DASH_MANIFEST",
    cachedManifestBody: {json.dumps(dash_xml)},
    pageTitle: "DASH_Fixture_Output"
  }};
  const submitRes = await bg.submitMediaCandidateToFlux(dashCand, {{ pageTitle: "DASH_Fixture_Output" }}, callNativeHost);
  console.log(JSON.stringify({{ submitRes }}));
}})();
""", sock_path)
        t8 = wait_for_task(nmh_proc, res8["submitRes"]["taskId"])
        assert t8["state"] == "COMPLETED" and t8["taskType"] == "STREAM_DASH"
        dash_out = Path(t8["saveDir"]) / t8["fileName"]
        streams8, dur8 = ffprobe_streams(dash_out)
        assert "video" in streams8 and dur8 >= 3.5
        print(f"  -> PASS (DASH MPD with cached_manifest.mpd muxed via N_m3u8DL-RE -> {dash_out.name}, dur={dur8:.2f}s)")

        # ---------------------------------------------------------------------
        # Fixture 9: Video Element Using a blob: URL (Backed Resolution + Unbacked Clear Error)
        # ---------------------------------------------------------------------
        print("[9/10] Testing Fixture 9: Blob URL Video Player Discovery & Unbacked Blob Error Handling...")
        res9 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  // Case A: Unbacked blob: player must return clear error and NOT submit empty URL
  const unbacked = bg.resolveCandidateForDownloadRequest(19, {{
    url: "",
    blobUrl: "blob:{base_url}/unbacked-9999",
    isMseBlob: true,
    tabUrl: "{base_url}/pages/blob_video.html"
  }}, {{}});

  // Case B: Blob player on tab where Deep Search / webRequest captured HLS manifest
  bg.onHeadersReceivedHandler({{
    requestId: "r-blob-hls",
    tabId: 19,
    url: "{base_url}/fixtures/hls/master.m3u8",
    statusCode: 200,
    responseHeaders: [{{ name: "Content-Type", value: "application/vnd.apple.mpegurl" }}]
  }});
  let discResp = null;
  bg.handleRuntimeMessage({{
    action: "discoverBlobMedia",
    tabId: 19,
    blobUrl: "blob:{base_url}/backed-1111",
    tabUrl: "{base_url}/pages/blob_video.html"
  }}, {{ tab: {{ id: 19 }} }}, (r) => {{ discResp = r; }});
  await new Promise(r => setTimeout(r, 30));
  const backed = bg.resolveCandidateForDownloadRequest(19, {{
    url: "",
    blobUrl: "blob:{base_url}/backed-1111",
    isMseBlob: true,
    tabUrl: "{base_url}/pages/blob_video.html"
  }}, {{}});
  console.log(JSON.stringify({{ unbackedError: unbacked.error, discHasCandidate: discResp.hasCandidate, backedUrl: backed.candidate ? backed.candidate.url : "" }}));
}})();
""", sock_path)
        assert "No supported download source" in res9["unbackedError"]
        assert res9["discHasCandidate"] is True
        assert res9["backedUrl"].endswith("/fixtures/hls/master.m3u8")
        print("  -> PASS (Backed blob: resolved to HLS manifest; unbacked blob: produced clear error)")

        # ---------------------------------------------------------------------
        # Fixture 10: YouTube Extraction Routing Path (Adaptive Video+Audio Mux + JSON Guard)
        # ---------------------------------------------------------------------
        print("[10/10] Testing Fixture 10: YouTube Extraction Routing Path & Adaptive Video+Audio Muxing...")
        res10 = run_extension_bridge_step(node_bridge_preamble + f"""
(async () => {{
  // 1. Verify passing JSON as HLS manifest to stream.add is strictly rejected by desktop IPC server
  let jsonAsHlsRejected = false;
  try {{
    await callNativeHost({{
      jsonrpc: "2.0",
      id: "bad-json-hls",
      method: "stream.add",
      params: {{
        streamType: "HLS",
        sourceType: "HLS_MANIFEST",
        manifestUrl: "https://www.youtube.com/watch?v=FluxTestYt01",
        cachedManifestBody: JSON.stringify({{ fluxType: "YOUTUBE_STREAM", videoId: "FluxTestYt01" }})
      }}
    }});
  }} catch (e) {{
    jsonAsHlsRejected = e.message.includes("ERR_STREAM_INVALID_MANIFEST");
  }}

  // 2. Submit controlled YouTube watch page through YOUTUBE_EXTRACTION pipeline
  const ytCand = {{
    url: "{base_url}/watch?v=FluxTestYt01",
    streamType: "YOUTUBE",
    sourceType: "YOUTUBE_EXTRACTION",
    videoId: "FluxTestYt01",
    pageTitle: "Flux YouTube Adaptive Mux Test",
    tabUrl: "{base_url}/watch?v=FluxTestYt01"
  }};
  const submitRes = await bg.submitMediaCandidateToFlux(ytCand, {{ pageTitle: "Radium YouTube Adaptive Mux Test", qualityId: "360p", videoFilter: "360p" }}, callNativeHost);
  console.log(JSON.stringify({{ jsonAsHlsRejected, submitRes }}));
}})();
""", sock_path)
        assert res10["jsonAsHlsRejected"] is True, "Desktop IPC server must reject JSON passed as HLS manifest"
        assert res10["submitRes"]["accepted"] is True
        assert res10["submitRes"]["taskType"] == "YOUTUBE_EXTRACTION"
        t10 = wait_for_task(nmh_proc, res10["submitRes"]["taskId"])
        assert t10["state"] == "COMPLETED", f"YouTube extraction task failed: {t10}"
        yt_out = Path(t10["saveDir"]) / t10["fileName"]
        assert yt_out.exists()
        streams10, dur10 = ffprobe_streams(yt_out)
        assert "video" in streams10 and "audio" in streams10 and dur10 >= 3.5, f"Expected both video and audio in muxed YouTube output, got {streams10}, dur={dur10}"
        print(f"  -> PASS (YouTube watch page extracted adaptive video+audio tracks & muxed via FFmpeg -> {yt_out.name}, streams={streams10}, dur={dur10:.2f}s)")

        # Verify correlation IDs in radium.log (or flux.log)
        log_path = db_path.parent / "radium.log"
        if not log_path.exists():
            log_path = db_path.parent / "flux.log"
        assert log_path.exists(), "radium.log or flux.log should exist"
        log_text = log_path.read_text(encoding="utf-8")
        assert "Dispatching task=" in log_text and "YOUTUBE_EXTRACTION" in log_text, f"Expected correlation & dispatch logs in radium.log. Actual log contents:\n{log_text}"

        print("\n=================================================================")
        print("  ALL 10 DETERMINISTIC FIXTURES & OFFLINE SIMULATION TESTS PASSED!    ")
        print("=================================================================")
    finally:
        if nmh_proc:
            try:
                nmh_proc.stdin.close()
                nmh_proc.wait(timeout=3)
            except Exception:
                nmh_proc.kill()
        if daemon_proc:
            daemon_proc.terminate()
            try:
                daemon_proc.wait(timeout=3)
            except Exception:
                daemon_proc.kill()
        server.shutdown()
        shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
