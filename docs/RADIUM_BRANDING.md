# Radium Download Manager — Product Identity & Branding Specification

**Document Status:** Approved Product Branding Baseline  
**Date:** 2026-10-20  
**Scope:** Defines the complete visual, typographic, naming, and architectural identity for **Radium Download Manager** across the desktop application, browser extension, in-page video overlay, installer, and documentation.

---

## 1. Product Naming Hierarchy

| Attribute | Specification | Permitted Usage | Prohibited Usage |
| :--- | :--- | :--- | :--- |
| **Official Full Name** | **Radium Download Manager** | Main window title bar, installer welcome & finish pages, Add/Remove Programs display name, documentation titles, browser extension manifest name. | Never use "Flux" or "OpenIDM" as the current product name. |
| **Short Brand Name** | **Radium** | System tray icon tooltip, extension badge, in-page download button ("Download in Radium"), toolbar buttons, dialog prompts, CLI banner. | Never use alone in legal notices or copyright declarations. |
| **Abbreviation** | **RDM** | Compact UI labels, compact logos, release tags where short codes are required. | Do not use ambiguous abbreviations. |
| **Executable Names** | `radium.exe`<br>`radium-nmh.exe` | Binary filenames on disk, Task Manager process list, start menu shortcuts, registry launch paths. (Compatibility aliases: `flux.exe`, `flux-nmh.exe`). | Do not rely on deprecated executable names in new distributions. |
| **Native Host Identifier** | `com.flux.downloadmanager`<br>`org.openidm.native` | Browser native messaging registrations in Windows Registry and host JSON manifests (preserved for backward compatibility). | Do not break existing browser extension registrations. |
| **Application Data Path** | `%LOCALAPPDATA%\Radium Download Manager\` | Default directory for `state.db`, `radium.log`, `settings.json`, and temporary files. Automatically migrates legacy state from `%LOCALAPPDATA%\Flux` or `%LOCALAPPDATA%\OpenIDM`. | Never scatter files across user documents or system directories. |
| **Default Download Path** | `%USERPROFILE%\Downloads\Radium\` | Default destination folder for completed user downloads. (Preserves legacy `%USERPROFILE%\Downloads\Flux` or `%USERPROFILE%\Downloads\OpenIDM` if present). | Do not download directly into root C:\ or temp folders. |

---

## 2. Color Palette & Visual Identity

Radium employs a sophisticated, modern Windows 11 palette characterized by a **deep charcoal dark theme**, a **coral red accent**, and a crisp, high-contrast light theme.

```
       DEEP CHARCOAL DARK THEME                   CORAL RED ACCENT
┌──────────────────────────────────────┐       ┌────────────────────────┐
│ App Background:    #161618 (Charcoal) │       │ Primary Accent: #FF4757│
│ Surface Primary:   #1E1E22            │       │ Accent Hover:   #FF666D│
│ Surface Elevated:  #28282E            │       │ Progress Fill:  #FF4757│
│ Surface Selected:  #302328 (Warm tint)│       │ Focus Ring:     #FF4757│
└──────────────────────────────────────┘       └────────────────────────┘
```

### 2.1 Design Tokens Table

| Token Name | Dark Theme (Default) | Light Theme | Usage |
| :--- | :--- | :--- | :--- |
| `app_background` | `#161618` | `#F8FAFC` | Desktop client window background |
| `cosmic_void` | `#121214` | — | Deep background where needed |
| `surface_primary` | `#1E1E22` | `#FFFFFF` | Toolbar, sidebar, table header, details panel |
| `surface_elevated` | `#28282E` | `#F1F5F9` | Button backgrounds, badges, cards |
| `surface_hover` | `#2B2B32` | `#E2E8F0` | Hovered list rows and toolbar buttons |
| `surface_selected` | `#302328` | `#DBEAFE` | Selected task row with restrained coral tint |
| `surface_pressed` | `#3A2B30` | — | Pressed controls |
| `accent_primary` | `#FF4757` | `#FF4757` | Primary actions, active downloads, progress fills |
| `accent_hover` | `#FF5F6A` | `#FF353E` | Hover state for coral actions |
| `text_primary` | `#F2F2F3` | `#0F172A` | Primary headings, task filenames, speeds |
| `text_secondary` | `#B4B4BD` | `#475569` | Subheadings, dates, metadata labels |
| `text_muted` | `#85858F` | `#94A3B8` | Disabled controls and low-priority metadata |
| `border_subtle` | `#35353D` | `#CBD5E1` | Card borders, column dividers, status bar |
| `border_focus` | `#FF4757` | `#FF4757` | Keyboard focus indicator |
| `status_success` | `#A9B4C2` | `#7D8A99` | Completed downloads |
| `status_warning` | `#F2B84B` | `#D97706` | Paused and scheduled downloads |
| `status_danger` | `#FF9F43` | `#D97A24` | Failed and canceled downloads |
| `progress_track` | `#303036` | `#E2E8F0` | Unfilled progress track |

---

## 3. Typography & Spacing System

- **Primary Font Family**: `Segoe UI Variable`, `Segoe UI`, `-apple-system`, `sans-serif`.
- **Monospace Code/Hash Font**: `Consolas`, `Cascadia Code`, `Courier New`, monospace.
- **Base Grid**: 4px / 8px grid system for all layout padding, margins, and control heights.
- **Border Radius**: 6px for standard buttons and dialog cards; 8px for modal containers; 9999px for pills and badges.

---

## 4. Logo & Iconography

- **Official Mark**: Stylized letter **R** within a coral red rounded hexagonal / shield container.
- **Desktop Executable Icon**: Multi-size Windows icon file (`radium.ico`) supporting 16x16, 24x24, 32x32, 48x48, and 256x256 dimensions.
- **Browser Extension Icons**: PNG suite at 16x16, 32x32, 48x48, and 128x128 pixels in `extension/icons/`.
- **System Tray Icon**: High-contrast icon rendered dynamically with GDI vector fallback.
