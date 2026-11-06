#include "../lock_safety.h"
#include "../hotkey_validation.h"

#include <cstdio>

namespace
{
    AppConfig EmptyShortcuts()
    {
        AppConfig config;
        config.unlock.key = -1;
        config.toggleLock.key = -1;
        config.jumpLeft.key = -1;
        config.jumpRight.key = -1;
        return config;
    }

    bool TestCenterLockRequiresAnExitHotkey()
    {
        AppConfig config = EmptyShortcuts();
        config.centerLock = true;
        return !mouse_lock::CanEnableLock(config);
    }

    bool TestNonCenterLockDoesNotRequireAnExitHotkey()
    {
        AppConfig config = EmptyShortcuts();
        return mouse_lock::CanEnableLock(config);
    }

    bool TestEitherUnlockOrToggleHotkeyIsEnough()
    {
        AppConfig config = EmptyShortcuts();
        config.centerLock = true;
        config.unlock.modifiers = { Modifier::Control };
        config.unlock.key = 0;
        if (!mouse_lock::CanEnableLock(config))
            return false;

        config.unlock.key = -1;
        config.unlock.modifiers.clear();
        config.toggleLock.modifiers = { Modifier::Control, Modifier::Alt };
        config.toggleLock.key = 'L';
        return mouse_lock::CanEnableLock(config);
    }

    bool TestJumpHotkeysDoNotCountAsAnExitHotkey()
    {
        AppConfig config = EmptyShortcuts();
        config.centerLock = true;
        config.jumpLeft.modifiers = { Modifier::Control, Modifier::Alt };
        config.jumpLeft.key = 0x25;
        config.jumpRight.modifiers = { Modifier::Control, Modifier::Alt };
        config.jumpRight.key = 0x27;
        return !mouse_lock::CanEnableLock(config);
    }

    bool TestModifierlessEmptyHotkeyDoesNotCountAsConfigured()
    {
        AppConfig config = EmptyShortcuts();
        config.centerLock = true;
        config.unlock.key = 0;
        return !mouse_lock::CanEnableLock(config);
    }

    bool TestModifierOnlyToggleHotkeyDoesNotCountAsConfigured()
    {
        AppConfig config = EmptyShortcuts();
        config.centerLock = true;
        config.toggleLock.modifiers = { Modifier::Control };
        config.toggleLock.key = 0;
        return !mouse_lock::CanEnableLock(config);
    }

    bool TestInvalidNegativeHotkeyDoesNotCountAsConfigured()
    {
        AppConfig config = EmptyShortcuts();
        config.centerLock = true;
        config.unlock.modifiers = { Modifier::Control };
        config.unlock.key = -2;
        return !mouse_lock::CanEnableLock(config);
    }

    bool TestDuplicateHotkeyIsRejectedAcrossActions()
    {
        AppConfig config;
        config.unlock.modifiers = { Modifier::Control, Modifier::Alt };
        config.unlock.key = 'I';
        Hotkey candidate{{ Modifier::Alt, Modifier::Control }, 'I'};
        return mouse_lock::ConflictsWithOtherHotkey(
            config, mouse_lock::HotkeyAction::ToggleLock, candidate);
    }

    bool TestDifferentHotkeysAndUnsetHotkeysDoNotConflict()
    {
        AppConfig config;
        config.unlock.modifiers = { Modifier::Control };
        config.unlock.key = 'I';
        Hotkey different{{ Modifier::Control }, 'J'};
        Hotkey unset;
        unset.key = -1;
        return !mouse_lock::ConflictsWithOtherHotkey(
                config, mouse_lock::HotkeyAction::ToggleLock, different) &&
            !mouse_lock::ConflictsWithOtherHotkey(
                config, mouse_lock::HotkeyAction::ToggleLock, unset);
    }

    bool TestHotkeyDoesNotConflictWithItsOwnAction()
    {
        AppConfig config;
        config.jumpRight.modifiers = { Modifier::Control, Modifier::Alt };
        config.jumpRight.key = 0x27;
        return !mouse_lock::ConflictsWithOtherHotkey(
            config, mouse_lock::HotkeyAction::JumpRight, config.jumpRight);
    }
}

int main()
{
    if (!TestCenterLockRequiresAnExitHotkey() ||
        !TestNonCenterLockDoesNotRequireAnExitHotkey() ||
        !TestEitherUnlockOrToggleHotkeyIsEnough() ||
        !TestJumpHotkeysDoNotCountAsAnExitHotkey() ||
        !TestModifierlessEmptyHotkeyDoesNotCountAsConfigured() ||
        !TestModifierOnlyToggleHotkeyDoesNotCountAsConfigured() ||
        !TestInvalidNegativeHotkeyDoesNotCountAsConfigured() ||
        !TestDuplicateHotkeyIsRejectedAcrossActions() ||
        !TestDifferentHotkeysAndUnsetHotkeysDoNotConflict() ||
        !TestHotkeyDoesNotConflictWithItsOwnAction())
    {
        std::fprintf(stderr, "FAIL: center locking must retain a usable exit hotkey\n");
        return 1;
    }

    std::puts("PASS: center-lock exit hotkey safety");
    return 0;
}
