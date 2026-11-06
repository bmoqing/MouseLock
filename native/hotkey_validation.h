#pragma once

#include "config.h"

namespace mouse_lock
{
    enum class HotkeyAction
    {
        Unlock,
        ToggleLock,
        JumpLeft,
        JumpRight
    };

    inline const Hotkey& HotkeyForAction(const AppConfig& config, HotkeyAction action)
    {
        switch (action)
        {
        case HotkeyAction::Unlock: return config.unlock;
        case HotkeyAction::ToggleLock: return config.toggleLock;
        case HotkeyAction::JumpLeft: return config.jumpLeft;
        case HotkeyAction::JumpRight: return config.jumpRight;
        }
        return config.unlock;
    }

    inline bool SameHotkey(const Hotkey& left, const Hotkey& right)
    {
        if (left.key != right.key)
            return false;

        for (Modifier modifier : left.modifiers)
        {
            bool found = false;
            for (Modifier other : right.modifiers)
            {
                if (modifier == other)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }

        for (Modifier modifier : right.modifiers)
        {
            bool found = false;
            for (Modifier other : left.modifiers)
            {
                if (modifier == other)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
        return true;
    }

    inline bool ConflictsWithOtherHotkey(const AppConfig& config,
                                         HotkeyAction action,
                                         const Hotkey& candidate)
    {
        if (!IsSet(candidate))
            return false;

        const HotkeyAction actions[] = {
            HotkeyAction::Unlock,
            HotkeyAction::ToggleLock,
            HotkeyAction::JumpLeft,
            HotkeyAction::JumpRight
        };
        for (HotkeyAction otherAction : actions)
        {
            const Hotkey& other = HotkeyForAction(config, otherAction);
            if (otherAction != action && IsSet(other) && SameHotkey(candidate, other))
                return true;
        }
        return false;
    }
}
