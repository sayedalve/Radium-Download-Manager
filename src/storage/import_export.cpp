// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Task Import & Export with Automated Credential Redaction
#include "storage/import_export.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

namespace openidm::storage {

namespace {

std::string ScrubUrlCredentials(std::string_view url) {
    std::string s(url);
    size_t proto = s.find("://");
    if (proto == std::string::npos) return s;
    size_t host_start = proto + 3;
    size_t path_pos = s.find('/', host_start);
    std::string authority = (path_pos == std::string::npos) ? s.substr(host_start) : s.substr(host_start, path_pos - host_start);

    size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        std::string userinfo = authority.substr(0, at);
        std::string host = authority.substr(at + 1);
        size_t colon = userinfo.find(':');
        std::string scrubbed_user = (colon != std::string::npos) ? userinfo.substr(0, colon) + ":[REDACTED]" : userinfo;
        std::string rest = (path_pos == std::string::npos) ? "" : s.substr(path_pos);
        return s.substr(0, host_start) + scrubbed_user + "@" + host + rest;
    }
    return s;
}

std::string EscapeCsv(std::string_view str) {
    bool needs_quotes = false;
    for (char c : str) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return std::string(str);

    std::string out = "\"";
    for (char c : str) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

std::vector<std::vector<std::string>> ParseRfc4180Csv(std::string_view csv) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> current_row;
    std::string current_field;
    bool in_quotes = false;

    size_t i = 0;
    const size_t len = csv.size();

    while (i < len) {
        char c = csv[i];

        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < len && csv[i + 1] == '"') {
                    // Escaped double-quote: "" -> "
                    current_field.push_back('"');
                    i += 2;
                } else {
                    // Closing quote
                    in_quotes = false;
                    i++;
                }
            } else {
                current_field.push_back(c);
                i++;
            }
        } else {
            if (c == '"') {
                in_quotes = true;
                i++;
            } else if (c == ',') {
                current_row.push_back(std::move(current_field));
                current_field.clear();
                i++;
            } else if (c == '\r') {
                if (i + 1 < len && csv[i + 1] == '\n') {
                    i++;
                }
                current_row.push_back(std::move(current_field));
                current_field.clear();
                rows.push_back(std::move(current_row));
                current_row.clear();
                i++;
            } else if (c == '\n') {
                current_row.push_back(std::move(current_field));
                current_field.clear();
                rows.push_back(std::move(current_row));
                current_row.clear();
                i++;
            } else {
                current_field.push_back(c);
                i++;
            }
        }
    }

    if (!current_field.empty() || !current_row.empty()) {
        current_row.push_back(std::move(current_field));
        rows.push_back(std::move(current_row));
    }

    return rows;
}

} // namespace

Result<std::string> TaskImportExport::ExportTasksToString(
    const std::vector<TaskModel>& tasks,
    ExportFormat format,
    bool include_credentials) {

    if (format == ExportFormat::kJson) {
        std::string out = "[\n";
        for (size_t i = 0; i < tasks.size(); ++i) {
            const auto& t = tasks[i];
            json::Value obj(json::Object{});
            obj["taskId"] = t.task_id;
            obj["taskType"] = TaskTypeToString(t.task_type);
            obj["url"] = include_credentials ? t.url : ScrubUrlCredentials(t.url);
            obj["fileName"] = t.file_name;
            obj["category"] = t.category;
            obj["queueId"] = t.queue_id;
            obj["saveDir"] = t.save_dir;
            obj["totalBytes"] = static_cast<double>(t.total_bytes);
            obj["downloadedBytes"] = static_cast<double>(t.downloaded_bytes);
            obj["state"] = TaskStateToString(t.state);
            obj["createdAtMs"] = static_cast<double>(t.created_at_ms);
            if (!t.torrent_file_paths.empty()) {
                json::Array selection;
                for (size_t selection_index = 0; selection_index < t.torrent_file_paths.size(); ++selection_index) {
                    json::Value entry(json::Object{});
                    entry["path"] = t.torrent_file_paths[selection_index];
                    entry["selected"] = selection_index < t.torrent_file_selection.size() && t.torrent_file_selection[selection_index] != 0;
                    selection.push_back(std::move(entry));
                }
                obj["torrentSelection"] = std::move(selection);
            }

            json::Value hdrs(json::Object{});
            for (const auto& [k, v] : t.headers) {
                hdrs[k] = v;
            }
            obj["headers"] = std::move(hdrs);

            if (include_credentials) {
                json::Value auth_hdrs(json::Object{});
                for (const auto& [k, v] : t.auth_headers) {
                    auth_hdrs[k] = v;
                }
                obj["authHeaders"] = std::move(auth_hdrs);
            }

            out += "  " + obj.dump();
            if (i + 1 < tasks.size()) out += ",";
            out += "\n";
        }
        out += "]\n";
        return Result<std::string>::Ok(out);
    } else {
        std::string out = "URL,FileName,Category,Queue,TotalBytes,State,TorrentSelection\r\n";
        for (const auto& t : tasks) {
            std::string u = include_credentials ? t.url : ScrubUrlCredentials(t.url);
            std::string selection_json;
            if (!t.torrent_file_paths.empty()) {
                json::Array selection;
                for (size_t selection_index = 0; selection_index < t.torrent_file_paths.size(); ++selection_index) {
                    json::Value entry(json::Object{});
                    entry["path"] = t.torrent_file_paths[selection_index];
                    entry["selected"] = selection_index < t.torrent_file_selection.size() &&
                                         t.torrent_file_selection[selection_index] != 0;
                    selection.push_back(std::move(entry));
                }
                selection_json = json::Value(std::move(selection)).dump();
            }
            out += EscapeCsv(u) + "," +
                   EscapeCsv(t.file_name) + "," +
                   EscapeCsv(t.category) + "," +
                   EscapeCsv(t.queue_id) + "," +
                   std::to_string(t.total_bytes) + "," +
                   EscapeCsv(TaskStateToString(t.state)) + "," +
                   EscapeCsv(selection_json) + "\r\n";
        }
        return Result<std::string>::Ok(out);
    }
}

