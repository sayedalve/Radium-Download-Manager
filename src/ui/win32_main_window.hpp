// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Windows 11 Cosmic Dark Desktop Interface, Hit-Testing,
// Interaction Controller, Cosmic Starfield & Professional Settings Center
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/download_controller.hpp"
#include "ipc/ipc_server.hpp"
#include "ui/design_system.hpp"
#include "common/version.hpp"

namespace openidm::ui {

enum class SidebarCategory {
    kAllDownloads = 0,
    kInProgress,
    kQueued,
    kCompleted,
    kFailed,
    kVideoDownloads,
    kScheduled
};

enum class SortColumn {
    kCreatedAt = 0,
    kFileName,
    kDownloadType,
    kStatus,
    kProgress,
    kSize,
    kSpeed,
    kEta
};

enum class ToolbarActionId {
    kAddUrl = 0,
    kStart,
    kPause,
    kResume,
    kRetry,
    kCancel,
    kRemove,
    kClearHistory,
    kOpenFolder,
    kSettings,
    kStartAll,
    kPauseAll
};

enum class SettingsCategory {
    kGeneral = 0,
    kDownloads,
    kSaveLocations,
    kConnectionsAndSpeed,
    kBrowserIntegration,
    kFileTypes,
    kNotifications,
    kAboutAndDiagnostics,
    kKeyboardShortcuts
};

enum class SettingControlType {
    kToggleSwitch = 0,
    kNumericStepper,
    kDropdown,
    kFolderSelector,
    kDiagnosticReadOnly
};

enum class HitTargetType {
    kNone = 0,
    kToolbarButton,
    kSearchBox,
    kSidebarCategory,
    kTableHeaderColumn,
    kTaskRow,
    kTaskRowActionButton,
    kSettingsCategoryTab,
    kSettingsItemControl,
    kSettingsApplyButton,
    kSettingsCancelButton,
    kSettingsRestoreDefaultsButton,
    kSettingsModalBackground,
    kSettingsBackdrop,
    kSelectAllCheckbox,
    kSortDropdown,
    kFilterDropdown,
    kAddDownloadButton,
    kSearchSlidersButton,
    kStatusToggleOff,
    kRowCheckbox,
    kRowInlineAction
};

struct HitTestResult {
    HitTargetType type{HitTargetType::kNone};
    int index{-1};
    std::string task_id;
    std::string tooltip_text;
};

struct ToolbarButtonLayout {
    ToolbarActionId action_id;
    std::string label;
    bool enabled{false};
    bool is_primary{false};
    RectI rect;
};

struct SidebarCategoryLayout {
    SidebarCategory category;
    std::string label;
    size_t count{0};
    bool selected{false};
    RectI rect;
};

struct TableColumnLayout {
    SortColumn sort_column;
    std::string title;
    RectI rect;
};

struct TaskRowLayout {
    std::string task_id;
    RectI row_rect;
    RectI filename_rect;
    RectI progress_bar_rect;
    RectI action_button_rect;
    RectI checkbox_rect;
    RectI action_icon_rect;
};

struct WindowLayoutGeometry {
    RectI toolbar_rect;
    RectI search_box_rect;
    RectI sidebar_rect;
    RectI table_header_rect;
    RectI table_body_rect;
    RectI details_panel_rect;
    RectI status_bar_rect;
    RectI select_all_checkbox_rect;
    RectI sort_dropdown_rect;
    RectI filter_dropdown_rect;
    RectI add_download_button_rect;
    RectI settings_button_rect;
    RectI search_sliders_rect;
    RectI status_toggle_off_rect;
    std::vector<ToolbarButtonLayout> toolbar_buttons;
    std::vector<SidebarCategoryLayout> sidebar_items;
    std::vector<TableColumnLayout> table_columns;
    std::vector<TaskRowLayout> task_rows;
};

struct ToolbarState {
    bool can_add_url{true};
    bool can_start{false};
    bool can_pause{false};
    bool can_resume{false};
    bool can_retry{false};
    bool can_cancel{false};
    bool can_remove{false};
    bool can_delete_file{false};
    bool can_open_file{false};
    bool can_open_folder{false};
    bool can_clear_history{false};
    bool can_open_settings{true};
    bool can_start_all{true};
    bool can_pause_all{true};
};

struct CategoryCounts {
    size_t all_downloads{0};
    size_t in_progress{0};
    size_t queued{0};
    size_t completed{0};
    size_t failed{0};
    size_t video_downloads{0};
    size_t scheduled{0};
};

struct FormattedTaskRow {
    std::string task_id;
    TaskState state{TaskState::kQueued};
    std::string category{"General"};
    std::string display_filename;
    std::string full_filename_tooltip;
    std::string full_status_tooltip;
    bool is_filename_truncated{false};
    std::string file_type_badge;
    bool is_stream{false};
    std::string status_text;
    std::string progress_text;
    double progress_fraction{0.0};
    bool progress_indeterminate{false};
    std::string size_text;
    std::string speed_text;
    std::string upload_speed_text{"0 B/s"};
    std::string added_time_text;
    std::string eta_text;
    std::string primary_action_label;
    ColorRgba status_indicator_color;
    ColorRgba progress_fill_color;
    bool selected{false};
    bool checked{false};
};

enum class TaskbarProgressState {
    kNoProgress = 0,
    kIndeterminate,
    kNormal,
    kError,
    kPaused
};

struct TaskbarProgressInfo {
    TaskbarProgressState state{TaskbarProgressState::kNoProgress};
    uint64_t completed_bytes{0};
    uint64_t total_bytes{0};
    double progress_fraction{0.0};
};

struct TaskDetailsViewModel {
    bool has_selection{false};
    std::string task_id;
    std::string file_name;
    std::string redacted_source_url;
    std::string redacted_page_url;
    std::string destination_path;
    std::string size_summary;
    std::string progress_summary;
    std::string speed_summary;
    std::string created_time_str;
    std::string updated_time_str;
    std::string completed_time_str;
    uint32_t retry_count{0};
    std::string engine_name;
    std::string stream_format;
    std::string selected_tracks_summary;
    std::string redacted_headers_summary;
    std::string error_code;
    std::string error_explanation;
    std::string speed_limit_summary;
    std::string alternate_urls_summary;
    std::string schedule_summary;
};

struct StatusBarViewModel {
    size_t active_downloads{0};
    uint64_t total_speed_bps{0};
    std::string formatted_total_speed;
    uint64_t total_upload_speed_bps{0};
    std::string formatted_total_upload_speed;
    size_t remaining_queued{0};
    bool browser_ipc_listening{false};
    bool browser_capture_enabled{true};
    bool workers_ready{false};
    std::string speed_limit_state;
    std::string status_message;
};

struct AddDownloadDialogInput {
    std::string url;
    std::string custom_filename;
    std::string save_dir;
    uint32_t max_connections{8};
    bool start_immediately{true};
    bool scheduled{false};
    std::string schedule_time_local;
    bool is_stream{false};
    std::string video_filter{"best"};
    std::string audio_filter{"best"};
    std::string subtitle_filter{"all"};
    std::string output_format{"mp4"};
    uint64_t total_bytes{0};
};

struct BrowserIntegrationStatusView {
    bool ipc_server_running{false};
    std::string ipc_endpoint;
    bool capture_enabled{true};
    bool nm3u8dl_available{false};
    std::string nm3u8dl_path;
    bool ffmpeg_available{false};
    std::string ffmpeg_path;
    std::vector<std::string> supported_browsers;
};

struct AboutDialogView {
    std::string app_name{"Radium Download Manager"};
    std::string version{FLUX_VERSION_STRING};
    std::string license{"Apache-2.0"};
    std::string target_platform{"Windows 11 x64 (Native Win32 + Direct2D/GDI)"};
    std::vector<std::string> upstream_attributions;
};

struct FileCategoryRuleView {
    std::string category_name;
    std::string extensions_display;
    std::string target_subfolder;
};

struct SettingItemView {
    std::string key;
    SettingsCategory category{SettingsCategory::kGeneral};
    std::string group_title;
    std::string label;
    std::string description;
    SettingControlType control_type{SettingControlType::kToggleSwitch};
    std::string value_text;
    std::string default_value_text;
    bool bool_value{false};
    bool is_modified{false};
    bool is_advanced{false};
    std::string validation_error;
    RectI row_rect;
    RectI control_rect;
};

struct SettingsCategoryTabView {
    SettingsCategory category{SettingsCategory::kGeneral};
    std::string label;
    size_t item_count{0};
    size_t modified_count{0};
    bool selected{false};
    RectI rect;
};

struct SettingsCenterViewModel {
    bool is_open{false};
    SettingsCategory active_category{SettingsCategory::kGeneral};
    std::string search_query;
    bool has_unsaved_changes{false};
    bool is_valid{true};
    std::string validation_error;
    std::string status_banner;
    std::vector<SettingsCategoryTabView> categories;
    std::vector<SettingItemView> visible_items;
    std::vector<FileCategoryRuleView> file_category_rules;
    BrowserIntegrationStatusView browser_status;
    AboutDialogView about_info;
    RectI modal_rect;
    RectI sidebar_rect;
    RectI content_rect;
    RectI search_box_rect;
    RectI restore_defaults_rect;
    RectI cancel_button_rect;
    RectI apply_button_rect;
};

class DesktopView {
public:
    explicit DesktopView(DownloadController& controller, ipc::IpcServer* ipc_server = nullptr);

