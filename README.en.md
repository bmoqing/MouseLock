<p align="center">
  <a href="README.en.md">🇺🇸 English</a> |
  <a href="README.md">🇨🇳 中文</a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/bmoqing/MouseLock/refs/heads/main/docs/MouseLock.ico" alt="MouseLock" width="128" height="128">
</p>

<h1 align="center">MouseLock</h1>

<p align="center">
  <strong>A mouse locking tool designed for multi-monitor users. It keeps the mouse within the current display to prevent accidental movement to another screen during games, presentations, or precision drawing.</strong>
</p>

## v2.1.1

- Fixed an issue where the cursor could occasionally cross into a secondary monitor during fast, large mouse movements in some games.
- Added low-level mouse hook tracking for the latest cursor position, with immediate recovery when the cursor leaves the locked area or another application changes the clipping region.
- Kept the 25 ms backup validation to improve stability during games, fullscreen transitions, and temporary mouse control by other applications.

## v2.1.0

- Added optional center locking and foreground application binding. Bindings use executable paths, apply to all windows of the application, and can find background processes without visible windows.
- Added mouse button 4 and 5 support for keyboard shortcuts. They can be assigned to screen switching or temporary unlock.
- Fixed an issue where the cursor could remain trapped by the old monitor boundary while dragging a bound application window between monitors. The old restriction is released during the move and reapplied on the destination monitor.
- Improved lock state synchronization and monitor switching to reduce normal clicks being interpreted as drags in Adobe Premiere Pro, Photoshop, and similar applications.
- Fixed garbled Chinese characters at the end of UTF-8 log lines and added startup, hook, lock, and scheduled task diagnostics.
- Moved configuration and logs to `%LOCALAPPDATA%\MouseLock` by default. Existing configuration is migrated on first launch.
- Made the startup task user-specific and added startup validation and repair.

## Features

- **Smart locking**: Keeps the mouse cursor inside the current monitor.
- **Center locking**: Optionally locks the cursor to the geometric center of the current monitor, calculated from each monitor's actual resolution.
- **Foreground application binding**: Select applications from a unified list. The list is deduplicated by executable path; when a bound application is in the foreground, the cursor is locked to the monitor containing that application.
- **Temporary unlock**: Hold a configured hotkey to unlock temporarily. The lock is restored when the hotkey is released.
- **Quick switching**: Jump to the previous or next monitor with one shortcut. The lock moves to the destination monitor, and keyboard shortcuts support mouse buttons 4 and 5.
- **Cross-monitor window dragging**: When a bound window is moved to another monitor, the lock area switches automatically instead of keeping the old monitor boundary.
- **Resilient locking**: Uses system event hooks (`SetWinEventHook`) and periodic validation to restore the lock if another application removes it.
- **Low resource usage**: Native Win32 implementation with no .NET or CLR dependency.
- **Broad compatibility**: Built on Win32 APIs (`ClipCursor`, `SetWinEventHook`, and low-level keyboard and mouse hooks), supporting arbitrary monitor arrangements.

## Default Hotkeys

| Function | Default hotkey |
| --- | --- |
| Temporary unlock (hold) | `Ctrl` |
| Jump to previous monitor | `Ctrl + Alt + Left` |
| Jump to next monitor | `Ctrl + Alt + Right` |

All three hotkeys can be customized in Settings. The input fields support keyboard keys, mouse buttons 4 and 5, and modifier combinations. Use the buttons on the right to restore defaults or clear a shortcut. A shortcut can be left empty to disable it. Changes are saved and applied immediately.

## Usage

1. Download and run `MouseLock.exe`. The application requests administrator privileges through its UAC manifest.
2. Check **Enable mouse locking** in the main window.
3. Closing the main window hides it in the system tray; it does not exit the application.
4. Right-click the tray icon to enable or disable locking, switch monitors, open binding settings, open general settings, or exit.
5. Configure the following options in Settings:
   - The three hotkeys.
   - Lock the mouse to the center of the current monitor.
   - Lock only when a selected foreground application is active. Select multiple applications from the binding list. Window and process bindings use the application path as a single identity, so all windows of the application are covered.
   - Search the application list. By default, only applications with visible windows are shown; enable **Show background applications without visible windows** to find background processes, then use **Bind selected items** to apply the selection.
   - Start silently in the system tray.
   - Start automatically with Windows using a scheduled task that runs at logon with elevated privileges.

Configuration and logs are stored in `%LOCALAPPDATA%\MouseLock` by default, separate from the executable directory. An existing `config.json` beside the executable is migrated on first launch. If the user data directory is unavailable, the application falls back to the executable directory; if the log cannot be written there, it also tries a temporary directory.

## Build

Requirements: Windows 10/11 and Visual Studio Build Tools with the C++ toolchain, Windows SDK, and resource compiler (`rc.exe`).

```bat
cd native
build_vs18.bat
```

The generated `native\MouseLock.exe` is a standalone executable. The application manifest requests administrator privileges at startup.

## Technical Details

- **Mouse locking**: `ClipCursor` limits the cursor area. `SetWinEventHook` observes focus and desktop changes, while a timer restores the clipping region when needed.
- **Center locking**: Uses the center of the current monitor and restricts the cursor to a 1 x 1 pixel area. The center is recalculated when switching monitors.
- **Foreground binding**: Uses `EVENT_SYSTEM_FOREGROUND` to track the foreground window, matches selected applications by executable path, and uses `MonitorFromWindow` to identify the monitor. The old clipping region is released when a bound window starts moving and reapplied when the move ends.
- **Hotkeys**: Low-level keyboard and mouse hooks (`WH_KEYBOARD_LL` and `WH_MOUSE_LL`) capture keyboard and mouse button 4/5 combinations. Temporary unlock uses a hold-to-unlock behavior.
- **Monitor switching**: `EnumDisplayMonitors` enumerates monitors and sorts them by physical coordinates. `SetCursorPos` moves the cursor to the center of the destination monitor.
- **Configuration**: Uses a lightweight JSON reader and writer. Changes are saved immediately.
- **Single instance**: A named mutex prevents multiple instances from running at the same time.
- **Automatic startup**: Uses a per-user Windows scheduled task with a logon trigger, highest available privileges, and an interactive token. The task is validated and repaired at startup when needed.
- **Diagnostics**: UTF-8 logs record startup environment, hook installation, lock state, and scheduled task operations. Logs are stored at `%LOCALAPPDATA%\MouseLock\MouseLock.log` by default.

## Project Structure

```text
MouseLock/
└── native/               # C++/Win32 source
    ├── main.cpp          # Entry point, tray icon, hooks, main window
    ├── locker.cpp/h      # Core mouse locking logic
    ├── settings.cpp/h    # Settings window
    ├── config.cpp/h      # JSON configuration and hotkey formatting
    ├── resources.rc      # Resource script (icon and application manifest)
    ├── resource.h        # Resource IDs
    ├── app.manifest      # Application manifest (administrator privileges)
    ├── MouseLock.ico/png # Icon resources
    └── build_vs18.bat    # One-click Visual Studio Build Tools script
```

## License

MIT License
