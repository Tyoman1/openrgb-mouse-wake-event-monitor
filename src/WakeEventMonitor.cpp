/*---------------------------------------------------------*\
| WakeEventMonitor.exe                                       |
|                                                            |
|  Diagnostic utility for detecting physical wireless mouse  |
|  OFF тЖТ ON events.                                          |
|                                                            |
|  Monitors:                                                 |
|    - WM_DEVICECHANGE (USB/HID topology changes)            |
|    - WM_INPUT_DEVICE_CHANGE (Raw Input device arrival/     |
|      removal via RIDEV_DEVNOTIFY)                          |
|    - Raw Input mouse packets (WM_INPUT)                    |
|    - First-movement-after-gap detection                    |
|                                                            |
|  Usage:                                                    |
|    1. Run as administrator (for WM_DEVICECHANGE)           |
|    2. Press F8 just before turning mouse OFF              |
|    3. Press F9 just after turning mouse ON               |
|    4. Review the log to see what events fired             |
|                                                            |
|  No periodic HID polling.  Event-driven only.              |
\*---------------------------------------------------------*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601  /* Windows 7+ API levels */
#define NTDDI_VERSION 0x06010000
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <dbt.h>
#include <hidusage.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <map>

#pragma comment(lib, "setupapi")
#pragma comment(lib, "hid")
#pragma comment(lib, "user32")

/*---------------------------------------------------------*\
| Logging                                                  |
\*---------------------------------------------------------*/
static FILE* g_log = NULL;

static void OpenLog()
{
    fopen_s(&g_log, "WakeEventMonitor.log", "w");
    if (g_log)
    {
        fprintf(g_log, "WakeEventMonitor diagnostic log\n");
        fprintf(g_log, "================================\n");
        fprintf(g_log, "Timestamp        Event\n");
        fprintf(g_log, "================================\n");
        fflush(g_log);
    }
}

static void LogEvent(const char* fmt, ...)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    char prefix[64];
    sprintf_s(prefix, "%02d:%02d:%02d.%03d ",
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    printf("%s", prefix);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");

    if (g_log)
    {
        fprintf(g_log, "%s", prefix);
        va_start(args, fmt);
        vfprintf(g_log, fmt, args);
        va_end(args);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
}

/*---------------------------------------------------------*\
| Device topology snapshot                                   |
\*---------------------------------------------------------*/
struct DeviceEntry
{
    std::string path;
    WORD        vid;
    WORD        pid;
    int         iface;
    std::string instance_id;
};

static std::vector<DeviceEntry> SnapshotTopology()
{
    std::vector<DeviceEntry> result;

    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO dev_info = SetupDiGetClassDevsW(&hid_guid, NULL, NULL,
                                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE)
        return result;

    SP_DEVICE_INTERFACE_DATA di = { sizeof(SP_DEVICE_INTERFACE_DATA) };
    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, idx, &di);
         idx++)
    {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(dev_info, &di, NULL, 0, &req, NULL);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) continue;

        std::vector<BYTE> buf(req);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &di, detail, (DWORD)buf.size(), &req, NULL))
            continue;

        std::wstring path = detail->DevicePath;

        /* Parse VID, PID, MI */
        WORD vid = 0, pid = 0;
        int mi = -1;
        const wchar_t* s = path.c_str();
        const wchar_t* p;

        p = wcsstr(s, L"vid_");
        if (p) { wchar_t v[16]={0}; wcsncpy_s(v, p+4, 4); vid = (WORD)wcstol(v,NULL,16); }
        p = wcsstr(s, L"pid_");
        if (p) { wchar_t v[16]={0}; wcsncpy_s(v, p+4, 4); pid = (WORD)wcstol(v,NULL,16); }
        p = wcsstr(s, L"mi_");
        if (p) { wchar_t v[16]={0}; wcsncpy_s(v, p+3, 2); mi = (int)wcstol(v,NULL,16); }

        char path_a[512] = {0};
        WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, path_a, (int)sizeof(path_a)-1, NULL, NULL);

        DeviceEntry e;
        e.path = path_a;
        e.vid = vid;
        e.pid = pid;
        e.iface = mi;

        /* Instance ID from device info */
        SP_DEVINFO_DATA did = { sizeof(SP_DEVINFO_DATA) };
        if (SetupDiEnumDeviceInfo(dev_info, idx, &did))
        {
            WCHAR iid[256] = {0};
            if (CM_Get_Device_IDW(did.DevInst, iid, (ULONG)(sizeof(iid)/sizeof(WCHAR)), 0) == CR_SUCCESS)
            {
                char a[256]={0};
                WideCharToMultiByte(CP_ACP, 0, iid, -1, a, (int)sizeof(a)-1, NULL, NULL);
                e.instance_id = a;
            }
        }

        result.push_back(e);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return result;
}

