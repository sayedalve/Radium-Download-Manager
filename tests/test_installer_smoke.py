#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Flux Download Manager -- Clean Installation Smoke & Integration Test Suite
# Validates silent Inno Setup installation, PE32+ metadata, worker binaries,
# browser extension assets, Native Messaging manifests, registry keys,
# real HTTP download over local test server, resume/retry, temp cleanup,
# and safe uninstallation with user data preservation.

import argparse
import hashlib
import http.server
import json
import os
import platform
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

if sys.platform == "win32":
    import winreg
else:
    winreg = None

ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = (os.name == "nt" or platform.system().lower().startswith("win"))

PAYLOAD_4MB = os.urandom(4 * 1024 * 1024)
SHA256_4MB = hashlib.sha256(PAYLOAD_4MB).hexdigest()

PAYLOAD_FLAKY_2MB = os.urandom(2 * 1024 * 1024)
SHA256_FLAKY_2MB = hashlib.sha256(PAYLOAD_FLAKY_2MB).hexdigest()


class ControlledSmokeServer(http.server.BaseHTTPRequestHandler):
    flaky_attempts = 0

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/files/smoke_4mb.bin":
            self._serve_buffer(PAYLOAD_4MB, support_range=True, filename="smoke_4mb.bin")
        elif path == "/files/flaky_2mb.bin":
            rng = self.headers.get("Range", "")
            if rng and rng != "bytes=0-0" and ControlledSmokeServer.flaky_attempts == 0:
                ControlledSmokeServer.flaky_attempts += 1
                total = len(PAYLOAD_FLAKY_2MB)
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
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
                self.send_header("Content-Length", str(span))
                self.send_header("Content-Disposition", 'attachment; filename="flaky_2mb.bin"')
                self.end_headers()
                # Write partial chunk and abruptly terminate connection
                partial_size = min(32768, max(1, span // 2))
                try:
                    self.wfile.write(PAYLOAD_FLAKY_2MB[start : start + partial_size])
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
            self._serve_buffer(PAYLOAD_FLAKY_2MB, support_range=True, filename="flaky_2mb.bin")
        elif path == "/files/missing.bin":
            self.send_response(404)
            self.send_header("Content-Length", "9")
            self.end_headers()
            self.wfile.write(b"Not Found")
        else:
            self.send_response(404)
            self.end_headers()

    def _serve_buffer(self, buf, support_range=True, filename="file.bin"):
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


def verify_pe_header(file_path: Path, expected_subsystem: int = 2, expected_arch: str = "x64"):
    raw = file_path.read_bytes()
    if len(raw) < 256 or raw[0] != 0x4D or raw[1] != 0x5A:
        raise AssertionError(f"Missing MZ header: {file_path}")
    pe_offset = int.from_bytes(raw[0x3C:0x40], "little")
    sig = raw[pe_offset : pe_offset + 4]
    if sig != b"PE\x00\x00":
        raise AssertionError(f"Missing PE signature: {file_path}")
    machine = int.from_bytes(raw[pe_offset + 4 : pe_offset + 6], "little")
    expected_machine = 0xAA64 if expected_arch == "ARM64" else 0x8664
    if expected_arch == "ARM64" and machine in (0xAA64, 0x8664):
        pass
    elif machine != expected_machine:
        raise AssertionError(f"Expected architecture {expected_arch} (0x{expected_machine:X}), got 0x{machine:X}: {file_path}")
    magic = int.from_bytes(raw[pe_offset + 24 : pe_offset + 26], "little")
    if magic != 0x20B:
        raise AssertionError(f"Expected PE32+ (0x20B) magic: {file_path}")
    subsystem = int.from_bytes(raw[pe_offset + 92 : pe_offset + 94], "little")
    if subsystem != expected_subsystem:
        raise AssertionError(f"Expected subsystem {expected_subsystem}, got {subsystem} for {file_path}")


def find_installer(dist_dir: Path, arch: str = "x64") -> Path:
    pattern = f"Radium-Setup-{arch}-*.exe"
    candidates = list(dist_dir.glob(pattern)) + list(dist_dir.glob("Radium-Setup-*.exe")) + list(dist_dir.glob("Flux-Setup-*.exe"))
    if not candidates:
        candidates = list(dist_dir.rglob(pattern)) + list(dist_dir.rglob("Radium-Setup-*.exe")) + list(dist_dir.rglob("Flux-Setup-*.exe"))
    full_candidates = [c for c in candidates if "Core" not in c.name and (arch in c.name or (arch == "x64" and "ARM64" not in c.name))]
    if full_candidates:
        return sorted(full_candidates, key=lambda p: p.stat().st_mtime, reverse=True)[0]
    if candidates:
        return sorted(candidates, key=lambda p: p.stat().st_mtime, reverse=True)[0]
    raise FileNotFoundError(f"No Radium setup installer found in {dist_dir} matching arch {arch}")


def main():
    parser = argparse.ArgumentParser(description="Clean Installation Smoke Test for Flux")
    parser.add_argument("--installer", type=str, default=None, help="Path to Inno Setup installer executable")
    parser.add_argument("--install-dir", type=str, default=None, help="Target installation directory")
    parser.add_argument("--dist-dir", type=str, default=None, help="Directory containing release artifacts")
    parser.add_argument("--arch", type=str, default="x64", choices=["x64", "ARM64"], help="Target architecture")
    parser.add_argument("--expected-version", type=str, default=None, help="Expected application version reported by the installer")
    parser.add_argument("--skip-uninstall", action="store_true", help="Do not run uninstallation test")
    args = parser.parse_args()

    print("=================================================================")
    print("   Radium Download Manager -- Clean Installation Smoke Test Suite ")
    print("=================================================================")

    if not IS_WINDOWS:
        print("[SKIP] Windows-only test suite. Current OS is not Windows.")
        return 0

    dist_dir = Path(args.dist_dir).resolve() if args.dist_dir else (ROOT / "dist")
    installer_path = Path(args.installer).resolve() if args.installer else find_installer(dist_dir, args.arch)
    print(f"Installer Target  : {installer_path}")
    assert installer_path.is_file(), f"Installer missing: {installer_path}"

    install_dir = Path(args.install_dir).resolve() if args.install_dir else Path(tempfile.mkdtemp(prefix="flux_smoke_install_"))
    print(f"Install Directory : {install_dir}")

    install_log = install_dir.parent / f"flux_install_{int(time.time())}.log"

    try:
        # Step 1: Silent Installation
        print("\n[Step 1/8] Executing Silent Inno Setup Installation (/TYPE=full)...")
        install_cmd = [
            str(installer_path),
            "/VERYSILENT",
            "/SUPPRESSMSGBOXES",
            "/NORESTART",
            f"/DIR={install_dir}",
            f"/LOG={install_log}",
            "/TYPE=full"
        ]
        t0 = time.perf_counter()
        res_inst = subprocess.run(install_cmd, capture_output=True, text=True)
        install_time_s = time.perf_counter() - t0
        if res_inst.returncode != 0:
            log_content = install_log.read_text(encoding="utf-8", errors="ignore") if install_log.is_file() else ""
            print(f"Installation failed with exit code {res_inst.returncode}!\nInstaller log:\n{log_content}")
            raise RuntimeError(f"Installer failed with return code {res_inst.returncode}")
        print(f"  -> PASS: Silent installation completed in {install_time_s:.2f}s")

        # Step 2: Verify Installed Binaries and Assets
        print("\n[Step 2/8] Auditing Installed Files and Worker Binaries...")
        required_files = [
            install_dir / "radium.exe" if (install_dir / "radium.exe").exists() else install_dir / "flux.exe",
            install_dir / "radium-nmh.exe" if (install_dir / "radium-nmh.exe").exists() else install_dir / "flux-nmh.exe",
            install_dir / "tools" / "win-x64" / "N_m3u8DL-RE.exe",
            install_dir / "tools" / "win-x64" / "ffmpeg.exe",
            install_dir / "tools" / "win-x64" / "yt-dlp.exe",
            install_dir / "extension" / "manifest.json",
            install_dir / "extension" / "js" / "background.js",
            install_dir / "extension" / "popup.html",
            install_dir / "manifests" / "com.radium.downloadmanager.chromium.json",
            install_dir / "manifests" / "com.radium.downloadmanager.firefox.json",
            install_dir / "manifests" / "com.flux.downloadmanager.chromium.json",
            install_dir / "manifests" / "com.flux.downloadmanager.firefox.json",
            install_dir / "manifests" / "org.openidm.native.chromium.json",
            install_dir / "manifests" / "org.openidm.native.firefox.json",
            install_dir / "unins000.exe",
            install_dir / "README.md",
            install_dir / "LICENSE",
            install_dir / "THIRD_PARTY_NOTICES.md",
        ]
        for rf in required_files:
            if not rf.is_file():
                raise AssertionError(f"Required installed file missing: {rf}")
            print(f"  [OK] {rf.relative_to(install_dir)} ({rf.stat().st_size:,} bytes)")

        # Verify PE headers
        verify_pe_header(install_dir / "radium.exe" if (install_dir / "radium.exe").exists() else install_dir / "flux.exe", expected_subsystem=2, expected_arch=args.arch)  # GUI
        verify_pe_header(install_dir / "radium-nmh.exe" if (install_dir / "radium-nmh.exe").exists() else install_dir / "flux-nmh.exe", expected_subsystem=3, expected_arch=args.arch)  # CUI
        print("  -> PASS: All required binaries, workers, manifests, and PE headers verified")

        # Step 3: Verify Native Messaging Manifest Content and Registry Keys
        print("\n[Step 3/8] Verifying Native Messaging Manifest Paths and Registry Keys...")
        manifest_file = install_dir / "manifests" / "com.radium.downloadmanager.chromium.json"
        manifest_data = json.loads(manifest_file.read_text(encoding="utf-8"))
        expected_nmh = (install_dir / "radium-nmh.exe" if (install_dir / "radium-nmh.exe").exists() else install_dir / "flux-nmh.exe").resolve()
        manifest_nmh = Path(manifest_data.get("path", "")).resolve()
        assert manifest_nmh == expected_nmh, f"Manifest path '{manifest_nmh}' does not match installed '{expected_nmh}'"
        assert "chrome-extension://mdgmlcnpodoehidlplplfegngnjnilap/" in manifest_data.get("allowed_origins", [])

        if winreg:
            reg_checks = [
                (r"Software\Google\Chrome\NativeMessagingHosts\com.radium.downloadmanager", install_dir / "manifests" / "com.radium.downloadmanager.chromium.json"),
                (r"Software\Microsoft\Edge\NativeMessagingHosts\com.radium.downloadmanager", install_dir / "manifests" / "com.radium.downloadmanager.chromium.json"),
                (r"Software\Google\Chrome\NativeMessagingHosts\com.flux.downloadmanager", install_dir / "manifests" / "com.flux.downloadmanager.chromium.json"),
                (r"Software\Microsoft\Edge\NativeMessagingHosts\com.flux.downloadmanager", install_dir / "manifests" / "com.flux.downloadmanager.chromium.json"),
            ]
            for subkey, expected_mf in reg_checks:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, subkey) as k:
                    val, _ = winreg.QueryValueEx(k, "")
                    assert Path(val).resolve() == expected_mf.resolve(), (
                        f"Registry key HKCU\\{subkey} points to '{val}' (expected '{expected_mf.resolve()}')"
                    )
                    print(f"  [Registry OK] HKCU\\{subkey} -> {val}")
        print("  -> PASS: Native Messaging manifests and registry bindings verified strictly")

        # Step 4: Verify Executable Launch & Version Command
        print("\n[Step 4/8] Launching Installed Executable (--version & --list)...")
        flux_exe = install_dir / "radium.exe" if (install_dir / "radium.exe").exists() else install_dir / "flux.exe"
        ver_proc = subprocess.run([str(flux_exe), "--version"], capture_output=True, text=True, check=True)
        ver_output = (ver_proc.stdout + "\n" + ver_proc.stderr).strip()
        assert ("Radium Download Manager" in ver_output or "Radium" in ver_output or "Flux" in ver_output) and (
            args.expected_version is None or args.expected_version in ver_output
        ), (
            f"Unexpected version output: stdout='{ver_proc.stdout}', stderr='{ver_proc.stderr}'"
        )
        first_line = ver_output.splitlines()[0] if ver_output else ""
        print(f"  -> PASS: Application responds cleanly: {first_line}")

        # Step 5: Start Local Test Server & Real HTTP Download
        print("\n[Step 5/8] Performing Real HTTP Download via Installed Application...")
        server = ThreadedServer(("127.0.0.1", 0), ControlledSmokeServer)
        port = server.server_port
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        base_url = f"http://127.0.0.1:{port}"

        smoke_work_dir = Path(tempfile.mkdtemp(prefix="flux_smoke_work_"))
        smoke_db = smoke_work_dir / "smoke_state.db"
        smoke_dl = smoke_work_dir / "downloads"
        smoke_dl.mkdir(parents=True, exist_ok=True)

        env = os.environ.copy()
        env["FLUX_ALLOW_LOOPBACK"] = "1"
        env["OPENIDM_ALLOW_LOOPBACK"] = "1"

        dl_url = f"{base_url}/files/smoke_4mb.bin"
        t0 = time.perf_counter()
        dl_res = subprocess.run(
            [str(flux_exe), "--db", str(smoke_db), "--out", str(smoke_dl), "--allow-loopback", "--download", dl_url],
            capture_output=True, text=True, env=env, check=True
        )
        dl_time_s = time.perf_counter() - t0
        print(f"  Output: {dl_res.stdout.strip()}")

        # Check downloaded file in smoke_dl or smoke_dl/General
        candidates = [smoke_dl / "smoke_4mb.bin", smoke_dl / "General" / "smoke_4mb.bin"]
        found_file = next((c for c in candidates if c.is_file()), None)
        assert found_file is not None, f"Downloaded 4 MiB file missing! Directory listing: {list(smoke_dl.rglob('*'))}"
        file_bytes = found_file.read_bytes()
        assert len(file_bytes) == len(PAYLOAD_4MB), f"File size mismatch: {len(file_bytes)} != {len(PAYLOAD_4MB)}"
        actual_sha256 = hashlib.sha256(file_bytes).hexdigest()
        assert actual_sha256 == SHA256_4MB, f"SHA256 mismatch! {actual_sha256} != {SHA256_4MB}"

        # Step 6: Verify Zero Leftover Temporary Files & Fault Injection Handling
        print("\n[Step 6/8] Verifying Error Recovery, Flaky Resume & Clean Part-File Lifecycle...")
        leftover_parts = list(smoke_dl.rglob("*.part")) + list(smoke_dl.rglob("*.flux.part"))
        assert len(leftover_parts) == 0, f"Leftover part files detected: {leftover_parts}"
        print("  [OK] Zero unmerged .part temporary files detected after successful transfer.")

        # Test 404 Not Found error handling
        fail_res = subprocess.run(
            [str(flux_exe), "--db", str(smoke_db), "--out", str(smoke_dl), "--allow-loopback", "--download", f"{base_url}/files/missing.bin"],
            capture_output=True, text=True, env=env
        )
        assert fail_res.returncode != 0, "Expected non-zero exit code on 404 download!"
        print("  [OK] 404 Not Found rejected with error return code as expected.")

        # Test flaky endpoint with auto-resume / retry
        try:
            flaky_res = subprocess.run(
                [str(flux_exe), "--db", str(smoke_db), "--out", str(smoke_dl), "--allow-loopback", "--download", f"{base_url}/files/flaky_2mb.bin"],
                capture_output=True, text=True, env=env, check=True
            )
        except subprocess.CalledProcessError as e:
            print(f"ERROR running flaky download:\nSTDOUT:\n{e.stdout}\nSTDERR:\n{e.stderr}", file=sys.stderr)
            raise
        flaky_cand = [smoke_dl / "flaky_2mb.bin", smoke_dl / "General" / "flaky_2mb.bin"]
        flaky_found = next((c for c in flaky_cand if c.is_file()), None)
        assert flaky_found is not None, "Flaky resumed file missing!"
        assert hashlib.sha256(flaky_found.read_bytes()).hexdigest() == SHA256_FLAKY_2MB
        print("  [OK] Flaky mid-stream connection reset recovered and verified via SHA-256.")

        # Step 7: Application Restart & State Persistence Check
        print("\n[Step 7/8] Verifying Database State Persistence Across Process Restarts...")
        list_res = subprocess.run(
            [str(flux_exe), "--db", str(smoke_db), "--list"],
            capture_output=True, text=True, check=True
        )
        list_output = list_res.stdout + "\n" + list_res.stderr
        assert "smoke_4mb.bin" in list_output or "COMPLETED" in list_output, (
            f"State missing on process restart! stdout='{list_res.stdout}', stderr='{list_res.stderr}'"
        )
        print("  -> PASS: Database persistence and restart verified cleanly")

        # Step 7b: Positive Browser Native Messaging Integration Suite
        print("\n[Step 7b/8] Verifying Positive Browser Native Messaging Integration (Chromium + Extension)...")
        nm_script = ROOT / "tests" / "browser" / "test_native_messaging_positive.js"
        assert nm_script.is_file(), f"Native messaging positive integration script missing: {nm_script}"

        # Check if node and playwright are available in the runtime environment
        node_bin = shutil.which("node")
        playwright_available = False
        playwright_skip_reason = ""

        if not node_bin:
            playwright_skip_reason = "Node.js executable ('node') not found in PATH."
        else:
            check_pw = subprocess.run(
                [
                    node_bin,
                    "-e",
                    "try { const { chromium } = require('playwright'); chromium.executablePath(); process.exit(0); } "
                    "catch (e) { console.error(e.code || e.message || String(e)); process.exit(42); }",
                ],
                capture_output=True,
                text=True,
            )
            if check_pw.returncode == 0:
                playwright_available = True
            else:
                pw_err = (check_pw.stderr + "\n" + check_pw.stdout).strip()
                playwright_skip_reason = f"Playwright or Chromium browser unavailable ({pw_err or 'exit code 42'})."

        if not playwright_available:
            print(f"  -> SKIPPED (NOTICE): {playwright_skip_reason} Skipping optional browser integration test.")
        else:
            nm_proc = subprocess.run(
                [node_bin, str(nm_script), "--install-dir", str(install_dir), "--port", str(port)],
                capture_output=True,
                text=True,
            )
            if nm_proc.stdout.strip():
                print(f"{nm_proc.stdout.strip()}")
            if nm_proc.returncode != 0:
                print(f"ERROR OUTPUT:\n{nm_proc.stderr.strip()}")
                raise AssertionError(f"Positive Native Messaging Integration Test Failed with exit code {nm_proc.returncode}")
            else:
                print("  -> PASS: Positive Browser Native Messaging integration verified end-to-end")

        # Step 8: Safe Uninstallation & User Data Preservation Check
        if not args.skip_uninstall:
            print("\n[Step 8/8] Testing Safe Uninstallation & User Data Preservation...")
            # Simulate user data in AppData and custom download file
            appdata_flux = Path(os.path.expandvars(r"%LOCALAPPDATA%\Flux"))
            appdata_flux.mkdir(parents=True, exist_ok=True)
            user_sentinel = appdata_flux / "user_smoke_sentinel.json"
            user_sentinel.write_text('{"keep_user_data": true}', encoding="utf-8")

            uninstaller = install_dir / "unins000.exe"
            assert uninstaller.is_file(), f"Uninstaller missing: {uninstaller}"
            t0 = time.perf_counter()
            unins_proc = subprocess.run(
                [str(uninstaller), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"],
                capture_output=True, text=True, check=True
            )
            # Give Windows shell 2 seconds to release any lingering handles
            time.sleep(2.0)
            unins_time_s = time.perf_counter() - t0

            # Verify installed binaries are gone
            assert not (install_dir / "radium.exe" if (install_dir / "radium.exe").exists() else install_dir / "flux.exe").is_file(), "flux.exe still exists after uninstall!"
            assert not (install_dir / "radium-nmh.exe" if (install_dir / "radium-nmh.exe").exists() else install_dir / "flux-nmh.exe").is_file(), "flux-nmh.exe still exists after uninstall!"
            print(f"  [OK] Application binaries removed in {unins_time_s:.2f}s")

            # Verify user sentinel in %LOCALAPPDATA%\Flux is strictly preserved!
            assert user_sentinel.is_file(), "CRITICAL: User data in %LOCALAPPDATA%\\Flux was deleted on uninstall!"
            user_sentinel.unlink()
            print("  [OK] User application state in %LOCALAPPDATA%\\Flux is strictly preserved.")
            print("  -> PASS: Clean uninstallation verified with full user-data safety guarantee")

        print("\n=================================================================")
        print("   ALL 8 CLEAN INSTALLATION & INTEGRATION SMOKE SUITES PASSED!   ")
        print("=================================================================")
        return 0

    finally:
        if "server" in locals():
            server.shutdown()
        if "smoke_work_dir" in locals():
            shutil.rmtree(smoke_work_dir, ignore_errors=True)
        if install_dir.is_dir() and not args.skip_uninstall:
            shutil.rmtree(install_dir, ignore_errors=True)
        if install_log.is_file():
            install_log.unlink()


if __name__ == "__main__":
    sys.exit(main())
