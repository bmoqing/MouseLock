#pragma once

#include <windows.h>
#include <algorithm>

enum class LockMode
{
    ScreenBounds = 0,
    Center = 1,
    Custom = 2
};

struct NormalizedLockPoint
{
    int x = 5000;
    int y = 5000;
};

constexpr int kLockPositionScale = 10000;

inline int ClampLockPosition(long long value)
{
    return static_cast<int>((std::max)(0LL,
        (std::min)(static_cast<long long>(kLockPositionScale), value)));
}

inline POINT MapNormalizedLockPoint(const RECT& bounds,
                                    const NormalizedLockPoint& point)
{
    const long long width = (std::max)(1L, bounds.right - bounds.left);
    const long long height = (std::max)(1L, bounds.bottom - bounds.top);
    const long long x = ((width - 1) * ClampLockPosition(point.x) +
        kLockPositionScale / 2) / kLockPositionScale;
    const long long y = ((height - 1) * ClampLockPosition(point.y) +
        kLockPositionScale / 2) / kLockPositionScale;
    return POINT{ bounds.left + static_cast<LONG>(x),
                  bounds.top + static_cast<LONG>(y) };
}

inline NormalizedLockPoint NormalizeLockPoint(const RECT& bounds, POINT point)
{
    const long long width = (std::max)(1L, bounds.right - bounds.left);
    const long long height = (std::max)(1L, bounds.bottom - bounds.top);
    const long long x = (std::max)(0LL, (std::min)(width - 1,
        static_cast<long long>(point.x) - bounds.left));
    const long long y = (std::max)(0LL, (std::min)(height - 1,
        static_cast<long long>(point.y) - bounds.top));
    const long long xSpan = (std::max)(1LL, width - 1);
    const long long ySpan = (std::max)(1LL, height - 1);
    return NormalizedLockPoint{
        static_cast<int>((x * kLockPositionScale + xSpan / 2) / xSpan),
        static_cast<int>((y * kLockPositionScale + ySpan / 2) / ySpan)
    };
}
