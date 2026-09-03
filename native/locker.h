// 鼠标锁定核心：ClipCursor 锁定/解锁、屏幕跳转、备用校验。
#pragma once

#include <windows.h>

class Locker
{
public:
    bool IsLocked() const { return m_locked; }

    // 锁定到光标当前所在屏幕
    bool LockToCurrentScreen();

    // 锁定到包含指定物理坐标的屏幕
    bool LockToScreenAt(int x, int y);

    // 解除锁定
    void Unlock();

    // 跳转到相邻屏幕（-1 上一个，+1 下一个）
    bool Jump(int direction);

    // 备用校验：光标越界时夹回并重新裁剪
    void Revalidate();

private:
    bool m_locked = false;
    RECT m_lockedRect{};
};