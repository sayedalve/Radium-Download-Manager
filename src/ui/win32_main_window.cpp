// SPDX-License-Identifier: Apache-2.0
#include "ui/win32_main_window.hpp"

#include "brisk_engine/http_client.hpp"
#include "common/crypto_utils.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

#endif

namespace openidm::ui {

namespace {

#ifdef _WIN32
UINT g_msgTaskbarButtonCreated = 0;
HICON CreateRadiumAppIcon();
HICON CreateFluxAppIcon();

bool ShowAddTorrentDialogWin32(
    HWND hwnd_parent,
    DesktopView& view,
    DownloadController& controller,
    torrent::TorrentInfo& info,
    const std::string& torrent_file_path = "",
    const std::string& initial_save_dir = "",
    bool start_immediately = true,
    std::string* selected_save_dir = nullptr,
    bool* selected_start_immediately = nullptr,
    std::string* selected_category = nullptr);

std::wstring Utf8ToW(std::string_view sv) {
    if (sv.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), len);
    return out;
}

std::string WToUtf8(std::wstring_view wv) {
    if (wv.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()), out.data(), len, nullptr, nullptr);
    return out;
}
#endif

std::string ToLower(std::string_view sv) {
    std::string s(sv);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string ToLowerStr(std::string_view sv) {
    return ToLower(sv);
}

std::string FormatTimestampUtc(int64_t epoch_ms) {
    if (epoch_ms <= 0) return "—";
    std::time_t tt = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S UTC");
    return oss.str();
}

std::string FormatConflictPolicyLabel(ConflictPolicy p) {
    switch (p) {
        case ConflictPolicy::kAutoRename:   return "Auto-Rename (file (1).ext)";
        case ConflictPolicy::kOverwrite:    return "Overwrite Existing File";
        case ConflictPolicy::kFailIfExists: return "Skip / Fail if File Exists";
    }
    return "Auto-Rename (file (1).ext)";
}

} // namespace

DesktopView::DesktopView(DownloadController& controller, ipc::IpcServer* ipc_server)
    : m_controller(controller),
      m_ipc_server(ipc_server),
      m_metrics(DesignSystem::ComputeMetrics(96)),
      m_palette(DesignSystem::GetRedesignPalette()) {
    auto settings = m_controller.GetSettings();
    m_draft_settings = settings;
    m_anim.reduced_motion = settings.reduced_motion;
    m_anim.window_visible = !settings.start_minimized;
    m_anim.window_minimized = settings.start_minimized;
    SetTheme(settings.theme, false);
    LoadColumnWidthsFromSettings();
}

void DesktopView::SetWindowDimensions(int width, int height, uint32_t dpi) {
    ClearHoverState();
    m_metrics = DesignSystem::ComputeMetrics(dpi);
    m_window_width = std::max(width, 100);
    m_window_height = std::max(height, 100);
}

void DesktopView::SetWindowVisibility(bool visible, bool minimized) {
    m_anim.window_visible = visible;
    m_anim.window_minimized = minimized;
}

void DesktopView::TriggerActionPulse(const std::string& task_id, const std::string& action) {
    if (!m_anim.IsActive()) return;
    m_anim.icon_pulses[task_id] = {GetTickCount64(), action};
}

TaskbarProgressInfo DesktopView::ComputeTaskbarProgress() const {
    TaskbarProgressInfo info;
    auto tasks = m_controller.ListTasks();
    if (tasks.empty()) {
        info.state = TaskbarProgressState::kNoProgress;
        return info;
    }

    size_t active_count = 0;
    size_t paused_count = 0;
    size_t failed_count = 0;
    uint64_t active_completed = 0;
    uint64_t active_total = 0;
    size_t active_with_total = 0;

    uint64_t paused_completed = 0;
    uint64_t paused_total = 0;

    for (const auto& t : tasks) {
        if (t.state == TaskState::kDownloading || t.state == TaskState::kProbing || 
            t.state == TaskState::kMuxing || t.state == TaskState::kVerifying || 
            t.state == TaskState::kFinalizing) {
            active_count++;
            if (t.total_bytes > 0) {
                active_completed += t.downloaded_bytes;
                active_total += t.total_bytes;
                active_with_total++;
            }
        } else if (t.state == TaskState::kPaused) {
            paused_count++;
            if (t.total_bytes > 0) {
                paused_completed += t.downloaded_bytes;
                paused_total += t.total_bytes;
            }
        } else if (t.state == TaskState::kFailed) {
            failed_count++;
        }
    }

    if (active_count > 0) {
        if (active_with_total == 0 || active_total == 0) {
            info.state = TaskbarProgressState::kIndeterminate;
        } else {
            info.state = TaskbarProgressState::kNormal;
            info.completed_bytes = active_completed;
            info.total_bytes = active_total;
            info.progress_fraction = static_cast<double>(active_completed) / static_cast<double>(active_total);
        }
    } else {
        // When there are no active downloads (kDownloading, kProbing, kMuxing, kVerifying, kFinalizing),
        // clear taskbar progress completely using TBPF_NOPROGRESS.
        // Historical completed, failed, canceled, and paused downloads in the task list must NEVER keep
        // a stale progress indicator on the Windows taskbar.
        info.state = TaskbarProgressState::kNoProgress;
    }

    return info;
}

std::vector<std::string> DesktopView::GetVisibleColumns() const {
    if (IsCompactColumnsMode()) {
        return {"Filename", "Status", "Progress", "Size", "Speed", "Actions"};
    }
    return {"Filename", "Type", "Status", "Progress", "Size", "Speed", "ETA", "Actions"};
}

WindowLayoutGeometry DesktopView::ComputeLayoutGeometry(size_t scroll_offset) const {
    WindowLayoutGeometry g;
    const auto& m = m_metrics;
    int w = m_window_width;
    int h = m_window_height;

    int bar_h = DesignSystem::ScaleForDpi(56, m.dpi);
    g.toolbar_rect = {0, 0, w, bar_h};

    int chk_s = DesignSystem::ScaleForDpi(20, m.dpi);
    int chk_y = (bar_h - chk_s) / 2;
    g.select_all_checkbox_rect = {16, chk_y, 16 + chk_s, chk_y + chk_s};

    int sort_w = DesignSystem::ScaleForDpi(110, m.dpi);
    int sort_h = DesignSystem::ScaleForDpi(36, m.dpi);
    int sort_y = (bar_h - sort_h) / 2;
    g.sort_dropdown_rect = {16 + chk_s + 12, sort_y, 16 + chk_s + 12 + sort_w, sort_y + sort_h};

    int filt_w = DesignSystem::ScaleForDpi(110, m.dpi);
    int filt_x = g.sort_dropdown_rect.right + 10;
    g.filter_dropdown_rect = {filt_x, sort_y, filt_x + filt_w, sort_y + sort_h};

    int btn_h = DesignSystem::ScaleForDpi(36, m.dpi);
    int by = (bar_h - btn_h) / 2;

    int add_btn_w = DesignSystem::ScaleForDpi(145, m.dpi);
    int add_btn_x = w - add_btn_w - 16;
    g.add_download_button_rect = {add_btn_x, by, add_btn_x + add_btn_w, by + btn_h};

    int settings_btn_w = btn_h;
    int settings_btn_x = add_btn_x - settings_btn_w - 10;
    g.settings_button_rect = {settings_btn_x, by, settings_btn_x + settings_btn_w, by + btn_h};
    g.search_sliders_rect = g.settings_button_rect;

    int search_max_w = DesignSystem::ScaleForDpi(260, m.dpi);
    int search_w = std::min(search_max_w, std::max(140, settings_btn_x - filt_x - filt_w - 20));
    int search_x = settings_btn_x - search_w - 12;
    g.search_box_rect = {search_x, by, search_x + search_w, by + btn_h};

    auto tb = ComputeToolbarState();
    struct BtnDef { ToolbarActionId id; const char* label; bool enabled; bool primary; int base_w; };
    const BtnDef defs[] = {
        {ToolbarActionId::kAddUrl,       "+ Add download", tb.can_add_url,       true,  145},
        {ToolbarActionId::kStartAll,     "Start all",      tb.can_start_all,     false, 82},
        {ToolbarActionId::kPauseAll,     "Pause all",      tb.can_pause_all,     false, 82},
        {ToolbarActionId::kStart,        "Start",          tb.can_start,         false, 62},
        {ToolbarActionId::kPause,        "Pause",          tb.can_pause,         false, 62},
        {ToolbarActionId::kResume,       "Resume",         tb.can_resume,        false, 68},
        {ToolbarActionId::kRetry,        "Retry",          tb.can_retry,         false, 62},
        {ToolbarActionId::kCancel,       "Cancel",         tb.can_cancel,        false, 64},
        {ToolbarActionId::kClearHistory, "Clear History",  tb.can_clear_history, false, 96},
        {ToolbarActionId::kSettings,     "Settings",       tb.can_open_settings, false, 36}
    };
    int act_x = filt_x + filt_w + DesignSystem::ScaleForDpi(12, m.dpi);
    int max_act_x = search_x - DesignSystem::ScaleForDpi(12, m.dpi);
    for (size_t i = 0; i < 10; ++i) {
        const auto& d = defs[i];
        RectI brc{0, 0, 0, 0};
        if (i == 0) {
            brc = g.add_download_button_rect;
        } else if (i == 9) {
            brc = g.settings_button_rect;
        } else if (d.enabled) {
            int bw = DesignSystem::ScaleForDpi(d.base_w, m.dpi);
            if (act_x + bw <= max_act_x) {
                brc = {act_x, by, act_x + bw, by + btn_h};
                act_x += bw + DesignSystem::ScaleForDpi(8, m.dpi);
            }
        }
        g.toolbar_buttons.push_back({d.id, d.label, d.enabled, d.primary, brc});
    }

    auto counts = ComputeCategoryCounts();
    const std::pair<SidebarCategory, size_t> cats[] = {
        {SidebarCategory::kAllDownloads,   counts.all_downloads},
        {SidebarCategory::kInProgress,     counts.in_progress},
        {SidebarCategory::kQueued,         counts.queued},
        {SidebarCategory::kCompleted,      counts.completed},
        {SidebarCategory::kFailed,         counts.failed},
        {SidebarCategory::kVideoDownloads, counts.video_downloads},
        {SidebarCategory::kScheduled,      counts.scheduled}
    };
    for (size_t i = 0; i < 7; ++i) {
        const auto& [cat, cnt] = cats[i];
        RectI crc = (i == 0) ? g.filter_dropdown_rect : RectI{w + 300, static_cast<int>(i * 35), w + 450, static_cast<int>((i + 1) * 35)};
        g.sidebar_items.push_back({cat, CategoryToLabel(cat), cnt, cat == m_selected_category, crc});
    }
    g.sidebar_rect = {0, bar_h, 0, bar_h};

    int hdr_h = DesignSystem::ScaleForDpi(38, m.dpi);
    int sb_h = 0; // Bottom status bar permanently removed
    int det_h = (m_selected_task_id.empty() || h < DesignSystem::ScaleForDpi(580, m.dpi))
        ? 0
        : DesignSystem::ScaleForDpi(115, m.dpi);
    g.table_header_rect = {0, bar_h, w, bar_h + hdr_h};
    g.table_body_rect = {0, bar_h + hdr_h, w, h - sb_h - det_h};
    g.details_panel_rect = {0, h - sb_h - det_h, w, h - sb_h};
    g.status_bar_rect = {0, h, w, h};
    g.status_toggle_off_rect = {0, 0, 0, 0};

    int col_chk_w = DesignSystem::ScaleForDpi(72, m.dpi);
    int def_name_w = DesignSystem::ScaleForDpi(320, m.dpi);
    int def_size_w = DesignSystem::ScaleForDpi(90, m.dpi);
    int def_stat_w = DesignSystem::ScaleForDpi(125, m.dpi);
    int def_dl_w = DesignSystem::ScaleForDpi(95, m.dpi);
    int def_ul_w = DesignSystem::ScaleForDpi(85, m.dpi);
    int def_added_w = DesignSystem::ScaleForDpi(120, m.dpi);

    if (m_column_widths.size() < 6) {
        m_column_widths = {def_name_w, def_size_w, def_stat_w, def_dl_w, def_ul_w, def_added_w};
    } else {
        if (m_column_widths[0] < 120 || m_column_widths[0] > 2000) m_column_widths[0] = def_name_w;
        if (m_column_widths[1] < 40 || m_column_widths[1] > 2000) m_column_widths[1] = def_size_w;
        if (m_column_widths[2] < 40 || m_column_widths[2] > 2000) m_column_widths[2] = def_stat_w;
        if (m_column_widths[3] < 40 || m_column_widths[3] > 2000) m_column_widths[3] = def_dl_w;
        if (m_column_widths[4] < 40 || m_column_widths[4] > 2000) m_column_widths[4] = def_ul_w;
        if (m_column_widths[5] < 40 || m_column_widths[5] > 2000) m_column_widths[5] = def_added_w;
    }

    int base_col_size_w = std::max(DesignSystem::ScaleForDpi(75, m.dpi), m_column_widths[1]);
    int base_col_stat_w = std::max(DesignSystem::ScaleForDpi(95, m.dpi), m_column_widths[2]);
    int base_col_dl_w   = std::max(DesignSystem::ScaleForDpi(80, m.dpi), m_column_widths[3]);
    int base_col_ul_w   = std::max(DesignSystem::ScaleForDpi(70, m.dpi), m_column_widths[4]);
    int base_col_added_w= std::max(DesignSystem::ScaleForDpi(90, m.dpi), m_column_widths[5]);

    int col_size_w = base_col_size_w;
    int col_stat_w = base_col_stat_w;
    int col_dl_w   = base_col_dl_w;
    int col_ul_w   = base_col_ul_w;
    int col_added_w= base_col_added_w;

    // Intelligent responsive hiding and adaptation at narrow client widths
    int total_avail = w - 32;
    if (total_avail < DesignSystem::ScaleForDpi(620, m.dpi)) {
        col_added_w = 0; // Cleanly hide Added column
    }
    if (total_avail < DesignSystem::ScaleForDpi(520, m.dpi)) {
        col_ul_w = 0; // Cleanly hide Upload column
    }
    if (total_avail < DesignSystem::ScaleForDpi(420, m.dpi)) {
        col_dl_w = 0; // Cleanly hide Download speed column
    }

    int min_name_w = DesignSystem::ScaleForDpi(260, m.dpi);
    int fixed_non_name_w = col_size_w + col_stat_w + col_dl_w + col_ul_w + col_added_w;
    // Persisted column widths can exceed the restored window's available space.
    // Shrink the flexible metadata columns first so the Name column keeps room
    // for its checkbox/index/icon prefix and never overflows into Size.
    int fixed_budget = std::max(0, total_avail - min_name_w);
    int fixed_overflow = std::max(0, fixed_non_name_w - fixed_budget);
    auto shrink_column = [&fixed_overflow](int& width, int minimum) {
        const int reduction = std::min(fixed_overflow, std::max(0, width - minimum));
        width -= reduction;
        fixed_overflow -= reduction;
    };
    shrink_column(col_stat_w, DesignSystem::ScaleForDpi(95, m.dpi));
    shrink_column(col_added_w, DesignSystem::ScaleForDpi(90, m.dpi));
    shrink_column(col_ul_w, DesignSystem::ScaleForDpi(70, m.dpi));
    shrink_column(col_dl_w, DesignSystem::ScaleForDpi(80, m.dpi));
    shrink_column(col_size_w, DesignSystem::ScaleForDpi(75, m.dpi));
    fixed_non_name_w = col_size_w + col_stat_w + col_dl_w + col_ul_w + col_added_w;
    // The row prefix (checkbox, index and file icon) consumes about 100 px.
    // Keep enough room for a useful filename so owner-drawn text cannot spill
    // across the Size column when a narrower width was saved previously.
    int col_name_w = m_column_widths[0];
    int available_for_name = total_avail - fixed_non_name_w;
    int col_name_total_w = (available_for_name >= min_name_w)
        ? std::min(available_for_name, std::max(col_name_w, min_name_w))
        : std::max(DesignSystem::ScaleForDpi(120, m.dpi), available_for_name);

    int cx = 16;
    g.table_columns.push_back({SortColumn::kFileName, "Name", {cx, bar_h, cx + col_name_total_w, bar_h + hdr_h}});
    cx += col_name_total_w;

    g.table_columns.push_back({SortColumn::kSize, "Size", {cx, bar_h, cx + col_size_w, bar_h + hdr_h}});
    cx += col_size_w;

    g.table_columns.push_back({SortColumn::kStatus, "Status", {cx, bar_h, cx + col_stat_w, bar_h + hdr_h}});
    cx += col_stat_w;

    g.table_columns.push_back({SortColumn::kSpeed, "Download", {cx, bar_h, cx + col_dl_w, bar_h + hdr_h}});
    cx += col_dl_w;

    g.table_columns.push_back({SortColumn::kDownloadType, "Upload", {cx, bar_h, cx + col_ul_w, bar_h + hdr_h}});
    cx += col_ul_w;

    g.table_columns.push_back({SortColumn::kCreatedAt, "Added", {cx, bar_h, cx + col_added_w, bar_h + hdr_h}});
    cx += col_added_w;

    g.table_columns.push_back({SortColumn::kProgress, "Progress", {cx, bar_h, cx, bar_h + hdr_h}});
    g.table_columns.push_back({SortColumn::kEta, "ETA", {cx, bar_h, cx, bar_h + hdr_h}});

    int row_h = DesignSystem::ScaleForDpi(52, m.dpi);
    size_t max_rows = static_cast<size_t>(std::max(1, g.table_body_rect.height() / row_h));
    auto visible_rows = BuildVisibleTaskRows(scroll_offset, max_rows);
    int ry = g.table_body_rect.top;
    int dl_col_left = (g.table_columns[3].rect.width() > 0) ? g.table_columns[3].rect.left : g.table_columns[2].rect.right;

    for (const auto& r : visible_rows) {
        if (ry + row_h > g.table_body_rect.bottom) break;
        RectI row_rc{0, ry, w, ry + row_h};
        RectI fn_rc{16 + col_chk_w, ry, 16 + col_name_total_w, ry + row_h};
        RectI prog_rc{16 + col_chk_w + 4, ry + row_h - 14, 16 + col_chk_w + std::min(160, std::max(50, col_name_total_w - col_chk_w - 70)), ry + row_h - 11};
        RectI act_rc{dl_col_left, ry + (row_h - 22) / 2, dl_col_left + 22, ry + (row_h + 22) / 2};
        RectI chk_rc{16, ry + (row_h - 20) / 2, 36, ry + (row_h + 20) / 2};
        g.task_rows.push_back({r.task_id, row_rc, fn_rc, prog_rc, act_rc, chk_rc, act_rc});
        ry += row_h;
    }

    return g;
}

HitTestResult DesktopView::HitTestPoint(int x, int y, size_t scroll_offset) const {
    if (m_settings_center_open) {
        auto sv = BuildSettingsCenterView();
        if (sv.apply_button_rect.Contains(x, y)) {
            return {HitTargetType::kSettingsApplyButton, 0, "", "Apply and save modified settings"};
        }
        if (sv.cancel_button_rect.Contains(x, y)) {
            return {HitTargetType::kSettingsCancelButton, 0, "", "Discard unsaved changes and close Settings (Esc)"};
        }
        if (sv.restore_defaults_rect.Contains(x, y)) {
            return {HitTargetType::kSettingsRestoreDefaultsButton, 0, "", "Restore all settings to factory defaults"};
        }
        for (size_t i = 0; i < sv.categories.size(); ++i) {
            if (sv.categories[i].rect.Contains(x, y)) {
                return {HitTargetType::kSettingsCategoryTab, static_cast<int>(i), "", sv.categories[i].label};
            }
        }
        for (size_t i = 0; i < sv.visible_items.size(); ++i) {
            if (sv.visible_items[i].control_rect.Contains(x, y) || sv.visible_items[i].row_rect.Contains(x, y)) {
                return {HitTargetType::kSettingsItemControl, static_cast<int>(i), sv.visible_items[i].key, sv.visible_items[i].description};
            }
        }
        if (sv.modal_rect.Contains(x, y)) {
            return {HitTargetType::kSettingsModalBackground, 0, "", ""};
        }
        return {HitTargetType::kSettingsBackdrop, 0, "", "Click outside to close settings"};
    }

    auto geom = ComputeLayoutGeometry(scroll_offset);
    if (geom.search_sliders_rect.Contains(x, y)) {
        return {HitTargetType::kSearchSlidersButton, 0, "", "Settings Center (Ctrl+,)"};
    }
    if (geom.search_box_rect.Contains(x, y)) {
        return {HitTargetType::kSearchBox, 0, "", "Search downloads (Ctrl+K)"};
    }
    if (geom.add_download_button_rect.Contains(x, y)) {
        return {HitTargetType::kToolbarButton, 0, "", "+ Add download (Ctrl+N)"};
    }
    if (geom.select_all_checkbox_rect.Contains(x, y)) {
        return {HitTargetType::kSelectAllCheckbox, 0, "", "Select all / Deselect all"};
    }
    if (geom.sort_dropdown_rect.Contains(x, y)) {
        return {HitTargetType::kSortDropdown, 0, "", "Sort downloads"};
    }
    for (size_t i = 0; i < geom.sidebar_items.size(); ++i) {
        if (geom.sidebar_items[i].rect.width() > 0 && geom.sidebar_items[i].rect.height() > 0 && geom.sidebar_items[i].rect.Contains(x, y)) {
            return {HitTargetType::kSidebarCategory, static_cast<int>(i), "", geom.sidebar_items[i].label};
        }
    }
    if (geom.filter_dropdown_rect.Contains(x, y)) {
        return {HitTargetType::kFilterDropdown, 0, "", "Filter by category"};
    }
    if (geom.status_toggle_off_rect.Contains(x, y)) {
        return {HitTargetType::kStatusToggleOff, 0, "", "Speed limiter toggle"};
    }
    for (size_t i = 0; i < geom.toolbar_buttons.size(); ++i) {
        if (geom.toolbar_buttons[i].rect.Contains(x, y)) {
            return {HitTargetType::kToolbarButton, static_cast<int>(i), "", geom.toolbar_buttons[i].label};
        }
    }
    if (geom.toolbar_rect.Contains(x, y)) {
        return {HitTargetType::kNone, 0, "", ""};
    }
    if (geom.table_header_rect.Contains(x, y)) {
        // Table header row is static and non-interactive
        return {HitTargetType::kNone, 0, "", ""};
    }
    if (geom.status_bar_rect.Contains(x, y)) {
        return {HitTargetType::kNone, 0, "", ""};
    }
    if (geom.details_panel_rect.height() > 0 && geom.details_panel_rect.Contains(x, y)) {
        return {HitTargetType::kNone, 0, "", ""};
    }
    std::vector<FormattedTaskRow> rows;
    bool rows_built = false;
    for (size_t i = 0; i < geom.task_rows.size(); ++i) {
        if (geom.task_rows[i].checkbox_rect.Contains(x, y)) {
            return {HitTargetType::kRowCheckbox, static_cast<int>(i), geom.task_rows[i].task_id, "Select download"};
        }
        if (geom.task_rows[i].action_button_rect.Contains(x, y) || geom.task_rows[i].action_icon_rect.Contains(x, y)) {
            if (!rows_built) {
                rows = BuildVisibleTaskRows(scroll_offset, geom.task_rows.size());
                rows_built = true;
            }
            std::string act_lbl = (i < rows.size()) ? rows[i].primary_action_label : "Action";
 return {HitTargetType::kTaskRowActionButton, static_cast<int>(i), geom.task_rows[i].task_id, act_lbl};
 }
 if (geom.task_rows[i].row_rect.Contains(x, y)) {
 if (!rows_built) {
 rows = BuildVisibleTaskRows(scroll_offset, geom.task_rows.size());
 rows_built = true;
 }
 std::string tip;
 if (i < rows.size()) {
 if (geom.task_rows[i].filename_rect.Contains(x, y)) {
 tip = rows[i].full_filename_tooltip;
 } else if (!rows[i].full_status_tooltip.empty()) {
 tip = rows[i].full_status_tooltip;
 } else {
 tip = rows[i].full_filename_tooltip;
 }
 }
 return {HitTargetType::kTaskRow, static_cast<int>(i), geom.task_rows[i].task_id, tip};
 }
 }
 return {};
}

bool DesktopView::HandleMouseClick(int x, int y, bool double_click, size_t scroll_offset) {
    auto hit = HitTestPoint(x, y, scroll_offset);

    if (m_settings_center_open) {
        auto sv = BuildSettingsCenterView();
        switch (hit.type) {
            case HitTargetType::kSettingsCategoryTab: {
                if (hit.index >= 0 && static_cast<size_t>(hit.index) < sv.categories.size()) {
                    SelectSettingsCategory(sv.categories[static_cast<size_t>(hit.index)].category);
                    return true;
                }
                return false;
            }
            case HitTargetType::kSettingsItemControl: {
                if (!hit.task_id.empty()) {
                    CycleOrToggleDraftSetting(hit.task_id);
                    return true;
                }
                return false;
            }
            case HitTargetType::kSettingsApplyButton: {
                auto r = ApplySettingsChanges();
                if (r.ok()) {
                    CloseSettingsCenter(false);
                }
                return r.ok();
            }
            case HitTargetType::kSettingsCancelButton: {
                if (HasUnsavedSettingsChanges()) {
#ifdef _WIN32
                    if (m_hwnd) {
                        int res = MessageBoxW(m_hwnd, L"You have unsaved changes. Discard changes and close settings?", L"Discard Changes", MB_YESNO | MB_ICONQUESTION);
                        if (res != IDYES) return true;
                    }
#endif
                }
                CloseSettingsCenter(true);
                return true;
            }
            case HitTargetType::kSettingsRestoreDefaultsButton: {
                ActionRestoreDefaultSettings();
                return true;
            }
            case HitTargetType::kSettingsBackdrop: {
                if (HasUnsavedSettingsChanges()) {
#ifdef _WIN32
                    if (m_hwnd) {
                        int res = MessageBoxW(m_hwnd, L"You have unsaved changes. Discard changes and close settings?", L"Discard Changes", MB_YESNO | MB_ICONQUESTION);
                        if (res != IDYES) return true;
                    }
#endif
                }
                CloseSettingsCenter(true);
                return true;
            }
            case HitTargetType::kSettingsModalBackground: {
                return true;
            }
            default:
                return true;
        }
    }

    auto geom = ComputeLayoutGeometry(scroll_offset);
    switch (hit.type) {
        case HitTargetType::kSelectAllCheckbox: {
            ToggleSelectAll();
            return true;
        }
        case HitTargetType::kSortDropdown: {
            CycleSortDropdown();
            return true;
        }
        case HitTargetType::kFilterDropdown: {
            CycleFilterDropdown();
            return true;
        }
        case HitTargetType::kSearchSlidersButton: {
            OpenSettingsCenter(SettingsCategory::kGeneral);
            return true;
        }
        case HitTargetType::kStatusToggleOff: {
            ToggleSpeedLimiter();
            return true;
        }
        case HitTargetType::kRowCheckbox: {
            ToggleTaskChecked(hit.task_id);
            SelectTask(hit.task_id);
            return true;
        }
        case HitTargetType::kToolbarButton: {
            if (hit.index >= 0 && static_cast<size_t>(hit.index) < geom.toolbar_buttons.size()) {
                const auto& btn = geom.toolbar_buttons[static_cast<size_t>(hit.index)];
                if (!btn.enabled) return false;
                TriggerToolbarAction(btn.action_id);
                return true;
            }
            return false;
        }
        case HitTargetType::kSidebarCategory: {
            if (hit.index >= 0 && static_cast<size_t>(hit.index) < geom.sidebar_items.size()) {
                SelectCategory(geom.sidebar_items[static_cast<size_t>(hit.index)].category);
                return true;
            }
            return false;
        }
        case HitTargetType::kTableHeaderColumn: {
            if (hit.index >= 0 && static_cast<size_t>(hit.index) < geom.table_columns.size()) {
                ToggleSortColumn(geom.table_columns[static_cast<size_t>(hit.index)].sort_column);
                return true;
            }
            return false;
        }
        case HitTargetType::kTaskRowActionButton: {
            SelectTask(hit.task_id);
            TriggerRowPrimaryAction(hit.task_id);
            return true;
        }
        case HitTargetType::kTaskRow: {
            SelectTask(hit.task_id);
            if (double_click) {
                ActionOpenCompletedFile(false);
            }
            return true;
        }
        default:
            return false;
    }
}

HitTestResult DesktopView::HandleMouseMove(int x, int y, size_t scroll_offset) {
    auto hit = HitTestPoint(x, y, scroll_offset);

    int new_toolbar_idx = (hit.type == HitTargetType::kToolbarButton) ? hit.index : -1;
    if (new_toolbar_idx != m_hovered_toolbar_index) {
        m_hovered_toolbar_index = new_toolbar_idx;
        if (new_toolbar_idx >= 0) {
            m_anim.hovered_toolbar_idx = new_toolbar_idx;
            m_anim.toolbar_hover_start_ms = GetTickCount64();
        } else {
            m_anim.hovered_toolbar_idx = -1;
            m_anim.toolbar_hover_start_ms = 0;
        }
    }

    int new_row_idx = (hit.type == HitTargetType::kTaskRow || hit.type == HitTargetType::kTaskRowActionButton) ? hit.index : -1;
    if (new_row_idx != m_hovered_row_index) {
        m_hovered_row_index = new_row_idx;
        if (new_row_idx >= 0) {
            m_anim.hovered_row_idx = new_row_idx;
            m_anim.row_hover_start_ms = GetTickCount64();
        } else {
            m_anim.hovered_row_idx = -1;
            m_anim.row_hover_start_ms = 0;
        }
    }

    m_hovered_sidebar_index = (hit.type == HitTargetType::kSidebarCategory) ? hit.index : -1;
    m_active_tooltip = hit.tooltip_text;
    return hit;
}

void DesktopView::ClearHoverState() {
    m_hovered_toolbar_index = -1;
    m_hovered_sidebar_index = -1;
    m_hovered_row_index = -1;
    m_anim.hovered_toolbar_idx = -1;
    m_anim.toolbar_hover_start_ms = 0;
    m_anim.hovered_row_idx = -1;
    m_anim.row_hover_start_ms = 0;
    m_active_tooltip.clear();
}

RectI DesktopView::GetRowRect(size_t row_index) const {
    int bar_h = DesignSystem::ScaleForDpi(48, m_metrics.dpi);
    int hdr_h = DesignSystem::ScaleForDpi(38, m_metrics.dpi);
    int row_h = DesignSystem::ScaleForDpi(52, m_metrics.dpi);
    int ry = bar_h + hdr_h + static_cast<int>(row_index) * row_h;
    return RectI{0, ry, m_window_width, ry + row_h};
}

void DesktopView::SetTheme(std::string_view theme_name, bool os_prefers_light) {
    if (theme_name == "light" || theme_name == "system" || theme_name == "dark" || theme_name == "coral" || theme_name == "coral_dark" || theme_name == "redesign" || theme_name == "classic_blue") {
        m_theme_name = std::string(theme_name);
    } else {
        m_theme_name = "coral_dark";
    }
    m_palette = DesignSystem::ResolvePalette(m_theme_name, os_prefers_light);
}

std::string DesktopView::FormatBytes(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes >= 1073741824ULL) {
        oss << (static_cast<double>(bytes) / 1073741824.0) << " GiB";
    } else if (bytes >= 1048576ULL) {
        oss << (static_cast<double>(bytes) / 1048576.0) << " MiB";
    } else if (bytes >= 1024ULL) {
        oss << (static_cast<double>(bytes) / 1024.0) << " KiB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

std::string DesktopView::FormatSpeed(uint64_t bps) {
    if (bps == 0) return "—";
    return FormatBytes(bps) + "/s";
}

std::string DesktopView::FormatBandwidthLimit(uint64_t bps) {
    if (bps == 0) return "Unlimited";
    return FormatBytes(bps) + "/s";
}

std::string DesktopView::FormatEta(uint64_t remaining_bytes, uint64_t speed_bps) {
    if (speed_bps == 0 || remaining_bytes == 0) return "—";
    uint64_t secs = remaining_bytes / speed_bps;
    if (secs > 359999ULL) return ">99h";
    uint64_t hrs = secs / 3600;
    uint64_t mins = (secs % 3600) / 60;
    uint64_t rem_s = secs % 60;
    char buf[32];
    if (hrs > 0) {
        std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
                      static_cast<unsigned long long>(hrs),
                      static_cast<unsigned long long>(mins),
                      static_cast<unsigned long long>(rem_s));
    } else {
        std::snprintf(buf, sizeof(buf), "%02llu:%02llu",
                      static_cast<unsigned long long>(mins),
                      static_cast<unsigned long long>(rem_s));
    }
    return std::string(buf);
}

std::string DesktopView::TruncateFilenameGracefully(std::string_view filename, size_t max_chars) {
#ifdef _WIN32
    std::wstring wstr = Utf8ToW(filename);
    if (wstr.size() <= max_chars || max_chars < 12) {
        return std::string(filename);
    }
    size_t tail_len = std::min<size_t>(10, (max_chars - 3) / 3);
    size_t head_len = max_chars - 3 - tail_len;
    std::wstring truncated = wstr.substr(0, head_len) + L"..." + wstr.substr(wstr.size() - tail_len);
    return WToUtf8(truncated);
#else
    if (filename.size() <= max_chars || max_chars < 12) {
        return std::string(filename);
    }
    size_t tail_len = std::min<size_t>(10, (max_chars - 3) / 3);
    size_t head_len = max_chars - 3 - tail_len;
    return std::string(filename.substr(0, head_len)) + "..." +
           std::string(filename.substr(filename.size() - tail_len));
#endif
}

std::string DesktopView::FormatFriendlyTime(int64_t epoch_ms) {
    if (epoch_ms <= 0) return "—";
    std::time_t item_t = static_cast<std::time_t>(epoch_ms / 1000);
    std::time_t now_t = std::time(nullptr);
    std::tm item_tm{};
    std::tm now_tm{};
#ifdef _WIN32
    localtime_s(&item_tm, &item_t);
    localtime_s(&now_tm, &now_t);
#else
    localtime_r(&item_t, &item_tm);
    localtime_r(&now_t, &now_tm);
#endif
    int hour12 = item_tm.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = (item_tm.tm_hour >= 12) ? "PM" : "AM";
    char time_str[32];
    std::snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour12, item_tm.tm_min, ampm);

    if (item_tm.tm_year == now_tm.tm_year && item_tm.tm_yday == now_tm.tm_yday) {
        return "Today " + std::string(time_str);
    }
    if (item_tm.tm_year == now_tm.tm_year && item_tm.tm_yday == now_tm.tm_yday - 1) {
        return "Yesterday " + std::string(time_str);
    }
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int m_idx = std::clamp(item_tm.tm_mon, 0, 11);
    char date_str[32];
    if (item_tm.tm_year == now_tm.tm_year) {
        std::snprintf(date_str, sizeof(date_str), "%s %d", months[m_idx], item_tm.tm_mday);
    } else {
        std::snprintf(date_str, sizeof(date_str), "%s %d, %d", months[m_idx], item_tm.tm_mday, 1900 + item_tm.tm_year);
    }
    return std::string(date_str);
}

std::string DesktopView::FormatSizePretty(uint64_t bytes) {
    if (bytes == 0) return "0 B";
    char buf[32];
    double b = static_cast<double>(bytes);
    if (bytes >= 1073741824ULL) {
        double gib = b / 1073741824.0;
        std::snprintf(buf, sizeof(buf), "%.1f GB", gib);
    } else if (bytes >= 1048576ULL) {
        double mib = b / 1048576.0;
        if (mib >= 10.0) {
            std::snprintf(buf, sizeof(buf), (mib == static_cast<int>(mib)) ? "%.0f MB" : "%.1f MB", mib);
        } else {
            std::snprintf(buf, sizeof(buf), "%.2f MB", mib);
        }
    } else if (bytes >= 1024ULL) {
        double kib = b / 1024.0;
        std::snprintf(buf, sizeof(buf), (kib >= 100.0) ? "%.0f KB" : "%.1f KB", kib);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return std::string(buf);
}

std::string DesktopView::FormatSpeedKbps(uint64_t bps) {
    if (bps == 0) return "0 KB/s";
    char buf[64]{};
    if (bps < 1024ULL * 1024ULL) {
        uint64_t kbps = (bps + 512) / 1024;
        if (kbps == 0 && bps > 0) kbps = 1;
        std::snprintf(buf, sizeof(buf), "%llu KB/s", static_cast<unsigned long long>(kbps));
    } else if (bps < 1024ULL * 1024ULL * 1024ULL) {
        double mbps = static_cast<double>(bps) / (1024.0 * 1024.0);
        std::snprintf(buf, sizeof(buf), "%.2f MB/s", mbps);
    } else {
        double gbps = static_cast<double>(bps) / (1024.0 * 1024.0 * 1024.0);
        std::snprintf(buf, sizeof(buf), "%.2f GB/s", gbps);
    }
    return std::string(buf);
}

void DesktopView::ToggleSelectAll() {
    auto visible = GetFilteredAndSortedTasks();
    if (visible.empty()) {
        m_checked_task_ids.clear();
        m_select_all_checked = false;
        return;
    }
    bool all_checked = true;
    for (const auto& t : visible) {
        if (m_checked_task_ids.find(t.task_id) == m_checked_task_ids.end()) {
            all_checked = false;
            break;
        }
    }
    if (all_checked) {
        m_checked_task_ids.clear();
        m_select_all_checked = false;
    } else {
        for (const auto& t : visible) {
            m_checked_task_ids.insert(t.task_id);
        }
        m_select_all_checked = true;
    }
}

void DesktopView::ToggleTaskChecked(const std::string& task_id) {
    if (m_checked_task_ids.count(task_id)) {
        m_checked_task_ids.erase(task_id);
    } else {
        m_checked_task_ids.insert(task_id);
    }
    auto visible = GetFilteredAndSortedTasks();
    m_select_all_checked = (!visible.empty() && m_checked_task_ids.size() >= visible.size());
}

void DesktopView::SetTaskChecked(const std::string& task_id, bool checked) {
    if (checked) {
        m_checked_task_ids.insert(task_id);
    } else {
        m_checked_task_ids.erase(task_id);
    }
    auto visible = GetFilteredAndSortedTasks();
    m_select_all_checked = (!visible.empty() && m_checked_task_ids.size() >= visible.size());
}

void DesktopView::CycleSortDropdown() {
    switch (m_sort_column) {
        case SortColumn::kCreatedAt:
            SetSort(SortColumn::kSize, false);
            break;
        case SortColumn::kSize:
            SetSort(SortColumn::kStatus, true);
            break;
        case SortColumn::kStatus:
            SetSort(SortColumn::kSpeed, false);
            break;
        case SortColumn::kSpeed:
            SetSort(SortColumn::kFileName, true);
            break;
        case SortColumn::kFileName:
        default:
            SetSort(SortColumn::kCreatedAt, false);
            break;
    }
}

void DesktopView::CycleFilterDropdown() {
    switch (m_selected_category) {
        case SidebarCategory::kAllDownloads:
            SelectCategory(SidebarCategory::kInProgress);
            break;
        case SidebarCategory::kInProgress:
            SelectCategory(SidebarCategory::kQueued);
            break;
        case SidebarCategory::kQueued:
            SelectCategory(SidebarCategory::kCompleted);
            break;
        case SidebarCategory::kCompleted:
            SelectCategory(SidebarCategory::kFailed);
            break;
        case SidebarCategory::kFailed:
            SelectCategory(SidebarCategory::kVideoDownloads);
            break;
        case SidebarCategory::kVideoDownloads:
            SelectCategory(SidebarCategory::kScheduled);
            break;
        case SidebarCategory::kScheduled:
        default:
            SelectCategory(SidebarCategory::kAllDownloads);
            break;
    }
}

void DesktopView::SelectCategory(SidebarCategory category) {
    ClearHoverState();
    if (m_selected_category != category) {
        m_anim.prev_category_idx = static_cast<int>(m_selected_category);
        m_anim.current_category_idx = static_cast<int>(category);
        m_anim.category_transition_start_ms = GetTickCount64();
    }
    m_selected_category = category;
    auto visible = GetFilteredAndSortedTasks();
    bool still_visible = false;
    for (const auto& t : visible) {
        if (t.task_id == m_selected_task_id) {
            still_visible = true;
            break;
        }
    }
    if (!still_visible) {
        m_selected_task_id = visible.empty() ? "" : visible.front().task_id;
    }
    m_last_feedback_message = "Filtered category: " + CategoryToLabel(category);
}

void DesktopView::SetSidebarVisible(bool visible) {
    if (m_sidebar_visible != visible) {
        m_sidebar_visible = visible;
        ClearHoverState();
    }
}

void DesktopView::ToggleSidebarVisible() {
    SetSidebarVisible(!m_sidebar_visible);
}

std::string DesktopView::CategoryToLabel(SidebarCategory category) {
    switch (category) {
        case SidebarCategory::kAllDownloads:   return "All Downloads";
        case SidebarCategory::kInProgress:     return "In Progress";
        case SidebarCategory::kQueued:         return "Queued";
        case SidebarCategory::kCompleted:      return "Completed";
        case SidebarCategory::kFailed:         return "Failed";
        case SidebarCategory::kVideoDownloads: return "Video Downloads";
        case SidebarCategory::kScheduled:      return "Scheduled";
    }
    return "All Downloads";
}

bool DesktopView::MatchesCategory(const TaskModel& t, SidebarCategory cat) const {
    switch (cat) {
        case SidebarCategory::kAllDownloads:
            return true;
        case SidebarCategory::kInProgress:
            return t.state == TaskState::kProbing || t.state == TaskState::kDownloading ||
                   t.state == TaskState::kMuxing || t.state == TaskState::kVerifying ||
                   t.state == TaskState::kFinalizing;
        case SidebarCategory::kQueued:
            return t.state == TaskState::kQueued || (t.state == TaskState::kPaused && !t.scheduled);
        case SidebarCategory::kCompleted:
            return t.state == TaskState::kCompleted;
        case SidebarCategory::kFailed:
            return t.state == TaskState::kFailed || t.state == TaskState::kCanceled;
        case SidebarCategory::kVideoDownloads:
            return t.task_type == TaskType::kStreamHls || t.task_type == TaskType::kStreamDash ||
                   t.task_type == TaskType::kStreamMss || t.category == "Video";
        case SidebarCategory::kScheduled:
            return t.scheduled;
    }
    return true;
}

bool DesktopView::MatchesSearch(const TaskModel& t) const {
    if (m_search_query.empty()) return true;
    std::string q = ToLower(m_search_query);
    return ToLower(t.file_name).find(q) != std::string::npos ||
           ToLower(t.url).find(q) != std::string::npos ||
           ToLower(t.page_url).find(q) != std::string::npos ||
           ToLower(t.category).find(q) != std::string::npos;
}

CategoryCounts DesktopView::ComputeCategoryCounts() const {
    CategoryCounts counts{};
    auto tasks = m_controller.ListTasks();
    for (const auto& t : tasks) {
        ++counts.all_downloads;
        if (MatchesCategory(t, SidebarCategory::kInProgress)) ++counts.in_progress;
        if (MatchesCategory(t, SidebarCategory::kQueued)) ++counts.queued;
        if (MatchesCategory(t, SidebarCategory::kCompleted)) ++counts.completed;
        if (MatchesCategory(t, SidebarCategory::kFailed)) ++counts.failed;
        if (MatchesCategory(t, SidebarCategory::kVideoDownloads)) ++counts.video_downloads;
        if (MatchesCategory(t, SidebarCategory::kScheduled)) ++counts.scheduled;
    }
    return counts;
}

void DesktopView::SetSearchQuery(std::string query) {
    ClearHoverState();
    m_search_query = std::move(query);
}

void DesktopView::SetSort(SortColumn column, bool ascending) {
    ClearHoverState();
    m_sort_column = column;
    m_sort_ascending = ascending;
}

void DesktopView::ToggleSortColumn(SortColumn column) {
    if (m_sort_column == column) {
        m_sort_ascending = !m_sort_ascending;
    } else {
        m_sort_column = column;
        m_sort_ascending = true;
    }
}

void DesktopView::SelectTask(const std::string& task_id) {
    m_selected_task_id = task_id;
}

void DesktopView::SelectNextRow() {
    auto list = GetFilteredAndSortedTasks();
    if (list.empty()) {
        m_selected_task_id.clear();
        return;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].task_id == m_selected_task_id) {
            size_t next = std::min(i + 1, list.size() - 1);
            m_selected_task_id = list[next].task_id;
            return;
        }
    }
    m_selected_task_id = list.front().task_id;
}

void DesktopView::SelectPrevRow() {
    auto list = GetFilteredAndSortedTasks();
    if (list.empty()) {
        m_selected_task_id.clear();
        return;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].task_id == m_selected_task_id) {
            size_t prev = (i > 0) ? (i - 1) : 0;
            m_selected_task_id = list[prev].task_id;
            return;
        }
    }
    m_selected_task_id = list.front().task_id;
}

void DesktopView::SelectFirstRow() {
    auto list = GetFilteredAndSortedTasks();
    m_selected_task_id = list.empty() ? "" : list.front().task_id;
}

void DesktopView::SelectLastRow() {
    auto list = GetFilteredAndSortedTasks();
    m_selected_task_id = list.empty() ? "" : list.back().task_id;
}

void DesktopView::ScrollBy(int delta_rows) {
    ClearHoverState();
    auto filtered = GetFilteredAndSortedTasks();
    int row_h = DesignSystem::ScaleForDpi(52, m_metrics.dpi);
    int hdr_h = DesignSystem::ScaleForDpi(38, m_metrics.dpi);
    int bar_h = DesignSystem::ScaleForDpi(48, m_metrics.dpi);
    int sb_h = DesignSystem::ScaleForDpi(36, m_metrics.dpi);
    int body_h = std::max(50, m_window_height - bar_h - hdr_h - sb_h);
    size_t visible_count = static_cast<size_t>(std::max(1, body_h / row_h));

    if (filtered.size() <= visible_count) {
        m_scroll_offset = 0;
        return;
    }
    size_t max_scroll = filtered.size() - visible_count;
    int cur = static_cast<int>(m_scroll_offset) + delta_rows;
    if (cur < 0) cur = 0;
    if (cur > static_cast<int>(max_scroll)) cur = static_cast<int>(max_scroll);
    m_scroll_offset = static_cast<size_t>(cur);
}

bool DesktopView::ScrollTaskIntoView(const std::string& task_id, bool select_row) {
    if (task_id.empty()) return false;
    auto filtered = GetFilteredAndSortedTasks();
    if (filtered.empty()) return false;

    // Check if task exists in currently filtered/sorted list
    int found_idx = -1;
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (filtered[i].task_id == task_id) {
            found_idx = static_cast<int>(i);
            break;
        }
    }

    // If not visible under current filter/search, switch category to AllDownloads and clear search
    if (found_idx < 0) {
        auto task_opt = m_controller.GetTask(task_id);
        if (!task_opt.has_value()) return false;
        m_selected_category = SidebarCategory::kAllDownloads;
        m_search_query.clear();
        filtered = GetFilteredAndSortedTasks();
        for (size_t i = 0; i < filtered.size(); ++i) {
            if (filtered[i].task_id == task_id) {
                found_idx = static_cast<int>(i);
                break;
            }
        }
        if (found_idx < 0) return false;
    }

    if (select_row) {
        m_selected_task_id = task_id;
    }

    int row_h = DesignSystem::ScaleForDpi(52, m_metrics.dpi);
    int hdr_h = DesignSystem::ScaleForDpi(38, m_metrics.dpi);
    int bar_h = DesignSystem::ScaleForDpi(48, m_metrics.dpi);
    int sb_h = DesignSystem::ScaleForDpi(36, m_metrics.dpi);
    int body_h = std::max(50, m_window_height - bar_h - hdr_h - sb_h);
    size_t visible_count = static_cast<size_t>(std::max(1, body_h / row_h));

    if (filtered.size() <= visible_count) {
        m_scroll_offset = 0;
    } else {
        size_t max_scroll = filtered.size() - visible_count;
        size_t target_idx = static_cast<size_t>(found_idx);
        if (target_idx < m_scroll_offset) {
            m_scroll_offset = target_idx;
        } else if (target_idx >= m_scroll_offset + visible_count) {
            m_scroll_offset = target_idx - visible_count + 1;
        }
        if (m_scroll_offset > max_scroll) {
            m_scroll_offset = max_scroll;
        }
    }
    return true;
}

bool DesktopView::CheckAndAutoScrollNewOrStartedDownloads() {
    auto all_tasks = m_controller.ListTasks();

    if (!m_known_tasks_initialized) {
        m_known_tasks_initialized = true;
        for (const auto& t : all_tasks) {
            m_known_task_states[t.task_id] = t.state;
        }
        return false;
    }

    std::string task_to_focus;

    // Check for newly added downloads
    for (const auto& t : all_tasks) {
        auto it = m_known_task_states.find(t.task_id);
        if (it == m_known_task_states.end()) {
            // Brand new download task detected!
            task_to_focus = t.task_id;
            break;
        }
    }

    // Refresh known state cache
    m_known_task_states.clear();
    for (const auto& t : all_tasks) {
        m_known_task_states[t.task_id] = t.state;
    }

    if (!task_to_focus.empty()) {
        return ScrollTaskIntoView(task_to_focus, true);
    }

    return false;
}

int DesktopView::GetColumnWidth(size_t col_idx) const {
    if (col_idx < m_column_widths.size()) {
        return m_column_widths[col_idx];
    }
    return 100;
}

void DesktopView::SetColumnWidth(size_t col_idx, int width) {
    if (col_idx < m_column_widths.size()) {
        m_column_widths[col_idx] = std::max(50, width);
    }
}

void DesktopView::StartColumnResize(int col_idx, int mouse_x) {
    if (col_idx >= 0 && col_idx < static_cast<int>(m_column_widths.size())) {
        m_resizing_col_index = col_idx;
        m_resize_start_x = mouse_x;
        m_resize_orig_width = m_column_widths[static_cast<size_t>(col_idx)];
    }
}

void DesktopView::UpdateColumnResize(int mouse_x) {
    if (m_resizing_col_index >= 0 && m_resizing_col_index < static_cast<int>(m_column_widths.size())) {
        int delta = mouse_x - m_resize_start_x;
        int min_w = 60;
        int max_w = 1200;
        if (m_resizing_col_index == 0) { // Name
            min_w = 140;
            max_w = 1600;
        } else if (m_resizing_col_index == 1) { // Size
            min_w = 60;
            max_w = 350;
        } else if (m_resizing_col_index == 2) { // Status
            min_w = 90;
            max_w = 450;
        } else if (m_resizing_col_index == 3) { // Download
            min_w = 75;
            max_w = 350;
        } else if (m_resizing_col_index == 4) { // Upload
            min_w = 65;
            max_w = 300;
        } else if (m_resizing_col_index == 5) { // Added
            min_w = 80;
            max_w = 350;
        }
        int new_w = std::clamp(m_resize_orig_width + delta, min_w, max_w);
        m_column_widths[static_cast<size_t>(m_resizing_col_index)] = new_w;
    }
}

void DesktopView::EndColumnResize() {
    m_resizing_col_index = -1;
    SaveColumnWidthsToSettings();
}

int DesktopView::HitTestColumnDivider(int x, int y, size_t scroll_offset) const {
    auto geom = ComputeLayoutGeometry(scroll_offset);
    if (y < geom.table_header_rect.top || y > geom.table_header_rect.bottom) {
        return -1;
    }
    for (size_t i = 0; i < geom.table_columns.size() && i <= 5; ++i) {
        int edge_x = geom.table_columns[i].rect.right;
        if (std::abs(x - edge_x) <= 4) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void DesktopView::SaveColumnWidthsToSettings() {
    if (m_column_widths.size() >= 6) {
        std::string val = std::to_string(m_column_widths[0]) + "," +
                          std::to_string(m_column_widths[1]) + "," +
                          std::to_string(m_column_widths[2]) + "," +
                          std::to_string(m_column_widths[3]) + "," +
                          std::to_string(m_column_widths[4]) + "," +
                          std::to_string(m_column_widths[5]);
        auto s = m_controller.GetSettings();
        if (s.table_column_widths != val) {
            s.table_column_widths = val;
            m_controller.UpdateSettings(s);
        }
    }
}

void DesktopView::LoadColumnWidthsFromSettings() {
    auto s = m_controller.GetSettings();
    if (!s.table_column_widths.empty()) {
        std::vector<int> widths;
        std::stringstream ss(s.table_column_widths);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                widths.push_back(std::stoi(token));
            } catch (...) {}
        }
        if (widths.size() >= 6) {
            m_column_widths = std::move(widths);
        }
    }
}

std::vector<TaskModel> DesktopView::GetFilteredAndSortedTasks() const {
    auto all = m_controller.ListTasks();
    std::vector<TaskModel> filtered;
    filtered.reserve(all.size());
    for (const auto& t : all) {
        if (MatchesCategory(t, m_selected_category) && MatchesSearch(t)) {
            filtered.push_back(t);
        }
    }

    auto compute_pct = [](const TaskModel& t) -> double {
        if (t.state == TaskState::kCompleted) return 100.0;
        double pct = (t.total_bytes > 0)
            ? (static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes)
            : t.stream_percent;
        if (t.state == TaskState::kFinalizing) return std::clamp(pct, 99.8, 99.9);
        if (t.state == TaskState::kVerifying) return std::clamp(pct, 99.0, 99.5);
        if (t.state == TaskState::kMuxing) return std::clamp(pct, 98.0, 99.0);
        return std::clamp(pct, 0.0, 99.0);
    };

    std::sort(filtered.begin(), filtered.end(), [&](const TaskModel& a, const TaskModel& b) {
        int cmp = 0;
        switch (m_sort_column) {
            case SortColumn::kFileName:
                cmp = ToLower(a.file_name).compare(ToLower(b.file_name));
                break;
            case SortColumn::kDownloadType:
                cmp = TaskTypeToString(a.task_type).compare(TaskTypeToString(b.task_type));
                break;
            case SortColumn::kStatus:
                cmp = TaskStateToString(a.state).compare(TaskStateToString(b.state));
                break;
            case SortColumn::kProgress: {
                double pa = compute_pct(a);
                double pb = compute_pct(b);
                cmp = (pa < pb) ? -1 : (pa > pb ? 1 : 0);
                break;
            }
            case SortColumn::kSize:
                cmp = (a.total_bytes < b.total_bytes) ? -1 : (a.total_bytes > b.total_bytes ? 1 : 0);
                break;
            case SortColumn::kSpeed:
                cmp = (a.speed_bps < b.speed_bps) ? -1 : (a.speed_bps > b.speed_bps ? 1 : 0);
                break;
            case SortColumn::kEta: {
                uint64_t ra = (a.total_bytes > a.downloaded_bytes) ? (a.total_bytes - a.downloaded_bytes) : 0;
                uint64_t rb = (b.total_bytes > b.downloaded_bytes) ? (b.total_bytes - b.downloaded_bytes) : 0;
                uint64_t ea = (a.speed_bps > 0) ? (ra / a.speed_bps) : UINT64_MAX;
                uint64_t eb = (b.speed_bps > 0) ? (rb / b.speed_bps) : UINT64_MAX;
                cmp = (ea < eb) ? -1 : (ea > eb ? 1 : 0);
                break;
            }
            case SortColumn::kCreatedAt:
            default:
                cmp = (a.created_at_ms < b.created_at_ms) ? -1 : (a.created_at_ms > b.created_at_ms ? 1 : 0);
                break;
        }
        return m_sort_ascending ? (cmp < 0) : (cmp > 0);
    });

    return filtered;
}

std::vector<FormattedTaskRow> DesktopView::BuildVisibleTaskRows(size_t scroll_offset, size_t max_visible_rows) const {
    auto list = GetFilteredAndSortedTasks();
    std::vector<FormattedTaskRow> rows;
    if (scroll_offset >= list.size()) return rows;

    size_t end_idx = std::min(list.size(), scroll_offset + max_visible_rows);
    rows.reserve(end_idx - scroll_offset);

    size_t max_fn_chars = IsCompactColumnsMode() ? 28 : 40;

    for (size_t i = scroll_offset; i < end_idx; ++i) {
        const auto& t = list[i];
        FormattedTaskRow row;
        row.task_id = t.task_id;
        row.state = t.state;
        row.category = t.category;
        row.full_filename_tooltip = t.file_name;
        row.display_filename = TruncateFilenameGracefully(t.file_name, max_fn_chars);
        row.is_filename_truncated = (row.display_filename != t.file_name);
        row.is_stream = (t.task_type != TaskType::kHttpFile);

        if (t.task_type == TaskType::kStreamHls) row.file_type_badge = "HLS STREAM";
        else if (t.task_type == TaskType::kStreamDash) row.file_type_badge = "DASH STREAM";
        else row.file_type_badge = t.range_supported ? "HTTP MULTI" : "HTTP FILE";

        if (!t.status_detail.empty()) {
            row.status_text = t.status_detail;
        } else {
            switch (t.state) {
                case TaskState::kQueued:      row.status_text = "Queued"; break;
                case TaskState::kProbing:     row.status_text = row.is_stream ? "Probing manifest..." : "Probing server..."; break;
                case TaskState::kDownloading: row.status_text = row.is_stream ? "Downloading video segments..." : "Downloading..."; break;
                case TaskState::kMuxing:      row.status_text = "Merging tracks (FFmpeg)..."; break;
                case TaskState::kVerifying:   row.status_text = "Verifying output..."; break;
                case TaskState::kFinalizing:  row.status_text = "Finalizing output..."; break;
                case TaskState::kPaused:      row.status_text = t.scheduled ? ("Scheduled (" + t.schedule_time_local + ")") : "Paused"; break;
                case TaskState::kCompleted:   row.status_text = "Completed"; break;
                case TaskState::kFailed:      row.status_text = "Failed (" + (t.error_code.empty() ? "Error" : t.error_code) + ")"; break;
                case TaskState::kCanceled:    row.status_text = "Canceled"; break;
            }
        }

        // Semantic status indicator & progress fill colors
        switch (t.state) {
            case TaskState::kCompleted:
                row.status_indicator_color = m_palette.status_success;
                row.progress_fill_color = m_palette.status_success;
                break;
            case TaskState::kFailed:
            case TaskState::kCanceled:
                row.status_indicator_color = m_palette.status_danger;
                row.progress_fill_color = m_palette.status_danger;
                break;
            case TaskState::kPaused:
                row.status_indicator_color = m_palette.status_warning;
                row.progress_fill_color = m_palette.status_warning;
                break;
            case TaskState::kProbing:
            case TaskState::kMuxing:
            case TaskState::kVerifying:
            case TaskState::kFinalizing:
                row.status_indicator_color = m_palette.accent_violet;
                row.progress_fill_color = row.is_stream ? m_palette.badge_stream : m_palette.accent_cyan;
                break;
            case TaskState::kDownloading:
                row.status_indicator_color = row.is_stream ? m_palette.badge_stream : m_palette.accent_cyan;
                row.progress_fill_color = row.is_stream ? m_palette.badge_stream : m_palette.accent_primary;
                break;
            case TaskState::kQueued:
            default:
                row.status_indicator_color = m_palette.text_secondary;
                row.progress_fill_color = row.is_stream ? m_palette.badge_stream : m_palette.accent_primary;
                break;
        }

        std::ostringstream prog_ss;
        if (t.state == TaskState::kCompleted) {
            row.progress_fraction = 1.0;
            row.progress_text = "100.0%";
        } else if (t.total_bytes > 0) {
            double pct = (static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes);
            if (t.state == TaskState::kFinalizing) {
                pct = std::clamp(pct, 99.8, 99.9);
            } else if (t.state == TaskState::kVerifying) {
                pct = std::clamp(pct, 99.0, 99.5);
            } else if (t.state == TaskState::kMuxing) {
                pct = std::clamp(pct, 98.0, 99.0);
            } else {
                pct = std::clamp(pct, 0.0, 99.0);
            }
            row.progress_fraction = pct / 100.0;
            prog_ss << std::fixed << std::setprecision(1) << pct << "%";
            row.progress_text = prog_ss.str();
        } else if (row.is_stream && t.total_segments > 0) {
            double pct = (static_cast<double>(t.completed_segments) * 100.0) / static_cast<double>(t.total_segments);
            if (t.state == TaskState::kFinalizing) {
                pct = std::clamp(pct, 99.8, 99.9);
            } else if (t.state == TaskState::kVerifying) {
                pct = std::clamp(pct, 99.0, 99.5);
            } else if (t.state == TaskState::kMuxing) {
                pct = std::clamp(pct, 98.0, 99.0);
            } else {
                pct = std::clamp(pct, 0.0, 99.0);
            }
            row.progress_fraction = pct / 100.0;
            prog_ss << std::fixed << std::setprecision(1) << pct << "% (" << t.completed_segments << "/" << t.total_segments << " segs)";
            row.progress_text = prog_ss.str();
        } else {
            row.progress_fraction = 0.0;
            row.progress_indeterminate = (t.state == TaskState::kDownloading || t.state == TaskState::kProbing ||
                                          t.state == TaskState::kMuxing || t.state == TaskState::kVerifying ||
                                          t.state == TaskState::kFinalizing);
            row.progress_text = FormatBytes(t.downloaded_bytes) + " (Size Unknown)";
        }

        if (t.state == TaskState::kCompleted) {
            row.size_text = (t.total_bytes > 0) ? FormatSizePretty(t.total_bytes) : (t.downloaded_bytes > 0 ? FormatSizePretty(t.downloaded_bytes) : "Unknown");
        } else if (t.total_bytes > 0) {
            if (t.downloaded_bytes > 0) {
                row.size_text = FormatSizePretty(t.downloaded_bytes) + " / " + FormatSizePretty(t.total_bytes);
            } else {
                row.size_text = FormatSizePretty(t.total_bytes);
            }
        } else {
            if (t.downloaded_bytes > 0) {
                row.size_text = FormatSizePretty(t.downloaded_bytes) + " / Unknown";
            } else {
                row.size_text = "Unknown";
            }
        }
        row.added_time_text = FormatFriendlyTime(t.created_at_ms);
        row.upload_speed_text = (t.upload_speed_bps > 0) ? FormatSpeed(t.upload_speed_bps) : "0 KB/s";
        row.checked = (m_checked_task_ids.count(t.task_id) > 0);

        if (t.state == TaskState::kDownloading) {
            double pct = (t.total_bytes > 0)
                ? std::clamp((static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes), 0.0, 99.0)
                : (row.is_stream && t.total_segments > 0
                    ? std::clamp((static_cast<double>(t.completed_segments) * 100.0) / static_cast<double>(t.total_segments), 0.0, 99.0)
                    : 0.0);
            int ipct = static_cast<int>(pct);
            row.status_text = (t.total_bytes > 0 || (row.is_stream && t.total_segments > 0)) ? ("Downloading " + std::to_string(ipct) + "%") : "Downloading...";
            row.status_indicator_color = {0xFF, 0x47, 0x57, 255};
        } else if (t.state == TaskState::kProbing) {
            row.status_text = "Connecting...";
            row.status_indicator_color = {0x8B, 0x5C, 0xF6, 255};
        } else if (t.state == TaskState::kMuxing) {
            row.status_text = "Merging (FFmpeg)...";
            row.status_indicator_color = {0x8B, 0x5C, 0xF6, 255};
        } else if (t.state == TaskState::kVerifying) {
            row.status_text = "Verifying...";
            row.status_indicator_color = {0x8B, 0x5C, 0xF6, 255};
        } else if (t.state == TaskState::kPaused) {
            if (t.scheduled) {
                row.status_text = "Scheduled (" + t.schedule_time_local + ")";
            } else if (t.total_bytes > 0) {
                double pct = (static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes);
            if (t.state == TaskState::kFinalizing) {
                pct = std::clamp(pct, 99.8, 99.9);
            } else if (t.state == TaskState::kVerifying) {
                pct = std::clamp(pct, 99.0, 99.5);
            } else if (t.state == TaskState::kMuxing) {
                pct = std::clamp(pct, 98.0, 99.0);
            } else {
                pct = std::clamp(pct, 0.0, 99.0);
            }
                row.status_text = "Paused " + std::to_string(static_cast<int>(pct)) + "%";
            } else {
                row.status_text = "Paused";
            }
            row.status_indicator_color = m_palette.status_warning;
        } else if (t.state == TaskState::kQueued) {
            row.status_text = "Queued";
            row.status_indicator_color = {0x72, 0x73, 0x7A, 255};
        } else if (t.state == TaskState::kCompleted) {
            row.status_text = "Completed";
            row.status_indicator_color = m_palette.status_success;
        } else if (t.state == TaskState::kFailed) {
            row.status_text = t.error_code.empty() ? "Failed" : ("Failed (" + t.error_code + ")");
            row.status_indicator_color = m_palette.status_danger;
        } else if (t.state == TaskState::kCanceled) {
            row.status_text = "Canceled";
            row.status_indicator_color = m_palette.status_danger;
        }

        if (!t.error_code.empty()) {
            row.full_status_tooltip = t.error_code + (!t.error_message.empty() ? (": " + t.error_message) : "");
        } else if (!t.status_detail.empty()) {
            row.full_status_tooltip = t.status_detail;
        } else {
            row.full_status_tooltip = row.status_text;
        }

        row.speed_text = (t.state == TaskState::kDownloading) ? FormatSpeedKbps(t.speed_bps) : "0 KB/s";
        uint64_t rem_bytes = (t.total_bytes > t.downloaded_bytes) ? (t.total_bytes - t.downloaded_bytes) : 0;
        row.eta_text = (t.state == TaskState::kDownloading) ? FormatEta(rem_bytes, t.speed_bps) : "—";

        switch (t.state) {
            case TaskState::kDownloading:
            case TaskState::kProbing:
                row.primary_action_label = "Pause";
                break;
            case TaskState::kPaused:
                row.primary_action_label = "Resume";
                break;
            case TaskState::kQueued:
                row.primary_action_label = "Pause";
                break;
            case TaskState::kFailed:
            case TaskState::kCanceled:
                row.primary_action_label = "Retry";
                break;
            case TaskState::kCompleted:
                row.primary_action_label = "Open";
                break;
            default:
                row.primary_action_label = "Wait";
                break;
        }
        row.selected = (t.task_id == m_selected_task_id);
        rows.push_back(std::move(row));
    }
    return rows;
}

ToolbarState DesktopView::ComputeToolbarState() const {
    ToolbarState st;
    st.can_start_all = true;
    st.can_pause_all = true;
    auto all = m_controller.ListTasks();
    for (const auto& t : all) {
        if (t.state == TaskState::kCompleted || t.state == TaskState::kCanceled) {
            st.can_clear_history = true;
            break;
        }
    }

    if (m_selected_task_id.empty()) return st;
    auto sel = m_controller.GetTask(m_selected_task_id);
    if (!sel.has_value()) return st;

    const auto& t = *sel;
    st.can_remove = true;
    st.can_open_folder = !t.save_dir.empty();

    switch (t.state) {
        case TaskState::kQueued:
            st.can_pause = true;
            st.can_cancel = true;
            break;
        case TaskState::kProbing:
        case TaskState::kDownloading:
        case TaskState::kMuxing:
            st.can_pause = true;
            st.can_cancel = true;
            break;
        case TaskState::kPaused:
            st.can_start = true;
            st.can_resume = true;
            st.can_cancel = true;
            st.can_delete_file = true;
            break;
        case TaskState::kFailed:
        case TaskState::kCanceled:
            st.can_start = true;
            st.can_retry = true;
            break;
        case TaskState::kCompleted:
            st.can_open_file = true;
            st.can_delete_file = true;
            break;
        default:
            break;
    }
    return st;
}

TaskDetailsViewModel DesktopView::BuildSelectedTaskDetails() const {
    TaskDetailsViewModel vm;
    if (m_selected_task_id.empty()) return vm;
    auto sel = m_controller.GetTask(m_selected_task_id);
    if (!sel.has_value()) return vm;

    const auto& t = *sel;
    vm.has_selection = true;
    vm.task_id = t.task_id;
    vm.file_name = t.file_name;
    vm.redacted_source_url = Logger::RedactSensitiveData(t.url);
    vm.redacted_page_url = t.page_url.empty() ? "—" : Logger::RedactSensitiveData(t.page_url);
    vm.destination_path = (std::filesystem::path(t.save_dir) / t.file_name).string();
    vm.size_summary = (t.total_bytes > 0)
        ? (FormatBytes(t.downloaded_bytes) + " of " + FormatBytes(t.total_bytes))
        : (FormatBytes(t.downloaded_bytes) + " (Total size unknown)");
    vm.progress_summary = t.status_detail.empty() ? TaskStateToString(t.state) : t.status_detail;
    vm.speed_summary = FormatSpeed(t.speed_bps);
    vm.created_time_str = FormatTimestampUtc(t.created_at_ms);
    vm.updated_time_str = FormatTimestampUtc(t.updated_at_ms);
    vm.completed_time_str = FormatTimestampUtc(t.completed_at_ms);
    vm.retry_count = t.retry_count;

    if (t.task_type == TaskType::kHttpFile) {
        vm.engine_name = "Brisk Segmented HTTP/1.1 Engine (" + std::to_string(t.max_connections) + " max connections)";
        vm.stream_format = t.range_supported ? "HTTP/1.1 Byte-Range (206 Partial Content)" : "HTTP/1.1 Single-Stream (200 OK)";
        vm.selected_tracks_summary = "N/A (Direct File Transfer — " + std::to_string(t.segments.size()) + " segments)";
    } else {
        vm.engine_name = "N_m3u8DL-RE + FFmpeg Managed Stream Pipeline";
        vm.stream_format = (t.task_type == TaskType::kStreamDash) ? "MPEG-DASH (.mpd)" : "Apple HLS (.m3u8)";
        vm.selected_tracks_summary = "Video: " + t.stream_options.video_filter +
                                     " | Audio: " + t.stream_options.audio_filter +
                                     " | Subtitles: " + t.stream_options.subtitle_filter +
                                     " | Container: ." + t.stream_options.output_format;
    }

    std::ostringstream hdr_ss;
    for (const auto& [k, v] : t.headers) {
        hdr_ss << k << ": " << Logger::RedactSensitiveData(v) << "; ";
    }
    if (!t.auth_headers.empty()) {
        hdr_ss << "Auth/Cookies: [" << t.auth_headers.size() << " protected credential header(s) — REDACTED]";
    }
    vm.redacted_headers_summary = hdr_ss.str().empty() ? "Default headers" : hdr_ss.str();

    vm.error_code = t.error_code;
    if (t.error_code.empty()) {
        vm.error_explanation = "No errors.";
    } else if (t.error_code == "ERR_HTTP_403_EXPIRED_OR_AUTH") {
        vm.error_explanation = t.error_message + " — The server rejected the link or the signed token expired. Use 'Refresh URL' or re-capture from your browser.";
    } else if (t.error_code == "ERR_STREAM_DRM_PROTECTED") {
        vm.error_explanation = t.error_message + " — Encrypted with commercial DRM (Widevine/PlayReady/FairPlay), which Radium does not circumvent.";
    } else if (t.error_code == "ERR_DISK_FULL") {
        vm.error_explanation = t.error_message + " — Free up disk space in the destination folder and click Resume or Retry.";
    } else {
        vm.error_explanation = t.error_message;
    }

    vm.speed_limit_summary = (t.speed_limit_bps > 0) ? FormatSpeed(t.speed_limit_bps) : "Unlimited (Global Cap)";
    if (!t.alternate_urls.empty()) {
        vm.alternate_urls_summary = std::to_string(t.alternate_urls.size()) + " mirror URL(s) configured";
    } else {
        vm.alternate_urls_summary = "None (Primary only)";
    }
    if (t.scheduled) {
        vm.schedule_summary = "Starts at " + t.schedule_time_local;
        if (!t.schedule_stop_time_local.empty()) {
            vm.schedule_summary += " | Stops at " + t.schedule_stop_time_local;
        }
    } else if (!t.schedule_stop_time_local.empty()) {
        vm.schedule_summary = "Scheduled stop at " + t.schedule_stop_time_local;
    } else {
        vm.schedule_summary = "Manual queue order";
    }

    return vm;
}

StatusBarViewModel DesktopView::BuildStatusBar() const {
    StatusBarViewModel sb;
    auto tasks = m_controller.ListTasks();
    for (const auto& t : tasks) {
        if (t.state == TaskState::kProbing || t.state == TaskState::kDownloading ||
            t.state == TaskState::kMuxing || t.state == TaskState::kVerifying) {
            ++sb.active_downloads;
            sb.total_speed_bps += t.speed_bps;
        } else if (t.state == TaskState::kQueued) {
            ++sb.remaining_queued;
        }
        sb.total_upload_speed_bps += t.upload_speed_bps;
    }
    sb.formatted_total_speed = FormatSpeed(sb.total_speed_bps);
    sb.formatted_total_upload_speed = FormatSpeed(sb.total_upload_speed_bps);
    sb.speed_limit_state = IsSpeedLimiterActive() ? "ON" : "OFF";
    sb.browser_ipc_listening = (m_ipc_server != nullptr);
    sb.browser_capture_enabled = m_controller.GetSettings().browser_capture_enabled;
    sb.workers_ready = m_controller.AreWorkersAvailable();

    std::ostringstream msg;
    msg << "Active: " << sb.active_downloads
        << " | Speed: " << sb.formatted_total_speed
        << " | Upload: " << sb.formatted_total_upload_speed
        << " | Queued: " << sb.remaining_queued
        << " | Browser Capture: " << (sb.browser_capture_enabled ? "Enabled" : "Paused")
        << " | Stream Workers: " << (sb.workers_ready ? "Ready" : "Missing");
    sb.status_message = msg.str();
    return sb;
}

std::string DesktopView::GetEmptyStateMessage() const {
    auto all = m_controller.ListTasks();
    if (all.empty()) {
        return "No downloads yet. Click '+ Add URL' (Ctrl+N) or use the Radium Browser Extension to capture files and video streams.";
    }
    if (!m_search_query.empty()) {
        return "No downloads match the search filter \"" + m_search_query + "\". Clear the search box to view all tasks.";
    }
    switch (m_selected_category) {
        case SidebarCategory::kInProgress:
            return "No active transfers right now. Select a paused task and click Resume, or add a new URL.";
        case SidebarCategory::kQueued:
            return "No queued downloads waiting to start.";
        case SidebarCategory::kCompleted:
            return "No completed downloads in history yet.";
        case SidebarCategory::kFailed:
            return "No failed downloads. All transfers are healthy.";
        case SidebarCategory::kVideoDownloads:
            return "No video or HLS/DASH stream downloads yet. Open a video page with the Radium Browser Extension enabled.";
        case SidebarCategory::kScheduled:
            return "No scheduled tasks configured.";
        default:
            return "No tasks in this view.";
    }
}

// -----------------------------------------------------------------------------
// Professional Download Manager Settings Center Implementation
// -----------------------------------------------------------------------------

void DesktopView::OpenSettingsCenter(SettingsCategory initial_category) {
    m_settings_center_open = true;
    m_anim.settings_open_start_ms = GetTickCount64();
    m_settings_category = initial_category;
    m_draft_settings = m_controller.GetSettings();
    m_settings_validation_key.clear();
    m_settings_validation_error.clear();
    m_last_feedback_message = "Opened Settings Dialog";
}

void DesktopView::CloseSettingsCenter(bool revert_unsaved) {
    if (revert_unsaved) {
        RevertSettingsChanges();
    }
    m_settings_center_open = false;
    m_settings_search_query.clear();
}

void DesktopView::SelectSettingsCategory(SettingsCategory category) {
    m_settings_category = category;
    m_last_feedback_message = "Settings category: " + SettingsCategoryToLabel(category);
}

std::string DesktopView::SettingsCategoryToLabel(SettingsCategory category) {
    switch (category) {
        case SettingsCategory::kGeneral:             return "General";
        case SettingsCategory::kDownloads:           return "Downloads & Queue";
        case SettingsCategory::kSaveLocations:       return "Save Locations";
        case SettingsCategory::kConnectionsAndSpeed: return "Connections & Speed";
        case SettingsCategory::kBrowserIntegration:  return "Browser Integration";
        case SettingsCategory::kFileTypes:           return "File Types";
        case SettingsCategory::kNotifications:       return "Notifications";
        case SettingsCategory::kAboutAndDiagnostics: return "About & Diagnostics";
    }
    return "General";
}

void DesktopView::SetSettingsSearchQuery(std::string query) {
    m_settings_search_query = std::move(query);
}

void DesktopView::SetDraftSettings(const AppSettings& draft) {
    m_draft_settings = draft;
    auto v = ValidateSettingsInput(m_draft_settings);
    if (!v.ok()) {
        m_settings_validation_error = v.error->message;
    } else {
        m_settings_validation_key.clear();
        m_settings_validation_error.clear();
    }
}

bool DesktopView::HasUnsavedSettingsChanges() const {
    return m_draft_settings != m_controller.GetSettings();
}

Result<void> DesktopView::ValidateSettingsInput(const AppSettings& candidate) const {
    if (candidate.max_concurrent_tasks < 1 || candidate.max_concurrent_tasks > 16) {
        return Result<void>::Err("ERR_SETTINGS_CONCURRENT_RANGE", "Simultaneous downloads must be between 1 and 16.");
    }
    if (candidate.default_connections_per_task < 1 || candidate.default_connections_per_task > 32) {
        return Result<void>::Err("ERR_SETTINGS_CONNECTIONS_RANGE", "Default connections per task must be between 1 and 32.");
    }
    if (candidate.default_download_dir.empty()) {
        return Result<void>::Err("ERR_SETTINGS_EMPTY_SAVE_DIR", "Default download directory cannot be empty.");
    }
    for (char c : candidate.default_download_dir) {
        if (static_cast<unsigned char>(c) < 0x20) {
            return Result<void>::Err("ERR_SETTINGS_INVALID_SAVE_DIR", "Default download directory contains invalid control characters.");
        }
    }
    if (candidate.stall_timeout_ms < 500 || candidate.stall_timeout_ms > 60000) {
        return Result<void>::Err("ERR_SETTINGS_STALL_TIMEOUT", "Segment stall timeout must be between 500 ms and 60,000 ms.");
    }
    if (candidate.min_split_bytes < 65536ULL || candidate.min_split_bytes > 10485760ULL) {
        return Result<void>::Err("ERR_SETTINGS_MIN_SPLIT", "Minimum segment split size must be between 64 KiB and 10 MiB.");
    }
    if (candidate.min_capture_size_bytes > 10737418240ULL) {
        return Result<void>::Err("ERR_SETTINGS_CAPTURE_SIZE", "Minimum browser capture size cannot exceed 10 GiB.");
    }
    return Result<void>::Ok();
}

Result<void> DesktopView::SetDraftSettingByKey(std::string_view key, std::string_view value_str) {
    auto parse_u64 = [](std::string_view sv, uint64_t& out) -> bool {
        if (sv.empty()) return false;
        std::string s(sv);
        char* endp = nullptr;
        long long v = std::strtoll(s.c_str(), &endp, 10);
        if (endp == s.c_str() || *endp != '\0' || v < 0) return false;
        out = static_cast<uint64_t>(v);
        return true;
    };
    auto parse_bool = [](std::string_view sv) -> bool {
        std::string l = ToLower(sv);
        return l == "true" || l == "1" || l == "yes" || l == "on" || l == "enabled";
    };

    m_settings_validation_key.clear();
    m_settings_validation_error.clear();

    if (key == "start_minimized") {
        m_draft_settings.start_minimized = parse_bool(value_str);
    } else if (key == "close_to_tray") {
        m_draft_settings.close_to_tray = parse_bool(value_str);
    } else if (key == "reduced_motion") {
        m_draft_settings.reduced_motion = parse_bool(value_str);
    } else if (key == "max_concurrent_tasks") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 1 || v > 16) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Simultaneous downloads must be an integer between 1 and 16.";
            return Result<void>::Err("ERR_SETTINGS_CONCURRENT_RANGE", m_settings_validation_error);
        }
        m_draft_settings.max_concurrent_tasks = static_cast<uint32_t>(v);
    } else if (key == "auto_start_downloads") {
        m_draft_settings.auto_start_downloads = parse_bool(value_str);
    } else if (key == "conflict_policy") {
        if (value_str != "auto_rename" && value_str != "overwrite" && value_str != "fail_if_exists") {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Conflict policy must be auto_rename, overwrite, or fail_if_exists.";
            return Result<void>::Err("ERR_SETTINGS_CONFLICT_POLICY", m_settings_validation_error);
        }
        m_draft_settings.conflict_policy = ConflictPolicyFromString(std::string(value_str));
    } else if (key == "default_download_dir") {
        if (value_str.empty()) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Default download directory cannot be empty.";
            return Result<void>::Err("ERR_SETTINGS_EMPTY_SAVE_DIR", m_settings_validation_error);
        }
        for (char c : value_str) {
            if (static_cast<unsigned char>(c) < 0x20) {
                m_settings_validation_key = std::string(key);
                m_settings_validation_error = "Default download directory contains invalid control characters.";
                return Result<void>::Err("ERR_SETTINGS_INVALID_SAVE_DIR", m_settings_validation_error);
            }
        }
        m_draft_settings.default_download_dir = std::string(value_str);
    } else if (key == "category_subfolders_enabled") {
        m_draft_settings.category_subfolders_enabled = parse_bool(value_str);
    } else if (key == "default_connections_per_task") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 1 || v > 32) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Default connections per task must be between 1 and 32.";
            return Result<void>::Err("ERR_SETTINGS_CONNECTIONS_RANGE", m_settings_validation_error);
        }
        m_draft_settings.default_connections_per_task = static_cast<uint32_t>(v);
    } else if (key == "speed_limit_bps") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v)) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Global speed limit must be a non-negative byte/s rate (0 = Unlimited).";
            return Result<void>::Err("ERR_SETTINGS_SPEED_LIMIT", m_settings_validation_error);
        }
        m_draft_settings.speed_limit_bps = v;
    } else if (key == "stall_timeout_ms") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 500 || v > 60000) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Segment stall timeout must be between 500 ms and 60,000 ms.";
            return Result<void>::Err("ERR_SETTINGS_STALL_TIMEOUT", m_settings_validation_error);
        }
        m_draft_settings.stall_timeout_ms = static_cast<int>(v);
    } else if (key == "min_split_bytes") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 65536ULL || v > 10485760ULL) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Minimum segment split size must be between 65,536 B (64 KiB) and 10,485,760 B (10 MiB).";
            return Result<void>::Err("ERR_SETTINGS_MIN_SPLIT", m_settings_validation_error);
        }
        m_draft_settings.min_split_bytes = v;
    } else if (key == "browser_capture_enabled") {
        m_draft_settings.browser_capture_enabled = parse_bool(value_str);
    } else if (key == "min_capture_size_bytes") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v > 10737418240ULL) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Minimum automatic capture size must be between 0 B and 10 GiB.";
            return Result<void>::Err("ERR_SETTINGS_CAPTURE_SIZE", m_settings_validation_error);
        }
        m_draft_settings.min_capture_size_bytes = v;
    } else if (key == "show_completion_notification") {
        m_draft_settings.show_completion_notification = parse_bool(value_str);
    } else if (key == "max_auto_retries") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v > 50) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Maximum automatic retries must be between 0 and 50.";
            return Result<void>::Err("ERR_SETTINGS_RETRIES_RANGE", m_settings_validation_error);
        }
        m_draft_settings.max_auto_retries = static_cast<uint32_t>(v);
    } else if (key == "retry_interval_sec") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 1 || v > 600) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Retry interval must be between 1 and 600 seconds.";
            return Result<void>::Err("ERR_SETTINGS_RETRY_INTERVAL", m_settings_validation_error);
        }
        m_draft_settings.retry_interval_sec = static_cast<uint32_t>(v);
    } else if (key == "queue_completion_action") {
        if (value_str != "NONE" && value_str != "SLEEP" && value_str != "SHUTDOWN") {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Queue post-completion action must be NONE, SLEEP, or SHUTDOWN.";
            return Result<void>::Err("ERR_SETTINGS_COMPLETION_ACTION", m_settings_validation_error);
        }
        m_draft_settings.queue_completion_action = std::string(value_str);
    } else {
        return Result<void>::Err("ERR_SETTINGS_UNKNOWN_KEY", "Unknown setting key: " + std::string(key));
    }

    return Result<void>::Ok();
}

Result<void> DesktopView::CycleOrToggleDraftSetting(std::string_view key) {
    if (key == "start_minimized") {
        m_draft_settings.start_minimized = !m_draft_settings.start_minimized;
    } else if (key == "close_to_tray") {
        m_draft_settings.close_to_tray = !m_draft_settings.close_to_tray;
    } else if (key == "reduced_motion") {
        m_draft_settings.reduced_motion = !m_draft_settings.reduced_motion;
    } else if (key == "max_concurrent_tasks") {
        uint32_t next = (m_draft_settings.max_concurrent_tasks >= 16) ? 1 : (m_draft_settings.max_concurrent_tasks + 1);
        m_draft_settings.max_concurrent_tasks = next;
    } else if (key == "auto_start_downloads") {
        m_draft_settings.auto_start_downloads = !m_draft_settings.auto_start_downloads;
    } else if (key == "conflict_policy") {
        if (m_draft_settings.conflict_policy == ConflictPolicy::kAutoRename) {
            m_draft_settings.conflict_policy = ConflictPolicy::kOverwrite;
        } else if (m_draft_settings.conflict_policy == ConflictPolicy::kOverwrite) {
            m_draft_settings.conflict_policy = ConflictPolicy::kFailIfExists;
        } else {
            m_draft_settings.conflict_policy = ConflictPolicy::kAutoRename;
        }
    } else if (key == "category_subfolders_enabled") {
        m_draft_settings.category_subfolders_enabled = !m_draft_settings.category_subfolders_enabled;
    } else if (key == "default_connections_per_task") {
        const uint32_t presets[] = {1, 2, 4, 8, 16, 24, 32};
        uint32_t next = 8;
        for (size_t i = 0; i < 7; ++i) {
            if (m_draft_settings.default_connections_per_task == presets[i]) {
                next = presets[(i + 1) % 7];
                break;
            }
        }
        m_draft_settings.default_connections_per_task = next;
    } else if (key == "speed_limit_bps") {
        const uint64_t presets[] = {0ULL, 1048576ULL, 5242880ULL, 10485760ULL, 26214400ULL, 52428800ULL};
        uint64_t next = 0ULL;
        for (size_t i = 0; i < 6; ++i) {
            if (m_draft_settings.speed_limit_bps == presets[i]) {
                next = presets[(i + 1) % 6];
                break;
            }
        }
        m_draft_settings.speed_limit_bps = next;
    } else if (key == "stall_timeout_ms") {
        const int presets[] = {1500, 3000, 5000, 10000, 15000};
        int next = 3000;
        for (size_t i = 0; i < 5; ++i) {
            if (m_draft_settings.stall_timeout_ms == presets[i]) {
                next = presets[(i + 1) % 5];
                break;
            }
        }
        m_draft_settings.stall_timeout_ms = next;
    } else if (key == "min_split_bytes") {
        const uint64_t presets[] = {65536ULL, 131072ULL, 262144ULL, 524288ULL, 1048576ULL};
        uint64_t next = 131072ULL;
        for (size_t i = 0; i < 5; ++i) {
            if (m_draft_settings.min_split_bytes == presets[i]) {
                next = presets[(i + 1) % 5];
                break;
            }
        }
        m_draft_settings.min_split_bytes = next;
    } else if (key == "browser_capture_enabled") {
        m_draft_settings.browser_capture_enabled = !m_draft_settings.browser_capture_enabled;
    } else if (key == "min_capture_size_bytes") {
        const uint64_t presets[] = {0ULL, 262144ULL, 524288ULL, 1048576ULL, 5242880ULL, 10485760ULL};
        uint64_t next = 1048576ULL;
        for (size_t i = 0; i < 6; ++i) {
            if (m_draft_settings.min_capture_size_bytes == presets[i]) {
                next = presets[(i + 1) % 6];
                break;
            }
        }
        m_draft_settings.min_capture_size_bytes = next;
    } else if (key == "show_completion_notification") {
        m_draft_settings.show_completion_notification = !m_draft_settings.show_completion_notification;
    } else if (key == "proxy_mode") {
        if (m_draft_settings.proxy_mode == "direct") {
            m_draft_settings.proxy_mode = "system";
        } else if (m_draft_settings.proxy_mode == "system") {
            m_draft_settings.proxy_mode = "manual";
        } else {
            m_draft_settings.proxy_mode = "direct";
        }
    } else if (key == "queue_completion_action") {
        if (m_draft_settings.queue_completion_action == "NONE") {
            m_draft_settings.queue_completion_action = "SLEEP";
        } else if (m_draft_settings.queue_completion_action == "SLEEP") {
            m_draft_settings.queue_completion_action = "SHUTDOWN";
        } else {
            m_draft_settings.queue_completion_action = "NONE";
        }
    } else if (key == "max_auto_retries") {
        uint32_t next = (m_draft_settings.max_auto_retries >= 15) ? 0 : (m_draft_settings.max_auto_retries + 3);
        m_draft_settings.max_auto_retries = next;
    } else if (key == "retry_interval_sec") {
        const uint32_t presets[] = {5, 10, 15, 30, 60};
        uint32_t next = 15;
        for (size_t i = 0; i < 5; ++i) {
            if (m_draft_settings.retry_interval_sec == presets[i]) {
                next = presets[(i + 1) % 5];
                break;
            }
        }
        m_draft_settings.retry_interval_sec = next;
    }
    m_settings_validation_key.clear();
    m_settings_validation_error.clear();
    return Result<void>::Ok();
}

Result<void> DesktopView::ApplySettingsChanges() {
    if (!m_settings_validation_error.empty()) {
        return Result<void>::Err("ERR_SETTINGS_INVALID", m_settings_validation_error);
    }
    auto val = ValidateSettingsInput(m_draft_settings);
    if (!val.ok()) {
        m_settings_validation_error = val.error->message;
        return val;
    }
    return ActionSaveSettings(m_draft_settings);
}

void DesktopView::RevertSettingsChanges() {
    m_draft_settings = m_controller.GetSettings();
    m_settings_validation_key.clear();
    m_settings_validation_error.clear();
    m_last_feedback_message = "Reverted unsaved settings changes";
}

std::vector<SettingItemView> DesktopView::BuildAllSettingItems() const {
    const auto saved = m_controller.GetSettings();
    const auto& s = m_draft_settings;
    std::string def_dir = PathSanitizer::GetDefaultDownloadsDir(PathSanitizer::GetExecutableDir()).string();

    std::vector<SettingItemView> items;
    auto add_item = [&](
        std::string key,
        SettingsCategory cat,
        std::string group,
        std::string label,
        std::string desc,
        SettingControlType ctrl,
        std::string val_txt,
        std::string def_txt,
        bool bool_val,
        bool modified,
        bool adv = false) {
        SettingItemView it;
        it.key = std::move(key);
        it.category = cat;
        it.group_title = std::move(group);
        it.label = std::move(label);
        it.description = std::move(desc);
        it.control_type = ctrl;
        it.value_text = std::move(val_txt);
        it.default_value_text = std::move(def_txt);
        it.bool_value = bool_val;
        it.is_modified = modified;
        it.is_advanced = adv;
        if (it.key == m_settings_validation_key) {
            it.validation_error = m_settings_validation_error;
        }
        items.push_back(std::move(it));
    };

    // 1. General
    add_item(
        "start_minimized", SettingsCategory::kGeneral, "Startup & Window Behavior",
        "Start Minimized to System Tray",
        "Launch Radium quietly in the system tray without opening the main window on startup.",
        SettingControlType::kToggleSwitch,
        s.start_minimized ? "Enabled" : "Disabled", "Disabled",
        s.start_minimized, s.start_minimized != saved.start_minimized);
    add_item(
        "close_to_tray", SettingsCategory::kGeneral, "Startup & Window Behavior",
        "Minimize to System Tray on Close",
        "Keep active downloads and browser IPC listening in the tray when closing the main window.",
        SettingControlType::kToggleSwitch,
        s.close_to_tray ? "Enabled" : "Disabled", "Enabled",
        s.close_to_tray, s.close_to_tray != saved.close_to_tray);
    add_item(
        "reduced_motion", SettingsCategory::kGeneral, "Interface & Accessibility",
        "Reduce UI Animations",
        "Disable smooth interface transitions and progress animations for accessibility.",
        SettingControlType::kToggleSwitch,
        s.reduced_motion ? "Reduced Motion" : "Standard Animations", "Standard Animations",
        s.reduced_motion, s.reduced_motion != saved.reduced_motion);

    // 2. Downloads & Queue
    add_item(
        "max_concurrent_tasks", SettingsCategory::kDownloads, "Queue Concurrency",
        "Maximum Simultaneous Downloads",
        "Active HTTP and stream transfers allowed to run concurrently in the main queue (1–16).",
        SettingControlType::kDropdown,
        std::to_string(s.max_concurrent_tasks) + " tasks", "3 tasks",
        false, s.max_concurrent_tasks != saved.max_concurrent_tasks);
    add_item(
        "auto_start_downloads", SettingsCategory::kDownloads, "Default Task Behavior",
        "Start New Downloads Immediately",
        "Automatically begin transfer when a download or media stream is added from browser or clipboard.",
        SettingControlType::kToggleSwitch,
        s.auto_start_downloads ? "Enabled" : "Queued Paused", "Enabled",
        s.auto_start_downloads, s.auto_start_downloads != saved.auto_start_downloads);
    add_item(
        "conflict_policy", SettingsCategory::kDownloads, "Default Task Behavior",
        "Existing File Conflict Resolution",
        "Action taken when a completed download matches an existing filename in the destination folder.",
        SettingControlType::kDropdown,
        FormatConflictPolicyLabel(s.conflict_policy), FormatConflictPolicyLabel(ConflictPolicy::kAutoRename),
        false, s.conflict_policy != saved.conflict_policy);
    add_item(
        "max_auto_retries", SettingsCategory::kDownloads, "Automation & Recovery",
        "Max Automatic Retries",
        "Number of automatic retries on transient network errors before marking failed (0–50).",
        SettingControlType::kDropdown,
        std::to_string(s.max_auto_retries) + " retries", "5 retries",
        false, s.max_auto_retries != saved.max_auto_retries);
    add_item(
        "retry_interval_sec", SettingsCategory::kDownloads, "Automation & Recovery",
        "Base Retry Interval",
        "Initial wait interval in seconds before retry attempt (with exponential backoff).",
        SettingControlType::kDropdown,
        std::to_string(s.retry_interval_sec) + " seconds", "15 seconds",
        false, s.retry_interval_sec != saved.retry_interval_sec);
    add_item(
        "queue_completion_action", SettingsCategory::kDownloads, "Automation & Recovery",
        "Queue Post-Completion Action",
        "System action taken when all active transfers finish (NONE, SLEEP, or SHUTDOWN).",
        SettingControlType::kDropdown,
        s.queue_completion_action, "NONE",
        false, s.queue_completion_action != saved.queue_completion_action);

    // 3. Save Locations
    add_item(
        "default_download_dir", SettingsCategory::kSaveLocations, "Destination Folders",
        "Default Download Directory",
        "Base directory where completed files and muxed media containers are saved.",
        SettingControlType::kFolderSelector,
        s.default_download_dir, def_dir,
        false, s.default_download_dir != saved.default_download_dir);
    add_item(
        "category_subfolders_enabled", SettingsCategory::kSaveLocations, "Destination Folders",
        "Organize Downloads into Category Subfolders",
        "Route Video, Audio, Compressed, Documents, and Programs into dedicated subfolders automatically.",
        SettingControlType::kToggleSwitch,
        s.category_subfolders_enabled ? "Enabled" : "Flat Directory", "Enabled",
        s.category_subfolders_enabled, s.category_subfolders_enabled != saved.category_subfolders_enabled);
    for (const auto& [cat_name, custom_dir] : s.category_folders) {
        if (!custom_dir.empty()) {
            add_item(
                "cat_folder_" + ToLower(cat_name), SettingsCategory::kSaveLocations, "Custom Category Folders",
                cat_name + " Folder Path",
                "Custom destination folder for " + cat_name + " files.",
                SettingControlType::kDiagnosticReadOnly,
                custom_dir, s.default_download_dir,
                false, false);
        }
    }
    if (!s.site_rules.empty()) {
        add_item(
            "site_rules_count", SettingsCategory::kSaveLocations, "Site-Specific Rules",
            "Configured Domain Routing Rules",
            std::to_string(s.site_rules.size()) + " site rules active in SQLite persistence store.",
            SettingControlType::kDiagnosticReadOnly,
            std::to_string(s.site_rules.size()) + " rules", "0 rules",
            false, false);
    }

    // 4. Connections & Speed Limits
    add_item(
        "default_connections_per_task", SettingsCategory::kConnectionsAndSpeed, "Segmented HTTP Engine",
        "Default Connections per Task",
        "Maximum dynamic work-stealing HTTP range connections spawned per file transfer (1–32).",
        SettingControlType::kDropdown,
        std::to_string(s.default_connections_per_task) + " connections", "8 connections",
        false, s.default_connections_per_task != saved.default_connections_per_task);
    add_item(
        "speed_limit_bps", SettingsCategory::kConnectionsAndSpeed, "Bandwidth Throttling",
        "Global Download Speed Limit",
        "Cap aggregate transfer rate across active downloads (0 = Unlimited).",
        SettingControlType::kDropdown,
        FormatBandwidthLimit(s.speed_limit_bps), "Unlimited",
        false, s.speed_limit_bps != saved.speed_limit_bps);
    add_item(
        "stall_timeout_ms", SettingsCategory::kConnectionsAndSpeed, "Advanced Transfer Tuning",
        "Segment Stall Timeout",
        "Inactivity threshold before resetting a stalled HTTP range connection (500–60,000 ms).",
        SettingControlType::kDropdown,
        std::to_string(s.stall_timeout_ms) + " ms", "3000 ms",
        false, s.stall_timeout_ms != saved.stall_timeout_ms, true);
    add_item(
        "min_split_bytes", SettingsCategory::kConnectionsAndSpeed, "Advanced Transfer Tuning",
        "Minimum Work-Stealing Split Size",
        "Smallest remaining byte range eligible for dynamic segment splitting (64 KiB – 10 MiB).",
        SettingControlType::kDropdown,
        FormatBytes(s.min_split_bytes), "128.0 KiB",
        false, s.min_split_bytes != saved.min_split_bytes, true);
    add_item(
        "proxy_mode", SettingsCategory::kConnectionsAndSpeed, "Proxy Configuration",
        "Proxy Connection Mode",
        "Network proxy mode for HTTP/HTTPS transfers (direct, system, manual).",
        SettingControlType::kDropdown,
        s.proxy_mode, "system",
        false, s.proxy_mode != saved.proxy_mode);
    add_item(
        "proxy_server", SettingsCategory::kConnectionsAndSpeed, "Proxy Configuration",
        "Manual Proxy Server",
        "Proxy server address (e.g., http://127.0.0.1:8080 or socks5://...).",
        SettingControlType::kDiagnosticReadOnly,
        s.proxy_server.empty() ? "(None / Direct)" : s.proxy_server, "(None / Direct)",
        false, s.proxy_server != saved.proxy_server);
    add_item(
        "proxy_bypass", SettingsCategory::kConnectionsAndSpeed, "Proxy Configuration",
        "Proxy Bypass List",
        "Hosts and domains that bypass proxy (e.g., <local>;*.internal).",
        SettingControlType::kDiagnosticReadOnly,
        s.proxy_bypass.empty() ? "<local>" : s.proxy_bypass, "<local>",
        false, s.proxy_bypass != saved.proxy_bypass);
    add_item(
        "server_auth_username", SettingsCategory::kConnectionsAndSpeed, "Server & Proxy Authentication",
        "Default HTTP Auth User",
        "Default username for HTTP Basic/Digest or Proxy 407 authentication (passwords DPAPI-encrypted).",
        SettingControlType::kDiagnosticReadOnly,
        s.server_auth_username.empty() ? "(None)" : s.server_auth_username, "(None)",
        false, s.server_auth_username != saved.server_auth_username);

    // 5. Browser Integration
    add_item(
        "browser_capture_enabled", SettingsCategory::kBrowserIntegration, "Extension Capture & Interception",
        "Enable Browser Download & Media Capture",
        "Accept file downloads and HLS/DASH streams from the Radium Browser Extension (hold Alt when clicking a link to bypass).",
        SettingControlType::kToggleSwitch,
        s.browser_capture_enabled ? "Enabled" : "Paused", "Enabled",
        s.browser_capture_enabled, s.browser_capture_enabled != saved.browser_capture_enabled);
    add_item(
        "min_capture_size_bytes", SettingsCategory::kBrowserIntegration, "Extension Capture & Interception",
        "Minimum Automatic File Capture Size",
        "Minimum size threshold for size-based fallback download interception.",
        SettingControlType::kDropdown,
        FormatBytes(s.min_capture_size_bytes), "1.0 MiB",
        false, s.min_capture_size_bytes != saved.min_capture_size_bytes);
    auto bstat = GetBrowserIntegrationStatus();
    add_item(
        "ipc_endpoint_status", SettingsCategory::kBrowserIntegration, "Native Messaging & IPC Status",
        "Local IPC Endpoint (" + std::string(bstat.ipc_server_running ? "Listening" : "Offline") + ")",
        "Host ID: com.radium.downloadmanager  |  Endpoint: " + bstat.ipc_endpoint,
        SettingControlType::kDiagnosticReadOnly,
        bstat.ipc_server_running ? "Connected" : "Standby", "Connected",
        bstat.ipc_server_running, false);

    // 6. File Types
    const std::pair<const char*, const char*> cat_rules[] = {
        {"Video",      ".mp4, .mkv, .webm, .avi, .mov, .flv, .ts, .m4v, .wmv"},
        {"Audio",      ".mp3, .m4a, .flac, .wav, .ogg, .aac, .opus"},
        {"Compressed", ".zip, .rar, .7z, .tar, .gz, .bz2, .xz, .tgz, .zst"},
        {"Documents",  ".pdf, .doc, .docx, .xls, .xlsx, .ppt, .pptx, .txt, .md, .epub, .csv"},
        {"Programs",   ".exe, .msi, .iso, .img, .dmg, .deb, .rpm, .apk, .msix"}
    };
    for (const auto& [cname, exts] : cat_rules) {
        std::filesystem::path target_p = s.category_subfolders_enabled
            ? (std::filesystem::path(s.default_download_dir) / cname)
            : std::filesystem::path(s.default_download_dir);
        add_item(
            std::string("filetype_rule_") + ToLower(cname),
            SettingsCategory::kFileTypes, "Automatic Category Classification Rules",
            std::string(cname) + " Category (" + exts + ")",
            "Destination: " + target_p.string(),
            SettingControlType::kDiagnosticReadOnly,
            s.category_subfolders_enabled ? (std::string(cname) + "/") : "Root Folder",
            std::string(cname) + "/",
            true, false);
    }

    // 7. Notifications
    add_item(
        "show_completion_notification", SettingsCategory::kNotifications, "System Tray Notifications",
        "Show Completion & Error Notifications",
        "Display a Windows system tray notification when a background download completes or fails.",
        SettingControlType::kToggleSwitch,
        s.show_completion_notification ? "Enabled" : "Muted", "Enabled",
        s.show_completion_notification, s.show_completion_notification != saved.show_completion_notification);

    // 8. About & Diagnostics
    auto ab = GetAboutDialogView();
    add_item(
        "diag_app_summary", SettingsCategory::kAboutAndDiagnostics, "About Radium",
        "Radium Download Manager",
        "Fast, clean download manager and browser video grabber for Windows 11. Multi-connection acceleration with zero bloat.",
        SettingControlType::kDiagnosticReadOnly,
        "v" + ab.version, "v" + ab.version, true, false);
    add_item(
        "diag_video_fetch", SettingsCategory::kAboutAndDiagnostics, "Video & Stream Fetching",
        "Browser Video Sniffer & Stream Pipeline",
        "Automatically sniffs video streams (HLS m3u8, DASH mpd, and web videos) via the browser extension and muxes quality streams with N_m3u8DL-RE & FFmpeg.",
        SettingControlType::kDiagnosticReadOnly,
        (bstat.nm3u8dl_available && bstat.ffmpeg_available) ? "Active" : "Workers Needed", "Active",
        bstat.nm3u8dl_available && bstat.ffmpeg_available, false);
    add_item(
        "diag_version", SettingsCategory::kAboutAndDiagnostics, "System & License",
        "Platform & License",
        "Windows 11 x64 (Native Win32 C++20)  |  " + ab.license,
        SettingControlType::kDiagnosticReadOnly,
        "Active", "Active", true, false);
    add_item(
        "diag_storage", SettingsCategory::kAboutAndDiagnostics, "System & License",
        "Queue Database",
        "SQLite WAL active at: " + m_controller.GetConfig().state_db_path.string(),
        SettingControlType::kDiagnosticReadOnly,
        "WAL Active", "WAL Active", true, false);

    return items;
}

SettingsCenterViewModel DesktopView::BuildSettingsCenterView() const {
    SettingsCenterViewModel vm;
    vm.is_open = m_settings_center_open;
    vm.active_category = m_settings_category;
    vm.search_query = m_settings_search_query;
    vm.has_unsaved_changes = HasUnsavedSettingsChanges();
    auto val_res = ValidateSettingsInput(m_draft_settings);
    vm.is_valid = m_settings_validation_error.empty() && val_res.ok();
    vm.validation_error = !m_settings_validation_error.empty()
        ? m_settings_validation_error
        : (val_res.ok() ? "" : val_res.error->message);
    vm.browser_status = GetBrowserIntegrationStatus();
    vm.about_info = GetAboutDialogView();

    if (!vm.is_valid) {
        vm.status_banner = "Validation Error: " + vm.validation_error;
    } else if (vm.has_unsaved_changes) {
        vm.status_banner = "Unsaved changes — Click 'Apply & Save' to persist or 'Cancel' to revert.";
    } else {
        vm.status_banner = "All configuration changes saved to SQLite state store.";
    }

    const auto& m = m_metrics;
    int margin_x = DesignSystem::ScaleForDpi(44, m.dpi);
    int margin_y = DesignSystem::ScaleForDpi(36, m.dpi);
    vm.modal_rect = {margin_x, margin_y, m_window_width - margin_x, m_window_height - margin_y};
    int side_w = DesignSystem::ScaleForDpi(210, m.dpi);
    int header_h = DesignSystem::ScaleForDpi(52, m.dpi);
    int footer_h = DesignSystem::ScaleForDpi(52, m.dpi);

    vm.sidebar_rect = {vm.modal_rect.left, vm.modal_rect.top + header_h, vm.modal_rect.left + side_w, vm.modal_rect.bottom - footer_h};
    vm.content_rect = {vm.sidebar_rect.right + 1, vm.modal_rect.top + header_h, vm.modal_rect.right, vm.modal_rect.bottom - footer_h};
    vm.search_box_rect = {
        vm.modal_rect.right - DesignSystem::ScaleForDpi(240, m.dpi) - m.space_md,
        vm.modal_rect.top + (header_h - m.button_height) / 2,
        vm.modal_rect.right - m.space_md,
        vm.modal_rect.top + (header_h + m.button_height) / 2
    };

    int footer_btn_y = vm.modal_rect.bottom - (footer_h + m.button_height) / 2;
    int restore_w = DesignSystem::ScaleForDpi(140, m.dpi);
    int cancel_w = DesignSystem::ScaleForDpi(96, m.dpi);
    int apply_w = DesignSystem::ScaleForDpi(124, m.dpi);

    vm.restore_defaults_rect = {
        vm.modal_rect.left + m.space_md,
        footer_btn_y,
        vm.modal_rect.left + m.space_md + restore_w,
        footer_btn_y + m.button_height
    };
    vm.apply_button_rect = {
        vm.modal_rect.right - m.space_md - apply_w,
        footer_btn_y,
        vm.modal_rect.right - m.space_md,
        footer_btn_y + m.button_height
    };
    vm.cancel_button_rect = {
        vm.apply_button_rect.left - m.space_sm - cancel_w,
        footer_btn_y,
        vm.apply_button_rect.left - m.space_sm,
        footer_btn_y + m.button_height
    };

    auto all_items = BuildAllSettingItems();
    const SettingsCategory all_cats[] = {
        SettingsCategory::kGeneral,
        SettingsCategory::kDownloads,
        SettingsCategory::kSaveLocations,
        SettingsCategory::kConnectionsAndSpeed,
        SettingsCategory::kBrowserIntegration,
        SettingsCategory::kFileTypes,
        SettingsCategory::kNotifications,
        SettingsCategory::kAboutAndDiagnostics
    };

    int tab_y = vm.sidebar_rect.top + m.space_sm;
    int tab_h = DesignSystem::ScaleForDpi(32, m.dpi);
    for (SettingsCategory c : all_cats) {
        size_t cnt = 0;
        size_t mod_cnt = 0;
        for (const auto& it : all_items) {
            if (it.category == c) {
                ++cnt;
                if (it.is_modified) ++mod_cnt;
            }
        }
        RectI trc{vm.sidebar_rect.left + m.space_sm, tab_y, vm.sidebar_rect.right - m.space_sm, tab_y + tab_h};
        vm.categories.push_back({c, SettingsCategoryToLabel(c), cnt, mod_cnt, c == m_settings_category, trc});
        tab_y += tab_h + m.space_xs;
    }

    std::string q = ToLower(m_settings_search_query);
    int row_y = vm.content_rect.top + m.space_sm;
    int row_h = DesignSystem::ScaleForDpi(48, m.dpi);
    int ctrl_w = DesignSystem::ScaleForDpi(170, m.dpi);
    int ctrl_h = DesignSystem::ScaleForDpi(28, m.dpi);

    for (auto& it : all_items) {
        bool matches = false;
        if (!q.empty()) {
            matches = ToLower(it.label).find(q) != std::string::npos ||
                      ToLower(it.description).find(q) != std::string::npos ||
                      ToLower(it.group_title).find(q) != std::string::npos ||
                      ToLower(it.key).find(q) != std::string::npos;
        } else {
            matches = (it.category == m_settings_category);
        }
        if (!matches) continue;

        it.row_rect = {vm.content_rect.left + m.space_md, row_y, vm.content_rect.right - m.space_md, row_y + row_h};
        it.control_rect = {
            it.row_rect.right - ctrl_w - m.space_sm,
            row_y + (row_h - ctrl_h) / 2,
            it.row_rect.right - m.space_sm,
            row_y + (row_h + ctrl_h) / 2
        };
        vm.visible_items.push_back(it);
        row_y += row_h + m.space_xs;
    }

    const std::pair<const char*, const char*> rules[] = {
        {"Video",      ".mp4, .mkv, .webm, .avi, .mov, .flv, .ts, .m4v, .wmv"},
        {"Audio",      ".mp3, .m4a, .flac, .wav, .ogg, .aac, .opus"},
        {"Compressed", ".zip, .rar, .7z, .tar, .gz, .bz2, .xz, .tgz, .zst"},
        {"Documents",  ".pdf, .doc, .docx, .xls, .xlsx, .ppt, .pptx, .txt, .md, .epub, .csv"},
        {"Programs",   ".exe, .msi, .iso, .img, .dmg, .deb, .rpm, .apk, .msix"}
    };
    for (const auto& [cn, exts] : rules) {
        std::filesystem::path p = m_draft_settings.category_subfolders_enabled
            ? (std::filesystem::path(m_draft_settings.default_download_dir) / cn)
            : std::filesystem::path(m_draft_settings.default_download_dir);
        vm.file_category_rules.push_back({cn, exts, p.string()});
    }

    return vm;
}

// -----------------------------------------------------------------------------
// Dialog & Toolbar Action Controllers
// -----------------------------------------------------------------------------


Result<TaskModel> DesktopView::SubmitAddDownloadDialog(const AddDownloadDialogInput& input) {
    bool is_ftp = (input.url.find("ftp://") == 0);
    bool is_torrent = (input.url.find("magnet:?") == 0 || input.url.rfind(".torrent") != std::string::npos);
    bool is_rtsp_mms = (input.url.find("rtsp://") == 0 || input.url.find("mms://") == 0);

#ifdef _WIN32
    if (is_torrent && input.url.rfind(".torrent") != std::string::npos && m_hwnd && IsWindow(m_hwnd)) {
        std::error_code torrent_ec;
        if (std::filesystem::exists(input.url, torrent_ec)) {
            auto parsed_torrent = torrent::TorrentParser::ParseTorrentFile(input.url);
            if (parsed_torrent.ok()) {
                auto tinfo = std::move(*parsed_torrent.value);
                std::string target_save = input.save_dir.empty() ? m_controller.GetSettings().default_download_dir : input.save_dir;
                bool start_torrent = input.start_immediately;
                if (!ShowAddTorrentDialogWin32(m_hwnd, *this, m_controller, tinfo, input.url, target_save, start_torrent,
                                               &target_save, &start_torrent)) {
                    m_last_feedback_message = "Torrent file selection canceled";
                    return Result<TaskModel>::Err("ERR_TORRENT_SELECTION_CANCELED", "Torrent file selection canceled");
                }
                TaskModel t;
                t.task_type = TaskType::kTorrent;
                t.url = input.url;
                t.file_name = tinfo.name.empty() ? std::filesystem::path(input.url).filename().string() : tinfo.name;
                t.save_dir = target_save;
                t.total_bytes = tinfo.selected_bytes();
                for (const auto& file : tinfo.files) {
                    t.torrent_file_paths.push_back(file.relative_path.generic_string());
                    t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                }
                auto res = m_controller.AddTask(std::move(t), start_torrent, false);
                if (res.ok()) {
                    m_selected_task_id = res.value->task_id;
                    ScrollTaskIntoView(res.value->task_id, true);
                    m_last_feedback_message = "Added download: " + res.value->file_name;
                }
                return res;
            }
        }
    }
#endif

    if (!is_ftp && !is_torrent && !is_rtsp_mms) {
        auto parsed = brisk::HttpClient::ParseUrl(input.url);
        if (!parsed.ok()) {
            m_last_feedback_message = "Invalid URL: " + parsed.error->message;
            return Result<TaskModel>::Err(parsed.error->code, parsed.error->message);
        }
    }

    TaskModel t;
    t.url = input.url;
    t.file_name = input.custom_filename;
    t.save_dir = input.save_dir;
    t.max_connections = std::clamp(input.max_connections, 1u, 32u);
    t.scheduled = input.scheduled;
    t.schedule_time_local = input.schedule_time_local;

    bool is_youtube = (input.url.find("youtube.com") != std::string::npos || input.url.find("youtu.be") != std::string::npos);
    if (is_ftp) {
        t.task_type = TaskType::kFtp;
    } else if (is_torrent) {
        t.task_type = TaskType::kTorrent;
    } else if (is_rtsp_mms) {
        t.task_type = TaskType::kRtsp;
        t.category = "Video";
    } else if (input.is_stream || is_youtube || input.url.find(".m3u8") != std::string::npos) {
        t.task_type = TaskType::kStreamHls;
        t.category = "Video";
        t.stream_options.video_filter = input.video_filter.empty() ? "best" : input.video_filter;
        t.stream_options.audio_filter = input.audio_filter.empty() ? "best" : input.audio_filter;
        t.stream_options.subtitle_filter = input.subtitle_filter.empty() ? "all" : input.subtitle_filter;
        t.stream_options.output_format = (input.output_format == "mkv") ? "mkv" : "mp4";
    } else if (input.url.find(".mpd") != std::string::npos) {
        t.task_type = TaskType::kStreamDash;
        t.category = "Video";
        t.stream_options.video_filter = input.video_filter.empty() ? "best" : input.video_filter;
        t.stream_options.audio_filter = input.audio_filter.empty() ? "best" : input.audio_filter;
        t.stream_options.subtitle_filter = input.subtitle_filter.empty() ? "all" : input.subtitle_filter;
        t.stream_options.output_format = (input.output_format == "mkv") ? "mkv" : "mp4";
    } else {
        t.task_type = TaskType::kHttpFile;
    }

    if (t.file_name.empty()) {
        if (is_youtube) {
            t.file_name = "youtube_video.mp4";
        } else if (is_torrent && input.url.rfind(".torrent") != std::string::npos) {
            t.file_name = std::filesystem::path(input.url).filename().string();
        } else if (is_torrent) {
            t.file_name = "torrent_download";
        } else {
            auto derived = PathSanitizer::ExtractFromUrl(t.url);
            t.file_name = derived.empty() ? "download" : derived;
        }
    }

    if (input.total_bytes > 0) {
        t.total_bytes = input.total_bytes;
    }

    auto res = m_controller.AddTask(std::move(t), input.start_immediately, false);
    if (res.ok()) {
        m_selected_task_id = res.value->task_id;
        ScrollTaskIntoView(res.value->task_id, true);
        m_last_feedback_message = "Added download: " + res.value->file_name;
    } else {
        m_last_feedback_message = "Failed to add download: " + res.error->message;
    }
    return res;
}

Result<stream::StreamProbeResult> DesktopView::ProbeStreamForDialog(
    const std::string& manifest_url,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers) {
    auto res = m_controller.ProbeStream(manifest_url, headers, auth_headers);
    if (!res.ok()) {
        m_last_feedback_message = "Stream probe failed: " + res.error->message;
    } else {
        m_last_feedback_message = "Discovered " + std::to_string(res.value->tracks.size()) + " stream tracks";
    }
    return res;
}

Result<TaskModel> DesktopView::ConfirmStreamSelectionDialog(
    const stream::StreamProbeResult& probe_result,
    const std::string& page_title,
    const std::string& save_dir,
    const StreamSelectionOptions& chosen_options,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers) {

    if (probe_result.tracks.empty()) {
        return Result<TaskModel>::Err("ERR_STREAM_NO_TRACKS", "Cannot create stream task from empty track list");
    }

    TaskModel t;
    t.url = probe_result.manifest_url;
    t.task_type = (probe_result.manifest_url.find(".mpd") != std::string::npos)
                      ? TaskType::kStreamDash
                      : TaskType::kStreamHls;
    std::string fmt = (chosen_options.output_format == "mkv") ? "mkv" : "mp4";
    if (!page_title.empty()) {
        t.file_name = PathSanitizer::SanitizeFileName(page_title) + "." + fmt;
    }
    t.save_dir = save_dir;
    t.headers = headers;
    t.auth_headers = auth_headers;
    t.stream_options = chosen_options;
    t.stream_options.output_format = fmt;

    auto res = m_controller.AddTask(std::move(t), true, false);
    if (res.ok()) {
        m_selected_task_id = res.value->task_id;
        ScrollTaskIntoView(res.value->task_id, true);
        m_last_feedback_message = "Queued stream download: " + res.value->file_name;
    }
    return res;
}

Result<void> DesktopView::TriggerToolbarAction(ToolbarActionId action_id) {
    switch (action_id) {
        case ToolbarActionId::kStartAll:     return ActionStartAll();
        case ToolbarActionId::kPauseAll:     return ActionPauseAll();
        case ToolbarActionId::kStart:        return ActionStartSelected();
        case ToolbarActionId::kPause:        return ActionPauseSelected();
        case ToolbarActionId::kResume:       return ActionResumeSelected();
        case ToolbarActionId::kRetry:        return ActionRetrySelected();
        case ToolbarActionId::kCancel:       return ActionCancelSelected();
        case ToolbarActionId::kRemove: {
            return ActionRemoveSelectedFromList();
        }
        case ToolbarActionId::kClearHistory: {
            auto r = ActionClearCompletedHistory();
            return r.ok() ? Result<void>::Ok() : Result<void>::Err(r.error->code, r.error->message);
        }
        case ToolbarActionId::kOpenFolder: {
            auto r = ActionOpenDestinationFolder(false);
            return r.ok() ? Result<void>::Ok() : Result<void>::Err(r.error->code, r.error->message);
        }
        case ToolbarActionId::kAddUrl:
            m_last_feedback_message = "Opened Add Download Dialog";
            return Result<void>::Ok();
        case ToolbarActionId::kSettings:
            OpenSettingsCenter(SettingsCategory::kGeneral);
            return Result<void>::Ok();
    }
    return Result<void>::Ok();
}

Result<void> DesktopView::TriggerRowPrimaryAction(const std::string& task_id) {
    SelectTask(task_id);
    auto t = m_controller.GetTask(task_id);
    if (!t.has_value()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    switch (t->state) {
        case TaskState::kDownloading:
        case TaskState::kProbing:
        case TaskState::kQueued:
            return ActionPauseSelected();
        case TaskState::kPaused:
            return ActionResumeSelected();
        case TaskState::kFailed:
        case TaskState::kCanceled:
            return ActionRetrySelected();
        case TaskState::kCompleted: {
            auto r = ActionOpenCompletedFile(false);
            return r.ok() ? Result<void>::Ok() : Result<void>::Err(r.error->code, r.error->message);
        }
        default:
            return Result<void>::Ok();
    }
}

Result<void> DesktopView::ActionStartAll() {
    auto all = m_controller.ListTasks();
    size_t count = 0;
    for (const auto& t : all) {
        if ((t.state == TaskState::kPaused || t.state == TaskState::kFailed || t.state == TaskState::kQueued) && !t.scheduled) {
            count++;
            TriggerActionPulse(t.task_id, "start");
        }
    }
    m_controller.StartAllTasks();
    if (count > 0) {
        m_last_feedback_message = "Started " + std::to_string(count) + " download(s)";
    } else {
        m_last_feedback_message = "No paused downloads to start";
    }
    return Result<void>::Ok();
}

Result<void> DesktopView::ActionPauseAll() {
    auto all = m_controller.ListTasks();
    size_t count = 0;
    for (const auto& t : all) {
        if (t.state == TaskState::kQueued || t.state == TaskState::kProbing ||
            t.state == TaskState::kDownloading || t.state == TaskState::kMuxing) {
            count++;
            TriggerActionPulse(t.task_id, "pause");
        }
    }
    m_controller.PauseAllTasks();
    if (count > 0) {
        m_last_feedback_message = "Paused " + std::to_string(count) + " download(s)";
    } else {
        m_last_feedback_message = "No active downloads to pause";
    }
    return Result<void>::Ok();
}

Result<void> DesktopView::ActionStartSelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        TriggerActionPulse(id, "start");
        auto r = m_controller.StartTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Started task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionPauseSelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        TriggerActionPulse(id, "pause");
        auto r = m_controller.PauseTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Paused task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionResumeSelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        TriggerActionPulse(id, "resume");
        auto r = m_controller.ResumeTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Resumed task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionRetrySelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        TriggerActionPulse(id, "retry");
        auto r = m_controller.RetryTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Retrying task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionCancelSelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        auto r = m_controller.CancelTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Canceled task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionRemoveSelectedFromList() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        auto r = m_controller.RemoveTask(id, false);
        if (!r.ok()) last_res = r;
    }
    m_checked_task_ids.clear();
    m_select_all_checked = false;
    SelectFirstRow();
    m_last_feedback_message = last_res.ok() ? "Removed task(s) from list (completed file kept on disk)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionDeleteSelectedWithFile() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        auto r = m_controller.RemoveTask(id, true);
        if (!r.ok()) last_res = r;
    }
    m_checked_task_ids.clear();
    m_select_all_checked = false;
    SelectFirstRow();
    m_last_feedback_message = last_res.ok() ? "Deleted task(s) and removed output file from disk" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionMoveSelectedToRecycleBin() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        auto t = m_controller.GetTask(id);
        if (t.has_value()) {
            std::filesystem::path fp = std::filesystem::path(t->save_dir) / t->file_name;
            std::error_code ec;
            if (std::filesystem::exists(fp, ec)) {
#ifdef _WIN32
                std::wstring wpath = fp.wstring();
                wpath.push_back(L'\0'); // Double-null terminated buffer for SHFileOperationW
                SHFILEOPSTRUCTW op{};
                op.wFunc = FO_DELETE;
                op.pFrom = wpath.c_str();
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
                int ret = SHFileOperationW(&op);
                if (ret != 0 || op.fAnyOperationsAborted) {
                    last_res = Result<void>::Err("ERR_RECYCLE_BIN_FAILED", "Failed to move file to Recycle Bin: error code " + std::to_string(ret));
                    continue; // Do not remove task if file operation failed
                }
#else
                std::filesystem::remove(fp, ec);
#endif
            }
        }
        auto r = m_controller.RemoveTask(id, false);
        if (!r.ok()) last_res = r;
    }
    m_checked_task_ids.clear();
    m_select_all_checked = false;
    SelectFirstRow();
    m_last_feedback_message = last_res.ok() ? "Moved file(s) to Recycle Bin and removed task(s) from list" : last_res.error->message;
    return last_res;
}

Result<int> DesktopView::ActionClearCompletedHistory() {
    auto r = m_controller.ClearCompletedHistory();
    if (r.ok()) {
        m_last_feedback_message = "Cleared " + std::to_string(*r.value) + " completed/canceled item(s) from history";
    }
    return r;
}

Result<std::filesystem::path> DesktopView::ActionOpenCompletedFile(bool dry_run) {
    if (m_selected_task_id.empty()) {
        return Result<std::filesystem::path>::Err("ERR_NO_SELECTION", "No task selected");
    }
    auto t = m_controller.GetTask(m_selected_task_id);
    if (!t.has_value()) {
        return Result<std::filesystem::path>::Err("ERR_NOT_FOUND", "Task not found");
    }
    if (t->error_code == "ERR_THREAT_DETECTED" || t->status_detail.find("Threat detected") != std::string::npos) {
        m_last_feedback_message = "Security Block: Cannot open file because an antivirus threat was detected!";
        return Result<std::filesystem::path>::Err("ERR_THREAT_DETECTED", m_last_feedback_message);
    }
    if (t->state != TaskState::kCompleted) {
        return Result<std::filesystem::path>::Err("ERR_NOT_COMPLETED", "File is not yet completed");
    }
    std::filesystem::path fp = std::filesystem::path(t->save_dir) / t->file_name;
    std::error_code ec;
    if (!std::filesystem::exists(fp, ec)) {
        m_last_feedback_message = "File no longer exists on disk: " + fp.string();
        return Result<std::filesystem::path>::Err("ERR_FILE_MISSING", m_last_feedback_message);
    }
    if (!dry_run) {
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", fp.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    m_last_feedback_message = "Opened file: " + fp.filename().string();
    return Result<std::filesystem::path>::Ok(fp);
}

Result<std::filesystem::path> DesktopView::ActionOpenDestinationFolder(bool dry_run) {
    if (m_selected_task_id.empty()) {
        return Result<std::filesystem::path>::Err("ERR_NO_SELECTION", "No task selected");
    }
    auto t = m_controller.GetTask(m_selected_task_id);
    if (!t.has_value()) {
        return Result<std::filesystem::path>::Err("ERR_NOT_FOUND", "Task not found");
    }
    std::filesystem::path dir(t->save_dir);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return Result<std::filesystem::path>::Err("ERR_DIR_MISSING", "Destination folder does not exist");
    }
    if (!dry_run) {
#ifdef _WIN32
        std::filesystem::path fp = dir / t->file_name;
        if (std::filesystem::exists(fp, ec)) {
            std::wstring param = L"/select,\"" + fp.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
#endif
    }
    m_last_feedback_message = "Opened destination folder: " + dir.string();
    return Result<std::filesystem::path>::Ok(dir);
}

void DesktopView::ActionSetConcurrentLimit(uint32_t max_tasks) {
    m_controller.SetMaxConcurrentTasks(max_tasks);
    m_draft_settings.max_concurrent_tasks = m_controller.GetMaxConcurrentTasks();
    m_last_feedback_message = "Max concurrent downloads set to " + std::to_string(m_controller.GetMaxConcurrentTasks());
}

void DesktopView::ActionSetSpeedLimit(uint64_t bps) {
    m_controller.SetGlobalSpeedLimit(bps);
    m_draft_settings.speed_limit_bps = bps;
    m_last_feedback_message = (bps == 0) ? "Speed limit set to Unlimited" : ("Speed limit set to " + FormatSpeed(bps));
}

void DesktopView::ActionSetBrowserCapture(bool enabled) {
    auto s = m_controller.GetSettings();
    s.browser_capture_enabled = enabled;
    m_controller.UpdateSettings(s);
    m_draft_settings.browser_capture_enabled = enabled;
    if (m_ipc_server) {
        m_ipc_server->SetCaptureEnabled(enabled);
    }
    m_last_feedback_message = enabled ? "Browser download capture enabled" : "Browser download capture paused";
}

Result<void> DesktopView::ActionSaveSettings(const AppSettings& settings) {
    auto val = ValidateSettingsInput(settings);
    if (!val.ok()) {
        m_settings_validation_error = val.error->message;
        m_last_feedback_message = "Settings validation failed: " + val.error->message;
        return val;
    }
    auto r = m_controller.UpdateSettings(settings);
    if (r.ok()) {
        m_draft_settings = m_controller.GetSettings();
        m_settings_validation_key.clear();
        m_settings_validation_error.clear();
        SetTheme(settings.theme, false);
        m_anim.reduced_motion = settings.reduced_motion;
        if (m_ipc_server) {
            m_ipc_server->SetCaptureEnabled(settings.browser_capture_enabled);
        }
        m_last_feedback_message = "Settings saved";
    }
    return r;
}

Result<AppSettings> DesktopView::ActionRestoreDefaultSettings() {
    auto r = m_controller.RestoreDefaultSettings();
    if (r.ok()) {
        m_draft_settings = *r.value;
        m_settings_validation_key.clear();
        m_settings_validation_error.clear();
        SetTheme(r.value->theme, false);
        m_anim.reduced_motion = r.value->reduced_motion;
        if (m_ipc_server) {
            m_ipc_server->SetCaptureEnabled(r.value->browser_capture_enabled);
        }
        m_last_feedback_message = "Restored default settings";
    }
    return r;
}

Result<void> DesktopView::ActionSetTaskSpeedLimit(const std::string& task_id, uint64_t bps) {
    auto r = m_controller.SetTaskSpeedLimit(task_id, bps);
    if (r.ok()) {
        m_last_feedback_message = (bps == 0) ? "Task speed limit set to Unlimited" : ("Task speed limit set to " + FormatSpeed(bps));
    }
    return r;
}

Result<void> DesktopView::ActionAddAlternateUrl(const std::string& task_id, const std::string& alt_url) {
    auto r = m_controller.AddAlternateUrl(task_id, alt_url);
    if (r.ok()) {
        m_last_feedback_message = "Added alternate mirror URL";
    }
    return r;
}

Result<void> DesktopView::ActionSetTaskSchedule(const std::string& task_id, const std::string& start_time_local, const std::string& stop_time_local) {
    auto r = m_controller.SetTaskSchedule(task_id, start_time_local, stop_time_local);
    if (r.ok()) {
        m_last_feedback_message = "Updated task schedule";
    }
    return r;
}

Result<std::vector<crawler::GrabbedItem>> DesktopView::ActionLaunchSiteGrabber(
    const crawler::SiteGrabberConfig& cfg,
    std::atomic<bool>& cancel_requested,
    crawler::GrabbedItemCallback on_item_found) {
    m_last_feedback_message = "Running Site Grabber on " + cfg.start_url + "...";
    auto r = m_controller.CrawlSite(cfg, cancel_requested, on_item_found);
    if (r.ok()) {
        m_last_feedback_message = "Site Grabber completed. Found " + std::to_string(r.value->size()) + " media/file items.";
    } else {
        m_last_feedback_message = "Site Grabber failed: " + r.error->message;
    }
    return r;
}

bool DesktopView::HandleKeyShortcut(const std::string& key_chord) {
    if (key_chord == "Escape" || key_chord == "VK_ESCAPE") {
        if (m_settings_center_open) {
            if (HasUnsavedSettingsChanges()) {
#ifdef _WIN32
                if (m_hwnd) {
                    int res = MessageBoxW(m_hwnd, L"You have unsaved changes. Discard changes and close settings?", L"Discard Changes", MB_YESNO | MB_ICONQUESTION);
                    if (res != IDYES) return true;
                }
#endif
            }
            CloseSettingsCenter(true);
            return true;
        }
        return false;
    }
    if (key_chord == "Down" || key_chord == "VK_DOWN") {
        SelectNextRow();
        return true;
    }
    if (key_chord == "Up" || key_chord == "VK_UP") {
        SelectPrevRow();
        return true;
    }
    if (key_chord == "Home" || key_chord == "VK_HOME") {
        SelectFirstRow();
        return true;
    }
    if (key_chord == "End" || key_chord == "VK_END") {
        SelectLastRow();
        return true;
    }
    if (key_chord == "Space" || key_chord == "VK_SPACE") {
        auto tb = ComputeToolbarState();
        if (tb.can_pause) ActionPauseSelected();
        else if (tb.can_resume) ActionResumeSelected();
        return true;
    }
    if (key_chord == "Delete" || key_chord == "VK_DELETE") {
#ifdef _WIN32
        if (m_hwnd) {
            ShowDeleteConfirmationDialogWin32(m_hwnd, *this, m_controller);
            return true;
        }
#endif
        ActionRemoveSelectedFromList();
        return true;
    }
    if (key_chord == "Shift+Delete") {
#ifdef _WIN32
        if (m_hwnd) {
            ShowDeleteConfirmationDialogWin32(m_hwnd, *this, m_controller);
            return true;
        }
#endif
        ActionDeleteSelectedWithFile();
        return true;
    }
    if (key_chord == "F5" || key_chord == "VK_F5") {
        ActionRetrySelected();
        return true;
    }
    if (key_chord == "Enter" || key_chord == "VK_RETURN") {
        ActionOpenCompletedFile(true);
        return true;
    }
    if (key_chord == "Ctrl+N") {
        TriggerToolbarAction(ToolbarActionId::kAddUrl);
        return true;
    }
    if (key_chord == "Ctrl+,") {
        TriggerToolbarAction(ToolbarActionId::kSettings);
        return true;
    }
    return false;
}

BrowserIntegrationStatusView DesktopView::GetBrowserIntegrationStatus() const {
    BrowserIntegrationStatusView v;
    v.ipc_server_running = (m_ipc_server != nullptr);
    v.ipc_endpoint = m_ipc_server ? m_ipc_server->GetEndpointPath() : ipc::IpcServer::DefaultSocketPath();
    v.capture_enabled = m_controller.GetSettings().browser_capture_enabled;
    const auto& cfg = m_controller.GetConfig();
    std::error_code ec;
    v.nm3u8dl_path = cfg.nm3u8dl_binary_path.string();
    v.nm3u8dl_available = std::filesystem::exists(cfg.nm3u8dl_binary_path, ec);
    v.ffmpeg_path = cfg.ffmpeg_binary_path.string();
    v.ffmpeg_available = std::filesystem::exists(cfg.ffmpeg_binary_path, ec);
    v.supported_browsers = {"Google Chrome (MV3)", "Microsoft Edge (MV3)", "Brave Browser (MV3)", "Mozilla Firefox (MV3)"};
    return v;
}

AboutDialogView DesktopView::GetAboutDialogView() const {
    AboutDialogView a;
    a.upstream_attributions = {
        "N_m3u8DL-RE Stream Downloader (https://github.com/nilaoda/N_m3u8DL-RE) - MIT",
        "FFmpeg Media Muxer (https://ffmpeg.org) - LGPL-2.1+ / LGPL-3.0",
        "Microsoft WIL (https://github.com/microsoft/wil) - MIT",
        "SQLite (https://www.sqlite.org) - Public Domain",
        "nlohmann/json (https://github.com/nlohmann/json) - MIT"
    };
    return a;
}

std::string DesktopView::RenderQueueTableText() const {
    auto rows = BuildVisibleTaskRows(0, 500);
    std::ostringstream oss;
    oss << "========================================================================================================================\n";
    oss << " Radium Download Manager v" << FLUX_VERSION_STRING << "  |  Category: " << CategoryToLabel(m_selected_category)
        << "  |  Theme: " << m_theme_name << "\n";
    oss << "========================================================================================================================\n";
    oss << std::left
        << std::setw(10) << "TASK ID"
        << std::setw(14) << "TYPE"
        << std::setw(34) << "FILENAME"
        << std::setw(28) << "STATUS / STAGE"
        << std::setw(14) << "PROGRESS"
        << std::setw(18) << "SIZE"
        << std::setw(12) << "SPEED" << "\n";
    oss << "------------------------------------------------------------------------------------------------------------------------\n";
    if (rows.empty()) {
        oss << "  " << GetEmptyStateMessage() << "\n";
    } else {
        for (const auto& r : rows) {
            oss << (r.selected ? "> " : "  ")
                << std::left
                << std::setw(8) << r.task_id.substr(0, 8)
                << std::setw(14) << r.file_type_badge
                << std::setw(34) << r.display_filename
                << std::setw(28) << r.status_text.substr(0, 26)
                << std::setw(14) << r.progress_text.substr(0, 12)
                << std::setw(18) << r.size_text.substr(0, 16)
                << std::setw(12) << r.speed_text << "\n";
        }
    }
    oss << "------------------------------------------------------------------------------------------------------------------------\n";
    auto sb = BuildStatusBar();
    oss << " " << sb.status_message << "\n";
    oss << "========================================================================================================================\n";
    return oss.str();
}

std::string DesktopView::RenderFullDesktopSnapshotText() const {
    std::ostringstream oss;
    auto counts = ComputeCategoryCounts();
    oss << RenderQueueTableText();
    oss << "[Sidebar Counts] All=" << counts.all_downloads
        << " InProgress=" << counts.in_progress
        << " Queued=" << counts.queued
        << " Completed=" << counts.completed
        << " Failed=" << counts.failed
        << " Video=" << counts.video_downloads
        << " Scheduled=" << counts.scheduled << "\n";
    auto details = BuildSelectedTaskDetails();
    if (details.has_selection) {
        oss << "[Details Panel] File=" << details.file_name
            << " | Engine=" << details.engine_name
            << " | Source=" << details.redacted_source_url
            << " | Headers=" << details.redacted_headers_summary << "\n";
    }
    return oss.str();
}

#ifdef _WIN32
// -----------------------------------------------------------------------------
// Native Windows 11 Win32 + GDI Double-Buffered Dark Interface, Taskbar Progress & Settings Center
// -----------------------------------------------------------------------------
namespace {

constexpr UINT WM_OPENIDM_TRAYICON = WM_APP + 101;
constexpr UINT WM_OPENIDM_SHOW_NEW_DOWNLOAD = WM_APP + 102;
constexpr UINT WM_OPENIDM_SELECT_TORRENT_METADATA = WM_APP + 103;
constexpr UINT WM_OPENIDM_PROBE_COMPLETE          = WM_APP + 104;
constexpr UINT WM_OPENIDM_TORRENT_FETCH_COMPLETE = WM_APP + 105;

struct ProbeResultPayload {
    uint64_t request_id{0};
    int64_t content_length{-1};
    bool success{false};
    std::string final_filename;
};
constexpr UINT IDT_UI_REFRESH      = 1001;
constexpr int  IDC_SEARCH_EDIT     = 1101;

struct OfferNotificationData {
    std::string task_id;
    std::string url;
    std::string file_name;
    std::string save_dir;
    uint64_t file_size{0};
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> auth_headers;
};

struct TorrentFetchPayload {
    OfferNotificationData offer;
    std::string metainfo_path;
    std::string error_message;
};

bool IsRemoteTorrentUrl(std::string_view url) {
    auto parsed = brisk::HttpClient::ParseUrl(url);
    if (!parsed.ok()) return false;
    std::string path = parsed.value->path_and_query;
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return path.ends_with(".torrent");
}

bool IsRemoteTorrentOffer(const OfferNotificationData& offer) {
    if (IsRemoteTorrentUrl(offer.url)) return true;
    auto parsed = brisk::HttpClient::ParseUrl(offer.url);
    if (!parsed.ok()) return false;
    std::string file_name = offer.file_name;
    std::transform(file_name.begin(), file_name.end(), file_name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return file_name.ends_with(".torrent");
}

Result<std::string> FetchTorrentMetainfo(const OfferNotificationData& offer) {
    std::map<std::string, std::string> headers = offer.headers;
    for (const auto& [name, value] : offer.auth_headers) headers[name] = value;
    std::string body;
    constexpr size_t kMaxTorrentMetainfoBytes = 16 * 1024 * 1024;
    std::atomic<bool> cancel{false};
    auto response = brisk::HttpClient::PerformGet(
        offer.url, headers, std::nullopt, std::nullopt, 15000, false, cancel,
        [](const brisk::HttpResponseMeta&) { return brisk::HeaderAction::kContinue; },
        [&body, kMaxTorrentMetainfoBytes](const uint8_t* data, size_t len) {
            if (len > kMaxTorrentMetainfoBytes - std::min(body.size(), kMaxTorrentMetainfoBytes)) return false;
            body.append(reinterpret_cast<const char*>(data), len);
            return true;
        });
    if (!response.ok()) {
        return Result<std::string>::Err(response.error->code, response.error->message);
    }
    if (body.empty()) {
        return Result<std::string>::Err("ERR_TORRENT_EMPTY", "The server returned an empty torrent file");
    }

    wchar_t local_app_data[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local_app_data) != S_OK) {
        return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not locate the user data directory");
    }
    std::error_code ec;
    const std::filesystem::path torrent_root = std::filesystem::path(local_app_data) / L"OpenIDM" / L"tmp" / L"torrents";
    std::filesystem::create_directories(torrent_root, ec);
    if (ec) return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not create the torrent metadata folder");

    GUID file_id{};
    if (CoCreateGuid(&file_id) != S_OK) {
        return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not create a unique torrent metadata filename");
    }
    wchar_t guid_text[40]{};
    if (StringFromGUID2(file_id, guid_text, static_cast<int>(std::size(guid_text))) == 0) {
        return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not create a unique torrent metadata filename");
    }
    std::wstring guid(guid_text);
    guid.erase(std::remove(guid.begin(), guid.end(), L'{'), guid.end());
    guid.erase(std::remove(guid.begin(), guid.end(), L'}'), guid.end());
    const auto task_dir = torrent_root / guid;
    std::filesystem::create_directories(task_dir, ec);
    if (ec) return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not create the task metadata folder");
    const auto final_path = task_dir / L"metainfo.torrent";
    const auto pending_path = task_dir / L"metainfo.tmp";
    {
        std::ofstream output(pending_path, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(body.data(), static_cast<std::streamsize>(body.size()))) {
            std::filesystem::remove(pending_path, ec);
            return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not save the received torrent metadata");
        }
    }
    std::filesystem::rename(pending_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(pending_path, ec);
        return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not finalize the received torrent metadata");
    }
    return Result<std::string>::Ok(final_path.string());
}

struct TorrentMetadataSelectionRequest {
    torrent::TorrentInfo* info{nullptr};
    bool accepted{false};
};

constexpr int IDM_TRAY_SHOW        = 2001;
constexpr int IDM_TRAY_HIDE        = 2002;
constexpr int IDM_TRAY_PAUSE_ALL   = 2003;
constexpr int IDM_TRAY_RESUME_ALL  = 2004;
constexpr int IDM_TRAY_EXIT        = 2005;

constexpr int IDM_CTX_START        = 3001;
constexpr int IDM_CTX_PAUSE        = 3002;
constexpr int IDM_CTX_RESUME       = 3003;
constexpr int IDM_CTX_RETRY        = 3004;
constexpr int IDM_CTX_CANCEL       = 3005;
constexpr int IDM_CTX_OPEN_FILE    = 3006;
constexpr int IDM_CTX_OPEN_FOLDER  = 3007;
constexpr int IDM_CTX_REMOVE_LIST  = 3008;
constexpr int IDM_CTX_DELETE_DISK  = 3009;
constexpr int IDM_CTX_SETTINGS     = 3010;
constexpr int IDM_CTX_BROWSER_STAT = 3011;
constexpr int IDM_CTX_ABOUT        = 3012;
constexpr int IDM_CTX_ANTIVIRUS    = 3013;
constexpr int IDM_CTX_ZIP_PREVIEW   = 3014;
constexpr int IDM_CTX_EXPORT_TASKS  = 3015;
constexpr int IDM_CTX_IMPORT_TASKS  = 3016;
constexpr int IDM_CTX_OPEN_TORRENT  = 3025;
constexpr int IDM_CTX_RENAME        = 3017;
constexpr int IDM_CTX_MOVE_TO       = 3018;
constexpr int IDM_CTX_COPY_URL      = 3019;
constexpr int IDM_CTX_REFRESH_URL   = 3020;
constexpr int IDM_CTX_ADD_MIRROR    = 3021;
constexpr int IDM_CTX_SCHEDULE      = 3022;
constexpr int IDM_CTX_SPEED_LIMIT   = 3024;
constexpr int IDM_CTX_FILE_INTEGRITY = 3023;

struct Win32WindowContext {
    DesktopView* view{nullptr};
    DownloadController* controller{nullptr};
    HWND hwnd{nullptr};
    HWND hSearchEdit{nullptr};
    NOTIFYICONDATAW nid{};
    bool tray_added{false};
    bool is_dragging_vscroll{false};
    int vscroll_drag_start_y{0};
    size_t vscroll_drag_start_offset{0};
    bool is_vscroll_hovered{false};
    ITaskbarList3* pTaskbarList{nullptr};
    bool com_initialized{false};
    TaskbarProgressInfo last_taskbar_info{};
    bool taskbar_info_initialized{false};
    bool mouse_tracking_active{false};
};

// Modern Edit Control Subclass: custom padding, Segoe UI typography, subtle borders, and Coral Red focus glow
constexpr UINT_PTR SUBCLASS_MODERN_EDIT_ID = 9001;

struct ModernEditData {
    bool is_focused{false};
    bool is_hovered{false};
    int pad_x{8};
    int pad_y{4};
    int radius{6};
};

LRESULT CALLBACK ModernEditSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* data = reinterpret_cast<ModernEditData*>(dwRefData);
    if (!data) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    switch (uMsg) {
        case WM_SETFOCUS: {
            data->is_focused = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_KILLFOCUS: {
            data->is_focused = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_MOUSEMOVE: {
            if (!data->is_hovered) {
                data->is_hovered = true;
                TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            data->is_hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_PAINT: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            // Draw modern border over non-client / border region
            HDC hdc = GetWindowDC(hWnd);
            if (hdc) {
                RECT wr{};
                GetWindowRect(hWnd, &wr);
                RECT rc{0, 0, wr.right - wr.left, wr.bottom - wr.top};
                bool is_enabled = IsWindowEnabled(hWnd) != FALSE;
                
                // Focus uses the shared Radium coral accent.
                // Hover: #555866
                // Normal: #3E404A
                // Disabled: #2A2A30
                COLORREF border_clr = !is_enabled ? RGB(0x2A, 0x2A, 0x30)
                                      : (data->is_focused ? DesignSystem::GetRedesignPalette().accent_primary.ToWin32ColorRef()
                                      : (data->is_hovered ? RGB(0x55, 0x58, 0x66) : RGB(0x3E, 0x40, 0x4A)));
                
                HBRUSH hBorderBrush = CreateSolidBrush(border_clr);
                FrameRect(hdc, &rc, hBorderBrush);
                DeleteObject(hBorderBrush);

                // Subtle inner accent line when focused for a crisp, high-DPI modern border feel
                if (data->is_focused && is_enabled) {
                    RECT inner_rc = rc;
                    InflateRect(&inner_rc, -1, -1);
                    HBRUSH hGlowBrush = CreateSolidBrush(RGB(0x80, 0x27, 0x2B));
                    FrameRect(hdc, &inner_rc, hGlowBrush);
                    DeleteObject(hGlowBrush);
                }
                ReleaseDC(hWnd, hdc);
            }
            return res;
        }
        case WM_NCPAINT: {
            // Suppress standard Windows 3D border painting
            return 0;
        }
        case WM_NCDESTROY: {
            RemoveWindowSubclass(hWnd, ModernEditSubclassProc, uIdSubclass);
            delete data;
            break;
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void InstallModernEditControl(HWND hEdit, uint32_t dpi = 96) {
    if (!hEdit) return;
    int pad_x = DesignSystem::ScaleForDpi(8, dpi);
    int pad_y = DesignSystem::ScaleForDpi(4, dpi);
    int r = DesignSystem::ScaleForDpi(6, dpi);
    SendMessageW(hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(pad_x, pad_x));

    auto* data = new ModernEditData{false, false, pad_x, pad_y, r};
    SetWindowSubclass(hEdit, ModernEditSubclassProc, SUBCLASS_MODERN_EDIT_ID, reinterpret_cast<DWORD_PTR>(data));
}

bool SyncTaskbarProgress(HWND hwnd, ITaskbarList3* pTaskbar, const DesktopView& view, Win32WindowContext* ctx) {
    if (!hwnd || !pTaskbar) return false;
    auto info = view.ComputeTaskbarProgress();
    
    // Prevent flickering: check if taskbar state or relevant values actually changed and were successfully applied previously
    if (ctx && ctx->taskbar_info_initialized) {
        if (ctx->last_taskbar_info.state == info.state &&
            ctx->last_taskbar_info.total_bytes == info.total_bytes) {
            if (info.state == TaskbarProgressState::kNoProgress ||
                info.state == TaskbarProgressState::kIndeterminate ||
                info.state == TaskbarProgressState::kError) {
                return true; // Nothing changed, skip redundant Win32 ITaskbarList3 COM dispatch
            }
            if (info.state == TaskbarProgressState::kNormal || info.state == TaskbarProgressState::kPaused) {
                // If progress value delta is identical or negligible, skip
                if (ctx->last_taskbar_info.completed_bytes == info.completed_bytes) {
                    return true;
                }
            }
        }
    }

    HRESULT hrState = S_OK;
    HRESULT hrVal = S_OK;

    switch (info.state) {
        case TaskbarProgressState::kNoProgress: {
            // Explicitly clear progress value and reset state to TBPF_NOPROGRESS with verification and retry
            bool reset_ok = false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                hrVal = pTaskbar->SetProgressValue(hwnd, 0, 1);
                hrState = pTaskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
                if (SUCCEEDED(hrState) && SUCCEEDED(hrVal)) {
                    reset_ok = true;
                    break;
                }
                Sleep(10);
            }
            OPENIDM_LOG_INFO("UI", "Taskbar progress cleared via TBPF_NOPROGRESS (hrState=" + 
                             std::to_string(static_cast<long>(hrState)) + ", success=" + (reset_ok ? "true" : "false") + ")");
            break;
        }
        case TaskbarProgressState::kIndeterminate:
            hrState = pTaskbar->SetProgressState(hwnd, TBPF_INDETERMINATE);
            break;
        case TaskbarProgressState::kNormal:
            hrState = pTaskbar->SetProgressState(hwnd, TBPF_NORMAL);
            hrVal = pTaskbar->SetProgressValue(hwnd, info.completed_bytes, std::max<uint64_t>(1ULL, info.total_bytes));
            break;
        case TaskbarProgressState::kPaused:
            hrState = pTaskbar->SetProgressState(hwnd, TBPF_PAUSED);
            if (info.total_bytes > 0) {
                hrVal = pTaskbar->SetProgressValue(hwnd, info.completed_bytes, info.total_bytes);
            }
            break;
        case TaskbarProgressState::kError:
            hrState = pTaskbar->SetProgressState(hwnd, TBPF_ERROR);
            break;
    }

    bool success = SUCCEEDED(hrState) && SUCCEEDED(hrVal);
    if (ctx) {
        if (success) {
            ctx->last_taskbar_info = info;
            ctx->taskbar_info_initialized = true;
        } else {
            // If the COM call failed (e.g., explorer restarting, E_FAIL, or invalid hwnd),
            // invalidate cached state so the next UI timer tick will retry.
            ctx->taskbar_info_initialized = false;
        }
    }

    return success;
}

RECT ToWinRect(const RectI& r) {
    return RECT{r.left, r.top, r.right, r.bottom};
}

void FillRectSolid(HDC hdc, const RECT& rc, ColorRgba col) {
    HBRUSH br = CreateSolidBrush(col.ToWin32ColorRef());
    FillRect(hdc, &rc, br);
    DeleteObject(br);
}

void FrameRectBorder(HDC hdc, const RECT& rc, ColorRgba col, int thickness = 1) {
    HBRUSH br = CreateSolidBrush(col.ToWin32ColorRef());
    RECT top_r{rc.left, rc.top, rc.right, rc.top + thickness};
    RECT bot_r{rc.left, rc.bottom - thickness, rc.right, rc.bottom};
    RECT lft_r{rc.left, rc.top, rc.left + thickness, rc.bottom};
    RECT rgt_r{rc.right - thickness, rc.top, rc.right, rc.bottom};
    FillRect(hdc, &top_r, br);
    FillRect(hdc, &bot_r, br);
    FillRect(hdc, &lft_r, br);
    FillRect(hdc, &rgt_r, br);
    DeleteObject(br);
}

void ApplyWindowGlassBackdropWin32(HWND hwnd, bool is_transient_dialog = false) {
    if (!hwnd || !IsWindow(hwnd)) return;

    // 1. Immersive dark mode (DWMWA_USE_IMMERSIVE_DARK_MODE = 20)
    BOOL use_dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &use_dark, sizeof(use_dark));

    // 2. Windows 11 rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = 33, 2 = DWMWCP_ROUND)
    int corner_pref = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner_pref, sizeof(corner_pref));

    // 3. Titlebar styling
    COLORREF titlebar_col = RGB(0x16, 0x16, 0x18);
    DwmSetWindowAttribute(hwnd, 35, &titlebar_col, sizeof(titlebar_col));
    COLORREF titletext_col = RGB(0xF2, 0xF2, 0xF3);
    DwmSetWindowAttribute(hwnd, 36, &titletext_col, sizeof(titletext_col));

    // 4. Windows 11 Mica / Acrylic backdrop (DWMWA_SYSTEMBACKDROP_TYPE = 38)
    // 2 = DWMSBT_MAINWINDOW (Mica), 3 = DWMSBT_TRANSIENTWINDOW (Acrylic)
    int backdrop_type = is_transient_dialog ? 3 : 2;
    HRESULT hr = DwmSetWindowAttribute(hwnd, 38, &backdrop_type, sizeof(backdrop_type));
    if (FAILED(hr)) {
        // Fallback for Windows 11 21H2 (Build 22000): DWMWA_MICA_EFFECT = 1029
        BOOL mica_on = TRUE;
        DwmSetWindowAttribute(hwnd, 1029, &mica_on, sizeof(mica_on));
    }
}

void DrawGlassPanelGdi(HDC hdc, const RECT& rc, int radius, ColorRgba fill, ColorRgba border, int border_w = 1, bool add_top_specular = true) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HRGN hRgn = (radius > 0) ? CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius) : nullptr;
    if (hRgn) {
        SelectClipRgn(hdc, hRgn);
    }

    HDC memDC = CreateCompatibleDC(hdc);
    if (memDC) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = 1;
        bmi.bmiHeader.biHeight = 1;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hbm && bits) {
            uint8_t a = fill.a;
            uint8_t pr = static_cast<uint8_t>((static_cast<uint32_t>(fill.r) * a) / 255u);
            uint8_t pg = static_cast<uint8_t>((static_cast<uint32_t>(fill.g) * a) / 255u);
            uint8_t pb = static_cast<uint8_t>((static_cast<uint32_t>(fill.b) * a) / 255u);
            *reinterpret_cast<uint32_t*>(bits) = (static_cast<uint32_t>(a) << 24) |
                                                  (static_cast<uint32_t>(pr) << 16) |
                                                  (static_cast<uint32_t>(pg) << 8) |
                                                  static_cast<uint32_t>(pb);

            HGDIOBJ oldBm = SelectObject(memDC, hbm);
            BLENDFUNCTION bf{};
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;

            AlphaBlend(hdc, rc.left, rc.top, w, h, memDC, 0, 0, 1, 1, bf);

            if (add_top_specular && h > 6) {
                uint8_t ha = 36;
                *reinterpret_cast<uint32_t*>(bits) = (static_cast<uint32_t>(ha) << 24) |
                                                      (static_cast<uint32_t>(ha) << 16) |
                                                      (static_cast<uint32_t>(ha) << 8) |
                                                      static_cast<uint32_t>(ha);
                int hl_margin = (radius > 4) ? (radius / 2) : 2;
                int hl_w = std::max(2, w - hl_margin * 2);
                AlphaBlend(hdc, rc.left + hl_margin, rc.top + 1, hl_w, 1, memDC, 0, 0, 1, 1, bf);
            }

            SelectObject(memDC, oldBm);
            DeleteObject(hbm);
        }
        DeleteDC(memDC);
    }

    if (hRgn) {
        SelectClipRgn(hdc, nullptr);
        DeleteObject(hRgn);
    }

    if (border_w > 0) {
        HPEN hPen = CreatePen(PS_SOLID, border_w, border.ToWin32ColorRef());
        HGDIOBJ oldPen = SelectObject(hdc, hPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

        if (radius > 0) {
            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
        } else {
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        }

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(hPen);
    }
}

bool SelectFolderWithDialogWin32(HWND hwnd, std::string& io_folder) {
    wchar_t display_name[MAX_PATH]{};
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.pszDisplayName = display_name;
    bi.lpszTitle = L"Select Default Download Directory for Radium";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path_buf[MAX_PATH]{};
    bool ok = (SHGetPathFromIDListW(pidl, path_buf) != FALSE);
    CoTaskMemFree(pidl);
    if (ok && path_buf[0] != L'\0') {
        io_folder = WToUtf8(path_buf);
        return true;
    }
    return false;
}

struct QualityChoice {
    std::string id;
    std::string label;
    std::string resolution;
    int height{0};
    int64_t size{-1};
    bool size_estimated{false};
    std::string video_filter;
    std::string audio_filter{"best"};
    std::string video_url;
    std::string paired_audio_url;
    bool is_audio_only{false};
    std::string ext{"mp4"};
    int width{0};
    int itag{0};
};

struct NewDownloadDialogContext {
    DesktopView* view{nullptr};
    DownloadController* controller{nullptr};
    HWND hDlg{nullptr};
    HWND hUrlEdit{nullptr};
    HWND hFilenameEdit{nullptr};
    HWND hSaveDirEdit{nullptr};
    HWND hBrowseBtn{nullptr};
    HWND hQualityCombo{nullptr};
    HWND hFileSizeLabel{nullptr};
    HWND hSpaceLabel{nullptr};
    HWND hStartChk{nullptr};
    HWND hScheduleChk{nullptr};
    HWND hScheduleTimeEdit{nullptr};
    HWND hDownloadBtn{nullptr};
    HWND hCancelBtn{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontTitle{nullptr};
    HFONT hFontSmall{nullptr};
    HBRUSH hBgBrush{nullptr};
    HBRUSH hEditBrush{nullptr};
    std::string existing_task_id;
    std::string initial_filename;
    std::string initial_save_dir;
    int64_t initial_file_size{-1};
    uint32_t dpi{96};
    bool submitted{false};
    bool is_submitting{false};
    std::vector<QualityChoice> quality_choices;

    uint64_t current_probe_id{0};
    std::shared_ptr<std::atomic<bool>> dlg_alive{std::make_shared<std::atomic<bool>>(true)};
};

void DrawRoundedBoxGdi(HDC hdc, const RECT& rc, int radius, ColorRgba bg, ColorRgba border, int border_w);

LRESULT CALLBACK NewDownloadDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<NewDownloadDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<NewDownloadDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_OPENIDM_PROBE_COMPLETE: {
            auto* p = reinterpret_cast<ProbeResultPayload*>(lParam);
            if (!p) return 0;
            std::unique_ptr<ProbeResultPayload> auto_cleanup(p);
            if (ctx && ctx->dlg_alive && *(ctx->dlg_alive) && p->request_id == ctx->current_probe_id) {
                if (p->success && p->content_length > 0) {
                    ctx->initial_file_size = p->content_length;
                    std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(p->content_length));
                    std::wstring txt = L"Download Size: " + Utf8ToW(sz_str);
                    SetWindowTextW(ctx->hFileSizeLabel, txt.c_str());
                } else {
                    SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Unknown size");
                }
                if (!p->final_filename.empty()) {
                    wchar_t fn_buf[512]{};
                    GetWindowTextW(ctx->hFilenameEdit, fn_buf, 511);
                    std::string cur_fn = WToUtf8(fn_buf);
                    if (cur_fn.empty() || cur_fn == "download") {
                        SetWindowTextW(ctx->hFilenameEdit, Utf8ToW(p->final_filename).c_str());
                    }
                }
            }
            return 0;
        }
        case DM_GETDEFID: {
            return MAKELRESULT(5008, DC_HASDEFID);
        }
        case DM_SETDEFID: {
            return TRUE;
        }
        case WM_CREATE: {
            ctx->hDlg = hwnd;
            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;
            ctx->hFontBody = CreateFontW(
                -DesignSystem::ScaleForDpi(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBold = CreateFontW(
                -DesignSystem::ScaleForDpi(15, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontTitle = CreateFontW(
                -DesignSystem::ScaleForDpi(19, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI Variable Display");
            ctx->hFontSmall = CreateFontW(
                -DesignSystem::ScaleForDpi(12, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hBgBrush = CreateSolidBrush(RGB(0x16, 0x16, 0x18));
            ctx->hEditBrush = CreateSolidBrush(RGB(0x20, 0x20, 0x26));

            HINSTANCE hInst = GetModuleHandleW(nullptr);

            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int full_w = DesignSystem::ScaleForDpi(532, dpi);
            int edit_h = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kControlHeight, dpi);

            // URL Edit (Row 0: Label at 76, Control at 98)
            int y_url = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(0), dpi);
            ctx->hUrlEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad_x, y_url, full_w, edit_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5001)), hInst, nullptr);
            SendMessageW(ctx->hUrlEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hUrlEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Paste or enter download URL (HTTP, HTTPS, FTP, or video link)..."));
            InstallModernEditControl(ctx->hUrlEdit, dpi);

            // Filename Edit (Row 1: Label at 136, Control at 158)
            int y_fn = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(1), dpi);
            ctx->hFilenameEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad_x, y_fn, full_w, edit_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5002)), hInst, nullptr);
            SendMessageW(ctx->hFilenameEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hFilenameEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"File name (leave blank to auto-detect from server)"));
            InstallModernEditControl(ctx->hFilenameEdit, dpi);

            // Save Dir Edit & Browse Button (Row 2: Label at 196, Control at 218)
            auto s = ctx->controller->GetSettings();
            std::wstring def_dir = ctx->initial_save_dir.empty() ? Utf8ToW(s.default_download_dir) : Utf8ToW(ctx->initial_save_dir);
            int y_dir = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(2), dpi);
            int browse_w = DesignSystem::ScaleForDpi(100, dpi);
            int dir_w = full_w - browse_w - DesignSystem::ScaleForDpi(10, dpi);
            ctx->hSaveDirEdit = CreateWindowExW(0, L"EDIT", def_dir.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad_x, y_dir, dir_w, edit_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5003)), hInst, nullptr);
            SendMessageW(ctx->hSaveDirEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernEditControl(ctx->hSaveDirEdit, dpi);

            ctx->hBrowseBtn = CreateWindowExW(0, L"BUTTON", L"Browse...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                pad_x + dir_w + DesignSystem::ScaleForDpi(10, dpi), y_dir, browse_w, edit_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5004)), hInst, nullptr);

            // Quality / Format Combobox (Row 3: Label at 256, Control at 278)
            int y_q = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(3), dpi);
            int q_w = DesignSystem::ScaleForDpi(280, dpi);
            ctx->hQualityCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                pad_x, y_q, q_w, DesignSystem::ScaleForDpi(180, dpi), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5005)), hInst, nullptr);
            SendMessageW(ctx->hQualityCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            bool is_video_stream = false;
            std::optional<TaskModel> existing_task;
            if (!ctx->existing_task_id.empty()) {
                auto t = ctx->controller->GetTask(ctx->existing_task_id);
                if (t.has_value()) {
                    existing_task = t;
                    if (t->task_type == TaskType::kStreamHls || t->task_type == TaskType::kStreamDash || t->category == "Video") {
                        is_video_stream = true;
                    }
                }
            }

            ctx->quality_choices.clear();
            if (existing_task.has_value() && !existing_task->stream_options.cached_manifest_body.empty() &&
                (existing_task->stream_options.cached_manifest_body.front() == '{' || existing_task->stream_options.cached_manifest_body.front() == '[')) {
                try {
                    auto root = openidm::json::Parse(existing_task->stream_options.cached_manifest_body);
                    if (root.is_object() && root["qualities"].is_array()) {
                        for (const auto& q : root["qualities"].as_array()) {
                            QualityChoice qc;
                            qc.id = q["id"].as_string();
                            qc.label = q["label"].as_string();
                            qc.resolution = q["resolution"].as_string();
                            qc.height = static_cast<int>(q["height"].as_int64(0));
                            qc.width = static_cast<int>(q["width"].as_int64(0));
                            qc.itag = static_cast<int>(q["itag"].as_int64(0));
                            qc.size = q["size"].as_int64(-1);
                            qc.size_estimated = q["sizeEstimated"].as_bool(false);
                            qc.video_filter = q["videoFilter"].as_string(qc.id);
                            qc.audio_filter = "best";
                            qc.video_url = q["videoUrl"].as_string();
                            qc.paired_audio_url = q["pairedAudioUrl"].as_string();
                            qc.is_audio_only = q["isAudioOnly"].as_bool(false);
                            qc.ext = q["ext"].as_string(qc.is_audio_only ? "m4a" : "mp4");
                            if (qc.label.empty()) {
                                qc.label = !qc.resolution.empty() ? qc.resolution : qc.id;
                            }
                            ctx->quality_choices.push_back(std::move(qc));
                        }
                    }
                } catch (...) {}
            }

            if (ctx->quality_choices.empty()) {
                if (is_video_stream) {
                    ctx->quality_choices.push_back({"best", "Best Available Quality (Auto)", "Auto", 0, ctx->initial_file_size, true, "best", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"1080p", "1080p Full HD", "1080p", 1080, -1, false, "1080p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"720p", "720p HD", "720p", 720, -1, false, "720p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"480p", "480p SD", "480p", 480, -1, false, "480p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"360p", "360p Standard", "360p", 360, -1, false, "360p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"144p", "144p Compact", "144p", 144, -1, false, "144p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"audio", "Audio Only (M4A)", "Audio", 0, -1, false, "audio", "best", "", "", true, "m4a"});
                } else {
                    ctx->quality_choices.push_back({"original", "Original / Direct File (Server Default)", "", 0, ctx->initial_file_size, false, "best", "best", "", "", false, ""});
                }
            }

            int sel_idx = 0;
            std::string initial_vfilter = existing_task.has_value() ? existing_task->stream_options.video_filter : "best";
            std::string low_init = ToLowerStr(initial_vfilter);
            int target_itag = 0;
            if (existing_task.has_value() && !existing_task->stream_options.cached_manifest_body.empty() &&
                (existing_task->stream_options.cached_manifest_body.front() == '{' || existing_task->stream_options.cached_manifest_body.front() == '[')) {
                try {
                    auto mroot = openidm::json::Parse(existing_task->stream_options.cached_manifest_body);
                    if (mroot.is_object()) {
                        if (mroot["selectedItag"].as_int64(0) > 0) target_itag = static_cast<int>(mroot["selectedItag"].as_int64(0));
                        else if (mroot["itag"].as_int64(0) > 0) target_itag = static_cast<int>(mroot["itag"].as_int64(0));
                        if ((low_init.empty() || low_init == "best" || low_init == "auto") && mroot["resolution"].is_string()) {
                            low_init = ToLowerStr(mroot["resolution"].as_string());
                        }
                    }
                } catch (...) {}
            }

            bool matched_explicit = false;
            for (size_t i = 0; i < ctx->quality_choices.size(); ++i) {
                const auto& qc = ctx->quality_choices[i];
                std::wstring disp = Utf8ToW(qc.label);
                if (qc.size > 0) {
                    std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(qc.size));
                    disp += qc.size_estimated ? (L" (~" + Utf8ToW(sz_str) + L" est.)") : (L" (" + Utf8ToW(sz_str) + L")");
                }
                SendMessageW(ctx->hQualityCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(disp.c_str()));
                if (!matched_explicit) {
                    if (target_itag > 0 && qc.itag == target_itag) {
                        sel_idx = static_cast<int>(i);
                        matched_explicit = true;
                    } else if (!low_init.empty() && low_init != "best" && low_init != "auto") {
                        std::string low_id = ToLowerStr(qc.id);
                        std::string low_res = ToLowerStr(qc.resolution);
                        std::string low_vf = ToLowerStr(qc.video_filter);
                        if (low_id == low_init || low_res == low_init || low_vf == low_init ||
                            (!low_id.empty() && low_init.find(low_id) != std::string::npos) ||
                            (!low_init.empty() && low_id.find(low_init) != std::string::npos)) {
                            sel_idx = static_cast<int>(i);
                            matched_explicit = true;
                        }
                    }
                }
            }
            if (!matched_explicit && !ctx->quality_choices.empty()) {
                sel_idx = 0;
                for (size_t i = 0; i < ctx->quality_choices.size(); ++i) {
                    if (!ctx->quality_choices[i].is_audio_only) {
                        sel_idx = static_cast<int>(i);
                        break;
                    }
                }
            }
            SendMessageW(ctx->hQualityCombo, CB_SETCURSEL, sel_idx, 0);

            // File Size Label
            std::wstring sz_text = L"Size: Unknown";
            if (sel_idx < static_cast<int>(ctx->quality_choices.size()) && ctx->quality_choices[sel_idx].size > 0) {
                const auto& qc = ctx->quality_choices[sel_idx];
                std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(qc.size));
                sz_text = std::wstring(L"Size: ") + (qc.size_estimated ? L"~" : L"") + Utf8ToW(sz_str) + (qc.size_estimated ? L" (estimated)" : L"");
            } else if (ctx->initial_file_size > 0) {
                sz_text = L"Size: " + Utf8ToW(DesktopView::FormatSizePretty(static_cast<uint64_t>(ctx->initial_file_size)));
            }
            int sz_x = pad_x + q_w + DesignSystem::ScaleForDpi(16, dpi);
            int sz_w = full_w - q_w - DesignSystem::ScaleForDpi(16, dpi);
            ctx->hFileSizeLabel = CreateWindowExW(0, L"STATIC", sz_text.c_str(),
                WS_CHILD | WS_VISIBLE,
                sz_x, y_q + DesignSystem::ScaleForDpi(2, dpi), sz_w, DesignSystem::ScaleForDpi(22, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5012)), hInst, nullptr);
            SendMessageW(ctx->hFileSizeLabel, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);

            // Disk Space Label
            std::string space_text = "Disk space: Checking...";
            auto space_res = PathSanitizer::CheckAvailableDiskSpace(s.default_download_dir, 1024);
            if (space_res.ok() && space_res.value.has_value()) {
                space_text = "Free space: " + DesktopView::FormatSizePretty(*space_res.value) + " available on destination drive";
            }
            int y_space = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kSpaceLabelTop, dpi);
            ctx->hSpaceLabel = CreateWindowExW(0, L"STATIC", Utf8ToW(space_text).c_str(),
                WS_CHILD | WS_VISIBLE,
                pad_x, y_space, full_w, DesignSystem::ScaleForDpi(20, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5006)), hInst, nullptr);
            SendMessageW(ctx->hSpaceLabel, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            // Start Immediately Checkbox
            int y_start = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kStartChkTop, dpi);
            int start_w = DesignSystem::ScaleForDpi(206, dpi);
            ctx->hStartChk = CreateWindowExW(0, L"BUTTON", L"Start download immediately",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                pad_x, y_start, start_w, DesignSystem::ScaleForDpi(24, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5007)), hInst, nullptr);
            SendMessageW(ctx->hStartChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hStartChk, BM_SETCHECK, s.auto_start_downloads ? BST_CHECKED : BST_UNCHECKED, 0);

            // Schedule Checkbox & Time Edit
            int y_sched = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kScheduleChkTop, dpi);
            int sched_w = DesignSystem::ScaleForDpi(130, dpi);
            int sched_x = pad_x + start_w + DesignSystem::ScaleForDpi(8, dpi);
            ctx->hScheduleChk = CreateWindowExW(0, L"BUTTON", L"Schedule at:",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                sched_x, y_sched, sched_w, DesignSystem::ScaleForDpi(24, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5010)), hInst, nullptr);
            SendMessageW(ctx->hScheduleChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);

            ctx->hScheduleTimeEdit = CreateWindowExW(0, L"EDIT", L"23:00",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_DISABLED,
                sched_x + sched_w + DesignSystem::ScaleForDpi(8, dpi), y_sched,
                DesignSystem::ScaleForDpi(72, dpi), DesignSystem::ScaleForDpi(24, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5011)), hInst, nullptr);
            SendMessageW(ctx->hScheduleTimeEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernEditControl(ctx->hScheduleTimeEdit, dpi);

            // Download & Cancel Action Buttons
            int y_btn = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonsTop, dpi);
            int btn_action_h = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonHeight, dpi);
            int cancel_w = DesignSystem::ScaleForDpi(100, dpi);
            int dl_btn_w = DesignSystem::ScaleForDpi(124, dpi);
            int cancel_x = pad_x + full_w - cancel_w;
            int dl_x = cancel_x - dl_btn_w - DesignSystem::ScaleForDpi(10, dpi);

            ctx->hDownloadBtn = CreateWindowExW(0, L"BUTTON", L"Download",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                dl_x, y_btn, dl_btn_w, btn_action_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5008)), hInst, nullptr);

            ctx->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                cancel_x, y_btn, cancel_w, btn_action_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5009)), hInst, nullptr);

            return 0;
        }
        case WM_SIZE: {
            if (!ctx || !ctx->hUrlEdit) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int client_w = rc.right - rc.left;
            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;
            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int full_w = client_w - pad_x * 2;
            if (full_w < DesignSystem::ScaleForDpi(300, dpi)) full_w = DesignSystem::ScaleForDpi(300, dpi);
            int edit_h = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kControlHeight, dpi);

            // Row 0: URL Edit
            int y_url = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(0), dpi);
            SetWindowPos(ctx->hUrlEdit, nullptr, pad_x, y_url, full_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 1: Filename Edit
            int y_fn = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(1), dpi);
            SetWindowPos(ctx->hFilenameEdit, nullptr, pad_x, y_fn, full_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 2: Save Location & Browse
            int y_dir = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(2), dpi);
            int browse_w = DesignSystem::ScaleForDpi(100, dpi);
            int dir_w = full_w - browse_w - DesignSystem::ScaleForDpi(10, dpi);
            SetWindowPos(ctx->hSaveDirEdit, nullptr, pad_x, y_dir, dir_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hBrowseBtn, nullptr, pad_x + dir_w + DesignSystem::ScaleForDpi(10, dpi), y_dir, browse_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 3: Quality Combo & Size Label
            int y_q = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(3), dpi);
            int q_w = DesignSystem::ScaleForDpi(280, dpi);
            if (q_w > full_w - DesignSystem::ScaleForDpi(160, dpi)) q_w = full_w - DesignSystem::ScaleForDpi(160, dpi);
            SetWindowPos(ctx->hQualityCombo, nullptr, pad_x, y_q, q_w, DesignSystem::ScaleForDpi(180, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            int sz_x = pad_x + q_w + DesignSystem::ScaleForDpi(16, dpi);
            int sz_w = full_w - q_w - DesignSystem::ScaleForDpi(16, dpi);
            SetWindowPos(ctx->hFileSizeLabel, nullptr, sz_x, y_q + DesignSystem::ScaleForDpi(2, dpi), sz_w, DesignSystem::ScaleForDpi(22, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 4: Space Label
            int y_space = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kSpaceLabelTop, dpi);
            SetWindowPos(ctx->hSpaceLabel, nullptr, pad_x, y_space, full_w, DesignSystem::ScaleForDpi(20, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 5: Start & Schedule Checkboxes
            int y_start = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kStartChkTop, dpi);
            int start_w = DesignSystem::ScaleForDpi(206, dpi);
            SetWindowPos(ctx->hStartChk, nullptr, pad_x, y_start, start_w, DesignSystem::ScaleForDpi(24, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            int y_sched = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kScheduleChkTop, dpi);
            int sched_w = DesignSystem::ScaleForDpi(130, dpi);
            int sched_x = pad_x + start_w + DesignSystem::ScaleForDpi(8, dpi);
            SetWindowPos(ctx->hScheduleChk, nullptr, sched_x, y_sched, sched_w, DesignSystem::ScaleForDpi(24, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hScheduleTimeEdit, nullptr, sched_x + sched_w + DesignSystem::ScaleForDpi(8, dpi), y_sched, DesignSystem::ScaleForDpi(72, dpi), DesignSystem::ScaleForDpi(24, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 6: Buttons aligned to bottom right
            int y_btn = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonsTop, dpi);
            int btn_action_h = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonHeight, dpi);
            int cancel_w = DesignSystem::ScaleForDpi(100, dpi);
            int dl_btn_w = DesignSystem::ScaleForDpi(124, dpi);
            int cancel_x = pad_x + full_w - cancel_w;
            int dl_x = cancel_x - dl_btn_w - DesignSystem::ScaleForDpi(10, dpi);
            SetWindowPos(ctx->hCancelBtn, nullptr, cancel_x, y_btn, cancel_w, btn_action_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hDownloadBtn, nullptr, dl_x, y_btn, dl_btn_w, btn_action_h, SWP_NOZORDER | SWP_NOACTIVATE);

            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;
            HDC hdcBtn = dis->hDC;
            RECT rcBtn = dis->rcItem;
            bool is_selected = (dis->itemState & ODS_SELECTED) != 0;
            bool is_focused = (dis->itemState & ODS_FOCUS) != 0;
            uint32_t btn_dpi = ctx->dpi ? ctx->dpi : 96;
            FillRect(hdcBtn, &rcBtn, ctx->hBgBrush);

            if (dis->CtlID == 5008) { // Download button (Coral accent #FF4F57)
                int btn_r = DesignSystem::ScaleForDpi(8, btn_dpi);
                const auto& accent = ctx->view->GetPalette().accent_primary;
                ColorRgba bg_col = is_selected ? accent.Interpolate(ColorRgba{0, 0, 0, 255}, 0.15f) : (is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.12f) : accent);
                ColorRgba border_col = is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.32f) : bg_col;
                DrawRoundedBoxGdi(hdcBtn, rcBtn, btn_r, bg_col, border_col, is_focused ? 1 : 0);
                SelectObject(hdcBtn, ctx->hFontBold);
                SetTextColor(hdcBtn, RGB(255, 255, 255));
                SetBkMode(hdcBtn, TRANSPARENT);
                DrawTextW(hdcBtn, L"Download", -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 5009) { // Cancel button (Glass elevated)
                int btn_r = DesignSystem::ScaleForDpi(8, btn_dpi);
                ColorRgba bg_col = is_selected ? ColorRgba{0x33, 0x33, 0x38, 255} : (is_focused ? ColorRgba{0x2A, 0x2A, 0x30, 255} : ColorRgba{0x22, 0x22, 0x28, 255});
                ColorRgba border_col = is_focused ? ColorRgba{0x55, 0x58, 0x66, 220} : ColorRgba{0x3E, 0x40, 0x4A, 180};
                DrawRoundedBoxGdi(hdcBtn, rcBtn, btn_r, bg_col, border_col, 1);
                SelectObject(hdcBtn, ctx->hFontBody);
                SetTextColor(hdcBtn, RGB(0xDE, 0xDF, 0xE4));
                SetBkMode(hdcBtn, TRANSPARENT);
                DrawTextW(hdcBtn, L"Cancel", -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 5004) { // Browse button (Glass secondary)
                int btn_r = DesignSystem::ScaleForDpi(6, btn_dpi);
                ColorRgba bg_col = is_selected ? ColorRgba{0x33, 0x33, 0x38, 255} : (is_focused ? ColorRgba{0x2A, 0x2A, 0x30, 255} : ColorRgba{0x22, 0x22, 0x28, 255});
                ColorRgba border_col = is_focused ? ColorRgba{0x55, 0x58, 0x66, 220} : ColorRgba{0x3E, 0x40, 0x4A, 180};
                DrawRoundedBoxGdi(hdcBtn, rcBtn, btn_r, bg_col, border_col, 1);
                SelectObject(hdcBtn, ctx->hFontBody);
                SetTextColor(hdcBtn, RGB(0xDE, 0xDF, 0xE4));
                SetBkMode(hdcBtn, TRANSPARENT);
                DrawTextW(hdcBtn, L"Browse...", -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdcEdit, RGB(0xF2, 0xF2, 0xF3));
            SetBkColor(hdcEdit, RGB(0x20, 0x20, 0x26));
            return reinterpret_cast<LRESULT>(ctx->hEditBrush);
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = reinterpret_cast<HDC>(wParam);
            HWND hCtl = reinterpret_cast<HWND>(lParam);
            if (hCtl == ctx->hQualityCombo) {
                SetBkMode(hdcStatic, OPAQUE);
                SetBkColor(hdcStatic, RGB(0x20, 0x20, 0x26));
                SetTextColor(hdcStatic, RGB(0xF2, 0xF2, 0xF3));
                return reinterpret_cast<LRESULT>(ctx->hEditBrush);
            } else if (hCtl == ctx->hSpaceLabel) {
                SetTextColor(hdcStatic, RGB(0x8E, 0x8E, 0x98));
            } else if (hCtl == ctx->hFileSizeLabel) {
                SetTextColor(hdcStatic, RGB(0x38, 0xBD, 0xF8));
            } else {
                SetTextColor(hdcStatic, RGB(0xA4, 0xA5, 0xAC));
            }
            SetBkColor(hdcStatic, RGB(0x16, 0x16, 0x18));
            return reinterpret_cast<LRESULT>(ctx->hBgBrush);
        }
        case WM_CTLCOLORBTN: {
            return reinterpret_cast<LRESULT>(ctx->hBgBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, ctx->hBgBrush);

            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;
            const auto& pal = ctx->view->GetPalette();

            int pad_x = DesignSystem::ScaleForDpi(24, dpi);

            // Refined Header: Title & Subtitle
            SelectObject(hdc, ctx->hFontTitle);
            SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
            SetBkMode(hdc, TRANSPARENT);
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kHeaderTitleTop, dpi), L"New Download", 12);

            SelectObject(hdc, ctx->hFontSmall);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kHeaderSubtitleTop, dpi),
                     L"Configure download source, file name and destination folder", 59);

            // Subtle divider line
            RECT div_rc{
                pad_x,
                DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kDividerTop, dpi),
                rc.right - pad_x,
                DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kDividerTop + 1, dpi)
            };
            FillRectSolid(hdc, div_rc, pal.border_subtle);
            RECT accent_div{div_rc.left, div_rc.top, div_rc.left + DesignSystem::ScaleForDpi(44, dpi), div_rc.bottom};
            FillRectSolid(hdc, accent_div, pal.accent_primary);

            // Field labels: separate non-overlapping rows above each control
            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(0), dpi), L"Download URL:", 13);
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(1), dpi), L"File Name:", 10);
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(2), dpi), L"Save Location:", 14);
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(3), dpi), L"Quality / Format:", 17);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == 5001 && code == EN_CHANGE) {
                wchar_t url_buf[1024]{};
                GetWindowTextW(ctx->hUrlEdit, url_buf, 1023);
                std::string u = WToUtf8(url_buf);
                while (!u.empty() && (u.front() == ' ' || u.front() == '\t')) u.erase(0, 1);
                while (!u.empty() && (u.back() == ' ' || u.back() == '\t')) u.pop_back();

                wchar_t fn_buf[512]{};
                GetWindowTextW(ctx->hFilenameEdit, fn_buf, 511);
                std::string cur_fn = WToUtf8(fn_buf);

                if (cur_fn.empty() || cur_fn == "youtube_video.mp4" || cur_fn == "download") {
                    if (u.find("youtube.com") != std::string::npos || u.find("youtu.be") != std::string::npos) {
                        SetWindowTextW(ctx->hFilenameEdit, L"youtube_video.mp4");
                    } else if (!u.empty()) {
                        auto derived = PathSanitizer::ExtractFromUrl(u);
                        if (!derived.empty()) {
                            SetWindowTextW(ctx->hFilenameEdit, Utf8ToW(derived).c_str());
                        }
                    }
                }

                bool is_stream_url = (u.find("youtube.com") != std::string::npos ||
                                      u.find("youtu.be") != std::string::npos ||
                                      u.find(".m3u8") != std::string::npos ||
                                      u.find(".mpd") != std::string::npos);
                int count = static_cast<int>(SendMessageW(ctx->hQualityCombo, CB_GETCOUNT, 0, 0));
                bool has_stream_items = (count > 1);
                if (is_stream_url && !has_stream_items) {
                    SendMessageW(ctx->hQualityCombo, CB_RESETCONTENT, 0, 0);
                    ctx->quality_choices.clear();
                    ctx->quality_choices.push_back({"best", "Best Available Quality (Auto)", "Auto", 0, -1, true, "best", "best", "", "", false});
                    ctx->quality_choices.push_back({"1080p", "1080p Full HD", "1080p", 1080, -1, false, "1080p", "best", "", "", false});
                    ctx->quality_choices.push_back({"720p", "720p HD", "720p", 720, -1, false, "720p", "best", "", "", false});
                    ctx->quality_choices.push_back({"480p", "480p SD", "480p", 480, -1, false, "480p", "best", "", "", false});
                    ctx->quality_choices.push_back({"360p", "360p Standard", "360p", 360, -1, false, "360p", "best", "", "", false});
                    ctx->quality_choices.push_back({"144p", "144p Compact", "144p", 144, -1, false, "144p", "best", "", "", false});
                    ctx->quality_choices.push_back({"audio", "Audio Only (M4A)", "Audio", 0, -1, false, "audio", "best", "", "", true});
                    for (const auto& qc : ctx->quality_choices) {
                        SendMessageW(ctx->hQualityCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Utf8ToW(qc.label).c_str()));
                    }
                    SendMessageW(ctx->hQualityCombo, CB_SETCURSEL, 0, 0);
                } else if (!is_stream_url && has_stream_items) {
                    SendMessageW(ctx->hQualityCombo, CB_RESETCONTENT, 0, 0);
                    ctx->quality_choices.clear();
                    ctx->quality_choices.push_back({"original", "Original / Direct File (Server Default)", "", 0, -1, false, "best", "best", "", "", false, ""});
                    SendMessageW(ctx->hQualityCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Original / Direct File (Server Default)"));
                    SendMessageW(ctx->hQualityCombo, CB_SETCURSEL, 0, 0);
                }

                // Check size probing for local torrent or remote HTTP/HTTPS resource
                std::error_code tor_ec;
                if (u.rfind(".torrent") != std::string::npos && std::filesystem::exists(u, tor_ec)) {
                    auto parsed = torrent::TorrentParser::ParseTorrentFile(u);
                    if (parsed.ok()) {
                        uint64_t sel_b = parsed.value->selected_bytes();
                        uint64_t tot_b = parsed.value->total_length;
                        std::string sz_str = DesktopView::FormatSizePretty(sel_b > 0 ? sel_b : tot_b);
                        std::wstring txt = L"Download Size: " + Utf8ToW(sz_str);
                        SetWindowTextW(ctx->hFileSizeLabel, txt.c_str());
                    } else {
                        SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Unknown size");
                    }
                } else if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) {
                    SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Checking size…");
                    uint64_t req_id = ++ctx->current_probe_id;
                    auto alive_flag = ctx->dlg_alive;
                    HWND target_hwnd = hwnd;
                    std::thread([target_hwnd, alive_flag, req_id, u]() {
                        auto probe_res = openidm::brisk::HttpClient::ProbeResource(u, {}, {}, {}, false, 5000);
                        if (!alive_flag || !(*alive_flag) || !IsWindow(target_hwnd)) return;
                        auto* payload = new ProbeResultPayload();
                        payload->request_id = req_id;
                        if (probe_res.ok()) {
                            payload->success = true;
                            payload->content_length = probe_res.value->has_content_length ? static_cast<int64_t>(probe_res.value->content_length) : -1;
                            std::string fn = PathSanitizer::ExtractFromContentDisposition(probe_res.value->content_disposition);
                            if (fn.empty()) {
                                fn = PathSanitizer::ExtractFromUrl(probe_res.value->final_url.empty() ? u : probe_res.value->final_url);
                            }
                            payload->final_filename = PathSanitizer::SanitizeFileName(fn);
                        } else {
                            payload->success = false;
                            payload->content_length = -1;
                        }
                        PostMessageW(target_hwnd, WM_OPENIDM_PROBE_COMPLETE, 0, reinterpret_cast<LPARAM>(payload));
                    }).detach();
                } else if (u.empty()) {
                    SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Unknown size");
                }
                return 0;
            }

            if (id == 5005 && code == CBN_SELCHANGE) {
                int cur = static_cast<int>(SendMessageW(ctx->hQualityCombo, CB_GETCURSEL, 0, 0));
                if (cur >= 0 && static_cast<size_t>(cur) < ctx->quality_choices.size()) {
                    const auto& qc = ctx->quality_choices[static_cast<size_t>(cur)];
                    if (qc.size > 0) {
                        std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(qc.size));
                        std::wstring txt = std::wstring(L"Size: ") + (qc.size_estimated ? L"~" : L"") + Utf8ToW(sz_str) + (qc.size_estimated ? L" (estimated)" : L"");
                        SetWindowTextW(ctx->hFileSizeLabel, txt.c_str());
                    } else {
                        SetWindowTextW(ctx->hFileSizeLabel, L"Size: Unknown");
                    }
                }
                return 0;
            }

            // Live destination disk space checking on edit change or blur
            if (id == 5003 && (code == EN_CHANGE || code == EN_KILLFOCUS)) {
                wchar_t dir_buf[MAX_PATH]{};
                GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                std::string chosen_dir = WToUtf8(dir_buf);
                if (!chosen_dir.empty()) {
                    auto space_res = PathSanitizer::CheckAvailableDiskSpace(chosen_dir, 1024);
                    if (space_res.ok() && space_res.value.has_value()) {
                        std::string space_text = "Free space: " + DesktopView::FormatSizePretty(*space_res.value) + " available on destination drive";
                        SetWindowTextW(ctx->hSpaceLabel, Utf8ToW(space_text).c_str());
                    } else {
                        SetWindowTextW(ctx->hSpaceLabel, L"Disk space: Checking...");
                    }
                }
                return 0;
            }

            if (id == 5004 && code == BN_CLICKED) { // Browse
                wchar_t dir_buf[MAX_PATH]{};
                GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                std::string chosen_dir = WToUtf8(dir_buf);
                if (SelectFolderWithDialogWin32(hwnd, chosen_dir)) {
                    SetWindowTextW(ctx->hSaveDirEdit, Utf8ToW(chosen_dir).c_str());
                    auto space_res = PathSanitizer::CheckAvailableDiskSpace(chosen_dir, 1024);
                    if (space_res.ok() && space_res.value.has_value()) {
                        std::string space_text = "Free space: " + DesktopView::FormatSizePretty(*space_res.value) + " available on destination drive";
                        SetWindowTextW(ctx->hSpaceLabel, Utf8ToW(space_text).c_str());
                    }
                }
                return 0;
            }

            if (id == 5007 && code == BN_CLICKED) { // Start immediately toggled
                bool start_chk = (SendMessageW(ctx->hStartChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (start_chk) {
                    SendMessageW(ctx->hScheduleChk, BM_SETCHECK, BST_UNCHECKED, 0);
                    EnableWindow(ctx->hScheduleTimeEdit, FALSE);
                }
                return 0;
            }

            if (id == 5010 && code == BN_CLICKED) { // Schedule toggled
                bool sched = (SendMessageW(ctx->hScheduleChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                EnableWindow(ctx->hScheduleTimeEdit, sched ? TRUE : FALSE);
                if (sched) {
                    SendMessageW(ctx->hStartChk, BM_SETCHECK, BST_UNCHECKED, 0);
                }
                return 0;
            }

            if ((id == 5008 || id == IDOK) && (code == BN_CLICKED || code == 0 || code == 1)) { // Download
                if (ctx->is_submitting) {
                    return 0;
                }
                ctx->is_submitting = true;

                wchar_t url_buf[2048]{};
                GetWindowTextW(ctx->hUrlEdit, url_buf, 2047);
                std::string u = WToUtf8(url_buf);
                while (!u.empty() && (u.front() == ' ' || u.front() == '\t')) u.erase(0, 1);
                while (!u.empty() && (u.back() == ' ' || u.back() == '\t')) u.pop_back();

                if (u.empty()) {
                    ctx->is_submitting = false;
                    MessageBoxW(hwnd, L"Please enter a valid download URL.", L"Missing URL", MB_OK | MB_ICONWARNING);
                    SetFocus(ctx->hUrlEdit);
                    return 0;
                }

                // If entered URL is a local .torrent file, close New Download dialog and open dedicated Add Torrent dialog
                std::error_code tor_ec;
                if (u.rfind(".torrent") != std::string::npos && std::filesystem::exists(u, tor_ec)) {
                    auto parsed = torrent::TorrentParser::ParseTorrentFile(u);
                    if (parsed.ok()) {
                        auto tinfo = std::move(*parsed.value);
                        HWND parent_wnd = GetWindow(hwnd, GW_OWNER);
                        if (!parent_wnd) parent_wnd = hwnd;
                        std::string target_save = ctx->controller->GetSettings().default_download_dir;
                        bool start_torrent = true;
                        DestroyWindow(hwnd);
                        if (ShowAddTorrentDialogWin32(parent_wnd, *ctx->view, *ctx->controller, tinfo, u, target_save,
                                                      start_torrent, &target_save, &start_torrent)) {
                            TaskModel t;
                            t.task_type = TaskType::kTorrent;
                            t.url = u;
                            t.file_name = tinfo.name.empty() ? std::filesystem::path(u).filename().string() : tinfo.name;
                            t.save_dir = target_save;
                            t.total_bytes = tinfo.selected_bytes();
                            for (const auto& file : tinfo.files) {
                                t.torrent_file_paths.push_back(file.relative_path.generic_string());
                                t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                            }
                            auto res = ctx->controller->AddTask(std::move(t), start_torrent, false);
                            if (res.ok()) {
                                ctx->view->SelectTask(res.value->task_id);
                                ctx->view->ScrollTaskIntoView(res.value->task_id, true);
                            }
                        }
                        return 0;
                    }
                }

                wchar_t fn_buf[512]{};
                GetWindowTextW(ctx->hFilenameEdit, fn_buf, 511);
                std::string fn = WToUtf8(fn_buf);

                wchar_t dir_buf[MAX_PATH]{};
                GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                std::string sd = WToUtf8(dir_buf);
                if (sd.empty()) sd = ctx->controller->GetSettings().default_download_dir;

                int cur_sel = static_cast<int>(SendMessageW(ctx->hQualityCombo, CB_GETCURSEL, 0, 0));
                std::string vfilter = "best";
                std::string afilter = "best";
                int64_t chosen_sz = -1;
                if (cur_sel >= 0 && static_cast<size_t>(cur_sel) < ctx->quality_choices.size()) {
                    const auto& qc = ctx->quality_choices[static_cast<size_t>(cur_sel)];
                    vfilter = qc.video_filter.empty() ? (!qc.id.empty() ? qc.id : "best") : qc.video_filter;
                    afilter = qc.audio_filter.empty() ? "best" : qc.audio_filter;
                    chosen_sz = qc.size;
                } else if (cur_sel >= 0) {
                    wchar_t sel_buf[128]{};
                    SendMessageW(ctx->hQualityCombo, CB_GETLBTEXT, cur_sel, reinterpret_cast<LPARAM>(sel_buf));
                    std::wstring wsel(sel_buf);
                    vfilter = (wsel.find(L"Audio Only") != std::wstring::npos) ? "audio" : "best";
                }

                bool start_now = (SendMessageW(ctx->hStartChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool sched = (SendMessageW(ctx->hScheduleChk, BM_GETCHECK, 0, 0) == BST_CHECKED);

                AddDownloadDialogInput in;
                in.url = u;
                in.custom_filename = fn;
                in.save_dir = sd;
                in.max_connections = ctx->controller->GetSettings().default_connections_per_task;
                in.start_immediately = start_now;
                in.scheduled = sched;
                if (sched) {
                    wchar_t tbuf[64]{};
                    GetWindowTextW(ctx->hScheduleTimeEdit, tbuf, 63);
                    in.schedule_time_local = WToUtf8(tbuf);
                }
                in.video_filter = vfilter;
                in.audio_filter = afilter;
                if (chosen_sz > 0) {
                    in.total_bytes = static_cast<uint64_t>(chosen_sz);
                }
                bool is_yt = (u.find("youtube.com") != std::string::npos || u.find("youtu.be") != std::string::npos);
                if (is_yt || u.find(".m3u8") != std::string::npos || u.find(".mpd") != std::string::npos) {
                    in.is_stream = true;
                }

                OPENIDM_LOG_INFO("UI", "NEW_DOWNLOAD_SUBMIT_ATTEMPT: url=" + u + " filename=" + fn + " quality=" + vfilter);

                if (!ctx->existing_task_id.empty()) {
                    auto t_opt = ctx->controller->GetTask(ctx->existing_task_id);
                    if (t_opt.has_value()) {
                        auto t = *t_opt;
                        bool is_video_stream = (t.task_type == TaskType::kStreamHls || t.task_type == TaskType::kStreamDash || t.category == "Video" || is_yt);
                        std::string old_fn = t.file_name;
                        std::string old_sd = t.save_dir;
                        if (!fn.empty()) t.file_name = fn;
                        if (!sd.empty()) t.save_dir = sd;
                        t.scheduled = sched;
                        if (sched) {
                            wchar_t tbuf[64]{};
                            GetWindowTextW(ctx->hScheduleTimeEdit, tbuf, 63);
                            t.schedule_time_local = WToUtf8(tbuf);
                        }
                        t.stream_options.video_filter = vfilter;
                        t.stream_options.audio_filter = afilter;
                        if (chosen_sz > 0) {
                            t.total_bytes = static_cast<uint64_t>(chosen_sz);
                        }

                        if (cur_sel >= 0 && static_cast<size_t>(cur_sel) < ctx->quality_choices.size()) {
                            const auto& qc = ctx->quality_choices[static_cast<size_t>(cur_sel)];
                            if (qc.is_audio_only) {
                                t.stream_options.output_format = "m4a";
                                t.category = "Audio";
                            } else if (!qc.ext.empty() && is_video_stream) {
                                t.stream_options.output_format = qc.ext;
                            }
                            // Clean up file name if it has an outdated resolution tag (e.g. [360p])
                            // ONLY applies to video streams with a resolution tag! Never to direct files or "Original"!
                            if (is_video_stream && !qc.resolution.empty() && qc.resolution != "Auto" && qc.resolution != "Original") {
                                static const std::regex kResTagRe(R"(\s*\[\s*\d{3,4}p[_\w]*\s*\])", std::regex::icase);
                                std::string base_fn = std::regex_replace(t.file_name, kResTagRe, "");
                                auto dot_pos = base_fn.rfind('.');
                                std::string stem = (dot_pos != std::string::npos) ? base_fn.substr(0, dot_pos) : base_fn;
                                std::string ext = (dot_pos != std::string::npos) ? base_fn.substr(dot_pos + 1) : t.stream_options.output_format;
                                if (!qc.is_audio_only && !qc.ext.empty()) ext = qc.ext;
                                t.file_name = stem + " [" + qc.resolution + "]." + ext;
                            }
                            if (!t.stream_options.cached_manifest_body.empty() &&
                                (t.stream_options.cached_manifest_body.front() == '{' || t.stream_options.cached_manifest_body.front() == '[')) {
                                try {
                                    auto jroot = openidm::json::Parse(t.stream_options.cached_manifest_body);
                                    if (jroot.is_object()) {
                                        if (!qc.video_url.empty()) jroot["videoUrl"] = qc.video_url;
                                        if (!qc.paired_audio_url.empty()) jroot["audioUrl"] = qc.paired_audio_url;
                                        jroot["requiresCipherExtraction"] = qc.video_url.empty();
                                        jroot["requiresAudioMux"] = !qc.paired_audio_url.empty();
                                        if (!qc.resolution.empty()) jroot["resolution"] = qc.resolution;
                                        if (!qc.video_filter.empty()) jroot["videoFilter"] = qc.video_filter;
                                        if (qc.itag > 0) jroot["selectedItag"] = qc.itag;
                                        t.stream_options.cached_manifest_body = jroot.dump();
                                    }
                                } catch (...) {}
                            }
                        }

                        // If task was already completed (e.g. chunked transfer ingestion), ensure file on disk matches any rename
                        if (t.state == TaskState::kCompleted) {
                            std::filesystem::path old_p = std::filesystem::path(old_sd) / old_fn;
                            std::filesystem::path new_p = std::filesystem::path(t.save_dir) / t.file_name;
                            if (old_p != new_p) {
                                std::error_code ren_ec;
                                if (std::filesystem::exists(old_p, ren_ec)) {
                                    std::filesystem::create_directories(new_p.parent_path(), ren_ec);
                                    std::filesystem::rename(old_p, new_p, ren_ec);
                                    if (ren_ec) {
                                        ren_ec.clear();
                                        std::filesystem::copy_file(old_p, new_p, std::filesystem::copy_options::overwrite_existing, ren_ec);
                                        std::filesystem::remove(old_p, ren_ec);
                                    }
                                }
                            }
                        }

                        if (t.state == TaskState::kFailed || t.state == TaskState::kPaused || t.state == TaskState::kQueued) {
                            t.state = TaskState::kQueued;
                            t.status_detail = "Queued";
                            t.error_code.clear();
                            t.error_message.clear();
                        }

                        ctx->controller->UpdateTask(t);
                        if (start_now && t.state != TaskState::kCompleted) {
                            ctx->controller->StartTask(ctx->existing_task_id);
                        }
                        ctx->submitted = true;
                        OPENIDM_LOG_INFO("UI", "NEW_DOWNLOAD_UPDATED: taskId=" + ctx->existing_task_id);
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }

                auto res = ctx->view->SubmitAddDownloadDialog(in);
                if (!res.ok()) {
                    ctx->is_submitting = false;
                    std::wstring err = L"Could not add download:\n" + Utf8ToW(res.error->message);
                    MessageBoxW(hwnd, err.c_str(), L"Error Adding Download", MB_OK | MB_ICONERROR);
                    return 0;
                }

                ctx->submitted = true;
                OPENIDM_LOG_INFO("UI", "NEW_DOWNLOAD_SUBMITTED: taskId=" + res.value->task_id + " file=" + res.value->file_name);
                DestroyWindow(hwnd);
                return 0;
            }

            if ((id == 5009 || id == IDCANCEL) && (code == BN_CLICKED || code == 0 || code == 1)) { // Cancel
                if (!ctx->existing_task_id.empty()) {
                    ctx->controller->CancelTask(ctx->existing_task_id);
                    ctx->controller->RemoveTask(ctx->existing_task_id, false);
                }
                DestroyWindow(hwnd);
                return 0;
            }

            return 0;
        }
        case WM_DESTROY: {
            if (ctx && ctx->dlg_alive) {
                *(ctx->dlg_alive) = false;
            }
            if (!ctx->submitted && !ctx->existing_task_id.empty()) {
                ctx->controller->CancelTask(ctx->existing_task_id);
                ctx->controller->RemoveTask(ctx->existing_task_id, false);
            }
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
            if (ctx->hFontSmall) DeleteObject(ctx->hFontSmall);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            if (ctx->hEditBrush) DeleteObject(ctx->hEditBrush);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowNewDownloadDialogWin32(
    HWND hwnd_parent,
    DesktopView& view,
    DownloadController& controller,
    const std::string& initial_url,
    const std::string& suggested_filename = "",
    const std::string& save_dir = "",
    int64_t file_size = -1,
    const std::string& existing_task_id = "") {

    // Avoid duplicate windows and duplicate downloads: if a dialog is already open, bring it to front
    HWND existing_dlg = FindWindowW(L"Radium_NewDownloadDlgClass", nullptr);
    if (existing_dlg && IsWindow(existing_dlg)) {
        if (!IsWindowVisible(existing_dlg) || IsIconic(existing_dlg)) {
            ShowWindow(existing_dlg, SW_RESTORE);
        }
        BringWindowToTop(existing_dlg);
        SetForegroundWindow(existing_dlg);
        SetActiveWindow(existing_dlg);
        return;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = NewDownloadDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hAppIcon = CreateRadiumAppIcon();
        wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"Radium_NewDownloadDlgClass";
        RegisterClassExW(&wc);
        class_registered = true;
    }

    UINT dpi = 96;
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            using fnGetDpiForWindow = UINT(WINAPI*)(HWND);
            auto pfnGetDpi = reinterpret_cast<fnGetDpiForWindow>(GetProcAddress(hUser32, "GetDpiForWindow"));
            if (pfnGetDpi) {
                dpi = pfnGetDpi(hwnd_parent);
            }
        }
    }
    if (dpi == 0) dpi = 96;

    NewDownloadDialogContext ctx;
    ctx.view = &view;
    ctx.controller = &controller;
    ctx.existing_task_id = existing_task_id;
    ctx.initial_filename = suggested_filename;
    ctx.initial_save_dir = save_dir;
    ctx.initial_file_size = file_size;
    ctx.dpi = dpi;

    std::string candidate_url = initial_url;
    if (candidate_url.empty() && OpenClipboard(hwnd_parent)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            const wchar_t* wtxt = static_cast<const wchar_t*>(GlobalLock(hData));
            if (wtxt) {
                std::string s = WToUtf8(wtxt);
                if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 || s.rfind("ftp://", 0) == 0 ||
                    s.rfind("magnet:?", 0) == 0 || s.rfind(".torrent") != std::string::npos) {
                    candidate_url = s;
                }
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
    }

    // Direct routing: If URL or clipboard points to a local .torrent file, open dedicated Add Torrent dialog directly
    if (!candidate_url.empty()) {
        std::error_code tor_ec;
        if (candidate_url.rfind(".torrent") != std::string::npos && std::filesystem::exists(candidate_url, tor_ec)) {
            auto parsed = torrent::TorrentParser::ParseTorrentFile(candidate_url);
            if (parsed.ok()) {
                auto tinfo = std::move(*parsed.value);
                std::string target_save = save_dir.empty() ? controller.GetSettings().default_download_dir : save_dir;
                bool start_torrent = true;
                if (ShowAddTorrentDialogWin32(hwnd_parent, view, controller, tinfo, candidate_url, target_save,
                                              start_torrent, &target_save, &start_torrent)) {
                    TaskModel t;
                    t.task_type = TaskType::kTorrent;
                    t.url = candidate_url;
                    t.file_name = tinfo.name.empty() ? std::filesystem::path(candidate_url).filename().string() : tinfo.name;
                    t.save_dir = target_save;
                    t.total_bytes = tinfo.selected_bytes();
                    for (const auto& file : tinfo.files) {
                        t.torrent_file_paths.push_back(file.relative_path.generic_string());
                        t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                    }
                    auto res = controller.AddTask(std::move(t), start_torrent, false);
                    if (res.ok()) {
                        view.SelectTask(res.value->task_id);
                        view.ScrollTaskIntoView(res.value->task_id, true);
                    }
                }
                return;
            }
        }
    }

    RECT work_rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rc, 0);
    int screen_w = work_rc.right - work_rc.left;
    int screen_h = work_rc.bottom - work_rc.top;
    if (screen_w <= 0) screen_w = GetSystemMetrics(SM_CXSCREEN);
    if (screen_h <= 0) screen_h = GetSystemMetrics(SM_CYSCREEN);

    RECT parent_rc{};
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        GetWindowRect(hwnd_parent, &parent_rc);
    }
    int dlg_w = DesignSystem::ScaleForDpi(520, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(480, dpi);
    int dlg_x = 0;
    int dlg_y = 0;

    bool parent_is_interactive = (hwnd_parent && IsWindow(hwnd_parent) && IsWindowVisible(hwnd_parent) && !IsIconic(hwnd_parent));
    HWND owner_hwnd = parent_is_interactive ? hwnd_parent : nullptr;

    if (!parent_is_interactive || parent_rc.left < -10000 || parent_rc.top < -10000 ||
        (parent_rc.right - parent_rc.left) < 200 || (parent_rc.bottom - parent_rc.top) < 200) {
        dlg_x = work_rc.left + (screen_w - dlg_w) / 2;
        dlg_y = work_rc.top + (screen_h - dlg_h) / 2;
    } else {
        dlg_x = parent_rc.left + (parent_rc.right - parent_rc.left - dlg_w) / 2;
        dlg_y = parent_rc.top + (parent_rc.bottom - parent_rc.top - dlg_h) / 2;
    }
    if (dlg_x < work_rc.left) dlg_x = work_rc.left + 20;
    if (dlg_y < work_rc.top) dlg_y = work_rc.top + 20;

    // Clean modern dialog frame without redundant 3D modal borders or nested outlines
    DWORD ex_style = (owner_hwnd ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT;
    HWND hDlg = CreateWindowExW(
        ex_style,
        L"Radium_NewDownloadDlgClass",
        L"New Download \u2014 Radium",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        owner_hwnd, nullptr, hInst, &ctx);

    if (!hDlg) return;
    ctx.hDlg = hDlg;

    HICON hDlgIcon = CreateRadiumAppIcon();
    if (hDlgIcon) {
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hDlgIcon));
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hDlgIcon));
    }

    ApplyWindowGlassBackdropWin32(hDlg, true /* is_transient_dialog */);

    if (!candidate_url.empty()) {
        SetWindowTextW(ctx.hUrlEdit, Utf8ToW(candidate_url).c_str());
        SendMessageW(ctx.hUrlEdit, EM_SETSEL, 0, -1);
        if (candidate_url.find("youtube.com") != std::string::npos || candidate_url.find("youtu.be") != std::string::npos) {
            SetWindowTextW(ctx.hFilenameEdit, L"youtube_video.mp4");
        } else {
            auto derived = PathSanitizer::ExtractFromUrl(candidate_url);
            if (!derived.empty()) {
                SetWindowTextW(ctx.hFilenameEdit, Utf8ToW(derived).c_str());
            }
        }
    }
    if (!suggested_filename.empty()) {
        SetWindowTextW(ctx.hFilenameEdit, Utf8ToW(suggested_filename).c_str());
    }
    if (!save_dir.empty()) {
        SetWindowTextW(ctx.hSaveDirEdit, Utf8ToW(save_dir).c_str());
    }
    if (file_size > 0) {
        std::wstring s_sz = L"Download Size: " + Utf8ToW(DesktopView::FormatSizePretty(static_cast<uint64_t>(file_size)));
        SetWindowTextW(ctx.hFileSizeLabel, s_sz.c_str());
    } else if (!candidate_url.empty()) {
        SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(5001, EN_CHANGE), reinterpret_cast<LPARAM>(ctx.hUrlEdit));
    }

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        EnableWindow(owner_hwnd, FALSE);
    }
    SetWindowPos(hDlg, HWND_TOP, dlg_x, dlg_y, dlg_w, dlg_h, SWP_SHOWWINDOW);
    ShowWindow(hDlg, SW_SHOWNORMAL);
    BringWindowToTop(hDlg);
    SetForegroundWindow(hDlg);
    SetActiveWindow(hDlg);
    SetFocus(ctx.hUrlEdit);
    OPENIDM_LOG_INFO("UI", "NEW_DOWNLOAD_DIALOG_SHOWN: taskId=" + existing_task_id + " url=" + candidate_url + " filename=" + suggested_filename);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(5009, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hCancelBtn));
                break;
            } else if (msg.wParam == VK_RETURN && GetFocus() != ctx.hBrowseBtn && GetFocus() != ctx.hCancelBtn) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(5008, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hDownloadBtn));
                if (!IsWindow(hDlg)) break;
                continue;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        EnableWindow(owner_hwnd, TRUE);
        SetForegroundWindow(owner_hwnd);
        InvalidateRect(owner_hwnd, nullptr, TRUE);
    }
}

void ShowAddUrlQuickDialogWin32(HWND hwnd, DesktopView& view, DownloadController& controller) {
    ShowNewDownloadDialogWin32(hwnd, view, controller, "");
}

void CopyTextToClipboardWin32(HWND hwnd, const std::string& text) {
    if (text.empty()) return;
    std::wstring wtext = Utf8ToW(text);
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (wtext.size() + 1) * sizeof(wchar_t);
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hGlob) {
        void* p = GlobalLock(hGlob);
        if (p) {
            std::memcpy(p, wtext.c_str(), bytes);
            GlobalUnlock(hGlob);
            SetClipboardData(CF_UNICODETEXT, hGlob);
        }
    }
    CloseClipboard();
}

struct SimpleInputDialogContext {
    std::wstring title;
    std::wstring prompt;
    std::wstring initial_value;
    std::wstring result_value;
    bool submitted{false};
    HWND hEdit{nullptr};
    HWND hOkBtn{nullptr};
    HWND hCancelBtn{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HBRUSH hBgBrush{nullptr};
    HBRUSH hEditBrush{nullptr};
};

LRESULT CALLBACK SimpleInputDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<SimpleInputDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<SimpleInputDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            uint32_t dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;

            ctx->hFontBody = CreateFontW(-DesignSystem::ScaleForDpi(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBold = CreateFontW(-DesignSystem::ScaleForDpi(15, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hBgBrush = CreateSolidBrush(RGB(0x18, 0x18, 0x1E));
            ctx->hEditBrush = CreateSolidBrush(RGB(0x20, 0x20, 0x28));

            HINSTANCE hInst = GetModuleHandleW(nullptr);
            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int edit_y = DesignSystem::ScaleForDpi(60, dpi);
            int edit_h = DesignSystem::ScaleForDpi(28, dpi);
            int edit_w = DesignSystem::ScaleForDpi(432, dpi);

            ctx->hEdit = CreateWindowExW(0, L"EDIT", ctx->initial_value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad_x, edit_y, edit_w, edit_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7001)), hInst, nullptr);
            SendMessageW(ctx->hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hEdit, EM_SETSEL, 0, -1);
            InstallModernEditControl(ctx->hEdit, dpi);

            int btn_y = DesignSystem::ScaleForDpi(108, dpi);
            int btn_w = DesignSystem::ScaleForDpi(100, dpi);
            int btn_h = DesignSystem::ScaleForDpi(32, dpi);
            int cancel_x = pad_x + edit_w - btn_w;
            int ok_x = cancel_x - btn_w - DesignSystem::ScaleForDpi(12, dpi);

            ctx->hOkBtn = CreateWindowExW(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                ok_x, btn_y, btn_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7002)), hInst, nullptr);

            ctx->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                cancel_x, btn_y, btn_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7003)), hInst, nullptr);
            return 0;
        }
        case WM_SIZE: {
            if (!ctx || !ctx->hEdit) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int client_w = rc.right - rc.left;
            int client_h = rc.bottom - rc.top;
            uint32_t dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;

            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int edit_y = DesignSystem::ScaleForDpi(60, dpi);
            int edit_h = DesignSystem::ScaleForDpi(28, dpi);
            int edit_w = std::max(DesignSystem::ScaleForDpi(200, dpi), client_w - pad_x * 2);
            SetWindowPos(ctx->hEdit, nullptr, pad_x, edit_y, edit_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);

            int btn_h = DesignSystem::ScaleForDpi(32, dpi);
            int btn_w = DesignSystem::ScaleForDpi(100, dpi);
            int btn_y = client_h - btn_h - DesignSystem::ScaleForDpi(18, dpi);
            int cancel_x = pad_x + edit_w - btn_w;
            int ok_x = cancel_x - btn_w - DesignSystem::ScaleForDpi(12, dpi);
            SetWindowPos(ctx->hCancelBtn, nullptr, cancel_x, btn_y, btn_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hOkBtn, nullptr, ok_x, btn_y, btn_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            bool is_sel = (dis->itemState & ODS_SELECTED) != 0;
            if (dis->CtlID == 7002) { // OK button (Coral Red accent #FF4F57)
                ColorRgba bg = is_sel ? ColorRgba{0xE0, 0x43, 0x4B, 255} : ColorRgba{0xFF, 0x4F, 0x57, 255};
                DrawRoundedBoxGdi(hdc, rc, 16, bg, bg, 0);
                SelectObject(hdc, ctx->hFontBold);
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"OK", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 7003) { // Cancel button
                ColorRgba bg = is_sel ? ColorRgba{0x28, 0x28, 0x32, 255} : ColorRgba{0x20, 0x20, 0x28, 255};
                ColorRgba border{0x44, 0x44, 0x50, 255};
                DrawRoundedBoxGdi(hdc, rc, 16, bg, border, 1);
                SelectObject(hdc, ctx->hFontBody);
                SetTextColor(hdc, RGB(0xDE, 0xDF, 0xE4));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Cancel", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdcEdit, RGB(0xF2, 0xF2, 0xF3));
            SetBkColor(hdcEdit, RGB(0x20, 0x20, 0x28));
            return reinterpret_cast<LRESULT>(ctx->hEditBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, ctx->hBgBrush);

            uint32_t dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;

            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int prompt_y = DesignSystem::ScaleForDpi(20, dpi);
            int prompt_h = DesignSystem::ScaleForDpi(32, dpi);

            SetBkMode(hdc, TRANSPARENT);
            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, RGB(0xF2, 0xF2, 0xF3));
            RECT prc{pad_x, prompt_y, rc.right - pad_x, prompt_y + prompt_h};
            DrawTextW(hdc, ctx->prompt.c_str(), -1, &prc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 7002) { // OK
                wchar_t buf[2048]{};
                GetWindowTextW(ctx->hEdit, buf, 2047);
                ctx->result_value = buf;
                ctx->submitted = true;
                DestroyWindow(hwnd);
                return 0;
            } else if (id == 7003) { // Cancel
                ctx->submitted = false;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            if (ctx->hEditBrush) DeleteObject(ctx->hEditBrush);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowSimpleInputDialogWin32(HWND hwnd_parent, const std::wstring& title, const std::wstring& prompt, const std::wstring& initial_value, std::wstring& out_value) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SimpleInputDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hAppIcon = CreateRadiumAppIcon();
        wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"Radium_SimpleInputDialogClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    SimpleInputDialogContext ctx;
    ctx.title = title;
    ctx.prompt = prompt;
    ctx.initial_value = initial_value;

    UINT dpi = 96;
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        dpi = GetDpiForWindow(hwnd_parent);
    }
    if (dpi == 0) dpi = 96;

    RECT prc{};
    GetWindowRect(hwnd_parent, &prc);
    int dlg_w = DesignSystem::ScaleForDpi(480, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(190, dpi);
    int dlg_x = prc.left + (prc.right - prc.left - dlg_w) / 2;
    int dlg_y = prc.top + (prc.bottom - prc.top - dlg_h) / 2;

    HWND hDlg = CreateWindowExW(
        (hwnd_parent ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT,
        L"Radium_SimpleInputDialogClass",
        title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        hwnd_parent, nullptr, hInst, &ctx);

    if (!hDlg) return false;

    HICON hDlgIcon = CreateRadiumAppIcon();
    if (hDlgIcon) {
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hDlgIcon));
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hDlgIcon));
    }

    BOOL use_dark = TRUE;
    DwmSetWindowAttribute(hDlg, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));

    EnableWindow(hwnd_parent, FALSE);
    SetForegroundWindow(hDlg);
    SetFocus(ctx.hEdit);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                DestroyWindow(hDlg);
                break;
            } else if (msg.wParam == VK_RETURN) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(7002, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hOkBtn));
                if (!IsWindow(hDlg)) break;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    EnableWindow(hwnd_parent, TRUE);
    SetForegroundWindow(hwnd_parent);
    if (ctx.submitted) {
        out_value = ctx.result_value;
        return true;
    }
    return false;
}

struct DeleteConfirmDialogContext {
    DesktopView* view{nullptr};
    DownloadController* controller{nullptr};
    std::vector<std::string> task_ids;
    std::vector<std::pair<std::string, std::string>> files_info;
    UINT dpi{96};
    HWND hDlg{nullptr};
    HWND owner{nullptr};
    HWND hRemoveBtn{nullptr};
    HWND hPermanentBtn{nullptr};
    HWND hCancelBtn{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontTitle{nullptr};
    HFONT hFontSmall{nullptr};
    HBRUSH hBgBrush{nullptr};
};

LRESULT CALLBACK DeleteConfirmDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<DeleteConfirmDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<DeleteConfirmDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            ctx->hDlg = hwnd;
            ctx->dpi = GetDpiForWindow(hwnd);
            if (ctx->dpi == 0) ctx->dpi = 96;

            ctx->hFontBody = CreateFontW(-DesignSystem::ScaleForDpi(14, ctx->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBold = CreateFontW(-DesignSystem::ScaleForDpi(14, ctx->dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontTitle = CreateFontW(-DesignSystem::ScaleForDpi(18, ctx->dpi), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontSmall = CreateFontW(-DesignSystem::ScaleForDpi(12, ctx->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hBgBrush = CreateSolidBrush(RGB(0x16, 0x16, 0x18));

            HINSTANCE hInst = GetModuleHandleW(nullptr);

            int btn_h = DesignSystem::ScaleForDpi(36, ctx->dpi);
            int pad = DesignSystem::ScaleForDpi(18, ctx->dpi);
            int perm_w = DesignSystem::ScaleForDpi(158, ctx->dpi);
            int remove_w = DesignSystem::ScaleForDpi(138, ctx->dpi);
            int cancel_w = DesignSystem::ScaleForDpi(90, ctx->dpi);
            int spacing = DesignSystem::ScaleForDpi(8, ctx->dpi);

            RECT client_rc{};
            GetClientRect(hwnd, &client_rc);
            int btn_y = (client_rc.bottom > 0) ? (client_rc.bottom - btn_h - pad) : DesignSystem::ScaleForDpi(180, ctx->dpi);
            int buttons_w = perm_w + remove_w + cancel_w + spacing * 2;
            int btn_x = std::max(pad, (static_cast<int>(client_rc.right) - buttons_w) / 2);

            // 1. Permanently Delete Button (Coral Red Accent #FF4F57)
            ctx->hPermanentBtn = CreateWindowExW(0, L"BUTTON", L"Permanently Delete",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                btn_x, btn_y, perm_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(6001)), hInst, nullptr);

            // 2. Remove from List Button (keeps output files)
            ctx->hRemoveBtn = CreateWindowExW(0, L"BUTTON", L"Remove from List",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                btn_x + perm_w + spacing, btn_y, remove_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(6004)), hInst, nullptr);

            // 3. Cancel Button (Safe Default, Initially Focused)
            ctx->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                btn_x + perm_w + spacing + remove_w + spacing, btn_y, cancel_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(6003)), hInst, nullptr);

            return 0;
        }
        case WM_CTLCOLORBTN: {
            return reinterpret_cast<LRESULT>(ctx->hBgBrush);
        }
        case WM_SIZE: {
            if (!ctx || !ctx->hPermanentBtn) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            (void)rc;
            int client_h = rc.bottom - rc.top;
            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;

            int btn_h = DesignSystem::ScaleForDpi(36, dpi);
            int pad = DesignSystem::ScaleForDpi(18, dpi);
            int spacing = DesignSystem::ScaleForDpi(8, dpi);
            int btn_y = client_h - btn_h - pad;

            int perm_w = DesignSystem::ScaleForDpi(158, dpi);
            int remove_w = DesignSystem::ScaleForDpi(138, dpi);
            int cancel_w = DesignSystem::ScaleForDpi(90, dpi);
            int buttons_w = perm_w + remove_w + cancel_w + spacing * 2;
            int btn_x = std::max(pad, (static_cast<int>(rc.right - rc.left) - buttons_w) / 2);
            SetWindowPos(ctx->hPermanentBtn, nullptr, btn_x, btn_y, perm_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hRemoveBtn, nullptr, btn_x + perm_w + spacing, btn_y, remove_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hCancelBtn, nullptr, btn_x + perm_w + spacing + remove_w + spacing, btn_y, cancel_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            bool is_sel = (dis->itemState & ODS_SELECTED) != 0;
            bool is_focused = (dis->itemState & ODS_FOCUS) != 0;
            int r = DesignSystem::ScaleForDpi(6, ctx->dpi);
            FillRect(hdc, &rc, ctx->hBgBrush);

            if (dis->CtlID == 6001) { // Permanently Delete (Coral Red Accent #FF4F57)
                const auto& accent = ctx->view->GetPalette().accent_primary;
                ColorRgba bg = is_sel ? accent.Interpolate(ColorRgba{0, 0, 0, 255}, 0.15f) : (is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.12f) : accent);
                ColorRgba border = is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.32f) : bg;
                DrawRoundedBoxGdi(hdc, rc, r, bg, border, is_focused ? 1 : 0);
                SelectObject(hdc, ctx->hFontBold);
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Permanently Delete", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 6003) { // Cancel
                ColorRgba bg = is_sel ? ColorRgba{0x33, 0x33, 0x38, 255} : (is_focused ? ColorRgba{0x2A, 0x2A, 0x30, 255} : ColorRgba{0x22, 0x22, 0x28, 255});
                ColorRgba border = is_focused ? ColorRgba{0x55, 0x58, 0x66, 220} : ColorRgba{0x38, 0x3A, 0x44, 150};
                DrawRoundedBoxGdi(hdc, rc, r, bg, border, 1);
                SelectObject(hdc, ctx->hFontBody);
                SetTextColor(hdc, RGB(0xD0, 0xD1, 0xD6));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Cancel", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 6004) { // Remove from List
                ColorRgba bg = is_sel ? ColorRgba{0x33, 0x33, 0x38, 255} : (is_focused ? ColorRgba{0x2A, 0x2A, 0x30, 255} : ColorRgba{0x22, 0x22, 0x28, 255});
                ColorRgba border = is_focused ? ColorRgba{0x55, 0x58, 0x66, 220} : ColorRgba{0x3E, 0x40, 0x4A, 180};
                DrawRoundedBoxGdi(hdc, rc, r, bg, border, 1);
                SelectObject(hdc, ctx->hFontBold);
                SetTextColor(hdc, RGB(0xF2, 0xF2, 0xF3));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Remove from List", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, ctx->hBgBrush);

            int pad = DesignSystem::ScaleForDpi(18, ctx->dpi);
            int content_x = pad;
            int content_w = rc.right - rc.left - pad * 2;

            // Title: choose how to remove the selected download(s)
            SetBkMode(hdc, TRANSPARENT);
            SelectObject(hdc, ctx->hFontTitle);
            SetTextColor(hdc, RGB(0xF2, 0xF2, 0xF3));
            int title_y = pad;
            RECT title_rc{content_x, title_y, content_x + content_w, title_y + DesignSystem::ScaleForDpi(26, ctx->dpi)};
            DrawTextW(hdc, L"Remove Download", -1, &title_rc, DT_LEFT | DT_TOP | DT_SINGLELINE);
            const auto& pal = ctx->view->GetPalette();
            RECT divider{content_x, title_y + DesignSystem::ScaleForDpi(26, ctx->dpi), content_x + content_w,
                         title_y + DesignSystem::ScaleForDpi(27, ctx->dpi)};
            FillRectSolid(hdc, divider, pal.border_subtle);
            RECT accent_divider{divider.left, divider.top, divider.left + DesignSystem::ScaleForDpi(38, ctx->dpi), divider.bottom};
            FillRectSolid(hdc, accent_divider, pal.accent_primary);

            // Subtitle / question with middle truncation
            SelectObject(hdc, ctx->hFontBody);
            SetTextColor(hdc, RGB(0xDE, 0xDF, 0xE4));
            std::wstring q_text;
            if (ctx->files_info.size() == 1) {
                std::string fname = ctx->files_info[0].first;
                if (fname.size() > 42) {
                    auto dot = fname.rfind('.');
                    std::string ext = (dot != std::string::npos) ? fname.substr(dot) : "";
                    std::string stem = (dot != std::string::npos) ? fname.substr(0, dot) : fname;
                    if (stem.size() > 28) {
                        stem = stem.substr(0, 20) + "..." + stem.substr(stem.size() - 6);
                    }
                    fname = stem + ext;
                }
                q_text = L"Choose how to remove \u201c" + Utf8ToW(fname) + L"\u201d.";
            } else {
                q_text = L"Choose how to remove " + std::to_wstring(ctx->files_info.size()) + L" selected downloads.";
            }
            int q_y = title_y + DesignSystem::ScaleForDpi(28, ctx->dpi);
            RECT q_rc{content_x, q_y, content_x + content_w, q_y + DesignSystem::ScaleForDpi(22, ctx->dpi)};
            DrawTextW(hdc, q_text.c_str(), -1, &q_rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Path & hint
            if (!ctx->files_info.empty()) {
                std::wstring path_str;
                if (ctx->files_info.size() == 1) {
                    path_str = L"Location: " + Utf8ToW((std::filesystem::path(ctx->files_info[0].second) / ctx->files_info[0].first).string());
                } else {
                    path_str = L"Target Folder: " + Utf8ToW(ctx->files_info[0].second) + L" (" + std::to_wstring(ctx->files_info.size()) + L" items)";
                }
                SelectObject(hdc, ctx->hFontSmall);
                SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
                int path_y = q_y + DesignSystem::ScaleForDpi(22, ctx->dpi);
                RECT p_rc{content_x, path_y, content_x + content_w, path_y + DesignSystem::ScaleForDpi(20, ctx->dpi)};
                DrawTextW(hdc, path_str.c_str(), -1, &p_rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 6001) { // Permanently Delete
                auto res = ctx->view->ActionDeleteSelectedWithFile();
                if (!res.ok()) {
                    std::string err = res.error.has_value() ? res.error->message : "Unknown error";
                    MessageBoxW(hwnd, Utf8ToW("Deletion failed: " + err).c_str(),
                        L"Error \u2014 Radium", MB_ICONERROR | MB_OK);
                }
                if (ctx->owner && IsWindow(ctx->owner)) {
                    EnableWindow(ctx->owner, TRUE);
                    SetActiveWindow(ctx->owner);
                    SetForegroundWindow(ctx->owner);
                }
                DestroyWindow(hwnd);
                return 0;
            } else if (id == 6003) { // Cancel
                if (ctx->owner && IsWindow(ctx->owner)) {
                    EnableWindow(ctx->owner, TRUE);
                    SetActiveWindow(ctx->owner);
                    SetForegroundWindow(ctx->owner);
                }
                DestroyWindow(hwnd);
                return 0;
            } else if (id == 6004) { // Remove from List
                auto res = ctx->view->ActionRemoveSelectedFromList();
                if (!res.ok()) {
                    std::string err = res.error.has_value() ? res.error->message : "Unknown error";
                    MessageBoxW(hwnd, Utf8ToW("Remove failed: " + err).c_str(), L"Error \u2014 Radium", MB_ICONERROR | MB_OK);
                }
                if (ctx->owner && IsWindow(ctx->owner)) {
                    EnableWindow(ctx->owner, TRUE);
                    SetActiveWindow(ctx->owner);
                    SetForegroundWindow(ctx->owner);
                }
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
            if (ctx->hFontSmall) DeleteObject(ctx->hFontSmall);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowDeleteConfirmationDialogWin32(HWND hwnd_parent, DesktopView& view, DownloadController& controller) {
    auto checked = view.GetCheckedTaskIds();
    std::vector<std::string> ids;
    if (!checked.empty()) {
        ids.assign(checked.begin(), checked.end());
    } else if (!view.GetSelectedTaskId().empty()) {
        ids.push_back(view.GetSelectedTaskId());
    }
    if (ids.empty()) return;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DeleteConfirmDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hAppIcon = CreateRadiumAppIcon();
        wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"Radium_DeleteConfirmDlgClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    DeleteConfirmDialogContext ctx;
    ctx.owner = hwnd_parent;
    ctx.view = &view;
    ctx.controller = &controller;
    ctx.task_ids = ids;
    for (const auto& tid : ids) {
        auto t = controller.GetTask(tid);
        if (t.has_value()) {
            ctx.files_info.push_back({t->file_name, t->save_dir});
        }
    }

    UINT dpi = GetDpiForWindow(hwnd_parent);
    if (dpi == 0) dpi = 96;
    ctx.dpi = dpi;
    int dlg_w = DesignSystem::ScaleForDpi(500, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(220, dpi);

    RECT work_rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rc, 0);
    int screen_w = work_rc.right - work_rc.left;
    int screen_h = work_rc.bottom - work_rc.top;
    if (screen_w <= 0) screen_w = GetSystemMetrics(SM_CXSCREEN);
    if (screen_h <= 0) screen_h = GetSystemMetrics(SM_CYSCREEN);

    RECT prc{};
    bool parent_is_interactive = (hwnd_parent && IsWindow(hwnd_parent) && IsWindowVisible(hwnd_parent) && !IsIconic(hwnd_parent));
    if (parent_is_interactive) {
        GetWindowRect(hwnd_parent, &prc);
    }
    int dlg_x = 0;
    int dlg_y = 0;
    if (!parent_is_interactive || prc.left < -10000 || prc.top < -10000 ||
        (prc.right - prc.left) < 200 || (prc.bottom - prc.top) < 200) {
        dlg_x = work_rc.left + (screen_w - dlg_w) / 2;
        dlg_y = work_rc.top + (screen_h - dlg_h) / 2;
    } else {
        dlg_x = prc.left + (prc.right - prc.left - dlg_w) / 2;
        dlg_y = prc.top + (prc.bottom - prc.top - dlg_h) / 2;
    }
    if (dlg_x < work_rc.left) dlg_x = work_rc.left + 20;
    if (dlg_y < work_rc.top) dlg_y = work_rc.top + 20;

    HWND owner_hwnd = parent_is_interactive ? hwnd_parent : nullptr;
    DWORD ex_style = (owner_hwnd ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT;

    HWND hDlg = CreateWindowExW(
        ex_style,
        L"Radium_DeleteConfirmDlgClass",
        L"Remove Download \u2014 Radium",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        owner_hwnd, nullptr, hInst, &ctx);

    if (!hDlg) return;

    HICON hDlgIcon = CreateRadiumAppIcon();
    if (hDlgIcon) {
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hDlgIcon));
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hDlgIcon));
    }

    ApplyWindowGlassBackdropWin32(hDlg, true /* is_transient_dialog */);

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        EnableWindow(owner_hwnd, FALSE);
    }
    SetWindowPos(hDlg, HWND_TOP, dlg_x, dlg_y, dlg_w, dlg_h, SWP_SHOWWINDOW);
    ShowWindow(hDlg, SW_SHOWNORMAL);
    BringWindowToTop(hDlg);
    SetForegroundWindow(hDlg);
    SetActiveWindow(hDlg);
    SetFocus(ctx.hCancelBtn); // Safe default focus

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                DestroyWindow(hDlg);
                break;
            } else if (msg.wParam == VK_RETURN) {
                HWND focused = GetFocus();
                if (focused == ctx.hPermanentBtn) {
                    SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(6001, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hPermanentBtn));
                } else if (focused == ctx.hRemoveBtn) {
                    SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(6004, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hRemoveBtn));
                } else {
                    // Default enter action: Safe option (Cancel)
                    DestroyWindow(hDlg);
                }
                if (!IsWindow(hDlg)) break;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        EnableWindow(owner_hwnd, TRUE);
        SetForegroundWindow(owner_hwnd);
        InvalidateRect(owner_hwnd, nullptr, TRUE);
    }
}

namespace {

void DrawCaretGlyphGdi(HDC hdc, int cx, int cy, ColorRgba col, bool up = false);
void DrawRoundedBoxGdi(HDC hdc, const RECT& rc, int radius, ColorRgba bg, ColorRgba border, int border_w);

void PaintSettingsCenterOverlayWin32(HDC hdc, DesktopView& view, const RECT& client_rc, HFONT hFontBody, HFONT hFontBold) {
    const auto& pal = view.GetPalette();
    const auto& m = view.GetMetrics();
    auto sv = view.BuildSettingsCenterView();

    // 1. Smooth subtle entrance transition (150ms slide up + scrim fade)
    float p = view.GetAnimationState().ComputeTransitionProgress(view.GetAnimationState().settings_open_start_ms, 150);
    uint8_t scrim_alpha = static_cast<uint8_t>(175 * p);
    DrawGlassPanelGdi(hdc, client_rc, 0, ColorRgba{0x08, 0x08, 0x0A, scrim_alpha}, ColorRgba{0, 0, 0, 0}, 0, false);

    // 2. Translucent glass modal card with specular highlight
    int slide_offset_y = static_cast<int>((1.0f - p) * 12.0f);
    RECT modal_rc = ToWinRect(sv.modal_rect);
    modal_rc.top += slide_offset_y;
    modal_rc.bottom += slide_offset_y;
    DrawGlassPanelGdi(hdc, modal_rc, DesignSystem::ScaleForDpi(12, m.dpi), pal.glass_surface, pal.glass_border, 1, true /* add_top_specular */);

    int header_h = DesignSystem::ScaleForDpi(52, m.dpi);
    int footer_h = DesignSystem::ScaleForDpi(52, m.dpi);
    RECT hdr_rc{modal_rc.left + 1, modal_rc.top + 1, modal_rc.right - 1, modal_rc.top + header_h};
    DrawGlassPanelGdi(hdc, hdr_rc, DesignSystem::ScaleForDpi(12, m.dpi), pal.glass_elevated, pal.glass_border, 0, true /* top specular */);

    SelectObject(hdc, hFontBold);
    SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
    RECT title_rc = hdr_rc;
    title_rc.left += m.space_lg;
    DrawTextW(hdc, L"Radium Settings & Configuration Center", -1, &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Sidebar categories
    RECT side_rc = ToWinRect(sv.sidebar_rect);
    DrawGlassPanelGdi(hdc, side_rc, 0, pal.glass_surface, pal.border_subtle, 0, false);
    SelectObject(hdc, hFontBody);
    for (const auto& cat : sv.categories) {
        RECT crc = ToWinRect(cat.rect);
        if (cat.selected) {
            DrawGlassPanelGdi(hdc, crc, DesignSystem::ScaleForDpi(6, m.dpi), pal.glass_elevated, pal.accent_primary, 1, false);
        }
        SetTextColor(hdc, (cat.selected ? pal.text_primary : pal.text_secondary).ToWin32ColorRef());
        std::string lbl = cat.label;
        if (cat.modified_count > 0) {
            lbl += " *";
        }
        std::wstring wlbl = Utf8ToW(lbl);
        RECT trc = crc;
        trc.left += m.space_md;
        DrawTextW(hdc, wlbl.c_str(), -1, &trc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // Content rows
    RECT content_rc = ToWinRect(sv.content_rect);
    DrawGlassPanelGdi(hdc, content_rc, 0, pal.glass_surface, pal.border_subtle, 0, false);
    for (const auto& item : sv.visible_items) {
        RECT rrc = ToWinRect(item.row_rect);
        ColorRgba row_bg = item.is_modified ? pal.glass_elevated : pal.glass_surface;
        ColorRgba row_border = item.is_modified ? pal.accent_primary : pal.glass_border;
        DrawGlassPanelGdi(hdc, rrc, DesignSystem::ScaleForDpi(6, m.dpi), row_bg, row_border, 1, false);

        RECT lbl_rc = rrc;
        lbl_rc.left += m.space_md;
        lbl_rc.top += 6;
        lbl_rc.right = item.control_rect.left - m.space_sm;
        lbl_rc.bottom = rrc.top + (rrc.bottom - rrc.top) / 2 + 2;
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        std::wstring wtitle = Utf8ToW(item.label + (item.is_modified ? "  [Modified]" : ""));
        DrawTextW(hdc, wtitle.c_str(), -1, &lbl_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT desc_rc = rrc;
        desc_rc.left += m.space_md;
        desc_rc.top = lbl_rc.bottom;
        desc_rc.right = item.control_rect.left - m.space_sm;
        desc_rc.bottom -= 4;
        SelectObject(hdc, hFontBody);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring wdesc = Utf8ToW(item.description);
        DrawTextW(hdc, wdesc.c_str(), -1, &desc_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT ctrl_rc = ToWinRect(item.control_rect);
        if (item.control_type == SettingControlType::kToggleSwitch) {
            int sw_w = DesignSystem::ScaleForDpi(44, m.dpi);
            int sw_h = DesignSystem::ScaleForDpi(22, m.dpi);
            int sw_x = ctrl_rc.right - sw_w - 4;
            int sw_y = ctrl_rc.top + (ctrl_rc.bottom - ctrl_rc.top - sw_h) / 2;
            RECT sw_rc{sw_x, sw_y, sw_x + sw_w, sw_y + sw_h};
            ColorRgba track_bg = item.bool_value ? pal.accent_primary : pal.surface_elevated;
            ColorRgba track_border = item.bool_value ? pal.accent_hover : pal.border_subtle;
            DrawRoundedBoxGdi(hdc, sw_rc, sw_h / 2, track_bg, track_border, 1);

            int thumb_sz = sw_h - 4;
            int thumb_x = item.bool_value ? (sw_rc.right - thumb_sz - 2) : (sw_rc.left + 2);
            int thumb_y = sw_y + 2;
            RECT thumb_rc{thumb_x, thumb_y, thumb_x + thumb_sz, thumb_y + thumb_sz};
            DrawRoundedBoxGdi(hdc, thumb_rc, thumb_sz / 2, ColorRgba{255, 255, 255, 255}, ColorRgba{240, 240, 240, 255}, 1);
        } else {
            ColorRgba ctrl_bg = pal.glass_elevated;
            ColorRgba ctrl_border = pal.glass_border;
            DrawGlassPanelGdi(hdc, ctrl_rc, DesignSystem::ScaleForDpi(6, m.dpi), ctrl_bg, ctrl_border, 1, false);
            SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
            std::wstring wval = Utf8ToW(item.value_text);
            if (item.control_type == SettingControlType::kDropdown) {
                RECT txt_rc = ctrl_rc;
                txt_rc.right -= 18;
                txt_rc.left += 8;
                DrawTextW(hdc, wval.c_str(), -1, &txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                DrawCaretGlyphGdi(hdc, ctrl_rc.right - 12, ctrl_rc.top + (ctrl_rc.bottom - ctrl_rc.top) / 2, pal.text_muted, false);
            } else {
                DrawTextW(hdc, wval.c_str(), -1, &ctrl_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
    }

    // Footer bar with status banner and Reset / Cancel / Apply buttons
    RECT ftr_rc{modal_rc.left + 1, modal_rc.bottom - footer_h, modal_rc.right - 1, modal_rc.bottom - 1};
    DrawGlassPanelGdi(hdc, ftr_rc, DesignSystem::ScaleForDpi(12, m.dpi), pal.glass_elevated, pal.glass_border, 0, false);

    RECT rst_rc = ToWinRect(sv.restore_defaults_rect);
    DrawGlassPanelGdi(hdc, rst_rc, DesignSystem::ScaleForDpi(6, m.dpi), pal.glass_surface, pal.glass_border, 1, false);
    SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
    DrawTextW(hdc, L"Restore Defaults", -1, &rst_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT can_rc = ToWinRect(sv.cancel_button_rect);
    DrawGlassPanelGdi(hdc, can_rc, DesignSystem::ScaleForDpi(6, m.dpi), pal.glass_surface, pal.glass_border, 1, false);
    DrawTextW(hdc, L"Cancel", -1, &can_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT app_rc = ToWinRect(sv.apply_button_rect);
    ColorRgba app_bg = sv.is_valid ? pal.accent_primary : pal.status_danger;
    DrawGlassPanelGdi(hdc, app_rc, DesignSystem::ScaleForDpi(6, m.dpi), app_bg, app_bg, 0, false);
    SelectObject(hdc, hFontBold);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, L"Apply & Save", -1, &app_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT banner_rc{rst_rc.right + m.space_md, ftr_rc.top, can_rc.left - m.space_md, ftr_rc.bottom};
    SelectObject(hdc, hFontBody);
    SetTextColor(hdc, (sv.is_valid ? pal.text_secondary : pal.status_danger).ToWin32ColorRef());
    std::wstring wbanner = Utf8ToW(sv.status_banner);
    DrawTextW(hdc, wbanner.c_str(), -1, &banner_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawRoundedBoxGdi(HDC hdc, const RECT& rc, int radius, ColorRgba bg, ColorRgba border, int border_w) {
    HPEN hPen = (border_w > 0) ? CreatePen(PS_SOLID, border_w, border.ToWin32ColorRef()) : CreatePen(PS_NULL, 0, 0);
    HBRUSH hBrush = CreateSolidBrush(bg.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void DrawCheckboxGdi(HDC hdc, const RECT& rc, bool checked, ColorRgba bg, ColorRgba border, ColorRgba check_col) {
    DrawRoundedBoxGdi(hdc, rc, 6, bg, border, 1);
    if (checked) {
        HPEN hPen = CreatePen(PS_SOLID, 2, check_col.ToWin32ColorRef());
        HGDIOBJ oldPen = SelectObject(hdc, hPen);
        POINT pts[3] = {
            {rc.left + 4, rc.top + (rc.bottom - rc.top) / 2},
            {rc.left + (rc.right - rc.left) / 2 - 1, rc.bottom - 5},
            {rc.right - 4, rc.top + 5}
        };
        Polyline(hdc, pts, 3);
        SelectObject(hdc, oldPen);
        DeleteObject(hPen);
    }
}

void DrawDocFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba col) {
    int fold = 5;
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    POINT pts[6] = {
        {x, y},
        {x + w - fold, y},
        {x + w, y + fold},
        {x + w, y + h},
        {x, y + h},
        {x, y}
    };
    Polygon(hdc, pts, 6);

    MoveToEx(hdc, x + w - fold, y, nullptr);
    LineTo(hdc, x + w - fold, y + fold);
    LineTo(hdc, x + w, y + fold);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

void DrawVideoFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Film sheet frame with coral red accent and centered play triangle
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    int cx = x + w / 2 - 1;
    int cy = y + h / 2 + 2;
    int sz = 4;
    POINT tri[3] = {
        {cx - sz + 1, cy - sz},
        {cx + sz + 1, cy},
        {cx - sz + 1, cy + sz}
    };
    HPEN hPen = CreatePen(PS_SOLID, 1, accent.ToWin32ColorRef());
    HBRUSH hBrush = CreateSolidBrush(accent.ToWin32ColorRef());
    HGDIOBJ op = SelectObject(hdc, hPen);
    HGDIOBJ ob = SelectObject(hdc, hBrush);
    Polygon(hdc, tri, 3);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void DrawAudioFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Violet sheet outline with 3 vertical equalizer lines
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    int cx = x + w / 2;
    int cy = y + h / 2 + 2;
    HPEN hPen = CreatePen(PS_SOLID, 2, accent.ToWin32ColorRef());
    HGDIOBJ op = SelectObject(hdc, hPen);
    // Bar 1
    MoveToEx(hdc, cx - 4, cy + 3, nullptr);
    LineTo(hdc, cx - 4, cy - 3);
    // Bar 2 (center, tallest)
    MoveToEx(hdc, cx, cy + 5, nullptr);
    LineTo(hdc, cx, cy - 5);
    // Bar 3
    MoveToEx(hdc, cx + 4, cy + 4, nullptr);
    LineTo(hdc, cx + 4, cy - 2);
    SelectObject(hdc, op);
    DeleteObject(hPen);
}

void DrawArchiveFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Amber sheet outline with zipper pattern in center
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    int cx = x + w / 2;
    int cy = y + h / 2 + 2;
    HPEN hPen = CreatePen(PS_SOLID, 1, accent.ToWin32ColorRef());
    HGDIOBJ op = SelectObject(hdc, hPen);
    for (int dy = -4; dy <= 4; dy += 2) {
        int tooth_w = ((dy % 4) == 0) ? 3 : 2;
        MoveToEx(hdc, cx - tooth_w, cy + dy, nullptr);
        LineTo(hdc, cx + tooth_w, cy + dy);
    }
    SelectObject(hdc, op);
    DeleteObject(hPen);
}

void DrawPdfFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Crimson sheet outline with solid red fold badge
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    int cx = x + w / 2;
    int cy = y + h / 2 + 2;
    RECT badge{cx - 6, cy - 3, cx + 6, cy + 4};
    HBRUSH hBrush = CreateSolidBrush(accent.ToWin32ColorRef());
    FillRect(hdc, &badge, hBrush);
    DeleteObject(hBrush);
}

void DrawImageFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Cyan sheet with sun and two mountains
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    int cx = x + w / 2;
    int cy = y + h / 2 + 2;
    HPEN hPen = CreatePen(PS_SOLID, 1, accent.ToWin32ColorRef());
    HGDIOBJ op = SelectObject(hdc, hPen);
    // Sun
    Ellipse(hdc, cx + 1, cy - 4, cx + 5, cy);
    // Mountain 1 (larger)
    POINT m1[3] = {{cx - 6, cy + 5}, {cx - 2, cy - 1}, {cx + 2, cy + 5}};
    Polyline(hdc, m1, 3);
    // Mountain 2 (smaller)
    POINT m2[3] = {{cx, cy + 5}, {cx + 3, cy + 1}, {cx + 6, cy + 5}};
    Polyline(hdc, m2, 3);
    SelectObject(hdc, op);
    DeleteObject(hPen);
}

void DrawExecutableFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Emerald window frame with titlebar
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    int cx = x + w / 2;
    int cy = y + h / 2 + 2;
    HPEN hPen = CreatePen(PS_SOLID, 1, accent.ToWin32ColorRef());
    HGDIOBJ op = SelectObject(hdc, hPen);
    RECT win_rc{cx - 5, cy - 4, cx + 5, cy + 5};
    Rectangle(hdc, win_rc.left, win_rc.top, win_rc.right, win_rc.bottom);
    MoveToEx(hdc, win_rc.left, cy - 2, nullptr);
    LineTo(hdc, win_rc.right, cy - 2);
    SelectObject(hdc, op);
    DeleteObject(hPen);
}

void DrawDocumentFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Slate blue sheet with text lines
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    int cx = x + w / 2;
    int cy = y + h / 2 + 2;
    HPEN hPen = CreatePen(PS_SOLID, 1, accent.ToWin32ColorRef());
    HGDIOBJ op = SelectObject(hdc, hPen);
    MoveToEx(hdc, cx - 4, cy - 3, nullptr); LineTo(hdc, cx + 4, cy - 3);
    MoveToEx(hdc, cx - 4, cy, nullptr);     LineTo(hdc, cx + 4, cy);
    MoveToEx(hdc, cx - 4, cy + 3, nullptr); LineTo(hdc, cx + 2, cy + 3);
    SelectObject(hdc, op);
    DeleteObject(hPen);
}

void DrawGenericFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
}

void DrawFileTypeIconGdi(HDC hdc, int x, int y, int w, int h, const FormattedTaskRow& r, const ThemePalette& pal) {
    std::string name = ToLower(r.full_filename_tooltip.empty() ? r.display_filename : r.full_filename_tooltip);
    auto ext_pos = name.rfind('.');
    std::string ext = (ext_pos != std::string::npos) ? name.substr(ext_pos) : "";

    ColorRgba accent = pal.accent_primary; // Signature coral red #FF4F57
    ColorRgba accent_subtle = ColorRgba{0xFF, 0x73, 0x7A, 255};

    if (r.is_stream || r.category == "Video" || ext == ".mp4" || ext == ".mkv" || ext == ".webm" ||
        ext == ".avi" || ext == ".mov" || ext == ".flv" || ext == ".ts" || ext == ".m4v" || ext == ".m3u8" || ext == ".mpd") {
        DrawVideoFileIconGdi(hdc, x, y, w, h, accent);
    } else if (r.category == "Audio" || ext == ".mp3" || ext == ".m4a" || ext == ".aac" ||
               ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".opus") {
        DrawAudioFileIconGdi(hdc, x, y, w, h, accent);
    } else if (r.category == "Compressed" || ext == ".zip" || ext == ".rar" || ext == ".7z" ||
               ext == ".tar" || ext == ".gz" || ext == ".bz2" || ext == ".xz" || ext == ".iso") {
        DrawArchiveFileIconGdi(hdc, x, y, w, h, accent);
    } else if (r.category == "PDF" || ext == ".pdf") {
        DrawPdfFileIconGdi(hdc, x, y, w, h, accent);
    } else if (r.category == "Image" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
               ext == ".gif" || ext == ".webp" || ext == ".svg" || ext == ".bmp" || ext == ".ico") {
        DrawImageFileIconGdi(hdc, x, y, w, h, accent);
    } else if (r.category == "Programs" || ext == ".exe" || ext == ".msi" || ext == ".dll" ||
               ext == ".bat" || ext == ".cmd" || ext == ".ps1") {
        DrawExecutableFileIconGdi(hdc, x, y, w, h, accent);
    } else if (r.category == "Documents" || ext == ".txt" || ext == ".doc" || ext == ".docx" ||
               ext == ".xls" || ext == ".xlsx" || ext == ".ppt" || ext == ".pptx" || ext == ".csv" ||
               ext == ".md" || ext == ".json" || ext == ".xml" || ext == ".cpp" || ext == ".h" ||
               ext == ".hpp" || ext == ".js" || ext == ".ts" || ext == ".py") {
        DrawDocumentFileIconGdi(hdc, x, y, w, h, accent);
    } else {
        DrawGenericFileIconGdi(hdc, x, y, w, h, accent_subtle);
    }
}

void DrawPauseGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    int bar_w = 2;
    int bar_h = 10;
    int gap = 3;
    RECT r1{cx - gap - bar_w, cy - bar_h / 2, cx - gap, cy + bar_h / 2};
    RECT r2{cx + gap, cy - bar_h / 2, cx + gap + bar_w, cy + bar_h / 2};
    FillRectSolid(hdc, r1, col);
    FillRectSolid(hdc, r2, col);
}

void DrawPlayGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    int sz = 4;
    POINT pts[3] = {
        {cx - sz + 1, cy - sz},
        {cx + sz + 1, cy},
        {cx - sz + 1, cy + sz}
    };
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HBRUSH hBrush = CreateSolidBrush(col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void DrawSearchGlassGdi(HDC hdc, int x, int y, int size, ColorRgba col) {
    HPEN hPen = CreatePen(PS_SOLID, 2, col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    int radius = size - 4;
    Ellipse(hdc, x, y, x + radius, y + radius);

    MoveToEx(hdc, x + radius - 1, y + radius - 1, nullptr);
    LineTo(hdc, x + size + 2, y + size + 2);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

void DrawGearGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    HPEN hPen = CreatePen(PS_SOLID, 2, col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    int r_outer = 6;
    Ellipse(hdc, cx - r_outer, cy - r_outer, cx + r_outer + 1, cy + r_outer + 1);

    int r_inner = 2;
    Ellipse(hdc, cx - r_inner, cy - r_inner, cx + r_inner + 1, cy + r_inner + 1);

    int tooth = 3;
    MoveToEx(hdc, cx + r_outer, cy, nullptr); LineTo(hdc, cx + r_outer + tooth, cy);
    MoveToEx(hdc, cx - r_outer, cy, nullptr); LineTo(hdc, cx - r_outer - tooth, cy);
    MoveToEx(hdc, cx, cy + r_outer, nullptr); LineTo(hdc, cx, cy + r_outer + tooth);
    MoveToEx(hdc, cx, cy - r_outer, nullptr); LineTo(hdc, cx, cy - r_outer - tooth);
    MoveToEx(hdc, cx + 4, cy + 4, nullptr); LineTo(hdc, cx + 7, cy + 7);
    MoveToEx(hdc, cx - 4, cy - 4, nullptr); LineTo(hdc, cx - 7, cy - 7);
    MoveToEx(hdc, cx + 4, cy - 4, nullptr); LineTo(hdc, cx + 7, cy - 7);
    MoveToEx(hdc, cx - 4, cy + 4, nullptr); LineTo(hdc, cx - 7, cy + 7);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

void DrawSlidersGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    DrawGearGlyphGdi(hdc, rc, col);
}

void DrawCaretGlyphGdi(HDC hdc, int cx, int cy, ColorRgba col, bool up) {
    int w = 3;
    POINT pts[3];
    if (up) {
        pts[0] = {cx - w, cy + 2};
        pts[1] = {cx, cy - 2};
        pts[2] = {cx + w, cy + 2};
    } else {
        pts[0] = {cx - w, cy - 2};
        pts[1] = {cx, cy + 2};
        pts[2] = {cx + w, cy - 2};
    }
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HBRUSH hBrush = CreateSolidBrush(col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void DrawFunnelGlyphGdi(HDC hdc, int x, int y, int size, ColorRgba col) {
    POINT pts[4] = {
        {x, y},
        {x + size, y},
        {x + size / 2 + 1, y + size - 2},
        {x + size / 2 - 1, y + size - 2}
    };
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HBRUSH hBrush = CreateSolidBrush(col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
    Polygon(hdc, pts, 4);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void DrawHamburgerGlyphGdi(HDC hdc, int x, int y, int w, int h, ColorRgba col) {
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    int y0 = y + 1;
    int y1 = y + h / 2;
    int y2 = y + h - 2;
    MoveToEx(hdc, x, y0, nullptr); LineTo(hdc, x + w, y0);
    MoveToEx(hdc, x, y1, nullptr); LineTo(hdc, x + w, y1);
    MoveToEx(hdc, x, y2, nullptr); LineTo(hdc, x + w, y2);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

void DrawTimerClockGdi(HDC hdc, int x, int y, int radius, ColorRgba col) {
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Ellipse(hdc, x - radius, y - radius, x + radius, y + radius);
    MoveToEx(hdc, x, y, nullptr);
    LineTo(hdc, x, y - radius + 3);
    MoveToEx(hdc, x, y, nullptr);
    LineTo(hdc, x + radius - 4, y);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

HICON CreateRadiumAppIcon() {
    static HICON s_cached_icon = nullptr;
    if (s_cached_icon) return s_cached_icon;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    s_cached_icon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (s_cached_icon) return s_cached_icon;

    int size = 32;
    HDC hdcScreen = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmColor = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBm = SelectObject(hdc, hbmColor);

    RECT rc{0, 0, size, size};
    HBRUSH bgBr = CreateSolidBrush(RGB(0x16, 0x16, 0x18));
    FillRect(hdc, &rc, bgBr);
    DeleteObject(bgBr);

    RECT sqRc{2, 2, size - 2, size - 2};
    DrawRoundedBoxGdi(hdc, sqRc, 8, {0xFF, 0x47, 0x57, 255}, {0xFF, 0x47, 0x57, 255}, 1);

    HFONT hFont = CreateFontW(
        -20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT textRc{2, 2, size - 2, size - 2};
    DrawTextW(hdc, L"R", 1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);

    SelectObject(hdc, oldBm);
    DeleteDC(hdc);
    ReleaseDC(nullptr, hdcScreen);

    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;
    s_cached_icon = CreateIconIndirect(&ii);
    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return s_cached_icon;
}

HICON CreateFluxAppIcon() {
    return CreateRadiumAppIcon();
}

// -----------------------------------------------------------------------------
// Radium Dedicated Add Torrent Dialog (qBittorrent Organization & Dark Glass Styling)
// -----------------------------------------------------------------------------
constexpr int kAddTorModeCombo       = 8201;
constexpr int kAddTorSaveDirEdit     = 8202;
constexpr int kAddTorBrowseBtn       = 8203;
constexpr int kAddTorIncompleteChk   = 8204;
constexpr int kAddTorRememberPathChk = 8205;
constexpr int kAddTorCategoryCombo   = 8206;
constexpr int kAddTorDefaultCatChk   = 8207;
constexpr int kAddTorTagsEdit        = 8208;
constexpr int kAddTorStartChk        = 8209;
constexpr int kAddTorStopCondCombo   = 8210;
constexpr int kAddTorQueueTopChk     = 8211;
constexpr int kAddTorSkipHashChk     = 8212;
constexpr int kAddTorSequentialChk   = 8213;
constexpr int kAddTorFirstLastChk    = 8214;
constexpr int kAddTorLayoutCombo     = 8215;
constexpr int kAddTorNeverShowChk    = 8216;
constexpr int kAddTorSaveTorrentBtn  = 8217;
constexpr int kAddTorSelectAllBtn    = 8218;
constexpr int kAddTorSelectNoneBtn   = 8219;
constexpr int kAddTorFilterEdit      = 8220;
constexpr int kAddTorFileList        = 8221;
constexpr int kAddTorOkBtn           = 8222;
constexpr int kAddTorCancelBtn       = 8223;

std::string FormatAddTorrentBytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
    return out.str();
}

struct AddTorrentTreeNode {
    int id{0};
    int parent_id{-1};
    int depth{0};
    bool is_dir{false};
    bool expanded{true};
    bool checked{true};
    int check_state{1}; // 0 = unchecked, 1 = checked, 2 = partial
    int file_idx{-1};
    uint64_t size{0};
    std::wstring name;
    std::string full_path;
    std::vector<int> children_ids;
};

struct AddTorrentDialogContext {
    HWND hDlg{nullptr};
    HWND owner{nullptr};
    DesktopView* view{nullptr};
    DownloadController* controller{nullptr};
    torrent::TorrentInfo* info{nullptr};
    std::string torrent_file_path;
    std::string target_save_dir;
    bool start_immediately{true};
    bool add_to_queue_top{false};
    bool skip_hash_check{false};
    bool sequential_download{false};
    bool first_last_piece_priority{false};
    bool remember_save_path{true};
    bool use_incomplete_path{false};
    bool never_show_again{false};
    std::string selected_category;
    std::string selected_tags;
    bool accepted{false};
    uint32_t dpi{96};

    // Controls
    HWND hModeCombo{nullptr};
    HWND hSaveDirEdit{nullptr};
    HWND hBrowseBtn{nullptr};
    HWND hIncompleteChk{nullptr};
    HWND hRememberPathChk{nullptr};
    HWND hCategoryCombo{nullptr};
    HWND hDefaultCatChk{nullptr};
    HWND hTagsEdit{nullptr};
    HWND hStartChk{nullptr};
    HWND hStopCondCombo{nullptr};
    HWND hQueueTopChk{nullptr};
    HWND hSkipHashChk{nullptr};
    HWND hSequentialChk{nullptr};
    HWND hFirstLastChk{nullptr};
    HWND hLayoutCombo{nullptr};
    HWND hNeverShowChk{nullptr};
    HWND hSaveTorrentBtn{nullptr};

    HWND hSelectAllBtn{nullptr};
    HWND hSelectNoneBtn{nullptr};
    HWND hFilterEdit{nullptr};
    HWND hFileList{nullptr};
    HWND hSummaryStatic{nullptr};

    HWND hOkBtn{nullptr};
    HWND hCancelBtn{nullptr};

    // Fonts & GDI Resources
    HFONT hFontTitle{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontSmall{nullptr};
    HBRUSH hBgBrush{nullptr};
    HBRUSH hPanelBrush{nullptr};
    HBRUSH hEditBrush{nullptr};

    // Tree data
    std::vector<AddTorrentTreeNode> all_nodes;
    std::vector<int> visible_node_indices;
    std::string filter_text;

    void BuildTree() {
        all_nodes.clear();
        if (!info) return;

        // Root node
        AddTorrentTreeNode root;
        root.id = 0;
        root.parent_id = -1;
        root.depth = 0;
        root.is_dir = true;
        root.expanded = true;
        root.checked = true;
        root.check_state = 1;
        root.name = Utf8ToW(info->name.empty() ? "Torrent Contents" : info->name);
        root.full_path = info->name;
        root.size = 0;
        all_nodes.push_back(root);

        for (size_t f_idx = 0; f_idx < info->files.size(); ++f_idx) {
            const auto& file = info->files[f_idx];
            std::string p_str = file.relative_path.generic_string();
            // Split path components
            std::vector<std::string> parts;
            std::string cur;
            for (char c : p_str) {
                if (c == '/' || c == '\\') {
                    if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) parts.push_back(cur);

            int current_parent_id = 0;
            int current_depth = 1;
            std::string accumulated_path = info->name;

            for (size_t p_i = 0; p_i < parts.size(); ++p_i) {
                bool is_leaf = (p_i == parts.size() - 1);
                accumulated_path += "/" + parts[p_i];

                int found_child_id = -1;
                for (int cid : all_nodes[current_parent_id].children_ids) {
                    if (all_nodes[cid].name == Utf8ToW(parts[p_i])) {
                        found_child_id = cid;
                        break;
                    }
                }

                if (found_child_id == -1) {
                    AddTorrentTreeNode node;
                    node.id = static_cast<int>(all_nodes.size());
                    node.parent_id = current_parent_id;
                    node.depth = current_depth;
                    node.is_dir = !is_leaf;
                    node.expanded = true;
                    node.checked = is_leaf ? file.selected : true;
                    node.check_state = node.checked ? 1 : 0;
                    node.file_idx = is_leaf ? static_cast<int>(f_idx) : -1;
                    node.size = is_leaf ? file.length : 0;
                    node.name = Utf8ToW(parts[p_i]);
                    node.full_path = accumulated_path;

                    all_nodes[current_parent_id].children_ids.push_back(node.id);
                    all_nodes.push_back(node);
                    found_child_id = node.id;
                }
                current_parent_id = found_child_id;
                current_depth++;
            }
        }

        // Compute directory sizes
        RecalculateDirectorySizes(0);
        UpdateCheckStatesFromLeaves();
        RebuildVisibleList();
    }

    uint64_t RecalculateDirectorySizes(int node_id) {
        if (node_id < 0 || node_id >= static_cast<int>(all_nodes.size())) return 0;
        if (!all_nodes[node_id].is_dir) return all_nodes[node_id].size;
        uint64_t total = 0;
        for (int cid : all_nodes[node_id].children_ids) {
            total += RecalculateDirectorySizes(cid);
        }
        all_nodes[node_id].size = total;
        return total;
    }

    void UpdateCheckStatesFromLeaves() {
        for (int i = static_cast<int>(all_nodes.size()) - 1; i >= 0; --i) {
            if (all_nodes[i].is_dir) {
                if (all_nodes[i].children_ids.empty()) {
                    all_nodes[i].check_state = all_nodes[i].checked ? 1 : 0;
                    continue;
                }
                int checked_count = 0;
                int partial_count = 0;
                for (int cid : all_nodes[i].children_ids) {
                    if (all_nodes[cid].check_state == 1) checked_count++;
                    else if (all_nodes[cid].check_state == 2) partial_count++;
                }
                if (checked_count == static_cast<int>(all_nodes[i].children_ids.size())) {
                    all_nodes[i].check_state = 1;
                    all_nodes[i].checked = true;
                } else if (checked_count == 0 && partial_count == 0) {
                    all_nodes[i].check_state = 0;
                    all_nodes[i].checked = false;
                } else {
                    all_nodes[i].check_state = 2; // Partial
                    all_nodes[i].checked = true;
                }
            } else {
                all_nodes[i].check_state = all_nodes[i].checked ? 1 : 0;
            }
        }
    }

    void SetNodeChecked(int node_id, bool checked) {
        if (node_id < 0 || node_id >= static_cast<int>(all_nodes.size())) return;
        all_nodes[node_id].checked = checked;
        all_nodes[node_id].check_state = checked ? 1 : 0;
        for (int cid : all_nodes[node_id].children_ids) {
            SetNodeChecked(cid, checked);
        }
    }

    void ToggleNode(int node_id) {
        if (node_id < 0 || node_id >= static_cast<int>(all_nodes.size())) return;
        bool new_state = (all_nodes[node_id].check_state == 0);
        SetNodeChecked(node_id, new_state);
        UpdateCheckStatesFromLeaves();
        SyncToFileInfo();
        RebuildVisibleList();
    }

    void SyncToFileInfo() {
        if (!info) return;
        for (const auto& node : all_nodes) {
            if (!node.is_dir && node.file_idx >= 0 && static_cast<size_t>(node.file_idx) < info->files.size()) {
                info->files[static_cast<size_t>(node.file_idx)].selected = (node.check_state == 1);
            }
        }
    }

    void RebuildVisibleList() {
        visible_node_indices.clear();
        std::string filter_low = ToLower(filter_text);
        std::function<void(int)> collect = [&](int node_id) {
            if (node_id < 0 || node_id >= static_cast<int>(all_nodes.size())) return;
            const auto& node = all_nodes[node_id];

            bool matches_filter = true;
            if (!filter_low.empty()) {
                std::string full_low = ToLower(node.full_path);
                matches_filter = (full_low.find(filter_low) != std::string::npos);
            }

            if (matches_filter || filter_low.empty()) {
                visible_node_indices.push_back(node_id);
            }

            if (node.is_dir && (node.expanded || !filter_low.empty())) {
                for (int cid : node.children_ids) {
                    collect(cid);
                }
            }
        };

        if (!all_nodes.empty()) {
            collect(0);
        }

        if (hFileList && IsWindow(hFileList)) {
            SendMessageW(hFileList, LB_RESETCONTENT, 0, 0);
            SendMessageW(hFileList, LB_SETCOUNT, static_cast<WPARAM>(visible_node_indices.size()), 0);
            InvalidateRect(hFileList, nullptr, FALSE);
        }
    }

    std::wstring GetSummaryText() const {
        if (!info) return L"";
        uint64_t sel_bytes = 0;
        size_t sel_files = 0;
        for (const auto& f : info->files) {
            if (f.selected) {
                sel_bytes += f.length;
                sel_files++;
            }
        }
        std::string s = "Selected " + FormatAddTorrentBytes(sel_bytes) + " of " + FormatAddTorrentBytes(info->total_length) +
                        " (" + std::to_string(sel_files) + "/" + std::to_string(info->files.size()) + " files)";
        return Utf8ToW(s);
    }
};

LRESULT CALLBACK AddTorrentDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<AddTorrentDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<AddTorrentDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx) ctx->hDlg = hwnd;
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            HINSTANCE hInst = GetModuleHandleW(nullptr);
            ctx->dpi = ctx->view ? ctx->view->GetMetrics().dpi : 96;
            if (ctx->dpi == 0) ctx->dpi = 96;

            int f_title_sz = -DesignSystem::ScaleForDpi(16, ctx->dpi);
            int f_body_sz  = -DesignSystem::ScaleForDpi(12, ctx->dpi);
            int f_small_sz = -DesignSystem::ScaleForDpi(11, ctx->dpi);

            ctx->hFontTitle = CreateFontW(f_title_sz, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBold  = CreateFontW(f_body_sz, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBody  = CreateFontW(f_body_sz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontSmall = CreateFontW(f_small_sz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

            ctx->hBgBrush = CreateSolidBrush(RGB(0x16, 0x16, 0x18));
            ctx->hPanelBrush = CreateSolidBrush(RGB(0x1E, 0x1E, 0x22));
            ctx->hEditBrush = CreateSolidBrush(RGB(0x20, 0x20, 0x26));

            RECT rcClient{};
            GetClientRect(hwnd, &rcClient);
            int w = rcClient.right - rcClient.left;
            int h = rcClient.bottom - rcClient.top;

            int left_w = DesignSystem::ScaleForDpi(330, ctx->dpi);
            int margin = DesignSystem::ScaleForDpi(14, ctx->dpi);
            int ctrl_h = DesignSystem::ScaleForDpi(26, ctx->dpi);
            int combo_h = DesignSystem::ScaleForDpi(200, ctx->dpi);

            // --- LEFT PANE CONTROLS ---
            int cur_y = DesignSystem::ScaleForDpi(18, ctx->dpi);

            // Torrent Management Mode
            cur_y += DesignSystem::ScaleForDpi(20, ctx->dpi);
            ctx->hModeCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), combo_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorModeCombo)), hInst, nullptr);
            SendMessageW(ctx->hModeCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Manual"));
            SendMessageW(ctx->hModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic"));
            SendMessageW(ctx->hModeCombo, CB_SETCURSEL, 0, 0);

            // Save at
            cur_y += DesignSystem::ScaleForDpi(46, ctx->dpi);
            int edit_w = left_w - DesignSystem::ScaleForDpi(62, ctx->dpi);
            ctx->hSaveDirEdit = CreateWindowExW(0, L"EDIT", Utf8ToW(ctx->target_save_dir).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, edit_w, ctrl_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSaveDirEdit)), hInst, nullptr);
            SendMessageW(ctx->hSaveDirEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernEditControl(ctx->hSaveDirEdit, ctx->dpi);

            ctx->hBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(14, ctx->dpi) + edit_w, cur_y, DesignSystem::ScaleForDpi(28, ctx->dpi), ctrl_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorBrowseBtn)), hInst, nullptr);

            cur_y += DesignSystem::ScaleForDpi(32, ctx->dpi);
            ctx->hIncompleteChk = CreateWindowExW(0, L"BUTTON", L"Use another path for incomplete torrent",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorIncompleteChk)), hInst, nullptr);
            SendMessageW(ctx->hIncompleteChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            cur_y += DesignSystem::ScaleForDpi(24, ctx->dpi);
            ctx->hRememberPathChk = CreateWindowExW(0, L"BUTTON", L"Remember last used save path",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorRememberPathChk)), hInst, nullptr);
            SendMessageW(ctx->hRememberPathChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            SendMessageW(ctx->hRememberPathChk, BM_SETCHECK, BST_CHECKED, 0);

            // Category & Tags
            cur_y += DesignSystem::ScaleForDpi(40, ctx->dpi);
            ctx->hCategoryCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), combo_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorCategoryCombo)), hInst, nullptr);
            SendMessageW(ctx->hCategoryCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(None)"));
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Torrents"));
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Video"));
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Music"));
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Software"));
            SendMessageW(ctx->hCategoryCombo, CB_SETCURSEL, 1, 0);

            cur_y += DesignSystem::ScaleForDpi(30, ctx->dpi);
            ctx->hDefaultCatChk = CreateWindowExW(0, L"BUTTON", L"Set as default category",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorDefaultCatChk)), hInst, nullptr);
            SendMessageW(ctx->hDefaultCatChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            cur_y += DesignSystem::ScaleForDpi(40, ctx->dpi);
            ctx->hTagsEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), ctrl_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorTagsEdit)), hInst, nullptr);
            SendMessageW(ctx->hTagsEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernEditControl(ctx->hTagsEdit, ctx->dpi);

            // Torrent Options
            cur_y += DesignSystem::ScaleForDpi(42, ctx->dpi);
            ctx->hStartChk = CreateWindowExW(0, L"BUTTON", L"Start torrent",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorStartChk)), hInst, nullptr);
            SendMessageW(ctx->hStartChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
            SendMessageW(ctx->hStartChk, BM_SETCHECK, ctx->start_immediately ? BST_CHECKED : BST_UNCHECKED, 0);

            cur_y += DesignSystem::ScaleForDpi(36, ctx->dpi);
            ctx->hStopCondCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), combo_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorStopCondCombo)), hInst, nullptr);
            SendMessageW(ctx->hStopCondCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hStopCondCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
            SendMessageW(ctx->hStopCondCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Metadata received"));
            SendMessageW(ctx->hStopCondCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Files checked"));
            SendMessageW(ctx->hStopCondCombo, CB_SETCURSEL, 0, 0);

            cur_y += DesignSystem::ScaleForDpi(28, ctx->dpi);
            ctx->hQueueTopChk = CreateWindowExW(0, L"BUTTON", L"Add to top of queue",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorQueueTopChk)), hInst, nullptr);
            SendMessageW(ctx->hQueueTopChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            cur_y += DesignSystem::ScaleForDpi(22, ctx->dpi);
            ctx->hSkipHashChk = CreateWindowExW(0, L"BUTTON", L"Skip hash check",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSkipHashChk)), hInst, nullptr);
            SendMessageW(ctx->hSkipHashChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            cur_y += DesignSystem::ScaleForDpi(22, ctx->dpi);
            ctx->hSequentialChk = CreateWindowExW(0, L"BUTTON", L"Download in sequential order",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSequentialChk)), hInst, nullptr);
            SendMessageW(ctx->hSequentialChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            cur_y += DesignSystem::ScaleForDpi(22, ctx->dpi);
            ctx->hFirstLastChk = CreateWindowExW(0, L"BUTTON", L"Download first and last pieces first",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorFirstLastChk)), hInst, nullptr);
            SendMessageW(ctx->hFirstLastChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            cur_y += DesignSystem::ScaleForDpi(36, ctx->dpi);
            ctx->hLayoutCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), cur_y, left_w - DesignSystem::ScaleForDpi(20, ctx->dpi), combo_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorLayoutCombo)), hInst, nullptr);
            SendMessageW(ctx->hLayoutCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hLayoutCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Original"));
            SendMessageW(ctx->hLayoutCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Create subfolder"));
            SendMessageW(ctx->hLayoutCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Don't create subfolder"));
            SendMessageW(ctx->hLayoutCombo, CB_SETCURSEL, 0, 0);

            // Bottom left
            int bot_y = h - DesignSystem::ScaleForDpi(50, ctx->dpi);
            ctx->hNeverShowChk = CreateWindowExW(0, L"BUTTON", L"Never show again",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(10, ctx->dpi), bot_y + DesignSystem::ScaleForDpi(4, ctx->dpi), DesignSystem::ScaleForDpi(140, ctx->dpi), DesignSystem::ScaleForDpi(22, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorNeverShowChk)), hInst, nullptr);
            SendMessageW(ctx->hNeverShowChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            ctx->hSaveTorrentBtn = CreateWindowExW(0, L"BUTTON", L"Save .torrent...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(160, ctx->dpi), bot_y, DesignSystem::ScaleForDpi(130, ctx->dpi), DesignSystem::ScaleForDpi(30, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSaveTorrentBtn)), hInst, nullptr);

            // --- RIGHT PANE CONTROLS ---
            int right_x = margin + left_w + DesignSystem::ScaleForDpi(12, ctx->dpi);
            int right_w = w - right_x - margin;

            // Toolbar
            int r_top = DesignSystem::ScaleForDpi(14, ctx->dpi);
            ctx->hSelectAllBtn = CreateWindowExW(0, L"BUTTON", L"Select All",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                right_x, r_top, DesignSystem::ScaleForDpi(90, ctx->dpi), DesignSystem::ScaleForDpi(28, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSelectAllBtn)), hInst, nullptr);

            ctx->hSelectNoneBtn = CreateWindowExW(0, L"BUTTON", L"Select None",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                right_x + DesignSystem::ScaleForDpi(98, ctx->dpi), r_top, DesignSystem::ScaleForDpi(90, ctx->dpi), DesignSystem::ScaleForDpi(28, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSelectNoneBtn)), hInst, nullptr);

            int filter_x = right_x + DesignSystem::ScaleForDpi(200, ctx->dpi);
            int filter_w = right_w - DesignSystem::ScaleForDpi(200, ctx->dpi);
            ctx->hFilterEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                filter_x, r_top, filter_w, DesignSystem::ScaleForDpi(28, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorFilterEdit)), hInst, nullptr);
            SendMessageW(ctx->hFilterEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernEditControl(ctx->hFilterEdit, ctx->dpi);

            // File list
            int list_top = r_top + DesignSystem::ScaleForDpi(62, ctx->dpi);
            int list_h = h - list_top - DesignSystem::ScaleForDpi(80, ctx->dpi);
            ctx->hFileList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_NODATA | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                right_x, list_top, right_w, list_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorFileList)), hInst, nullptr);
            SendMessageW(ctx->hFileList, LB_SETITEMHEIGHT, 0, DesignSystem::ScaleForDpi(28, ctx->dpi));

            // Summary
            int sum_y = list_top + list_h + DesignSystem::ScaleForDpi(8, ctx->dpi);
            ctx->hSummaryStatic = CreateWindowExW(0, L"STATIC", ctx->GetSummaryText().c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                right_x, sum_y, right_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, nullptr, hInst, nullptr);
            SendMessageW(ctx->hSummaryStatic, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

            // Bottom action buttons
            int btn_h = DesignSystem::ScaleForDpi(34, ctx->dpi);
            int ok_w = DesignSystem::ScaleForDpi(130, ctx->dpi);
            int can_w = DesignSystem::ScaleForDpi(100, ctx->dpi);
            int can_x = right_x + right_w - can_w;
            int ok_x = can_x - ok_w - DesignSystem::ScaleForDpi(10, ctx->dpi);
            int action_y = h - DesignSystem::ScaleForDpi(46, ctx->dpi);

            ctx->hOkBtn = CreateWindowExW(0, L"BUTTON", L"Add Torrent",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                ok_x, action_y, ok_w, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorOkBtn)), hInst, nullptr);

            ctx->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                can_x, action_y, can_w, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorCancelBtn)), hInst, nullptr);

            ctx->BuildTree();
            return 0;
        }

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND hCtl = reinterpret_cast<HWND>(lParam);
            if (hCtl == ctx->hModeCombo || hCtl == ctx->hCategoryCombo ||
                hCtl == ctx->hStopCondCombo || hCtl == ctx->hLayoutCombo) {
                SetBkMode(hdc, OPAQUE);
                SetBkColor(hdc, RGB(0x20, 0x20, 0x26));
                SetTextColor(hdc, RGB(0xF2, 0xF2, 0xF3));
                return reinterpret_cast<INT_PTR>(ctx->hEditBrush);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0xDE, 0xDF, 0xE4));
            return reinterpret_cast<INT_PTR>(ctx->hBgBrush);
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(0x20, 0x20, 0x26));
            SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
            return reinterpret_cast<INT_PTR>(ctx->hEditBrush);
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(0x18, 0x18, 0x1C));
            SetTextColor(hdc, RGB(0xEE, 0xEE, 0xF2));
            return reinterpret_cast<INT_PTR>(ctx->hPanelBrush);
        }

        case WM_SIZE: {
            if (!ctx || !ctx->hFileList) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;

            int left_w = DesignSystem::ScaleForDpi(330, dpi);
            int margin = DesignSystem::ScaleForDpi(14, dpi);
            int right_x = margin + left_w + margin;
            int right_w = w - right_x - margin;
            if (right_w < DesignSystem::ScaleForDpi(200, dpi)) right_w = DesignSystem::ScaleForDpi(200, dpi);

            int r_top = DesignSystem::ScaleForDpi(18, dpi);
            int filter_x = right_x + DesignSystem::ScaleForDpi(200, dpi);
            int filter_w = std::max(DesignSystem::ScaleForDpi(100, dpi), right_w - DesignSystem::ScaleForDpi(200, dpi));
            SetWindowPos(ctx->hSelectAllBtn, nullptr, right_x, r_top, DesignSystem::ScaleForDpi(90, dpi), DesignSystem::ScaleForDpi(28, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hSelectNoneBtn, nullptr, right_x + DesignSystem::ScaleForDpi(98, dpi), r_top, DesignSystem::ScaleForDpi(90, dpi), DesignSystem::ScaleForDpi(28, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hFilterEdit, nullptr, filter_x, r_top, filter_w, DesignSystem::ScaleForDpi(28, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            int list_top = r_top + DesignSystem::ScaleForDpi(62, dpi);
            int list_h = std::max(DesignSystem::ScaleForDpi(100, dpi), h - list_top - DesignSystem::ScaleForDpi(80, dpi));
            SetWindowPos(ctx->hFileList, nullptr, right_x, list_top, right_w, list_h, SWP_NOZORDER | SWP_NOACTIVATE);

            int sum_y = list_top + list_h + DesignSystem::ScaleForDpi(8, dpi);
            SetWindowPos(ctx->hSummaryStatic, nullptr, right_x, sum_y, right_w, DesignSystem::ScaleForDpi(20, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            int btn_h = DesignSystem::ScaleForDpi(34, dpi);
            int ok_w = DesignSystem::ScaleForDpi(130, dpi);
            int can_w = DesignSystem::ScaleForDpi(100, dpi);
            int can_x = right_x + right_w - can_w;
            int ok_x = can_x - ok_w - DesignSystem::ScaleForDpi(10, dpi);
            int action_y = h - DesignSystem::ScaleForDpi(46, dpi);
            SetWindowPos(ctx->hCancelBtn, nullptr, can_x, action_y, can_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hOkBtn, nullptr, ok_x, action_y, ok_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);

            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mi->CtlID == kAddTorFileList) {
                mi->itemHeight = DesignSystem::ScaleForDpi(28, ctx->dpi);
                return TRUE;
            }
            return FALSE;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rcClient{};
            GetClientRect(hwnd, &rcClient);
            FillRect(hdc, &rcClient, ctx->hBgBrush);

            const auto& pal = ctx->view ? ctx->view->GetPalette() : DesignSystem::GetRedesignPalette();
            int left_w = DesignSystem::ScaleForDpi(330, ctx->dpi);
            int margin = DesignSystem::ScaleForDpi(14, ctx->dpi);

            // Left pane card
            RECT left_card{margin, margin, margin + left_w, rcClient.bottom - margin};
            DrawRoundedBoxGdi(hdc, left_card, DesignSystem::ScaleForDpi(10, ctx->dpi),
                              pal.surface_primary, pal.border_subtle, 1);
            RECT accent_rule{left_card.left + DesignSystem::ScaleForDpi(12, ctx->dpi), left_card.top,
                             left_card.right - DesignSystem::ScaleForDpi(12, ctx->dpi), left_card.top + DesignSystem::ScaleForDpi(2, ctx->dpi)};
            FillRectSolid(hdc, accent_rule, pal.accent_primary);

            // Group labels on left pane
            SelectObject(hdc, ctx->hFontBold);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());

            TextOutW(hdc, margin + DesignSystem::ScaleForDpi(10, ctx->dpi), DesignSystem::ScaleForDpi(8, ctx->dpi), L"Torrent Management Mode:", 24);
            // Labels are placed one full text line above controls so GDI text metrics and
            // the native edit/combo borders cannot collide at non-100% DPI.
            TextOutW(hdc, margin + DesignSystem::ScaleForDpi(10, ctx->dpi), DesignSystem::ScaleForDpi(48, ctx->dpi), L"Save at:", 8);
            TextOutW(hdc, margin + DesignSystem::ScaleForDpi(10, ctx->dpi), DesignSystem::ScaleForDpi(144, ctx->dpi), L"Category:", 9);
            TextOutW(hdc, margin + DesignSystem::ScaleForDpi(10, ctx->dpi), DesignSystem::ScaleForDpi(214, ctx->dpi), L"Tags:", 5);
            TextOutW(hdc, margin + DesignSystem::ScaleForDpi(10, ctx->dpi), DesignSystem::ScaleForDpi(256, ctx->dpi), L"Torrent Options:", 16);
            TextOutW(hdc, margin + DesignSystem::ScaleForDpi(10, ctx->dpi), DesignSystem::ScaleForDpi(292, ctx->dpi), L"Stop condition:", 15);
            TextOutW(hdc, margin + DesignSystem::ScaleForDpi(10, ctx->dpi), DesignSystem::ScaleForDpi(422, ctx->dpi), L"Content layout:", 15);

            // Header for file list on right pane
            int right_x = margin + left_w + DesignSystem::ScaleForDpi(12, ctx->dpi);
            int right_w = rcClient.right - right_x - margin;
            int hdr_y = DesignSystem::ScaleForDpi(50, ctx->dpi);
            int hdr_h = DesignSystem::ScaleForDpi(24, ctx->dpi);
            RECT hdr_rc{right_x, hdr_y, right_x + right_w, hdr_y + hdr_h};
            DrawRoundedBoxGdi(hdc, hdr_rc, DesignSystem::ScaleForDpi(4, ctx->dpi),
                              pal.surface_elevated, pal.border_subtle, 1);
            RECT hdr_accent{hdr_rc.left, hdr_rc.top, hdr_rc.left + DesignSystem::ScaleForDpi(40, ctx->dpi), hdr_rc.top + DesignSystem::ScaleForDpi(2, ctx->dpi)};
            FillRectSolid(hdc, hdr_accent, pal.accent_primary);

            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, RGB(0x9E, 0xA2, 0xB0));
            TextOutW(hdc, right_x + DesignSystem::ScaleForDpi(38, ctx->dpi), hdr_y + DesignSystem::ScaleForDpi(4, ctx->dpi), L"Name", 4);

            int size_col_w = DesignSystem::ScaleForDpi(110, ctx->dpi);
            int prio_col_w = DesignSystem::ScaleForDpi(90, ctx->dpi);
            int size_x = right_x + right_w - size_col_w - prio_col_w;
            int prio_x = right_x + right_w - prio_col_w;

            TextOutW(hdc, size_x, hdr_y + DesignSystem::ScaleForDpi(4, ctx->dpi), L"Size", 4);
            TextOutW(hdc, prio_x, hdr_y + DesignSystem::ScaleForDpi(4, ctx->dpi), L"Priority", 8);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;

            if (dis->CtlID == kAddTorFileList) {
                if (dis->itemID == static_cast<UINT>(-1) || dis->itemID >= ctx->visible_node_indices.size()) return FALSE;
                int node_id = ctx->visible_node_indices[dis->itemID];
                const auto& node = ctx->all_nodes[node_id];

                HDC hdcItem = dis->hDC;
                RECT rc = dis->rcItem;

                const ThemePalette pal = ctx->view ? ctx->view->GetPalette() : DesignSystem::GetRedesignPalette();
                bool is_selected = (dis->itemState & ODS_SELECTED) != 0;
                ColorRgba row_bg = is_selected ? pal.surface_selected : ((dis->itemID % 2 == 0) ? ColorRgba{0x18, 0x18, 0x1C, 255} : ColorRgba{0x1C, 0x1C, 0x21, 255});
                HBRUSH brBg = CreateSolidBrush(row_bg.ToWin32ColorRef());
                FillRect(hdcItem, &rc, brBg);
                DeleteObject(brBg);
                if (is_selected) {
                    RECT accent_bar{rc.left, rc.top, rc.left + DesignSystem::ScaleForDpi(3, ctx->dpi), rc.bottom};
                    FillRectSolid(hdcItem, accent_bar, pal.accent_primary);
                }

                int indent = node.depth * DesignSystem::ScaleForDpi(18, ctx->dpi) + DesignSystem::ScaleForDpi(8, ctx->dpi);

                // Caret expand/collapse glyph if folder
                if (node.is_dir) {
                    RECT caret_rc{rc.left + indent - DesignSystem::ScaleForDpi(14, ctx->dpi), rc.top + (rc.bottom - rc.top) / 2 - 4,
                                  rc.left + indent - DesignSystem::ScaleForDpi(6, ctx->dpi), rc.top + (rc.bottom - rc.top) / 2 + 4};
                    int cx = (caret_rc.left + caret_rc.right) / 2;
                    int cy = (caret_rc.top + caret_rc.bottom) / 2;
                    DrawCaretGlyphGdi(hdcItem, cx, cy, ColorRgba{0x8E, 0x92, 0xA0, 255}, !node.expanded);
                }

                // Checkbox
                RECT chk_rc{rc.left + indent, rc.top + (rc.bottom - rc.top) / 2 - DesignSystem::ScaleForDpi(7, ctx->dpi),
                            rc.left + indent + DesignSystem::ScaleForDpi(14, ctx->dpi), rc.top + (rc.bottom - rc.top) / 2 + DesignSystem::ScaleForDpi(7, ctx->dpi)};

                if (node.check_state == 1) {
                    DrawCheckboxGdi(hdcItem, chk_rc, true, pal.accent_primary, pal.accent_primary, ColorRgba{255, 255, 255, 255});
                } else if (node.check_state == 2) { // Partial
                    DrawRoundedBoxGdi(hdcItem, chk_rc, 4, ColorRgba{pal.accent_primary.r, pal.accent_primary.g, pal.accent_primary.b, 180}, pal.accent_primary, 1);
                    RECT bar{chk_rc.left + 3, chk_rc.top + (chk_rc.bottom - chk_rc.top) / 2 - 1, chk_rc.right - 3, chk_rc.top + (chk_rc.bottom - chk_rc.top) / 2 + 1};
                    FillRectSolid(hdcItem, bar, ColorRgba{255, 255, 255, 255});
                } else {
                    DrawCheckboxGdi(hdcItem, chk_rc, false, ColorRgba{0x22, 0x22, 0x28, 255}, ColorRgba{0x55, 0x58, 0x66, 255}, ColorRgba{255, 255, 255, 255});
                }

                // Icon
                int icon_x = rc.left + indent + DesignSystem::ScaleForDpi(20, ctx->dpi);
                int icon_y = rc.top + (rc.bottom - rc.top) / 2 - DesignSystem::ScaleForDpi(8, ctx->dpi);
                int icon_sz = DesignSystem::ScaleForDpi(16, ctx->dpi);

                if (node.is_dir) {
                    // Golden yellow folder icon
                    RECT f_top{icon_x, icon_y + 1, icon_x + icon_sz / 2, icon_y + 4};
                    FillRectSolid(hdcItem, f_top, ColorRgba{0xF5, 0xB0, 0x41, 255});
                    RECT f_body{icon_x, icon_y + 3, icon_x + icon_sz, icon_y + icon_sz};
                    DrawRoundedBoxGdi(hdcItem, f_body, 3, ColorRgba{0xFA, 0xCA, 0x64, 255}, ColorRgba{0xE6, 0x9B, 0x20, 255}, 1);
                } else {
                    // Colorful file type icon
                    FormattedTaskRow dummy_row;
                    dummy_row.display_filename = WToUtf8(node.name);
                    DrawFileTypeIconGdi(hdcItem, icon_x, icon_y, icon_sz, icon_sz, dummy_row, ctx->view ? ctx->view->GetPalette() : DesignSystem::GetRedesignPalette());
                }

                // File name
                int name_x = icon_x + icon_sz + DesignSystem::ScaleForDpi(8, ctx->dpi);
                int size_col_w = DesignSystem::ScaleForDpi(110, ctx->dpi);
                int prio_col_w = DesignSystem::ScaleForDpi(90, ctx->dpi);
                int size_x = rc.right - size_col_w - prio_col_w;
                int prio_x = rc.right - prio_col_w;

                RECT name_rc{name_x, rc.top, size_x - DesignSystem::ScaleForDpi(6, ctx->dpi), rc.bottom};
                SelectObject(hdcItem, node.is_dir ? ctx->hFontBold : ctx->hFontBody);
                SetBkMode(hdcItem, TRANSPARENT);
                SetTextColor(hdcItem, is_selected ? RGB(255, 255, 255) : (node.is_dir ? RGB(0xFD, 0xFD, 0xFE) : RGB(0xDE, 0xDF, 0xE4)));
                DrawTextW(hdcItem, node.name.c_str(), -1, &name_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // Size
                RECT sz_rc{size_x, rc.top, prio_x - DesignSystem::ScaleForDpi(6, ctx->dpi), rc.bottom};
                SelectObject(hdcItem, ctx->hFontSmall);
                SetTextColor(hdcItem, RGB(0x9E, 0xA2, 0xB0));
                std::wstring sz_txt = Utf8ToW(FormatAddTorrentBytes(node.size));
                DrawTextW(hdcItem, sz_txt.c_str(), -1, &sz_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // Priority
                RECT prio_rc{prio_x, rc.top, rc.right - DesignSystem::ScaleForDpi(6, ctx->dpi), rc.bottom};
                DrawTextW(hdcItem, node.is_dir ? L"" : L"Normal", -1, &prio_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                return TRUE;
            }

            // Buttons
            HDC hdcBtn = dis->hDC;
            RECT rcBtn = dis->rcItem;
            bool is_selected = (dis->itemState & ODS_SELECTED) != 0;
            bool is_focused  = (dis->itemState & ODS_FOCUS) != 0;
            FillRect(hdcBtn, &rcBtn, ctx->hBgBrush);

            if (dis->CtlID == kAddTorOkBtn) { // Primary Coral Red
                const auto& accent = ctx->view->GetPalette().accent_primary;
                ColorRgba bg_col = is_selected ? accent.Interpolate(ColorRgba{0, 0, 0, 255}, 0.15f) : (is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.12f) : accent);
                DrawRoundedBoxGdi(hdcBtn, rcBtn, DesignSystem::ScaleForDpi(8, ctx->dpi), bg_col, bg_col, 0);
                SelectObject(hdcBtn, ctx->hFontBold);
                SetTextColor(hdcBtn, RGB(255, 255, 255));
                SetBkMode(hdcBtn, TRANSPARENT);
                DrawTextW(hdcBtn, L"Add Torrent", -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == kAddTorCancelBtn || dis->CtlID == kAddTorBrowseBtn ||
                       dis->CtlID == kAddTorSelectAllBtn || dis->CtlID == kAddTorSelectNoneBtn || dis->CtlID == kAddTorSaveTorrentBtn) {
                ColorRgba bg_col = is_selected ? ColorRgba{0x33, 0x33, 0x3A, 255} : (is_focused ? ColorRgba{0x2C, 0x2C, 0x34, 255} : ColorRgba{0x22, 0x22, 0x28, 255});
                ColorRgba border_col = is_focused ? ColorRgba{0x55, 0x58, 0x66, 220} : ColorRgba{0x3E, 0x40, 0x4A, 180};
                DrawRoundedBoxGdi(hdcBtn, rcBtn, DesignSystem::ScaleForDpi(6, ctx->dpi), bg_col, border_col, 1);
                SelectObject(hdcBtn, ctx->hFontBody);
                SetTextColor(hdcBtn, RGB(0xDE, 0xDF, 0xE4));
                SetBkMode(hdcBtn, TRANSPARENT);

                const wchar_t* label = L"";
                if (dis->CtlID == kAddTorCancelBtn) label = L"Cancel";
                else if (dis->CtlID == kAddTorBrowseBtn) label = L"...";
                else if (dis->CtlID == kAddTorSelectAllBtn) label = L"Select All";
                else if (dis->CtlID == kAddTorSelectNoneBtn) label = L"Select None";
                else if (dis->CtlID == kAddTorSaveTorrentBtn) label = L"Save .torrent...";

                DrawTextW(hdcBtn, label, -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == kAddTorBrowseBtn && code == BN_CLICKED) {
                std::string chosen = ctx->target_save_dir;
                if (SelectFolderWithDialogWin32(hwnd, chosen)) {
                    ctx->target_save_dir = chosen;
                    SetWindowTextW(ctx->hSaveDirEdit, Utf8ToW(chosen).c_str());
                }
                return 0;
            } else if (id == kAddTorSaveTorrentBtn && code == BN_CLICKED) {
                std::filesystem::path source(ctx->torrent_file_path);
                if (source.empty() || !std::filesystem::is_regular_file(source)) {
                    MessageBoxW(hwnd, L"The source torrent file is no longer available.", L"Save Torrent", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                std::wstring source_name = source.filename().wstring();
                std::array<wchar_t, MAX_PATH> destination{};
                const size_t copy_len = std::min(source_name.size(), destination.size() - 1);
                std::copy_n(source_name.data(), copy_len, destination.data());
                OPENFILENAMEW dialog{};
                dialog.lStructSize = sizeof(dialog);
                dialog.hwndOwner = hwnd;
                dialog.lpstrFile = destination.data();
                dialog.nMaxFile = static_cast<DWORD>(destination.size());
                dialog.lpstrFilter = L"BitTorrent files (*.torrent)\0*.torrent\0All files (*.*)\0*.*\0\0";
                dialog.nFilterIndex = 1;
                dialog.lpstrDefExt = L"torrent";
                dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
                if (GetSaveFileNameW(&dialog) && !CopyFileW(source.c_str(), destination.data(), FALSE)) {
                    DWORD error = GetLastError();
                    std::wstring message = L"Could not save the torrent file (Windows error " + std::to_wstring(error) + L").";
                    MessageBoxW(hwnd, message.c_str(), L"Save Torrent", MB_OK | MB_ICONERROR);
                }
                return 0;
            } else if (id == kAddTorSelectAllBtn && code == BN_CLICKED) {
                if (!ctx->all_nodes.empty()) {
                    ctx->SetNodeChecked(0, true);
                    ctx->UpdateCheckStatesFromLeaves();
                    ctx->SyncToFileInfo();
                    ctx->RebuildVisibleList();
                    SetWindowTextW(ctx->hSummaryStatic, ctx->GetSummaryText().c_str());
                }
                return 0;
            } else if (id == kAddTorSelectNoneBtn && code == BN_CLICKED) {
                if (!ctx->all_nodes.empty()) {
                    ctx->SetNodeChecked(0, false);
                    ctx->UpdateCheckStatesFromLeaves();
                    ctx->SyncToFileInfo();
                    ctx->RebuildVisibleList();
                    SetWindowTextW(ctx->hSummaryStatic, ctx->GetSummaryText().c_str());
                }
                return 0;
            } else if (id == kAddTorFilterEdit && code == EN_CHANGE) {
                wchar_t buf[256]{};
                GetWindowTextW(ctx->hFilterEdit, buf, 255);
                ctx->filter_text = WToUtf8(buf);
                ctx->RebuildVisibleList();
                return 0;
            } else if (id == kAddTorFileList && code == LBN_SELCHANGE) {
                LRESULT cur = SendMessageW(ctx->hFileList, LB_GETCURSEL, 0, 0);
                if (cur >= 0 && static_cast<size_t>(cur) < ctx->visible_node_indices.size()) {
                    int node_id = ctx->visible_node_indices[static_cast<size_t>(cur)];
                    ctx->ToggleNode(node_id);
                    SetWindowTextW(ctx->hSummaryStatic, ctx->GetSummaryText().c_str());
                }
                return 0;
            } else if (id == kAddTorFileList && code == LBN_DBLCLK) {
                LRESULT cur = SendMessageW(ctx->hFileList, LB_GETCURSEL, 0, 0);
                if (cur >= 0 && static_cast<size_t>(cur) < ctx->visible_node_indices.size()) {
                    int node_id = ctx->visible_node_indices[static_cast<size_t>(cur)];
                    if (ctx->all_nodes[node_id].is_dir) {
                        ctx->all_nodes[node_id].expanded = !ctx->all_nodes[node_id].expanded;
                        ctx->RebuildVisibleList();
                    }
                }
                return 0;
            } else if ((id == kAddTorOkBtn || id == IDOK) && (code == BN_CLICKED || code == 0 || code == 1)) {
                // Ensure at least one file is selected
                bool any_selected = false;
                if (ctx->info) {
                    for (const auto& f : ctx->info->files) {
                        if (f.selected) { any_selected = true; break; }
                    }
                }
                if (!any_selected) {
                    MessageBoxW(hwnd, L"Select at least one file to continue download.", L"No Files Selected", MB_OK | MB_ICONWARNING);
                    return 0;
                }

                // Read save dir
                wchar_t dir_buf[MAX_PATH]{};
                GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                std::string sd = WToUtf8(dir_buf);
                if (!sd.empty()) ctx->target_save_dir = sd;

                ctx->start_immediately = (SendMessageW(ctx->hStartChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->add_to_queue_top = (SendMessageW(ctx->hQueueTopChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->skip_hash_check = (SendMessageW(ctx->hSkipHashChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->sequential_download = (SendMessageW(ctx->hSequentialChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->first_last_piece_priority = (SendMessageW(ctx->hFirstLastChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->remember_save_path = (SendMessageW(ctx->hRememberPathChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->use_incomplete_path = (SendMessageW(ctx->hIncompleteChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->never_show_again = (SendMessageW(ctx->hNeverShowChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                wchar_t category_buf[64]{};
                const LRESULT category_index = SendMessageW(ctx->hCategoryCombo, CB_GETCURSEL, 0, 0);
                if (category_index >= 0) {
                    SendMessageW(ctx->hCategoryCombo, CB_GETLBTEXT, category_index, reinterpret_cast<LPARAM>(category_buf));
                    ctx->selected_category = WToUtf8(category_buf);
                    if (ctx->selected_category == "(None)") ctx->selected_category = "General";
                }

                ctx->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            } else if ((id == kAddTorCancelBtn || id == IDCANCEL) && (code == BN_CLICKED || code == 0 || code == 1)) {
                ctx->accepted = false;
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            ctx->accepted = false;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontSmall) DeleteObject(ctx->hFontSmall);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            if (ctx->hPanelBrush) DeleteObject(ctx->hPanelBrush);
            if (ctx->hEditBrush) DeleteObject(ctx->hEditBrush);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowAddTorrentDialogWin32(
    HWND hwnd_parent,
    DesktopView& view,
    DownloadController& controller,
    torrent::TorrentInfo& info,
    const std::string& torrent_file_path,
    const std::string& initial_save_dir,
    bool start_immediately,
    std::string* selected_save_dir,
    bool* selected_start_immediately,
    std::string* selected_category) {

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = AddTorrentDialogProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hAppIcon = CreateRadiumAppIcon();
        wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"Radium_AddTorrentDlgClass";
        RegisterClassExW(&wc);
        class_registered = true;
    }

    uint32_t dpi = view.GetMetrics().dpi;
    if (dpi == 0) dpi = 96;

    std::string s_dir = initial_save_dir;
    if (s_dir.empty()) {
        s_dir = controller.GetSettings().default_download_dir;
    }

    AddTorrentDialogContext ctx;
    ctx.owner = hwnd_parent;
    ctx.view = &view;
    ctx.controller = &controller;
    ctx.info = &info;
    ctx.torrent_file_path = torrent_file_path;
    ctx.target_save_dir = s_dir;
    ctx.start_immediately = start_immediately;
    ctx.dpi = dpi;

    int dlg_w = DesignSystem::ScaleForDpi(1040, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(660, dpi);

    RECT work_rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rc, 0);
    int screen_w = work_rc.right - work_rc.left;
    int screen_h = work_rc.bottom - work_rc.top;
    if (screen_w <= 0) screen_w = GetSystemMetrics(SM_CXSCREEN);
    if (screen_h <= 0) screen_h = GetSystemMetrics(SM_CYSCREEN);

    int dlg_x = work_rc.left + (screen_w - dlg_w) / 2;
    int dlg_y = work_rc.top + (screen_h - dlg_h) / 2;

    HWND hDlg = CreateWindowExW(
        (hwnd_parent ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT,
        L"Radium_AddTorrentDlgClass",
        Utf8ToW("Add Torrent \u2014 " + (info.name.empty() ? "Radium" : info.name)).c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        hwnd_parent, nullptr, hInst, &ctx);

    if (!hDlg) return false;
    ctx.hDlg = hDlg;

    ApplyWindowGlassBackdropWin32(hDlg, true);

    if (hwnd_parent && IsWindow(hwnd_parent)) {
        EnableWindow(hwnd_parent, FALSE);
    }

    ShowWindow(hDlg, SW_SHOWNORMAL);
    BringWindowToTop(hDlg);
    SetForegroundWindow(hDlg);
    SetActiveWindow(hDlg);
    SetFocus(ctx.hFilterEdit);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(kAddTorCancelBtn, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hCancelBtn));
                break;
            } else if (msg.wParam == VK_RETURN && GetFocus() != ctx.hBrowseBtn && GetFocus() != ctx.hCancelBtn) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(kAddTorOkBtn, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hOkBtn));
                if (!IsWindow(hDlg)) break;
                continue;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    if (hwnd_parent && IsWindow(hwnd_parent)) {
        EnableWindow(hwnd_parent, TRUE);
        SetForegroundWindow(hwnd_parent);
        InvalidateRect(hwnd_parent, nullptr, TRUE);
    }

    if (ctx.accepted) {
        if (selected_save_dir) *selected_save_dir = ctx.target_save_dir;
        if (selected_start_immediately) *selected_start_immediately = ctx.start_immediately;
        if (selected_category) *selected_category = ctx.selected_category;
    }

    return ctx.accepted;
}


void PaintDetailsPanelWin32(HDC hdc, DesktopView& view, const RectI& rect, HFONT hFontBody, HFONT hFontBold, HFONT hFontSmall) {
    (void)hFontBody;
    if (rect.height() <= 0) return;
    const auto& pal = view.GetPalette();
    const auto& m = view.GetMetrics();
    auto details = view.BuildSelectedTaskDetails();
    if (!details.has_selection) return;

    RECT det_rc = ToWinRect(rect);

    // Floating rounded glass card with subtle border
    int margin_x = DesignSystem::ScaleForDpi(16, m.dpi);
    int margin_y = DesignSystem::ScaleForDpi(6, m.dpi);
    RECT card_rc{det_rc.left + margin_x, det_rc.top + margin_y, det_rc.right - margin_x, det_rc.bottom - margin_y};
    int card_radius = DesignSystem::ScaleForDpi(10, m.dpi);
    DrawGlassPanelGdi(hdc, card_rc, card_radius, pal.glass_surface, pal.glass_border, 1, false);

    // Left Icon Box
    int icon_box_w = DesignSystem::ScaleForDpi(42, m.dpi);
    int icon_box_h = DesignSystem::ScaleForDpi(46, m.dpi);
    int icon_box_x = card_rc.left + DesignSystem::ScaleForDpi(14, m.dpi);
    int icon_box_y = card_rc.top + (card_rc.bottom - card_rc.top - icon_box_h) / 2;
    RECT icon_box_rc{icon_box_x, icon_box_y, icon_box_x + icon_box_w, icon_box_y + icon_box_h};
    DrawRoundedBoxGdi(hdc, icon_box_rc, DesignSystem::ScaleForDpi(8, m.dpi), pal.surface_elevated, pal.border_subtle, 1);

    // Media / File Icon inside icon box
    FormattedTaskRow dummy_row{};
    dummy_row.full_filename_tooltip = details.file_name;
    dummy_row.is_stream = !details.stream_format.empty();
    int icon_w = DesignSystem::ScaleForDpi(20, m.dpi);
    int icon_h = DesignSystem::ScaleForDpi(24, m.dpi);
    int icon_x = icon_box_rc.left + (icon_box_w - icon_w) / 2;
    int icon_y = icon_box_rc.top + (icon_box_h - icon_h) / 2;
    DrawFileTypeIconGdi(hdc, icon_x, icon_y, icon_w, icon_h, dummy_row, pal);

    // Text content bounds
    int text_left = icon_box_rc.right + DesignSystem::ScaleForDpi(14, m.dpi);
    int text_right = card_rc.right - DesignSystem::ScaleForDpi(16, m.dpi);
    int total_avail_w = text_right - text_left;
    if (total_avail_w <= 50) return;

    int line_h = DesignSystem::ScaleForDpi(24, m.dpi);
    int top_y = card_rc.top + (card_rc.bottom - card_rc.top - line_h * 3) / 2;

    if (total_avail_w >= DesignSystem::ScaleForDpi(480, m.dpi)) {
        // Two-column responsive layout (Matches Image 3)
        int col1_w = total_avail_w / 2 - DesignSystem::ScaleForDpi(12, m.dpi);
        int col2_x = text_left + col1_w + DesignSystem::ScaleForDpi(24, m.dpi);
        int col2_w = text_right - col2_x;

        // Col 1, Row 1: Filename + Engine Badge
        RECT fn_rc{text_left, top_y, text_left + col1_w, top_y + line_h};
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        std::wstring wname = Utf8ToW(details.file_name);
        if (!details.engine_name.empty()) {
            wname += L"  [" + Utf8ToW(details.engine_name) + L"]";
        }
        DrawTextW(hdc, wname.c_str(), -1, &fn_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 1, Row 2: Size | Speed | Progress summary
        RECT stat_rc{text_left, top_y + line_h, text_left + col1_w, top_y + line_h * 2};
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring wstat = Utf8ToW(details.size_summary);
        if (!details.speed_summary.empty()) {
            wstat += L" \u2022 " + Utf8ToW(details.speed_summary);
        }
        if (!details.progress_summary.empty()) {
            wstat += L" \u2022 " + Utf8ToW(details.progress_summary);
        }
        DrawTextW(hdc, wstat.c_str(), -1, &stat_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 1, Row 3: Destination Folder
        RECT dest_rc{text_left, top_y + line_h * 2, text_left + col1_w, top_y + line_h * 3};
        SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
        std::wstring wdest = L"Folder: " + Utf8ToW(details.destination_path);
        DrawTextW(hdc, wdest.c_str(), -1, &dest_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);

        // Col 2, Row 1: Source URL (redacted)
        RECT url_rc{col2_x, top_y, col2_x + col2_w, top_y + line_h};
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring wurl = L"Source: " + Utf8ToW(details.redacted_source_url.empty() ? details.redacted_page_url : details.redacted_source_url);
        DrawTextW(hdc, wurl.c_str(), -1, &url_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 2, Row 2: Timestamps
        RECT time_rc{col2_x, top_y + line_h, col2_x + col2_w, top_y + line_h * 2};
        std::wstring wtime = L"Added: " + Utf8ToW(details.created_time_str);
        if (!details.completed_time_str.empty() && details.completed_time_str != "\u2014") {
            wtime += L" \u2022 Finished: " + Utf8ToW(details.completed_time_str);
        }
        SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
        DrawTextW(hdc, wtime.c_str(), -1, &time_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 2, Row 3: Error explanation or stream details
        RECT err_rc{col2_x, top_y + line_h * 2, col2_x + col2_w, top_y + line_h * 3};
        if (!details.error_explanation.empty()) {
            SetTextColor(hdc, pal.accent_primary.ToWin32ColorRef());
            std::wstring werr = L"Notice: " + Utf8ToW(details.error_explanation);
            DrawTextW(hdc, werr.c_str(), -1, &err_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (!details.stream_format.empty()) {
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            std::wstring wstream = L"Stream: " + Utf8ToW(details.stream_format);
            if (!details.selected_tracks_summary.empty()) {
                wstream += L" (" + Utf8ToW(details.selected_tracks_summary) + L")";
            }
            DrawTextW(hdc, wstream.c_str(), -1, &err_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (!details.speed_limit_summary.empty() && details.speed_limit_summary != "Unlimited") {
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            std::wstring wlimit = L"Speed Limit: " + Utf8ToW(details.speed_limit_summary);
            DrawTextW(hdc, wlimit.c_str(), -1, &err_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        // Compact single-column layout for narrower windows
        RECT fn_rc{text_left, top_y, text_right, top_y + line_h};
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        std::wstring wname = Utf8ToW(details.file_name);
        DrawTextW(hdc, wname.c_str(), -1, &fn_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT stat_rc{text_left, top_y + line_h, text_right, top_y + line_h * 2};
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring wstat = Utf8ToW(details.size_summary);
        if (!details.speed_summary.empty()) wstat += L" \u2022 " + Utf8ToW(details.speed_summary);
        if (!details.progress_summary.empty()) wstat += L" \u2022 " + Utf8ToW(details.progress_summary);
        DrawTextW(hdc, wstat.c_str(), -1, &stat_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT dest_rc{text_left, top_y + line_h * 2, text_right, top_y + line_h * 3};
        SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
        std::wstring wdest = L"Folder: " + Utf8ToW(details.destination_path);
        DrawTextW(hdc, wdest.c_str(), -1, &dest_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
}

void PaintMainWindow(HWND hwnd, DesktopView& view) {
    PAINTSTRUCT ps{};
    HDC hdc_screen = BeginPaint(hwnd, &ps);

    RECT client_rc{};
    GetClientRect(hwnd, &client_rc);
    int w = client_rc.right - client_rc.left;
    int h = client_rc.bottom - client_rc.top;

    HDC hdc = CreateCompatibleDC(hdc_screen);
    HBITMAP hbm = CreateCompatibleBitmap(hdc_screen, std::max(1, w), std::max(1, h));
    HGDIOBJ old_bm = SelectObject(hdc, hbm);

    const auto& pal = view.GetPalette();
    auto geom = view.ComputeLayoutGeometry(view.GetScrollOffset());

    const auto& m = view.GetMetrics();
    auto all_tasks = view.GetFilteredAndSortedTasks();
    int row_h_calc = DesignSystem::ScaleForDpi(52, m.dpi);
    size_t visible_count = static_cast<size_t>(std::max(1, geom.table_body_rect.height() / row_h_calc));
    ShowScrollBar(hwnd, SB_VERT, FALSE);

    // 0. Base Matte Charcoal Canvas
    FillRectSolid(hdc, client_rc, pal.app_background);
    SetBkMode(hdc, TRANSPARENT);

    HFONT hFontBody = CreateFontW(
        -m.font_body_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontBold = CreateFontW(
        -m.font_body_strong_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmall = CreateFontW(
        -m.font_caption_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmallBold = CreateFontW(
        -m.font_caption_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(hdc, hFontBody);

    // 1. Top Glass Toolbar Surface & Controls
    RECT tb_rc = ToWinRect(geom.toolbar_rect);
    DrawGlassPanelGdi(hdc, tb_rc, 0, pal.glass_surface, pal.glass_border, 1, true /* add_top_specular */);

    // 1.1 Select All Checkbox
    RECT sel_rc = ToWinRect(geom.select_all_checkbox_rect);
    DrawCheckboxGdi(hdc, sel_rc, view.IsSelectAllChecked(), pal.surface_primary, pal.border_subtle, pal.accent_primary);

    // 1.2 Sort Dropdown
    RECT sort_rc = ToWinRect(geom.sort_dropdown_rect);
    DrawRoundedBoxGdi(hdc, sort_rc, 18, pal.surface_primary, pal.border_subtle, 1);
    DrawHamburgerGlyphGdi(hdc, sort_rc.left + 14, sort_rc.top + (sort_rc.bottom - sort_rc.top) / 2 - 5, 12, 10, pal.text_muted);
    RECT sort_txt_rc{sort_rc.left + 32, sort_rc.top, sort_rc.right - 18, sort_rc.bottom};
    SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
    std::wstring sort_label = L"Sort";
    DrawTextW(hdc, sort_label.c_str(), -1, &sort_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawCaretGlyphGdi(hdc, sort_rc.right - 12, sort_rc.top + (sort_rc.bottom - sort_rc.top) / 2, pal.text_muted, false);

    // 1.3 Filter Dropdown
    RECT filter_rc = ToWinRect(geom.filter_dropdown_rect);
    DrawRoundedBoxGdi(hdc, filter_rc, 18, pal.surface_primary, pal.border_subtle, 1);
    DrawFunnelGlyphGdi(hdc, filter_rc.left + 14, filter_rc.top + (filter_rc.bottom - filter_rc.top) / 2 - 5, 12, pal.text_muted);
    RECT filter_txt_rc{filter_rc.left + 32, filter_rc.top, filter_rc.right - 18, filter_rc.bottom};
    SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
    std::wstring wcat = (view.GetSelectedCategory() == SidebarCategory::kAllDownloads) ? L"All files" : Utf8ToW(view.CategoryToLabel(view.GetSelectedCategory()));
    DrawTextW(hdc, wcat.c_str(), -1, &filter_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawCaretGlyphGdi(hdc, filter_rc.right - 12, filter_rc.top + (filter_rc.bottom - filter_rc.top) / 2, pal.text_muted, false);

    // 1.4 + Add download Pill Button (smooth hover and press)
    RECT add_rc = ToWinRect(geom.add_download_button_rect);
    bool add_hovered = (view.GetHoveredToolbarIndex() == 0);
    bool add_pressed = (view.GetAnimationState().pressed_toolbar_idx == 0);
    float add_hp = add_hovered ? view.GetAnimationState().ComputeTransitionProgress(view.GetAnimationState().toolbar_hover_start_ms, 140) : 0.0f;
    ColorRgba add_bg = pal.accent_primary;
    if (add_pressed) {
        add_bg = pal.accent_primary.Interpolate(ColorRgba{0x00, 0x00, 0x00, 255}, 0.15f);
    } else if (add_hp > 0.0f) {
        add_bg = pal.accent_primary.Interpolate(pal.accent_hover, add_hp);
    }
    DrawRoundedBoxGdi(hdc, add_rc, 20, add_bg, add_bg, 0);
    SelectObject(hdc, hFontBold);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, L"+ Add download", -1, &add_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 1.5 Dynamic Context Action Buttons (smooth hover & press transitions)
    for (size_t btn_idx = 1; btn_idx < geom.toolbar_buttons.size() - 1; ++btn_idx) {
        const auto& btn = geom.toolbar_buttons[btn_idx];
        if (btn.rect.width() <= 0 || btn.rect.height() <= 0) continue;
        RECT brc = ToWinRect(btn.rect);
        bool hovered = (view.GetHoveredToolbarIndex() == static_cast<int>(btn_idx));
        bool pressed = (view.GetAnimationState().pressed_toolbar_idx == static_cast<int>(btn_idx));
        float hp = hovered ? view.GetAnimationState().ComputeTransitionProgress(view.GetAnimationState().toolbar_hover_start_ms, 140) : 0.0f;

        ColorRgba btn_bg = pal.glass_surface;
        ColorRgba btn_border = pal.glass_border;
        if (pressed) {
            btn_bg = pal.surface_pressed;
            btn_border = pal.glass_highlight;
        } else if (hp > 0.0f) {
            btn_bg = pal.glass_surface.Interpolate(pal.glass_elevated, hp);
            btn_border = pal.glass_border.Interpolate(pal.glass_highlight, hp);
        }
        DrawGlassPanelGdi(hdc, brc, 16, btn_bg, btn_border, 1, false);

        SelectObject(hdc, hFontSmallBold);
        ColorRgba txt_col = hovered ? pal.text_primary : pal.text_secondary;
        SetTextColor(hdc, txt_col.ToWin32ColorRef());
        std::wstring wlbl = Utf8ToW(btn.label);
        DrawTextW(hdc, wlbl.c_str(), -1, &brc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 1.6 Search Capsule
    RECT search_rc = ToWinRect(geom.search_box_rect);
    DrawRoundedBoxGdi(hdc, search_rc, 20, pal.surface_primary, pal.border_subtle, 1);
    DrawSearchGlassGdi(hdc, search_rc.left + 14, search_rc.top + (search_rc.bottom - search_rc.top) / 2 - 6, 13, pal.text_muted);

    // 1.7 Settings Gear Button
    RECT settings_rc = ToWinRect(geom.settings_button_rect);
    DrawRoundedBoxGdi(hdc, settings_rc, 20, pal.surface_primary, pal.border_subtle, 1);
    DrawGearGlyphGdi(hdc, settings_rc, pal.text_secondary);

    // 2. Table Column Headers (static and non-interactive)
    RECT header_rc = ToWinRect(geom.table_header_rect);
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
    RECT clip_hdr_rc{};
    bool needs_header = (IntersectRect(&clip_hdr_rc, &ps.rcPaint, &header_rc) != 0);
    if (needs_header) {
    for (size_t col_idx = 0; col_idx < geom.table_columns.size(); ++col_idx) {
        const auto& col = geom.table_columns[col_idx];
        if (col.rect.width() <= 0) continue;
        RECT crc = ToWinRect(col.rect);
        std::wstring wt = Utf8ToW(col.title);
        DrawTextW(hdc, wt.c_str(), -1, &crc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Draw Excel-style resize divider handle between columns
        if (col_idx < 6 && col.rect.right > 0 && col.rect.right < w) {
            bool is_resizing = (view.GetResizingColumnIndex() == static_cast<int>(col_idx));
            RECT div_handle{col.rect.right - 1, header_rc.top + 8, col.rect.right + 1, header_rc.bottom - 8};
            FillRectSolid(hdc, div_handle, is_resizing ? pal.accent_primary : ColorRgba{0x45, 0x47, 0x50, 255});
        }
    }
    RECT hdr_div{0, header_rc.bottom - 1, w, header_rc.bottom};
    FillRectSolid(hdc, hdr_div, pal.border_subtle);
    }

    // 3. Task Rows
    RECT table_body_winrc = ToWinRect(geom.table_body_rect);
    RECT clip_check_rc{};
    bool needs_rows = (IntersectRect(&clip_check_rc, &ps.rcPaint, &table_body_winrc) != 0);
    auto rows = needs_rows ? view.BuildVisibleTaskRows(view.GetScrollOffset(), geom.task_rows.size()) : std::vector<FormattedTaskRow>{};
    if (needs_rows && rows.empty()) {
        RECT empty_rc = ToWinRect(geom.table_body_rect);
        int empty_h = empty_rc.bottom - empty_rc.top;
        int empty_w = empty_rc.right - empty_rc.left;
        int center_y = empty_rc.top + empty_h / 2;
        int box_w = std::min(DesignSystem::ScaleForDpi(480, m.dpi), std::max(DesignSystem::ScaleForDpi(280, m.dpi), empty_w - 60));
        int box_left = empty_rc.left + (empty_w - box_w) / 2;

        // Card container for empty state
        RECT card_rc{box_left, center_y - DesignSystem::ScaleForDpi(80, m.dpi),
                     box_left + box_w, center_y + DesignSystem::ScaleForDpi(75, m.dpi)};
        DrawRoundedBoxGdi(hdc, card_rc, DesignSystem::ScaleForDpi(12, m.dpi),
                          pal.surface_primary, pal.border_subtle, 1);

        // Heading title
        RECT title_rc{card_rc.left + 16, card_rc.top + DesignSystem::ScaleForDpi(16, m.dpi),
                      card_rc.right - 16, card_rc.top + DesignSystem::ScaleForDpi(42, m.dpi)};
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        std::wstring title_str = (view.GetSelectedCategory() == SidebarCategory::kAllDownloads) ? L"No Downloads in Queue" : L"No Items in this Category";
        DrawTextW(hdc, title_str.c_str(), -1, &title_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);

        // Subtitle guidance message
        RECT msg_rc{card_rc.left + 20, card_rc.top + DesignSystem::ScaleForDpi(44, m.dpi),
                    card_rc.right - 20, card_rc.top + DesignSystem::ScaleForDpi(96, m.dpi)};
        SelectObject(hdc, hFontBody);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring emsg = Utf8ToW(view.GetEmptyStateMessage());
        DrawTextW(hdc, emsg.c_str(), -1, &msg_rc, DT_CENTER | DT_TOP | DT_WORDBREAK);

        // Action hint pill / shortcut
        RECT hint_rc{card_rc.left + (box_w - DesignSystem::ScaleForDpi(200, m.dpi)) / 2,
                     card_rc.bottom - DesignSystem::ScaleForDpi(38, m.dpi),
                     card_rc.left + (box_w + DesignSystem::ScaleForDpi(200, m.dpi)) / 2,
                     card_rc.bottom - DesignSystem::ScaleForDpi(12, m.dpi)};
        DrawRoundedBoxGdi(hdc, hint_rc, DesignSystem::ScaleForDpi(6, m.dpi),
                          pal.surface_hover, pal.border_subtle, 1);
        SelectObject(hdc, hFontSmallBold);
        SetTextColor(hdc, pal.accent_primary.ToWin32ColorRef());
        DrawTextW(hdc, L"Press Ctrl+N to Add URL", -1, &hint_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        for (size_t i = 0; i < rows.size() && i < geom.task_rows.size(); ++i) {
            const auto& rl = geom.task_rows[i];
            RECT rrc = ToWinRect(rl.row_rect);
            RECT row_clip{};
            if (!IntersectRect(&row_clip, &ps.rcPaint, &rrc)) continue;
            const auto& r = rows[i];

            // Selection keeps its short animation, but pointer hover changes
            // immediately when WM_MOUSEMOVE invalidates the old and new rows.
            {
                bool is_selected = r.selected;
                bool is_hovered = (view.GetHoveredRowIndex() == static_cast<int>(i));
                const auto& anim = view.GetAnimationState();
                if (is_selected) {
                    float sel_p = anim.ComputeTransitionProgress(anim.row_select_start_ms, 120);
                    ColorRgba row_col = pal.surface_primary.Interpolate(pal.surface_selected, sel_p);
                    FillRectSolid(hdc, rrc, row_col);
                } else if (is_hovered) {
                    FillRectSolid(hdc, rrc, pal.surface_hover);
                }
            }

            RECT row_div{0, rrc.bottom - 1, w, rrc.bottom};
            FillRectSolid(hdc, row_div, ColorRgba{0x20, 0x20, 0x24, 255});

            RECT chk_rc = ToWinRect(rl.checkbox_rect);
            DrawCheckboxGdi(hdc, chk_rc, r.checked, pal.surface_primary, pal.border_subtle, pal.accent_primary);

            int idx_w = DesignSystem::ScaleForDpi(32, m.dpi);
            RECT idx_rc{chk_rc.right + 4, rrc.top, chk_rc.right + 4 + idx_w, rrc.bottom};
            SelectObject(hdc, hFontBody);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            std::wstring widx = std::to_wstring(view.GetScrollOffset() + i + 1);
            DrawTextW(hdc, widx.c_str(), -1, &idx_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            int icon_x = idx_rc.right + 6;
            int icon_w = DesignSystem::ScaleForDpi(20, m.dpi);
            int icon_h = DesignSystem::ScaleForDpi(24, m.dpi);
            int icon_y = rrc.top + (rrc.bottom - rrc.top - icon_h) / 2;
            DrawFileTypeIconGdi(hdc, icon_x, icon_y, icon_w, icon_h, r, pal);

            SelectObject(hdc, hFontBold);
            SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
            int name_right = std::min(rl.filename_rect.right, geom.table_columns[0].rect.right);
            RECT fn_rc{icon_x + icon_w + 10, rrc.top + 7, std::max(icon_x + icon_w + 10, name_right - 10), rrc.top + 27};
            std::wstring wfn = Utf8ToW(r.display_filename);
            if (fn_rc.right > fn_rc.left) {
                DrawTextW(hdc, wfn.c_str(), -1, &fn_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // Sleek horizontal linear progress bar (8px height) with smoothly interpolated fill
            int bar_x = icon_x + icon_w + 10;
            int bar_y = rrc.top + 31;
            int progress_right = name_right - DesignSystem::ScaleForDpi(8, m.dpi);
            int pct_width = DesignSystem::ScaleForDpi(48, m.dpi);
            int pct_gap = DesignSystem::ScaleForDpi(8, m.dpi);
            int bar_max_w = std::min(DesignSystem::ScaleForDpi(160, m.dpi),
                                     progress_right - bar_x - pct_gap - pct_width);
            int bar_h = DesignSystem::ScaleForDpi(8, m.dpi);
            if (bar_max_w > 0) {
            RECT track_rc{bar_x, bar_y, bar_x + bar_max_w, bar_y + bar_h};
            FillRectSolid(hdc, track_rc, pal.progress_track);
            // Smooth interpolated progress: lerp cached value toward actual, capped at 99% unless complete
            {
                auto& interp_map = view.GetAnimationState().progress_interpolated;
                float actual_frac = static_cast<float>(std::clamp(r.progress_fraction, 0.0, 1.0));
                float disp_frac = actual_frac;
                if (view.GetAnimationState().IsActive()) {
                    auto it = interp_map.find(r.task_id);
                    if (it == interp_map.end()) {
                        interp_map[r.task_id] = actual_frac;
                        disp_frac = actual_frac;
                    } else {
                        float prev = it->second;
                        // Interpolate 15% per frame toward actual (smooth but fast catch-up)
                        float next = prev + (actual_frac - prev) * 0.15f;
                        it->second = next;
                        disp_frac = next;
                    }
                }
                int fill_w = static_cast<int>(static_cast<double>(bar_max_w) * disp_frac);
                if (fill_w > 0) {
                    ColorRgba fill_col = r.progress_fill_color;
                    // For actively downloading tasks, apply a subtle live pulse brightness
                    if (r.state == TaskState::kDownloading) {
                        uint64_t tick_now = GetTickCount64();
                        float pulse_t = static_cast<float>((tick_now / 20) % 50) / 50.0f;
                        float wave = (pulse_t <= 0.5f) ? (pulse_t * 2.0f) : ((1.0f - pulse_t) * 2.0f);
                        fill_col = fill_col.Interpolate(pal.accent_hover, wave * 0.4f);
                    }
                    RECT fill_rc{bar_x, bar_y, bar_x + fill_w, bar_y + bar_h};
                    FillRectSolid(hdc, fill_rc, fill_col);

                    // Multi-segment division lines when actively downloading with multiple segments
                    if (r.state == TaskState::kDownloading && bar_max_w >= 60) {
                        for (int seg = 1; seg <= 3; ++seg) {
                            int seg_x = bar_x + (bar_max_w * seg) / 4;
                            if (seg_x < bar_x + fill_w) {
                                RECT seg_rc{seg_x, bar_y, seg_x + 1, bar_y + bar_h};
                                FillRectSolid(hdc, seg_rc, ColorRgba{0x10, 0x10, 0x14, 180});
                            }
                        }
                    }
                }
            }
            RECT pct_rc{bar_x + bar_max_w + pct_gap, bar_y - 4,
                        bar_x + bar_max_w + pct_gap + pct_width, bar_y + bar_h + 4};
            SelectObject(hdc, hFontSmallBold);
            SetTextColor(hdc, (r.state == TaskState::kDownloading) ? pal.accent_primary.ToWin32ColorRef() : pal.text_secondary.ToWin32ColorRef());
            std::wstring wpct;
            if (r.state == TaskState::kCompleted) wpct = L"100%";
            else if (r.state == TaskState::kQueued) wpct = L"0%";
            else {
                char pbuf[32];
                double display_pct = std::min(99.0, r.progress_fraction * 100.0);
                std::snprintf(pbuf, sizeof(pbuf), "%.1f%%", display_pct);
                wpct = Utf8ToW(pbuf);
            }
            DrawTextW(hdc, wpct.c_str(), -1, &pct_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }

            // Column 1: Size
            if (geom.table_columns.size() > 1 && geom.table_columns[1].rect.width() > 0) {
                RECT sz_rc = ToWinRect(geom.table_columns[1].rect);
                sz_rc.top = rrc.top;
                sz_rc.bottom = rrc.bottom;
                sz_rc.right -= DesignSystem::ScaleForDpi(4, m.dpi);
                SelectObject(hdc, hFontBody);
                SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
                std::wstring wsz = Utf8ToW(r.size_text);
                DrawTextW(hdc, wsz.c_str(), -1, &sz_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // Column 2: Status
            if (geom.table_columns.size() > 2 && geom.table_columns[2].rect.width() > 0) {
                RECT st_rc = ToWinRect(geom.table_columns[2].rect);
                st_rc.top = rrc.top;
                st_rc.bottom = rrc.bottom;

                int dot_r = DesignSystem::ScaleForDpi(4, m.dpi);
                int dot_cy = (st_rc.top + st_rc.bottom) / 2;
                HPEN hDotPen = CreatePen(PS_SOLID, 1, r.status_indicator_color.ToWin32ColorRef());
                HBRUSH hDotBrush = CreateSolidBrush(r.status_indicator_color.ToWin32ColorRef());
                HGDIOBJ op = SelectObject(hdc, hDotPen);
                HGDIOBJ ob = SelectObject(hdc, hDotBrush);
                Ellipse(hdc, st_rc.left, dot_cy - dot_r, st_rc.left + dot_r * 2, dot_cy + dot_r);
                SelectObject(hdc, ob);
                SelectObject(hdc, op);
                DeleteObject(hDotBrush);
                DeleteObject(hDotPen);

                RECT st_txt_rc{st_rc.left + dot_r * 2 + 8, st_rc.top, st_rc.right - 4, st_rc.bottom};
                SelectObject(hdc, hFontBody);
                SetTextColor(hdc, r.status_indicator_color.ToWin32ColorRef());
                std::wstring wst = Utf8ToW(r.status_text);
                DrawTextW(hdc, wst.c_str(), -1, &st_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // Column 3: Download Speed strictly in KB/s
            if (geom.table_columns.size() > 3 && geom.table_columns[3].rect.width() > 0) {
                RECT dl_rc = ToWinRect(geom.table_columns[3].rect);
                dl_rc.top = rrc.top;
                dl_rc.bottom = rrc.bottom;

                RECT act_rc = ToWinRect(rl.action_icon_rect);

                // Compute action icon pulse overlay (one-time 200ms brightness flare)
                ColorRgba icon_tint_col = pal.accent_primary;
                {
                    const auto& anim = view.GetAnimationState();
                    auto it = anim.icon_pulses.find(r.task_id);
                    if (it != anim.icon_pulses.end()) {
                        uint64_t now = GetTickCount64();
                        uint64_t elapsed = (now >= it->second.start_ms) ? (now - it->second.start_ms) : 0;
                        if (elapsed < 200) {
                            float pulse_t = 1.0f - static_cast<float>(elapsed) / 200.0f;
                            // Flare: brighten toward white then fade back
                            icon_tint_col = pal.accent_primary.Interpolate(ColorRgba{255, 255, 255, 255}, pulse_t * 0.6f);
                        }
                    }
                }

                if (r.state == TaskState::kDownloading || r.state == TaskState::kProbing) {
                    DrawRoundedBoxGdi(hdc, act_rc, 6, pal.surface_primary, pal.border_subtle, 1);
                    DrawPauseGlyphGdi(hdc, act_rc, icon_tint_col);

                    RECT spd_rc{act_rc.right + 8, dl_rc.top, dl_rc.right - 2, dl_rc.bottom};
                    SelectObject(hdc, hFontBody);
                    SetTextColor(hdc, pal.accent_primary.ToWin32ColorRef());
                    std::wstring wspd = Utf8ToW(r.speed_text);
                    DrawTextW(hdc, wspd.c_str(), -1, &spd_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                } else if (r.state == TaskState::kPaused || r.state == TaskState::kQueued) {
                    ColorRgba play_tint = pal.text_secondary;
                    {
                        const auto& anim = view.GetAnimationState();
                        auto it = anim.icon_pulses.find(r.task_id);
                        if (it != anim.icon_pulses.end()) {
                            uint64_t now = GetTickCount64();
                            uint64_t elapsed = (now >= it->second.start_ms) ? (now - it->second.start_ms) : 0;
                            if (elapsed < 200) {
                                float pulse_t = 1.0f - static_cast<float>(elapsed) / 200.0f;
                                play_tint = pal.text_secondary.Interpolate(pal.accent_primary, pulse_t);
                            }
                        }
                    }
                    DrawRoundedBoxGdi(hdc, act_rc, 6, pal.surface_primary, pal.border_subtle, 1);
                    DrawPlayGlyphGdi(hdc, act_rc, play_tint);

                    RECT spd_rc{act_rc.right + 8, dl_rc.top, dl_rc.right - 2, dl_rc.bottom};
                    SelectObject(hdc, hFontBody);
                    SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
                    DrawTextW(hdc, L"0 KB/s", -1, &spd_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                } else {
                    RECT spd_rc{act_rc.left, dl_rc.top, dl_rc.right - 2, dl_rc.bottom};
                    SelectObject(hdc, hFontBody);
                    SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
                    DrawTextW(hdc, L"0 KB/s", -1, &spd_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                }
            }

            // Column 4: Upload Speed strictly in KB/s
            if (geom.table_columns.size() > 4 && geom.table_columns[4].rect.width() > 0) {
                RECT ul_rc = ToWinRect(geom.table_columns[4].rect);
                ul_rc.top = rrc.top;
                ul_rc.bottom = rrc.bottom;
                ul_rc.right -= DesignSystem::ScaleForDpi(2, m.dpi);
                SelectObject(hdc, hFontBody);
                SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
                std::wstring wul = Utf8ToW(r.upload_speed_text);
                DrawTextW(hdc, wul.c_str(), -1, &ul_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // Column 5: Added Timestamp
            if (geom.table_columns.size() > 5 && geom.table_columns[5].rect.width() > 0) {
                RECT add_time_rc = ToWinRect(geom.table_columns[5].rect);
                add_time_rc.top = rrc.top;
                add_time_rc.bottom = rrc.bottom;
                add_time_rc.right -= DesignSystem::ScaleForDpi(8, m.dpi);
                SelectObject(hdc, hFontBody);
                SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
                std::wstring wadd = Utf8ToW(r.added_time_text);
                DrawTextW(hdc, wadd.c_str(), -1, &add_time_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
    }

    // 3.4 Custom Slim Fluent Scrollbar
    auto* ctx = reinterpret_cast<Win32WindowContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (all_tasks.size() > visible_count) {
        int track_top = geom.table_body_rect.top + 4;
        int track_bottom = geom.table_body_rect.bottom - 4;
        int track_h = std::max(10, track_bottom - track_top);
        size_t max_offset = all_tasks.size() - visible_count;
        int thumb_h = std::max(24, static_cast<int>(track_h * visible_count / all_tasks.size()));
        int thumb_travel = track_h - thumb_h;
        int thumb_y = track_top + (thumb_travel > 0 ? static_cast<int>(thumb_travel * std::min(view.GetScrollOffset(), max_offset) / max_offset) : 0);

        RECT thumb_rc{w - 11, thumb_y, w - 5, thumb_y + thumb_h};
        bool active = (ctx && (ctx->is_dragging_vscroll || ctx->is_vscroll_hovered));
        ColorRgba thumb_col = active ? ColorRgba{255, 255, 255, 80} : ColorRgba{255, 255, 255, 38};
        DrawRoundedBoxGdi(hdc, thumb_rc, 3, thumb_col, thumb_col, 0);
    }

    // 3.5 Selected Download Details Inspector
    if (geom.details_panel_rect.height() > 0) {
        PaintDetailsPanelWin32(hdc, view, geom.details_panel_rect, hFontBody, hFontBold, hFontSmall);
    }

    // 4. Status Bar / Footer Glass Surface (Permanently removed)
    if (geom.status_bar_rect.height() > 0) {
    RECT status_rc = ToWinRect(geom.status_bar_rect);
    DrawGlassPanelGdi(hdc, status_rc, 0, pal.glass_surface, pal.glass_border, 1, false);

    RECT off_rc = ToWinRect(geom.status_toggle_off_rect);
    bool limiter = view.IsSpeedLimiterActive();
    ColorRgba off_bg = limiter ? pal.accent_primary : pal.surface_primary;
    DrawRoundedBoxGdi(hdc, off_rc, 10, off_bg, pal.border_subtle, 1);
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, limiter ? RGB(255, 255, 255) : pal.text_secondary.ToWin32ColorRef());
    DrawTextW(hdc, limiter ? L"ON" : L"OFF", -1, &off_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int timer_r = DesignSystem::ScaleForDpi(8, m.dpi);
    int timer_cx = off_rc.right + DesignSystem::ScaleForDpi(20, m.dpi);
    int timer_cy = (status_rc.top + status_rc.bottom) / 2;
    DrawTimerClockGdi(hdc, timer_cx, timer_cy, timer_r, pal.text_secondary);

    auto sb = view.BuildStatusBar();

    // 4a. Download Speed
    SelectObject(hdc, hFontBody);
    RECT dl_stat_rc{timer_cx + timer_r * 2 + 8, status_rc.top, timer_cx + timer_r * 2 + DesignSystem::ScaleForDpi(110, m.dpi), status_rc.bottom};
    SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
    std::wstring wstat_dl = std::wstring(L"\x2913 ") + Utf8ToW(sb.formatted_total_speed);
    DrawTextW(hdc, wstat_dl.c_str(), -1, &dl_stat_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4b. Upload Speed
    RECT ul_stat_rc{dl_stat_rc.right + 10, status_rc.top, dl_stat_rc.right + DesignSystem::ScaleForDpi(100, m.dpi), status_rc.bottom};
    SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
    std::wstring wstat_ul = std::wstring(L"\x2912 ") + Utf8ToW(sb.formatted_total_upload_speed);
    DrawTextW(hdc, wstat_ul.c_str(), -1, &ul_stat_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4c. Active & Queued Counts
    RECT counts_rc{ul_stat_rc.right + 12, status_rc.top, ul_stat_rc.right + DesignSystem::ScaleForDpi(145, m.dpi), status_rc.bottom};
    SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
    std::wstring wcounts = L"Active: " + std::to_wstring(sb.active_downloads) + L" | Queued: " + std::to_wstring(sb.remaining_queued);
    DrawTextW(hdc, wcounts.c_str(), -1, &counts_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int stat_r = DesignSystem::ScaleForDpi(4, m.dpi);

    // 4d. Browser Status Indicator & Label
    RECT brw_rc{counts_rc.right + 12, status_rc.top, counts_rc.right + DesignSystem::ScaleForDpi(125, m.dpi), status_rc.bottom};
    bool browser_active = sb.browser_ipc_listening && sb.browser_capture_enabled;
    ColorRgba brw_dot_color = browser_active ? ColorRgba(0x22, 0xC5, 0x5E) : (sb.browser_ipc_listening ? ColorRgba(0x3B, 0x82, 0xF6) : pal.text_muted);
    HPEN hBrwPen = CreatePen(PS_SOLID, 1, brw_dot_color.ToWin32ColorRef());
    HBRUSH hBrwBrush = CreateSolidBrush(brw_dot_color.ToWin32ColorRef());
    HGDIOBJ op_brw = SelectObject(hdc, hBrwPen);
    HGDIOBJ ob_brw = SelectObject(hdc, hBrwBrush);
    Ellipse(hdc, brw_rc.left, timer_cy - stat_r, brw_rc.left + stat_r * 2, timer_cy + stat_r);
    SelectObject(hdc, ob_brw);
    SelectObject(hdc, op_brw);
    DeleteObject(hBrwBrush);
    DeleteObject(hBrwPen);

    RECT brw_txt_rc{brw_rc.left + stat_r * 2 + 6, status_rc.top, brw_rc.right, status_rc.bottom};
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, browser_active ? pal.text_primary.ToWin32ColorRef() : pal.text_secondary.ToWin32ColorRef());
    std::wstring wbrw = browser_active ? L"Browser: Active" : (sb.browser_ipc_listening ? L"Browser: Connected" : L"Browser: Standby");
    DrawTextW(hdc, wbrw.c_str(), -1, &brw_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4e. Stream Workers Status Indicator & Label
    RECT wrk_rc{brw_rc.right + 12, status_rc.top, brw_rc.right + DesignSystem::ScaleForDpi(110, m.dpi), status_rc.bottom};
    ColorRgba wrk_dot_color = sb.workers_ready ? ColorRgba(0x22, 0xC5, 0x5E) : pal.accent_primary;
    HPEN hWrkPen = CreatePen(PS_SOLID, 1, wrk_dot_color.ToWin32ColorRef());
    HBRUSH hWrkBrush = CreateSolidBrush(wrk_dot_color.ToWin32ColorRef());
    HGDIOBJ op_wrk = SelectObject(hdc, hWrkPen);
    HGDIOBJ ob_wrk = SelectObject(hdc, hWrkBrush);
    Ellipse(hdc, wrk_rc.left, timer_cy - stat_r, wrk_rc.left + stat_r * 2, timer_cy + stat_r);
    SelectObject(hdc, ob_wrk);
    SelectObject(hdc, op_wrk);
    DeleteObject(hWrkBrush);
    DeleteObject(hWrkPen);

    RECT wrk_txt_rc{wrk_rc.left + stat_r * 2 + 6, status_rc.top, wrk_rc.right, status_rc.bottom};
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, sb.workers_ready ? pal.text_primary.ToWin32ColorRef() : pal.accent_primary.ToWin32ColorRef());
    std::wstring wwrk = sb.workers_ready ? L"Workers: Ready" : L"Workers: Missing";
    DrawTextW(hdc, wwrk.c_str(), -1, &wrk_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4f. Priority / Limiter State Indicator & Label
    RECT pri_rc{wrk_rc.right + 12, status_rc.top, wrk_rc.right + DesignSystem::ScaleForDpi(90, m.dpi), status_rc.bottom};
    ColorRgba dot_color = limiter ? pal.accent_primary : pal.surface_elevated;
    HPEN hDotPen = CreatePen(PS_SOLID, 1, dot_color.ToWin32ColorRef());
    HBRUSH hDotBrush = CreateSolidBrush(dot_color.ToWin32ColorRef());
    HGDIOBJ op = SelectObject(hdc, hDotPen);
    HGDIOBJ ob = SelectObject(hdc, hDotBrush);
    Ellipse(hdc, pri_rc.left, timer_cy - stat_r, pri_rc.left + stat_r * 2, timer_cy + stat_r);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(hDotBrush);
    DeleteObject(hDotPen);

    RECT pri_txt_rc{pri_rc.left + stat_r * 2 + 6, status_rc.top, pri_rc.right, status_rc.bottom};
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, limiter ? pal.text_primary.ToWin32ColorRef() : pal.text_secondary.ToWin32ColorRef());
    std::wstring wlimit = limiter ? L"LIMIT ON" : L"NORMAL";
    DrawTextW(hdc, wlimit.c_str(), -1, &pri_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4g. Active or Selected Filename (Right-aligned)
    if (!rows.empty()) {
        std::string right_fn;
        std::string sel_id = view.GetSelectedTaskId();
        if (!sel_id.empty()) {
            for (const auto& r : rows) {
                if (r.task_id == sel_id) {
                    right_fn = r.full_filename_tooltip;
                    break;
                }
            }
        }
        if (right_fn.empty()) {
            for (const auto& r : rows) {
                if (r.state == TaskState::kDownloading) {
                    right_fn = r.full_filename_tooltip;
                    break;
                }
            }
        }
        if (right_fn.empty()) {
            right_fn = rows[0].full_filename_tooltip;
        }

        int min_margin = DesignSystem::ScaleForDpi(24, m.dpi);
        int right_end = w - DesignSystem::ScaleForDpi(16, m.dpi);
        int left_start = pri_rc.right + min_margin;
        int avail_fn_w = right_end - left_start;
        if (avail_fn_w >= DesignSystem::ScaleForDpi(140, m.dpi)) {
            RECT rfn_rc{left_start, status_rc.top, right_end, status_rc.bottom};
            SelectObject(hdc, hFontBody);
            SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
            std::wstring wrfn = Utf8ToW(right_fn);
            DrawTextW(hdc, wrfn.c_str(), -1, &rfn_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    }

    // 5. Settings Center Overlay (when active)
    if (view.IsSettingsCenterOpen()) {
        PaintSettingsCenterOverlayWin32(hdc, view, client_rc, hFontBody, hFontBold);
    }

    SelectObject(hdc, old_font);
    DeleteObject(hFontBody);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);
    DeleteObject(hFontSmallBold);

    BitBlt(hdc_screen, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, old_bm);
    DeleteObject(hbm);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
}

static void InitWin32DarkMode(HWND hwnd) {
    ApplyWindowGlassBackdropWin32(hwnd, false /* is_transient_dialog */);
    BOOL use_dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &use_dark, sizeof(use_dark));
    COLORREF titlebar_col = RGB(0x16, 0x16, 0x18);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &titlebar_col, sizeof(titlebar_col));
    COLORREF titletext_col = RGB(0xFF, 0xFF, 0xFF);
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &titletext_col, sizeof(titletext_col));

    HMODULE hUxTheme = LoadLibraryW(L"uxtheme.dll");
    if (hUxTheme) {
        using fnSetPreferredAppMode = int(WINAPI*)(int mode);
        auto pfnSetPreferredAppMode = reinterpret_cast<fnSetPreferredAppMode>(GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135)));
        if (pfnSetPreferredAppMode) {
            pfnSetPreferredAppMode(2 /* ForceDark */);
        }
        using fnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND, BOOL);
        auto pfnAllowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133)));
        if (pfnAllowDarkModeForWindow) {
            pfnAllowDarkModeForWindow(hwnd, TRUE);
        }
        using fnFlushMenuThemes = void(WINAPI*)();
        auto pfnFlushMenuThemes = reinterpret_cast<fnFlushMenuThemes>(GetProcAddress(hUxTheme, MAKEINTRESOURCEA(136)));
        if (pfnFlushMenuThemes) {
            pfnFlushMenuThemes();
        }
    }
}

LRESULT CALLBACK OpenIdmWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<Win32WindowContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<Win32WindowContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx || !ctx->view) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_CREATE: {
            if (g_msgTaskbarButtonCreated == 0) {
                g_msgTaskbarButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
            }
            InitWin32DarkMode(hwnd);
            DragAcceptFiles(hwnd, TRUE);
            HICON hAppIcon = CreateRadiumAppIcon();
            if (hAppIcon) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));
            }
            // Reset taskbar cache upon creation/re-creation
            ctx->taskbar_info_initialized = false;
            ctx->last_taskbar_info = {};

            // Initialize COM for ITaskbarList3 (apartment-threaded, UI thread)
            HRESULT hr_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            ctx->com_initialized = SUCCEEDED(hr_com) || (hr_com == RPC_E_CHANGED_MODE);
            if (ctx->com_initialized) {
                ITaskbarList3* pTbl = nullptr;
                HRESULT hr_tbl = CoCreateInstance(
                    CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                    IID_ITaskbarList3, reinterpret_cast<void**>(&pTbl));
                if (SUCCEEDED(hr_tbl) && pTbl) {
                    if (SUCCEEDED(pTbl->HrInit())) {
                        ctx->pTaskbarList = pTbl;
                    } else {
                        pTbl->Release();
                    }
                }
            }
            auto geom = ctx->view->ComputeLayoutGeometry(0);
            int edit_pad = DesignSystem::ScaleForDpi(32, ctx->view->GetMetrics().dpi);
            int edit_h = DesignSystem::ScaleForDpi(22, ctx->view->GetMetrics().dpi);
            int edit_y = geom.search_box_rect.top + (geom.search_box_rect.height() - edit_h) / 2;
            ctx->hSearchEdit = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                geom.search_box_rect.left + edit_pad, edit_y,
                std::max(10, geom.search_box_rect.width() - edit_pad - 14), edit_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_EDIT)),
                GetModuleHandleW(nullptr), nullptr);
            int edit_font_sz = DesignSystem::ScaleForDpi(15, ctx->view->GetMetrics().dpi);
            HFONT hEditFont = CreateFontW(
                -edit_font_sz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SendMessageW(ctx->hSearchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hEditFont), TRUE);
            SendMessageW(ctx->hSearchEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search in downloads..."));
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            HWND hCtl = reinterpret_cast<HWND>(lParam);
            if (hCtl == ctx->hSearchEdit) {
                SetTextColor(hdcEdit, RGB(0xF0, 0xF0, 0xF5));
                SetBkColor(hdcEdit, RGB(0x1E, 0x1E, 0x22));
                static HBRUSH hEditBrush = CreateSolidBrush(RGB(0x1E, 0x1E, 0x22));
                return reinterpret_cast<LRESULT>(hEditBrush);
            }
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_SEARCH_EDIT && HIWORD(wParam) == EN_CHANGE) {
                wchar_t buf[512]{};
                GetWindowTextW(ctx->hSearchEdit, buf, 511);
                if (ctx->view->IsSettingsCenterOpen()) {
                    ctx->view->SetSettingsSearchQuery(WToUtf8(buf));
                } else {
                    ctx->view->SetSearchQuery(WToUtf8(buf));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (ctx->view->IsResizingColumn() || ctx->view->HitTestColumnDivider(pt.x, pt.y, ctx->view->GetScrollOffset()) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = ctx->view->GetMetrics().min_window_width;
            mmi->ptMinTrackSize.y = ctx->view->GetMetrics().min_window_height;
            return 0;
        }
        case WM_DPICHANGED: {
            UINT new_dpi = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ctx->view->SetWindowDimensions(
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                new_dpi);
            SetWindowPos(
                hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
            if (ctx->hSearchEdit) {
                int edit_pad = DesignSystem::ScaleForDpi(32, ctx->view->GetMetrics().dpi);
                int edit_h = DesignSystem::ScaleForDpi(22, ctx->view->GetMetrics().dpi);
                int edit_y = geom.search_box_rect.top + (geom.search_box_rect.height() - edit_h) / 2;
                MoveWindow(ctx->hSearchEdit, geom.search_box_rect.left + edit_pad, edit_y,
                           std::max(10, geom.search_box_rect.width() - edit_pad - 14), edit_h, TRUE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SHOWWINDOW: {
            bool shown = (wParam != FALSE);
            ctx->view->SetWindowVisibility(shown, IsIconic(hwnd) != FALSE);
            return 0;
        }
        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) {
                ctx->view->SetWindowVisibility(true, true);
                return 0;
            }
            ctx->view->SetWindowVisibility(IsWindowVisible(hwnd) != FALSE, false);
            UINT dpi = GetDpiForWindow(hwnd);
            ctx->view->SetWindowDimensions(LOWORD(lParam), HIWORD(lParam), dpi > 0 ? dpi : 96);
            auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
            if (ctx->hSearchEdit) {
                int edit_pad = DesignSystem::ScaleForDpi(32, ctx->view->GetMetrics().dpi);
                int edit_h = DesignSystem::ScaleForDpi(22, ctx->view->GetMetrics().dpi);
                int edit_y = geom.search_box_rect.top + (geom.search_box_rect.height() - edit_h) / 2;
                MoveWindow(ctx->hSearchEdit, geom.search_box_rect.left + edit_pad, edit_y,
                           std::max(10, geom.search_box_rect.width() - edit_pad - 14), edit_h, TRUE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (ctx->view->IsResizingColumn()) {
                ctx->view->UpdateColumnResize(x);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ctx->is_dragging_vscroll) {
                auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
                auto all_tasks = ctx->view->GetFilteredAndSortedTasks();
                int row_h_calc = DesignSystem::ScaleForDpi(52, ctx->view->GetMetrics().dpi);
                size_t visible_count = static_cast<size_t>(std::max(1, geom.table_body_rect.height() / row_h_calc));
                if (all_tasks.size() > visible_count) {
                    size_t max_offset = all_tasks.size() - visible_count;
                    int track_top = geom.table_body_rect.top + 4;
                    int track_bottom = geom.table_body_rect.bottom - 4;
                    int track_h = std::max(10, track_bottom - track_top);
                    int thumb_h = std::max(24, static_cast<int>(track_h * visible_count / all_tasks.size()));
                    int thumb_travel = track_h - thumb_h;
                    if (thumb_travel > 0) {
                        int dy = y - ctx->vscroll_drag_start_y;
                        int offset_delta = (dy * static_cast<int>(max_offset)) / thumb_travel;
                        int new_offset = std::clamp(static_cast<int>(ctx->vscroll_drag_start_offset) + offset_delta, 0, static_cast<int>(max_offset));
                        ctx->view->SetScrollOffset(static_cast<size_t>(new_offset));
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }

            RECT client_rc{};
            GetClientRect(hwnd, &client_rc);
            int win_w = client_rc.right - client_rc.left;
            auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
            bool on_scroll = (x >= win_w - 16 && x <= win_w && y >= geom.table_body_rect.top && y <= geom.table_body_rect.bottom);
            if (on_scroll != ctx->is_vscroll_hovered) {
                ctx->is_vscroll_hovered = on_scroll;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (!ctx->mouse_tracking_active) {
                TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                ctx->mouse_tracking_active = true;
            }

            int old_row = ctx->view->GetHoveredRowIndex();
            int old_tb = ctx->view->GetHoveredToolbarIndex();
            int old_sb = ctx->view->GetHoveredSidebarIndex();

            size_t scroll_offset = ctx->view->GetScrollOffset();
            ctx->view->HandleMouseMove(x, y, scroll_offset);

            int new_row = ctx->view->GetHoveredRowIndex();
            int new_tb = ctx->view->GetHoveredToolbarIndex();
            int new_sb = ctx->view->GetHoveredSidebarIndex();

            if (old_row != new_row) {
                if (old_row >= 0) {
                    RECT rc = ToWinRect(ctx->view->GetRowRect(static_cast<size_t>(old_row)));
                    RedrawWindow(hwnd, &rc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
                }
                if (new_row >= 0) {
                    RECT rc = ToWinRect(ctx->view->GetRowRect(static_cast<size_t>(new_row)));
                    RedrawWindow(hwnd, &rc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
                }
            }

            if (old_tb != new_tb || old_sb != new_sb) {
                RECT tb_rc = ToWinRect(geom.toolbar_rect);
                InvalidateRect(hwnd, &tb_rc, FALSE);
            }

            return 0;
        }
        case WM_MOUSELEAVE: {
            ctx->mouse_tracking_active = false;
            int old_row = ctx->view->GetHoveredRowIndex();
            int old_tb = ctx->view->GetHoveredToolbarIndex();
            int old_sb = ctx->view->GetHoveredSidebarIndex();

            ctx->view->ClearHoverState();

            if (old_row >= 0) {
                RECT rc = ToWinRect(ctx->view->GetRowRect(static_cast<size_t>(old_row)));
                RedrawWindow(hwnd, &rc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            }
            if (old_tb >= 0 || old_sb >= 0) {
                auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
                RECT tb_rc = ToWinRect(geom.toolbar_rect);
                InvalidateRect(hwnd, &tb_rc, FALSE);
            }
            if (ctx->is_vscroll_hovered) {
                ctx->is_vscroll_hovered = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (ctx->view->IsResizingColumn()) {
                ReleaseCapture();
                ctx->view->EndColumnResize();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ctx->is_dragging_vscroll) {
                ctx->is_dragging_vscroll = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Clear toolbar press state for button release animation
            if (ctx->view->GetAnimationState().pressed_toolbar_idx >= 0) {
                ctx->view->GetAnimationState().pressed_toolbar_idx = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            int rows_to_scroll = (zDelta > 0) ? -3 : 3;
            ctx->view->ScrollBy(rows_to_scroll);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            size_t scroll_offset = ctx->view->GetScrollOffset();

            // Custom Slim Scrollbar Hit Test
            RECT client_rc{};
            GetClientRect(hwnd, &client_rc);
            int win_w = client_rc.right - client_rc.left;
            auto geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
            if (x >= win_w - 16 && x <= win_w && y >= geom.table_body_rect.top && y <= geom.table_body_rect.bottom) {
                auto all_tasks = ctx->view->GetFilteredAndSortedTasks();
                int row_h_calc = DesignSystem::ScaleForDpi(52, ctx->view->GetMetrics().dpi);
                size_t visible_count = static_cast<size_t>(std::max(1, geom.table_body_rect.height() / row_h_calc));
                if (all_tasks.size() > visible_count) {
                    size_t max_offset = all_tasks.size() - visible_count;
                    int track_top = geom.table_body_rect.top + 4;
                    int track_bottom = geom.table_body_rect.bottom - 4;
                    int track_h = std::max(10, track_bottom - track_top);
                    int thumb_h = std::max(24, static_cast<int>(track_h * visible_count / all_tasks.size()));
                    int thumb_travel = track_h - thumb_h;
                    int thumb_y = track_top + (thumb_travel > 0 ? static_cast<int>(thumb_travel * std::min(scroll_offset, max_offset) / max_offset) : 0);

                    if (y >= thumb_y && y <= thumb_y + thumb_h) {
                        ctx->is_dragging_vscroll = true;
                        ctx->vscroll_drag_start_y = y;
                        ctx->vscroll_drag_start_offset = scroll_offset;
                        SetCapture(hwnd);
                        return 0;
                    } else if (y < thumb_y) {
                        ctx->view->ScrollBy(-static_cast<int>(visible_count));
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    } else {
                        ctx->view->ScrollBy(static_cast<int>(visible_count));
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
            }

            int div_idx = ctx->view->HitTestColumnDivider(x, y, scroll_offset);
            if (div_idx >= 0 && msg == WM_LBUTTONDOWN) {
                SetCapture(hwnd);
                ctx->view->StartColumnResize(div_idx, x);
                return 0;
            }

            auto hit = ctx->view->HitTestPoint(x, y, scroll_offset);

            // Track pressed toolbar button for press animation feedback
            if (msg == WM_LBUTTONDOWN) {
                int new_pressed = -1;
                if (hit.type == HitTargetType::kToolbarButton) new_pressed = hit.index;
                else if (hit.type == HitTargetType::kAddDownloadButton) new_pressed = 0;
                ctx->view->GetAnimationState().pressed_toolbar_idx = new_pressed;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (ctx->view->IsSettingsCenterOpen()) {
                if (hit.type == HitTargetType::kSettingsItemControl) {
                    if (hit.task_id == "default_download_dir") {
                        std::string chosen_dir = ctx->view->GetDraftSettings().default_download_dir;
                        if (SelectFolderWithDialogWin32(hwnd, chosen_dir)) {
                            ctx->view->SetDraftSettingByKey("default_download_dir", chosen_dir);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    auto sv = ctx->view->BuildSettingsCenterView();
                    const SettingItemView* clicked_itm = nullptr;
                    for (const auto& itm : sv.visible_items) {
                        if (itm.key == hit.task_id) {
                            clicked_itm = &itm;
                            break;
                        }
                    }
                    if (clicked_itm && clicked_itm->control_type == SettingControlType::kDropdown) {
                        RECT ctrl_rc = ToWinRect(clicked_itm->control_rect);
                        POINT pt{ctrl_rc.left, ctrl_rc.bottom + 2};
                        ClientToScreen(hwnd, &pt);
                        HMENU hMenu = CreatePopupMenu();
                        if (hit.task_id == "conflict_policy") {
                            auto pol = ctx->view->GetDraftSettings().conflict_policy;
                            AppendMenuW(hMenu, MF_STRING | (pol == ConflictPolicy::kAutoRename ? MF_CHECKED : 0), 1, L"Auto-Rename (Numbered Copy)");
                            AppendMenuW(hMenu, MF_STRING | (pol == ConflictPolicy::kOverwrite ? MF_CHECKED : 0), 2, L"Overwrite Existing File");
                            AppendMenuW(hMenu, MF_STRING | (pol == ConflictPolicy::kFailIfExists ? MF_CHECKED : 0), 3, L"Prompt / Fail if Exists");
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c == 1) ctx->view->SetDraftSettingByKey("conflict_policy", "auto_rename");
                            else if (c == 2) ctx->view->SetDraftSettingByKey("conflict_policy", "overwrite");
                            else if (c == 3) ctx->view->SetDraftSettingByKey("conflict_policy", "fail_if_exists");
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "proxy_mode") {
                            auto pm = ctx->view->GetDraftSettings().proxy_mode;
                            AppendMenuW(hMenu, MF_STRING | (pm == "direct" ? MF_CHECKED : 0), 1, L"Direct (No Proxy)");
                            AppendMenuW(hMenu, MF_STRING | (pm == "system" ? MF_CHECKED : 0), 2, L"Use Windows System Proxy");
                            AppendMenuW(hMenu, MF_STRING | (pm == "manual" ? MF_CHECKED : 0), 3, L"Manual HTTP/SOCKS Proxy");
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c == 1) ctx->view->SetDraftSettingByKey("proxy_mode", "direct");
                            else if (c == 2) ctx->view->SetDraftSettingByKey("proxy_mode", "system");
                            else if (c == 3) ctx->view->SetDraftSettingByKey("proxy_mode", "manual");
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "default_connections_per_task") {
                            auto conns = ctx->view->GetDraftSettings().default_connections_per_task;
                            const uint32_t presets[] = {1, 2, 4, 8, 16, 24, 32};
                            for (size_t i = 0; i < 7; ++i) {
                                std::wstring lbl = std::to_wstring(presets[i]) + L" Connections" + (presets[i] == 8 ? L" (Recommended)" : L"");
                                AppendMenuW(hMenu, MF_STRING | (conns == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(10 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 10 && c < 17) {
                                ctx->view->SetDraftSettingByKey("default_connections_per_task", std::to_string(presets[c - 10]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "max_concurrent_tasks") {
                            auto mc = ctx->view->GetDraftSettings().max_concurrent_tasks;
                            const uint32_t presets[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16};
                            for (size_t i = 0; i < 10; ++i) {
                                std::wstring lbl = std::to_wstring(presets[i]) + L" Tasks" + (presets[i] == 3 ? L" (Default)" : (presets[i] == 6 ? L" (Recommended)" : L""));
                                AppendMenuW(hMenu, MF_STRING | (mc == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(20 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 20 && c < 30) {
                                ctx->view->SetDraftSettingByKey("max_concurrent_tasks", std::to_string(presets[c - 20]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "max_auto_retries") {
                            auto retries = ctx->view->GetDraftSettings().max_auto_retries;
                            const uint32_t presets[] = {0, 1, 2, 3, 5, 8, 10, 15, 20, 30, 50};
                            for (size_t i = 0; i < 11; ++i) {
                                std::wstring lbl = (presets[i] == 0) ? L"0 (No Retries)" : (std::to_wstring(presets[i]) + L" Retries" + (presets[i] == 5 ? L" (Default)" : L""));
                                AppendMenuW(hMenu, MF_STRING | (retries == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(80 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 80 && c < 91) {
                                ctx->view->SetDraftSettingByKey("max_auto_retries", std::to_string(presets[c - 80]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "retry_interval_sec") {
                            auto sec = ctx->view->GetDraftSettings().retry_interval_sec;
                            const uint32_t presets[] = {5, 10, 15, 30, 60, 120};
                            for (size_t i = 0; i < 6; ++i) {
                                std::wstring lbl = std::to_wstring(presets[i]) + L" Seconds" + (presets[i] == 15 ? L" (Default)" : L"");
                                AppendMenuW(hMenu, MF_STRING | (sec == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(100 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 100 && c < 106) {
                                ctx->view->SetDraftSettingByKey("retry_interval_sec", std::to_string(presets[c - 100]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "min_split_bytes") {
                            auto msb = ctx->view->GetDraftSettings().min_split_bytes;
                            const uint64_t presets[] = {65536ULL, 131072ULL, 262144ULL, 524288ULL, 1048576ULL, 2097152ULL, 5242880ULL};
                            const wchar_t* lbls[] = {L"64 KiB", L"128 KiB (Default)", L"256 KiB", L"512 KiB", L"1 MiB", L"2 MiB", L"5 MiB"};
                            for (size_t i = 0; i < 7; ++i) {
                                AppendMenuW(hMenu, MF_STRING | (msb == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(110 + i), lbls[i]);
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 110 && c < 117) {
                                ctx->view->SetDraftSettingByKey("min_split_bytes", std::to_string(presets[c - 110]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "speed_limit_bps") {
                            auto sp = ctx->view->GetDraftSettings().speed_limit_bps;
                            struct SpDef { uint64_t bps; const wchar_t* lbl; };
                            const SpDef sp_presets[] = {
                                {0ULL, L"Unlimited"},
                                {1048576ULL, L"1 MB/s (8 Mbps)"},
                                {5242880ULL, L"5 MB/s (40 Mbps)"},
                                {10485760ULL, L"10 MB/s (80 Mbps)"},
                                {26214400ULL, L"25 MB/s (200 Mbps)"},
                                {52428800ULL, L"50 MB/s (400 Mbps)"}
                            };
                            for (size_t i = 0; i < 6; ++i) {
                                AppendMenuW(hMenu, MF_STRING | (sp == sp_presets[i].bps ? MF_CHECKED : 0), static_cast<UINT_PTR>(30 + i), sp_presets[i].lbl);
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 30 && c < 36) {
                                ctx->view->SetDraftSettingByKey("speed_limit_bps", std::to_string(sp_presets[c - 30].bps));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "stall_timeout_ms") {
                            auto st = ctx->view->GetDraftSettings().stall_timeout_ms;
                            const int timeouts[] = {1500, 3000, 5000, 10000, 15000};
                            for (size_t i = 0; i < 5; ++i) {
                                std::wstring lbl = std::to_wstring(timeouts[i] / 1000) + L" Seconds" + (timeouts[i] == 3000 ? L" (Default)" : L"");
                                AppendMenuW(hMenu, MF_STRING | (st == timeouts[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(40 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 40 && c < 45) {
                                ctx->view->SetDraftSettingByKey("stall_timeout_ms", std::to_string(timeouts[c - 40]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "queue_completion_action") {
                            auto qa = ctx->view->GetDraftSettings().queue_completion_action;
                            AppendMenuW(hMenu, MF_STRING | (qa == "NONE" ? MF_CHECKED : 0), 51, L"None (Do Nothing)");
                            AppendMenuW(hMenu, MF_STRING | (qa == "SLEEP" ? MF_CHECKED : 0), 52, L"Put PC to Sleep");
                            AppendMenuW(hMenu, MF_STRING | (qa == "SHUTDOWN" ? MF_CHECKED : 0), 53, L"Shutdown PC");
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c == 51) ctx->view->SetDraftSettingByKey("queue_completion_action", "NONE");
                            else if (c == 52) ctx->view->SetDraftSettingByKey("queue_completion_action", "SLEEP");
                            else if (c == 53) ctx->view->SetDraftSettingByKey("queue_completion_action", "SHUTDOWN");
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else {
                            DestroyMenu(hMenu);
                        }
                    }
                }
                if (hit.type == HitTargetType::kSettingsRestoreDefaultsButton) {
                    if (MessageBoxW(hwnd, L"Restore all Radium settings to their factory defaults?", L"Confirm Restore Defaults", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        ctx->view->ActionRestoreDefaultSettings();
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                ctx->view->HandleMouseClick(x, y, msg == WM_LBUTTONDBLCLK, scroll_offset);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit.type == HitTargetType::kSortDropdown) {
                geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
                POINT pt{geom.sort_dropdown_rect.left, geom.sort_dropdown_rect.bottom + 2};
                ClientToScreen(hwnd, &pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kCreatedAt && !ctx->view->IsSortAscending() ? MF_CHECKED : 0), 101, L"Newest (Default)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kCreatedAt && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 102, L"Oldest");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kFileName && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 103, L"Name (A-Z)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kFileName && !ctx->view->IsSortAscending() ? MF_CHECKED : 0), 104, L"Name (Z-A)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kSize && !ctx->view->IsSortAscending() ? MF_CHECKED : 0), 105, L"Size (Largest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kSize && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 106, L"Size (Smallest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kStatus ? MF_CHECKED : 0), 107, L"Status");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kSpeed ? MF_CHECKED : 0), 108, L"Speed (Fastest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kProgress && !ctx->view->IsSortAscending() ? MF_CHECKED : 0), 109, L"Progress (Highest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kProgress && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 110, L"Progress (Lowest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kEta && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 111, L"ETA (Shortest)");
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == 101) ctx->view->SetSort(SortColumn::kCreatedAt, false);
                else if (cmd == 102) ctx->view->SetSort(SortColumn::kCreatedAt, true);
                else if (cmd == 103) ctx->view->SetSort(SortColumn::kFileName, true);
                else if (cmd == 104) ctx->view->SetSort(SortColumn::kFileName, false);
                else if (cmd == 105) ctx->view->SetSort(SortColumn::kSize, false);
                else if (cmd == 106) ctx->view->SetSort(SortColumn::kSize, true);
                else if (cmd == 107) ctx->view->SetSort(SortColumn::kStatus, true);
                else if (cmd == 108) ctx->view->SetSort(SortColumn::kSpeed, false);
                else if (cmd == 109) ctx->view->SetSort(SortColumn::kProgress, false);
                else if (cmd == 110) ctx->view->SetSort(SortColumn::kProgress, true);
                else if (cmd == 111) ctx->view->SetSort(SortColumn::kEta, true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit.type == HitTargetType::kFilterDropdown || (hit.type == HitTargetType::kSidebarCategory && hit.index == 0)) {
                geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
                POINT pt{geom.filter_dropdown_rect.left, geom.filter_dropdown_rect.bottom + 2};
                ClientToScreen(hwnd, &pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kAllDownloads ? MF_CHECKED : 0), 201, L"All files");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kInProgress ? MF_CHECKED : 0), 202, L"In Progress");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kCompleted ? MF_CHECKED : 0), 203, L"Completed");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kQueued ? MF_CHECKED : 0), 204, L"Queued");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kFailed ? MF_CHECKED : 0), 207, L"Failed / Canceled");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kVideoDownloads ? MF_CHECKED : 0), 205, L"Video Downloads");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kScheduled ? MF_CHECKED : 0), 206, L"Scheduled");
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == 201) ctx->view->SelectCategory(SidebarCategory::kAllDownloads);
                else if (cmd == 202) ctx->view->SelectCategory(SidebarCategory::kInProgress);
                else if (cmd == 203) ctx->view->SelectCategory(SidebarCategory::kCompleted);
                else if (cmd == 204) ctx->view->SelectCategory(SidebarCategory::kQueued);
                else if (cmd == 205) ctx->view->SelectCategory(SidebarCategory::kVideoDownloads);
                else if (cmd == 206) ctx->view->SelectCategory(SidebarCategory::kScheduled);
                else if (cmd == 207) ctx->view->SelectCategory(SidebarCategory::kFailed);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit.type == HitTargetType::kSearchBox && ctx->hSearchEdit) {
                SetFocus(ctx->hSearchEdit);
                SendMessageW(ctx->hSearchEdit, EM_SETSEL, 0, -1);
                return 0;
            }

            ctx->view->HandleMouseClick(x, y, msg == WM_LBUTTONDBLCLK, scroll_offset);

            if (hit.type == HitTargetType::kToolbarButton || hit.type == HitTargetType::kAddDownloadButton) {
                geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
                if (hit.type == HitTargetType::kAddDownloadButton) {
                    ShowAddUrlQuickDialogWin32(hwnd, *ctx->view, *ctx->controller);
                } else if (hit.index >= 0 && static_cast<size_t>(hit.index) < geom.toolbar_buttons.size()) {
                    auto aid = geom.toolbar_buttons[static_cast<size_t>(hit.index)].action_id;
                    if (aid == ToolbarActionId::kAddUrl) {
                        ShowAddUrlQuickDialogWin32(hwnd, *ctx->view, *ctx->controller);
                    }
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (ctx->view->IsSettingsCenterOpen()) return 0;
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT client_pt = pt;
            ScreenToClient(hwnd, &client_pt);
            size_t scroll_offset = ctx->view->GetScrollOffset();
            auto hit = ctx->view->HitTestPoint(client_pt.x, client_pt.y, scroll_offset);
            if (hit.type == HitTargetType::kTaskRow || hit.type == HitTargetType::kTaskRowActionButton) {
                ctx->view->SelectTask(hit.task_id);
            }
            auto tb = ctx->view->ComputeToolbarState();
            std::string sel = ctx->view->GetSelectedTaskId();
            auto sel_task = sel.empty() ? std::nullopt : ctx->controller->GetTask(sel);
            bool has_sel = sel_task.has_value();
            bool is_completed = (has_sel && sel_task->state == TaskState::kCompleted);

            HMENU hMenu = CreatePopupMenu();

            // Group 1: Open / Navigate
            AppendMenuW(hMenu, tb.can_open_file ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_OPEN_FILE, L"Open File (Enter)");
            AppendMenuW(hMenu, tb.can_open_folder ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_OPEN_FOLDER, L"Show in Folder");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

            // Group 2: Transfer Controls
            AppendMenuW(hMenu, tb.can_start ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_START, L"Start Download");
            AppendMenuW(hMenu, tb.can_pause ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_PAUSE, L"Pause Download");
            AppendMenuW(hMenu, tb.can_resume ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_RESUME, L"Resume Download");
            AppendMenuW(hMenu, tb.can_retry ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_RETRY, L"Retry Download (F5)");
            AppendMenuW(hMenu, tb.can_cancel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_CANCEL, L"Cancel Download");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

            // Group 3: Task Management & URLs
            AppendMenuW(hMenu, has_sel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_RENAME, L"Rename File...");
            AppendMenuW(hMenu, has_sel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_MOVE_TO, L"Move To Folder...");
            AppendMenuW(hMenu, has_sel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_COPY_URL, L"Copy URL to Clipboard");
            AppendMenuW(hMenu, has_sel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_REFRESH_URL, L"Change / Refresh URL...");
            AppendMenuW(hMenu, has_sel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_ADD_MIRROR, L"Add Mirror URL...");
            AppendMenuW(hMenu, has_sel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_SCHEDULE, L"Schedule Download...");
            AppendMenuW(hMenu, has_sel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_SPEED_LIMIT, L"Set Speed Limit...");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

            // Group 4: Integrity & Security
            AppendMenuW(hMenu, (has_sel && is_completed) ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_FILE_INTEGRITY, L"File Integrity (SHA-256 Checksum)...");
            AppendMenuW(hMenu, tb.can_open_file ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_ANTIVIRUS, L"Scan with Antivirus (Windows Defender)...");
            AppendMenuW(hMenu, tb.can_open_file ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_ZIP_PREVIEW, L"Preview Archive Contents (ZIP)...");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

            // Group 5: Removal & Deletion
            AppendMenuW(hMenu, tb.can_remove ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_REMOVE_LIST, L"Remove from List (Delete)");
            AppendMenuW(hMenu, tb.can_delete_file ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_DELETE_DISK, L"Delete File... (Shift+Delete)");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

            // Group 6: Tasks Import/Export & Application
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_EXPORT_TASKS, L"Export Tasks (JSON / CSV)...");
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_OPEN_TORRENT, L"Open Torrent File...");
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_IMPORT_TASKS, L"Import Tasks from File...");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_SETTINGS, L"Settings & Configuration (Ctrl+,)...");
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_BROWSER_STAT, L"Browser Integration Diagnostics...");
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_ABOUT, L"About Radium Download Manager...");

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            if (cmd == IDM_CTX_START) ctx->view->ActionStartSelected();
            else if (cmd == IDM_CTX_PAUSE) ctx->view->ActionPauseSelected();
            else if (cmd == IDM_CTX_RESUME) ctx->view->ActionResumeSelected();
            else if (cmd == IDM_CTX_RETRY) ctx->view->ActionRetrySelected();
            else if (cmd == IDM_CTX_CANCEL) ctx->view->ActionCancelSelected();
            else if (cmd == IDM_CTX_OPEN_FILE) ctx->view->ActionOpenCompletedFile(false);
            else if (cmd == IDM_CTX_OPEN_FOLDER) ctx->view->ActionOpenDestinationFolder(false);
            else if (cmd == IDM_CTX_RENAME) {
                if (has_sel) {
                    std::wstring new_name;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Rename Download", L"Enter new file name:", Utf8ToW(sel_task->file_name), new_name)) {
                        std::string clean_name = PathSanitizer::SanitizeFileName(WToUtf8(new_name));
                        if (!clean_name.empty()) {
                            std::filesystem::path old_p = std::filesystem::path(sel_task->save_dir) / sel_task->file_name;
                            std::filesystem::path new_p = std::filesystem::path(sel_task->save_dir) / clean_name;
                            std::error_code ec;
                            if (std::filesystem::exists(old_p, ec)) {
                                std::filesystem::rename(old_p, new_p, ec);
                            }
                            sel_task->file_name = clean_name;
                            ctx->controller->UpdateTask(*sel_task);
                            ctx->view->SetFeedbackMessage("Renamed download to " + clean_name);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_MOVE_TO) {
                if (has_sel) {
                    std::string chosen_dir = sel_task->save_dir;
                    if (SelectFolderWithDialogWin32(hwnd, chosen_dir) && chosen_dir != sel_task->save_dir) {
                        std::filesystem::path old_p = std::filesystem::path(sel_task->save_dir) / sel_task->file_name;
                        std::filesystem::path new_p = std::filesystem::path(chosen_dir) / sel_task->file_name;
                        std::error_code ec;
                        if (std::filesystem::exists(old_p, ec)) {
                            std::filesystem::rename(old_p, new_p, ec);
                        }
                        sel_task->save_dir = chosen_dir;
                        ctx->controller->UpdateTask(*sel_task);
                        ctx->view->SetFeedbackMessage("Moved download to " + chosen_dir);
                    }
                }
            } else if (cmd == IDM_CTX_COPY_URL) {
                if (has_sel) {
                    CopyTextToClipboardWin32(hwnd, sel_task->url);
                    ctx->view->SetFeedbackMessage("Copied download URL to clipboard");
                }
            } else if (cmd == IDM_CTX_REFRESH_URL) {
                if (has_sel) {
                    std::wstring new_url_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Refresh / Change URL", L"Enter updated download URL (e.g. refreshed signed link):", Utf8ToW(sel_task->url), new_url_w)) {
                        std::string new_u = WToUtf8(new_url_w);
                        while (!new_u.empty() && (new_u.front() == ' ' || new_u.front() == '\t')) new_u.erase(0, 1);
                        while (!new_u.empty() && (new_u.back() == ' ' || new_u.back() == '\t')) new_u.pop_back();
                        if (!new_u.empty()) {
                            auto r = ctx->controller->RefreshTaskUrl(sel, new_u);
                            if (r.ok()) {
                                ctx->view->SetFeedbackMessage("Updated download URL");
                            } else {
                                MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Failed to Update URL", MB_OK | MB_ICONERROR);
                            }
                        }
                    }
                }
            } else if (cmd == IDM_CTX_ADD_MIRROR) {
                if (has_sel) {
                    std::wstring mirror_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Add Mirror URL", L"Enter alternate download mirror URL:", L"", mirror_w)) {
                        std::string mir = WToUtf8(mirror_w);
                        while (!mir.empty() && (mir.front() == ' ' || mir.front() == '\t')) mir.erase(0, 1);
                        while (!mir.empty() && (mir.back() == ' ' || mir.back() == '\t')) mir.pop_back();
                        if (!mir.empty()) {
                            auto r = ctx->controller->AddAlternateUrl(sel, mir);
                            if (r.ok()) {
                                ctx->view->SetFeedbackMessage("Added alternate mirror URL");
                            } else {
                                MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Failed to Add Mirror", MB_OK | MB_ICONERROR);
                            }
                        }
                    }
                }
            } else if (cmd == IDM_CTX_SPEED_LIMIT) {
                if (has_sel) {
                    uint64_t cur_limit = sel_task->speed_limit_bps;
                    std::wstring init_val = (cur_limit > 0) ? std::to_wstring(cur_limit / 1024) : L"0";
                    std::wstring val_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Set Download Speed Limit", L"Enter speed limit in KB/s (0 = Unlimited):", init_val, val_w)) {
                        std::string val_str = WToUtf8(val_w);
                        try {
                            uint64_t kbps = std::stoull(val_str);
                            uint64_t bps = kbps * 1024ULL;
                            auto r = ctx->controller->SetTaskSpeedLimit(sel, bps);
                            if (r.ok()) {
                                ctx->view->SetFeedbackMessage((bps == 0) ? "Task speed limit set to Unlimited" : ("Task speed limit set to " + DesktopView::FormatSpeed(bps)));
                            } else {
                                MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Speed Limit Failed", MB_OK | MB_ICONWARNING);
                            }
                        } catch (...) {
                            MessageBoxW(hwnd, L"Invalid speed limit number entered.", L"Invalid Input", MB_OK | MB_ICONWARNING);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_SCHEDULE) {
                if (has_sel) {
                    std::wstring init_t = sel_task->schedule_time_local.empty() ? L"23:00" : Utf8ToW(sel_task->schedule_time_local);
                    std::wstring time_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Schedule Download", L"Enter scheduled start time (24-hour HH:MM format, e.g. 23:00):", init_t, time_w)) {
                        std::string t_str = WToUtf8(time_w);
                        auto r = ctx->controller->SetTaskSchedule(sel, t_str, "");
                        if (r.ok()) {
                            ctx->view->SetFeedbackMessage("Scheduled download for " + t_str);
                        } else {
                            MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Schedule Failed", MB_OK | MB_ICONWARNING);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_FILE_INTEGRITY) {
                if (has_sel) {
                    std::filesystem::path fp = std::filesystem::path(sel_task->save_dir) / sel_task->file_name;
                    std::error_code ec;
                    if (!std::filesystem::exists(fp, ec)) {
                        MessageBoxW(hwnd, L"Completed file does not exist on disk.", L"File Missing", MB_OK | MB_ICONWARNING);
                    } else {
                        auto sha_res = CryptoUtils::ComputeFileSha256Hex(fp);
                        if (sha_res.ok()) {
                            std::string hash = *sha_res.value;
                            std::wstring msg_text = L"File: " + Utf8ToW(sel_task->file_name) + L"\n"
                                                  + L"SHA-256 Checksum:\n" + Utf8ToW(hash) + L"\n\n"
                                                  + L"Copy checksum to clipboard?";
                            if (MessageBoxW(hwnd, msg_text.c_str(), L"File Integrity (SHA-256)", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                                CopyTextToClipboardWin32(hwnd, hash);
                                ctx->view->SetFeedbackMessage("Copied SHA-256 hash to clipboard");
                            }
                        } else {
                            MessageBoxW(hwnd, Utf8ToW(sha_res.error->message).c_str(), L"Checksum Computation Error", MB_OK | MB_ICONERROR);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_REMOVE_LIST) {
                ctx->view->ActionRemoveSelectedFromList();
            } else if (cmd == IDM_CTX_DELETE_DISK) {
                ShowDeleteConfirmationDialogWin32(hwnd, *ctx->view, *ctx->controller);
            } else if (cmd == IDM_CTX_ANTIVIRUS) {
                if (!sel.empty()) {
                    auto rep = ctx->controller->ScanTaskFile(sel);
                    if (rep.ok()) {
                        std::wstring alert_info;
                        UINT icon = MB_ICONINFORMATION;
                        if (rep.value->status == security::ScanStatus::kClean) {
                            alert_info = L"Antivirus scan completed: Verified clean. No threats were found.";
                            icon = MB_ICONINFORMATION;
                        } else if (rep.value->status == security::ScanStatus::kThreatDetected) {
                            alert_info = L"SECURITY WARNING: A threat was detected by " +
                                std::wstring(rep.value->scanner_name.begin(), rep.value->scanner_name.end()) +
                                L"!\nAutomatic execution of this file is blocked.";
                            icon = MB_ICONSTOP;
                        } else if (rep.value->status == security::ScanStatus::kScannerError) {
                            alert_info = L"Antivirus scan failed: The scanner encountered an error during execution.";
                            icon = MB_ICONWARNING;
                        } else if (rep.value->status == security::ScanStatus::kUnavailable) {
                            alert_info = L"Antivirus scanner is unavailable on this system.";
                            icon = MB_ICONWARNING;
                        } else {
                            alert_info = L"Antivirus scan was skipped or not performed.";
                            icon = MB_ICONWARNING;
                        }
                        MessageBoxW(hwnd, alert_info.c_str(), L"Antivirus Scan Result", MB_OK | icon);
                    } else {
                        std::wstring err_msg = L"Failed to initiate antivirus scan: " +
                            std::wstring(rep.error->message.begin(), rep.error->message.end());
                        MessageBoxW(hwnd, err_msg.c_str(), L"Antivirus Scan Error", MB_OK | MB_ICONERROR);
                    }
                }
            } else if (cmd == IDM_CTX_ZIP_PREVIEW) {
                if (!sel.empty()) {
                    auto t = ctx->controller->GetTask(sel);
                    if (t.has_value()) {
                        std::filesystem::path fp = std::filesystem::path(t->save_dir) / t->file_name;
                        auto preview_res = ctx->controller->PreviewZipArchive(fp);
                        if (preview_res.ok()) {
                            std::wstring preview_info = L"Archive: " + fp.filename().wstring() + L"\n" +
                                                        L"Files: " + std::to_wstring(preview_res.value->total_files) + L"\n" +
                                                        L"Folders: " + std::to_wstring(preview_res.value->total_folders) + L"\n" +
                                                        L"Total Uncompressed: " + std::to_wstring(preview_res.value->total_uncompressed_bytes / 1024) + L" KB\n" +
                                                        (preview_res.value->is_potential_zip_bomb ? L"[ALERT: High compression ratio (Zip Bomb Risk)]\n" : L"") +
                                                        (preview_res.value->has_zip_slip_risk ? L"[ALERT: Suspicious paths (Zip Slip Risk)]\n" : L"");
                            MessageBoxW(hwnd, preview_info.c_str(), L"ZIP Archive Preview", MB_OK | MB_ICONINFORMATION);
                        } else {
                            MessageBoxW(hwnd, L"Could not read ZIP Central Directory (file may not be a valid ZIP archive).", L"ZIP Preview Error", MB_OK | MB_ICONEXCLAMATION);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_EXPORT_TASKS) {
                auto exp_str = ctx->controller->ExportTasksData(storage::ExportFormat::kJson);
                if (exp_str.ok()) {
                    std::filesystem::path exp_path = PathSanitizer::GetDefaultDownloadDir() / "radium_tasks_export.json";
                    std::ofstream ofs(exp_path);
                    if (ofs) {
                        ofs << exp_str.value.value();
                        std::wstring export_info = L"Tasks successfully exported to:\n" + exp_path.wstring();
                        MessageBoxW(hwnd, export_info.c_str(), L"Export Tasks", MB_OK | MB_ICONINFORMATION);
                    }
                }
            } else if (cmd == IDM_CTX_OPEN_TORRENT) {
                wchar_t szFile[MAX_PATH] = L"";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
                ofn.lpstrFilter = L"Torrent Files (*.torrent) *.torrent All Files (*.*) *.* ";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    std::string tor_path = WToUtf8(szFile);
                    auto parsed = torrent::TorrentParser::ParseTorrentFile(tor_path);
                    if (parsed.ok()) {
                        auto tinfo = std::move(*parsed.value);
                        std::string default_dir = ctx->controller->GetSettings().default_download_dir;
                        bool start_torrent = true;
                        if (ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, tinfo, tor_path, default_dir,
                                                      start_torrent, &default_dir, &start_torrent)) {
                            TaskModel t;
                            t.task_type = TaskType::kTorrent;
                            t.url = tor_path;
                            t.file_name = tinfo.name.empty() ? std::filesystem::path(tor_path).filename().string() : tinfo.name;
                            t.save_dir = default_dir;
                            t.total_bytes = tinfo.selected_bytes();
                            for (const auto& file : tinfo.files) {
                                t.torrent_file_paths.push_back(file.relative_path.generic_string());
                                t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                            }
                            auto res = ctx->controller->AddTask(std::move(t), start_torrent, false);
                            if (res.ok()) {
                                ctx->view->SelectTask(res.value->task_id);
                                ctx->view->ScrollTaskIntoView(res.value->task_id, true);
                            }
                        }
                    } else {
                        std::wstring err = L"Failed to parse torrent file:\n" + Utf8ToW(parsed.error->message);
                        MessageBoxW(hwnd, err.c_str(), L"Invalid Torrent", MB_OK | MB_ICONERROR);
                    }
                }
            } else if (cmd == IDM_CTX_IMPORT_TASKS) {
                wchar_t szFile[MAX_PATH] = L"";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
                ofn.lpstrFilter = L"Task Files (*.json;*.csv;*.txt)\0*.json;*.csv;*.txt\0JSON Files (*.json)\0*.json\0CSV / Text Files (*.csv;*.txt)\0*.csv;*.txt\0All Files (*.*)\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    std::filesystem::path imp_path(szFile);
                    std::ifstream ifs(imp_path, std::ios::binary);
                    if (ifs) {
                        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                        storage::ExportFormat fmt = (imp_path.extension() == ".json") ? storage::ExportFormat::kJson : storage::ExportFormat::kCsv;
                        auto imp_res = ctx->controller->ImportTasksData(content, fmt);
                        if (imp_res.ok()) {
                            std::wstring ok_msg = L"Successfully imported " + std::to_wstring(*imp_res.value) + L" task(s).";
                            MessageBoxW(hwnd, ok_msg.c_str(), L"Import Tasks", MB_OK | MB_ICONINFORMATION);
                        } else {
                            std::wstring err = L"Failed to import tasks: " + Utf8ToW(imp_res.error->message);
                            MessageBoxW(hwnd, err.c_str(), L"Import Error", MB_OK | MB_ICONERROR);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_SETTINGS) ctx->view->OpenSettingsCenter(SettingsCategory::kGeneral);
            else if (cmd == IDM_CTX_BROWSER_STAT) ctx->view->OpenSettingsCenter(SettingsCategory::kBrowserIntegration);
            else if (cmd == IDM_CTX_ABOUT) ctx->view->OpenSettingsCenter(SettingsCategory::kAboutAndDiagnostics);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t filePath[MAX_PATH];
                if (DragQueryFileW(hDrop, i, filePath, MAX_PATH) > 0) {
                    std::filesystem::path p(filePath);
                    std::string ext = p.extension().string();
                    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                    if (ext == ".torrent") {
                        // Route dropped torrents directly to dedicated Add Torrent dialog
                        std::string tor_path = WToUtf8(filePath);
                        auto parsed = torrent::TorrentParser::ParseTorrentFile(tor_path);
                        if (parsed.ok()) {
                            auto tinfo = std::move(*parsed.value);
                            std::string default_dir = ctx->controller->GetSettings().default_download_dir;
                            bool start_torrent = true;
                            if (ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, tinfo, tor_path, default_dir,
                                                          start_torrent, &default_dir, &start_torrent)) {
                                TaskModel t;
                                t.task_type = TaskType::kTorrent;
                                t.url = tor_path;
                                t.file_name = tinfo.name.empty() ? p.filename().string() : tinfo.name;
                                t.save_dir = default_dir;
                                t.total_bytes = tinfo.selected_bytes();
                                for (const auto& file : tinfo.files) {
                                    t.torrent_file_paths.push_back(file.relative_path.generic_string());
                                    t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                                }
                                auto res = ctx->controller->AddTask(std::move(t), start_torrent, false);
                                if (res.ok()) {
                                    ctx->view->SelectTask(res.value->task_id);
                                    ctx->view->ScrollTaskIntoView(res.value->task_id, true);
                                }
                            }
                        } else {
                            std::wstring err = L"Failed to parse torrent file:\n" + Utf8ToW(parsed.error->message);
                            MessageBoxW(hwnd, err.c_str(), L"Invalid Torrent", MB_OK | MB_ICONERROR);
                        }
                    } else if (ext == ".txt" || ext == ".csv" || ext == ".json") {
                        std::ifstream ifs(p);
                        if (ifs) {
                            std::stringstream ss_file;
                            ss_file << ifs.rdbuf();
                            std::string content = ss_file.str();
                            if (ext == ".json") {
                                ctx->controller->ImportTasksData(content, storage::ExportFormat::kJson);
                            } else if (ext == ".csv") {
                                ctx->controller->ImportTasksData(content, storage::ExportFormat::kCsv);
                            } else {
                                auto urls_res = storage::TaskImportExport::ImportFromUrlList(content);
                                if (urls_res.ok()) {
                                    for (auto& task_item : urls_res.value.value()) {
                                        ctx->controller->AddTask(std::move(task_item), true, false);
                                    }
                                }
                            }
                        }
                    } else {
                        std::string target_str = WToUtf8(filePath);
                        ShowNewDownloadDialogWin32(hwnd, *ctx->view, *ctx->controller, target_str, p.filename().string());
                    }
                }
            }
            DragFinish(hDrop);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_OPENIDM_SHOW_NEW_DOWNLOAD: {
            auto* offer = reinterpret_cast<OfferNotificationData*>(lParam);
            if (offer) {
                OPENIDM_LOG_INFO("UI", "DOWNLOAD_OFFER_RECEIVED: taskId=" + offer->task_id + " filename=" + offer->file_name);

                // Immediately bring the full Radium application to the foreground
                if (!IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_SHOW);
                }
                if (IsIconic(hwnd)) {
                    ShowWindow(hwnd, SW_RESTORE);
                }
                ctx->view->SetWindowVisibility(true, false);
                SetForegroundWindow(hwnd);
                BringWindowToTop(hwnd);
                PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);

                if (IsRemoteTorrentOffer(*offer)) {
                    OfferNotificationData torrent_offer = *offer;
                    delete offer;
                    const std::string torrent_task_id = torrent_offer.task_id;
                    try {
                        std::thread([hwnd, torrent_offer = std::move(torrent_offer)]() mutable {
                            auto* payload = new TorrentFetchPayload;
                            payload->offer = std::move(torrent_offer);
                            try {
                                auto fetched = FetchTorrentMetainfo(payload->offer);
                                if (fetched.ok()) {
                                    payload->metainfo_path = std::move(*fetched.value);
                                } else {
                                    payload->error_message = fetched.error->message;
                                }
                            } catch (...) {
                                payload->error_message = "Unexpected error while retrieving torrent metadata";
                            }
                            if (!PostMessageW(hwnd, WM_OPENIDM_TORRENT_FETCH_COMPLETE, 0,
                                              reinterpret_cast<LPARAM>(payload))) {
                                std::error_code ignored;
                                if (!payload->metainfo_path.empty()) {
                                    std::filesystem::remove_all(std::filesystem::path(payload->metainfo_path).parent_path(), ignored);
                                }
                                delete payload;
                            }
                        }).detach();
                    } catch (...) {
                        ctx->controller->CancelTask(torrent_task_id);
                        ctx->controller->RemoveTask(torrent_task_id, false);
                        MessageBoxW(hwnd, L"Could not start torrent metadata retrieval.", L"Torrent Error", MB_OK | MB_ICONERROR);
                    }
                    return 0;
                }

                bool is_offer_torrent = (offer->url.rfind(".torrent") != std::string::npos);
                std::error_code tor_ec;
                if (is_offer_torrent && std::filesystem::exists(offer->url, tor_ec)) {
                    auto parsed = torrent::TorrentParser::ParseTorrentFile(offer->url);
                    if (parsed.ok()) {
                        auto tinfo = std::move(*parsed.value);
                        std::string target_dir = offer->save_dir.empty() ? ctx->controller->GetSettings().default_download_dir : offer->save_dir;
                        if (ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, tinfo, offer->url, target_dir, true)) {
                            TaskModel t;
                            t.task_type = TaskType::kTorrent;
                            t.url = offer->url;
                            t.file_name = tinfo.name.empty() ? offer->file_name : tinfo.name;
                            t.save_dir = target_dir;
                            t.total_bytes = tinfo.selected_bytes();
                            for (const auto& file : tinfo.files) {
                                t.torrent_file_paths.push_back(file.relative_path.generic_string());
                                t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                            }
                            auto res = ctx->controller->AddTask(std::move(t), true, false);
                            if (res.ok()) {
                                ctx->view->SelectTask(res.value->task_id);
                                ctx->view->ScrollTaskIntoView(res.value->task_id, true);
                            }
                        }
                    } else {
                        ShowNewDownloadDialogWin32(
                            hwnd, *ctx->view, *ctx->controller, offer->url, offer->file_name, offer->save_dir, static_cast<int64_t>(offer->file_size), offer->task_id);
                    }
                } else {
                    ShowNewDownloadDialogWin32(
                        hwnd,
                        *ctx->view,
                        *ctx->controller,
                        offer->url,
                        offer->file_name,
                        offer->save_dir,
                        static_cast<int64_t>(offer->file_size),
                        offer->task_id);
                }
                delete offer;
            }
            return 0;
        }
        case WM_OPENIDM_TORRENT_FETCH_COMPLETE: {
            std::unique_ptr<TorrentFetchPayload> payload(reinterpret_cast<TorrentFetchPayload*>(lParam));
            if (!payload) return 0;

            auto remove_metainfo = [&]() {
                if (payload->metainfo_path.empty()) return;
                std::error_code ignored;
                std::filesystem::remove_all(std::filesystem::path(payload->metainfo_path).parent_path(), ignored);
            };
            auto remove_offer_task = [&]() {
                ctx->controller->CancelTask(payload->offer.task_id);
                ctx->controller->RemoveTask(payload->offer.task_id, false);
            };
            if (!payload->error_message.empty()) {
                remove_offer_task();
                std::wstring error = L"Could not retrieve the torrent file:\n" + Utf8ToW(payload->error_message);
                MessageBoxW(hwnd, error.c_str(), L"Torrent Download Error", MB_OK | MB_ICONERROR);
                return 0;
            }

            auto parsed = torrent::TorrentParser::ParseTorrentFile(payload->metainfo_path);
            if (!parsed.ok()) {
                remove_offer_task();
                remove_metainfo();
                std::wstring error = L"The downloaded file is not valid torrent metadata:\n" + Utf8ToW(parsed.error->message);
                MessageBoxW(hwnd, error.c_str(), L"Invalid Torrent", MB_OK | MB_ICONERROR);
                return 0;
            }
            auto existing = ctx->controller->GetTask(payload->offer.task_id);
            if (!existing.has_value()) {
                remove_metainfo();
                return 0;
            }

            auto tinfo = std::move(*parsed.value);
            std::string target_dir = payload->offer.save_dir.empty()
                ? ctx->controller->GetSettings().default_download_dir
                : payload->offer.save_dir;
            bool start_torrent = true;
            std::string selected_category;
            if (!ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, tinfo,
                                          payload->metainfo_path, target_dir, start_torrent,
                                          &target_dir, &start_torrent, &selected_category)) {
                remove_offer_task();
                remove_metainfo();
                return 0;
            }

            TaskModel task = *existing;
            task.task_type = TaskType::kTorrent;
            task.url = payload->metainfo_path;
            task.file_name = tinfo.name.empty() ? payload->offer.file_name : tinfo.name;
            task.category = selected_category.empty() ? "Torrents" : selected_category;
            task.save_dir = target_dir;
            task.part_path = std::filesystem::path(payload->metainfo_path).parent_path().string();
            task.total_bytes = tinfo.selected_bytes();
            task.downloaded_bytes = 0;
            task.headers.clear();
            task.auth_headers.clear();
            task.torrent_file_paths.clear();
            task.torrent_file_selection.clear();
            for (const auto& file : tinfo.files) {
                task.torrent_file_paths.push_back(file.relative_path.generic_string());
                task.torrent_file_selection.push_back(file.selected ? 1 : 0);
            }
            auto updated = ctx->controller->UpdateTask(task);
            if (!updated.ok()) {
                remove_offer_task();
                remove_metainfo();
                MessageBoxW(hwnd, Utf8ToW("Could not add torrent: " + updated.error->message).c_str(),
                            L"Torrent Error", MB_OK | MB_ICONERROR);
                return 0;
            }
            if (start_torrent) {
                auto started = ctx->controller->StartTask(task.task_id);
                if (!started.ok()) {
                    MessageBoxW(hwnd, Utf8ToW("Torrent was added but could not be started: " + started.error->message).c_str(),
                                L"Torrent Error", MB_OK | MB_ICONERROR);
                }
            }
            ctx->view->SelectTask(task.task_id);
            ctx->view->ScrollTaskIntoView(task.task_id, true);
            return 0;
        }
        case WM_OPENIDM_SELECT_TORRENT_METADATA: {
            auto* request = reinterpret_cast<TorrentMetadataSelectionRequest*>(lParam);
            if (request && request->info) {
                request->accepted = ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, *request->info);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == IDT_UI_REFRESH) {
                // Check for newly added or started downloads and auto-scroll into view
                ctx->view->CheckAndAutoScrollNewOrStartedDownloads();
                // Sync Windows taskbar progress indicator
                SyncTaskbarProgress(hwnd, ctx->pTaskbarList, *ctx->view, ctx);
                // Always repaint: active downloads, smooth animations, icon pulses
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wParam == VK_ESCAPE && ctx->view->IsSettingsCenterOpen()) {
                ctx->view->CloseSettingsCenter(true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ctrl_down && (wParam == 'K' || wParam == 'F')) {
                if (ctx->hSearchEdit) {
                    SetFocus(ctx->hSearchEdit);
                    SendMessageW(ctx->hSearchEdit, EM_SETSEL, 0, -1);
                }
                return 0;
            } else if (ctrl_down && wParam == 'N') {
                ShowAddUrlQuickDialogWin32(hwnd, *ctx->view, *ctx->controller);
            } else if (ctrl_down && wParam == VK_OEM_COMMA) {
                ctx->view->OpenSettingsCenter(SettingsCategory::kGeneral);
            } else if (wParam == VK_DOWN) ctx->view->HandleKeyShortcut("VK_DOWN");
            else if (wParam == VK_UP) ctx->view->HandleKeyShortcut("VK_UP");
            else if (wParam == VK_HOME) ctx->view->HandleKeyShortcut("VK_HOME");
            else if (wParam == VK_END) ctx->view->HandleKeyShortcut("VK_END");
            else if (wParam == VK_SPACE) ctx->view->HandleKeyShortcut("VK_SPACE");
            else if (wParam == VK_DELETE) {
                if (shift_down) {
                    ctx->view->HandleKeyShortcut("Shift+Delete");
                } else {
                    ctx->view->HandleKeyShortcut("VK_DELETE");
                }
            } else if (wParam == VK_F5) ctx->view->HandleKeyShortcut("VK_F5");
            else if (wParam == VK_RETURN) ctx->view->ActionOpenCompletedFile(false);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_OPENIDM_TRAYICON: {
            if (lParam == WM_RBUTTONUP) {
                POINT pt{};
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, L"Show Radium");
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_HIDE, L"Hide to Tray");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_PAUSE_ALL, L"Pause All Downloads");
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESUME_ALL, L"Resume All Downloads");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit Radium");
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == IDM_TRAY_SHOW) {
                    ShowWindow(hwnd, SW_RESTORE);
                    ctx->view->SetWindowVisibility(true, false);
                } else if (cmd == IDM_TRAY_HIDE) {
                    ShowWindow(hwnd, SW_HIDE);
                    ctx->view->SetWindowVisibility(false, false);
                } else if (cmd == IDM_TRAY_PAUSE_ALL) ctx->controller->PauseAllTasks();
                else if (cmd == IDM_TRAY_RESUME_ALL) ctx->controller->ResumeAllTasks();
                else if (cmd == IDM_TRAY_EXIT) DestroyWindow(hwnd);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_RESTORE);
                ctx->view->SetWindowVisibility(true, false);
                SetForegroundWindow(hwnd);
            }
            return 0;
        }
        case WM_POWERBROADCAST: {
            if (wParam == PBT_APMSUSPEND) {
                if (ctx && ctx->controller) {
                    ctx->controller->OnSystemSuspend();
                }
                return TRUE;
            } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                if (ctx && ctx->controller) {
                    ctx->controller->OnSystemResume();
                }
                return TRUE;
            }
            break;
        }
        case WM_CLOSE: {
            if (ctx->controller->GetSettings().close_to_tray && ctx->tray_added) {
                ShowWindow(hwnd, SW_HIDE);
                ctx->view->SetWindowVisibility(false, false);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_PAINT: {
            PaintMainWindow(hwnd, *ctx->view);
            return 0;
        }
        case WM_DESTROY: {
            KillTimer(hwnd, IDT_UI_REFRESH);
            // Clear taskbar progress and release COM
            if (ctx->pTaskbarList) {
                ctx->pTaskbarList->SetProgressState(hwnd, TBPF_NOPROGRESS);
                ctx->pTaskbarList->Release();
                ctx->pTaskbarList = nullptr;
            }
            ctx->taskbar_info_initialized = false;
            ctx->last_taskbar_info = {};
            if (ctx->com_initialized) {
                CoUninitialize();
                ctx->com_initialized = false;
            }
            if (ctx->tray_added) {
                Shell_NotifyIconW(NIM_DELETE, &ctx->nid);
                ctx->tray_added = false;
            }
            PostQuitMessage(0);
            return 0;
        }
    }
    if (g_msgTaskbarButtonCreated != 0 && msg == g_msgTaskbarButtonCreated) {
        // Explorer restarted or recreated taskbar buttons - re-acquire ITaskbarList3 and reset cached state
        if (ctx) {
            ctx->taskbar_info_initialized = false;
            ctx->last_taskbar_info = {};
            if (ctx->pTaskbarList) {
                ctx->pTaskbarList->Release();
                ctx->pTaskbarList = nullptr;
            }
            if (!ctx->com_initialized) {
                HRESULT hr_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                ctx->com_initialized = SUCCEEDED(hr_com) || (hr_com == RPC_E_CHANGED_MODE);
            }
            ITaskbarList3* pTbl = nullptr;
            HRESULT hr_tbl = CoCreateInstance(
                CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                IID_ITaskbarList3, reinterpret_cast<void**>(&pTbl));
            if (SUCCEEDED(hr_tbl) && pTbl) {
                if (SUCCEEDED(pTbl->HrInit())) {
                    ctx->pTaskbarList = pTbl;
                } else {
                    pTbl->Release();
                }
            }
            if (ctx->pTaskbarList && ctx->view) {
                SyncTaskbarProgress(hwnd, ctx->pTaskbarList, *ctx->view, ctx);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int DesktopView::RunWin32MessageLoop() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icex{sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icex);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HICON hAppIcon = CreateRadiumAppIcon();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = OpenIdmWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = hAppIcon;
    wc.lpszClassName = L"Radium_MainWindowClass";
    RegisterClassExW(&wc);

    Win32WindowContext ctx{this, &m_controller, nullptr, nullptr, {}, false};
    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Radium Download Manager",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        m_window_width, m_window_height,
        nullptr, nullptr, hInst, &ctx);
    if (!hwnd) return 1;
    ctx.hwnd = hwnd;

    BOOL use_dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &use_dark, sizeof(use_dark));
    COLORREF titlebar_col = RGB(0x16, 0x16, 0x18);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &titlebar_col, sizeof(titlebar_col));
    COLORREF titletext_col = RGB(0xFF, 0xFF, 0xFF);
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &titletext_col, sizeof(titletext_col));

    ctx.nid.cbSize = sizeof(NOTIFYICONDATAW);
    ctx.nid.hWnd = hwnd;
    ctx.nid.uID = 1;
    ctx.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    ctx.nid.uCallbackMessage = WM_OPENIDM_TRAYICON;
    ctx.nid.hIcon = wc.hIcon;
    wcsncpy_s(ctx.nid.szTip, L"Radium Download Manager", _TRUNCATE);
    ctx.tray_added = (Shell_NotifyIconW(NIM_ADD, &ctx.nid) != FALSE);

    SetWindowHwnd(hwnd);
    m_controller.SetTorrentMetadataSelectionCallback(
        [hwnd](torrent::TorrentInfo& info) {
            TorrentMetadataSelectionRequest request{&info, false};
            if (!IsWindow(hwnd)) {
                return Result<void>::Err("ERR_TORRENT_SELECTION_UNAVAILABLE", "The torrent file-selection window is unavailable");
            }
            SendMessageW(hwnd, WM_OPENIDM_SELECT_TORRENT_METADATA, 0, reinterpret_cast<LPARAM>(&request));
            return request.accepted
                ? Result<void>::Ok()
                : Result<void>::Err("ERR_TORRENT_SELECTION_CANCELED", "Torrent file selection canceled");
        });
    if (m_ipc_server) {
        m_ipc_server->SetOfferNotificationCallback(
            [hwnd, this](const TaskModel& task) {
                // Browser file offers are queued until the UI confirms them; `.torrent` offers are
                // routed to the metainfo fetch and dedicated file-selection dialog in the UI handler.
                auto* offer = new OfferNotificationData{
                    task.task_id, task.url, task.file_name, task.save_dir, task.total_bytes, task.headers, task.auth_headers};
                PostMessageW(hwnd, WM_OPENIDM_SHOW_NEW_DOWNLOAD, 0, reinterpret_cast<LPARAM>(offer));
            });
    }

    SetTimer(hwnd, IDT_UI_REFRESH, 200, nullptr);
    bool start_min = !m_start_visible;
    SetWindowVisibility(!start_min, start_min);
    ShowWindow(hwnd, start_min ? SW_HIDE : SW_SHOWNORMAL);
    if (!start_min) {
        SetForegroundWindow(hwnd);
    }
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
#endif

int DesktopView::RunApplicationLoop(bool headless_exit_immediately) {
    SelectFirstRow();
#ifdef _WIN32
    if (!headless_exit_immediately) {
        return RunWin32MessageLoop();
    }
#endif
    if (headless_exit_immediately) {
        return 0;
    }
    return 0;
}

} // namespace openidm::ui
