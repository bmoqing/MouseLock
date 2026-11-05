#pragma once

#include <windows.h>

namespace mouse_lock
{
    inline RECT FitWindowRectToWorkArea(const RECT& requested, const RECT& workArea)
    {
        const LONG requestedWidth = requested.right - requested.left;
        const LONG requestedHeight = requested.bottom - requested.top;
        const LONG workWidth = workArea.right - workArea.left;
        const LONG workHeight = workArea.bottom - workArea.top;
        const LONG width = requestedWidth < workWidth ? requestedWidth : workWidth;
        const LONG height = requestedHeight < workHeight ? requestedHeight : workHeight;

        RECT result{};
        result.left = workArea.left + (workWidth - width) / 2;
        result.top = workArea.top + (workHeight - height) / 2;
        result.right = result.left + width;
        result.bottom = result.top + height;
        return result;
    }

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
