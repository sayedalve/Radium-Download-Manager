// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Post-Download Antivirus Scanner Integration
#include "security/antivirus_scanner.hpp"

#include <chrono>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

namespace openidm::security {

namespace {

#ifdef _WIN32
Result<std::pair<int, std::string>> RunProcessCaptureOutput(
    const std::wstring& application_name,
    const std::wstring& command_line,
    int timeout_ms) {

    HANDLE hStdOutRead = nullptr, hStdOutWrite = nullptr;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) {
        return Result<std::pair<int, std::string>>::Err("ERR_PIPE", "Failed to create pipe for antivirus stdout");
    }
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hStdOutWrite;
    si.hStdError = hStdOutWrite;

    PROCESS_INFORMATION pi = {0};
    std::vector<wchar_t> cmd_buf(command_line.begin(), command_line.end());
    cmd_buf.push_back(L'\0');

    LPCWSTR app = application_name.empty() ? nullptr : application_name.c_str();

    BOOL ok = CreateProcessW(
        app, cmd_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(hStdOutWrite);

    if (!ok) {
        CloseHandle(hStdOutRead);
        return Result<std::pair<int, std::string>>::Err("ERR_PROCESS_START", "Failed to start scanner process");
    }

    std::string output;
    char buffer[1024];
    DWORD bytesRead = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        DWORD bytes_avail = 0;
        if (PeekNamedPipe(hStdOutRead, nullptr, 0, nullptr, &bytes_avail, nullptr) && bytes_avail > 0) {
            DWORD to_read = (std::min)(bytes_avail, static_cast<DWORD>(sizeof(buffer) - 1));
            if (ReadFile(hStdOutRead, buffer, to_read, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                output += buffer;
            }
        }

        DWORD wait_res = WaitForSingleObject(pi.hProcess, 50);
        if (wait_res == WAIT_OBJECT_0) {
            while (ReadFile(hStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                output += buffer;
            }
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed > timeout_ms) {
            TerminateProcess(pi.hProcess, 99);
            CloseHandle(hStdOutRead);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return Result<std::pair<int, std::string>>::Err("ERR_TIMEOUT", "Antivirus scan timed out");
        }
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(hStdOutRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return Result<std::pair<int, std::string>>::Ok({static_cast<int>(exit_code), output});
}
#endif

#ifdef _WIN32
std::wstring Utf8ToWide(std::string_view sv) {
    if (sv.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (sz <= 0) return {};
    std::wstring out(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), sz);
    return out;
}
#endif

} // namespace

bool AntivirusScanner::IsWindowsDefenderAvailable() {
#ifdef _WIN32
    return std::filesystem::exists(GetDefenderPath());
#else
    return false;
#endif
}

std::filesystem::path AntivirusScanner::GetDefenderPath() {
    std::error_code ec;
#ifdef _WIN32
    // Check %ProgramFiles%\Windows Defender\MpCmdRun.exe
    wchar_t pf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, pf) == S_OK) {
        std::filesystem::path p1 = std::filesystem::path(pf) / "Windows Defender" / "MpCmdRun.exe";
        if (std::filesystem::exists(p1, ec)) return p1;
    }

    // Check %ProgramData%\Microsoft\Windows Defender\Platform\*\MpCmdRun.exe
    wchar_t pd[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, pd) == S_OK) {
        std::filesystem::path platform_dir = std::filesystem::path(pd) / "Microsoft" / "Windows Defender" / "Platform";
        if (std::filesystem::exists(platform_dir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(platform_dir, ec)) {
                if (entry.is_directory()) {
                    std::filesystem::path candidate = entry.path() / "MpCmdRun.exe";
                    if (std::filesystem::exists(candidate, ec)) return candidate;
                }
            }
        }
    }
    return "C:\\Program Files\\Windows Defender\\MpCmdRun.exe";
#else
    return {};
#endif
}

Result<ScanReport> AntivirusScanner::ScanWithDefender(
    const std::filesystem::path& target_file,
    int timeout_ms) {

    std::error_code ec;
    if (!std::filesystem::exists(target_file, ec)) {
        return Result<ScanReport>::Err("ERR_FILE_NOT_FOUND", "Target file does not exist: " + target_file.string());
    }

#ifdef _WIN32
    auto defender_path = GetDefenderPath();
    if (defender_path.empty() || !std::filesystem::exists(defender_path, ec)) {
        return Result<ScanReport>::Err("ERR_DEFENDER_NOT_FOUND", "Windows Defender (MpCmdRun.exe) not found");
    }

    // MpCmdRun.exe requires an absolute normalized path. A relative path causes error 0x80508023
    // with exit code 2, which would otherwise be misclassified as a detected threat!
    std::filesystem::path abs_target = std::filesystem::absolute(target_file, ec);
    if (ec) {
        abs_target = target_file;
    }

    auto start_tp = std::chrono::steady_clock::now();

    std::wstring cmd = L"\"" + defender_path.wstring() + L"\" -Scan -ScanType 3 -File \"" + abs_target.wstring() + L"\"";
    auto proc_res = RunProcessCaptureOutput(defender_path.wstring(), cmd, timeout_ms);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_tp).count();

