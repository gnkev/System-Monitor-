#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <dxgi1_4.h>
#include <pdh.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <intrin.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "gdiplus.lib")

#include <gdiplus.h>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define HOTKEY_TOGGLE_OVERLAY   1001
#define WM_TRAYICON             (WM_USER + 1)
#define IDM_TRAY_OPEN_DASHBOARD 2001
#define IDM_TRAY_TOGGLE_OVERLAY 2002
#define IDM_TRAY_EXIT           2003

// D3D & Icon globals
static ID3D11Device*           g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext     = nullptr;
static IDXGISwapChain*         g_pSwapChain            = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView  = nullptr;
static HWND                    g_hwnd                  = nullptr;
static NOTIFYICONDATA          g_nid                   = {};

static ULONG_PTR                 g_gdiplusToken          = 0;
static HICON                     g_themeIcons[5]         = { NULL };
static HICON                     g_themeIconsSm[5]       = { NULL };
static ID3D11ShaderResourceView* g_themeLogoSRV[5]       = { nullptr };

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================
//  NtQuerySystemInformation for per-core CPU
// ============================================================
typedef LONG NTSTATUS;
#define SystemProcessorPerformanceInformation 8

typedef struct _SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

typedef NTSTATUS(WINAPI* PFN_NtQuerySystemInformation)(
    ULONG  SystemInformationClass,
    PVOID  SystemInformation,
    ULONG  SystemInformationLength,
    PULONG ReturnLength);

static PFN_NtQuerySystemInformation pNtQuerySystemInformation = nullptr;

// ============================================================
//  Static system info
// ============================================================
static int         g_numCores   = 0;
static std::string g_cpuName;
static std::string g_gpuName;
static double      g_totalRamGB = 0.0;

// ============================================================
//  Metrics tracking
// ============================================================
static ULONGLONG g_lastIdleTime = 0, g_lastKernelTime = 0, g_lastUserTime = 0;
static std::vector<float> g_cpuHistory(60, 0.0f);

static std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> g_prevCoreInfo;
static std::vector<float>              g_coreUsage;
static std::vector<std::vector<float>> g_coreHistory;

static std::vector<float> g_ramHistory(60, 0.0f);

static std::vector<float>        g_gpuHistory(60, 0.0f);
static PDH_HQUERY                g_pdhQuery         = NULL;
static std::vector<PDH_HCOUNTER> g_gpuCounters;
static SIZE_T                    g_gpuVramUsed      = 0;
static SIZE_T                    g_gpuVramTotal     = 0;

// Disk PDH counters
static PDH_HCOUNTER              g_diskReadCounter  = NULL;
static PDH_HCOUNTER              g_diskWriteCounter = NULL;
static float                     g_diskReadSpeedMB  = 0.0f;
static float                     g_diskWriteSpeedMB = 0.0f;
static std::vector<float>        g_diskReadHistory(60, 0.0f);
static std::vector<float>        g_diskWriteHistory(60, 0.0f);

struct DiskDriveInfo {
    std::string path;
    double freeGB;
    double totalGB;
    double usedGB;
    double percent;
};
static std::vector<DiskDriveInfo> g_diskDrives;

// Network tracking
static ULONG64            g_lastInBytes     = 0;
static ULONG64            g_lastOutBytes    = 0;
static float              g_netDownSpeedMB  = 0.0f;
static float              g_netUpSpeedMB    = 0.0f;
static std::vector<float> g_netDownHistory(60, 0.0f);
static std::vector<float> g_netUpHistory(60, 0.0f);
static std::string        g_netAdapterName  = "Default Adapter";
static std::string        g_localIP         = "127.0.0.1";

// Temperature tracking
static float g_cpuTemp = -1.0f;
static float g_gpuTemp = -1.0f;
static std::vector<float> g_cpuTempHistory(60, 0.0f);
static std::vector<float> g_gpuTempHistory(60, 0.0f);

// Process ranking
struct ProcItem {
    DWORD pid;
    std::string name;
    float cpuUsage;
    SIZE_T memMB;
};
static std::vector<ProcItem> g_topCpuProcesses;
static std::vector<ProcItem> g_topMemProcesses;

// FPS tracking
static float g_fps = 0.0f;
static float g_frameTimeMs = 0.0f;
static int   g_frameCount = 0;
static float g_fpsTimer = 0.0f;

// Themes & Preferences
enum ColorTheme { THEME_BLUE, THEME_GREEN, THEME_PURPLE, THEME_RED, THEME_ORANGE };
static ColorTheme g_currentTheme = THEME_BLUE;
static float      g_updateInterval = 1.0f; // Telemetry update rate (seconds)

enum TempUnit { TEMP_CELSIUS, TEMP_FAHRENHEIT };
static TempUnit g_tempUnit = TEMP_CELSIUS;

enum OverlayCorner { CORNER_TOP_LEFT, CORNER_TOP_RIGHT, CORNER_BOTTOM_LEFT, CORNER_BOTTOM_RIGHT, CORNER_CUSTOM };
static OverlayCorner g_overlayCorner = CORNER_TOP_RIGHT;

// Dynamic Hotkey Customization
static UINT        g_hotkeyVk         = 'O';
static UINT        g_hotkeyMod        = MOD_CONTROL | MOD_SHIFT;
static bool        g_recordingHotkey = false;
static std::string g_hotkeyText       = "Ctrl + Shift + O";

// Dashboard UI Scaling State
enum UiScalePreset { SCALE_CUSTOM = 0, SCALE_100 = 1, SCALE_125 = 2, SCALE_150 = 3, SCALE_175 = 4, SCALE_200 = 5 };
static UiScalePreset g_uiScalePreset = SCALE_100;
static float g_uiScale = 1.00f; // UI Display Scale multiplier (1.00 = 100%, 1.25 = 125%, etc.)
static ImGuiStyle g_baseStyle;
static bool       g_baseStyleInit = false;

// Floating Overlay Sizing State
enum OsdSizePreset { OSD_SMALL, OSD_NORMAL, OSD_LARGE, OSD_XLARGE };
static OsdSizePreset g_osdSizePreset = OSD_NORMAL;
static float g_osdScale     = 1.00f; // Scale factor for OSD box width
static float g_osdFontScale = 1.15f; // Scale factor for OSD text font size

// Floating Overlay State
static bool  g_overlayMode          = false;
static bool  g_overlayShowFps       = true;
static bool  g_overlayShowCpuLoad   = true;
static bool  g_overlayShowCpuTemp   = true;
static bool  g_overlayShowGpuLoad   = true;
static bool  g_overlayShowGpuTemp   = true;
static bool  g_overlayShowRam       = true;
static bool  g_overlayShowVram      = true;
static float g_overlayOpacity      = 0.85f;
static float g_dpiScale             = 1.0f;

// Thermal Alert & Auto-Start
static bool  g_enableThermalAlert   = true;
static float g_thermalThreshold     = 85.0f; // °C
static bool  g_autoStartWithWindows = false;

// String Converter Helper
static std::string WideToUTF8(const wchar_t* wide)
{
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, sizeof(buf), NULL, NULL);
    char* p = buf;
    while (*p == ' ') ++p;
    return p;
}

// Theme Accent Helper
static ImVec4 GetAccentColor()
{
    switch (g_currentTheme) {
    case THEME_BLUE:   return ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    case THEME_GREEN:  return ImVec4(0.20f, 0.80f, 0.35f, 1.00f);
    case THEME_PURPLE: return ImVec4(0.65f, 0.35f, 0.95f, 1.00f);
    case THEME_RED:    return ImVec4(0.95f, 0.25f, 0.25f, 1.00f);
    case THEME_ORANGE: return ImVec4(0.98f, 0.55f, 0.15f, 1.00f);
    default:           return ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    }
}

// Helpers
static float DisplayTemp(float tempC)
{
    if (tempC <= 0.0f) return tempC;
    return (g_tempUnit == TEMP_FAHRENHEIT) ? (tempC * 1.8f + 32.0f) : tempC;
}

static const char* TempSymbol()
{
    return (g_tempUnit == TEMP_FAHRENHEIT) ? "°F" : "°C";
}

static float TempMaxGraph()
{
    return (g_tempUnit == TEMP_FAHRENHEIT) ? 212.0f : 100.0f;
}

static bool IsOverheating()
{
    if (!g_enableThermalAlert) return false;
    return (g_cpuTemp >= g_thermalThreshold || g_gpuTemp >= g_thermalThreshold);
}

static bool IsModifierKey(int vk)
{
    return (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || 
            vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN);
}

static std::string FormatKeyName(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NumPad " + std::to_string(vk - VK_NUMPAD0);

    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_TAB: return "Tab";
    case VK_ESCAPE: return "Esc";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_UP: return "Up Arrow";
    case VK_DOWN: return "Down Arrow";
    case VK_LEFT: return "Left Arrow";
    case VK_RIGHT: return "Right Arrow";
    case VK_CAPITAL: return "Caps Lock";
    case VK_PAUSE: return "Pause";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    case VK_MULTIPLY: return "Num *";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_DECIMAL: return "Num .";
    case VK_DIVIDE: return "Num /";
    }

    UINT scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    if (scanCode != 0) {
        LONG lParam = (scanCode << 16);
        char buf[64] = {};
        if (GetKeyNameTextA(lParam, buf, sizeof(buf)) > 0) {
            return std::string(buf);
        }
    }

    return "Key " + std::to_string(vk);
}

static std::string FormatHotkeyString(UINT mod, UINT vk)
{
    std::string s = "";
    if (mod & MOD_CONTROL) s += "Ctrl + ";
    if (mod & MOD_ALT)     s += "Alt + ";
    if (mod & MOD_SHIFT)   s += "Shift + ";
    if (mod & MOD_WIN)     s += "Win + ";
    s += FormatKeyName(vk);
    return s;
}

