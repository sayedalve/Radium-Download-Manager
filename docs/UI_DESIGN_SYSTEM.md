# Radium Desktop UI Design System (`src/ui/design_system.hpp` & `src/ui/win32_main_window.cpp`)

## 1. Visual Direction & Design Philosophy

Flux Download Manager's desktop interface is engineered as a native, high-performance Windows 11 desktop application (`WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN`) rendered via double-buffered Win32 GDI and Direct2D primitives (`src/ui/design_system.hpp`, `src/ui/win32_main_window.cpp`) and backed by a platform-independent view-model (`openidm::ui::DesktopView`).

Key design principles:
- **Preserved Radium Brand Identity**: Warm matte charcoal canvas (`#161618`), dark charcoal surfaces (`#1E1E22`), elevated floating cards (`#28282E`), subtle architectural borders (`#35353D`), crisp light typography (`#F2F2F3` / `#B4B4BD`), and the signature **Coral Red** accent (`#FF4757`).
- **Authentic Glassmorphism Architecture**:
  - Genuine translucency rendered via Windows 11 Desktop Window Manager (DWM) composition backdrops: Mica (`DWMWA_SYSTEMBACKDROP_TYPE = 2`) on the main window and Acrylic (`DWMWA_SYSTEMBACKDROP_TYPE = 3`) on transient dialogs, with `DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND (33 = 2)`.
  - True 32-bit premultiplied ARGB DIBSection composition blended onto the destination device context using Win32 `msimg32.lib`'s `AlphaBlend`. Never uses fake solid lighter colors in place of transparency.
  - Restrained 1px specular top-edge highlights (`ColorRgba{255, 255, 255, 22}`) mimicking natural glass refraction.
- **Strict Resource Discipline**: Double-buffered `WM_PAINT` updates only on state change or a 100 ms UI timer (`IDT_UI_REFRESH`) without querying SQLite on the paint thread.
- **Zero Heavy Runtime Bloat**: Zero Electron, Qt, or Webview2 overhead. Built entirely in native ISO C++20 and Win32 APIs.

---

## 2. Color Tokens & Glass Palette (`openidm::ui::ThemePalette`)

Radium provides three theme modes (`ThemeMode::kDark`, `ThemeMode::kLight`, and `ThemeMode::kSystem`), persisted in SQLite (`settings.theme`) and synchronized with Windows 11 `DWMWA_USE_IMMERSIVE_DARK_MODE`. The default `coral_dark` and its `coral`, `charcoal`, and `dark` aliases resolve to the Coral Dark palette; `classic_blue` and `cosmic_blue` remain explicit legacy compatibility themes.

| Token Name | Dark Theme (Default) | Redesign Theme | Light Theme | Purpose |
|---|---|---|---|---|
| `app_background` | `#161618` | `#161618` | `#F8FAFC` | Main window canvas and calm static backdrop |
| `surface_primary` | `#1E1E22` | `#1E1E22` | `#FFFFFF` | Main toolbar container and search control |
| `surface_elevated` | `#28282E` | `#28282E` | `#F1F5F9` | Secondary toolbar buttons, dialog cards, and status bar |
| `surface_hover` | `#2B2B32` | `#2B2B32` | `#E2E8F0` | Interactive row, action button, and tab hover state |
| `surface_selected` | `#302328` | `#302328` | `#DBEAFE` | Selected download row and active settings category |
| `border_subtle` | `#2E2F35` | `#2E2F35` | `#CBD5E1` | 1px structural dividers and panel borders |
| `border_focus` | `#FF4757` | `#FF4757` | `#FF4757` | 2px keyboard focus ring and active input highlight |
| `text_primary` | `#F2F2F3` | `#F2F2F3` | `#0F172A` | Primary filenames, dialog titles, and active labels |
| `text_secondary` | `#B4B4BD` | `#B4B4BD` | `#475569` | Column headers, transfer sizes, timestamps, and details metadata |
| `text_muted` | `#85858F` | `#85858F` | `#94A3B8` | Disabled buttons, hints, and secondary units |
| `accent_primary` | `#FF4757` | `#FF4757` | `#FF4757` | Signature Radium Coral Red action accent and progress fills |
| `accent_hover` | `#FF5F6A` | `#FF5F6A` | `#FF353E` | Primary action button hover state |
| `glass_surface` | `rgba(30,30,34, 180)` | `rgba(30,30,34, 180)` | `rgba(255,255,255, 180)` | Translucent glass panel fill (AlphaBlend DIB) |
| `glass_elevated` | `rgba(40,40,46, 210)` | `rgba(40,40,46, 210)` | `rgba(240,244,248, 210)` | Translucent floating card and modal dialog card fill |
| `glass_border` | `rgba(255,255,255, 20)` | `rgba(255,255,255, 20)` | `rgba(0,0,0, 18)` | Fine low-contrast glass perimeter border |
| `glass_highlight` | `rgba(255,255,255, 26)` | `rgba(255,255,255, 26)` | `rgba(255,255,255, 45)` | Refractive top-edge specular highlight |
| `status_success` | `#A9B4C2` | `#A9B4C2` | `#7D8A99` | `Completed` state indicator and fill |
| `status_warning` | `#F2B84B` | `#F2B84B` | `#D97706` | `Paused` / scheduled state indicator and fill |
| `status_danger` | `#FF9F43` | `#FF9F43` | `#D97A24` | `Failed` / canceled state indicator and fill |
| `progress_track` | `#303036` | `#303036` | `#E2E8F0` | Unfilled progress bar track |

