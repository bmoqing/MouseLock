#include "../window_actions.h"
#include "../settings.h"

#include <cstdio>

int wmain()
{
    if (!IsSettingsCheckboxControlId(IDC_CHK_ENABLED) ||
        !IsSettingsCheckboxControlId(IDC_CHK_CENTER) ||
        !IsSettingsCheckboxControlId(IDC_CHK_SHOW_HIDDEN) ||
        IsSettingsCheckboxControlId(IDC_BTN_REFRESH_BINDINGS))
    {
        std::fprintf(stderr,
            "FAIL: only settings checkboxes should use the main-window background\n");
        return 1;
    }

    if (mouse_lock::ShouldHandleGlobalHotkeys(true) ||
        !mouse_lock::ShouldHandleGlobalHotkeys(false))
    {
        std::fprintf(stderr,
            "FAIL: global hotkeys must be suppressed during shortcut capture\n");
        return 1;
    }

    RECT requestedWindow{ 0, 0, 800, 832 };
    RECT workArea{ 100, 50, 900, 650 };
    RECT fittedWindow = mouse_lock::FitWindowRectToWorkArea(requestedWindow, workArea);
    if (fittedWindow.left != 100 || fittedWindow.top != 50 ||
        fittedWindow.right != 900 || fittedWindow.bottom != 650)
    {
        std::fprintf(stderr,
            "FAIL: main window must fit within a smaller work area\n");
        return 1;
    }

    requestedWindow = RECT{ 0, 0, 400, 200 };
    fittedWindow = mouse_lock::FitWindowRectToWorkArea(requestedWindow, workArea);
    if (fittedWindow.left != 300 || fittedWindow.top != 250 ||
        fittedWindow.right != 700 || fittedWindow.bottom != 450)
    {
        std::fprintf(stderr,
            "FAIL: main window must be centered within a larger work area\n");
        return 1;
    }

    if (!mouse_lock::ShouldHideMainWindowOnClose(false) ||
        mouse_lock::ShouldHideMainWindowOnClose(true))
    {
        std::fprintf(stderr, "FAIL: close behavior must depend on tray visibility\n");
        return 1;
    }

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
        L"MouseLock Window Actions Test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 240, 120, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!window)
    {
        std::fprintf(stderr, "FAIL: could not create a test window\n");
        return 1;
    }

    ShowWindow(window, SW_SHOWMINIMIZED);
    if (!IsIconic(window))
    {
        ShowWindow(window, SW_HIDE);
        DestroyWindow(window);
        std::fprintf(stderr, "FAIL: test window did not minimize\n");
        return 1;
    }

    mouse_lock::RestoreMainWindow(window);
    bool restored = IsWindowVisible(window) && !IsIconic(window);
    ShowWindow(window, SW_MINIMIZE);
    mouse_lock::RestoreMainWindow(window);
    mouse_lock::RestoreMainWindow(window);
    bool restoredAfterRepeatedLaunch =
        IsWindowVisible(window) && !IsIconic(window);
    ShowWindow(window, SW_HIDE);
    DestroyWindow(window);

    if (!restored || !restoredAfterRepeatedLaunch)
    {
        std::fprintf(stderr,
            "FAIL: repeated main-window restoration must unminimize the window\n");
        return 1;
    }

    std::puts("PASS: hidden-tray close behavior and main-window restoration");
    return 0;
}