    if (!proc_res.ok()) {
        return Result<ScanReport>::Err(proc_res.error->code, proc_res.error->message);
    }

    ScanReport rep;
    rep.scanner_name = "Microsoft Defender Antivirus";
    rep.exit_code = proc_res.value->first;
    rep.output_message = proc_res.value->second;
    rep.scan_duration_ms = elapsed;

    if (rep.exit_code == 0) {
        rep.status = ScanStatus::kClean;
    } else if (rep.exit_code == 2) {
        // Exit code 2 is returned both for genuine threat detections and for execution errors (e.g. hr = 0x80508023).
        // Confirm whether threats were actually reported in the scanner output or if an error occurred.
        if (rep.output_message.find("found no threats") != std::string::npos) {
            rep.status = ScanStatus::kClean;
        } else if (rep.output_message.find("Failed with hr =") != std::string::npos ||
                   rep.output_message.find("ERROR:") != std::string::npos ||
                   rep.output_message.find("error") != std::string::npos) {
            rep.status = ScanStatus::kScannerError;
        } else {
            rep.status = ScanStatus::kThreatDetected;
        }
    } else {
        rep.status = ScanStatus::kScannerError;
    }
    return Result<ScanReport>::Ok(std::move(rep));
#else
    (void)timeout_ms;
    ScanReport rep;
    rep.status = ScanStatus::kUnavailable;
    rep.scanner_name = "Mock Defender (POSIX)";
    rep.output_message = "Windows Defender is not available on non-Windows platforms";
    return Result<ScanReport>::Ok(rep);
#endif
}

Result<ScanReport> AntivirusScanner::ScanWithCustomExecutable(
    const std::filesystem::path& custom_exe,
    std::string_view args_template,
    const std::filesystem::path& target_file,
    int timeout_ms) {

    std::error_code ec;
    if (!std::filesystem::exists(custom_exe, ec)) {
        return Result<ScanReport>::Err("ERR_SCANNER_NOT_FOUND", "Custom scanner not found: " + custom_exe.string());
    }
    if (!std::filesystem::exists(target_file, ec)) {
        return Result<ScanReport>::Err("ERR_FILE_NOT_FOUND", "Target file does not exist: " + target_file.string());
    }

#ifdef _WIN32
    std::wstring wtarget = target_file.wstring();
    std::wstring wtempl = Utf8ToWide(args_template);
    std::wstring wplaceholder = L"{file}";
    size_t ph_pos = wtempl.find(wplaceholder);
    if (ph_pos != std::wstring::npos) {
        wtempl.replace(ph_pos, wplaceholder.size(), L"\"" + wtarget + L"\"");
    } else {
        wtempl += L" \"" + wtarget + L"\"";
    }

    std::wstring cmd = L"\"" + custom_exe.wstring() + L"\" " + wtempl;

    auto start_tp = std::chrono::steady_clock::now();
    auto proc_res = RunProcessCaptureOutput(custom_exe.wstring(), cmd, timeout_ms);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_tp).count();

    if (!proc_res.ok()) {
        return Result<ScanReport>::Err(proc_res.error->code, proc_res.error->message);
    }

    ScanReport rep;
    rep.scanner_name = custom_exe.filename().string();
    rep.exit_code = proc_res.value->first;
    rep.output_message = proc_res.value->second;
    rep.scan_duration_ms = elapsed;
    if (rep.exit_code == 0) {
        rep.status = ScanStatus::kClean;
    } else if (rep.exit_code == 1 || rep.exit_code == 2) {
        rep.status = ScanStatus::kThreatDetected;
    } else {
        rep.status = ScanStatus::kScannerError;
    }

    return Result<ScanReport>::Ok(std::move(rep));
#else
    (void)args_template;
    (void)timeout_ms;
    ScanReport rep;
    rep.status = ScanStatus::kUnavailable;
    rep.scanner_name = custom_exe.filename().string();
    rep.output_message = "Custom scanner execution is not available on non-Windows platforms";
    return Result<ScanReport>::Ok(rep);
#endif
}

} // namespace openidm::security
