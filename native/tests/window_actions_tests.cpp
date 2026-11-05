#include "../window_actions.h"

#include <cstdio>

int wmain()
{
    if (mouse_lock::ShouldHandleGlobalHotkeys(true) ||
        !mouse_lock::ShouldHandleGlobalHotkeys(false))
    {
        std::fprintf(stderr,
            "FAIL: global hotkeys must be suppressed during shortcut capture\n");
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
    ShowWindow(window, SW_HIDE);
    DestroyWindow(window);

    if (!restored)
    {
        std::fprintf(stderr, "FAIL: restoring the main window must unminimize it\n");
        return 1;
    }

    std::puts("PASS: hidden-tray close behavior and main-window restoration");
    return 0;
}