static void LogTopology(const std::vector<DeviceEntry>& devices, const char* label)
{
    LogEvent("=== Topology %s (%zu devices) ===", label, devices.size());
    for (size_t i = 0; i < devices.size(); i++)
    {
        const auto& d = devices[i];

        /* Only log Razer DeathAdder or interesting entries */
        if (d.vid == 0x1532 && (d.pid == 0x007C || d.pid == 0x007D))
        {
            char pid_str[16];
            sprintf_s(pid_str, "0x%04X", d.pid);
            LogEvent("  [%zu] PID %s MI=%d %s%s",
                     i, pid_str, d.iface,
                     (d.pid == 0x007C) ? "WIRED" : "WIRELESS",
                     (d.pid == 0x007D && d.iface == 0) ? " (BATTERY/RGB)" :
                     (d.pid == 0x007D && d.iface == 2) ? " (KEYBOARD)" : "");
        }
    }
}

/*---------------------------------------------------------*\
| Global state                                              |
\*---------------------------------------------------------*/
static HWND            g_hwnd        = NULL;
static bool            g_running     = true;
static bool            g_mouse_on    = false;  /* true = receiving input */
static LARGE_INTEGER   g_last_mouse  = { 0 };
static LARGE_INTEGER   g_freq        = { 0 };

static std::vector<DeviceEntry> g_last_topology;

/* User markers */
static int  g_marker_id = 0;

