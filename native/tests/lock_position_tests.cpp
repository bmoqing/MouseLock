#include "../lock_position_picker.h"
#include "../lock_position.h"
#include "../config.h"

#include <cstdio>

namespace
{
    bool TestNormalizedPointMapsAcrossMonitorSizes()
    {
        RECT source{ -1920, 0, 0, 1080 };
        RECT target{ 0, -120, 2560, 1320 };
        POINT picked{ -480, 810 };
        const NormalizedLockPoint normalized = NormalizeLockPoint(source, picked);
        const POINT mapped = MapNormalizedLockPoint(target, normalized);
        return normalized.x >= 7500 && normalized.x <= 7505 &&
            normalized.y >= 7500 && normalized.y <= 7510 &&
            mapped.x == 1920 && mapped.y == 960;
    }

    bool TestNormalizedPointClampsToMonitorBounds()
    {
        RECT monitor{ 100, 200, 500, 600 };
        POINT outside{ 900, -100 };
        const NormalizedLockPoint normalized = NormalizeLockPoint(monitor, outside);
        const POINT mapped = MapNormalizedLockPoint(monitor, normalized);
        return normalized.x == 10000 && normalized.y == 0 &&
            mapped.x == 499 && mapped.y == 200;
    }

    bool TestPickedPositionPreservesLockOptions()
    {
        AppConfig current;
        current.enabled = true;
        current.lockMode = LockMode::ScreenBounds;
        const RECT monitor{ 0, 0, 100, 100 };
        const POINT picked{ 25, 75 };
        const AppConfig updated = WithPickedLockPosition(current, monitor, picked);
        const NormalizedLockPoint expected = NormalizeLockPoint(monitor, picked);
        return updated.enabled == current.enabled &&
            updated.lockMode == current.lockMode &&
            updated.lockPoint.x == expected.x &&
            updated.lockPoint.y == expected.y;
    }

    bool TestPickerPaintClearsPreviousReticle()
    {
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc)
            return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = 16;
        info.bmiHeader.biHeight = -16;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
            &bits, nullptr, 0);
        if (!bitmap)
        {
            DeleteDC(dc);
            return false;
        }

        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        SetPixelV(dc, 4, 4, RGB(255, 72, 72));
        RECT client{ 0, 0, 16, 16 };
        const bool cleared = ClearLockPositionPickerPaintSurface(dc, client) &&
            GetPixel(dc, 4, 4) == RGB(0, 0, 0);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        return cleared;
    }

    bool TestReticleDamageAreaIsBounded()
    {
        const RECT damage = GetLockPositionPickerReticleDamageRect(POINT{ 120, 80 });
        return damage.left == 98 && damage.top == 58 &&
            damage.right == 142 && damage.bottom == 102 &&
            (damage.right - damage.left) * (damage.bottom - damage.top) == 1936;
    }
}

int main()
{
    if (!TestNormalizedPointMapsAcrossMonitorSizes() ||
        !TestNormalizedPointClampsToMonitorBounds())
    {
        std::fprintf(stderr, "FAIL: normalized lock point mapping\n");
        return 1;
    }

    if (!TestPickedPositionPreservesLockOptions())
    {
        std::fprintf(stderr,
            "FAIL: saving a picked position must preserve lock options\n");
        return 1;
    }

    if (!TestPickerPaintClearsPreviousReticle())
    {
        std::fprintf(stderr,
            "FAIL: picker repaint must clear the previous reticle\n");
        return 1;
    }

    if (!TestReticleDamageAreaIsBounded())
    {
        std::fprintf(stderr,
            "FAIL: picker movement should invalidate only the reticle area\n");
        return 1;
    }

    std::puts("PASS: normalized points and picker rendering behavior");
    return 0;
}
