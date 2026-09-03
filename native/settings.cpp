// 设置对话框实现：程序化创建控件，快捷键录入框采用子类化捕获按键。
#include "settings.h"

#include <string>
#include <functional>
#include "resource.h"

namespace
{
    struct SettingsCtx
    {
        AppConfig cfg;
        HWND hwnd = nullptr;
        HWND unlockEdit = nullptr;
        HWND jumpLeftEdit = nullptr;
        HWND jumpRightEdit = nullptr;
        HWND chkSilent = nullptr;
        HWND chkAutoStart = nullptr;
        std::function<void(const AppConfig&)> onChanged;
    };

    SettingsCtx* g_ctx = nullptr;
    WNDPROC g_origHotkeyEditProc = nullptr;
    HFONT g_boldFont = nullptr;

    const wchar_t* kSettingsClass = L"MouseLockSettings";

    bool IsModifierVk(int vk)
    {
        return vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL ||
               vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU ||
               vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT;
    }

    bool IsKeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    std::vector<Modifier> GetPressedModifiers()
    {
        std::vector<Modifier> mods;
        if (IsKeyDown(VK_LCONTROL) || IsKeyDown(VK_RCONTROL)) mods.push_back(Modifier::Control);
        if (IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU)) mods.push_back(Modifier::Alt);
        if (IsKeyDown(VK_LSHIFT) || IsKeyDown(VK_RSHIFT)) mods.push_back(Modifier::Shift);
        return mods;
    }

    void UpdateEditText(HWND edit, const Hotkey& hk)
    {
        SetWindowTextW(edit, FormatHotkey(hk).c_str());
    }

    // 将当前配置实时提交（写回并立即生效）
    void Commit()
    {
        if (g_ctx && g_ctx->onChanged)
            g_ctx->onChanged(g_ctx->cfg);
    }

    // 快捷键录入框子类过程
    LRESULT CALLBACK HotkeyEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_GETDLGCODE)
            return DLGC_WANTALLKEYS;
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
                    hk->key = (int)wp;
                    hk->modifiers = GetPressedModifiers();
                    UpdateEditText(hwnd, *hk);
                    Commit();
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
        return CallWindowProcW(g_origHotkeyEditProc, hwnd, msg, wp, lp);
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

    void ApplyDefaultGood(HWND edit, Hotkey& hk, const Hotkey& def)
    {
        hk = def;
        UpdateEditText(edit, hk);
        Commit();
    }

    void ClearGood(HWND edit, Hotkey& hk)
    {
        hk.key = -1;
        hk.modifiers.clear();
        UpdateEditText(edit, hk);
        Commit();
    }

    LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            CreateLabel(hwnd, L"临时解锁键（长按）：", 16, 22, false);
            CreateLabel(hwnd, L"跳转到上一屏幕：", 16, 58, false);
            CreateLabel(hwnd, L"跳转到下一屏幕：", 16, 94, false);
            CreateLabel(hwnd, L"启动选项", 16, 122, true);

            g_ctx->unlockEdit = CreateEdit(hwnd, 150, 18, 196, 23, &g_ctx->cfg.unlock);
            g_ctx->jumpLeftEdit = CreateEdit(hwnd, 150, 54, 196, 23, &g_ctx->cfg.jumpLeft);
            g_ctx->jumpRightEdit = CreateEdit(hwnd, 150, 90, 196, 23, &g_ctx->cfg.jumpRight);

            CreateButton(hwnd, L"↻", 352, 18, 26, 23, IDC_UNLOCK_RESTORE);
            CreateButton(hwnd, L"✕", 382, 18, 26, 23, IDC_UNLOCK_CLEAR);
            CreateButton(hwnd, L"↻", 352, 54, 26, 23, IDC_JUMPLEFT_RESTORE);
            CreateButton(hwnd, L"✕", 382, 54, 26, 23, IDC_JUMPLEFT_CLEAR);
            CreateButton(hwnd, L"↻", 352, 90, 26, 23, IDC_JUMPRIGHT_RESTORE);
            CreateButton(hwnd, L"✕", 382, 90, 26, 23, IDC_JUMPRIGHT_CLEAR);

            g_ctx->chkSilent = CreateCheck(hwnd, L"启动时静默启动到托盘（不显示主窗口）", 16, 146, IDC_CHK_SILENT);
            g_ctx->chkAutoStart = CreateCheck(hwnd, L"开机自动启动", 16, 174, IDC_CHK_AUTOSTART);

            UpdateEditText(g_ctx->unlockEdit, g_ctx->cfg.unlock);
            UpdateEditText(g_ctx->jumpLeftEdit, g_ctx->cfg.jumpLeft);
            UpdateEditText(g_ctx->jumpRightEdit, g_ctx->cfg.jumpRight);
            SendMessageW(g_ctx->chkSilent, BM_SETCHECK, g_ctx->cfg.startSilent ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(g_ctx->chkAutoStart, BM_SETCHECK, g_ctx->cfg.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
            return 0;
        }

        case WM_COMMAND:
        {
            int id = LOWORD(wp);
            switch (id)
            {
            case IDC_UNLOCK_RESTORE:
            {
                Hotkey def{ { Modifier::Control }, 0 };
                ApplyDefaultGood(g_ctx->unlockEdit, g_ctx->cfg.unlock, def);
                break;
            }
            case IDC_UNLOCK_CLEAR:
                ClearGood(g_ctx->unlockEdit, g_ctx->cfg.unlock);
                break;
            case IDC_JUMPLEFT_RESTORE:
            {
                Hotkey def{ { Modifier::Control, Modifier::Alt }, 0x25 };
                ApplyDefaultGood(g_ctx->jumpLeftEdit, g_ctx->cfg.jumpLeft, def);
                break;
            }
            case IDC_JUMPLEFT_CLEAR:
                ClearGood(g_ctx->jumpLeftEdit, g_ctx->cfg.jumpLeft);
                break;
            case IDC_JUMPRIGHT_RESTORE:
            {
                Hotkey def{ { Modifier::Control, Modifier::Alt }, 0x27 };
                ApplyDefaultGood(g_ctx->jumpRightEdit, g_ctx->cfg.jumpRight, def);
                break;
            }
            case IDC_JUMPRIGHT_CLEAR:
                ClearGood(g_ctx->jumpRightEdit, g_ctx->cfg.jumpRight);
                break;
            case IDC_CHK_SILENT:
                g_ctx->cfg.startSilent = SendMessageW(g_ctx->chkSilent, BM_GETCHECK, 0, 0) == BST_CHECKED;
                Commit();
                break;
            case IDC_CHK_AUTOSTART:
                g_ctx->cfg.autoStart = SendMessageW(g_ctx->chkAutoStart, BM_GETCHECK, 0, 0) == BST_CHECKED;
                Commit();
                break;
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wp;
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }

        case WM_DESTROY:
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void ShowSettingsDialog(HWND parent, const AppConfig& current,
                        const std::function<void(const AppConfig&)>& onChanged)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kSettingsClass;
    RegisterClassW(&wc);

    g_boldFont = CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    // 保存原始编辑控件过程（用于子类链）
    WNDCLASSEXW wce{};
    wce.cbSize = sizeof(wce);
    GetClassInfoExW(inst, L"EDIT", &wce);
    g_origHotkeyEditProc = wce.lpfnWndProc;

    SettingsCtx ctx;
    ctx.cfg = current;
    ctx.onChanged = onChanged;
    g_ctx = &ctx;

    RECT rc{ 0, 0, 430, 204 };
    DWORD style = WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kSettingsClass, L"鼠标锁定 - 设置", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        parent, nullptr, inst, nullptr);
    if (!hwnd)
    {
        DeleteObject(g_boldFont);
        g_boldFont = nullptr;
        g_ctx = nullptr;
        return;
    }

    // 窗口定位到屏幕中央
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, nullptr, (sw - (rc.right - rc.left)) / 2, (sh - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    EnableWindow(parent, FALSE);
    SetFocus(hwnd);

    MSG m;
    while (IsWindow(hwnd))
    {
        if (GetMessageW(&m, nullptr, 0, 0))
        {
            if (!IsDialogMessageW(hwnd, &m))
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }

    EnableWindow(parent, TRUE);
    SetActiveWindow(parent);

    DeleteObject(g_boldFont);
    g_boldFont = nullptr;
    g_ctx = nullptr;
}