/*---------------------------------------------------------*\
| Window procedure                                           |
\*---------------------------------------------------------*/
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_CLOSE:
        g_running = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    /* тФАтФА WM_DEVICECHANGE тФАтФА */
    case WM_DEVICECHANGE:
    {
        DWORD evt = (DWORD)wParam;
        DEV_BROADCAST_HDR* hdr = (DEV_BROADCAST_HDR*)lParam;

        const char* evt_name = "UNKNOWN";
        switch (evt)
        {
        case DBT_DEVICEARRIVAL:         evt_name = "DBT_DEVICEARRIVAL"; break;
        case DBT_DEVICEREMOVECOMPLETE:  evt_name = "DBT_DEVICEREMOVECOMPLETE"; break;
        case DBT_DEVNODES_CHANGED:      evt_name = "DBT_DEVNODES_CHANGED"; break;
        case DBT_QUERYCHANGECONFIG:     evt_name = "DBT_QUERYCHANGECONFIG"; break;
        case DBT_CONFIGCHANGED:         evt_name = "DBT_CONFIGCHANGED"; break;
        case DBT_DEVICEQUERYREMOVE:     evt_name = "DBT_DEVICEQUERYREMOVE"; break;
        case DBT_DEVICEQUERYREMOVEFAILED: evt_name = "DBT_DEVICEQUERYREMOVEFAILED"; break;
        case DBT_DEVICEREMOVEPENDING:   evt_name = "DBT_DEVICEREMOVEPENDING"; break;
        }

        if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
        {
            DEV_BROADCAST_DEVICEINTERFACE* di = (DEV_BROADCAST_DEVICEINTERFACE*)hdr;
            char name[256] = {0};
            WideCharToMultiByte(CP_ACP, 0, (LPCWCH)di->dbcc_name, -1, name, (int)sizeof(name)-1, NULL, NULL);

            LogEvent("WM_DEVICECHANGE %s: %s", evt_name, name);

            /* Check if any DeathAdder path appears/disappears */
            if (strstr(name, "vid_1532") || strstr(name, "pid_007C") || strstr(name, "pid_007D"))
                LogEvent("  >>> RAZER DEVICE AFFECTED <<<");

            /* Snapshot topology after change */
            auto topo = SnapshotTopology();
            LogTopology(topo, "after DEVICECHANGE");
        }
        else
        {
            LogEvent("WM_DEVICECHANGE %s (no interface)", evt_name);
        }
        return 0;
    }

    /* тФАтФА WM_INPUT_DEVICE_CHANGE тФАтФА */
    case WM_INPUT_DEVICE_CHANGE:
    {
        HANDLE hDevice = (HANDLE)lParam;
        DWORD  flags   = (DWORD)wParam;

        const char* type = (flags == GIDC_ARRIVAL) ? "GIDC_ARRIVAL"
                        : (flags == GIDC_REMOVAL) ? "GIDC_REMOVAL"
                        : "UNKNOWN";

        /* Get device info */
        RID_DEVICE_INFO info = {};
        info.cbSize = sizeof(RID_DEVICE_INFO);
        UINT infoSize = sizeof(info);
        wchar_t devName[256] = {0};
        UINT nameSize = 256;

        GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, devName, &nameSize);
        GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &info, &infoSize);

        char name_a[256] = {0};
        WideCharToMultiByte(CP_ACP, 0, devName, -1, name_a, (int)sizeof(name_a)-1, NULL, NULL);

        LogEvent("WM_INPUT_DEVICE_CHANGE %s hDevice=0x%p type=%d/%s name=%s",
                 type, hDevice, info.dwType,
                 (info.dwType == RIM_TYPEMOUSE) ? "MOUSE" :
                 (info.dwType == RIM_TYPEKEYBOARD) ? "KBD" :
                 (info.dwType == RIM_TYPEHID) ? "HID" : "?",
                 name_a);

        LogEvent("  >>> RAWINPUT DEVICE %s <<<", type);

        /* Snapshot topology after RawInput device change */
        auto topo = SnapshotTopology();
        LogTopology(topo, "after RAWINPUT change");
        return 0;
    }

    /* тФАтФА WM_INPUT (Raw Input mouse packets) тФАтФА */
    case WM_INPUT:
    {
        UINT dwSize = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));

        std::vector<BYTE> buffer(dwSize);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(), &dwSize, sizeof(RAWINPUTHEADER)) == dwSize)
        {
            RAWINPUT* raw = (RAWINPUT*)buffer.data();

            if (raw->header.dwType == RIM_TYPEMOUSE)
            {
                HANDLE hDevice = raw->header.hDevice;
                USHORT flags = raw->data.mouse.usFlags;
                ULONG buttons = raw->data.mouse.ulButtons;

                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);

                double gap_ms = 0;
                if (g_last_mouse.QuadPart != 0)
                    gap_ms = ((double)(now.QuadPart - g_last_mouse.QuadPart) * 1000.0) / g_freq.QuadPart;
                g_last_mouse = now;

                if (!g_mouse_on)
                {
                    g_mouse_on = true;
                    LogEvent("WM_INPUT MOUSE FIRST PACKET (gap=%.0f ms) hDevice=0x%p buttons=0x%04lX flags=0x%04X",
                             gap_ms, hDevice, buttons, flags);
                }

                /* Log every 1000th packet or notable events */
                static DWORD pkt_count = 0;
                pkt_count++;
                if ((pkt_count % 1000) == 0 || flags & 0x0001 /* MOUSE_MOVE_RELATIVE */)
                {
                    /* Quiet тАФ only log notable events or first after gap */
                }
            }
        }
        return 0;
    }

    /* тФАтФА Hotkeys for user markers тФАтФА */
    case WM_HOTKEY:
    {
        int id = (int)wParam;
        if (id == 1) /* F8 */
        {
            LogEvent("=== USER MARKER: Turning mouse OFF (F8) ===");
            g_mouse_on = false;

            auto topo = SnapshotTopology();
            LogTopology(topo, "at OFF marker");
        }
        else if (id == 2) /* F9 */
        {
            LogEvent("=== USER MARKER: Turning mouse ON (F9) ===");

            auto topo = SnapshotTopology();
            LogTopology(topo, "at ON marker");
        }
        return 0;
    }

    /* тФАтФА System command for Windows messages тФАтФА */
    case WM_POWERBROADCAST:
    {
        LogEvent("WM_POWERBROADCAST wParam=0x%08lX", wParam);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/*---------------------------------------------------------*\
| Main                                                      |
\*---------------------------------------------------------*/
int main()
{
    OpenLog();

    QueryPerformanceFrequency(&g_freq);

    LogEvent("=== WakeEventMonitor started ===");
    LogEvent("Instructions:");
    LogEvent("  F8 = mark mouse OFF (press before turning mouse off)");
    LogEvent("  F9 = mark mouse ON (press after turning mouse on)");
    LogEvent("  Close window to quit");
    LogEvent("");

    /* Initial topology snapshot */
    g_last_topology = SnapshotTopology();
    LogTopology(g_last_topology, "initial");

    /* Register window class */
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "WakeEventMonitorClass";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, wc.lpszClassName, "WakeEventMonitor",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                600, 400, NULL, NULL, wc.hInstance, NULL);

    if (!hwnd)
    {
        LogEvent("ERROR: CreateWindow failed (0x%lx)", GetLastError());
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    g_hwnd = hwnd;

    /* тФАтФА Register for WM_DEVICECHANGE тФАтФА */
    DEV_BROADCAST_DEVICEINTERFACE filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    HDEVNOTIFY notify_dev = RegisterDeviceNotification(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    /* Also register for HID class */
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    filter.dbcc_classguid = hid_guid;
    HDEVNOTIFY notify_hid = RegisterDeviceNotification(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (notify_dev || notify_hid)
        LogEvent("Registered for WM_DEVICECHANGE (USB+HID)");
    else
        LogEvent("WARNING: RegisterDeviceNotification failed (0x%lx)", GetLastError());

    /* тФАтФА Register for Raw Input (mouse) тФАтФА */
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage     = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags     = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid.hwndTarget  = hwnd;

    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        LogEvent("Registered for Raw Input mouse + WM_INPUT_DEVICE_CHANGE (RIDEV_DEVNOTIFY)");
    else
        LogEvent("WARNING: RegisterRawInputDevices failed (0x%lx)", GetLastError());

    /* тФАтФА Hotkeys тФАтФА */
    RegisterHotKey(hwnd, 1, 0, VK_F8);  /* F8 = OFF marker */
    RegisterHotKey(hwnd, 2, 0, VK_F9);  /* F9 = ON marker */

    LogEvent("");
    LogEvent("Ready. Waiting for events...");
    LogEvent("");

    /* тФАтФА Message loop тФАтФА */
    MSG msg;
    while (g_running && GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* Cleanup */
    if (notify_dev) UnregisterDeviceNotification(notify_dev);
    if (notify_hid) UnregisterDeviceNotification(notify_hid);

    LogEvent("=== WakeEventMonitor stopped ===");

    if (g_log) fclose(g_log);
    printf("\nLog written to WakeEventMonitor.log\n");
    return 0;
}
