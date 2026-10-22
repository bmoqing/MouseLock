// MouseLock Win32 原生实现 —— 入口、应用状态、键盘/事件钩子、托盘、主窗口。
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <atomic>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <string>
#include <iterator>

#include "config.h"
#include "locker.h"
#include "settings.h"
#include "resource.h"
#include "logger.h"
#include "handle_guard.h"
#include "constants.h"
#include "error_helpers.h"

// ---------------- 全局应用状态 ----------------
namespace
{
    // ========== 核心组件 ==========
    Locker g_locker;                    // 鼠标锁定器，控制 ClipCursor
    AppConfig g_cfg;                    // 用户配置（持久化到 config.json）
    std::wstring g_cfgPath;             // 配置文件完整路径

    // ========== 锁定状态 ==========
    bool g_enabled = true;              // 主开关：是否启用鼠标锁定
    bool g_unlockedByHold = false;      // 是否因长按解锁键而临时解锁
    bool g_bindingMatched = false;      // 前台窗口是否匹配用户绑定

    // ========== 键盘状态（由钩子维护） ==========
    bool g_lctrl = false, g_rctrl = false;
    bool g_lalt = false, g_ralt = false;
    bool g_lshift = false, g_rshift = false;              // 左右修饰键分别维护
    std::unordered_set<int> g_heldKeyboardKeys;           // 当前按下的键（含修饰键）
    std::unordered_set<int> g_heldMouseButtons;            // 当前按下的鼠标侧键

    // ========== 系统钩子 ==========
    HHOOK g_kbdHook = nullptr;          // 低级键盘钩子（捕获快捷键）
    HHOOK g_mouseHook = nullptr;         // 低级鼠标钩子（捕获鼠标侧键快捷键）
    std::unordered_set<int> g_swallowedKeyboardKeys; // 已吞掉按下事件的键
    std::unordered_set<int> g_swallowedMouseButtons; // 已吞掉按下事件的鼠标键
    std::atomic_bool g_unlockSyncPosted{ false };
    std::vector<HWINEVENTHOOK> g_winEventHooks;  // 窗口事件钩子（跟踪前台窗口）

    // ========== UI 资源 ==========
    HWND g_hwnd = nullptr;              // 主窗口句柄
    HICON g_icon = nullptr;             // 应用图标
    HFONT g_mainFont = nullptr;         // 主窗口大标题字体
    HINSTANCE g_inst = nullptr;         // 应用实例句柄
    bool g_allowClose = false;          // 是否允许关闭主窗口（防止误关）
    bool g_quitting = false;            // 是否正在退出程序
    bool g_settingsOpen = false;        // 设置窗口打开期间暂停备用校验

    // ========== 窗口/进程绑定 ==========
    std::unordered_set<HWND> g_boundWindows;    // 临时绑定的窗口（当前会话）
    std::unordered_set<DWORD> g_boundProcesses; // 临时绑定的进程（当前会话）
    // 持久化绑定统一保存在应用程序路径；旧进程路径字段仅用于兼容读取。

    // ========== 前台窗口跟踪（防抖动） ==========
    // 说明：窗口切换时不立即响应，而是连续采样 3 次确保稳定
    HWND g_lastForegroundWindow = nullptr;      // 上次确认的稳定前台窗口
    HWND g_pendingForegroundWindow = nullptr;   // 待确认的前台窗口
    int g_pendingForegroundSamples = 0;         // 连续采样计数（需达到 kForegroundStableSamples）
    bool g_foregroundRefreshRequested = false;  // 是否有刷新请求（由事件钩子设置）
    bool g_foregroundTransitionPending = false; // 是否正在窗口切换过程中
    bool g_moveSizeInProgress = false;          // 窗口正在被用户拖动或调整大小
    HWND g_moveSizeWindow = nullptr;            // 当前正在移动/调整大小的窗口
    // 注：这部分逻辑较复杂，可考虑未来封装为 ForegroundTracker 类

    // ========== 系统托盘 ==========
    NOTIFYICONDATAW g_nid{};            // 托盘图标数据结构
    HMENU g_trayMenu = nullptr;         // 托盘右键菜单
    bool g_trayAdded = false;           // 托盘图标是否已成功添加
    UINT g_taskbarCreatedMessage = 0;   // Explorer 重启后通知托盘图标重新注册

    constexpr int kTimerBackup = 1;
    constexpr int kBackupIntervalMs = 25;   // 25ms，提升前台切换和状态同步响应速度
    constexpr UINT kMsgJump = WM_APP + 2;
    constexpr UINT kMsgUnlockSync = WM_APP + 3;

    void RequestUnlockSync()
    {
        if (!g_hwnd)
            return;

        bool expected = false;
        if (!g_unlockSyncPosted.compare_exchange_strong(expected, true))
            return;

        if (!PostMessageW(g_hwnd, kMsgUnlockSync, 0, 0))
            g_unlockSyncPosted.store(false);
    }

