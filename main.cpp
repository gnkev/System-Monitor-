#include <windows.h>
#include <vector>
#include <string>
#include <dxgi1_4.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// D3D globals
static ID3D11Device*           g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext     = nullptr;
static IDXGISwapChain*         g_pSwapChain            = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView  = nullptr;

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
//  Static system info (read once at startup)
// ============================================================
static int         g_numCores   = 0;
static std::string g_cpuName;
static std::string g_gpuName;
static double      g_totalRamGB = 0.0;

// ============================================================
//  CPU tracking
// ============================================================
static ULONGLONG g_lastIdleTime = 0, g_lastKernelTime = 0, g_lastUserTime = 0;
static std::vector<float> g_cpuHistory(60, 0.0f);

static std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> g_prevCoreInfo;
static std::vector<float>              g_coreUsage;
static std::vector<std::vector<float>> g_coreHistory;

// ============================================================
//  Memory tracking
// ============================================================
static std::vector<float> g_ramHistory(60, 0.0f);

// ============================================================
//  GPU tracking
// ============================================================
static std::vector<float>       g_gpuHistory(60, 0.0f);
static PDH_HQUERY               g_pdhQuery    = NULL;
static std::vector<PDH_HCOUNTER> g_gpuCounters;
static SIZE_T                   g_gpuVramUsed  = 0;
static SIZE_T                   g_gpuVramTotal = 0;

// ============================================================
//  Init helpers
// ============================================================
static std::string WideToUTF8(const wchar_t* wide)
{
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, sizeof(buf), NULL, NULL);
    // trim leading spaces
    char* p = buf;
    while (*p == ' ') ++p;
    return p;
}

static std::string GetCPUName()
{
    char buf[256] = {};
    DWORD size = sizeof(buf);
    RegGetValueA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        "ProcessorNameString", RRF_RT_REG_SZ, NULL, buf, &size);
    std::string s = buf;
    size_t start = s.find_first_not_of(' ');
    return start != std::string::npos ? s.substr(start) : s;
}

static void InitSystemInfo()
{
    g_cpuName = GetCPUName();

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    g_numCores = (int)si.dwNumberOfProcessors;

    g_coreUsage.assign(g_numCores, 0.0f);
    g_prevCoreInfo.resize(g_numCores);
    g_coreHistory.assign(g_numCores, std::vector<float>(60, 0.0f));

    // Load NtQuerySystemInformation dynamically from ntdll
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
        pNtQuerySystemInformation = (PFN_NtQuerySystemInformation)
            GetProcAddress(ntdll, "NtQuerySystemInformation");

    // Prime per-core baseline
    if (pNtQuerySystemInformation)
    {
        ULONG len = 0;
        pNtQuerySystemInformation(SystemProcessorPerformanceInformation,
            g_prevCoreInfo.data(),
            (ULONG)(g_numCores * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)),
            &len);
    }

    // Total RAM
    MEMORYSTATUSEX mem = { sizeof(MEMORYSTATUSEX) };
    GlobalMemoryStatusEx(&mem);
    g_totalRamGB = mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
}

static void InitGPU()
{
    // GPU name from DXGI
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
    {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
        {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
                g_gpuName = WideToUTF8(desc.Description);

            // Total dedicated VRAM
            g_gpuVramTotal = desc.DedicatedVideoMemory;
            adapter->Release();
        }
        dxgiDevice->Release();
    }
    if (g_gpuName.empty()) g_gpuName = "Unknown GPU";

    // PDH: GPU engine utilization (3D engines only)
    if (PdhOpenQuery(NULL, 0, &g_pdhQuery) != ERROR_SUCCESS) return;

    DWORD pathLen = 0;
    PdhExpandWildCardPathW(NULL,
        L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
        NULL, &pathLen, 0);

    if (pathLen > 0)
    {
        std::vector<wchar_t> pathList(pathLen);
        if (PdhExpandWildCardPathW(NULL,
            L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
            pathList.data(), &pathLen, 0) == ERROR_SUCCESS)
        {
            for (const wchar_t* p = pathList.data(); *p; p += wcslen(p) + 1)
            {
                PDH_HCOUNTER counter = NULL;
                if (PdhAddCounterW(g_pdhQuery, p, 0, &counter) == ERROR_SUCCESS)
                    g_gpuCounters.push_back(counter);
            }
        }
    }

    PdhCollectQueryData(g_pdhQuery); // prime
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
    if (!pNtQuerySystemInformation) return;

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

// ============================================================
//  UI helpers
// ============================================================
static std::string FormatUptime()
{
    ULONGLONG ms = GetTickCount64();
    ULONGLONG s  = ms / 1000;
    ULONGLONG m  = s  / 60;
    ULONGLONG h  = m  / 60;
    ULONGLONG d  = h  / 24;

    char buf[64];
    if (d > 0)
        snprintf(buf, sizeof(buf), "%llu d  %02llu:%02llu:%02llu", d, h % 24, m % 60, s % 60);
    else
        snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", h % 24, m % 60, s % 60);
    return buf;
}

// Green -> yellow -> red based on utilization
static ImVec4 UsageColor(float pct)
{
    if (pct < 50.0f) return ImVec4(0.20f, 0.80f, 0.30f, 1.0f);
    if (pct < 80.0f) return ImVec4(1.00f, 0.70f, 0.00f, 1.0f);
    return               ImVec4(1.00f, 0.20f, 0.20f, 1.0f);
}

static void ColoredProgressBar(float frac, ImVec2 size = ImVec2(-1.0f, 0.0f))
{
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UsageColor(frac * 100.0f));
    ImGui::ProgressBar(frac, size);
    ImGui::PopStyleColor();
}

