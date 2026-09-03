// MouseLock Win32 原生实现 —— 入口、应用状态、键盘/事件钩子、托盘、主窗口。
#include <windows.h>
#include <shellapi.h>
#include <optional>
#include <unordered_set>
#include <vector>
#include <cwchar>

#include "config.h"
#include "locker.h"
#include "settings.h"
#include "resource.h"

// ---------------- 全局应用状态 ----------------
namespace
{
    Locker g_locker;
    AppConfig g_cfg;
    std::wstring g_cfgPath;

    bool g_enabled = true;
    bool g_unlockedByHold = false;
    bool g_ctrl = false, g_alt = false, g_shift = false;
    std::unordered_set<int> g_heldKeys;

    HHOOK g_kbdHook = nullptr;
    std::vector<HWINEVENTHOOK> g_winEventHooks;

    HWND g_hwnd = nullptr;          // 主窗口
    HICON g_icon = nullptr;
    HINSTANCE g_inst = nullptr;
    bool g_allowClose = false;
    bool g_quitting = false;

    NOTIFYICONDATAW g_nid{};
    HMENU g_trayMenu = nullptr;
    bool g_trayAdded = false;

    constexpr int kTimerBackup = 1;
    constexpr int kBackupIntervalMs = 250;

    bool IsModifierVk(UINT vk)
    {
        return vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_LMENU || vk == VK_RMENU ||
               vk == VK_LSHIFT || vk == VK_RSHIFT;
    }

    std::optional<Modifier> ModifierOf(UINT vk)
    {
        switch (vk)
        {
        case VK_LCONTROL:
        case VK_RCONTROL: return Modifier::Control;
        case VK_LMENU:
        case VK_RMENU: return Modifier::Alt;
        case VK_LSHIFT:
        case VK_RSHIFT: return Modifier::Shift;
        }
        return std::nullopt;
    }

    bool ContainsModifier(const std::vector<Modifier>& mods, Modifier m)
    {
        for (auto x : mods)
            if (x == m) return true;
        return false;
    }

    bool IsModifierDown(Modifier m)
    {
        switch (m)
        {
        case Modifier::Control: return g_ctrl;
        case Modifier::Alt: return g_alt;
        case Modifier::Shift: return g_shift;
        }
        return false;
    }