Result<void> TaskImportExport::ExportTasksToFile(
    const std::vector<TaskModel>& tasks,
    const std::filesystem::path& target_file,
    ExportFormat format,
    bool include_credentials) {

    auto str_res = ExportTasksToString(tasks, format, include_credentials);
    if (!str_res.ok()) return Result<void>::Err(str_res.error->code, str_res.error->message);

    std::ofstream ofs(target_file, std::ios::binary);
    if (!ofs) {
        return Result<void>::Err("ERR_FILE_WRITE", "Cannot open file for writing: " + target_file.string());
    }
    ofs.write(str_res.value->data(), str_res.value->size());
    return Result<void>::Ok();
}

Result<std::vector<TaskModel>> TaskImportExport::ImportFromJson(std::string_view json_str) {
    json::Value parsed;
    try {
        parsed = json::Parse(json_str);
    } catch (const std::exception& e) {
        return Result<std::vector<TaskModel>>::Err("ERR_JSON_IMPORT", std::string("Malformed JSON: ") + e.what());
    }

    if (!parsed.is_array()) {
        return Result<std::vector<TaskModel>>::Err("ERR_JSON_IMPORT", "Expected top-level JSON array of tasks");
    }

    std::vector<TaskModel> out;
    for (const auto& item : parsed.as_array()) {
        if (!item.is_object() || !item.contains("url")) continue;

        TaskModel t;
        t.task_id = item.contains("taskId") ? item["taskId"].as_string() : GenerateUuidV4();
        t.url = item["url"].as_string();
        t.task_type = item.contains("taskType") ? TaskTypeFromString(item["taskType"].as_string()) : TaskType::kHttpFile;
        t.file_name = item.contains("fileName") ? PathSanitizer::SanitizeFileName(item["fileName"].as_string())
                                                : PathSanitizer::SanitizeFileName(PathSanitizer::ExtractFromUrl(t.url));
        t.category = item.contains("category") ? item["category"].as_string() : InferCategoryFromFilename(t.file_name);
        t.queue_id = item.contains("queueId") ? item["queueId"].as_string() : "main";
        t.save_dir = item.contains("saveDir") ? item["saveDir"].as_string() : PathSanitizer::GetDefaultDownloadDir().string();
        t.total_bytes = item.contains("totalBytes") ? static_cast<uint64_t>(item["totalBytes"].as_int64()) : 0;
        t.state = TaskState::kQueued;
        t.created_at_ms = NowEpochMs();
        t.updated_at_ms = t.created_at_ms;
        if (item.contains("torrentSelection") && item["torrentSelection"].is_array()) {
            for (const auto& entry : item["torrentSelection"].as_array()) {
                if (!entry.is_object()) continue;
                t.torrent_file_paths.push_back(entry["path"].as_string(""));
                t.torrent_file_selection.push_back(entry["selected"].as_bool(true) ? 1 : 0);
            }
        }

        if (item.contains("headers") && item["headers"].is_object()) {
            for (const auto& [k, v] : item["headers"].as_object()) {
                if (v.is_string()) t.headers[k] = v.as_string();
            }
        }
        out.push_back(std::move(t));
    }
    return Result<std::vector<TaskModel>>::Ok(std::move(out));
}