// ============================================================
//  Style
// ============================================================
static void ApplyModernStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(15, 15);
    style.WindowRounding    = 10.0f;
    style.FramePadding      = ImVec2(5, 5);
    style.FrameRounding     = 6.0f;
    style.ItemSpacing       = ImVec2(12, 8);
    style.ItemInnerSpacing  = ImVec2(8, 6);
    style.IndentSpacing     = 25.0f;
    style.ScrollbarSize     = 15.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 20.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;
    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);

    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.36f, 0.42f, 0.47f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    c[ImGuiCol_Border]               = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.28f, 0.35f, 0.40f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.32f, 0.40f, 0.45f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.09f, 0.12f, 0.14f, 0.65f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.02f, 0.02f, 0.02f, 0.39f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.35f, 0.40f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.32f, 0.40f, 0.45f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.28f, 0.35f, 0.40f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.32f, 0.40f, 0.45f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.28f, 0.35f, 0.40f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.32f, 0.40f, 0.45f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.20f, 0.25f, 0.29f, 0.55f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    c[ImGuiCol_Tab]                  = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    c[ImGuiCol_TabActive]            = ImVec4(0.20f, 0.41f, 0.68f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.18f, 0.30f, 0.45f, 1.00f);
    c[ImGuiCol_PlotLines]            = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    c[ImGuiCol_PlotHistogram]        = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
}

