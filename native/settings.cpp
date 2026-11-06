// 设置对话框实现：程序化创建控件，快捷键录入框采用子类化捕获按键。
#include "settings.h"
#include "hotkey_validation.h"
#include "handle_guard.h"
#include "constants.h"

#include <string>
#include <functional>
#include <map>
#include <algorithm>
#include <cwctype>
#include <psapi.h>
#include <commctrl.h>
#include "logger.h"

namespace
{
    struct WindowOption
    {
        HWND hwnd = nullptr;
        DWORD processId = 0;
        std::wstring processPath;
        std::wstring label;
        bool hidden = false;
    };

    struct BindingOption
    {
        HWND hwnd = nullptr;
        DWORD processId = 0;
        std::wstring path;
        std::wstring label;
    };

    struct ApplicationOption
    {
        DWORD processId = 0;
        std::wstring path;
        std::wstring label;
        bool hidden = false;
    };

    // Owner-data list entries keep large lists out of the native LISTBOX string store.
    // The control only asks the parent to draw rows that are currently visible.
    struct ListDisplayItem
    {
        std::wstring label;
        LPARAM data = 0;
    };

    struct SettingsCtx
    {
        AppConfig cfg;
        HWND hwnd = nullptr;
        HWND unlockEdit = nullptr;
        HWND toggleLockEdit = nullptr;
        HWND jumpLeftEdit = nullptr;
        HWND jumpRightEdit = nullptr;
        HWND chkEnabled = nullptr;
        HWND chkSilent = nullptr;
        HWND chkAutoStart = nullptr;
        HWND chkCenter = nullptr;
        HWND chkBinding = nullptr;
        HWND chkHideTrayIcon = nullptr;
        HWND applicationList = nullptr;
        HWND bindingList = nullptr;
        HWND applicationSearch = nullptr;
        HWND showHidden = nullptr;
        HWND bindSelected = nullptr;
        HWND refreshBindings = nullptr;
        HWND removeBindings = nullptr;
        HWND clearBindings = nullptr;
        HWND applicationLabel = nullptr;
        HWND bindingLabel = nullptr;
        HWND tooltip = nullptr;
        std::vector<WindowOption> windowOptions;
        std::map<DWORD, std::wstring> processOptions;
        std::map<DWORD, std::wstring> processPathCache;
        std::map<DWORD, bool> processHasVisibleWindow;
        std::vector<ApplicationOption> applicationOptions;
        std::vector<BindingOption> bindingOptions;
        std::vector<ListDisplayItem> applicationListItems;
        std::vector<ListDisplayItem> bindingListItems;
        std::vector<HWND> boundWindows;
        std::vector<DWORD> boundProcesses;
        std::vector<std::wstring> boundWindowPaths;
        std::vector<std::wstring> boundProcessPaths;
        BindingChangedCallback onBindingsChanged;
        std::function<void(AppConfig&)> onChanged;
        std::function<void(bool)> onEnabledChanged;
        std::function<void(bool)> onHotkeyCaptureChanged;
    };

    SettingsCtx* g_ctx = nullptr;

