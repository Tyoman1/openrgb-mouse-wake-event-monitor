/*---------------------------------------------------------*\
| WakeEventMonitor.exe                                       |
|                                                            |
|  Diagnostic utility for detecting physical wireless mouse  |
|  OFF → ON events without periodic HID polling.             |
|                                                            |
|  Monitors:                                                 |
|    - WM_DEVICECHANGE (USB/HID topology changes)            |
|    - WM_INPUT_DEVICE_CHANGE (Raw Input device arrival/     |
|      removal via RIDEV_DEVNOTIFY)                          |
|    - Raw Input mouse packets (WM_INPUT)                    |
|    - First-movement-after-gap detection                    |
|                                                            |
|  Usage:                                                    |
|    F8  — mark mouse OFF                                    |
|    F9  — mark mouse ON                                     |
|    F10 — test marker                                       |
|                                                            |
|  No periodic HID polling.  Event-driven only.              |
\*---------------------------------------------------------*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

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
        fputs("WakeEventMonitor diagnostic log\n", g_log);
        fputs("================================\n", g_log);
        fputs("Timestamp        Event\n", g_log);
        fputs("================================\n", g_log);
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
        fputs(prefix, g_log);
        va_start(args, fmt);
        vfprintf(g_log, fmt, args);
        va_end(args);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

/*---------------------------------------------------------*\
| Device topology snapshot (Unicode API)                    |
\*---------------------------------------------------------*/
struct DeviceEntry
{
    std::wstring path;
    WORD         vid;
    WORD         pid;
    int          iface;
    std::wstring instance_id;
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
        if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &di, detail,
                                               (DWORD)buf.size(), &req, NULL))
            continue;

        std::wstring path = detail->DevicePath;

        /* Parse VID, PID, MI from wide path */
        WORD vid = 0, pid = 0;
        int mi = -1;

        const wchar_t* vid_pos = wcsstr(path.c_str(), L"vid_");
        if (vid_pos) { wchar_t v[16]={0}; wcsncpy_s(v, vid_pos+4, 4); vid = (WORD)wcstol(v,NULL,16); }

        const wchar_t* pid_pos = wcsstr(path.c_str(), L"pid_");
        if (pid_pos) { wchar_t v[16]={0}; wcsncpy_s(v, pid_pos+4, 4); pid = (WORD)wcstol(v,NULL,16); }

        const wchar_t* mi_pos = wcsstr(path.c_str(), L"mi_");
        if (mi_pos) { wchar_t v[16]={0}; wcsncpy_s(v, mi_pos+3, 2); mi = (int)wcstol(v,NULL,16); }

        DeviceEntry e;
        e.path = path;
        e.vid = vid;
        e.pid = pid;
        e.iface = mi;

        /* Instance ID from device info */
        SP_DEVINFO_DATA did = { sizeof(SP_DEVINFO_DATA) };
        if (SetupDiEnumDeviceInfo(dev_info, idx, &did))
        {
            WCHAR iid[256] = {0};
            if (CM_Get_Device_IDW(did.DevInst, iid,
                                  (ULONG)(sizeof(iid)/sizeof(WCHAR)), 0) == CR_SUCCESS)
                e.instance_id = iid;
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
static bool            g_mouse_on    = false;
static LARGE_INTEGER   g_last_mouse  = { 0 };
static LARGE_INTEGER   g_freq        = { 0 };

static std::vector<DeviceEntry> g_last_topology;

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

    /* ── WM_DEVICECHANGE (explicit W-struct) ── */
    case WM_DEVICECHANGE:
    {
        DWORD evt = (DWORD)wParam;
        auto* hdr = (DEV_BROADCAST_HDR*)lParam;

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
            /* Use explicit W-struct — never cast dbcc_name from char[] */
            auto* di = (DEV_BROADCAST_DEVICEINTERFACE_W*)hdr;

            LogEvent("WM_DEVICECHANGE %s: %ws", evt_name, di->dbcc_name);

            if (wcsstr(di->dbcc_name, L"vid_1532") ||
                wcsstr(di->dbcc_name, L"pid_007C") ||
                wcsstr(di->dbcc_name, L"pid_007D"))
                LogEvent("  >>> RAZER DEVICE AFFECTED <<<");

            auto topo = SnapshotTopology();
            LogTopology(topo, "after DEVICECHANGE");
        }
        else
        {
            LogEvent("WM_DEVICECHANGE %s (no interface)", evt_name);
        }
        return 0;
    }

    /* ── WM_INPUT_DEVICE_CHANGE ── */
    case WM_INPUT_DEVICE_CHANGE:
    {
        HANDLE hDevice = (HANDLE)lParam;
        DWORD  flags   = (DWORD)wParam;

        const char* type = (flags == GIDC_ARRIVAL) ? "GIDC_ARRIVAL"
                        : (flags == GIDC_REMOVAL) ? "GIDC_REMOVAL"
                        : "UNKNOWN";

        RID_DEVICE_INFO info = {};
        info.cbSize = sizeof(RID_DEVICE_INFO);
        UINT infoSize = sizeof(info);

        wchar_t devName[256] = {0};
        UINT nameSize = 256;

        GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, devName, &nameSize);
        GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &info, &infoSize);

        LogEvent("WM_INPUT_DEVICE_CHANGE %s hDevice=0x%p type=%d/%s name=%ws",
                 type, hDevice, info.dwType,
                 (info.dwType == RIM_TYPEMOUSE) ? "MOUSE" :
                 (info.dwType == RIM_TYPEKEYBOARD) ? "KBD" :
                 (info.dwType == RIM_TYPEHID) ? "HID" : "?",
                 devName);

        LogEvent("  >>> RAWINPUT DEVICE %s <<<", type);

        auto topo = SnapshotTopology();
        LogTopology(topo, "after RAWINPUT change");
        return 0;
    }

    /* ── WM_INPUT (Raw Input mouse packets) ── */
    case WM_INPUT:
    {
        UINT dwSize = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));

        std::vector<BYTE> buffer(dwSize);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(),
                            &dwSize, sizeof(RAWINPUTHEADER)) == dwSize)
        {
            auto* raw = (RAWINPUT*)buffer.data();

            if (raw->header.dwType == RIM_TYPEMOUSE)
            {
                HANDLE hDevice = raw->header.hDevice;
                USHORT flags   = raw->data.mouse.usFlags;
                ULONG  buttons = raw->data.mouse.ulButtons;

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
            }
        }
        return 0;
    }

    /* ── Hotkeys ── */
    case WM_HOTKEY:
    {
        int id = (int)wParam;
        if (id == 1) /* F8 = mouse OFF */
        {
            LogEvent("=== USER MARKER: MOUSE OFF (F8) ===");
            g_mouse_on = false;
            auto topo = SnapshotTopology();
            LogTopology(topo, "at OFF marker");
        }
        else if (id == 2) /* F9 = mouse ON */
        {
            LogEvent("=== USER MARKER: MOUSE ON (F9) ===");
            auto topo = SnapshotTopology();
            LogTopology(topo, "at ON marker");
        }
        else if (id == 3) /* F10 = generic test marker */
        {
            LogEvent("=== USER MARKER: TEST MARKER (F10) ===");
        }
        return 0;
    }

    case WM_POWERBROADCAST:
        LogEvent("WM_POWERBROADCAST wParam=0x%08lX", wParam);
        return 0;

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
    LogEvent("  F8  = mark mouse OFF (press before turning mouse off)");
    LogEvent("  F9  = mark mouse ON  (press after turning mouse on)");
    LogEvent("  F10 = test marker");
    LogEvent("  Close window to quit");
    LogEvent("");

    g_last_topology = SnapshotTopology();
    LogTopology(g_last_topology, "initial");

    /* Register window class */
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = GetModuleHandleW(NULL);
    wc.lpszClassName = L"WakeEventMonitorClass";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WakeEventMonitor",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 600, 400, NULL, NULL, wc.hInstance, NULL);

    if (!hwnd)
    {
        LogEvent("ERROR: CreateWindow failed (0x%lx)", GetLastError());
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    g_hwnd = hwnd;

    /* ── Register for WM_DEVICECHANGE ── */
    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size       = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid  = { 0x4d1e55b2, 0xf16f, 0x11cf,
                                  { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    /* This is GUID_DEVINTERFACE_HID — using raw value to avoid linker dependency */

    HDEVNOTIFY notify_hid = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    /* Also register for generic USB device interface */
    DEV_BROADCAST_DEVICEINTERFACE_W usb_filter = {};
    usb_filter.dbcc_size       = sizeof(usb_filter);
    usb_filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    usb_filter.dbcc_classguid  = { 0x4d1e55b2, 0xf16f, 0x11cf,
                                   { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

    HDEVNOTIFY notify_usb = RegisterDeviceNotificationW(hwnd, &usb_filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (notify_hid || notify_usb)
        LogEvent("Registered for WM_DEVICECHANGE (USB+HID)");
    else
        LogEvent("WARNING: RegisterDeviceNotification failed (0x%lx)", GetLastError());

    /* ── Register for Raw Input (mouse only) ── */
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage     = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags     = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    rid.hwndTarget  = hwnd;

    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        LogEvent("Registered for Raw Input mouse + WM_INPUT_DEVICE_CHANGE (RIDEV_DEVNOTIFY)");
    else
        LogEvent("WARNING: RegisterRawInputDevices failed (0x%lx)", GetLastError());

    /* ── Hotkeys ── */
    RegisterHotKey(hwnd, 1, 0, VK_F8);
    RegisterHotKey(hwnd, 2, 0, VK_F9);
    RegisterHotKey(hwnd, 3, 0, VK_F10);

    LogEvent("");
    LogEvent("Ready. Waiting for events...");
    LogEvent("");

    /* ── Message loop ── */
    MSG msg;
    while (g_running && GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (notify_hid) UnregisterDeviceNotification(notify_hid);
    if (notify_usb) UnregisterDeviceNotification(notify_usb);

    LogEvent("=== WakeEventMonitor stopped ===");

    if (g_log) fclose(g_log);
    printf("\nLog written to WakeEventMonitor.log\n");
    return 0;
}