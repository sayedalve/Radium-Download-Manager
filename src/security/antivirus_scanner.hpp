// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Post-Download Antivirus Scanner Integration (Windows Defender & Custom Scanners)
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "common/types.hpp"

namespace openidm::security {

enum class ScanStatus {
    kClean,
    kThreatDetected,
    kScannerError,
    kSkipped,
    kUnavailable
};

struct ScanReport {
    ScanStatus status{ScanStatus::kUnavailable};
    std::string scanner_name;
    std::string output_message;
    int exit_code{0};
    int64_t scan_duration_ms{0};
};

class AntivirusScanner {
public:
    AntivirusScanner() = default;
    ~AntivirusScanner() = default;

    // Detects whether Windows Defender (MpCmdRun.exe) is installed on this Windows host
    static bool IsWindowsDefenderAvailable();

    // Resolves default path to MpCmdRun.exe
    static std::filesystem::path GetDefenderPath();

    // Scans target_file using Windows Defender
    static Result<ScanReport> ScanWithDefender(
        const std::filesystem::path& target_file,
        int timeout_ms = 30000);

    // Scans target_file using a custom executable and arguments template
    // Template placeholder: {file} is replaced with target_file path
    static Result<ScanReport> ScanWithCustomExecutable(
        const std::filesystem::path& custom_exe,
        std::string_view args_template,
        const std::filesystem::path& target_file,
        int timeout_ms = 30000);
};

} // namespace openidm::security
