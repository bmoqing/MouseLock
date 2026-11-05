#pragma once

#include "config.h"

namespace mouse_lock
{
    inline bool IsUsableUnlockHotkey(const Hotkey& hotkey)
    {
        return IsSet(hotkey) &&
            (hotkey.key > 0 || (hotkey.key == 0 && !hotkey.modifiers.empty()));
    }

    inline bool IsUsableToggleHotkey(const Hotkey& hotkey)
    {
        return IsSet(hotkey) && hotkey.key > 0;
    }

    inline bool CanEnableLock(const AppConfig& config)
    {
        return !config.centerLock ||
            IsUsableUnlockHotkey(config.unlock) ||
            IsUsableToggleHotkey(config.toggleLock);
    }
}