Result<std::vector<TaskModel>> TaskImportExport::ImportFromCsv(std::string_view csv_str) {
    auto rows = ParseRfc4180Csv(csv_str);
    std::vector<TaskModel> out;
    bool header_skipped = false;

    for (const auto& cols : rows) {
        if (cols.empty() || (cols.size() == 1 && cols[0].empty())) continue;

        if (!header_skipped) {
            header_skipped = true;
            if (!cols.empty() && (cols[0] == "URL" || cols[0] == "url" || cols[0].find("URL") != std::string::npos)) {
                continue;
            }
        }

        if (!cols.empty() && !cols[0].empty()) {
            TaskModel t;
            t.task_id = GenerateUuidV4();
            t.url = cols[0];
            t.task_type = TaskType::kHttpFile;
            if (cols.size() > 1 && !cols[1].empty()) {
                t.file_name = PathSanitizer::SanitizeFileName(cols[1]);
            } else {
                t.file_name = PathSanitizer::SanitizeFileName(PathSanitizer::ExtractFromUrl(t.url));
            }
            if (cols.size() > 2 && !cols[2].empty()) {
                t.category = cols[2];
            } else {
                t.category = InferCategoryFromFilename(t.file_name);
            }
            if (cols.size() > 3 && !cols[3].empty()) {
                t.queue_id = cols[3];
            } else {
                t.queue_id = "main";
            }
            if (cols.size() > 4 && !cols[4].empty()) {
                t.total_bytes = static_cast<uint64_t>(std::strtoull(cols[4].c_str(), nullptr, 10));
            }
            if (cols.size() > 6 && !cols[6].empty()) {
                try {
                    auto selection = json::Parse(cols[6]);
                    if (selection.is_array()) {
                        for (const auto& entry : selection.as_array()) {
                            if (!entry.is_object()) continue;
                            t.torrent_file_paths.push_back(entry["path"].as_string(""));
                            t.torrent_file_selection.push_back(entry["selected"].as_bool(true) ? 1 : 0);
                        }
                    }
                } catch (const std::exception&) {
                    return Result<std::vector<TaskModel>>::Err("ERR_CSV_IMPORT", "Invalid TorrentSelection JSON");
                }
            }
            t.save_dir = PathSanitizer::GetDefaultDownloadDir().string();
            t.state = TaskState::kQueued;
            t.created_at_ms = NowEpochMs();
            t.updated_at_ms = t.created_at_ms;
            out.push_back(std::move(t));
        }
    }
    return Result<std::vector<TaskModel>>::Ok(std::move(out));
}

Result<std::vector<TaskModel>> TaskImportExport::ImportFromUrlList(std::string_view text_list) {
    std::vector<TaskModel> out;
    std::string s_list(text_list);
    std::stringstream ss(s_list);
    std::string line;

    while (std::getline(ss, line)) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();

        if (line.empty() || line.front() == '#') continue;

        TaskModel t;
        t.task_id = GenerateUuidV4();
        t.url = line;
        if (line.find("magnet:?") == 0) {
            t.task_type = TaskType::kTorrent;
            t.file_name = "magnet_" + t.task_id.substr(0, 8);
        } else if (line.find("ftp://") == 0 || line.find("ftps://") == 0) {
            t.task_type = TaskType::kFtp;
            t.file_name = PathSanitizer::SanitizeFileName(PathSanitizer::ExtractFromUrl(t.url));
        } else {
            t.task_type = TaskType::kHttpFile;
            t.file_name = PathSanitizer::SanitizeFileName(PathSanitizer::ExtractFromUrl(t.url));
        }
        t.category = InferCategoryFromFilename(t.file_name);
        t.queue_id = "main";
        t.save_dir = PathSanitizer::GetDefaultDownloadDir().string();
        t.state = TaskState::kQueued;
        t.created_at_ms = NowEpochMs();
        t.updated_at_ms = t.created_at_ms;
        out.push_back(std::move(t));
    }
    return Result<std::vector<TaskModel>>::Ok(std::move(out));
}

} // namespace openidm::storage
