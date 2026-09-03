// 鼠标锁定核心实现。
#include "locker.h"

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
}

bool Locker::LockToCurrentScreen()
{
    POINT pt{};
    if (!GetCursorPos(&pt))
        return false;
    return LockToScreenAt(pt.x, pt.y);
}

bool Locker::LockToScreenAt(int x, int y)
{
    POINT pt{ x, y };
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!hMon)
        return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi))
        return false;

    RECT rect = mi.rcMonitor;
    if (!ClipCursor(&rect))
        return false;

    m_lockedRect = rect;
    m_locked = true;
    return true;
}

void Locker::Unlock()
{
    ClipCursor(nullptr);
    m_locked = false;
}

bool Locker::Jump(int direction)
{
    auto monitors = GetMonitors();
    if (monitors.size() < 2)
        return false;

    POINT pt{};
    if (!GetCursorPos(&pt))
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
    SetCursorPos(cx, cy);

    return LockToScreenAt(cx, cy);
}

void Locker::Revalidate()
{
    if (!m_locked)
        return;

    POINT pt{};
    if (!GetCursorPos(&pt))
        return;

    const RECT& r = m_lockedRect;
    bool inside = pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
    if (inside)
        return;

    int x = pt.x < r.left ? r.left : (pt.x > r.right - 1 ? r.right - 1 : pt.x);
    int y = pt.y < r.top ? r.top : (pt.y > r.bottom - 1 ? r.bottom - 1 : pt.y);
    SetCursorPos(x, y);
    RECT rect = r;
    ClipCursor(&rect);
}