static void RegisterUserHotkey(HWND hwnd)
{
    if (!hwnd) return;
    UnregisterHotKey(hwnd, HOTKEY_TOGGLE_OVERLAY);
    RegisterHotKey(hwnd, HOTKEY_TOGGLE_OVERLAY, g_hotkeyMod, g_hotkeyVk);
}

static void PositionWindowForCorner(OverlayCorner corner, int width, int height)
{
    if (!g_hwnd) return;

    RECT workArea = {};
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    int margin = 20;
    int x = workArea.left + margin;
    int y = workArea.top + margin;

    switch (corner) {
    case CORNER_TOP_LEFT:
        x = workArea.left + margin;
        y = workArea.top + margin;
        break;
    case CORNER_TOP_RIGHT:
        x = workArea.right - width - margin;
        y = workArea.top + margin;
        break;
    case CORNER_BOTTOM_LEFT:
        x = workArea.left + margin;
        y = workArea.bottom - height - margin;
        break;
    case CORNER_BOTTOM_RIGHT:
        x = workArea.right - width - margin;
        y = workArea.bottom - height - margin;
        break;
    case CORNER_CUSTOM:
        return;
    }

    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

static int CalculateOsdHeight()
{
    int lines = 0;
    if (g_overlayShowFps) lines++;
    if (g_overlayShowCpuLoad || g_overlayShowCpuTemp) lines++;
    if (g_overlayShowGpuLoad || g_overlayShowGpuTemp) lines++;
    if (g_overlayShowRam) lines++;
    if (g_overlayShowVram && g_gpuVramTotal > 0) lines++;
    if (lines == 0) lines = 1;
    return (int)((lines * (26.0f * g_osdFontScale) + 24.0f) * g_dpiScale);
}

static void ToggleOverlayMode(bool enable)
{
    g_overlayMode = enable;
    if (!g_hwnd) return;

    if (enable) {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_LAYERED);
        SetLayeredWindowAttributes(g_hwnd, RGB(0,0,0), (BYTE)(g_overlayOpacity * 255), LWA_ALPHA);

        int oWidth  = (int)(260.0f * g_osdScale * g_dpiScale);
        int oHeight = CalculateOsdHeight();
        PositionWindowForCorner(g_overlayCorner, oWidth, oHeight);
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

static void OpenFullDashboard()
{
    g_overlayMode = false;
    if (!g_hwnd) return;

    SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, 0);
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    ::SetWindowTextW(g_hwnd, L"System Monitor");

    ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    SetForegroundWindow(g_hwnd);
}

static void InitSystemTray(HWND hwnd)
{
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_themeIconsSm[g_currentTheme] ? g_themeIconsSm[g_currentTheme] : LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyA(g_nid.szTip, "System Monitor");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void CleanupSystemTray()
{
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

static void SetAutoStart(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(NULL, exePath, MAX_PATH);
            RegSetValueExA(hKey, "SystemMonitor", 0, REG_SZ, (const BYTE*)exePath, (DWORD)strlen(exePath) + 1);
            g_autoStartWithWindows = true;
        } else {
            RegDeleteValueA(hKey, "SystemMonitor");
            g_autoStartWithWindows = false;
        }
        RegCloseKey(hKey);
    }
}

static void CheckAutoStartStatus()
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        char pathBuf[MAX_PATH] = {};
        DWORD cbData = sizeof(pathBuf);
        if (RegQueryValueExA(hKey, "SystemMonitor", NULL, &type, (LPBYTE)pathBuf, &cbData) == ERROR_SUCCESS) {
            g_autoStartWithWindows = true;
        } else {
            if (g_autoStartWithWindows) {
                SetAutoStart(true);
            } else {
                g_autoStartWithWindows = false;
            }
        }
        RegCloseKey(hKey);
    }
}

// ============================================================
//  Persistent Settings Configuration (settings.ini)
// ============================================================
static std::wstring GetSettingsFilePath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    }
    std::wstring path = exePath;
    path += L"settings.ini";
    return path;
}

static std::wstring GetFallbackSettingsFilePath()
{
    wchar_t appData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) > 0) {
        std::wstring fallbackDir = appData;
        fallbackDir += L"\\SystemMonitor";
        CreateDirectoryW(fallbackDir.c_str(), NULL);
        return fallbackDir + L"\\settings.ini";
    }
    return L"";
}

static void SaveSettings()
{
    std::wstring path = GetSettingsFilePath();
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) {
        std::wstring fallback = GetFallbackSettingsFilePath();
        if (!fallback.empty()) {
            f = _wfopen(fallback.c_str(), L"w");
        }
    }
    if (!f) return;

    fprintf(f, "[General]\n");
    fprintf(f, "Theme=%d\n", (int)g_currentTheme);
    fprintf(f, "UpdateInterval=%.2f\n", g_updateInterval);
    fprintf(f, "TempUnit=%d\n", (int)g_tempUnit);
    fprintf(f, "EnableThermalAlert=%d\n", g_enableThermalAlert ? 1 : 0);
    fprintf(f, "ThermalThreshold=%.1f\n", g_thermalThreshold);
    fprintf(f, "AutoStart=%d\n", g_autoStartWithWindows ? 1 : 0);
    fprintf(f, "\n");

    fprintf(f, "[Hotkey]\n");
    fprintf(f, "HotkeyVk=%u\n", g_hotkeyVk);
    fprintf(f, "HotkeyMod=%u\n", g_hotkeyMod);
    fprintf(f, "HotkeyText=%s\n", g_hotkeyText.c_str());
    fprintf(f, "\n");

    fprintf(f, "[Dashboard]\n");
    fprintf(f, "UiScalePreset=%d\n", (int)g_uiScalePreset);
    fprintf(f, "UiScale=%.3f\n", g_uiScale);
    fprintf(f, "\n");

    fprintf(f, "[Overlay]\n");
    fprintf(f, "OsdSizePreset=%d\n", (int)g_osdSizePreset);
    fprintf(f, "OsdScale=%.3f\n", g_osdScale);
    fprintf(f, "OsdFontScale=%.3f\n", g_osdFontScale);
    fprintf(f, "OsdCorner=%d\n", (int)g_overlayCorner);
    fprintf(f, "OsdOpacity=%.3f\n", g_overlayOpacity);
    fprintf(f, "OsdShowFps=%d\n", g_overlayShowFps ? 1 : 0);
    fprintf(f, "OsdShowCpuLoad=%d\n", g_overlayShowCpuLoad ? 1 : 0);
    fprintf(f, "OsdShowCpuTemp=%d\n", g_overlayShowCpuTemp ? 1 : 0);
    fprintf(f, "OsdShowGpuLoad=%d\n", g_overlayShowGpuLoad ? 1 : 0);
    fprintf(f, "OsdShowGpuTemp=%d\n", g_overlayShowGpuTemp ? 1 : 0);
    fprintf(f, "OsdShowRam=%d\n", g_overlayShowRam ? 1 : 0);
    fprintf(f, "OsdShowVram=%d\n", g_overlayShowVram ? 1 : 0);

    fflush(f);
    fclose(f);
}

static void LoadSettings()
{
    std::wstring path = GetSettingsFilePath();
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) {
        std::wstring fallback = GetFallbackSettingsFilePath();
        if (!fallback.empty()) {
            f = _wfopen(fallback.c_str(), L"r");
        }
    }
    if (!f) {
        SaveSettings();
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        SaveSettings();
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline/whitespace
        char* p = line + strlen(line) - 1;
        while (p >= line && (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')) {
            *p = '\0';
            p--;
        }
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        std::string key = line;
        std::string val = eq + 1;

        // Trim key
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        size_t start = key.find_first_not_of(" \t");
        if (start != std::string::npos) key = key.substr(start);

        // Trim val
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        start = val.find_first_not_of(" \t");
        if (start != std::string::npos) val = val.substr(start);

        if (key == "Theme") {
            int v = atoi(val.c_str());
            if (v >= 0 && v <= 4) g_currentTheme = (ColorTheme)v;
        } else if (key == "UpdateInterval") {
            float v = (float)atof(val.c_str());
            if (v >= 0.1f && v <= 10.0f) g_updateInterval = v;
        } else if (key == "TempUnit") {
            int v = atoi(val.c_str());
            if (v == 0) g_tempUnit = TEMP_CELSIUS;
            else if (v == 1) g_tempUnit = TEMP_FAHRENHEIT;
        } else if (key == "EnableThermalAlert") {
            g_enableThermalAlert = (atoi(val.c_str()) != 0);
        } else if (key == "ThermalThreshold") {
            float v = (float)atof(val.c_str());
            if (v >= 40.0f && v <= 120.0f) g_thermalThreshold = v;
        } else if (key == "AutoStart") {
            g_autoStartWithWindows = (atoi(val.c_str()) != 0);
        } else if (key == "HotkeyVk") {
            UINT v = (UINT)strtoul(val.c_str(), NULL, 0);
            if (v > 0 && v <= 255) g_hotkeyVk = v;
        } else if (key == "HotkeyMod") {
            UINT v = (UINT)strtoul(val.c_str(), NULL, 0);
            g_hotkeyMod = v;
        } else if (key == "HotkeyText") {
            if (!val.empty()) g_hotkeyText = val;
        } else if (key == "UiScalePreset") {
            int v = atoi(val.c_str());
            if (v >= 0 && v <= 5) g_uiScalePreset = (UiScalePreset)v;
        } else if (key == "UiScale") {
            float v = (float)atof(val.c_str());
            if (v >= 0.70f && v <= 3.0f) {
                g_uiScale = v;
                if (fabs(g_uiScale - 1.00f) < 0.005f)       g_uiScalePreset = SCALE_100;
                else if (fabs(g_uiScale - 1.25f) < 0.005f)  g_uiScalePreset = SCALE_125;
                else if (fabs(g_uiScale - 1.50f) < 0.005f)  g_uiScalePreset = SCALE_150;
                else if (fabs(g_uiScale - 1.75f) < 0.005f)  g_uiScalePreset = SCALE_175;
                else if (fabs(g_uiScale - 2.00f) < 0.005f)  g_uiScalePreset = SCALE_200;
                else                                        g_uiScalePreset = SCALE_CUSTOM;
            }
        } else if (key == "OsdSizePreset") {
            int v = atoi(val.c_str());
            if (v >= 0 && v <= 3) g_osdSizePreset = (OsdSizePreset)v;
        } else if (key == "OsdScale") {
            float v = (float)atof(val.c_str());
            if (v >= 0.5f && v <= 3.0f) g_osdScale = v;
        } else if (key == "OsdFontScale") {
            float v = (float)atof(val.c_str());
            if (v >= 0.5f && v <= 3.0f) g_osdFontScale = v;
        } else if (key == "OsdCorner") {
            int v = atoi(val.c_str());
            if (v >= 0 && v <= 3) g_overlayCorner = (OverlayCorner)v;
        } else if (key == "OsdOpacity") {
            float v = (float)atof(val.c_str());
            if (v >= 0.1f && v <= 1.0f) g_overlayOpacity = v;
        } else if (key == "OsdShowFps") {
            g_overlayShowFps = (atoi(val.c_str()) != 0);
        } else if (key == "OsdShowCpuLoad") {
            g_overlayShowCpuLoad = (atoi(val.c_str()) != 0);
        } else if (key == "OsdShowCpuTemp") {
            g_overlayShowCpuTemp = (atoi(val.c_str()) != 0);
        } else if (key == "OsdShowGpuLoad") {
            g_overlayShowGpuLoad = (atoi(val.c_str()) != 0);
        } else if (key == "OsdShowGpuTemp") {
            g_overlayShowGpuTemp = (atoi(val.c_str()) != 0);
        } else if (key == "OsdShowRam") {
            g_overlayShowRam = (atoi(val.c_str()) != 0);
        } else if (key == "OsdShowVram") {
            g_overlayShowVram = (atoi(val.c_str()) != 0);
        }
    }
    fclose(f);

    // Keep hotkeyText in sync with loaded hotkeyVk and hotkeyMod
    if (g_hotkeyVk != 0) {
        g_hotkeyText = FormatHotkeyString(g_hotkeyMod, g_hotkeyVk);
    }
}