    bool IsKeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    bool IsModifierPhysicallyDown(Modifier m)
    {
        switch (m)
        {
        case Modifier::Control: return IsKeyDown(VK_LCONTROL) || IsKeyDown(VK_RCONTROL);
        case Modifier::Alt: return IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU);
        case Modifier::Shift: return IsKeyDown(VK_LSHIFT) || IsKeyDown(VK_RSHIFT);
        }
        return false;
    }

    void SetModifierState(Modifier m, bool v)
    {
        switch (m)
        {
        case Modifier::Control: g_ctrl = v; break;
        case Modifier::Alt: g_alt = v; break;
        case Modifier::Shift: g_shift = v; break;
        }
    }

    // 基于钩子跟踪状态的“逻辑按下”
    bool HotkeyDown(const Hotkey& hk)
    {
        static const Modifier order[] = { Modifier::Control, Modifier::Alt, Modifier::Shift };
        for (auto m : order)
            if (IsModifierDown(m) != ContainsModifier(hk.modifiers, m)) return false;
        return hk.key == 0 || g_heldKeys.count(hk.key) > 0;
    }

    // 基于物理键状态的“实际按下”
    bool HotkeyPhysicallyDown(const Hotkey& hk)
    {
        static const Modifier order[] = { Modifier::Control, Modifier::Alt, Modifier::Shift };
        for (auto m : order)
            if (IsModifierPhysicallyDown(m) != ContainsModifier(hk.modifiers, m)) return false;
        return hk.key == 0 || IsKeyDown(hk.key);
    }

    bool TryMatch(const Hotkey& hk, UINT vk)
    {
        if (!IsSet(hk) || hk.key != (int)vk) return false;
        static const Modifier order[] = { Modifier::Control, Modifier::Alt, Modifier::Shift };
        for (auto m : order)
            if (IsModifierDown(m) != ContainsModifier(hk.modifiers, m)) return false;
        return true;
    }

    void ApplyUnlockState(bool active)
    {
        if (active == g_unlockedByHold)
            return;
        if (active)
        {
            if (g_enabled)
            {
                g_unlockedByHold = true;
                g_locker.Unlock();
            }
        }
        else if (g_unlockedByHold)
        {
            g_unlockedByHold = false;
            if (g_enabled)
                g_locker.LockToCurrentScreen();
        }
    }

    void UpdateUnlockHold()
    {
        bool active = IsSet(g_cfg.unlock) && HotkeyDown(g_cfg.unlock);
        ApplyUnlockState(active);
    }

    void ReconcileUnlockState()
    {
        bool active = IsSet(g_cfg.unlock) && HotkeyPhysicallyDown(g_cfg.unlock);
        ApplyUnlockState(active);
    }

    void ReassertLock()
    {
        if (g_enabled && !g_unlockedByHold && !g_locker.IsLocked())
            g_locker.LockToCurrentScreen();
    }

    void SyncUi();

    void SetEnabled(bool v)
    {
        if (g_enabled == v)
            return;
        g_enabled = v;
        g_unlockedByHold = false;
        if (g_enabled)
            g_locker.LockToCurrentScreen();
        else
            g_locker.Unlock();
        SyncUi();
    }

    void SyncUi()
    {
        // 托盘菜单勾选与文本
        if (g_trayMenu)
        {
            MENUITEMINFOW mii{};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_STRING | MIIM_STATE;
            mii.fState = g_enabled ? MFS_CHECKED : MFS_UNCHECKED;
            mii.dwTypeData = (LPWSTR)(g_enabled ? L"停用锁定" : L"启用锁定");
            SetMenuItemInfoW(g_trayMenu, ID_TRAY_TOGGLE, FALSE, &mii);
        }

        // 托盘 tooltip
        lstrcpynW(g_nid.szTip, g_enabled ? L"鼠标锁定 - 已启用" : L"鼠标锁定 - 已停用",
            sizeof(g_nid.szTip) / sizeof(WCHAR));
        g_nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);

        // 主窗口复选框
        if (g_hwnd)
        {
            HWND chk = GetDlgItem(g_hwnd, IDC_CHK_ENABLED);
            if (chk)
                SendMessageW(chk, BM_SETCHECK, g_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    void ShowMain()
    {
        if (!g_hwnd) return;
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        SetActiveWindow(g_hwnd);
    }

    void HideMain() { if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE); }

    void ShowBalloon(const wchar_t* info)
    {
        g_nid.uFlags = NIF_INFO;
        lstrcpynW(g_nid.szInfoTitle, L"鼠标锁定", sizeof(g_nid.szInfoTitle) / sizeof(WCHAR));
        lstrcpynW(g_nid.szInfo, info, sizeof(g_nid.szInfo) / sizeof(WCHAR));
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags = 0;
    }

    // ---------------- 钩子 ----------------

    LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp)
    {
        if (nCode == HC_ACTION)
        {
            auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
            bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            bool isUp = (wp == WM_KEYUP || wp == WM_SYSKEYUP);

            auto mod = ModifierOf(k->vkCode);
            if (mod.has_value())
                SetModifierState(*mod, isDown);
            else if (isDown)
                g_heldKeys.insert((int)k->vkCode);
            else
                g_heldKeys.erase((int)k->vkCode);

            UpdateUnlockHold();

            if (isDown && g_enabled)
            {
                if (TryMatch(g_cfg.jumpLeft, k->vkCode))
                {
                    g_locker.Jump(-1);
                    return 1; // 吞掉按键
                }
                if (TryMatch(g_cfg.jumpRight, k->vkCode))
                {
                    g_locker.Jump(1);
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, nCode, wp, lp);
    }

    void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
    {
        // 焦点/窗口切换可能被系统清除 ClipCursor，此处重新应用
        if (g_enabled && g_locker.IsLocked() && !g_unlockedByHold)
            g_locker.LockToCurrentScreen();
    }

    void InstallHooks()
    {
        g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);

        DWORD events[] = {
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_DESKTOPSWITCH,
            EVENT_SYSTEM_MOVESIZEEND,
            EVENT_SYSTEM_CAPTURESTART
        };
        for (DWORD evt : events)
        {
            HWINEVENTHOOK h = SetWinEventHook(evt, evt, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            if (h)
                g_winEventHooks.push_back(h);
        }
    }

    void UninstallHooks()
    {
        if (g_kbdHook) { UnhookWindowsHookEx(g_kbdHook); g_kbdHook = nullptr; }
        for (auto h : g_winEventHooks) UnhookWinEvent(h);
        g_winEventHooks.clear();
    }

    // ---------------- 开机自启（计划任务） ----------------

    void RunSchTasks(const std::wstring& args)
    {
        std::wstring cmd = L"schtasks.exe " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    void RemoveLegacyRunKey()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            RegDeleteValueW(key, L"MouseLock");
            RegCloseKey(key);
        }
    }

    std::wstring ExecutablePath()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return buf;
    }

    void AutoStart_SetEnabled(bool enabled)
    {
        RemoveLegacyRunKey();
        if (enabled)
        {
            std::wstring path = ExecutablePath();
            std::wstring tr = L"\"" + path + L"\"";
            std::wstring args = L"/Create /TN \"MouseLock\" /TR \"" + tr + L"\" /SC ONLOGON /RL HIGHEST /IT /F";
            RunSchTasks(args);
        }
        else
        {
            RunSchTasks(L"/Delete /TN \"MouseLock\" /F");
        }
    }

    void OpenSettings()
    {
        bool prevAutoStart = g_cfg.autoStart;
        ShowSettingsDialog(g_hwnd, g_cfg, [&](const AppConfig& cfg) {
            g_cfg = cfg;
            SaveConfig(g_cfgPath, cfg);
            if (cfg.autoStart != prevAutoStart)
            {
                AutoStart_SetEnabled(cfg.autoStart);
                prevAutoStart = cfg.autoStart;
            }
        });
    }

    // ---------------- 主窗口 ----------------

    void CreateMainControls(HWND hwnd)
    {
        HFONT bigFont = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

        HWND lbl = CreateWindowExW(0, L"STATIC", L"鼠标锁定工具",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 16, 300, 24, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)bigFont, TRUE);

        HWND chk = CreateWindowExW(0, L"BUTTON", L"启用鼠标锁定",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            20, 52, 300, 20, hwnd, (HMENU)(INT_PTR)IDC_CHK_ENABLED, g_inst, nullptr);
        SendMessageW(chk, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageW(chk, BM_SETCHECK, g_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND btnSettings = CreateWindowExW(0, L"BUTTON", L"设置...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            20, 96, 150, 32, hwnd, (HMENU)(INT_PTR)IDC_BTN_SETTINGS, g_inst, nullptr);
        SendMessageW(btnSettings, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        HWND btnHide = CreateWindowExW(0, L"BUTTON", L"隐藏到托盘",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            188, 96, 150, 32, hwnd, (HMENU)(INT_PTR)IDC_BTN_HIDE, g_inst, nullptr);
        SendMessageW(btnHide, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    }

    void ShowTrayMenu()
    {
        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(g_hwnd);
        TrackPopupMenu(g_trayMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
            pt.x, pt.y, 0, g_hwnd, nullptr);
        PostMessageW(g_hwnd, WM_NULL, 0, 0);
    }

    HMENU BuildTrayMenu()
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE, L"停用锁定");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_JUMPPREV, L"跳转到上一屏幕");
        AppendMenuW(menu, MF_STRING, ID_TRAY_JUMPNEXT, L"跳转到下一屏幕");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"设置...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出");
        return menu;
    }

    void HandleTrayCommand(int id)
    {
        switch (id)
        {
        case ID_TRAY_TOGGLE: SetEnabled(!g_enabled); break;
        case ID_TRAY_JUMPPREV: if (g_enabled) g_locker.Jump(-1); break;
        case ID_TRAY_JUMPNEXT: if (g_enabled) g_locker.Jump(1); break;
        case ID_TRAY_SETTINGS: OpenSettings(); break;
        case ID_TRAY_EXIT:
            g_allowClose = true;
            DestroyWindow(g_hwnd);
            break;
        }
    }

    void ExitApplication()
    {
        UninstallHooks();
        g_locker.Unlock();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
        if (g_trayMenu) { DestroyMenu(g_trayMenu); g_trayMenu = nullptr; }
        PostQuitMessage(0);
    }

    LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
            CreateMainControls(hwnd);
            return 0;

        case WM_COMMAND:
        {
            int id = LOWORD(wp);
            switch (id)
            {
            case IDC_CHK_ENABLED:
            {
                HWND chk = (HWND)lp;
                bool on = SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SetEnabled(on);
                break;
            }
            case IDC_BTN_SETTINGS: OpenSettings(); break;
            case IDC_BTN_HIDE: HideMain(); break;
            case ID_TRAY_TOGGLE:
            case ID_TRAY_JUMPPREV:
            case ID_TRAY_JUMPNEXT:
            case ID_TRAY_SETTINGS:
            case ID_TRAY_EXIT:
                HandleTrayCommand(id);
                break;
            }
            return 0;
        }

        case WM_TRAYICON:
        {
            if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
                ShowTrayMenu();
            else if (lp == WM_LBUTTONDBLCLK)
                ShowMain();
            return 0;
        }

        case WM_TIMER:
        {
            if (wp == kTimerBackup)
            {
                g_locker.Revalidate();
                ReconcileUnlockState();
                ReassertLock();
                // 登录/开机时任务栏可能尚未就绪，NIM_ADD 会静默失败，这里持续重试
                if (!g_trayAdded)
                {
                    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
                }
            }
            return 0;
        }

        case WM_CLOSE:
            if (!g_allowClose)
            {
                HideMain();
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wp;
            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_DESTROY:
            ExitApplication();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    const wchar_t* kMainClass = L"MouseLockMain";
}