    // Layout, Theme, DPI & Hit-Testing Geometry
    void SetWindowDimensions(int width, int height, uint32_t dpi = 96);
    int GetWindowWidth() const { return m_window_width; }
    int GetWindowHeight() const { return m_window_height; }
    const LayoutMetrics& GetMetrics() const { return m_metrics; }
    const ThemePalette& GetPalette() const { return m_palette; }
    bool IsCompactColumnsMode() const { return m_window_width < m_metrics.compact_width_threshold; }
    std::vector<std::string> GetVisibleColumns() const;

    // Lightweight Native UI Animation System
    UiAnimationState& GetAnimationState() { return m_anim; }
    const UiAnimationState& GetAnimationState() const { return m_anim; }
    void SetWindowVisibility(bool visible, bool minimized = false);
    void TriggerActionPulse(const std::string& task_id, const std::string& action);

    // Windows Taskbar Download Progress Computation
    TaskbarProgressInfo ComputeTaskbarProgress() const;

    WindowLayoutGeometry ComputeLayoutGeometry(size_t scroll_offset = 0) const;
    HitTestResult HitTestPoint(int x, int y, size_t scroll_offset = 0) const;
    bool HandleMouseClick(int x, int y, bool double_click = false, size_t scroll_offset = 0);
    HitTestResult HandleMouseMove(int x, int y, size_t scroll_offset = 0);
    void ClearHoverState();
    RectI GetRowRect(size_t row_index) const;
    const std::string& GetActiveTooltip() const { return m_active_tooltip; }
    int GetHoveredToolbarIndex() const { return m_hovered_toolbar_index; }
    int GetHoveredSidebarIndex() const { return m_hovered_sidebar_index; }
    int GetHoveredRowIndex() const { return m_hovered_row_index; }