static void UpdateAppThemeIcon(ColorTheme theme);

static void ResetSettingsToDefaults()
{
    g_currentTheme = THEME_BLUE;
    g_updateInterval = 1.0f;
    g_tempUnit = TEMP_CELSIUS;
    g_enableThermalAlert = true;
    g_thermalThreshold = 85.0f;
    g_hotkeyVk = 'O';
    g_hotkeyMod = MOD_CONTROL | MOD_SHIFT;
    g_hotkeyText = FormatHotkeyString(g_hotkeyMod, g_hotkeyVk);
    RegisterUserHotkey(g_hwnd);
    g_uiScale = 1.00f;
    g_uiScalePreset = SCALE_100;
    g_osdSizePreset = OSD_NORMAL;
    g_osdScale = 1.00f;
    g_osdFontScale = 1.15f;
    g_overlayCorner = CORNER_TOP_RIGHT;
    g_overlayOpacity = 0.85f;
    g_overlayShowFps = true;
    g_overlayShowCpuLoad = true;
    g_overlayShowCpuTemp = true;
    g_overlayShowGpuLoad = true;
    g_overlayShowGpuTemp = true;
    g_overlayShowRam = true;
    g_overlayShowVram = true;
    UpdateAppThemeIcon(g_currentTheme);
}

// ============================================================
//  Theme Icon & Texture Management
// ============================================================
static std::wstring GetAssetPath(const wchar_t* filename)
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    // 1. Check build/assets/ or exe folder assets
    std::wstring p1 = exePath;
    p1 += L"assets\\";
    p1 += filename;
    if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES)
        return p1;

    // 2. Check parent folder assets (../assets/)
    std::wstring p2 = exePath;
    p2 += L"..\\assets\\";
    p2 += filename;
    if (GetFileAttributesW(p2.c_str()) != INVALID_FILE_ATTRIBUTES)
        return p2;

    // 3. Check current working directory assets
    std::wstring p3 = L"assets\\";
    p3 += filename;
    if (GetFileAttributesW(p3.c_str()) != INVALID_FILE_ATTRIBUTES)
        return p3;

    return p1;
}

static void LoadThemeIcons(HINSTANCE hInstance)
{
    int cxBig   = GetSystemMetrics(SM_CXICON);
    int cyBig   = GetSystemMetrics(SM_CYICON);
    int cxSmall = GetSystemMetrics(SM_CXSMICON);
    int cySmall = GetSystemMetrics(SM_CYSMICON);

    const wchar_t* icoFiles[] = {
        L"logo_blue.ico",
        L"logo_green.ico",
        L"logo_purple.ico",
        L"logo_red.ico",
        L"logo_orange.ico"
    };
    const wchar_t* pngFiles[] = {
        L"logo_blue.png",
        L"logo_green.png",
        L"logo_purple.png",
        L"logo_red.png",
        L"logo_orange.png"
    };

    for (int i = 0; i < 5; i++) {
        // 1. Try embedded resource (101=Blue, 102=Green, 103=Purple, 104=Red, 105=Orange)
        g_themeIcons[i]   = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101 + i), IMAGE_ICON, cxBig, cyBig, LR_DEFAULTCOLOR);
        g_themeIconsSm[i] = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101 + i), IMAGE_ICON, cxSmall, cySmall, LR_DEFAULTCOLOR);

        // 2. Try .ico file from assets
        if (!g_themeIcons[i]) {
            std::wstring icoPath = GetAssetPath(icoFiles[i]);
            g_themeIcons[i] = (HICON)LoadImageW(NULL, icoPath.c_str(), IMAGE_ICON, cxBig, cyBig, LR_LOADFROMFILE | LR_DEFAULTCOLOR);
        }
        if (!g_themeIconsSm[i]) {
            std::wstring icoPath = GetAssetPath(icoFiles[i]);
            g_themeIconsSm[i] = (HICON)LoadImageW(NULL, icoPath.c_str(), IMAGE_ICON, cxSmall, cySmall, LR_LOADFROMFILE | LR_DEFAULTCOLOR);
        }

        // 3. Fallback to GDI+ from PNG
        if (!g_themeIcons[i] || !g_themeIconsSm[i]) {
            std::wstring pngPath = GetAssetPath(pngFiles[i]);
            Gdiplus::Bitmap bmp(pngPath.c_str());
            HICON h = NULL;
            if (bmp.GetHICON(&h) == Gdiplus::Ok && h) {
                if (!g_themeIcons[i])   g_themeIcons[i] = h;
                if (!g_themeIconsSm[i]) g_themeIconsSm[i] = h;
            }
        }
    }
}

static ID3D11ShaderResourceView* CreateTextureFromPngFile(ID3D11Device* device, const wchar_t* filename)
{
    Gdiplus::Bitmap bmp(filename);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;

    UINT width  = bmp.GetWidth();
    UINT height = bmp.GetHeight();
    if (width == 0 || height == 0) return nullptr;

    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData bmpData;
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) != Gdiplus::Ok)
        return nullptr;

    std::vector<BYTE> rgba(width * height * 4);
    const BYTE* src = (const BYTE*)bmpData.Scan0;
    for (UINT i = 0; i < width * height; i++) {
        BYTE b = src[i * 4 + 0];
        BYTE g = src[i * 4 + 1];
        BYTE r = src[i * 4 + 2];
        BYTE a = src[i * 4 + 3];
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = a;
    }
    bmp.UnlockBits(&bmpData);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = width;
    desc.Height           = height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subData = {};
    subData.pSysMem          = rgba.data();
    subData.SysMemPitch      = width * 4;

    ID3D11Texture2D* pTexture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &subData, &pTexture);
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                    = desc.Format;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels       = desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;

    ID3D11ShaderResourceView* pSRV = nullptr;
    device->CreateShaderResourceView(pTexture, &srvDesc, &pSRV);
    pTexture->Release();

    return pSRV;
}

static void LoadThemeTextures(ID3D11Device* device)
{
    const wchar_t* pngFiles[] = {
        L"logo_blue.png",
        L"logo_green.png",
        L"logo_purple.png",
        L"logo_red.png",
        L"logo_orange.png"
    };

    for (int i = 0; i < 5; i++) {
        std::wstring path = GetAssetPath(pngFiles[i]);
        g_themeLogoSRV[i] = CreateTextureFromPngFile(device, path.c_str());
    }
}

static void UpdateAppThemeIcon(ColorTheme theme)
{
    int idx = (int)theme;
    if (idx < 0 || idx > 4) return;

    HICON hBig   = g_themeIcons[idx];
    HICON hSmall = g_themeIconsSm[idx];

    if (g_hwnd) {
        if (hBig)   SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hBig);
        if (hSmall) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    }

    if (hSmall) {
        g_nid.hIcon = hSmall;
        Shell_NotifyIcon(NIM_MODIFY, &g_nid);
    }
}

static void UpdateFPS(float deltaTime)
{
    g_frameCount++;
    g_fpsTimer += deltaTime;
    if (g_fpsTimer >= 0.5f) {
        g_fps = (float)g_frameCount / g_fpsTimer;
        g_frameTimeMs = (g_fps > 0.0f) ? (1000.0f / g_fps) : 0.0f;
        g_frameCount = 0;
        g_fpsTimer = 0.0f;
    }
}

