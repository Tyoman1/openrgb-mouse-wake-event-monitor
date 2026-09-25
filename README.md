# OpenRGB Mouse Wake Event Monitor

Experimental Windows diagnostic utility for detecting wireless mouse **OFF → ON** events **without periodic HID polling**.

This repository contains an experimental diagnostic utility. It is **not** the production OpenRGB Wake Plugin.

## Investigated Problem

When a Razer DeathAdder V2 Pro is physically turned off and on (via the hardware switch on the mouse body), the OpenRGB Wake Plugin cannot detect this event. The mouse RGB stays on its default Razer rainbow cycle instead of restoring the configured OpenRGB profile.

This tool monitors Windows system events to answer: **can physical OFF/ON be detected without polling the mouse?**

## How it works

`WakeEventMonitor.exe` opens a hidden window and registers for:

- **WM_INPUT_DEVICE_CHANGE** — Raw Input device arrival/removal (GIDC_ARRIVAL / GIDC_REMOVAL)
- **WM_INPUT** — Raw Input mouse packets (first movement after gap detection)
- **WM_DEVICECHANGE** — USB/HID device topology changes
- **SetupAPI** — one-time HID topology snapshots on relevant events

All events are logged with millisecond timestamps.

**The tool does NOT perform any periodic HID polling.**

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Requires: Windows, MSVC 2022, CMake.

## Usage

```
WakeEventMonitor.exe
```

1. A console window opens showing live event log.
2. Press **F8** just before physically turning the mouse OFF.
3. Turn the mouse OFF.
4. Wait 1–30 seconds.
5. Turn the mouse ON.
6. Press **F9** just after turning the mouse ON.
7. Move the mouse.
8. Review the log in the console or in `WakeEventMonitor.log`.

### Hotkeys

| Key | Action |
|---|---|
| **F8**  | Mark: mouse physical OFF |
| **F9**  | Mark: mouse physical ON |
| **F10** | General test marker |
| Close window to quit |

### Test scenarios

| Test | Procedure |
|---|---|
| **A** | OFF → 1s → ON → move |
| **B** | OFF → 5s → ON → move |
| **C** | OFF → 30s → ON → move |
| **D** | No OFF, 5s idle → move (control) |
| **E** | Unplug dongle → 2s → plug dongle (USB control) |

No administrator rights required (may limit WM_DEVICECHANGE events).

## Logged events

| Prefix | Description |
|---|---|
| `WM_INPUT_DEVICE_CHANGE GIDC_ARRIVAL` | Raw Input device appeared |
| `WM_INPUT_DEVICE_CHANGE GIDC_REMOVAL` | Raw Input device disappeared |
| `WM_INPUT MOUSE FIRST PACKET` | First mouse movement after a gap |
| `WM_DEVICECHANGE DBT_DEVICEARRIVAL` | USB/HID device connected |
| `WM_DEVICECHANGE DBT_DEVICEREMOVECOMPLETE` | USB/HID device disconnected |
| `USER MARKER` | F8/F9/F10 pressed |

## Repository

**This is a diagnostic project, not a production plugin.**  
Production OpenRGB Wake Plugin: https://github.com/Tyoman1/openrgb-wake-plugin-hardened