// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Task Import & Export with Automated Credential Redaction
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"

namespace openidm::storage {

enum class ExportFormat {
    kJson,
    kCsv
};

class TaskImportExport {
public:
    TaskImportExport() = default;
    ~TaskImportExport() = default;

    // Exports task list to a JSON or CSV string with automated credential scrubbing
    static Result<std::string> ExportTasksToString(
        const std::vector<TaskModel>& tasks,
        ExportFormat format = ExportFormat::kJson,
        bool include_credentials = false);

    // Exports task list to a target file on disk
    static Result<void> ExportTasksToFile(
        const std::vector<TaskModel>& tasks,
        const std::filesystem::path& target_file,
        ExportFormat format = ExportFormat::kJson,
        bool include_credentials = false);

    // Imports tasks from a JSON payload
    static Result<std::vector<TaskModel>> ImportFromJson(std::string_view json_str);

    // Imports tasks from an RFC 4180 CSV payload
    static Result<std::vector<TaskModel>> ImportFromCsv(std::string_view csv_str);

    // Imports tasks from a simple text list of URLs (one URL per line)
    static Result<std::vector<TaskModel>> ImportFromUrlList(std::string_view text_list);
};

} // namespace openidm::storage
