#pragma once

#include <windows.h>
#include <functional>

constexpr LONG kLockPositionPickerReticlePaintMargin = 22;

inline RECT GetLockPositionPickerReticleDamageRect(POINT center)
{
    return RECT{
        center.x - kLockPositionPickerReticlePaintMargin,
        center.y - kLockPositionPickerReticlePaintMargin,
        center.x + kLockPositionPickerReticlePaintMargin,
        center.y + kLockPositionPickerReticlePaintMargin
    };
}

inline bool ClearLockPositionPickerPaintSurface(HDC dc, const RECT& client)
{
    if (!dc)
        return false;
    return FillRect(dc, &client,
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))) != 0;
}

class LockPositionPicker
{
public:
    using Callback = std::function<void(bool, POINT)>;

    bool Start(HWND owner, HINSTANCE instance, Callback callback);
    bool IsActive() const { return m_hwnd != nullptr; }
    void Cancel();
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    void Finish(bool selected, POINT point);
    void Paint(HWND hwnd);

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    HPEN m_reticlePen = nullptr;
    Callback m_callback;
    bool m_mouseDown = false;
    POINT m_selectedPoint{};
    POINT m_cursorClientPosition{};
    POINT m_lastPaintedCursorPosition{};
    bool m_hasCursorClientPosition = false;
    bool m_hasPaintedCursorPosition = false;
};
