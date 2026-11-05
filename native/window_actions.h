#pragma once

#include <windows.h>

namespace mouse_lock
{
    inline bool ShouldHandleGlobalHotkeys(bool hotkeyCaptureActive)
    {
        return !hotkeyCaptureActive;
    }

    inline bool ShouldHideMainWindowOnClose(bool trayIconHidden)
    {
        return !trayIconHidden;
    }

    inline void RestoreMainWindow(HWND hwnd)
    {
        if (!hwnd)
            return;

        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
    }
}
