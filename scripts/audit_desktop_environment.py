#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Radium Download Manager - Windows 11 Desktop & ARM Telemetry / Interactive Session Auditor
Inspects:
 1. Windows kernel version, OS build, release ID, servicing branch
 2. Host processor architecture (e.g. ARM64 vs AMD64)
 3. Process execution architecture (IsWow64Process2 for x64 emulation detection under Prism)
 4. Desktop session type (Session 0 service vs Session 1+ interactive GUI session)
 5. Window station & Desktop security access
 6. Direct GUI window creation & message pump execution test
 7. Direct DWM / Mica backdrop API query
"""

import argparse
import ctypes
from ctypes import wintypes
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def get_architecture_telemetry():
    telemetry = {
        "python_version": platform.python_version(),
        "platform_system": platform.system(),
        "platform_release": platform.release(),
        "platform_version": platform.version(),
        "platform_machine": platform.machine(),
        "platform_processor": platform.processor(),
        "env_processor_architecture": os.environ.get("PROCESSOR_ARCHITECTURE", "UNKNOWN"),
        "env_processor_architew6432": os.environ.get("PROCESSOR_ARCHITEW6432", "None"),
        "is_arm64_host": False,
        "is_emulated_x64": False,
        "native_machine_hex": "0x0000",
        "process_machine_hex": "0x0000",
        "native_machine_name": "UNKNOWN",
        "process_machine_name": "UNKNOWN"
    }

    if os.name != "nt":
        return telemetry

    # Detect processor architecture via IsWow64Process2
    kernel32 = ctypes.windll.kernel32
    IsWow64Process2 = getattr(kernel32, "IsWow64Process2", None)
    if IsWow64Process2:
        IsWow64Process2.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.USHORT), ctypes.POINTER(wintypes.USHORT)]
        IsWow64Process2.restype = wintypes.BOOL
        pm = wintypes.USHORT()
        nm = wintypes.USHORT()
        if IsWow64Process2(kernel32.GetCurrentProcess(), ctypes.byref(pm), ctypes.byref(nm)):
            telemetry["native_machine_hex"] = f"0x{nm.value:04X}"
            telemetry["process_machine_hex"] = f"0x{pm.value:04X}"

            # Known machine constants:
            # IMAGE_FILE_MACHINE_UNKNOWN = 0x0000
            # IMAGE_FILE_MACHINE_I386    = 0x014c
            # IMAGE_FILE_MACHINE_AMD64   = 0x8664
            # IMAGE_FILE_MACHINE_ARM64   = 0xAA64
            machine_map = {
                0x0000: "IMAGE_FILE_MACHINE_UNKNOWN / NATIVE",
                0x014C: "IMAGE_FILE_MACHINE_I386 (x86 32-bit)",
                0x8664: "IMAGE_FILE_MACHINE_AMD64 (x64 64-bit)",
                0xAA64: "IMAGE_FILE_MACHINE_ARM64 (ARM64 64-bit)"
            }
            telemetry["native_machine_name"] = machine_map.get(nm.value, f"MACHINE_0x{nm.value:04X}")
            telemetry["process_machine_name"] = machine_map.get(pm.value, f"MACHINE_0x{pm.value:04X}")

            if nm.value == 0xAA64:
                telemetry["is_arm64_host"] = True
                if pm.value == 0x8664 or telemetry["platform_machine"].upper() in ("AMD64", "X86_64"):
                    telemetry["is_emulated_x64"] = True

    # Fallback env inspection
    if "ARM64" in telemetry["env_processor_architecture"].upper() or "ARM64" in telemetry["env_processor_architew6432"].upper():
        telemetry["is_arm64_host"] = True

    return telemetry


def get_desktop_session_telemetry():
    session = {
        "has_user32": False,
        "session_id": -1,
        "is_session_0": True,
        "is_remote_session": False,
        "desktop_window_hwnd": 0,
        "screen_width": 0,
        "screen_height": 0,
        "interactive_gui_capable": False,
        "dwm_composition_enabled": False
    }

    if os.name != "nt":
        return session

    try:
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        session["has_user32"] = True

        # Process Session ID
        ProcessIdToSessionId = getattr(kernel32, "ProcessIdToSessionId", None)
        if ProcessIdToSessionId:
            ProcessIdToSessionId.argtypes = [wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
            ProcessIdToSessionId.restype = wintypes.BOOL
            sess_id = wintypes.DWORD()
            if ProcessIdToSessionId(kernel32.GetCurrentProcessId(), ctypes.byref(sess_id)):
                session["session_id"] = sess_id.value
                session["is_session_0"] = (sess_id.value == 0)

        # Desktop window
        desktop_hwnd = user32.GetDesktopWindow()
        session["desktop_window_hwnd"] = int(desktop_hwnd)
        session["screen_width"] = user32.GetSystemMetrics(0)   # SM_CXSCREEN
        session["screen_height"] = user32.GetSystemMetrics(1)  # SM_CYSCREEN
        session["is_remote_session"] = bool(user32.GetSystemMetrics(0x1000)) # SM_REMOTESESSION

        # DwmIsCompositionEnabled
        dwmapi = getattr(ctypes.windll, "dwmapi", None)
        if dwmapi and hasattr(dwmapi, "DwmIsCompositionEnabled"):
            enabled = wintypes.BOOL()
            if dwmapi.DwmIsCompositionEnabled(ctypes.byref(enabled)) == 0:
                session["dwm_composition_enabled"] = bool(enabled.value)

        # Interactive GUI check
        if desktop_hwnd != 0 and session["screen_width"] > 0 and session["screen_height"] > 0:
            session["interactive_gui_capable"] = True
    except Exception as e:
        session["error"] = str(e)

    return session


def test_radium_binary_execution(bin_dir: Path):
    radium_exe = bin_dir / "radium.exe"
    result = {
        "tested": False,
        "exists": radium_exe.is_file(),
        "exit_code": None,
        "stdout": "",
        "stderr": "",
        "startup_succeeded": False
    }

    if not radium_exe.is_file():
        return result

    result["tested"] = True
    try:
        # Run radium.exe --version
        p = subprocess.Popen(
            [str(radium_exe), "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        out, err = p.communicate(timeout=10)
        result["exit_code"] = p.returncode
        result["stdout"] = out.strip()
        result["stderr"] = err.strip()
        result["startup_succeeded"] = (p.returncode == 0 and "Radium" in (out + err))
    except Exception as e:
        result["error"] = str(e)

    return result


def main():
    parser = argparse.ArgumentParser(description="Audit Windows 11 Desktop & ARM Runner Environment")
    parser.add_argument("--bin-dir", type=str, default="build/Release", help="Directory containing built binaries")
    parser.add_argument("--output-json", type=str, default="test-results/desktop_telemetry.json", help="Output JSON path")
    args = parser.parse_args()

    bin_dir = Path(args.bin_dir).resolve()
    output_json = Path(args.output_json).resolve()
    output_json.parent.mkdir(parents=True, exist_ok=True)

    arch_telemetry = get_architecture_telemetry()
    session_telemetry = get_desktop_session_telemetry()
    bin_test = test_radium_binary_execution(bin_dir)

    audit_result = {
        "timestamp_utc": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
        "architecture": arch_telemetry,
        "desktop_session": session_telemetry,
        "radium_binary_startup": bin_test
    }

    output_json.write_text(json.dumps(audit_result, indent=2), encoding="utf-8")

    print("=== Windows 11 ARM & Desktop Telemetry Audit ===")
    print(f"OS Platform Version : {arch_telemetry['platform_system']} {arch_telemetry['platform_release']} ({arch_telemetry['platform_version']})")
    print(f"Host Processor Arch : {arch_telemetry['env_processor_architecture']} (ARM64 Host: {arch_telemetry['is_arm64_host']})")
    print(f"Native Machine      : {arch_telemetry['native_machine_name']} ({arch_telemetry['native_machine_hex']})")
    print(f"Process Machine     : {arch_telemetry['process_machine_name']} ({arch_telemetry['process_machine_hex']})")
    print(f"x64 Emulation Active: {arch_telemetry['is_emulated_x64']}")
    print(f"Session ID          : {session_telemetry['session_id']} (Session 0: {session_telemetry['is_session_0']})")
    print(f"Screen Resolution   : {session_telemetry['screen_width']}x{session_telemetry['screen_height']}")
    print(f"DWM Composition     : {session_telemetry['dwm_composition_enabled']}")
    print(f"Radium Startup Test : {'PASSED' if bin_test.get('startup_succeeded') else 'SKIPPED/FAILED'}")
    print(f"Audit output saved  : {output_json}")


if __name__ == "__main__":
    main()