    void SetTheme(std::string_view theme_name, bool os_prefers_light = false);
    std::string GetThemeName() const { return m_theme_name; }

    static std::string FormatBytes(uint64_t bytes);
    static std::string FormatSpeed(uint64_t bps);
    static std::string FormatBandwidthLimit(uint64_t bps);
    static std::string FormatEta(uint64_t remaining_bytes, uint64_t speed_bps);
    static std::string TruncateFilenameGracefully(std::string_view filename, size_t max_chars = 38);
    static std::string FormatFriendlyTime(int64_t epoch_ms);
    static std::string FormatSizePretty(uint64_t bytes);
    static std::string FormatSpeedKbps(uint64_t bps);

    bool IsSelectAllChecked() const { return m_select_all_checked; }
    void ToggleSelectAll();
    bool IsTaskChecked(const std::string& task_id) const { return m_checked_task_ids.count(task_id) > 0; }
    void ToggleTaskChecked(const std::string& task_id);
    void SetTaskChecked(const std::string& task_id, bool checked);
    void ClearCheckedTasks() { m_checked_task_ids.clear(); }
    const std::set<std::string>& GetCheckedTaskIds() const { return m_checked_task_ids; }
    void CycleSortDropdown();
    void CycleFilterDropdown();
    bool IsSpeedLimiterActive() const { return m_speed_limiter_active; }
    void ToggleSpeedLimiter() { m_speed_limiter_active = !m_speed_limiter_active; }

    // Sidebar Category Filtering, Sorting, Search & Selection
    void SelectCategory(SidebarCategory category);
    SidebarCategory GetSelectedCategory() const { return m_selected_category; }
    static std::string CategoryToLabel(SidebarCategory category);
    CategoryCounts ComputeCategoryCounts() const;
    bool IsSidebarVisible() const { return m_sidebar_visible; }
    void SetSidebarVisible(bool visible);
    void ToggleSidebarVisible();

    void SetSearchQuery(std::string query);
    const std::string& GetSearchQuery() const { return m_search_query; }

