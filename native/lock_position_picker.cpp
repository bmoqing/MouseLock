#include "lock_position_picker.h"

#include <string>
#include <utility>
#include <windowsx.h>

namespace
{
    constexpr wchar_t kPickerClassName[] = L"MouseLockPositionPicker";

    bool RegisterPickerClass(HINSTANCE instance)
    {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (GetClassInfoExW(instance, kPickerClassName, &existing))
            return true;

        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.hInstance = instance;
        cls.lpfnWndProc = LockPositionPicker::WindowProc;
        cls.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        cls.lpszClassName = kPickerClassName;
        return RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
}

bool LockPositionPicker::Start(HWND owner, HINSTANCE instance, Callback callback)
{
    if (m_hwnd || !callback || !RegisterPickerClass(instance))
        return false;

    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0)
        return false;

    m_owner = owner;
    m_callback = std::move(callback);
    m_hasCursorClientPosition = false;
    m_hasPaintedCursorPosition = false;
    m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kPickerClassName, L"选择鼠标锁定位置", WS_POPUP,
        left, top, width, height, owner, nullptr, instance, this);
    if (!m_hwnd)
    {
        m_owner = nullptr;
        m_callback = {};
        return false;
    }

    m_reticlePen = CreatePen(PS_SOLID, 2, RGB(255, 72, 72));
    if (!m_reticlePen)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        m_owner = nullptr;
        m_callback = {};
        return false;
    }

    SetLayeredWindowAttributes(m_hwnd, 0, 150, LWA_ALPHA);
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    SetCapture(m_hwnd);
    return true;
}

void LockPositionPicker::Cancel()
{
    if (m_hwnd)
        Finish(false, POINT{});
}

void LockPositionPicker::Finish(bool selected, POINT point)
{
    HWND hwnd = m_hwnd;
    m_hwnd = nullptr;
    if (hwnd)
    {
        if (GetCapture() == hwnd)
            ReleaseCapture();
        DestroyWindow(hwnd);
    }

    Callback callback = std::move(m_callback);
    m_callback = {};
    m_owner = nullptr;
    m_mouseDown = false;
    m_hasCursorClientPosition = false;
    m_hasPaintedCursorPosition = false;
    if (m_reticlePen)
    {
        DeleteObject(m_reticlePen);
        m_reticlePen = nullptr;
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (callback)
        callback(selected, point);
}

void LockPositionPicker::Paint(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    ClearLockPositionPickerPaintSurface(dc, client);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT instruction = client;
    instruction.top += 28;
    DrawTextW(dc, L"移动准星到目标位置并单击确认，按 Esc 取消",
        -1, &instruction, DT_CENTER | DT_TOP | DT_SINGLELINE);

    POINT cursor{};
    bool hasCursor = m_hasCursorClientPosition;
    if (hasCursor)
        cursor = m_cursorClientPosition;
    else
    {
        hasCursor = GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor);
        if (hasCursor)
        {
            m_cursorClientPosition = cursor;
            m_hasCursorClientPosition = true;
        }
    }

    if (hasCursor)
    {
        const int x = cursor.x;
        const int y = cursor.y;
        HGDIOBJ oldPen = SelectObject(dc, m_reticlePen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Ellipse(dc, x - 10, y - 10, x + 11, y + 11);
        MoveToEx(dc, x - 18, y, nullptr);
        LineTo(dc, x - 5, y);
        MoveToEx(dc, x + 5, y, nullptr);
        LineTo(dc, x + 18, y);
        MoveToEx(dc, x, y - 18, nullptr);
        LineTo(dc, x, y - 5);
        MoveToEx(dc, x, y + 5, nullptr);
        LineTo(dc, x, y + 18);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        m_lastPaintedCursorPosition = cursor;
        m_hasPaintedCursorPosition = true;
    }

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK LockPositionPicker::WindowProc(HWND hwnd, UINT msg,
                                                 WPARAM wp, LPARAM lp)
{
    auto* picker = reinterpret_cast<LockPositionPicker*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        picker = static_cast<LockPositionPicker*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(picker));
    }
    if (!picker)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        picker->Paint(hwnd);
        return 0;
    case WM_MOUSEMOVE:
    {
        const POINT cursor{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (picker->m_hasPaintedCursorPosition &&
            cursor.x == picker->m_lastPaintedCursorPosition.x &&
            cursor.y == picker->m_lastPaintedCursorPosition.y)
            return 0;

        if (picker->m_hasPaintedCursorPosition)
        {
            const RECT oldReticle = GetLockPositionPickerReticleDamageRect(
                picker->m_lastPaintedCursorPosition);
            InvalidateRect(hwnd, &oldReticle, FALSE);
        }
        const RECT newReticle = GetLockPositionPickerReticleDamageRect(cursor);
        InvalidateRect(hwnd, &newReticle, FALSE);
        picker->m_cursorClientPosition = cursor;
        picker->m_hasCursorClientPosition = true;
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        picker->m_mouseDown = GetCursorPos(&picker->m_selectedPoint) != FALSE;
        return 0;
    }
    case WM_LBUTTONUP:
        if (picker->m_mouseDown)
        {
            POINT point{};
            if (GetCursorPos(&point))
                picker->Finish(true, point);
            else
                picker->Finish(false, POINT{});
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (picker->m_hwnd == hwnd)
            picker->Finish(false, POINT{});
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
        {
            picker->Finish(false, POINT{});
            return 0;
        }
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