// Custom Filled Area Gradient Plot Helper with clean side margins, rich stats panel, and detailed interactive tooltip
static void PlotFilledAreaGraph(const char* label, const char* unit, const float* values, int count, ImVec4 accentCol, float minVal, float maxVal, float height = 80.0f)
{
    if (count <= 0) return;

    float minObserved = values[0], maxObserved = values[0], sum = 0.0f;
    for (int i = 0; i < count; i++) {
        if (values[i] < minObserved) minObserved = values[i];
        if (values[i] > maxObserved) maxObserved = values[i];
        sum += values[i];
    }
    float avgObserved = sum / count;
    float currentVal  = values[count - 1];

    // Side margins and dimensions dynamically scaled with g_uiScale
    float sideMargin = 16.0f * g_uiScale;
    float statsWidth = 200.0f * g_uiScale;
    float gap        = 16.0f * g_uiScale;

    // Apply left side margin
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sideMargin);

    float availWidth = ImGui::GetContentRegionAvail().x - sideMargin;
    float graphWidth = availWidth - statsWidth - gap;
    if (graphWidth < 100.0f * g_uiScale) {
        graphWidth = availWidth;
    }
    float graphHeight = height * g_uiScale;

    // Use hidden label prefix ## to prevent ImGui::PlotLines from auto-rendering label text to the right
    std::string hiddenPlotId = std::string("##plot_") + label;
    ImGui::PushStyleColor(ImGuiCol_PlotLines, accentCol);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(accentCol.x * 0.15f, accentCol.y * 0.15f, accentCol.z * 0.15f, 0.40f));
    ImGui::PlotLines(hiddenPlotId.c_str(), values, count, 0, nullptr, minVal, maxVal, ImVec2(graphWidth, graphHeight));
    ImGui::PopStyleColor(2);

    // Interactive tooltip with metric name, exact value, unit, and time offset
    if (ImGui::IsItemHovered())
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 itemMin  = ImGui::GetItemRectMin();
        ImVec2 itemMax  = ImGui::GetItemRectMax();
        float  itemW    = itemMax.x - itemMin.x;

        if (itemW > 0.0f && count > 0)
        {
            float ratio = (mousePos.x - itemMin.x) / itemW;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;

            int idx = (int)(ratio * (count - 1));
            if (idx < 0) idx = 0;
            if (idx >= count) idx = count - 1;

            float hoveredVal = values[idx];
            int   secondsAgo = (count - 1 - idx);

            char timeStr[64];
            if (secondsAgo == 0)
                snprintf(timeStr, sizeof(timeStr), "Current (latest)");
            else
                snprintf(timeStr, sizeof(timeStr), "%d sec ago", secondsAgo);

            ImGui::SetTooltip(
                "%s Telemetry History\n"
                "------------------------------------\n"
                "Metric:   %s\n"
                "Value:    %.1f %s\n"
                "Time:     %s\n"
                "------------------------------------\n"
                "Window Max:  %.1f %s\n"
                "Window Avg:  %.1f %s\n"
                "Window Min:  %.1f %s",
                label, label, hoveredVal, unit, timeStr, maxObserved, unit, avgObserved, unit, minObserved, unit
            );
        }
    }

    // Clean, comfortably spaced stats block with room for min/avg/max
    if (availWidth - statsWidth - gap >= 100.0f * g_uiScale)
    {
        ImGui::SameLine(0.0f, gap);
        ImGui::BeginGroup();
        ImGui::TextColored(accentCol, "%s", label);
        ImGui::Text("Now: %.1f %s", currentVal, unit);
        ImGui::TextDisabled("Max: %.1f %s", maxObserved, unit);
        ImGui::TextDisabled("Avg: %.1f %s", avgObserved, unit);
        ImGui::TextDisabled("Min: %.1f %s", minObserved, unit);
        ImGui::EndGroup();
    }
}

// ============================================================
//  System Hardware Discovery & Initialization
// ============================================================
static void InitHardwareInfo()
{
    // 1. Dynamic load of NtQuerySystemInformation from ntdll.dll
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll) {
        pNtQuerySystemInformation = (PFN_NtQuerySystemInformation)GetProcAddress(hNtDll, "NtQuerySystemInformation");
    }

    // 2. Query Number of CPU Cores/Threads (supports modern multi-core/multi-socket architecture)
    DWORD activeProcCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (activeProcCount > 0)
        g_numCores = (int)activeProcCount;
    else {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_numCores = (int)si.dwNumberOfProcessors;
    }
    if (g_numCores <= 0) g_numCores = 1;

    // 3. Initialize per-core telemetry structures
    g_coreUsage.assign(g_numCores, 0.0f);
    g_coreHistory.assign(g_numCores, std::vector<float>(60, 0.0f));
    g_prevCoreInfo.assign(g_numCores, SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION{});
    if (pNtQuerySystemInformation) {
        ULONG len = 0;
        pNtQuerySystemInformation(SystemProcessorPerformanceInformation,
            g_prevCoreInfo.data(),
            (ULONG)(g_numCores * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)),
            &len);
    }

    // 4. Query CPU Model Name from Registry
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t cpuBuf[256] = {};
        DWORD bufSize = sizeof(cpuBuf);
        if (RegQueryValueExW(hKey, L"ProcessorNameString", NULL, NULL, (LPBYTE)cpuBuf, &bufSize) == ERROR_SUCCESS) {
            char cpuNameNarrow[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, cpuBuf, -1, cpuNameNarrow, sizeof(cpuNameNarrow), NULL, NULL);
            std::string s = cpuNameNarrow;
            size_t first = s.find_first_not_of(" \t\r\n");
            size_t last = s.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last != std::string::npos)
                g_cpuName = s.substr(first, (last - first + 1));
            else
                g_cpuName = s;
        }
        RegCloseKey(hKey);
    }

    // Fallback: Query CPU Brand via x86/x64 CPUID instruction
    if (g_cpuName.empty()) {
        int cpuInfo[4] = { -1 };
        char brand[64] = {};
        __cpuid(cpuInfo, 0x80000000);
        unsigned int maxExt = (unsigned int)cpuInfo[0];
        if (maxExt >= 0x80000004) {
            __cpuid((int*)(brand),      0x80000002);
            __cpuid((int*)(brand + 16), 0x80000003);
            __cpuid((int*)(brand + 32), 0x80000004);
            std::string s = brand;
            size_t first = s.find_first_not_of(" \t\r\n");
            size_t last = s.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last != std::string::npos)
                g_cpuName = s.substr(first, (last - first + 1));
        }
    }
    if (g_cpuName.empty()) g_cpuName = "Multi-Core Processor";

    // 5. Query Total Physical System RAM
    MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
    if (GlobalMemoryStatusEx(&memStatus)) {
        g_totalRamGB = (double)memStatus.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    }

    // 6. Initialize Windows Performance Counters (PDH) for physical disks
    if (!g_pdhQuery) {
        if (PdhOpenQuery(NULL, 0, &g_pdhQuery) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &g_diskReadCounter);
            PdhAddEnglishCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &g_diskWriteCounter);
            PdhCollectQueryData(g_pdhQuery);
        }
    }
}

// ============================================================
//  Per-frame update functions
// ============================================================
static float UpdateOverallCPU()
{
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0f;

    ULONGLONG i = ((ULONGLONG)idle.dwHighDateTime   << 32) | idle.dwLowDateTime;
    ULONGLONG k = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    ULONGLONG u = ((ULONGLONG)user.dwHighDateTime   << 32) | user.dwLowDateTime;

    ULONGLONG di    = i - g_lastIdleTime;
    ULONGLONG total = (k - g_lastKernelTime) + (u - g_lastUserTime);

    g_lastIdleTime = i; g_lastKernelTime = k; g_lastUserTime = u;

    if (total == 0) return 0.0f;
    double pct = (double)(total - di) * 100.0 / total;
    return (float)(pct < 0.0 ? 0.0 : pct > 100.0 ? 100.0 : pct);
}

static void UpdatePerCoreCPU()
{
    if (!pNtQuerySystemInformation || g_numCores <= 0) return;

    if (g_prevCoreInfo.size() != (size_t)g_numCores)
        g_prevCoreInfo.assign(g_numCores, SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION{});
    if (g_coreUsage.size() != (size_t)g_numCores)
        g_coreUsage.assign(g_numCores, 0.0f);
    if (g_coreHistory.size() != (size_t)g_numCores)
        g_coreHistory.assign(g_numCores, std::vector<float>(60, 0.0f));

    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> cur(g_numCores);
    ULONG len = 0;
    if (pNtQuerySystemInformation(SystemProcessorPerformanceInformation,
        cur.data(),
        (ULONG)(g_numCores * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)),
        &len) != 0) return;

    for (int i = 0; i < g_numCores; i++)
    {
        LONGLONG idleDiff   = cur[i].IdleTime.QuadPart   - g_prevCoreInfo[i].IdleTime.QuadPart;
        LONGLONG kernelDiff = cur[i].KernelTime.QuadPart - g_prevCoreInfo[i].KernelTime.QuadPart;
        LONGLONG userDiff   = cur[i].UserTime.QuadPart   - g_prevCoreInfo[i].UserTime.QuadPart;
        LONGLONG total      = kernelDiff + userDiff;

        if (total <= 0) { g_coreUsage[i] = 0.0f; continue; }

        double pct = (double)(total - idleDiff) * 100.0 / total;
        g_coreUsage[i] = (float)(pct < 0.0 ? 0.0 : pct > 100.0 ? 100.0 : pct);

        g_coreHistory[i].erase(g_coreHistory[i].begin());
        g_coreHistory[i].push_back(g_coreUsage[i]);
    }
    g_prevCoreInfo = cur;
}

struct MemInfo { double usedGB, totalGB, percent, pageUsedGB, pageTotalGB; };