    bool IsModifierVk(UINT vk)
    {
        return vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_LMENU || vk == VK_RMENU ||
               vk == VK_LSHIFT || vk == VK_RSHIFT;
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
        case Modifier::Control: return g_lctrl || g_rctrl;
        case Modifier::Alt: return g_lalt || g_ralt;
        case Modifier::Shift: return g_lshift || g_rshift;
        }
        return false;
    }

    bool IsKeyDown(int vk) { return (GetAsyncKeyState(vk) & kAsyncKeyStatePressedMask) != 0; }

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

    void SetModifierState(UINT vk, bool v)
    {
        switch (vk)
        {
        case VK_LCONTROL: g_lctrl = v; break;
        case VK_RCONTROL: g_rctrl = v; break;
        case VK_LMENU: g_lalt = v; break;
        case VK_RMENU: g_ralt = v; break;
        case VK_LSHIFT: g_lshift = v; break;
        case VK_RSHIFT: g_rshift = v; break;
        }
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

    void ApplyForegroundBinding(HWND hwnd);
    void RequestForegroundRefresh();

    void ApplyUnlockState(bool active)
    {
        if (active == g_unlockedByHold)
            return;
        if (active)
        {
            if (g_enabled)
            {
                if (g_locker.Unlock())
                {
                    g_unlockedByHold = true;
                    LOG_INFO(L"快捷键: 临时解锁（长按中）");
                }
                else
                {
                    LOG_WARN(L"快捷键: 临时解锁失败，保持当前锁定状态");
                }
            }
        }
        else if (g_unlockedByHold)
        {
            g_unlockedByHold = false;
            LOG_INFO(L"快捷键: 恢复锁定（释放长按）");
            if (g_enabled)
            {
                if (g_cfg.foregroundBinding)
                    RequestForegroundRefresh();
                else
                    g_locker.LockToCurrentScreen(g_cfg.centerLock);
            }
        }
    }

    void ReconcileUnlockState()
    {
        bool active = IsSet(g_cfg.unlock) && HotkeyPhysicallyDown(g_cfg.unlock);
        ApplyUnlockState(active);
    }

    bool IsBoundForeground(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;
        wchar_t className[kMaxWindowClassNameLength]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        // 排除桌面和任务栏
        if (lstrcmpW(className, L"Progman") == 0 ||
            lstrcmpW(className, L"WorkerW") == 0 ||
            lstrcmpW(className, L"Shell_TrayWnd") == 0 ||
            lstrcmpW(className, L"Shell_SecondaryTrayWnd") == 0)
            return false;

        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (!processId)
            return false;

        // 使用 RAII 管理进程句柄，确保所有路径都释放
        unique_process_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
        if (process)
        {
            wchar_t imagePath[kMaxWindowsPathLength]{};
            DWORD length = static_cast<DWORD>(std::size(imagePath));
            bool pathMatched = false;

            if (QueryFullProcessImageNameW(process.get(), 0, imagePath, &length))
            {
                auto pathMatches = [&](const std::vector<std::wstring>& paths) {
                    return std::any_of(paths.begin(), paths.end(), [&](const std::wstring& path) {
                        return CompareStringOrdinal(path.c_str(), -1, imagePath, -1, TRUE) == CSTR_EQUAL;
                    });
                };
                // 窗口绑定和进程绑定效果相同：该进程的任何窗口都匹配
                pathMatched = pathMatches(g_cfg.boundWindowPaths) || pathMatches(g_cfg.boundProcessPaths);
            }

            if (pathMatched)
                return true;
        }

        // 临时绑定：检查进程ID
        if (g_boundProcesses.count(processId) != 0)
            return true;

        // 窗口绑定：按进程ID匹配（同一进程的所有窗口都匹配）
        for (HWND boundWindow : g_boundWindows)
        {
            DWORD boundProcessId = 0;
            if (boundWindow && IsWindow(boundWindow))
                GetWindowThreadProcessId(boundWindow, &boundProcessId);
            if (boundProcessId == processId)
                return true;
        }
        return false;
    }

    void ApplyForegroundBinding(HWND hwnd)
    {
        if (!g_enabled || g_unlockedByHold)
            return;
        if (!g_cfg.foregroundBinding)
        {
            g_bindingMatched = false;
            g_locker.LockToCurrentScreen(g_cfg.centerLock);
            return;
        }
        if (g_cfg.boundWindowPaths.empty() && g_cfg.boundProcessPaths.empty() &&
            g_boundWindows.empty() && g_boundProcesses.empty())
        {
            g_bindingMatched = false;
            g_locker.Unlock();
            return;
        }
        // 仅允许把当前稳定采样到的真实前台窗口用于绑定。
        // 设置窗口、窗口销毁和焦点切换期间，旧句柄不能再次触发副屏锁定。
        HWND actualForeground = GetForegroundWindow();
        if (actualForeground != hwnd)
        {
            g_bindingMatched = false;
            g_foregroundRefreshRequested = true;
            g_foregroundTransitionPending = true;
            g_locker.Unlock();
            return;
        }

        bool matched = IsBoundForeground(hwnd);
        if (matched != g_bindingMatched)
        {
            // 获取窗口信息用于日志
            wchar_t title[kMaxWindowTitleLength] = L"";
            GetWindowTextW(hwnd, title, static_cast<int>(kMaxWindowTitleLength));
            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);

            if (matched)
                LOG_INFO(L"绑定匹配: 窗口 '%s' (PID %d) - 锁定到该窗口所在屏幕", title, processId);
            else
                LOG_INFO(L"绑定取消: 窗口 '%s' (PID %d) - 解除锁定", title, processId);
        }
        g_bindingMatched = matched;

        if (!g_bindingMatched)
        {
            g_locker.Unlock();
            return;
        }

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!monitor || !CheckWin32Warn(GetMonitorInfoW(monitor, &monitorInfo), L"GetMonitorInfoW"))
        {
            g_bindingMatched = false;
            g_locker.Unlock();
            return;
        }

        // 获取当前鼠标位置和所在屏幕
        POINT cursorPos{};
        if (!CheckWin32Warn(GetCursorPos(&cursorPos), L"GetCursorPos"))
        {
            g_bindingMatched = false;
            g_locker.Unlock();
            return;
        }
        HMONITOR cursorMonitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);

        // 只有当鼠标已经在目标屏幕上时才锁定，否则保持解锁状态
        if (cursorMonitor != monitor)
        {
            g_locker.Unlock();
            return;
        }

        POINT center{
            monitorInfo.rcMonitor.left +
                (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left) / 2,
            monitorInfo.rcMonitor.top +
                (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top) / 2
        };
        if (!g_locker.LockToScreenAt(center.x, center.y, g_cfg.centerLock))
        {
            g_bindingMatched = false;
            LOG_WARN(L"锁定失败: 无法锁定到绑定窗口所在屏幕");
            g_locker.Unlock();
        }
    }

    void ReconcileForegroundWindow()
    {
        if (g_moveSizeInProgress && g_moveSizeWindow && !IsWindow(g_moveSizeWindow))
        {
            g_moveSizeInProgress = false;
            g_moveSizeWindow = nullptr;
        }

        // 绑定窗口跨屏拖动时，旧的 ClipCursor 区域会把鼠标留在原屏幕。
        // 移动期间临时解除裁剪，等 MOVESIZEEND 后再按新屏幕重新锁定。
        if (g_moveSizeInProgress && g_cfg.foregroundBinding)
        {
            g_bindingMatched = false;
            if (g_locker.IsLocked())
                g_locker.Unlock();
            return;
        }

        HWND foreground = GetForegroundWindow();
        if (foreground && !IsWindow(foreground))
            foreground = nullptr;
        if (foreground != g_pendingForegroundWindow)
        {
            g_pendingForegroundWindow = foreground;
            g_pendingForegroundSamples = 1;
            if (foreground != g_lastForegroundWindow && g_cfg.foregroundBinding)
            {
                g_foregroundTransitionPending = true;
                g_bindingMatched = false;
                g_locker.Unlock();
            }
            return;
        }
        if (g_pendingForegroundSamples < kForegroundStableSamples - 1)
        {
            ++g_pendingForegroundSamples;
            return;
        }
        bool foregroundChanged = foreground != g_lastForegroundWindow ||
            (g_lastForegroundWindow && !IsWindow(g_lastForegroundWindow));
        bool needsReapply = g_cfg.foregroundBinding && g_bindingMatched && !g_locker.IsLocked();
        if (!foregroundChanged && !g_foregroundRefreshRequested &&
            !g_foregroundTransitionPending && !needsReapply)
            return;
        g_lastForegroundWindow = foreground;
        g_foregroundRefreshRequested = false;
        g_foregroundTransitionPending = false;
        ApplyForegroundBinding(foreground);
    }

    void RequestForegroundRefresh()
    {
        g_foregroundRefreshRequested = true;
        g_foregroundTransitionPending = true;
        g_pendingForegroundWindow = nullptr;
        g_pendingForegroundSamples = 0;
        g_bindingMatched = false;
        if (g_cfg.foregroundBinding)
            g_locker.Unlock();
    }

    void ReassertLock()
    {
        // 前台绑定模式只能由 ReconcileForegroundWindow 使用稳定的当前前台窗口重锁，
        // 这里不能回退到可能已经失焦或销毁的 g_lastForegroundWindow。
        if (g_enabled && !g_unlockedByHold && !g_cfg.foregroundBinding &&
            !g_locker.IsLocked())
            g_locker.LockToCurrentScreen(g_cfg.centerLock);
    }

    void SyncUi();

    void SetEnabled(bool v)
    {
        if (g_enabled == v)
            return;
        g_enabled = v;
        g_unlockedByHold = false;
        LOG_INFO(L"锁定状态: %s", v ? L"启用" : L"禁用");
        if (g_enabled)
        {
            if (g_cfg.foregroundBinding)
                RequestForegroundRefresh();
            else
                g_locker.LockToCurrentScreen(g_cfg.centerLock);
        }
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

        if (g_trayMenu)
        {
            MENUITEMINFOW mii{};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_STRING | MIIM_STATE;
            mii.fState = g_cfg.foregroundBinding ? MFS_CHECKED : MFS_UNCHECKED;
            mii.dwTypeData = (LPWSTR)(g_cfg.foregroundBinding ? L"停用前台绑定" : L"启用前台绑定");
            SetMenuItemInfoW(g_trayMenu, ID_TRAY_BIND_TOGGLE, FALSE, &mii);
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

    bool TryAddTrayIcon(bool logFailure)
    {
        if (!g_hwnd || g_trayAdded)
            return g_trayAdded;

        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
        if (g_trayAdded)
            SyncUi();
        else if (logFailure)
            LOG_WARN(L"托盘图标添加失败，将稍后重试 (错误码 %d)", GetLastError());
        return g_trayAdded;
    }

    // ---------------- 钩子 ----------------

    LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp)
    {
        if (nCode == HC_ACTION)
        {
            auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
            bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            bool isUp = (wp == WM_KEYUP || wp == WM_SYSKEYUP);

            bool firstDown = false;
            if (isDown)
                firstDown = g_heldKeyboardKeys.insert((int)k->vkCode).second;
            else if (isUp)
                g_heldKeyboardKeys.erase((int)k->vkCode);

            if (IsModifierVk(k->vkCode))
                SetModifierState(k->vkCode, isDown);

            RequestUnlockSync();

            if (isDown && firstDown && g_enabled && !g_settingsOpen)
            {
                if (TryMatch(g_cfg.jumpLeft, k->vkCode))
                {
                    if (g_hwnd && PostMessageW(g_hwnd, kMsgJump, (WPARAM)-1, 0))
                    {
                        g_swallowedKeyboardKeys.insert((int)k->vkCode);
                        return 1; // 吞掉按键
                    }
                }
                if (TryMatch(g_cfg.jumpRight, k->vkCode))
                {
                    if (g_hwnd && PostMessageW(g_hwnd, kMsgJump, (WPARAM)1, 0))
                    {
                        g_swallowedKeyboardKeys.insert((int)k->vkCode);
                        return 1;
                    }
                }
            }

            if (isDown && g_swallowedKeyboardKeys.count((int)k->vkCode) != 0)
                return 1;

            if (isUp && g_swallowedKeyboardKeys.erase((int)k->vkCode) != 0)
                return 1;
        }
        return CallNextHookEx(nullptr, nCode, wp, lp);
    }

    int MouseButtonVkFromHook(WPARAM message, const MSLLHOOKSTRUCT& mouse)
    {
        if (message != WM_XBUTTONDOWN && message != WM_XBUTTONUP)
            return 0;
        switch (HIWORD(mouse.mouseData))
        {
        case XBUTTON1: return VK_XBUTTON1;
        case XBUTTON2: return VK_XBUTTON2;
        default: return 0;
        }
    }

    LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp)
    {
        if (nCode == HC_ACTION)
        {
            auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
            int button = MouseButtonVkFromHook(wp, *mouse);
            if (button != 0)
            {
                bool isDown = wp == WM_XBUTTONDOWN;
                bool isUp = wp == WM_XBUTTONUP;

                if (isDown)
                {
                    bool firstDown = g_heldMouseButtons.insert(button).second;
                    RequestUnlockSync();

                    if (firstDown && g_enabled && !g_settingsOpen)
                    {
                        int direction = 0;
                        if (TryMatch(g_cfg.jumpLeft, (UINT)button))
                            direction = -1;
                        else if (TryMatch(g_cfg.jumpRight, (UINT)button))
                            direction = 1;

                        if (direction != 0)
                        {
                            if (g_hwnd && PostMessageW(g_hwnd, kMsgJump,
                                    (WPARAM)direction, 0))
                            {
                                g_swallowedMouseButtons.insert(button);
                                return 1;
                            }
                        }
                    }
                }
                else if (isUp)
                {
                    g_heldMouseButtons.erase(button);
                    RequestUnlockSync();
                    if (g_swallowedMouseButtons.erase(button) != 0)
                        return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, nCode, wp, lp);
    }

    void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD)
    {
        if (event != EVENT_SYSTEM_FOREGROUND &&
            event != EVENT_SYSTEM_MOVESIZESTART &&
            event != EVENT_SYSTEM_MOVESIZEEND &&
            event != EVENT_SYSTEM_DESKTOPSWITCH)
            return;
        // WinEvent 回调可能发生在窗口销毁/焦点切换尚未完成的中间状态。
        // 不在回调线程里移动鼠标；移动开始时只释放旧裁剪，最终锁定交给主线程。
        if (event == EVENT_SYSTEM_MOVESIZESTART)
        {
            g_moveSizeInProgress = true;
            g_moveSizeWindow = hwnd;
            if (g_cfg.foregroundBinding && g_enabled && !g_unlockedByHold &&
                g_locker.IsLocked())
            {
                g_locker.Unlock();
            }
        }
        else if (event == EVENT_SYSTEM_MOVESIZEEND)
        {
            g_moveSizeInProgress = false;
            g_moveSizeWindow = nullptr;
        }
        g_foregroundRefreshRequested = true;
    }

    void InstallHooks()
    {
        g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
        if (!g_kbdHook)
        {
            DWORD error = GetLastError();
            LOG_ERROR(L"键盘钩子安装失败 (错误码 %d)", error);
            MessageBoxW(nullptr,
                L"无法安装键盘钩子，快捷键功能将不可用。\n\n可能原因：\n"
                L"• 杀毒软件拦截\n"
                L"• 权限不足\n"
                L"• 系统资源不足\n\n"
                L"程序将继续运行，但快捷键（临时解锁、跳屏）无法使用。",
                L"MouseLock - 警告", MB_OK | MB_ICONWARNING);
        }
        else
        {
            LOG_INFO(L"键盘钩子安装成功");
        }

        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
            GetModuleHandleW(nullptr), 0);
        if (!g_mouseHook)
        {
            LOG_WARN(L"鼠标钩子安装失败，鼠标侧键快捷键不可用 (错误码 %d)", GetLastError());
        }
        else
        {
            LOG_INFO(L"鼠标钩子安装成功");
        }


        DWORD events[] = {
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_DESKTOPSWITCH,
            EVENT_SYSTEM_MOVESIZESTART,
            EVENT_SYSTEM_MOVESIZEEND
        };
        int failedHooks = 0;
        for (DWORD evt : events)
        {
            HWINEVENTHOOK h = SetWinEventHook(evt, evt, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            if (h)
            {
                g_winEventHooks.push_back(h);
            }
            else
            {
                failedHooks++;
                LOG_WARN(L"窗口事件钩子安装失败 (事件 %d, 错误码 %d)", evt, GetLastError());
            }
        }

        if (failedHooks > 0)
        {
            LOG_WARN(L"部分窗口事件钩子安装失败 (%d/%d)", failedHooks, (int)std::size(events));
            if (failedHooks == std::size(events))
            {
                MessageBoxW(nullptr,
                    L"无法安装窗口事件钩子，前台窗口绑定功能可能不稳定。\n\n"
                    L"建议重启程序或联系开发者。",
                    L"MouseLock - 警告", MB_OK | MB_ICONWARNING);
            }
        }
        else
        {
            LOG_INFO(L"窗口事件钩子安装成功 (%d 个)", (int)std::size(events));
        }
    }

    void UninstallHooks()
    {
        if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
        if (g_kbdHook) { UnhookWindowsHookEx(g_kbdHook); g_kbdHook = nullptr; }
        g_swallowedKeyboardKeys.clear();
        g_swallowedMouseButtons.clear();
        g_heldKeyboardKeys.clear();
        g_heldMouseButtons.clear();
        g_lctrl = g_rctrl = false;
        g_lalt = g_ralt = false;
        g_lshift = g_rshift = false;
        for (auto h : g_winEventHooks) UnhookWinEvent(h);
        g_winEventHooks.clear();
    }

    // ---------------- 开机自启（计划任务） ----------------

    // 将一个参数编码为 Windows 命令行格式。这个函数会保留参数内部的引号，
    // 因此可以安全传递带空格的任务 XML 路径或可执行文件路径。
    std::wstring QuoteCommandLineArgument(const std::wstring& value)
    {
        if (value.empty())
            return L"\"\"";

        bool needsQuotes = false;
        for (wchar_t ch : value)
        {
            if (ch == L' ' || ch == L'\t' || ch == L'\"')
            {
                needsQuotes = true;
                break;
            }
        }
        if (!needsQuotes)
            return value;

        std::wstring result;
        result.reserve(value.size() + 2);
        result.push_back(L'\"');

        size_t backslashes = 0;
        for (wchar_t ch : value)
        {
            if (ch == L'\\')
            {
                ++backslashes;
                continue;
            }

            if (ch == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }

            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }

        // 结尾反斜杠必须加倍，否则会转义外层结束引号。
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    bool RunSchTasks(const std::wstring& args, DWORD* exitCodeOut = nullptr,
        bool logNonZeroExit = true)
    {
        if (exitCodeOut)
            *exitCodeOut = ERROR_INVALID_DATA;
        wchar_t systemDir[MAX_PATH]{};
        UINT systemDirLength = GetSystemDirectoryW(systemDir, static_cast<UINT>(std::size(systemDir)));
        if (systemDirLength == 0 || systemDirLength >= std::size(systemDir))
        {
            LogWin32Error(L"GetSystemDirectoryW", GetLastError());
            return false;
        }

        std::wstring schtasksPath = std::wstring(systemDir, systemDirLength) + L"\\schtasks.exe";
        std::wstring command = QuoteCommandLineArgument(schtasksPath);
        if (!args.empty())
        {
            command.push_back(L' ');
            command += args;
        }
        std::vector<wchar_t> commandLine(command.begin(), command.end());
        commandLine.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(schtasksPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            LogWin32Error(L"CreateProcessW(schtasks.exe)", GetLastError());
            return false;
        }

        bool success = false;
        DWORD waitResult = WaitForSingleObject(pi.hProcess, kProcessWaitTimeoutMs);
        if (waitResult == WAIT_TIMEOUT)
        {
            LOG_WARN(L"schtasks.exe 执行超时，已终止 (超过 %d 毫秒)", kProcessWaitTimeoutMs);
            TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(pi.hProcess, 1000);
        }
        else if (waitResult == WAIT_FAILED)
        {
            LogWin32Warning(L"WaitForSingleObject(schtasks.exe)", GetLastError());
        }
        else
        {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(pi.hProcess, &exitCode))
            {
                LogWin32Warning(L"GetExitCodeProcess(schtasks.exe)", GetLastError());
            }
            else if (exitCode != 0)
            {
                if (exitCodeOut)
                    *exitCodeOut = exitCode;
                if (logNonZeroExit)
                    LOG_WARN(L"schtasks.exe 执行失败 (退出码 %lu)", exitCode);
            }
            else
            {
                if (exitCodeOut)
                    *exitCodeOut = ERROR_SUCCESS;
                success = true;
            }
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return success;
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
        std::vector<wchar_t> buffer(kMaxWindowsPathLength + 1);
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            LogWin32Error(L"GetModuleFileNameW", GetLastError());
            return {};
        }
        if (length >= buffer.size())
        {
            LOG_ERROR(L"GetModuleFileNameW 返回的路径过长，无法配置开机自启");
            return {};
        }
        return std::wstring(buffer.data(), length);
    }

    bool DirectoryWritable(const std::wstring& directory)
    {
        if (directory.empty())
            return false;

        std::wstring probe = directory + L"\\.mouselock-write-test-" +
            std::to_wstring(GetCurrentProcessId());
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        CloseHandle(file);
        return true;
    }

    bool EnsureDirectory(const std::wstring& directory)
    {
        if (directory.empty())
            return false;

        DWORD attributes = GetFileAttributesW(directory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (GetLastError() != ERROR_FILE_NOT_FOUND &&
            GetLastError() != ERROR_PATH_NOT_FOUND)
            return false;

        return CreateDirectoryW(directory.c_str(), nullptr) != FALSE ||
            GetLastError() == ERROR_ALREADY_EXISTS;
    }

    std::wstring LocalAppDataDirectory()
    {
        wchar_t path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
            SHGFP_TYPE_CURRENT, path)))
            return {};
        return path;
    }

    std::wstring ResolveDataDirectory(const std::wstring& executableDirectory)
    {
        std::wstring localDirectory = LocalAppDataDirectory();
        if (!localDirectory.empty())
        {
            localDirectory += L"\\MouseLock";
            if (EnsureDirectory(localDirectory) && DirectoryWritable(localDirectory))
                return localDirectory;
        }

        // LocalAppData 不可用时保留 exe 目录作为最后备用位置。
        return executableDirectory;
    }

    bool IsProcessElevated()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        TOKEN_ELEVATION elevation{};
        DWORD returned = 0;
        bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
            sizeof(elevation), &returned) != FALSE && elevation.TokenIsElevated != 0;
        CloseHandle(token);
        return elevated;
    }

    void LogEnvironmentDiagnostics()
    {
        OSVERSIONINFOEXW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&version)))
            LOG_INFO(L"Windows 版本: %lu.%lu build %lu", version.dwMajorVersion,
                version.dwMinorVersion, version.dwBuildNumber);
        else
            LOG_WARN(L"无法读取 Windows 版本 (错误码 %d)", GetLastError());

        LOG_INFO(L"进程权限: %s，位数: %d 位", IsProcessElevated() ? L"管理员" : L"普通用户",
            static_cast<int>(sizeof(void*) * 8));
        LOG_INFO(L"显示器布局: %d 台，虚拟桌面 %d,%d - %d,%d",
            GetSystemMetrics(SM_CMONITORS),
            GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN));
    }

    std::wstring CurrentUserIdentifier()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            LogWin32Error(L"OpenProcessToken", GetLastError());
            return {};
        }

        DWORD required = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &required);
        if (required == 0)
        {
            LogWin32Error(L"GetTokenInformation(TokenUser)", GetLastError());
            CloseHandle(token);
            return {};
        }

        std::vector<BYTE> buffer(required);
        if (!GetTokenInformation(token, TokenUser, buffer.data(), required, &required))
        {
            LogWin32Error(L"GetTokenInformation(TokenUser)", GetLastError());
            CloseHandle(token);
            return {};
        }
        auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());

        CloseHandle(token);

        // 直接使用 SID，避免域名、本地化账户名或账户重命名导致任务失效。
        LPWSTR sidText = nullptr;
        if (!ConvertSidToStringSidW(user->User.Sid, &sidText))
        {
            LogWin32Error(L"ConvertSidToStringSidW", GetLastError());
            return {};
        }

        std::wstring sid(sidText);
        LocalFree(sidText);
        return sid;
    }

    constexpr wchar_t kLegacyAutoStartTaskName[] = L"MouseLock";

    std::wstring AutoStartTaskName(const std::wstring& userSid)
    {
        if (userSid.empty())
            return {};
        return L"MouseLock-" + userSid;
    }

    std::wstring XmlEscape(const std::wstring& value)
    {
        std::wstring escaped;
        escaped.reserve(value.size());
        for (wchar_t ch : value)
        {
            switch (ch)
            {
            case L'&': escaped += L"&amp;"; break;
            case L'<': escaped += L"&lt;"; break;
            case L'>': escaped += L"&gt;"; break;
            case L'\"': escaped += L"&quot;"; break;
            case L'\'': escaped += L"&apos;"; break;
            default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    }

    bool WriteUtf16File(const std::wstring& path, const std::wstring& content)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            LogWin32Error(L"CreateFileW(计划任务 XML)", GetLastError());
            return false;
        }

        const WORD bom = 0xFEFF;
        DWORD written = 0;
        bool success = WriteFile(file, &bom, sizeof(bom), &written, nullptr) != FALSE &&
            written == sizeof(bom);
        if (success && !content.empty())
        {
            DWORD bytes = static_cast<DWORD>(content.size() * sizeof(wchar_t));
            success = WriteFile(file, content.data(), bytes, &written, nullptr) != FALSE &&
                written == bytes;
        }
        if (success && !FlushFileBuffers(file))
            success = false;
        if (!success)
            LogWin32Error(L"写入计划任务 XML", GetLastError());
        CloseHandle(file);
        return success;
    }

    std::wstring BuildAutoStartTaskXml(const std::wstring& executablePath,
        const std::wstring& userSid)
    {
        std::wstring workingDirectory;
        size_t slash = executablePath.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            workingDirectory = executablePath.substr(0, slash);

        std::wstring xml =
            L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
            L"<Task version=\"1.3\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
            L"  <RegistrationInfo><Author>MouseLock</Author></RegistrationInfo>\r\n"
            L"  <Triggers>\r\n"
            L"    <LogonTrigger>\r\n"
            L"      <Enabled>true</Enabled>\r\n"
            L"      <UserId>" + XmlEscape(userSid) + L"</UserId>\r\n"
            L"    </LogonTrigger>\r\n"
            L"  </Triggers>\r\n"
            L"  <Principals>\r\n"
            L"    <Principal id=\"Author\">\r\n"
            L"      <UserId>" + XmlEscape(userSid) + L"</UserId>\r\n"
            L"      <LogonType>InteractiveToken</LogonType>\r\n"
            L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
            L"    </Principal>\r\n"
            L"  </Principals>\r\n"
            L"  <Settings>\r\n"
            L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
            L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
            L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
            L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
            L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n"
            L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
            L"    <Enabled>true</Enabled>\r\n"
            L"    <Hidden>false</Hidden>\r\n"
            L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
            L"    <WakeToRun>false</WakeToRun>\r\n"
            L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
            L"    <Priority>7</Priority>\r\n"
            L"  </Settings>\r\n"
            L"  <Actions Context=\"Author\">\r\n"
            L"    <Exec>\r\n"
            L"      <Command>" + XmlEscape(executablePath) + L"</Command>\r\n";
        if (!workingDirectory.empty())
            xml += L"      <WorkingDirectory>" + XmlEscape(workingDirectory) + L"</WorkingDirectory>\r\n";
        xml +=
            L"    </Exec>\r\n"
            L"  </Actions>\r\n"
            L"</Task>\r\n";
        return xml;
    }

    bool AutoStart_SetEnabled(bool enabled)
    {
        RemoveLegacyRunKey();
        std::wstring userIdentifier = CurrentUserIdentifier();
        std::wstring taskName = AutoStartTaskName(userIdentifier);
        if (taskName.empty())
        {
            LOG_ERROR(L"无法生成当前用户的开机自启任务名");
            return false;
        }

        auto removeTaskIfExists = [](const std::wstring& name, const wchar_t* description) {
            DWORD queryExit = ERROR_INVALID_DATA;
            if (!RunSchTasks(L"/Query /TN \"" + name + L"\"", &queryExit, false))
            {
                if (queryExit == 1)
                    return true;
                LOG_ERROR(L"检查%s失败", description);
                return false;
            }
            if (!RunSchTasks(L"/Delete /TN \"" + name + L"\" /F"))
            {
                LOG_ERROR(L"删除%s失败", description);
                return false;
            }
            return true;
        };

        if (!enabled)
        {
            bool removed = removeTaskIfExists(taskName, L"当前用户的开机自启计划任务");
            // 清理旧版本的固定任务名，避免关闭自启后旧任务仍继续启动。
            if (!removeTaskIfExists(kLegacyAutoStartTaskName, L"旧版开机自启计划任务"))
                removed = false;
            return removed;
        }

        std::wstring executablePath = ExecutablePath();
        if (executablePath.empty() || userIdentifier.empty())
            return false;

        wchar_t tempDirectory[MAX_PATH]{};
        DWORD tempLength = GetTempPathW(static_cast<DWORD>(std::size(tempDirectory)), tempDirectory);
        if (tempLength == 0 || tempLength >= std::size(tempDirectory))
        {
            LogWin32Error(L"GetTempPathW", GetLastError());
            return false;
        }

        wchar_t tempFile[MAX_PATH]{};
        if (GetTempFileNameW(tempDirectory, L"MLK", 0, tempFile) == 0)
        {
            LogWin32Error(L"GetTempFileNameW", GetLastError());
            return false;
        }

        bool success = WriteUtf16File(tempFile, BuildAutoStartTaskXml(executablePath, userIdentifier));
        if (success)
        {
            std::wstring args = L"/Create /TN \"" + taskName + L"\" /XML " +
                QuoteCommandLineArgument(tempFile) + L" /F";
            success = RunSchTasks(args);
        }
        DeleteFileW(tempFile);

        if (success)
            success = RunSchTasks(L"/Change /TN \"" + taskName + L"\" /ENABLE");
        if (success)
            success = RunSchTasks(L"/Query /TN \"" + taskName + L"\"");

        if (success && !removeTaskIfExists(kLegacyAutoStartTaskName, L"旧版开机自启计划任务"))
            LOG_WARN(L"当前用户任务已创建，但旧版固定任务清理失败");

        if (success)
            LOG_INFO(L"开机自启计划任务已创建或更新：%s（允许电池供电启动）", taskName.c_str());
        else
            LOG_ERROR(L"开机自启计划任务创建失败");
        return success;
    }

    void OpenSettings()
    {
        bool prevAutoStart = g_cfg.autoStart;
        bool prevCenterLock = g_cfg.centerLock;
        bool prevForegroundBinding = g_cfg.foregroundBinding;
        std::vector<HWND> boundWindows(g_boundWindows.begin(), g_boundWindows.end());
        std::vector<DWORD> boundProcesses(g_boundProcesses.begin(), g_boundProcesses.end());
        g_settingsOpen = true;
        bool backupTimerPaused = g_hwnd && KillTimer(g_hwnd, kTimerBackup) != FALSE;
        ShowSettingsDialog(g_hwnd, g_cfg, boundWindows, boundProcesses, [&](AppConfig& cfg) {
            AppConfig oldCfg = g_cfg;
            bool centerLockChanged = cfg.centerLock != prevCenterLock;
            bool foregroundBindingChanged = cfg.foregroundBinding != prevForegroundBinding;
            if (cfg.autoStart != prevAutoStart)
            {
                bool requestedAutoStart = cfg.autoStart;
                if (!AutoStart_SetEnabled(requestedAutoStart))
                {
                    MessageBoxW(g_hwnd,
                        requestedAutoStart
                            ? L"开机自启动设置失败。程序已记录详细原因，请检查计划任务服务和系统权限。"
                            : L"关闭开机自启动失败。程序已记录详细原因，请稍后重试。",
                        L"MouseLock - 开机自启", MB_OK | MB_ICONWARNING);
                    cfg.autoStart = prevAutoStart;
                }
                else
                {
                    prevAutoStart = cfg.autoStart;
                }
            }

            g_cfg = cfg;
            if (!SaveConfig(g_cfgPath, g_cfg))
            {
                MessageBoxW(g_hwnd, L"配置保存失败，请检查文件权限或磁盘空间。",
                    L"错误", MB_OK | MB_ICONERROR);
                if (cfg.autoStart != oldCfg.autoStart)
                    AutoStart_SetEnabled(oldCfg.autoStart);
                cfg = oldCfg;
                g_cfg = oldCfg;
                return;
            }
            if ((centerLockChanged || foregroundBindingChanged) &&
                g_enabled && !g_unlockedByHold)
            {
                if (g_cfg.foregroundBinding)
                    RequestForegroundRefresh();
                else
                    g_locker.LockToCurrentScreen(g_cfg.centerLock);
            }
            prevCenterLock = cfg.centerLock;
            prevForegroundBinding = cfg.foregroundBinding;
        }, [&](const std::vector<HWND>& windows, const std::vector<DWORD>& processes,
               const std::vector<std::wstring>& windowPaths,
               const std::vector<std::wstring>& processPaths) {
            g_boundWindows = std::unordered_set<HWND>(windows.begin(), windows.end());
            g_boundProcesses = std::unordered_set<DWORD>(processes.begin(), processes.end());
            g_cfg.boundWindowPaths = windowPaths;
            g_cfg.boundProcessPaths = processPaths;
            if (!SaveConfig(g_cfgPath, g_cfg))
            {
                MessageBoxW(g_hwnd, L"配置保存失败，请检查文件权限或磁盘空间。",
                    L"错误", MB_OK | MB_ICONERROR);
            }
            if (g_enabled && !g_unlockedByHold)
                RequestForegroundRefresh();
        });

        g_settingsOpen = false;
        if (backupTimerPaused && g_hwnd && !SetTimer(g_hwnd, kTimerBackup, kBackupIntervalMs, nullptr))
            LOG_ERROR(L"设置窗口关闭后恢复备用校验定时器失败 (错误码 %d)", GetLastError());
    }

    // ---------------- 主窗口 ----------------

    void CreateMainControls(HWND hwnd)
    {
        g_mainFont = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

        HWND lbl = CreateWindowExW(0, L"STATIC", L"鼠标锁定工具",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 16, 300, 24, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)g_mainFont, TRUE);

        HWND chk = CreateWindowExW(0, L"BUTTON", L"启用鼠标锁定",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            20, 52, 300, 20, hwnd, (HMENU)(INT_PTR)IDC_CHK_ENABLED, g_inst, nullptr);
        SendMessageW(chk, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageW(chk, BM_SETCHECK, g_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND btnSettings = CreateWindowExW(0, L"BUTTON", L"设置...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            20, 96, kButtonWidth + 50, 32, hwnd, (HMENU)(INT_PTR)IDC_BTN_SETTINGS, g_inst, nullptr);
        SendMessageW(btnSettings, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        HWND btnHide = CreateWindowExW(0, L"BUTTON", L"隐藏到托盘",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            188, 96, kButtonWidth + 50, 32, hwnd, (HMENU)(INT_PTR)IDC_BTN_HIDE, g_inst, nullptr);
        SendMessageW(btnHide, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    }

    HMENU BuildTrayMenu();

    void ShowTrayMenu()
    {
        POINT pt{};
        GetCursorPos(&pt);
        if (g_trayMenu)
            DestroyMenu(g_trayMenu);
        g_trayMenu = BuildTrayMenu();
        SyncUi();
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
        AppendMenuW(menu, MF_STRING, ID_TRAY_BIND_TOGGLE, L"启用前台绑定");
        AppendMenuW(menu, MF_STRING, ID_TRAY_BIND_SETTINGS, L"绑定设置...");
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
        case ID_TRAY_BIND_TOGGLE:
            g_cfg.foregroundBinding = !g_cfg.foregroundBinding;
            LOG_INFO(L"前台绑定: %s", g_cfg.foregroundBinding ? L"开启" : L"关闭");
            if (!SaveConfig(g_cfgPath, g_cfg))
            {
                LOG_ERROR(L"切换前台绑定时配置保存失败");
            }
            if (g_cfg.foregroundBinding)
                RequestForegroundRefresh();
            else
                g_locker.LockToCurrentScreen(g_cfg.centerLock);
            break;
        case ID_TRAY_BIND_SETTINGS: OpenSettings(); break;
        case ID_TRAY_SETTINGS: OpenSettings(); break;
        case ID_TRAY_EXIT:
            g_allowClose = true;
            DestroyWindow(g_hwnd);
            break;
        }
    }

    void ExitApplication()
    {
        LOG_INFO(L"MouseLock 退出");
        UninstallHooks();
        g_locker.Unlock();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
        if (g_mainFont) { DeleteObject(g_mainFont); g_mainFont = nullptr; }
        if (g_trayMenu) { DestroyMenu(g_trayMenu); g_trayMenu = nullptr; }
        ShutdownLogger();
        PostQuitMessage(0);
    }

    LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage)
        {
            // Explorer 重启后原托盘图标已经失效，需要重新注册。
            g_trayAdded = false;
            TryAddTrayIcon(true);
            return 0;
        }

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
            case ID_TRAY_BIND_TOGGLE:
            case ID_TRAY_BIND_SETTINGS:
            case ID_TRAY_SETTINGS:
            case ID_TRAY_EXIT:
                HandleTrayCommand(id);
                break;
            }
            return 0;
        }

        case kMsgJump:
        {
            int direction = static_cast<int>(wp);
            if (g_enabled && (direction == -1 || direction == 1))
            {
                LOG_INFO(L"快捷键: 跳转到%s屏", direction < 0 ? L"左" : L"右");
                g_locker.Jump(direction);
            }
            return 0;
        }

        case kMsgUnlockSync:
            g_unlockSyncPosted.store(false);
            ReconcileUnlockState();
            return 0;

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
            if (wp == kTimerBackup && !g_settingsOpen)
            {
                ReconcileForegroundWindow();
                g_locker.Revalidate();
                ReconcileUnlockState();
                ReassertLock();
                // 登录/开机时任务栏可能尚未就绪，NIM_ADD 会静默失败，这里持续重试
                if (!g_trayAdded)
                    TryAddTrayIcon(false);
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
    DWORD mutexError = GetLastError();
    if (!hMutex)
    {
        MessageBoxW(nullptr, L"无法创建 MouseLock 单实例锁，程序无法安全启动。",
            L"MouseLock - 启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"已经有一个 MouseLock 程序在运行。", L"MouseLock",
            MB_OK | MB_ICONINFORMATION);
        CloseHandle(hMutex);
        return 0;
    }

    // 加载配置并初始化日志
    {
        std::wstring executablePath = ExecutablePath();
        std::wstring executableDirectory = executablePath;
        size_t slash = executableDirectory.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            executableDirectory.resize(slash);
        else
            executableDirectory = L".";

        std::wstring dataDirectory = ResolveDataDirectory(executableDirectory);
        if (dataDirectory.empty())
            dataDirectory = executableDirectory;
        if (dataDirectory.empty())
            dataDirectory = L".";

        bool usingUserDataDirectory =
            CompareStringOrdinal(dataDirectory.c_str(), -1,
                executableDirectory.c_str(), -1, TRUE) != CSTR_EQUAL;

        std::wstring p = dataDirectory;
        if (p.back() != L'\\' && p.back() != L'/')
            p += L"\\";
        g_cfgPath = p + L"config.json";

        std::wstring legacyConfigPath = executableDirectory;
        if (legacyConfigPath.empty())
            legacyConfigPath = L".";
        if (legacyConfigPath.back() != L'\\' && legacyConfigPath.back() != L'/')
            legacyConfigPath += L"\\";
        legacyConfigPath += L"config.json";

        bool migratedLegacyConfig = false;
        bool configMigrationFailed = false;
        DWORD configMigrationError = ERROR_SUCCESS;
        if (usingUserDataDirectory &&
            GetFileAttributesW(g_cfgPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(legacyConfigPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            if (CopyFileW(legacyConfigPath.c_str(), g_cfgPath.c_str(), TRUE))
                migratedLegacyConfig = true;
            else
            {
                configMigrationFailed = true;
                configMigrationError = GetLastError();
            }
        }

        std::wstring logPath = p + L"MouseLock.log";
        bool loggerFallback = false;
        bool loggerReady = InitLogger(logPath);
        if (!loggerReady)
        {
            ShutdownLogger();

            std::vector<std::wstring> fallbackLogPaths;
            if (usingUserDataDirectory)
            {
                std::wstring executableLogPath = executableDirectory;
                if (executableLogPath.empty())
                    executableLogPath = L".";
                if (executableLogPath.back() != L'\\' && executableLogPath.back() != L'/')
                    executableLogPath += L"\\";
                fallbackLogPaths.push_back(executableLogPath + L"MouseLock.log");
            }

            wchar_t tempPath[MAX_PATH]{};
            DWORD tempLength = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
            if (tempLength > 0 && tempLength < std::size(tempPath))
                fallbackLogPaths.emplace_back(std::wstring(tempPath, tempLength) + L"MouseLock.log");

            for (const auto& fallbackPath : fallbackLogPaths)
            {
                if (CompareStringOrdinal(fallbackPath.c_str(), -1,
                        logPath.c_str(), -1, TRUE) == CSTR_EQUAL)
                    continue;
                if (InitLogger(fallbackPath))
                {
                    logPath = fallbackPath;
                    loggerFallback = true;
                    loggerReady = true;
                    break;
                }
                ShutdownLogger();
            }
        }

        LOG_INFO(L"MouseLock 启动");
        LOG_INFO(L"数据目录: %s", p.c_str());
        if (usingUserDataDirectory)
            LOG_INFO(L"配置和日志已切换到用户数据目录");
        if (loggerFallback)
            LOG_WARN(L"用户数据目录日志不可写，已切换备用日志：%s", logPath.c_str());
        if (!loggerReady)
            LOG_WARN(L"日志文件无法创建，后续故障将无法写入日志");
        if (configMigrationFailed)
            LOG_WARN(L"旧配置迁移失败，将尝试直接读取旧配置 (错误码 %d)",
                configMigrationError);
        LogEnvironmentDiagnostics();
        g_cfg = LoadConfig(g_cfgPath);
        if (configMigrationFailed)
        {
            g_cfg = LoadConfig(legacyConfigPath);
            if (!SaveConfig(g_cfgPath, g_cfg))
                LOG_WARN(L"读取旧配置后写入用户数据目录失败");
        }
        else if (migratedLegacyConfig)
        {
            LOG_INFO(L"旧配置已迁移到用户数据目录");
        }
        // 配置保持勾选时也重新校验计划任务，修复任务被系统清理、禁用或旧版本
        // 创建为“仅接通电源启动”的情况。
        if (g_cfg.autoStart && !AutoStart_SetEnabled(true))
        {
            LOG_WARN(L"开机自启任务自动修复失败，已将配置恢复为关闭");
            g_cfg.autoStart = false;
            if (!SaveConfig(g_cfgPath, g_cfg))
                LOG_WARN(L"保存开机自启回滚状态失败");
        }
    }

    g_icon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_MOUSELOCK),
        IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    if (!g_icon)
        LOG_WARN(L"应用图标加载失败 (错误码 %d)", GetLastError());

    // 托盘菜单
    g_trayMenu = BuildTrayMenu();
    if (!g_trayMenu)
        LOG_ERROR(L"托盘菜单创建失败 (错误码 %d)", GetLastError());

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
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LOG_ERROR(L"主窗口类注册失败 (错误码 %d)", GetLastError());
        if (g_trayMenu) { DestroyMenu(g_trayMenu); g_trayMenu = nullptr; }
        if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
        CloseHandle(hMutex);
        ShutdownLogger();
        return 1;
    }

    RECT rc{ 0, 0, kMainWindowWidth, kMainWindowHeight };
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (!AdjustWindowRectEx(&rc, style, FALSE, 0))
    {
        LOG_ERROR(L"主窗口尺寸计算失败 (错误码 %d)", GetLastError());
        if (g_trayMenu) { DestroyMenu(g_trayMenu); g_trayMenu = nullptr; }
        if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
        CloseHandle(hMutex);
        ShutdownLogger();
        return 1;
    }

    g_hwnd = CreateWindowExW(0, kMainClass, L"鼠标锁定 (MouseLock)", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd)
    {
        LOG_ERROR(L"主窗口创建失败 (错误码 %d)", GetLastError());
        if (g_trayMenu) { DestroyMenu(g_trayMenu); g_trayMenu = nullptr; }
        if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
        CloseHandle(hMutex);
        ShutdownLogger();
        return 1;
    }

    // 托盘图标
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreatedMessage == 0)
        LOG_WARN(L"注册 TaskbarCreated 消息失败 (错误码 %d)", GetLastError());

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, L"鼠标锁定", sizeof(g_nid.szTip) / sizeof(WCHAR));
    g_trayAdded = false;
    if (!TryAddTrayIcon(true))
        LOG_WARN(L"托盘图标初始化失败，将在定时器中重试");

    InstallHooks();
    if (!SetTimer(g_hwnd, kTimerBackup, kBackupIntervalMs, nullptr))
        LOG_ERROR(L"备用校验定时器创建失败 (错误码 %d)", GetLastError());
    SyncUi();

    if (g_enabled)
    {
        g_lastForegroundWindow = GetForegroundWindow();
        ApplyForegroundBinding(g_lastForegroundWindow);
    }

    if (g_cfg.startSilent)
        ShowBalloon(L"已在后台运行。");
    else
        ShowMain();

    // 消息循环
    MSG m{};
    while (true)
    {
        int result = GetMessageW(&m, nullptr, 0, 0);
        if (result == -1)
        {
            LOG_ERROR(L"主窗口消息循环失败 (错误码 %d)", GetLastError());
            break;
        }
        if (result == 0)
            break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    CloseHandle(hMutex);
    return 0;
}
