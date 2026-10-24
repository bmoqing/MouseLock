// 鼠标锁定核心实现。
#include "locker.h"
#include "logger.h"
#include "error_helpers.h"
#include "constants.h"

#include <vector>
#include <algorithm>

namespace
{
    struct MonitorData
    {
        HMONITOR handle = nullptr;
        RECT rect{};
    };

    BOOL CALLBACK EnumMonitorsProc(HMONITOR hMon, HDC, LPRECT rc, LPARAM data)
    {
        auto* list = reinterpret_cast<std::vector<MonitorData>*>(data);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi))
            list->push_back({ hMon, mi.rcMonitor });
        return TRUE;
    }

    std::vector<MonitorData> GetMonitors()
    {
        std::vector<MonitorData> list;
        EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, reinterpret_cast<LPARAM>(&list));
        std::sort(list.begin(), list.end(), [](const MonitorData& a, const MonitorData& b) {
            if (a.rect.left != b.rect.left)
                return a.rect.left < b.rect.left;
            return a.rect.top < b.rect.top;
        });
        return list;
    }

    bool SameRect(const RECT& a, const RECT& b)
    {
        return a.left == b.left && a.top == b.top &&
               a.right == b.right && a.bottom == b.bottom;
    }

    bool IsMouseButtonDown()
    {
        constexpr int buttons[] = {
            VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2
        };
        for (int button : buttons)
        {
            if ((GetAsyncKeyState(button) & kAsyncKeyStatePressedMask) != 0)
                return true;
        }
        return false;
    }

    bool IsClipCursorSetTo(const RECT& expected)
    {
        RECT current{};
        return GetClipCursor(&current) && SameRect(current, expected);
    }
}

bool Locker::LockToCurrentScreen(bool center)
{
    POINT pt{};
    if (!GetCursorPos(&pt))
        return false;
    return LockToScreenAt(pt.x, pt.y, center);
}

bool Locker::LockToScreenAt(int x, int y, bool center)
{
    POINT pt{ x, y };
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!hMon)
    {
        LOG_WARN(L"MonitorFromPoint 失败：找不到包含坐标 (%d, %d) 的显示器", x, y);
        return false;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!CheckWin32(GetMonitorInfoW(hMon, &mi), L"GetMonitorInfoW"))
        return false;

    RECT rect = mi.rcMonitor;

    if (center)
    {
        int cx = rect.left + (rect.right - rect.left) / 2;
        int cy = rect.top + (rect.bottom - rect.top) / 2;
        rect = { cx, cy, cx + 1, cy + 1 };

        // Foreground events can request the same lock repeatedly. Avoid moving
        // the cursor again when the existing centered lock is still valid.
        if (m_locked && m_centerLocked && SameRect(m_lockedRect, rect) &&
            IsClipCursorSetTo(rect))
            return true;

        if (!CheckWin32Warn(SetCursorPos(cx, cy), L"SetCursorPos"))
            return false;
    }

    // Reapplying the same clip rectangle is observable by some applications
    // as cursor activity. Keep a stable lock stable across timer/event ticks.
    if (!center && m_locked && !m_centerLocked && SameRect(m_lockedRect, rect) &&
        IsClipCursorSetTo(rect))
        return true;

    if (!CheckWin32(ClipCursor(&rect), L"ClipCursor"))
        return false;

    m_lockedRect = rect;
    m_locked = true;
    m_centerLocked = center;
    return true;
}

bool Locker::Unlock()
{
    if (!CheckWin32Warn(ClipCursor(nullptr), L"ClipCursor 解锁"))
        return false;

    m_locked = false;
    m_centerLocked = false;
    m_lockedRect = {};
    return true;
}

bool Locker::Jump(int direction)
{
    auto monitors = GetMonitors();
    if (monitors.size() < 2)
        return false;

    POINT pt{};
    if (!CheckWin32Warn(GetCursorPos(&pt), L"Jump: GetCursorPos"))
        return false;

    HMONITOR hCur = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

    int idx = -1;
    for (size_t i = 0; i < monitors.size(); ++i)
        if (monitors[i].handle == hCur) { idx = (int)i; break; }
    if (idx < 0)
        return false;

    int n = (int)monitors.size();
    int target = (idx + direction) % n;
    if (target < 0)
        target += n;

    const RECT& r = monitors[target].rect;
    int cx = r.left + (r.right - r.left) / 2;
    int cy = r.top + (r.bottom - r.top) / 2;
    if (!CheckWin32Warn(SetCursorPos(cx, cy), L"Jump: SetCursorPos"))
        return false;

    return LockToScreenAt(cx, cy, m_centerLocked);
}

void Locker::Revalidate(bool forceCursorCorrection, const POINT* observedCursor)
{
    if (!m_locked)
        return;

    POINT cursor{};
    if (observedCursor)
        cursor = *observedCursor;
    else if (!CheckWin32Warn(GetCursorPos(&cursor), L"Revalidate: GetCursorPos"))
        return;

    bool inside = cursor.x >= m_lockedRect.left && cursor.x < m_lockedRect.right &&
        cursor.y >= m_lockedRect.top && cursor.y < m_lockedRect.bottom;
    bool clipChanged = !IsClipCursorSetTo(m_lockedRect);
    if (inside && !clipChanged)
        return;

    // 快速移动时，系统可能先报告一个已经越过边界的坐标。
    // 只有确实越界才校正坐标，避免正常点击和拖动期间产生额外光标动作。
    if (!inside && (forceCursorCorrection || clipChanged))
    {
        POINT corrected{
            cursor.x < m_lockedRect.left ? m_lockedRect.left :
                (cursor.x >= m_lockedRect.right ? m_lockedRect.right - 1 : cursor.x),
            cursor.y < m_lockedRect.top ? m_lockedRect.top :
                (cursor.y >= m_lockedRect.bottom ? m_lockedRect.bottom - 1 : cursor.y)
        };
        if (!CheckWin32Warn(SetCursorPos(corrected.x, corrected.y),
                L"Revalidate: SetCursorPos"))
            return;
    }

    // 区域被外部程序清除或替换时重新应用；只有区域实际变化才调用。
    if (clipChanged &&
        !CheckWin32Warn(ClipCursor(&m_lockedRect), L"Revalidate: ClipCursor"))
        return;
}