### 2.1 Interaction and download-state behavior

- `Downloading`, probing, merging, verifying, and finalizing use the Coral accent for their status indicator and progress fill.
- `Completed` uses muted neutral `#A9B4C2`; `Paused` and scheduled work use `#F2B84B`; failed and canceled work uses orange `#FF9F43`; queued work uses the muted neutral indicator and track.
- Hovered rows use `surface_hover`. Selected rows use the warm `surface_selected` fill plus a narrow Coral indicator on the left edge, preserving readable status text.
- Toolbar controls use `surface_primary`/`surface_elevated` for normal states, `surface_hover` for hover, `surface_pressed` for pressed, and `border_focus` for keyboard focus. Disabled controls retain readable muted text.

---

## 3. Spacing, Layout Regions & High-DPI Scaling (`openidm::ui::LayoutMetrics`)

All layout dimensions use a **4px base grid** and scale proportionally across Per-Monitor V2 DPI settings (`96` DPI / 100%, `120` DPI / 125%, `144` DPI / 150%, `192` DPI / 200%, `240` DPI / 250%) via `DesignSystem::ScaleForDpi(base_px, dpi)`.

| Metric | 100% (`96 DPI`) | 125% (`120 DPI`) | 150% (`144 DPI`) | 200% (`192 DPI`) | Notes |
|---|---|---|---|---|---|
| `min_window_width` | `900 px` | `1125 px` | `1350 px` | `1800 px` | Enforced via `WM_GETMINMAXINFO` |
| `min_window_height` | `560 px` | `700 px` | `840 px` | `1120 px` | Enforced via `WM_GETMINMAXINFO` |
| `compact_width_threshold` | `1024 px` | `1280 px` | `1536 px` | `2048 px` | Below this width, `Type` and `ETA` columns auto-hide |
| `toolbar_height` | `48 px` | `60 px` | `72 px` | `96 px` | Top action bar |
| `sidebar_width` | `216 px` | `270 px` | `324 px` | `432 px` | Left navigation category pane |
| `table_header_height` | `34 px` | `43 px` | `51 px` | `68 px` | Sortable column header bar |
| `table_row_height` | `44 px` | `55 px` | `66 px` | `88 px` | Virtualized task row + 6px progress bar |
| `details_panel_height` | `188 px` | `235 px` | `282 px` | `376 px` | Bottom inspector panel |
| `status_bar_height` | `28 px` | `35 px` | `42 px` | `56 px` | Bottom application status bar |

### Typography Ramp
- **Font Family**: `Segoe UI Variable Display` / `Segoe UI` (Windows 11 native system font).
- **Caption / Badge Floor**: `11 px` (`font_caption_px`) — never smaller than 11px at 100% DPI.
- **Body / Table Cell Floor**: `13 px` (`font_body_px` Regular, `font_body_strong_px` Semibold).
- **Section Subtitle**: `15 px` (`font_subtitle_px` Semibold).
- **Dialog Title**: `18 px` (`font_title_px` Semibold).

---

## 4. Main Window Architecture & Control States

### A. Context-Sensitive Main Toolbar (`ToolbarState`)
The toolbar exposes 10 actions (`+ Add URL`, `Start`, `Pause`, `Resume`, `Retry`, `Cancel`, `Remove`, `Clear History`, `Open Folder`, `Settings`).
- Every control's enabled/disabled state is dynamically computed by `DesktopView::ComputeToolbarState()` based on the currently selected task's state (`kQueued`, `kProbing`, `kDownloading`, `kMuxing`, `kPaused`, `kCompleted`, `kFailed`, `kCanceled`).
- No decorative placeholder buttons exist.