    std::wstring ProcessPath(DWORD processId)
    {
        unique_process_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
        if (!process)
            return {};

        std::vector<wchar_t> path(1024);
        for (;;)
        {
            DWORD length = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
                return std::wstring(path.data(), length);

            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
                path.size() >= kMaxWindowsPathLength)
                return {};

            size_t nextSize = path.size() * 2;
            if (nextSize <= path.size())
                nextSize = path.size() + 1;
            if (nextSize > kMaxWindowsPathLength)
                nextSize = kMaxWindowsPathLength;
            path.resize(nextSize);
        }
    }

    const std::wstring& CachedProcessPath(DWORD processId)
    {
        static const std::wstring emptyPath;
        if (!g_ctx || !processId)
            return emptyPath;

        auto existing = g_ctx->processPathCache.find(processId);
        if (existing != g_ctx->processPathCache.end())
            return existing->second;

        auto inserted = g_ctx->processPathCache.emplace(processId, ProcessPath(processId));
        return inserted.first->second;
    }

    std::wstring ProcessName(DWORD processId)
    {
        std::wstring result = CachedProcessPath(processId);
        if (result.empty() && !g_ctx)
            result = ProcessPath(processId);
        size_t slash = result.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            result.erase(0, slash + 1);
        return result.empty() ? L"PID " + std::to_wstring(processId) : result;
    }

    bool SamePath(const std::wstring& left, const std::wstring& right)
    {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    bool ContainsPath(const std::vector<std::wstring>& paths, const std::wstring& path)
    {
        return std::any_of(paths.begin(), paths.end(), [&](const std::wstring& candidate) {
            return SamePath(candidate, path);
        });
    }

    bool ContainsText(const std::wstring& value, const std::wstring& query)
    {
        if (query.empty())
            return true;
        std::wstring lowerValue = value;
        std::wstring lowerQuery = query;
        std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), towlower);
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), towlower);
        return lowerValue.find(lowerQuery) != std::wstring::npos;
    }

    void ClearListViewSelection(HWND list)
    {
        if (!list)
            return;
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetSelectionMark(list, -1);
    }

    std::wstring ControlText(HWND control)
    {
        wchar_t text[512]{};
        GetWindowTextW(control, text, static_cast<int>(std::size(text)));
        return text;
    }

    BOOL CALLBACK CollectWindowOptions(HWND hwnd, LPARAM data)
    {
        auto* options = reinterpret_cast<std::vector<WindowOption>*>(data);
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (!processId || processId == GetCurrentProcessId())
            return TRUE;
        wchar_t className[64]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        if (lstrcmpW(className, L"Progman") == 0 ||
            lstrcmpW(className, L"WorkerW") == 0 ||
            lstrcmpW(className, L"Shell_TrayWnd") == 0 ||
            lstrcmpW(className, L"Shell_SecondaryTrayWnd") == 0)
            return TRUE;
        bool hidden = !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
            (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
        auto existing = std::find_if(options->begin(), options->end(),
            [processId](const WindowOption& option) {
                return option.processId == processId;
            });
        // 一个进程只保留一个可绑定项，避免浏览器不同页面/顶层窗口重复出现。
        // 优先保留可见、未最小化且不是工具窗口的代表窗口。
        if (existing != options->end())
        {
            if (existing->hidden && !hidden)
            {
                existing->hwnd = hwnd;
                existing->hidden = false;
                existing->processPath = CachedProcessPath(processId);
                existing->label = ProcessName(processId) + L" [PID " +
                    std::to_wstring(processId) + L"]";
            }
            return TRUE;
        }
        std::wstring path = CachedProcessPath(processId);
        std::wstring label = ProcessName(processId);
        if (hidden)
            label += L" [隐藏]";
        label += L" [PID " + std::to_wstring(processId) + L"]";
        options->push_back({ hwnd, processId, std::move(path), std::move(label), hidden });
        return TRUE;
    }

    bool IsPathBinding(const std::wstring& path)
    {
        if (!g_ctx || path.empty())
            return false;
        return ContainsPath(g_ctx->boundWindowPaths, path) ||
               ContainsPath(g_ctx->boundProcessPaths, path);
    }

    void AddBindingOption(HWND hwnd, DWORD processId,
                          const std::wstring& path, const std::wstring& label)
    {
        if (!g_ctx)
            return;
        g_ctx->bindingOptions.push_back({ hwnd, processId, path, label });
    }

    void CommitBindings()
    {
        if (!g_ctx)
            return;
        // 统一按路径绑定，旧版按窗口句柄/PID保存的临时项不能继续覆盖移除操作。
        g_ctx->boundWindows.clear();
        g_ctx->boundProcesses.clear();
        g_ctx->cfg.boundWindowPaths = g_ctx->boundWindowPaths;
        g_ctx->cfg.boundProcessPaths.clear();
        if (g_ctx->onBindingsChanged)
            g_ctx->onBindingsChanged(g_ctx->boundWindows, g_ctx->boundProcesses,
                g_ctx->boundWindowPaths, {});
    }

    // 收集并去重进程列表。未显示隐藏项时只使用窗口枚举结果，
    // 避免为所有后台进程打开句柄查询路径。
    void CollectUniqueProcesses(bool includeHidden)
    {
        if (!g_ctx)
            return;

        g_ctx->processOptions.clear();

        struct ProcessCandidate
        {
            DWORD processId = 0;
            std::wstring name;
            bool visible = false;
        };

        g_ctx->processHasVisibleWindow.clear();
        for (const auto& option : g_ctx->windowOptions)
            if (!option.hidden)
                g_ctx->processHasVisibleWindow[option.processId] = true;

        std::map<std::wstring, ProcessCandidate> uniqueProcesses;
        auto consider = [&](DWORD processId, const std::wstring& path) {
            if (!processId || processId == GetCurrentProcessId() || path.empty())
                return;

            auto visibleIt = g_ctx->processHasVisibleWindow.find(processId);
            bool visible = visibleIt != g_ctx->processHasVisibleWindow.end() && visibleIt->second;
            auto existing = uniqueProcesses.find(path);
            if (existing == uniqueProcesses.end() ||
                (!existing->second.visible && visible))
            {
                uniqueProcesses[path] = {
                    processId,
                    ProcessName(processId),
                    visible
                };
            }
        };

        if (!includeHidden)
        {
            for (const auto& option : g_ctx->windowOptions)
                if (!option.hidden)
                    consider(option.processId, option.processPath);
        }
        else
        {
            std::vector<DWORD> processIds(1024);
            DWORD bytes = 0;
            for (;;)
            {
                if (!EnumProcesses(processIds.data(),
                    static_cast<DWORD>(processIds.size() * sizeof(DWORD)), &bytes))
                    break;
                if (bytes < processIds.size() * sizeof(DWORD) ||
                    processIds.size() >= kMaxProcessEnumerationEntries)
                    break;
                processIds.resize(processIds.size() * 2);
            }

            size_t count = bytes / sizeof(DWORD);
            for (size_t i = 0; i < count; ++i)
            {
                DWORD processId = processIds[i];
                consider(processId, CachedProcessPath(processId));
            }
        }

        // 将去重后的进程添加到选项中
        g_ctx->applicationOptions.clear();
        for (const auto& [path, candidate] : uniqueProcesses)
        {
            g_ctx->processOptions.emplace(candidate.processId, candidate.name);
            std::wstring label = candidate.name + L" [PID " +
                std::to_wstring(candidate.processId) + L"]";
            if (!candidate.visible)
                label += L" [无可见窗口]";
            auto existing = std::find_if(g_ctx->applicationOptions.begin(),
                g_ctx->applicationOptions.end(), [&](const ApplicationOption& item) {
                    return SamePath(item.path, path);
                });
            if (existing == g_ctx->applicationOptions.end())
            {
                g_ctx->applicationOptions.push_back({
                    candidate.processId, path, std::move(label), !candidate.visible });
            }
            else if (existing->hidden && candidate.visible)
            {
                existing->processId = candidate.processId;
                existing->label = std::move(label);
                existing->hidden = false;
            }
        }
    }

    // 刷新统一的应用列表。窗口和进程只作为查找入口，最终按可执行文件路径绑定。
    void RefreshApplicationListDisplay()
    {
        if (!g_ctx)
            return;

        std::wstring query = ControlText(g_ctx->applicationSearch);
        bool showHidden = SendMessageW(g_ctx->showHidden, BM_GETCHECK, 0, 0) == BST_CHECKED;

        ClearListViewSelection(g_ctx->applicationList);
        ListView_SetItemCountEx(g_ctx->applicationList, 0,
            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        g_ctx->applicationListItems.clear();
        for (const auto& option : g_ctx->applicationOptions)
        {
            if ((!showHidden && option.hidden) || IsPathBinding(option.path) ||
                !ContainsText(option.label, query))
                continue;
            g_ctx->applicationListItems.push_back({ option.label,
                static_cast<LPARAM>(option.processId) });
        }
        ListView_SetItemCountEx(g_ctx->applicationList,
            static_cast<int>(g_ctx->applicationListItems.size()),
            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    }

    // 提取文件名（从完整路径）
    std::wstring ExtractFilename(const std::wstring& path)
    {
        std::wstring filename = path;
        size_t slash = filename.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            filename = filename.substr(slash + 1);
        return filename;
    }

    // 刷新当前绑定列表显示
    void RefreshBindingListDisplay()
    {
        if (!g_ctx)
            return;

        g_ctx->bindingOptions.clear();
        ClearListViewSelection(g_ctx->bindingList);
        ListView_SetItemCountEx(g_ctx->bindingList, 0,
            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        g_ctx->bindingListItems.clear();

        // 统一显示应用绑定。旧版的进程路径已在加载时并入 boundWindowPaths。
        for (const auto& path : g_ctx->boundWindowPaths)
        {
            DWORD processId = 0;
            auto option = std::find_if(g_ctx->windowOptions.begin(), g_ctx->windowOptions.end(),
                [&](const WindowOption& candidate) {
                    return SamePath(candidate.processPath, path);
                });
            if (option != g_ctx->windowOptions.end())
                processId = option->processId;

            std::wstring filename = ExtractFilename(path);
            std::wstring label = L"[应用] " + filename;
            if (processId != 0)
                label += L" [PID " + std::to_wstring(processId) + L"]";

            AddBindingOption(nullptr, processId, path, label);
        }

        // 显示所有绑定项
        for (size_t i = 0; i < g_ctx->bindingOptions.size(); ++i)
        {
            const auto& opt = g_ctx->bindingOptions[i];
            g_ctx->bindingListItems.push_back({ opt.label,
                static_cast<LPARAM>(i) });
        }
        ListView_SetItemCountEx(g_ctx->bindingList,
            static_cast<int>(g_ctx->bindingListItems.size()),
            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    }

    void RefreshBindingDisplays()
    {
        if (!g_ctx)
            return;

        // 搜索框和“显示隐藏项”只影响现有数据的筛选，不需要重新枚举进程。
        SendMessageW(g_ctx->applicationList, WM_SETREDRAW, FALSE, 0);
        SendMessageW(g_ctx->bindingList, WM_SETREDRAW, FALSE, 0);
        RefreshApplicationListDisplay();
        RefreshBindingListDisplay();

        HWND lists[] = { g_ctx->applicationList, g_ctx->bindingList };
        for (HWND list : lists)
        {
            SendMessageW(list, WM_SETREDRAW, TRUE, 0);
            // 立即完成刷新后的首帧绘制，避免第一次拖动滚动条时才处理积压的重绘。
            RedrawWindow(list, nullptr, nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
    }

    // 主刷新函数：协调所有列表的刷新
    void RefreshBindingLists()
    {
        if (!g_ctx)
            return;

        g_ctx->processPathCache.clear();

        // 步骤 1：收集窗口列表
        g_ctx->windowOptions.clear();
        EnumWindows(CollectWindowOptions, reinterpret_cast<LPARAM>(&g_ctx->windowOptions));

        // 步骤 2：收集并按可执行文件路径去重应用
        bool showHidden = SendMessageW(g_ctx->showHidden, BM_GETCHECK, 0, 0) == BST_CHECKED;
        CollectUniqueProcesses(showHidden);

        // 步骤 3：刷新应用列表和当前绑定列表
        RefreshBindingDisplays();
    }

    void ApplyBindingSelections(bool remove)
    {
        if (!g_ctx)
            return;
        std::vector<std::wstring> selectedPaths;
        int count = ListView_GetItemCount(g_ctx->applicationList);
        for (int i = 0; i < count; ++i)
            if ((ListView_GetItemState(g_ctx->applicationList, i, LVIS_SELECTED) & LVIS_SELECTED) != 0)
            {
                if (i >= 0 && static_cast<size_t>(i) < g_ctx->applicationListItems.size())
                {
                    DWORD processId = static_cast<DWORD>(
                        g_ctx->applicationListItems[static_cast<size_t>(i)].data);
                    auto option = std::find_if(g_ctx->applicationOptions.begin(),
                        g_ctx->applicationOptions.end(), [processId](const ApplicationOption& item) {
                            return item.processId == processId;
                        });
                    if (option != g_ctx->applicationOptions.end())
                        selectedPaths.push_back(option->path);
                }
            }
        if (remove)
        {
            std::vector<size_t> selectedBindings;
            count = ListView_GetItemCount(g_ctx->bindingList);
            for (int i = 0; i < count; ++i)
                if ((ListView_GetItemState(g_ctx->bindingList, i, LVIS_SELECTED) & LVIS_SELECTED) != 0)
                {
                    if (i >= 0 && static_cast<size_t>(i) < g_ctx->bindingListItems.size())
                    {
                        LRESULT item = g_ctx->bindingListItems[static_cast<size_t>(i)].data;
                        if (item >= 0 && static_cast<size_t>(item) < g_ctx->bindingOptions.size())
                            selectedBindings.push_back(static_cast<size_t>(item));
                    }
                }
            for (size_t item : selectedBindings)
            {
                const auto& binding = g_ctx->bindingOptions[item];
                g_ctx->boundWindowPaths.erase(std::remove_if(g_ctx->boundWindowPaths.begin(),
                    g_ctx->boundWindowPaths.end(), [&](const std::wstring& path) {
                        return SamePath(path, binding.path);
                    }), g_ctx->boundWindowPaths.end());
            }
        }
        else
        {
            for (const auto& path : selectedPaths)
            {
                if (!path.empty() && !ContainsPath(g_ctx->boundWindowPaths, path))
                    g_ctx->boundWindowPaths.push_back(path);
            }
        }
        CommitBindings();
        RefreshBindingLists();
    }

    WNDPROC g_origHotkeyEditProc = nullptr;
    HFONT g_boldFont = nullptr;

    bool IsModifierVk(int vk)
    {
        return vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL ||
               vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU ||
               vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT;
    }

    bool IsKeyDown(int vk) { return (GetAsyncKeyState(vk) & kAsyncKeyStatePressedMask) != 0; }

    std::vector<Modifier> GetPressedModifiers()
    {
        std::vector<Modifier> mods;
        if (IsKeyDown(VK_LCONTROL) || IsKeyDown(VK_RCONTROL)) mods.push_back(Modifier::Control);
        if (IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU)) mods.push_back(Modifier::Alt);
        if (IsKeyDown(VK_LSHIFT) || IsKeyDown(VK_RSHIFT)) mods.push_back(Modifier::Shift);
        return mods;
    }

    int MouseButtonVkFromMessage(UINT msg, WPARAM wp)
    {
        if (msg != WM_XBUTTONDOWN && msg != WM_XBUTTONUP)
            return 0;
        switch (HIWORD(wp))
        {
        case XBUTTON1: return VK_XBUTTON1;
        case XBUTTON2: return VK_XBUTTON2;
        default: return 0;
        }
    }

    void UpdateEditText(HWND edit, const Hotkey& hk)
    {
        SetWindowTextW(edit, FormatHotkey(hk).c_str());
    }

    void SyncConfigControls()
    {
        if (!g_ctx)
            return;
        UpdateEditText(g_ctx->unlockEdit, g_ctx->cfg.unlock);
        UpdateEditText(g_ctx->toggleLockEdit, g_ctx->cfg.toggleLock);
        UpdateEditText(g_ctx->jumpLeftEdit, g_ctx->cfg.jumpLeft);
        UpdateEditText(g_ctx->jumpRightEdit, g_ctx->cfg.jumpRight);
        SendMessageW(g_ctx->chkSilent, BM_SETCHECK,
            g_ctx->cfg.startSilent ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_ctx->chkAutoStart, BM_SETCHECK,
            g_ctx->cfg.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_ctx->chkCenter, BM_SETCHECK,
            g_ctx->cfg.centerLock ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_ctx->chkBinding, BM_SETCHECK,
            g_ctx->cfg.foregroundBinding ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_ctx->chkHideTrayIcon, BM_SETCHECK,
            g_ctx->cfg.hideTrayIcon ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    // 将当前配置实时提交（写回并立即生效）
    void Commit()
    {
        if (g_ctx && g_ctx->onChanged)
        {
            g_ctx->onChanged(g_ctx->cfg);
            SyncConfigControls();
        }
    }

    mouse_lock::HotkeyAction HotkeyActionFor(const Hotkey* hotkey)
    {
        if (hotkey == &g_ctx->cfg.toggleLock)
            return mouse_lock::HotkeyAction::ToggleLock;
        if (hotkey == &g_ctx->cfg.jumpLeft)
            return mouse_lock::HotkeyAction::JumpLeft;
        if (hotkey == &g_ctx->cfg.jumpRight)
            return mouse_lock::HotkeyAction::JumpRight;
        return mouse_lock::HotkeyAction::Unlock;
    }

    bool TrySetHotkey(HWND edit, Hotkey& hotkey, const Hotkey& candidate)
    {
        if (mouse_lock::ConflictsWithOtherHotkey(
                g_ctx->cfg, HotkeyActionFor(&hotkey), candidate))
        {
            UpdateEditText(edit, hotkey);
            MessageBeep(MB_ICONWARNING);
            MessageBoxW(g_ctx->hwnd, L"这个快捷键已分配给其他功能，请选择不同的快捷键。",
                L"快捷键冲突", MB_OK | MB_ICONWARNING);
            return false;
        }

        hotkey = candidate;
        UpdateEditText(edit, hotkey);
        Commit();
        return true;
    }

    // 快捷键录入框子类过程
    LRESULT CALLBACK HotkeyEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
        {
            if (g_ctx && g_ctx->onHotkeyCaptureChanged)
                g_ctx->onHotkeyCaptureChanged(msg == WM_SETFOCUS);
        }

        if (msg == WM_GETDLGCODE)
            return DLGC_WANTALLKEYS;

        if (msg == WM_XBUTTONDOWN)
        {
            auto* hk = reinterpret_cast<Hotkey*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            int mouseButton = MouseButtonVkFromMessage(msg, wp);
            if (hk && mouseButton != 0)
            {
                Hotkey candidate;
                candidate.key = mouseButton;
                candidate.modifiers = GetPressedModifiers();
                if (!TrySetHotkey(hwnd, *hk, candidate))
                    return 0;
                SetFocus(GetParent(hwnd)); // 输入完成后自动失焦
                return 0;
            }
        }

        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
        {
            auto* hk = reinterpret_cast<Hotkey*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (hk)
            {
                if (wp == VK_ESCAPE)
                {
                    hk->key = -1;
                    hk->modifiers.clear();
                    UpdateEditText(hwnd, *hk);
                    Commit();
                    return 0;
                }
                if (!IsModifierVk((int)wp))
                {
                    Hotkey candidate;
                    candidate.key = static_cast<int>(wp);
                    candidate.modifiers = GetPressedModifiers();
                    if (!TrySetHotkey(hwnd, *hk, candidate))
                        return 0;
                    SetFocus(GetParent(hwnd)); // 输入完成后自动失焦
                    return 0;
                }
            }
        }
        if (msg == WM_SETCURSOR)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (g_origHotkeyEditProc)
            return CallWindowProcW(g_origHotkeyEditProc, hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, bool bold)
    {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, 300, 16, parent, nullptr, nullptr, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)(bold ? g_boldFont : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return h;
    }

    HWND CreateEdit(HWND parent, int x, int y, int w, int h, Hotkey* target)
    {
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_READONLY | WS_TABSTOP,
            x, y, w, h, parent, nullptr, nullptr, nullptr);
        SetWindowLongPtrW(e, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(target));
        SetWindowLongPtrW(e, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HotkeyEditProc));
        SendMessageW(e, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return e;
    }

    HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id)
    {
        HWND b = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return b;
    }

    HWND CreateCheck(HWND parent, const wchar_t* text, int x, int y, int id)
    {
        HWND c = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            x, y, 400, 18, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return c;
    }

    const std::vector<ListDisplayItem>& ListItemsForList(HWND list)
    {
        static const std::vector<ListDisplayItem> empty;
        if (!g_ctx)
            return empty;
        if (list == g_ctx->applicationList)
            return g_ctx->applicationListItems;
        if (list == g_ctx->bindingList)
            return g_ctx->bindingListItems;
        return empty;
    }

    void ConfigureVirtualList(HWND list)
    {
        if (!list)
            return;

        LVCOLUMNW column{};
        column.mask = LVCF_WIDTH;
        column.cx = 320;
        ListView_InsertColumn(list, 0, &column);
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);
    }

    void LayoutBindingControls(HWND hwnd)
    {
        if (!g_ctx)
            return;
        if (IsIconic(hwnd))
            return;
        RECT client{};
        GetClientRect(hwnd, &client);
        int width = client.right - 32;
        if (width < 720)
            width = 720;
        int leftWidth = (width - 16) / 2;
        int rightX = 16 + leftWidth + 16;
        int rightWidth = width - leftWidth - 16;

        // Keep the hidden-entry checkbox and action buttons outside both lists.
        const int listTop = 397;
        const int labelHeight = 16;
        const int labelSearchGap = 4;
        const int searchHeight = 23;
        const int searchListGap = 4;
        const int hiddenHeight = 18;
        const int hiddenGap = 8;
        const int buttonHeight = 28;
        const int buttonGap = 8;
        const int bottomMargin = 12;
        int buttonTop = client.bottom - bottomMargin - buttonHeight;
        int hiddenTop = buttonTop - buttonGap - hiddenHeight;
        int listBottom = hiddenTop - hiddenGap;
        MoveWindow(g_ctx->applicationLabel, 16, 350, leftWidth, labelHeight, TRUE);
        MoveWindow(g_ctx->applicationSearch, 16, 370, leftWidth, searchHeight, TRUE);
        MoveWindow(g_ctx->applicationList, 16, listTop, leftWidth, listBottom - listTop, TRUE);
        ListView_SetColumnWidth(g_ctx->applicationList, 0, (std::max)(0, leftWidth - 4));
        MoveWindow(g_ctx->bindingLabel, rightX, 350, rightWidth, labelHeight, TRUE);
        MoveWindow(g_ctx->bindingList, rightX, 370, rightWidth, listBottom - 370, TRUE);
        ListView_SetColumnWidth(g_ctx->bindingList, 0, (std::max)(0, rightWidth - 4));
        MoveWindow(g_ctx->showHidden, 16, hiddenTop, width, hiddenHeight, TRUE);
        MoveWindow(g_ctx->refreshBindings, 16, buttonTop, 100, buttonHeight, TRUE);
        MoveWindow(g_ctx->bindSelected, 128, buttonTop, 120, buttonHeight, TRUE);
        MoveWindow(g_ctx->removeBindings, 260, buttonTop, 120, buttonHeight, TRUE);
        MoveWindow(g_ctx->clearBindings, 392, buttonTop, 120, buttonHeight, TRUE);
    }

    void ApplyDefaultGood(HWND edit, Hotkey& hk, const Hotkey& def)
    {
        TrySetHotkey(edit, hk, def);
    }

    void ClearGood(HWND edit, Hotkey& hk)
    {
        hk.key = -1;
        hk.modifiers.clear();
        UpdateEditText(edit, hk);
        Commit();
    }

}

void CreateSettingsControls(HWND parent, const AppConfig& current, bool enabled,
                            const std::vector<HWND>& boundWindows,
                            const std::vector<DWORD>& boundProcesses,
                            const SettingsCallbacks& callbacks)
{
    if (g_ctx)
        DestroySettingsControls();

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&commonControls);

    g_boldFont = CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    WNDCLASSEXW wce{};
    wce.cbSize = sizeof(wce);
    if (GetClassInfoExW(nullptr, L"EDIT", &wce) && wce.lpfnWndProc)
        g_origHotkeyEditProc = wce.lpfnWndProc;
    else
    {
        g_origHotkeyEditProc = nullptr;
        LOG_ERROR(L"无法获取 EDIT 控件的原始窗口过程，快捷键编辑框将使用系统默认过程");
    }

    g_ctx = new SettingsCtx();
    g_ctx->hwnd = parent;
    g_ctx->cfg = current;
    g_ctx->boundWindows = boundWindows;
    g_ctx->boundProcesses = boundProcesses;
    g_ctx->boundWindowPaths = current.boundWindowPaths;
    for (const auto& path : current.boundProcessPaths)
        if (!path.empty() && !ContainsPath(g_ctx->boundWindowPaths, path))
            g_ctx->boundWindowPaths.push_back(path);
    g_ctx->boundProcessPaths.clear();
    g_ctx->onChanged = callbacks.onChanged;
    g_ctx->onBindingsChanged = callbacks.onBindingsChanged;
    g_ctx->onEnabledChanged = callbacks.onEnabledChanged;
    g_ctx->onHotkeyCaptureChanged = callbacks.onHotkeyCaptureChanged;

    CreateLabel(parent, L"临时解锁键（长按）：", 16, 22, false);
    CreateLabel(parent, L"跳转到上一屏幕：", 16, 58, false);
    CreateLabel(parent, L"跳转到下一屏幕：", 16, 94, false);
    CreateLabel(parent, L"启用/停用锁定：", 16, 130, false);
    CreateLabel(parent, L"锁定模式", 16, 158, true);

    g_ctx->unlockEdit = CreateEdit(parent, 150, 18, 196, 23, &g_ctx->cfg.unlock);
    g_ctx->toggleLockEdit = CreateEdit(parent, 150, 126, 196, 23, &g_ctx->cfg.toggleLock);
    g_ctx->jumpLeftEdit = CreateEdit(parent, 150, 54, 196, 23, &g_ctx->cfg.jumpLeft);
    g_ctx->jumpRightEdit = CreateEdit(parent, 150, 90, 196, 23, &g_ctx->cfg.jumpRight);

    CreateButton(parent, L"↻", 352, 18, 26, 23, IDC_UNLOCK_RESTORE);
    CreateButton(parent, L"✕", 382, 18, 26, 23, IDC_UNLOCK_CLEAR);
    CreateButton(parent, L"↻", 352, 126, 26, 23, IDC_TOGGLELOCK_RESTORE);
    CreateButton(parent, L"✕", 382, 126, 26, 23, IDC_TOGGLELOCK_CLEAR);
    CreateButton(parent, L"↻", 352, 54, 26, 23, IDC_JUMPLEFT_RESTORE);
    CreateButton(parent, L"✕", 382, 54, 26, 23, IDC_JUMPLEFT_CLEAR);
    CreateButton(parent, L"↻", 352, 90, 26, 23, IDC_JUMPRIGHT_RESTORE);
    CreateButton(parent, L"✕", 382, 90, 26, 23, IDC_JUMPRIGHT_CLEAR);

    g_ctx->chkEnabled = CreateCheck(parent, L"启用鼠标锁定", 520, 18, IDC_CHK_ENABLED);
    SendMessageW(g_ctx->chkEnabled, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    g_ctx->chkCenter = CreateCheck(parent, L"将鼠标锁定到当前屏幕正中间", 16, 182, IDC_CHK_CENTER);
    g_ctx->chkBinding = CreateCheck(parent, L"仅当前绑定窗口或进程在前台时锁定", 16, 210, IDC_CHK_BINDING);
    CreateLabel(parent, L"启动选项", 16, 238, true);
    g_ctx->chkSilent = CreateCheck(parent, L"启动时静默启动到托盘（不显示主窗口）", 16, 262, IDC_CHK_SILENT);
    g_ctx->chkAutoStart = CreateCheck(parent, L"开机自动启动", 16, 290, IDC_CHK_AUTOSTART);
    g_ctx->chkHideTrayIcon = CreateCheck(parent,
        L"隐藏系统托盘图标（再次运行程序可打开设置）", 16, 318, IDC_CHK_HIDE_TRAY_ICON);

    g_ctx->applicationLabel = CreateLabel(parent, L"绑定应用（窗口和进程统一）", 16, 350, true);
    g_ctx->applicationSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 370, 360, 23, parent,
        (HMENU)(INT_PTR)IDC_EDIT_WINDOW_SEARCH, nullptr, nullptr);
    g_ctx->applicationList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
            LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS,
        16, 397, 360, 120, parent, (HMENU)(INT_PTR)IDC_LIST_BIND_WINDOWS, nullptr, nullptr);
    g_ctx->bindingLabel = CreateLabel(parent, L"当前已绑定", 392, 350, true);
    g_ctx->bindingList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
            LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS,
        392, 370, 360, 322, parent, (HMENU)(INT_PTR)IDC_LIST_CURRENT_BINDINGS, nullptr, nullptr);
    ConfigureVirtualList(g_ctx->applicationList);
    ConfigureVirtualList(g_ctx->bindingList);
    g_ctx->showHidden = CreateCheck(parent, L"显示无可见窗口的后台应用", 16, 654, IDC_CHK_SHOW_HIDDEN);
    g_ctx->refreshBindings = CreateButton(parent, L"刷新列表", 16, 680, 100, 28, IDC_BTN_REFRESH_BINDINGS);
    g_ctx->bindSelected = CreateButton(parent, L"绑定选中项", 128, 680, 120, 28, IDC_BTN_BIND_SELECTED);
    g_ctx->removeBindings = CreateButton(parent, L"移除选中绑定", 260, 680, 120, 28, IDC_BTN_REMOVE_BINDINGS);
    g_ctx->clearBindings = CreateButton(parent, L"清空全部绑定", 392, 680, 120, 28, IDC_BTN_CLEAR_BINDINGS);

    SyncSettingsControls(current, enabled);
    SendMessageW(g_ctx->showHidden, BM_SETCHECK, BST_UNCHECKED, 0);
    RefreshBindingLists();
    LayoutBindingControls(parent);
}

void SyncSettingsControls(const AppConfig& config, bool enabled)
{
    if (!g_ctx)
        return;
    g_ctx->cfg = config;
    SyncConfigControls();
    SendMessageW(g_ctx->chkEnabled, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

void DestroySettingsControls()
{
    if (!g_ctx)
        return;
    delete g_ctx;
    g_ctx = nullptr;
    DeleteObject(g_boldFont);
    g_boldFont = nullptr;
    g_origHotkeyEditProc = nullptr;
}

bool HandleSettingsMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    if (!g_ctx)
        return false;

    switch (msg)
    {
    case WM_NOTIFY:
    {
        auto* header = reinterpret_cast<NMHDR*>(lp);
        if (!header || header->code != LVN_GETDISPINFO)
            return false;
        auto* info = reinterpret_cast<NMLVDISPINFOW*>(lp);
        if ((info->item.mask & LVIF_TEXT) != 0 && info->item.iItem >= 0)
        {
            const auto& items = ListItemsForList(header->hwndFrom);
            size_t index = static_cast<size_t>(info->item.iItem);
            if (index < items.size())
            {
                info->item.pszText = const_cast<LPWSTR>(items[index].label.c_str());
                info->item.cchTextMax = static_cast<int>(items[index].label.size() + 1);
            }
        }
        result = 0;
        return true;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == IDC_EDIT_WINDOW_SEARCH && HIWORD(wp) == EN_CHANGE)
        {
            RefreshBindingDisplays();
            result = 0;
            return true;
        }
        if (id == IDC_LIST_BIND_WINDOWS && HIWORD(wp) == LBN_SELCHANGE)
        {
            result = 0;
            return true;
        }
        switch (id)
        {
        case IDC_CHK_ENABLED:
            if (g_ctx->onEnabledChanged)
                g_ctx->onEnabledChanged(SendMessageW(g_ctx->chkEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case IDC_UNLOCK_RESTORE: { Hotkey def{{ Modifier::Control }, 0}; ApplyDefaultGood(g_ctx->unlockEdit, g_ctx->cfg.unlock, def); break; }
        case IDC_UNLOCK_CLEAR: ClearGood(g_ctx->unlockEdit, g_ctx->cfg.unlock); break;
        case IDC_TOGGLELOCK_RESTORE: { Hotkey def{{ Modifier::Control, Modifier::Alt }, 'L'}; ApplyDefaultGood(g_ctx->toggleLockEdit, g_ctx->cfg.toggleLock, def); break; }
        case IDC_TOGGLELOCK_CLEAR: ClearGood(g_ctx->toggleLockEdit, g_ctx->cfg.toggleLock); break;
        case IDC_JUMPLEFT_RESTORE: { Hotkey def{{ Modifier::Control, Modifier::Alt }, 0x25}; ApplyDefaultGood(g_ctx->jumpLeftEdit, g_ctx->cfg.jumpLeft, def); break; }
        case IDC_JUMPLEFT_CLEAR: ClearGood(g_ctx->jumpLeftEdit, g_ctx->cfg.jumpLeft); break;
        case IDC_JUMPRIGHT_RESTORE: { Hotkey def{{ Modifier::Control, Modifier::Alt }, 0x27}; ApplyDefaultGood(g_ctx->jumpRightEdit, g_ctx->cfg.jumpRight, def); break; }
        case IDC_JUMPRIGHT_CLEAR: ClearGood(g_ctx->jumpRightEdit, g_ctx->cfg.jumpRight); break;
        case IDC_CHK_SILENT: g_ctx->cfg.startSilent = SendMessageW(g_ctx->chkSilent, BM_GETCHECK, 0, 0) == BST_CHECKED; Commit(); break;
        case IDC_CHK_AUTOSTART: g_ctx->cfg.autoStart = SendMessageW(g_ctx->chkAutoStart, BM_GETCHECK, 0, 0) == BST_CHECKED; Commit(); break;
        case IDC_CHK_CENTER: g_ctx->cfg.centerLock = SendMessageW(g_ctx->chkCenter, BM_GETCHECK, 0, 0) == BST_CHECKED; Commit(); break;
        case IDC_CHK_BINDING: g_ctx->cfg.foregroundBinding = SendMessageW(g_ctx->chkBinding, BM_GETCHECK, 0, 0) == BST_CHECKED; Commit(); break;
        case IDC_CHK_HIDE_TRAY_ICON: g_ctx->cfg.hideTrayIcon = SendMessageW(g_ctx->chkHideTrayIcon, BM_GETCHECK, 0, 0) == BST_CHECKED; Commit(); break;
        case IDC_BTN_REFRESH_BINDINGS: RefreshBindingLists(); break;
        case IDC_BTN_BIND_SELECTED: ApplyBindingSelections(false); break;
        case IDC_BTN_REMOVE_BINDINGS: ApplyBindingSelections(true); break;
        case IDC_BTN_CLEAR_BINDINGS:
            g_ctx->boundWindows.clear(); g_ctx->boundProcesses.clear();
            g_ctx->boundWindowPaths.clear(); g_ctx->boundProcessPaths.clear();
            RefreshBindingLists(); CommitBindings(); break;
        case IDC_CHK_SHOW_HIDDEN: RefreshBindingLists(); break;
        default: return false;
        }
        result = 0;
        return true;
    }
    case WM_SIZE:
        LayoutBindingControls(hwnd); result = 0; return true;
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize.x = kSettingsWindowMinWidth;
        info->ptMinTrackSize.y = kSettingsWindowMinHeight;
        result = 0;
        return true;
    }
    case WM_CTLCOLORBTN:
        if (!IsSettingsCheckboxControlId(GetDlgCtrlID(reinterpret_cast<HWND>(lp))))
            return false;
        [[fallthrough]];
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        result = (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        return true;
    }
    default:
        return false;
    }
}