    void SetSort(SortColumn column, bool ascending);
    void ToggleSortColumn(SortColumn column);
    SortColumn GetSortColumn() const { return m_sort_column; }
    bool IsSortAscending() const { return m_sort_ascending; }

    void SelectTask(const std::string& task_id);
    std::string GetSelectedTaskId() const { return m_selected_task_id; }
    void SelectNextRow();
    void SelectPrevRow();
    void SelectFirstRow();
    void SelectLastRow();

    // Table Scrolling & Column Sizing
    size_t GetScrollOffset() const { return m_scroll_offset; }
    void SetScrollOffset(size_t offset) { m_scroll_offset = offset; }
    void ScrollBy(int delta_rows);
    bool ScrollTaskIntoView(const std::string& task_id, bool select_row = true);
    bool CheckAndAutoScrollNewOrStartedDownloads();

    int GetColumnWidth(size_t col_idx) const;
    void SetColumnWidth(size_t col_idx, int width);
    bool IsResizingColumn() const { return m_resizing_col_index >= 0; }
    int GetResizingColumnIndex() const { return m_resizing_col_index; }
    void StartColumnResize(int col_idx, int mouse_x);
    void UpdateColumnResize(int mouse_x);
    void EndColumnResize();
    int HitTestColumnDivider(int x, int y, size_t scroll_offset = 0) const;
    void SaveColumnWidthsToSettings();
    void LoadColumnWidthsFromSettings();

    void SetStartVisible(bool visible) { m_start_visible = visible; }
    bool GetStartVisible() const { return m_start_visible; }

#ifdef _WIN32
    void SetWindowHwnd(HWND hwnd) { m_hwnd = hwnd; }
    HWND GetWindowHwnd() const { return m_hwnd; }
#endif

    // Computed View Models
    std::vector<TaskModel> GetFilteredAndSortedTasks() const;
    std::vector<FormattedTaskRow> BuildVisibleTaskRows(size_t scroll_offset = 0, size_t max_visible_rows = 100) const;
    ToolbarState ComputeToolbarState() const;
    TaskDetailsViewModel BuildSelectedTaskDetails() const;
    StatusBarViewModel BuildStatusBar() const;
    std::string GetEmptyStateMessage() const;

    // Professional Download Manager Settings Center
    void OpenSettingsCenter(SettingsCategory initial_category = SettingsCategory::kGeneral);
    void CloseSettingsCenter(bool revert_unsaved = true);
    bool IsSettingsCenterOpen() const { return m_settings_center_open; }
    void SelectSettingsCategory(SettingsCategory category);
    SettingsCategory GetSelectedSettingsCategory() const { return m_settings_category; }
    static std::string SettingsCategoryToLabel(SettingsCategory category);
    void SetSettingsSearchQuery(std::string query);
    const std::string& GetSettingsSearchQuery() const { return m_settings_search_query; }
    const AppSettings& GetDraftSettings() const { return m_draft_settings; }
    void SetDraftSettings(const AppSettings& draft);
    bool HasUnsavedSettingsChanges() const;
    Result<void> SetDraftSettingByKey(std::string_view key, std::string_view value_str);
    Result<void> CycleOrToggleDraftSetting(std::string_view key);
    Result<void> ValidateSettingsInput(const AppSettings& candidate) const;
    Result<void> ApplySettingsChanges();
    void RevertSettingsChanges();
    SettingsCenterViewModel BuildSettingsCenterView() const;

    // All Interactive Actions & Dialog Controllers
    Result<TaskModel> SubmitAddDownloadDialog(const AddDownloadDialogInput& input);
    Result<stream::StreamProbeResult> ProbeStreamForDialog(
        const std::string& manifest_url,
        const std::map<std::string, std::string>& headers = {},
        const std::map<std::string, std::string>& auth_headers = {});
    Result<TaskModel> ConfirmStreamSelectionDialog(
        const stream::StreamProbeResult& probe_result,
        const std::string& page_title,
        const std::string& save_dir,
        const StreamSelectionOptions& chosen_options,
        const std::map<std::string, std::string>& headers = {},
        const std::map<std::string, std::string>& auth_headers = {});

    Result<void> TriggerToolbarAction(ToolbarActionId action_id);
    Result<void> TriggerRowPrimaryAction(const std::string& task_id);