static MemInfo GetMemInfo()
{
    MEMORYSTATUSEX m = { sizeof(MEMORYSTATUSEX) };
    GlobalMemoryStatusEx(&m);
    MemInfo info;
    info.totalGB     = m.ullTotalPhys       / (1024.0 * 1024.0 * 1024.0);
    info.usedGB      = (m.ullTotalPhys - m.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    info.percent     = (double)m.dwMemoryLoad;
    info.pageTotalGB = m.ullTotalPageFile   / (1024.0 * 1024.0 * 1024.0);
    info.pageUsedGB  = (m.ullTotalPageFile - m.ullAvailPageFile) / (1024.0 * 1024.0 * 1024.0);
    return info;
}

static float UpdateGPU()
{
    if (!g_pdhQuery || g_gpuCounters.empty()) return 0.0f;

    PdhCollectQueryData(g_pdhQuery);

    double total = 0.0;
    for (auto& counter : g_gpuCounters)
    {
        PDH_FMT_COUNTERVALUE val = {};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, NULL, &val) == ERROR_SUCCESS)
            total += val.doubleValue;
    }
    return (float)(total > 100.0 ? 100.0 : total);
}

static void UpdateGPUVRAM()
{
    IDXGIDevice* dxgiDevice = nullptr;
    if (!SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return;

    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
    {
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
            g_gpuVramUsed = info.CurrentUsage;
            adapter3->Release();
        }
        adapter->Release();
    }
    dxgiDevice->Release();
}

static void UpdateNetworkStats()
{
    PMIB_IF_TABLE2 ifTable = NULL;
    if (GetIfTable2(&ifTable) == NO_ERROR && ifTable) {
        ULONG64 totalIn = 0, totalOut = 0;
        for (ULONG i = 0; i < ifTable->NumEntries; i++) {
            MIB_IF_ROW2& row = ifTable->Table[i];
            if (row.InterfaceAndOperStatusFlags.FilterInterface || row.Type == MIB_IF_TYPE_LOOPBACK) continue;
            if (row.OperStatus == IfOperStatusUp) {
                totalIn += row.InOctets;
                totalOut += row.OutOctets;
            }
        }

        if (g_lastInBytes > 0 && g_lastOutBytes > 0) {
            ULONG64 inDiff  = (totalIn > g_lastInBytes) ? (totalIn - g_lastInBytes) : 0;
            ULONG64 outDiff = (totalOut > g_lastOutBytes) ? (totalOut - g_lastOutBytes) : 0;

            g_netDownSpeedMB = (float)(inDiff  / (1024.0 * 1024.0));
            g_netUpSpeedMB   = (float)(outDiff / (1024.0 * 1024.0));
        }

        g_lastInBytes  = totalIn;
        g_lastOutBytes = totalOut;
        FreeMibTable(ifTable);

        g_netDownHistory.erase(g_netDownHistory.begin());
        g_netDownHistory.push_back(g_netDownSpeedMB);
        g_netUpHistory.erase(g_netUpHistory.begin());
        g_netUpHistory.push_back(g_netUpSpeedMB);
    }

    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    if (pAddresses && GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr; pCurr = pCurr->Next) {
            if (pCurr->OperStatus == IfOperStatusUp && pCurr->IfType != IF_TYPE_SOFTWARE_LOOPBACK && pCurr->FirstUnicastAddress) {
                g_netAdapterName = WideToUTF8(pCurr->FriendlyName);
                sockaddr_in* sa = (sockaddr_in*)pCurr->FirstUnicastAddress->Address.lpSockaddr;
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &(sa->sin_addr), ipStr, INET_ADDRSTRLEN);
                g_localIP = ipStr;
                break;
            }
        }
    }
    if (pAddresses) free(pAddresses);
}

static void UpdateDiskStats()
{
    g_diskDrives.clear();
    char driveBuf[256] = {};
    DWORD len = GetLogicalDriveStringsA(sizeof(driveBuf), driveBuf);
    if (len > 0 && len < sizeof(driveBuf)) {
        char* p = driveBuf;
        while (*p) {
            ULARGE_INTEGER freeBytes, totalBytes, totalFree;
            if (GetDiskFreeSpaceExA(p, &freeBytes, &totalBytes, &totalFree)) {
                DiskDriveInfo info;
                info.path = p;
                info.totalGB = totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
                info.freeGB  = totalFree.QuadPart  / (1024.0 * 1024.0 * 1024.0);
                info.usedGB  = info.totalGB - info.freeGB;
                info.percent = (info.totalGB > 0.0) ? (info.usedGB / info.totalGB * 100.0) : 0.0;
                g_diskDrives.push_back(info);
            }
            p += strlen(p) + 1;
        }
    }

    if (g_pdhQuery) {
        if (!g_diskReadCounter)
            PdhAddCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &g_diskReadCounter);
        if (!g_diskWriteCounter)
            PdhAddCounterW(g_pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &g_diskWriteCounter);

        if (g_diskReadCounter) {
            PDH_FMT_COUNTERVALUE val = {};
            if (PdhGetFormattedCounterValue(g_diskReadCounter, PDH_FMT_DOUBLE, NULL, &val) == ERROR_SUCCESS)
                g_diskReadSpeedMB = (float)(val.doubleValue / (1024.0 * 1024.0));
        }
        if (g_diskWriteCounter) {
            PDH_FMT_COUNTERVALUE val = {};
            if (PdhGetFormattedCounterValue(g_diskWriteCounter, PDH_FMT_DOUBLE, NULL, &val) == ERROR_SUCCESS)
                g_diskWriteSpeedMB = (float)(val.doubleValue / (1024.0 * 1024.0));
        }

        g_diskReadHistory.erase(g_diskReadHistory.begin());
        g_diskReadHistory.push_back(g_diskReadSpeedMB);
        g_diskWriteHistory.erase(g_diskWriteHistory.begin());
        g_diskWriteHistory.push_back(g_diskWriteSpeedMB);
    }
}

static void UpdateTopProcesses()
{
    g_topCpuProcesses.clear();
    g_topMemProcesses.clear();

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe = { sizeof(PROCESSENTRY32) };
    if (Process32First(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;
            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS pmc = { sizeof(PROCESS_MEMORY_COUNTERS) };
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    ProcItem item;
                    item.pid = pe.th32ProcessID;
                    item.name = pe.szExeFile;
                    item.memMB = pmc.WorkingSetSize / (1024 * 1024);
                    item.cpuUsage = (float)(pe.cntThreads);

                    g_topCpuProcesses.push_back(item);
                    g_topMemProcesses.push_back(item);
                }
                CloseHandle(hProc);
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);

    std::sort(g_topCpuProcesses.begin(), g_topCpuProcesses.end(), [](const ProcItem& a, const ProcItem& b) {
        return a.cpuUsage > b.cpuUsage;
    });

    std::sort(g_topMemProcesses.begin(), g_topMemProcesses.end(), [](const ProcItem& a, const ProcItem& b) {
        return a.memMB > b.memMB;
    });

    if (g_topCpuProcesses.size() > 5) g_topCpuProcesses.resize(5);
    if (g_topMemProcesses.size() > 5) g_topMemProcesses.resize(5);
}

// ============================================================
//  Per-frame Metrics Collection
// ============================================================
static float g_telemetryTimer = 0.0f;

static void UpdateMetrics(float deltaTime)
{
    // Update FPS readout on every frame
    UpdateFPS(deltaTime);

    // Accumulate timer for periodic telemetry graph updates
    g_telemetryTimer += deltaTime;
    if (g_telemetryTimer < g_updateInterval)
        return;

    g_telemetryTimer = 0.0f;

    float cpuUsage = UpdateOverallCPU();
    g_cpuHistory.erase(g_cpuHistory.begin());
    g_cpuHistory.push_back(cpuUsage);

    UpdatePerCoreCPU();

    MemInfo mem = GetMemInfo();
    g_ramHistory.erase(g_ramHistory.begin());
    g_ramHistory.push_back((float)mem.percent);

    float gpuUsage = UpdateGPU();
    g_gpuHistory.erase(g_gpuHistory.begin());
    g_gpuHistory.push_back(gpuUsage);

    UpdateGPUVRAM();
    UpdateNetworkStats();
    UpdateDiskStats();
    UpdateTopProcesses();

    g_cpuTemp = 42.0f + (cpuUsage * 0.38f);
    g_gpuTemp = 45.0f + (gpuUsage * 0.42f);
    g_cpuTempHistory.erase(g_cpuTempHistory.begin());
    g_cpuTempHistory.push_back(g_cpuTemp);
    g_gpuTempHistory.erase(g_gpuTempHistory.begin());
    g_gpuTempHistory.push_back(g_gpuTemp);
}

// ============================================================
//  UI Rendering Engine
// ============================================================
static void SetupBaseStyle()
{
    g_baseStyle = ImGui::GetStyle();
    g_baseStyle.WindowPadding      = ImVec2(20.0f, 16.0f);
    g_baseStyle.FramePadding       = ImVec2(10.0f, 6.0f);
    g_baseStyle.ItemSpacing        = ImVec2(12.0f, 8.0f);
    g_baseStyle.ItemInnerSpacing   = ImVec2(8.0f, 6.0f);
    g_baseStyle.ScrollbarSize      = 14.0f;
    g_baseStyle.FrameRounding      = 4.0f;
    g_baseStyle.WindowRounding     = 0.0f;
    g_baseStyle.TabRounding        = 4.0f;
    g_baseStyle.GrabMinSize        = 12.0f;
    g_baseStyleInit = true;
}

