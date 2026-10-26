# Flux Download Manager — Product Identity & Branding Specification

**Document Status:** Approved Product Branding Baseline  
**Date:** 2026-10-16  
**Scope:** Defines the complete visual, typographic, naming, and architectural identity for **Flux Download Manager** across the desktop application, browser extension, in-page video overlay, installer, and documentation.

---

## 1. Product Naming Hierarchy

| Attribute | Specification | Permitted Usage | Prohibited Usage |
| :--- | :--- | :--- | :--- |
| **Official Full Name** | **Flux Download Manager** | Main window title bar, installer welcome & finish pages, Add/Remove Programs display name, documentation titles, browser extension manifest name. | Never abbreviate as "FDM" (to prevent confusion with Free Download Manager). Never use "OpenIDM". |
| **Short Brand Name** | **Flux** | System tray icon tooltip, extension badge, in-page download button ("Download with Flux"), toolbar buttons, dialog prompts, CLI banner. | Never use alone in legal notices or copyright declarations. |
| **Executable Names** | `flux.exe`<br>`flux-nmh.exe` | Binary filenames on disk, Task Manager process list, start menu shortcuts, registry launch paths. | Do not use `openidm.exe` or `openidm-nmh.exe` in release distributions. |
| **Native Host Identifier** | `com.flux.downloadmanager`<br>`org.flux.native` | Browser native messaging registrations in Windows Registry and host JSON manifests. | Do not expose raw internal pipe paths to users. |
| **Application Data Path** | `%LOCALAPPDATA%\Flux Download Manager\` | Default directory for `state.db`, `flux.log`, `settings.json`, and temporary files. | Never scatter files across user documents or system directories. |
| **Default Download Path** | `%USERPROFILE%\Downloads\Flux\` | Default destination folder for completed user downloads. | Do not download directly into root C:\ or temp folders. |

---

## 2. Color Palette & Visual Identity

Flux employs a sophisticated, modern Windows 11 palette characterized by a **deep charcoal dark theme**, a **restrained blue accent**, and a crisp, high-contrast light theme.

```
       DEEP CHARCOAL DARK THEME                   ELECTRIC BLUE ACCENT
┌──────────────────────────────────────┐       ┌────────────────────────┐
│ App Background:    #0F172A (Slate 900)│       │ Primary Accent: #2563EB│
│ Surface Primary:   #1E293B (Slate 800)│       │ Accent Hover:   #3B82F6│
│ Surface Elevated:  #334155 (Slate 700)│       │ Accent Active:  #1D4ED8│
│ Surface Selected:  #1E3A8A (Blue 900) │       │ Progress Fill:  #3B82F6│
└──────────────────────────────────────┘       └────────────────────────┘
```

### 2.1 Design Tokens Table

| Token Name | Dark Theme (Default) | Light Theme | Usage |
| :--- | :--- | :--- | :--- |
| `app_background` | `#0F172A` (Deep Slate) | `#F8FAFC` (Cool Off-White) | Desktop client window background |
| `surface_primary` | `#1E293B` (Dark Navy Surface) | `#FFFFFF` (Pure White) | Toolbar, sidebar, table header, details panel |
| `surface_elevated` | `#334155` (Elevated Card) | `#F1F5F9` (Subtle Gray) | Button backgrounds, badges, cards |
| `surface_hover` | `#475569` (Interactive Hover) | `#E2E8F0` (Hover Gray) | Hovered list rows and toolbar buttons |
| `surface_selected` | `#1E3A8A` (Deep Blue Select) | `#DBEAFE` (Light Blue Select) | Selected task row in queue table |
| `accent_primary` | `#2563EB` (Restrained Blue) | `#2563EB` (Restrained Blue) | Primary action buttons, selection indicators |
| `accent_hover` | `#3B82F6` (Vibrant Blue Hover) | `#1D4ED8` (Deep Blue Hover) | Hover state for primary action buttons |
| `text_primary` | `#F8FAFC` (Near White) | `#0F172A` (Deep Charcoal) | Primary headings, task filenames, speeds |
| `text_secondary` | `#94A3B8` (Muted Slate) | `#64748B` (Muted Gray) | Subheadings, dates, metadata labels |
| `text_muted` | `#64748B` (Dimmed Slate) | `#94A3B8` (Light Slate) | Disabled buttons, inactive status |
| `border_subtle` | `#334155` (Subtle Border) | `#E2E8F0` (Light Border) | Card borders, column dividers, status bar |
| `status_success` | `#22C55E` (Emerald Green) | `#16A34A` (Forest Green) | Completed downloads, connected indicators |
| `status_warning` | `#F59E0B` (Amber) | `#D97706` (Deep Amber) | Stalled transfers, retrying, warnings |
| `status_error` | `#EF4444` (Vibrant Red) | `#DC2626` (Crimson Red) | Failed downloads, disk full, network errors |
| `badge_stream` | `#8B5CF6` (Violet / Purple) | `#7C3AED` (Deep Violet) | HLS & DASH adaptive stream badges |

---

## 3. Typography & Spacing System

- **Primary Font Family**: `Segoe UI Variable`, `Segoe UI`, `-apple-system`, `sans-serif`.
- **Monospace Font (Speeds, Sizes, Hashes)**: `Cascadia Code`, `Consolas`, `monospace`.
- **4px / 8px Grid**:
  - `space_xs`: `4px`
  - `space_sm`: `8px`
  - `space_md`: `16px`
  - `space_lg`: `24px`
  - `space_xl`: `32px`

---

## 4. In-Page Video Overlay Specifications

- **Selector / Anchor**: Attached to HTML5 `<video>` elements with dimensions $\ge 240 \times 135\text{ px}$.
- **Container**: Injected `<div data-flux-shadow-host="true">` with closed Shadow DOM (`attachShadow({ mode: "closed" })`).
- **Positioning**: Fixed position, `top: max(8px, rect.top + 10px)`, `left: max(8px, rect.right - 168px)` (top-right of player, never covering bottom transport controls).
- **Badge Copy**: `"Download with Flux"` (or `"DRM Protected (Unsupported)"` when DRM is detected).
- **Style**:
  ```css
  background: #0F172A;
  color: #F8FAFC;
  border: 1px solid #2563EB;
  border-radius: 6px;
  padding: 6px 12px;
  font: 600 12px 'Segoe UI', sans-serif;
  cursor: pointer;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.4);
  ```

---

## 5. Backward Compatibility & Data Migration Rules

To preserve user data and prevent disruption:
1. When Flux launches, `PathSanitizer::GetDefaultAppDataDir()` resolves to `%LOCALAPPDATA%\Flux Download Manager`.
2. If `%LOCALAPPDATA%\Flux Download Manager\state.db` does not exist, the startup sequence checks `%LOCALAPPDATA%\OpenIDM\state.db`.
3. If legacy OpenIDM data exists:
   - Create `%LOCALAPPDATA%\Flux Download Manager\`.
   - Copy `state.db`, `state.db-wal`, and settings cleanly into the new Flux directory.
   - Leave the original OpenIDM files intact as a fallback safety measure.
   - Log the successful migration in `flux.log`.
4. Named Pipe IPC server opens `\\.\pipe\Flux_IPC_Default` as its primary socket, while also providing a bridge for any existing OpenIDM clients.
