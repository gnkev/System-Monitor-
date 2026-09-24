# System Monitor (v2.0)

[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20(x64)-blue.svg)](#requirements)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-00599C.svg)](#technology-stack)
[![Graphics](https://img.shields.io/badge/Renderer-DirectX%2011-black.svg)](#technology-stack)
[![UI](https://img.shields.io/badge/UI-Dear%20ImGui-orange.svg)](#technology-stack)

A high-performance, real-time Windows hardware monitoring dashboard and in-game OSD (On-Screen Display) overlay written in modern C++ with **Dear ImGui**, **Win32**, and **DirectX 11**.

Engineered as a lightweight, zero-bloat alternative to bulky software suites, System Monitor provides instant hardware telemetry, per-core breakdown, live historical graphing, customizable desktop/gaming overlays, system tray integration, and real-time color theme switching.

---

## 🌟 What's New in Version 2.0

### 1. 🖥️ Dynamic In-Game & Desktop OSD Overlay
- **Toggle Hotkey**: Instant global toggle using customizable key shortcuts (Default: `Ctrl + Shift + O`).
- **Layered Transparency**: Hardware-accelerated click-through borderless overlay (`WS_POPUP | WS_EX_TOPMOST | WS_EX_LAYERED`) that floats above games and desktop applications.
- **Corner Snapping & Offsets**: Anchor the overlay to any screen corner (**Top-Left**, **Top-Right**, **Bottom-Left**, **Bottom-Right**) with automatic margin offsets.
- **Granular Metric Selection**: Selectively display:
  - Framerate (FPS & frame times)
  - CPU Utilization & Temperature
  - GPU Utilization & Temperature
  - System RAM Usage
  - Dedicated VRAM Usage
- **Interactive Live Preview**: Dedicated preview box inside Settings allowing real-time inspection of your OSD layout, opacity, font size, and metrics as you tune sliders.

### 2. 🎨 Dynamic Cyberpunk Theme Engine & Real-Time Logo Swapping
Choose from 5 curated color themes with synchronized neon accent colors, glowing dashboard headers, and real-time icon switching:
- 🔵 **Electric Blue** (Default)
- 🟢 **Razer Green**
- 🟣 **Corsair Purple**
- 🔴 **ROG Red**
- 🟠 **AMD Orange**

**Live Theme Synchronization**:
- Switching themes updates the **Dashboard Accent Colors**, **Header Logo Graphics**, **Window Title Bar Icon**, **Windows Taskbar Button**, and **System Tray Icon** in real time without requiring an application restart.

### 3. 🔍 Universal Hardware Telemetry (Any CPU & GPU)
- **Universal Multi-Core Architecture**: Automatically detects dynamic CPU logical core counts via Windows PDH counters and `GetLogicalProcessorInformationEx`. Tested across Intel Core (including hybrid P-Core / E-Core layouts) and AMD Ryzen architectures.
- **Intelligent GPU Discovery**: Uses **DXGI 1.4** physical adapter enumeration to automatically discover the primary dedicated discrete GPU and Dedicated Video Memory (VRAM), ignoring virtual and integrated adapters.
- **Multi-Source Temperature Acquisition**: Hybrid thermal probe telemetry combining **NVIDIA NVML** with **WMI ACPI Thermal Zones** for broad CPU/GPU temperature sensor compatibility.

### 4. 📈 Interactive Real-Time Graphs & Precision Tooltips
- Smooth rolling history graphs for CPU utilization, GPU 3D engine load, RAM usage, and VRAM consumption.
- **Hover Inspect Tooltips**: Hovering anywhere over the graphs reveals exact metric names, percentage/gigabyte readouts, and timestamps.
- Clean min/max/avg statistical cards calibrated with generous margins to remain readable across all resolutions.

### 5. 🎚️ Display Scaling & Responsive UI Engine
- Native **Per-Monitor V2 DPI awareness** with crisp vector font rendering (**Segoe UI**).
- **Windows-Style UI Scaling**: Presets for `100%`, `125%`, `150%`, `175%`, `200%`, plus a free-form **Custom Slider**.
- Starts in maximized fullscreen mode by default for an immediate cockpit overview.

### 6. 💾 Persistent Settings Engine
- Instant write-through configuration system stored in `settings.ini` (with automatic `%APPDATA%` fallback).
- Saves all user preferences: active theme, polling rate, temperature unit (°C / °F), thermal warning thresholds, UI scaling, OSD metric toggles, OSD position, OSD opacity, and global hotkeys.
- Robust shutdown safety hooks (`WM_CLOSE`, `WM_DESTROY`, `IDM_TRAY_EXIT`, and process handlers) ensure settings persist even during forced task termination.
- **Windows Startup Integration**: Option to automatically start System Monitor in the background on Windows boot (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
- One-click **"Reset All Settings to Defaults"** recovery.

### 7. 📥 Windows System Tray & Native Shell Integration
- Minimize to tray to keep system performance monitored silently in the background.
- Tray context menu:
  - **Open Dashboard**
  - **Toggle Overlay**
  - **Exit**
- Native Windows metadata (`VERSIONINFO`) and embedded multi-resolution icons (`16px` to `256px`), ensuring the window and taskbar entry properly identify as **"System Monitor"**.

---

## 🛠️ Technology Stack

| Layer | Component | Description |
| :--- | :--- | :--- |
| **Language** | C++17 | Clean, modern Win32 / C++ implementation |
| **Graphics API** | DirectX 11 (`d3d11`) | High-performance hardware-accelerated rendering |
| **GUI Framework** | Dear ImGui | Docking/tables/vector UI widgets |
| **Sensors & Counters** | Windows PDH & WMI | Low-overhead native Windows performance queries |
| **Display Adapter** | DXGI 1.4 | Hardware adapter topology and dedicated memory discovery |
| **Imaging** | GDI+ | High-quality PNG icon & header texture loading |
| **Shell** | Win32 & ShellAPI | Tray icon, global hotkeys, high-DPI scaling, and layered windows |

---

## 🚀 Building & Running

### Prerequisites
- Windows 10 or 11 (64-bit)
- Microsoft Visual Studio 2022 (Community or Build Tools) with the **Desktop development with C++** workload.

### Building from Command Line
Run the following commands in the **x64 Native Tools Command Prompt for VS 2022** (or after running `vcvars64.bat`):

```bat
# 1. Create build directories and copy assets
if not exist "build" mkdir "build"
if not exist "build\assets" mkdir "build\assets"
copy /y assets\*.* build\assets\ >nul

# 2. Compile Windows resources
rc.exe /nologo /fo "build\resource.res" resource.rc

# 3. Compile and link SystemMonitor.exe
cl.exe /Zi /EHsc /nologo /I "." /I "imgui" /I "imgui\backends" ^
    /Fo"build/" /Fd"build/SystemMonitor.pdb" /Fe"build/SystemMonitor.exe" ^
    main.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_widgets.cpp imgui/imgui_tables.cpp ^
    imgui/backends/imgui_impl_win32.cpp imgui/backends/imgui_impl_dx11.cpp ^
    build\resource.res ^
    /link /SUBSYSTEM:WINDOWS d3d11.lib user32.lib gdi32.lib gdiplus.lib iphlpapi.lib psapi.lib ws2_32.lib
```

### Running
```bat
build\SystemMonitor.exe
```

---

## ⌨️ Default Shortcuts

| Shortcut | Action |
| :--- | :--- |
| `Ctrl + Shift + O` | Toggle Desktop / In-Game OSD Overlay |
| `Double Click Tray Icon` | Open / Focus Fullscreen Dashboard |
| `Right Click Tray Icon` | Open Tray Menu (Dashboard, Overlay, Exit) |

*(All hotkeys and key combinations can be rebound directly in the Settings tab).*

---

## 📁 Project Structure

```text
System-Monitor-/
├── assets/                  # High-resolution PNG and multi-resolution ICO theme logos
│   ├── logo_blue.{png,ico}
│   ├── logo_green.{png,ico}
│   ├── logo_purple.{png,ico}
│   ├── logo_red.{png,ico}
│   └── logo_orange.{png,ico}
├── imgui/                   # Dear ImGui library and DX11 / Win32 backends
├── main.cpp                 # Core application logic, telemetry queries, and UI
├── resource.rc              # Windows resource script (icons & VERSIONINFO)
├── .gitignore               # Build output and temporary file exclusions
└── README.md                # Project documentation
```

---

## 📄 License & Copyright

Copyright (c) 2026. Licensed under the [MIT License](LICENSE).

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

---

## ⚖️ Disclaimer & Trademark Notice

- **Hardware Monitoring & Sensor Data**: This software is provided "as is", without warranty of any kind, express or implied. Hardware telemetry (temperatures, clocks, utilization, voltages) is acquired through standard OS and driver interfaces (PDH, WMI, NVML, DXGI) and is intended for informational and diagnostic purposes only. The authors and contributors cannot be held liable for any damages, inaccuracies, hardware faults, or unintended system behavior resulting from the use of this software.
- **Third-Party Trademarks**: All product names, logos, brands, and trademarks referenced within this application (including Intel, AMD, Ryzen, NVIDIA, GeForce, Microsoft, Windows, DirectX, Razer, and Corsair) are the property of their respective trademark holders. Reference to these brands or inclusion of theme presets inspired by their aesthetic palettes is done purely for identification and nominative purposes, and does not imply any affiliation, sponsorship, or endorsement.