    Result<void> ActionStartAll();
    Result<void> ActionPauseAll();
    Result<void> ActionStartSelected();
    Result<void> ActionPauseSelected();
    Result<void> ActionResumeSelected();
    Result<void> ActionRetrySelected();
    Result<void> ActionCancelSelected();
    Result<void> ActionRemoveSelectedFromList();
    Result<void> ActionDeleteSelectedWithFile();
    Result<void> ActionMoveSelectedToRecycleBin();
    Result<int> ActionClearCompletedHistory();
    Result<std::filesystem::path> ActionOpenCompletedFile(bool dry_run = false);
    Result<std::filesystem::path> ActionOpenDestinationFolder(bool dry_run = false);

    void ActionSetConcurrentLimit(uint32_t max_tasks);
    void ActionSetSpeedLimit(uint64_t bps);
    void ActionSetBrowserCapture(bool enabled);
    Result<void> ActionSaveSettings(const AppSettings& settings);
    Result<AppSettings> ActionRestoreDefaultSettings();

    Result<void> ActionSetTaskSpeedLimit(const std::string& task_id, uint64_t bps);
    Result<void> ActionAddAlternateUrl(const std::string& task_id, const std::string& alt_url);
    Result<void> ActionSetTaskSchedule(const std::string& task_id, const std::string& start_time_local, const std::string& stop_time_local);
    Result<std::vector<crawler::GrabbedItem>> ActionLaunchSiteGrabber(
        const crawler::SiteGrabberConfig& cfg,
        std::atomic<bool>& cancel_requested,
        crawler::GrabbedItemCallback on_item_found = nullptr);

    bool HandleKeyShortcut(const std::string& key_chord);

    BrowserIntegrationStatusView GetBrowserIntegrationStatus() const;
    AboutDialogView GetAboutDialogView() const;
    const std::string& GetLastFeedbackMessage() const { return m_last_feedback_message; }
    void SetFeedbackMessage(std::string msg) { m_last_feedback_message = std::move(msg); }

    std::string RenderQueueTableText() const;
    std::string RenderFullDesktopSnapshotText() const;

    int RunApplicationLoop(bool headless_exit_immediately = false);

#ifdef _WIN32
    int RunWin32MessageLoop();
#endif

private:
    bool MatchesCategory(const TaskModel& t, SidebarCategory cat) const;
    bool MatchesSearch(const TaskModel& t) const;
    std::vector<SettingItemView> BuildAllSettingItems() const;

    DownloadController& m_controller;
    ipc::IpcServer* m_ipc_server{nullptr};

    int m_window_width{1180};
    int m_window_height{720};
    LayoutMetrics m_metrics;
    std::string m_theme_name{"coral_dark"};
    ThemePalette m_palette;
    UiAnimationState m_anim;

    SidebarCategory m_selected_category{SidebarCategory::kAllDownloads};
    SortColumn m_sort_column{SortColumn::kCreatedAt};
    bool m_sort_ascending{false};
    std::string m_search_query;
    std::string m_selected_task_id;
    std::string m_last_feedback_message{"Ready"};

    bool m_settings_center_open{false};
    SettingsCategory m_settings_category{SettingsCategory::kGeneral};
    std::string m_settings_search_query;
    AppSettings m_draft_settings;
    std::string m_settings_validation_key;
    std::string m_settings_validation_error;

    int m_hovered_toolbar_index{-1};
    int m_hovered_sidebar_index{-1};
    int m_hovered_row_index{-1};
    std::string m_active_tooltip;
    bool m_select_all_checked{false};
    std::set<std::string> m_checked_task_ids;
    bool m_speed_limiter_active{false};
    bool m_sidebar_visible{false};

    size_t m_scroll_offset{0};
    mutable std::map<std::string, TaskState> m_known_task_states;
    mutable bool m_known_tasks_initialized{false};
    mutable std::vector<int> m_column_widths;
    int m_resizing_col_index{-1};
    int m_resize_start_x{0};
    int m_resize_orig_width{0};
    bool m_start_visible{true};
#ifdef _WIN32
    HWND m_hwnd{nullptr};
#endif
};

#ifdef _WIN32
void ShowDeleteConfirmationDialogWin32(HWND hwnd_parent, DesktopView& view, DownloadController& controller);
bool ShowSimpleInputDialogWin32(HWND hwnd_parent, const std::wstring& title, const std::wstring& prompt, const std::wstring& initial_value, std::wstring& out_value);
void CopyTextToClipboardWin32(HWND hwnd, const std::string& text);
#endif

} // namespace openidm::ui