// ============================================================
//  WinMain
// ============================================================
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("System Monitor"), NULL };
    ::RegisterClassEx(&wc);
    HWND hwnd = ::CreateWindow(wc.lpszClassName, _T("System Monitor"),
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, NULL, NULL, wc.hInstance, NULL);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ApplyModernStyle();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Initialize monitoring (D3D must be up before InitGPU)
    InitSystemInfo();
    InitGPU();

    // Prime overall CPU baseline
    {
        FILETIME idle, kernel, user;
        GetSystemTimes(&idle, &kernel, &user);
        g_lastIdleTime   = ((ULONGLONG)idle.dwHighDateTime   << 32) | idle.dwLowDateTime;
        g_lastKernelTime = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        g_lastUserTime   = ((ULONGLONG)user.dwHighDateTime   << 32) | user.dwLowDateTime;
    }

    float updateTimer = 0.0f;
    bool  done        = false;

    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Update metrics every second
        updateTimer += io.DeltaTime;
        if (updateTimer >= 1.0f)
        {
            updateTimer = 0.0f;

            float cpu = UpdateOverallCPU();
            g_cpuHistory.erase(g_cpuHistory.begin());
            g_cpuHistory.push_back(cpu);
            UpdatePerCoreCPU();

            MemInfo mem = GetMemInfo();
            g_ramHistory.erase(g_ramHistory.begin());
            g_ramHistory.push_back((float)mem.percent);

            float gpuPct = UpdateGPU();
            g_gpuHistory.erase(g_gpuHistory.begin());
            g_gpuHistory.push_back(gpuPct);
            UpdateGPUVRAM();
        }

        // ---- Full-screen dashboard window ----
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGuiWindowFlags dashFlags =
            ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize      | ImGuiWindowFlags_NoMove     |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::Begin("Dashboard", NULL, dashFlags);

        // Header row: title + uptime
        ImGui::Text("System Monitor");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 220.0f);
        ImGui::TextDisabled("Uptime:  %s", FormatUptime().c_str());
        ImGui::Separator();

        // System info row
        ImGui::Spacing();
        ImGui::TextDisabled("%s     %d logical processors     %.1f GB RAM",
            g_cpuName.c_str(), g_numCores, g_totalRamGB);
        ImGui::TextDisabled("GPU: %s", g_gpuName.c_str());
        ImGui::Spacing();

        // ---- Tab bar ----
        if (ImGui::BeginTabBar("PerformanceTabs"))
        {
            // ============================
            //  CPU tab
            // ============================
            if (ImGui::BeginTabItem("CPU"))
            {
                float cpu = g_cpuHistory.back();

                ImGui::Text("Overall  %.1f%%", cpu);
                ColoredProgressBar(cpu / 100.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.26f, 0.59f, 0.98f, 1.0f));
                ImGui::PlotLines("##CPUGraph", g_cpuHistory.data(), (int)g_cpuHistory.size(),
                    0, nullptr, 0.0f, 100.0f, ImVec2(-1, 100));
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Text("Logical Processors  (%d)", g_numCores);
                ImGui::Spacing();

                // Responsive grid of per-core mini graphs
                float availW = ImGui::GetContentRegionAvail().x;
                int   cols   = (g_numCores <= 4) ? 2 : (g_numCores <= 8) ? 4 : (g_numCores <= 16) ? 4 : 6;
                float cellW  = (availW - (cols - 1) * ImGui::GetStyle().ItemSpacing.x) / (float)cols;

                for (int i = 0; i < g_numCores; i++)
                {
                    if (i % cols != 0) ImGui::SameLine();
                    ImGui::BeginGroup();

                    char label[32], graphId[32];
                    snprintf(label,   sizeof(label),   "CPU %d", i);
                    snprintf(graphId, sizeof(graphId), "##c%d",  i);

                    ImGui::TextDisabled("%s", label);
                    ImGui::PushStyleColor(ImGuiCol_PlotLines, UsageColor(g_coreUsage[i]));
                    ImGui::PlotLines(graphId, g_coreHistory[i].data(), (int)g_coreHistory[i].size(),
                        0, nullptr, 0.0f, 100.0f, ImVec2(cellW, 55));
                    ImGui::PopStyleColor();
                    ImGui::TextColored(UsageColor(g_coreUsage[i]), "%.0f%%", g_coreUsage[i]);

                    ImGui::EndGroup();
                }

                ImGui::EndTabItem();
            }

            // ============================
            //  Memory tab
            // ============================
            if (ImGui::BeginTabItem("Memory"))
            {
                MemInfo mem = GetMemInfo();

                ImGui::Text("Physical Memory");
                ImGui::Text("  %.2f GB used  /  %.2f GB total  (%.0f%%)",
                    mem.usedGB, mem.totalGB, mem.percent);
                ColoredProgressBar((float)(mem.usedGB / mem.totalGB));

                ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.56f, 0.83f, 0.26f, 1.0f));
                ImGui::PlotLines("##RAMGraph", g_ramHistory.data(), (int)g_ramHistory.size(),
                    0, nullptr, 0.0f, 100.0f, ImVec2(-1, 100));
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Text("Committed (Page File)");
                double pagePct = mem.pageTotalGB > 0.0 ? (mem.pageUsedGB / mem.pageTotalGB) : 0.0;
                ImGui::Text("  %.2f GB used  /  %.2f GB total  (%.0f%%)",
                    mem.pageUsedGB, mem.pageTotalGB, pagePct * 100.0);
                ImGui::ProgressBar((float)pagePct);

                ImGui::EndTabItem();
            }

            // ============================
            //  GPU tab
            // ============================
            if (ImGui::BeginTabItem("GPU"))
            {
                float gpuPct      = g_gpuHistory.back();
                float vramUsedGB  = (float)(g_gpuVramUsed  / (1024.0 * 1024.0 * 1024.0));
                float vramTotalGB = (float)(g_gpuVramTotal / (1024.0 * 1024.0 * 1024.0));

                ImGui::Text("GPU Utilization  %.1f%%", gpuPct);
                ColoredProgressBar(gpuPct / 100.0f);

                ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.98f, 0.59f, 0.26f, 1.0f));
                ImGui::PlotLines("##GPUGraph", g_gpuHistory.data(), (int)g_gpuHistory.size(),
                    0, nullptr, 0.0f, 100.0f, ImVec2(-1, 100));
                ImGui::PopStyleColor();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Text("VRAM");
                if (vramTotalGB > 0.0f)
                {
                    float frac = vramUsedGB / vramTotalGB;
                    ImGui::Text("  %.2f GB used  /  %.2f GB total  (%.0f%%)",
                        vramUsedGB, vramTotalGB, frac * 100.0f);
                    ColoredProgressBar(frac);
                }
                else
                {
                    ImGui::Text("  %.2f GB used", vramUsedGB);
                }

                if (g_gpuCounters.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("GPU utilization counters not available on this system.");
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();

        // Render
        ImGui::Render();
        const float cc[4] = { 0.11f, 0.15f, 0.17f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Cleanup
    if (g_pdhQuery) PdhCloseQuery(g_pdhQuery);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ============================================================
//  D3D helper functions (unchanged)
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
    return ::DefWindowProc(hWnd, msg, wParam, lParam);
}
