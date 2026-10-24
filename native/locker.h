// 鼠标锁定核心：ClipCursor 锁定/解锁、屏幕跳转、备用校验。
#pragma once

#include <windows.h>

class Locker
{
public:
    bool IsLocked() const { return m_locked; }

    // 锁定到光标当前所在屏幕
    bool LockToCurrentScreen(bool center = false);

    // 锁定到包含指定物理坐标的屏幕
    bool LockToScreenAt(int x, int y, bool center = false);

    // 解除锁定；只有系统成功取消光标裁剪时才清除内部锁定状态
    bool Unlock();

    // 跳转到相邻屏幕（-1 上一个，+1 下一个）
    bool Jump(int direction);

    // 备用校验：裁剪区域被外部修改或光标越界时立即恢复。
    // 鼠标按键按下期间也允许恢复，避免快速移动时穿过屏幕边界。
    void Revalidate(bool forceCursorCorrection = false, const POINT* observedCursor = nullptr);

private:
    bool m_locked = false;
    bool m_centerLocked = false;
    RECT m_lockedRect{};
};