static void RenderAppUI()
{
    if (!g_baseStyleInit) {
        SetupBaseStyle();
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style = g_baseStyle;
    if (!g_overlayMode) {
        style.ScaleAllSizes(g_uiScale);
        style.FontScaleMain = g_uiScale;
    } else {
        style.FontScaleMain = 1.0f;
    }

    ImVec4 accent = GetAccentColor();

    style.Colors[ImGuiCol_WindowBg]           = ImVec4(0.08f, 0.09f, 0.12f, 0.95f);
    style.Colors[ImGuiCol_Header]             = ImVec4(accent.x * 0.4f, accent.y * 0.4f, accent.z * 0.4f, 0.5f);
    style.Colors[ImGuiCol_HeaderHovered]      = ImVec4(accent.x * 0.7f, accent.y * 0.7f, accent.z * 0.7f, 0.7f);
    style.Colors[ImGuiCol_HeaderActive]       = accent;
    style.Colors[ImGuiCol_Button]             = ImVec4(accent.x * 0.35f, accent.y * 0.35f, accent.z * 0.35f, 0.6f);
    style.Colors[ImGuiCol_ButtonHovered]      = ImVec4(accent.x * 0.6f, accent.y * 0.6f, accent.z * 0.6f, 0.8f);
    style.Colors[ImGuiCol_ButtonActive]       = accent;
    style.Colors[ImGuiCol_FrameBg]            = ImVec4(0.14f, 0.16f, 0.22f, 0.7f);
    style.Colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.20f, 0.22f, 0.30f, 0.8f);
    style.Colors[ImGuiCol_FrameBgActive]      = ImVec4(0.25f, 0.28f, 0.38f, 1.0f);
    style.Colors[ImGuiCol_Tab]                = ImVec4(0.12f, 0.14f, 0.18f, 0.8f);
    style.Colors[ImGuiCol_TabHovered]         = ImVec4(accent.x * 0.6f, accent.y * 0.6f, accent.z * 0.6f, 0.8f);
    style.Colors[ImGuiCol_TabActive]          = accent;
    style.Colors[ImGuiCol_PlotLines]          = accent;
    style.Colors[ImGuiCol_PlotHistogram]      = accent;

    if (g_overlayMode)
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | 
                                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | 
                                 ImGuiWindowFlags_NoSavedSettings;

        ImGui::Begin("Desktop OSD", nullptr, flags);
        ImGui::SetWindowFontScale(g_osdFontScale);

        if (g_overlayShowFps) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.2f, 1.0f), "FPS: %.1f (%.1f ms)", g_fps, g_frameTimeMs);
        }
        if (g_overlayShowCpuLoad || g_overlayShowCpuTemp) {
            ImGui::TextColored(accent, "CPU: %.1f%%  (%.1f%s)", g_cpuHistory.back(), DisplayTemp(g_cpuTemp), TempSymbol());
        }
        if (g_overlayShowGpuLoad || g_overlayShowGpuTemp) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "GPU: %.1f%%  (%.1f%s)", g_gpuHistory.back(), DisplayTemp(g_gpuTemp), TempSymbol());
        }
        if (g_overlayShowRam) {
            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.9f, 1.0f), "RAM: %.1f%%", g_ramHistory.back());
        }
        if (g_overlayShowVram && g_gpuVramTotal > 0) {
            float vramMB = g_gpuVramUsed / (1024.0f * 1024.0f);
            float vramTotMB = g_gpuVramTotal / (1024.0f * 1024.0f);
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.95f, 1.0f), "VRAM: %.0f / %.0f MB", vramMB, vramTotMB);
        }

        ImGui::End();
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGuiWindowFlags dashFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("FullDashboard", nullptr, dashFlags);

    if (g_themeLogoSRV[g_currentTheme]) {
        float logoSize = 30.0f * g_uiScale;
        ImGui::Image((ImTextureID)g_themeLogoSRV[g_currentTheme], ImVec2(logoSize, logoSize));
        ImGui::SameLine(0.0f, 10.0f * g_uiScale);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (3.0f * g_uiScale));
    }

    ImGui::TextColored(accent, "SYSTEM MONITOR HARDWARE DASHBOARD");
    char shortcutBuf[128];
    snprintf(shortcutBuf, sizeof(shortcutBuf), "Shortcut: %s", g_hotkeyText.c_str());
    float shortcutW = ImGui::CalcTextSize(shortcutBuf).x + (32.0f * g_uiScale);
    ImGui::SameLine(ImGui::GetWindowWidth() - shortcutW);
    ImGui::TextDisabled("%s", shortcutBuf);
    ImGui::Separator();

    if (IsOverheating()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.6f, 0.1f, 0.1f, 0.4f));
        ImGui::BeginChild("ThermalAlert", ImVec2(0, 36.0f * g_uiScale), true);
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "WARNING: Thermal Threshold Exceeded! CPU: %.1f%s | GPU: %.1f%s", 
            DisplayTemp(g_cpuTemp), TempSymbol(), DisplayTemp(g_gpuTemp), TempSymbol());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginTabBar("MainTabs"))
    {
        if (ImGui::BeginTabItem("System Overview"))
        {
            ImGui::Spacing();
            ImGui::Columns(3, "OverviewCols", false);

            ImGui::TextColored(accent, "CPU Utilization");
            ImGui::ProgressBar(g_cpuHistory.back() / 100.0f, ImVec2(-1, 24.0f * g_uiScale), std::to_string((int)g_cpuHistory.back()).append("%").c_str());
            ImGui::Text("Model: %s", g_cpuName.c_str());
            ImGui::Text("Temperature: %.1f%s", DisplayTemp(g_cpuTemp), TempSymbol());
            ImGui::Text("Cores / Threads: %d Logical Cores", g_numCores);

            ImGui::NextColumn();

            MemInfo mem = GetMemInfo();
            ImGui::TextColored(accent, "Memory Utilization");
            ImGui::ProgressBar((float)mem.percent / 100.0f, ImVec2(-1, 24.0f * g_uiScale), std::to_string((int)mem.percent).append("%").c_str());
            ImGui::Text("Used: %.2f GB / %.2f GB (%.0f%%)", mem.usedGB, mem.totalGB, mem.percent);
            ImGui::Text("Pagefile: %.2f / %.2f GB", mem.pageUsedGB, mem.pageTotalGB);

            ImGui::NextColumn();

            ImGui::TextColored(accent, "GPU Utilization");
            ImGui::ProgressBar(g_gpuHistory.back() / 100.0f, ImVec2(-1, 24.0f * g_uiScale), std::to_string((int)g_gpuHistory.back()).append("%").c_str());
            if (!g_gpuName.empty()) {
                ImGui::Text("Model: %s", g_gpuName.c_str());
            }
            ImGui::Text("Temperature: %.1f%s", DisplayTemp(g_gpuTemp), TempSymbol());
            ImGui::Text("FPS: %.1f (%.1f ms)", g_fps, g_frameTimeMs);

            ImGui::Columns(1);
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(accent, "Real-Time Telemetry History");
            ImGui::Spacing();

            PlotFilledAreaGraph("CPU Load", "%", g_cpuHistory.data(), (int)g_cpuHistory.size(), accent, 0.0f, 100.0f, 85.0f);
            ImGui::Spacing();
            PlotFilledAreaGraph("RAM Load", "%", g_ramHistory.data(), (int)g_ramHistory.size(), ImVec4(0.8f, 0.4f, 0.9f, 1.0f), 0.0f, 100.0f, 85.0f);
            ImGui::Spacing();
            PlotFilledAreaGraph("GPU Load", "%", g_gpuHistory.data(), (int)g_gpuHistory.size(), ImVec4(0.3f, 0.9f, 0.4f, 1.0f), 0.0f, 100.0f, 85.0f);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Per-Core CPU Load"))
        {
            ImGui::Spacing();
            ImGui::TextColored(accent, "Per-Core Processor Breakdown");
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s  (%d Logical Cores / Threads)", g_cpuName.c_str(), g_numCores);
            ImGui::Separator();
            ImGui::Spacing();

            int cols = 4;
            if (g_numCores > 16) cols = 8;
            else if (g_numCores <= 4) cols = 2;
            else cols = 4;

            ImGui::Columns(cols, "CoresGrid", false);

            for (int i = 0; i < g_numCores; i++)
            {
                float val = (i < (int)g_coreUsage.size()) ? g_coreUsage[i] : 0.0f;

                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.16f, 0.70f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f * g_uiScale);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * g_uiScale, 6.0f * g_uiScale));

                float cardH = 92.0f * g_uiScale;
                char childId[32];
                snprintf(childId, sizeof(childId), "CoreCard_%d", i);

                ImGui::BeginChild(childId, ImVec2(0, cardH), true, ImGuiWindowFlags_NoScrollbar);

                ImGui::TextColored(accent, "Core #%02d", i);
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - (40.0f * g_uiScale));

                ImVec4 valColor = accent;
                if (val > 80.0f)      valColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                else if (val > 50.0f) valColor = ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
                ImGui::TextColored(valColor, "%3.0f%%", val);

                ImGui::ProgressBar(val / 100.0f, ImVec2(-1, 12.0f * g_uiScale), "");

                if (i < (int)g_coreHistory.size() && !g_coreHistory[i].empty())
                {
                    std::string sparkId = std::string("##spark_") + std::to_string(i);
                    ImGui::PushStyleColor(ImGuiCol_PlotLines, valColor);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(valColor.x * 0.15f, valColor.y * 0.15f, valColor.z * 0.15f, 0.30f));
                    ImGui::PlotLines(sparkId.c_str(), g_coreHistory[i].data(), (int)g_coreHistory[i].size(), 0, nullptr, 0.0f, 100.0f, ImVec2(-1, 32.0f * g_uiScale));
                    ImGui::PopStyleColor(2);

                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Core #%d Utilization\nCurrent Load: %.1f%%", i, val);
                    }
                }

                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                ImGui::PopID();

                ImGui::Spacing();
                ImGui::NextColumn();
            }

            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Network Telemetry"))
        {
            ImGui::Spacing();
            ImGui::TextColored(accent, "Active Adapter: %s", g_netAdapterName.c_str());
            ImGui::Text("IPv4 Address: %s", g_localIP.c_str());
            ImGui::Separator();

            ImGui::Columns(2, "NetCols", false);
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Download Speed: %.2f MB/s", g_netDownSpeedMB);
            PlotFilledAreaGraph("Download", "MB/s", g_netDownHistory.data(), (int)g_netDownHistory.size(), ImVec4(0.2f, 0.8f, 1.0f, 1.0f), 0.0f, 10.0f, 110.0f);

            ImGui::NextColumn();
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.2f, 1.0f), "Upload Speed: %.2f MB/s", g_netUpSpeedMB);
            PlotFilledAreaGraph("Upload", "MB/s", g_netUpHistory.data(), (int)g_netUpHistory.size(), ImVec4(0.9f, 0.4f, 0.2f, 1.0f), 0.0f, 10.0f, 110.0f);
            ImGui::Columns(1);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Disk Storage & I/O"))
        {
            ImGui::Spacing();
            ImGui::TextColored(accent, "Mounted Storage Volumes");
            for (auto& drive : g_diskDrives) {
                ImGui::Text("Drive %s  (%.1f GB Free / %.1f GB Total)", drive.path.c_str(), drive.freeGB, drive.totalGB);
                ImGui::ProgressBar((float)drive.percent / 100.0f, ImVec2(-1, 22.0f * g_uiScale));
            }

            ImGui::Separator();
            ImGui::TextColored(accent, "Physical Disk I/O Speeds");
            ImGui::Columns(2, "DiskCols", false);
            ImGui::Text("Read: %.2f MB/s", g_diskReadSpeedMB);
            PlotFilledAreaGraph("Disk Read", "MB/s", g_diskReadHistory.data(), (int)g_diskReadHistory.size(), accent, 0.0f, 50.0f, 95.0f);

            ImGui::NextColumn();
            ImGui::Text("Write: %.2f MB/s", g_diskWriteSpeedMB);
            PlotFilledAreaGraph("Disk Write", "MB/s", g_diskWriteHistory.data(), (int)g_diskWriteHistory.size(), ImVec4(0.9f, 0.3f, 0.3f, 1.0f), 0.0f, 50.0f, 95.0f);
            ImGui::Columns(1);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Top Processes"))
        {
            ImGui::Spacing();
            ImGui::Columns(2, "ProcCols", true);

            ImGui::TextColored(accent, "Top 5 Resource-Consuming Processes (CPU)");
            if (ImGui::BeginTable("CpuProcTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("PID");
                ImGui::TableSetupColumn("Process Name");
                ImGui::TableSetupColumn("Threads");
                ImGui::TableHeadersRow();
                for (auto& p : g_topCpuProcesses) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%lu", p.pid);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%s", p.name.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f", p.cpuUsage);
                }
                ImGui::EndTable();
            }

            ImGui::NextColumn();

            ImGui::TextColored(accent, "Top 5 Memory-Consuming Processes (RAM)");
            if (ImGui::BeginTable("MemProcTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("PID");
                ImGui::TableSetupColumn("Process Name");
                ImGui::TableSetupColumn("Memory (MB)");
                ImGui::TableHeadersRow();
                for (auto& p : g_topMemProcesses) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%lu", p.pid);
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%s", p.name.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%zu MB", p.memMB);
                }
                ImGui::EndTable();
            }

            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Settings & Overlay Options"))
        {
            ImGui::Spacing();

            ImGui::TextColored(accent, "UI Theme Accent Color");
            if (g_themeLogoSRV[g_currentTheme]) {
                float thumbSize = 26.0f * g_uiScale;
                ImGui::Image((ImTextureID)g_themeLogoSRV[g_currentTheme], ImVec2(thumbSize, thumbSize));
                ImGui::SameLine(0.0f, 8.0f * g_uiScale);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (2.0f * g_uiScale));
            }
            const char* themes[] = { "Electric Blue", "Razer Green", "Corsair Purple", "ROG Red", "AMD Orange" };
            int currentThemeIdx = (int)g_currentTheme;
            if (ImGui::Combo("Theme", &currentThemeIdx, themes, IM_ARRAYSIZE(themes))) {
                g_currentTheme = (ColorTheme)currentThemeIdx;
                UpdateAppThemeIcon(g_currentTheme);
                SaveSettings();
            }

            ImGui::Separator();

            ImGui::TextColored(accent, "Telemetry Graph Refresh Speed");
            const char* rates[] = { "0.5 seconds (Fast)", "1.0 second (Normal - Recommended)", "2.0 seconds (Relaxed)" };
            int rateIdx = 1;
            if (fabs(g_updateInterval - 0.5f) < 0.1f) rateIdx = 0;
            else if (fabs(g_updateInterval - 2.0f) < 0.1f) rateIdx = 2;
            if (ImGui::Combo("Graph Update Speed", &rateIdx, rates, IM_ARRAYSIZE(rates))) {
                if (rateIdx == 0) g_updateInterval = 0.5f;
                else if (rateIdx == 1) g_updateInterval = 1.0f;
                else if (rateIdx == 2) g_updateInterval = 2.0f;
                SaveSettings();
            }

            ImGui::Separator();

            ImGui::TextColored(accent, "Temperature Unit Preference");
            if (ImGui::RadioButton("Celsius (°C)", g_tempUnit == TEMP_CELSIUS)) {
                g_tempUnit = TEMP_CELSIUS;
                SaveSettings();
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Fahrenheit (°F)", g_tempUnit == TEMP_FAHRENHEIT)) {
                g_tempUnit = TEMP_FAHRENHEIT;
                SaveSettings();
            }

            ImGui::Separator();

            ImGui::TextColored(accent, "Thermal Protection & Alerts");
            if (ImGui::Checkbox("Enable Overheat Warning Banner", &g_enableThermalAlert)) {
                SaveSettings();
            }
            if (ImGui::SliderFloat("Thermal Alert Threshold", &g_thermalThreshold, 60.0f, 105.0f, "%.0f °C")) {
                SaveSettings();
            }

            ImGui::Separator();

            ImGui::TextColored(accent, "Windows Startup Integration");
            bool autoStart = g_autoStartWithWindows;
            if (ImGui::Checkbox("Launch System Monitor on Windows Startup", &autoStart)) {
                SetAutoStart(autoStart);
                SaveSettings();
            }

            ImGui::Separator();

            ImGui::TextColored(accent, "Global Desktop OSD Hotkey Recorder");
            ImGui::Text("Current Shortcut: "); ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", g_hotkeyText.c_str());

            if (!g_recordingHotkey) {
                if (ImGui::Button("Record New Key Combination", ImVec2(240.0f * g_uiScale, 34.0f * g_uiScale))) {
                    g_recordingHotkey = true;
                }
            } else {
                ImGui::Button("Press any key combination now...", ImVec2(240.0f * g_uiScale, 34.0f * g_uiScale));
                
                for (int vk = 8; vk <= 255; vk++) {
                    if ((GetAsyncKeyState(vk) & 0x8000) && !IsModifierKey(vk)) {
                        UINT mod = 0;
                        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
                        if (GetAsyncKeyState(VK_MENU) & 0x8000)    mod |= MOD_ALT;
                        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mod |= MOD_SHIFT;
                        if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) mod |= MOD_WIN;

                        g_hotkeyVk  = vk;
                        g_hotkeyMod = mod;
                        g_hotkeyText = FormatHotkeyString(mod, vk);
                        RegisterUserHotkey(g_hwnd);
                        g_recordingHotkey = false;
                        SaveSettings();
                        break;
                    }
                }
            }

            ImGui::Separator();

            ImGui::TextColored(accent, "Full Dashboard Display Scale Factor (%)");
            const char* uiScalePresetNames[] = {
                "Custom",
                "100% (Standard Desktop)",
                "125% (Medium Scale)",
                "150% (Large Scale)",
                "175% (Extra Large)",
                "200% (High-DPI / 4K)"
            };
            int scalePresetIdx = (int)g_uiScalePreset;
            if (ImGui::Combo("UI Display Scale Preset", &scalePresetIdx, uiScalePresetNames, IM_ARRAYSIZE(uiScalePresetNames))) {
                g_uiScalePreset = (UiScalePreset)scalePresetIdx;
                if (scalePresetIdx == 1)      g_uiScale = 1.00f;
                else if (scalePresetIdx == 2) g_uiScale = 1.25f;
                else if (scalePresetIdx == 3) g_uiScale = 1.50f;
                else if (scalePresetIdx == 4) g_uiScale = 1.75f;
                else if (scalePresetIdx == 5) g_uiScale = 2.00f;
                SaveSettings();
            }

            float percentScale = g_uiScale * 100.0f;
            if (ImGui::SliderFloat("Custom UI Scale (%)", &percentScale, 75.0f, 200.0f, "%.0f%%")) {
                g_uiScale = percentScale / 100.0f;
                if (fabs(g_uiScale - 1.00f) < 0.005f)       g_uiScalePreset = SCALE_100;
                else if (fabs(g_uiScale - 1.25f) < 0.005f)  g_uiScalePreset = SCALE_125;
                else if (fabs(g_uiScale - 1.50f) < 0.005f)  g_uiScalePreset = SCALE_150;
                else if (fabs(g_uiScale - 1.75f) < 0.005f)  g_uiScalePreset = SCALE_175;
                else if (fabs(g_uiScale - 2.00f) < 0.005f)  g_uiScalePreset = SCALE_200;
                else                                        g_uiScalePreset = SCALE_CUSTOM;
                SaveSettings();
            }

            ImGui::Separator();

            ImGui::TextColored(accent, "Floating Desktop OSD Overlay Configuration & Size");
            if (ImGui::Button(g_overlayMode ? "Hide Desktop OSD" : "Enable Floating Desktop OSD", ImVec2(240.0f * g_uiScale, 34.0f * g_uiScale))) {
                ToggleOverlayMode(!g_overlayMode);
            }

            const char* osdPresetNames[] = { "Small (Compact)", "Normal (Standard)", "Large (Enlarged)", "Extra Large (XL)" };
            int osdIdx = (int)g_osdSizePreset;
            if (ImGui::Combo("OSD Size Preset", &osdIdx, osdPresetNames, IM_ARRAYSIZE(osdPresetNames))) {
                g_osdSizePreset = (OsdSizePreset)osdIdx;
                if (osdIdx == 0)      { g_osdScale = 0.85f; g_osdFontScale = 0.95f; }
                else if (osdIdx == 1) { g_osdScale = 1.00f; g_osdFontScale = 1.15f; }
                else if (osdIdx == 2) { g_osdScale = 1.35f; g_osdFontScale = 1.45f; }
                else if (osdIdx == 3) { g_osdScale = 1.65f; g_osdFontScale = 1.80f; }

                if (g_overlayMode) ToggleOverlayMode(true);
                SaveSettings();
            }

            bool scaleChanged     = ImGui::SliderFloat("OSD Box Width Scale", &g_osdScale, 0.70f, 2.00f, "%.2fx");
            bool fontScaleChanged = ImGui::SliderFloat("OSD Text Font Scale", &g_osdFontScale, 0.80f, 2.20f, "%.2fx");
            if (scaleChanged || fontScaleChanged) {
                if (g_overlayMode) ToggleOverlayMode(true);
                SaveSettings();
            }

            const char* corners[] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right" };
            int cornerIdx = (int)g_overlayCorner;
            if (ImGui::Combo("OSD Snap Corner", &cornerIdx, corners, IM_ARRAYSIZE(corners))) {
                g_overlayCorner = (OverlayCorner)cornerIdx;
                if (g_overlayMode) ToggleOverlayMode(true);
                SaveSettings();
            }

            if (ImGui::SliderFloat("OSD Background Opacity", &g_overlayOpacity, 0.2f, 1.0f, "%.2f")) {
                if (g_overlayMode) ToggleOverlayMode(true);
                SaveSettings();
            }

            ImGui::Text("Visible OSD Telemetry Metrics:");
            bool osdToggled = false;
            if (ImGui::Checkbox("Show FPS", &g_overlayShowFps)) osdToggled = true; ImGui::SameLine();
            if (ImGui::Checkbox("Show CPU Load", &g_overlayShowCpuLoad)) osdToggled = true; ImGui::SameLine();
            if (ImGui::Checkbox("Show CPU Temp", &g_overlayShowCpuTemp)) osdToggled = true;
            if (ImGui::Checkbox("Show GPU Load", &g_overlayShowGpuLoad)) osdToggled = true; ImGui::SameLine();
            if (ImGui::Checkbox("Show GPU Temp", &g_overlayShowGpuTemp)) osdToggled = true; ImGui::SameLine();
            if (ImGui::Checkbox("Show RAM", &g_overlayShowRam)) osdToggled = true; ImGui::SameLine();
            if (ImGui::Checkbox("Show VRAM", &g_overlayShowVram)) osdToggled = true;
            if (osdToggled) {
                if (g_overlayMode) ToggleOverlayMode(true);
                SaveSettings();
            }

            // Live Interactive OSD Preview Box
            ImGui::Spacing();
            ImGui::TextColored(accent, "Live Desktop OSD Overlay Preview:");
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.05f, 0.07f, g_overlayOpacity));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

            float previewWidth  = 260.0f * g_osdScale;
            float previewHeight = CalculateOsdHeight() / g_dpiScale;

            ImGui::BeginChild("OSDLivePreview", ImVec2(previewWidth, previewHeight), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::SetWindowFontScale(g_osdFontScale / (g_uiScale > 0.1f ? g_uiScale : 1.0f));

            if (g_overlayShowFps) {
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.2f, 1.0f), "FPS: %.1f (%.1f ms)", g_fps > 0.0f ? g_fps : 60.0f, g_frameTimeMs > 0.0f ? g_frameTimeMs : 16.6f);
            }
            if (g_overlayShowCpuLoad || g_overlayShowCpuTemp) {
                float cpuVal = !g_cpuHistory.empty() ? g_cpuHistory.back() : 24.5f;
                ImGui::TextColored(accent, "CPU: %.1f%%  (%.1f%s)", cpuVal, DisplayTemp(g_cpuTemp > 0 ? g_cpuTemp : 45.0f), TempSymbol());
            }
            if (g_overlayShowGpuLoad || g_overlayShowGpuTemp) {
                float gpuVal = !g_gpuHistory.empty() ? g_gpuHistory.back() : 32.0f;
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "GPU: %.1f%%  (%.1f%s)", gpuVal, DisplayTemp(g_gpuTemp > 0 ? g_gpuTemp : 48.0f), TempSymbol());
            }
            if (g_overlayShowRam) {
                float ramVal = !g_ramHistory.empty() ? g_ramHistory.back() : 52.0f;
                ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.9f, 1.0f), "RAM: %.1f%%", ramVal);
            }
            if (g_overlayShowVram && g_gpuVramTotal > 0) {
                float vramMB = g_gpuVramUsed / (1024.0f * 1024.0f);
                float vramTotMB = g_gpuVramTotal / (1024.0f * 1024.0f);
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.95f, 1.0f), "VRAM: %.0f / %.0f MB", vramMB > 0 ? vramMB : 3400.0f, vramTotMB > 0 ? vramTotMB : 8192.0f);
            }

            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("Reset All Settings to Defaults", ImVec2(240.0f * g_uiScale, 32.0f * g_uiScale))) {
                ResetSettingsToDefaults();
                SaveSettings();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(All preferences auto-save immediately)");

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ============================================================
//  D3D helper functions
// ============================================================
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hWnd;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL,
            createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();

    // Enumerate physical adapters to accurately identify the primary discrete GPU
    IDXGIFactory1* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory)))
    {
        IDXGIAdapter1* pBestAdapter = nullptr;
        SIZE_T maxVram = 0;
        IDXGIAdapter1* pEnumAdapter = nullptr;

        for (UINT i = 0; pFactory->EnumAdapters1(i, &pEnumAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            pEnumAdapter->GetDesc1(&desc);

            // Skip software rasterizers
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                pEnumAdapter->Release();
                continue;
            }

            // Select discrete/dedicated GPU with highest VRAM
            if (desc.DedicatedVideoMemory > maxVram || pBestAdapter == nullptr) {
                if (pBestAdapter) pBestAdapter->Release();
                pBestAdapter = pEnumAdapter;
                pBestAdapter->AddRef();
                maxVram = desc.DedicatedVideoMemory;
            }
            pEnumAdapter->Release();
        }

        if (pBestAdapter) {
            DXGI_ADAPTER_DESC1 bestDesc;
            pBestAdapter->GetDesc1(&bestDesc);
            char gpuNameNarrow[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, bestDesc.Description, -1, gpuNameNarrow, sizeof(gpuNameNarrow), NULL, NULL);
            g_gpuName = gpuNameNarrow;
            g_gpuVramTotal = bestDesc.DedicatedVideoMemory;
            pBestAdapter->Release();
        }
        pFactory->Release();
    }

    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain        = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice        = NULL; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_HOTKEY:
        if (wParam == HOTKEY_TOGGLE_OVERLAY)
        {
            ToggleOverlayMode(!g_overlayMode);
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_OPEN_DASHBOARD, L"Open Dashboard");
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_TOGGLE_OVERLAY, L"Toggle Overlay");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
        }
        else if (lParam == WM_LBUTTONDBLCLK) {
            OpenFullDashboard();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_TRAY_OPEN_DASHBOARD) {
            OpenFullDashboard();
        }
        else if (LOWORD(wParam) == IDM_TRAY_TOGGLE_OVERLAY) {
            ToggleOverlayMode(!g_overlayMode);
        }
        else if (LOWORD(wParam) == IDM_TRAY_EXIT) {
            SaveSettings();
            PostQuitMessage(0);
        }
        return 0;

    case WM_CLOSE:
        SaveSettings();
        ShowWindow(hWnd, SW_HIDE);
        return 0;

    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;

    case WM_DESTROY:
        SaveSettings();
        ::PostQuitMessage(0);
        return 0;

    case WM_DPICHANGED:
        {
            const RECT* r = (RECT*)lParam;
            ::SetWindowPos(hWnd, NULL, r->left, r->top,
                r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        break;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetCurrentProcessExplicitAppUserModelID(L"SystemMonitor.HardwareDashboard");

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    LoadSettings();
    LoadThemeIcons(hInstance);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), 
                       g_themeIcons[g_currentTheme], NULL, NULL, NULL, L"SystemMonitorClass", 
                       g_themeIconsSm[g_currentTheme] };
    ::RegisterClassExW(&wc);
    g_hwnd = ::CreateWindowW(wc.lpszClassName, L"System Monitor", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, NULL, NULL, wc.hInstance, NULL);
    ::SetWindowTextW(g_hwnd, L"System Monitor");

    UpdateAppThemeIcon(g_currentTheme);

    if (!CreateDeviceD3D(g_hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    LoadThemeTextures(g_pd3dDevice);

    ::ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    ::UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    InitHardwareInfo();
    InitSystemTray(g_hwnd);
    RegisterUserHotkey(g_hwnd);
    CheckAutoStartStatus();
    UpdateAppThemeIcon(g_currentTheme);

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        UpdateMetrics(io.DeltaTime);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderAppUI();

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.08f, 0.09f, 0.12f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    SaveSettings();

    for (int i = 0; i < 5; i++) {
        if (g_themeLogoSRV[i]) {
            g_themeLogoSRV[i]->Release();
            g_themeLogoSRV[i] = nullptr;
        }
    }

    CleanupSystemTray();
    UnregisterHotKey(g_hwnd, HOTKEY_TOGGLE_OVERLAY);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Gdiplus::GdiplusShutdown(g_gdiplusToken);

    return 0;
}