### B. Navigation Sidebar (`SidebarCategory` & `CategoryCounts`)
Displays 7 categories with live badge counts computed in a single pass over `DownloadController::ListTasks()`:
1. `All Downloads`
2. `In Progress` (`PROBING`, `DOWNLOADING`, `MUXING`, `VERIFYING`)
3. `Queued` (`QUEUED`, non-scheduled `PAUSED`)
4. `Completed` (`COMPLETED`)
5. `Failed` (`FAILED`, `CANCELED`)
6. `Video Downloads` (`STREAM_HLS`, `STREAM_DASH`, `STREAM_MSS`, or `category == "Video"`)
7. `Scheduled` (`scheduled == true`)

Selecting a category filters the single unified queue view without creating separate queues.

### C. Virtualized Download List (`BuildVisibleTaskRows`)
- **Full Mode (`>= 1024 px` scaled)**: Displays 8 columns (`Filename`, `Type`, `Status`, `Progress`, `Size`, `Speed`, `ETA`, `Actions`).
- **Compact Mode (`< 1024 px` scaled)**: Automatically hides `Type` and `ETA` (`6` columns) to prevent horizontal clipping.
- **Graceful Filename Truncation**: `TruncateFilenameGracefully()` uses middle ellipsis (`very_long_lect..._1080p.mp4`) so the file extension is always visible while preserving `full_filename_tooltip` for tooltips and the Details Panel.
- **Honest Progress Formatting**:
  - When `total_bytes > 0`: displays exact percentage (`64.2%`) and determinate bar fill.
  - When `is_stream && total_segments > 0`: displays segment completion percentage (`50.0% (5/10 segs)`).
  - When `total_bytes == 0` on an unchunked HTTP transfer: displays `1.2 MiB (Size Unknown)` and sets `progress_indeterminate = true` — **never** fabricates a percentage.
- **Granular Stream Stages**: Distinguishes `Probing stream manifest...`, `Downloading video segments (X/Y)`, `Downloading audio segments (X/Y)`, `Downloading subtitle tracks (X/Y)`, `Merging audio/video tracks (FFmpeg)...`, `Verifying output container...`, `Completed`, and `Failed`.

### D. Floating Selected Download Inspector (`PaintDetailsPanelWin32`)
When a download row is selected, a floating rounded glass card (`card_radius = ScaleForDpi(10, dpi)`) renders smoothly above the status bar with 16px horizontal margin and 6px vertical margin:
- **Left Icon Box**: Rounded elevated container (`ScaleForDpi(42, dpi)` x `ScaleForDpi(46, dpi)`) housing a signature Coral Red media/file icon.
- **Left Metadata Block** (3 rows):
  - Row 1: Filename in bold white Segoe UI accompanied by engine badge (`[WinHTTP]`, `[Stream Pipeline]`).
  - Row 2: File size, current transfer rate, and progress summary (`45.2 MB of 120.0 MB (37.7%) • 8.4 MB/s`).
  - Row 3: Destination folder with native path ellipsis (`DT_PATH_ELLIPSIS`).
- **Right Metadata Block** (3 rows):
  - Row 1: Credential-redacted source URL with end ellipsis.
  - Row 2: Timestamps (`Added: 2026-10-19 21:05 • Finished: 2026-10-19 21:06`).
  - Row 3: Actionable Coral Red diagnostic notice (`Notice: ...`) if failed, or live stream track summary / status.
- **Responsive Geometry**: Seamlessly collapses into a compact single-column card on narrow viewports without clipping.

### E. Custom Slim Fluent Scrollbar & Calm Static Canvas
1. **Custom 6px Dark Fluent Scrollbar**:
   - Eliminated the standard 17px light-gray Win32 scrollbar (`ShowScrollBar(hwnd, SB_VERT, FALSE)`).
   - Rendered directly inside the table body gutter with a 6px width and 3px rounded radius (`rgba(255, 255, 255, 38)` on idle, `rgba(255, 255, 255, 80)` on hover/drag).
   - Full mouse drag tracking (`SetCapture`), track clicking (page up / down), and mouse wheel scrolling.
2. **Calm Static Canvas**:
   - Completely removed cosmic moving stars and background animation drift timers from the visible desktop UI. Canvas remains calm, matte charcoal (`#161618`).
3. **Unicode-Safe Filename Truncation**:
   - `TruncateFilenameGracefully()` operates on wide character code points via `Utf8ToW` / `WToUtf8` rather than byte slices, completely preventing multi-byte UTF-8 corruption (``) on Bengali, Arabic, or Asian scripts.