// ---------------- 入口 ----------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    g_inst = hInstance;

    // 单实例检测
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\MouseLock_SingleInstance_1A3B");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"已经有一个 MouseLock 程序在运行。", L"MouseLock",
            MB_OK | MB_ICONINFORMATION);
        CloseHandle(hMutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 加载配置
    {
        wchar_t dir[MAX_PATH]{};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        std::wstring p = dir;
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            p = p.substr(0, slash + 1);
        g_cfgPath = p + L"config.json";
        g_cfg = LoadConfig(g_cfgPath);
    }

    g_icon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_MOUSELOCK),
        IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);

    // 托盘菜单
    g_trayMenu = BuildTrayMenu();

    // 注册主窗口类
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = g_icon;
    wc.hIconSm = g_icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    RECT rc{ 0, 0, 360, 150 };
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    g_hwnd = CreateWindowExW(0, kMainClass, L"鼠标锁定 (MouseLock)", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    // 托盘图标
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, L"鼠标锁定", sizeof(g_nid.szTip) / sizeof(WCHAR));
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;

    InstallHooks();
    SetTimer(g_hwnd, kTimerBackup, kBackupIntervalMs, nullptr);
    SyncUi();

    if (g_enabled)
        g_locker.LockToCurrentScreen();

    if (g_cfg.startSilent)
        ShowBalloon(L"已在后台运行。");
    else
        ShowMain();

    // 消息循环
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    CloseHandle(hMutex);
    return 0;
}