4. **Status Bar Text Collision Guard**:
   - Strictly enforces minimum margins (`ScaleForDpi(24, dpi)`) after the rightmost status indicator (`NORMAL` / `LIMIT ON`) and automatically suppresses filename display when available width is below 140px, guaranteeing zero overlap.

### F. Dialog Modernization & Modal Lifecycle
1. **No WS_EX_TOPMOST / HWND_TOPMOST**:
   - Eliminated aggressive `WS_EX_TOPMOST` and `HWND_TOPMOST` styles from `NewDownloadDlgProc`, `ShowNewDownloadDialogWin32`, `ShowSimpleInputDialogWin32`, and `DeleteConfirmDlgProc`.
   - Modals are strictly owned by `hwndParent` maintaining proper Windows 11 taskbar, Alt+Tab, and Z-order stacking without permanently hijacking user focus across desktop applications.
2. **Refined Understated Button Architecture**:
   - Eradicated thick 2px pure white outlines and neon red glowing borders from buttons.
   - All dialog buttons use modern Windows 11 6-8px corner radii and clean 1px borders (`ColorRgba{0x3E, 0x40, 0x4A, 180}` or subtle coral highlights).
   - Handled `WM_CTLCOLORBTN` returning `ctx->hBgBrush` to eliminate Win32 button background artifacts.
3. **Live Destination Disk Space Checking**:
   - In `NewDownloadDlgProc`, an `EN_CHANGE` and `EN_KILLFOCUS` listener on the save directory edit control immediately invokes `GetDiskFreeSpaceExW()` on the user's destination drive.
   - Dynamically updates the disk space feedback label (e.g. `Free Space: 142.6 GB available`) scaled for DPI.
4. **Safe Deletion Confirmation Dialog (`DeleteConfirmDlgProc`)**:
   - Features middle-truncated filename display with full path rendered safely with ellipsis.
   - Default keyboard focus is assigned strictly to **Cancel** (`IDNO`), preventing accidental destructive deletions on Enter keypresses.
   - When file deletion fails, a clear error dialog is displayed to the user rather than failing silently.
5. **Settings Center Glass Modal (`PaintSettingsCenterOverlayWin32`)**:
   - Translucent glass scrim backdrop with rounded dialog card and specular edge reflection.
   - Fluid Windows 11 pill switches with smooth track and circular thumbs.
   - Accessible category navigation pills with Coral Red active state indicators.
6. **System Tray Integration (`Shell_NotifyIconW`)**:
   - Supports `Show Flux`, `Hide to Tray`, `Pause All Downloads`, `Resume All Downloads`, and `Exit Flux`, honoring `close_to_tray` on `WM_CLOSE`.

### G. Download Setup Dialog Consistency
- New Download, Add Torrent, and Remove Download share the matte charcoal canvas and `accent_primary` coral (`#FF4757`) for primary actions, focus borders, and selected-file indicators.
- New Download and Add Torrent use matching inset edit styling with a subtle default border and coral focus state. Torrent selections use the warm `surface_selected` fill with a coral edge indicator.
- Section dividers combine a muted rule with a short coral segment. Secondary actions remain on elevated charcoal surfaces so coral stays reserved for primary and selected states.
- Add Torrent section headings must keep a clear text-line gap before their controls at each DPI. Remove Download action buttons use centered, DPI-scaled widths sufficient to show their complete labels.

---

## 5. Accessibility & Keyboard Navigation

- **Keyboard Shortcuts (`DesktopView::HandleKeyShortcut` & `WM_KEYDOWN`)**:
  - `Up` / `Down` / `Home` / `End`: Navigate rows in the download table.
  - `Space`: Toggle Pause / Resume on the selected download.
  - `Delete`: Remove selected task from the list (preserving completed files on disk).
  - `Shift+Delete`: Delete selected task and remove its output file from disk.
  - `F5`: Retry a failed or canceled download.
  - `Enter`: Open a completed file.
- **No Emoji Policy**: All icons and badges use geometric vector primitives or accessible text labels (`FILE`, `HLS STREAM`, `DASH STREAM`) compatible with Windows Narrator.

### New Download field spacing update

The native New Download dialog now reserves a full label line and gap before each edit or combo control. Field rows and the lower metadata, scheduling, and action controls are shifted together using the existing DPI scale helper. Labels remain painted above their inputs; input borders and keyboard focus styles remain intact.

### Context menu state filtering

Task context menus omit disabled transfer actions and present only the action appropriate to the selected task state, with Cancel retained when available. Seeding control is available from More Actions only for completed torrents with uploaded bytes.

