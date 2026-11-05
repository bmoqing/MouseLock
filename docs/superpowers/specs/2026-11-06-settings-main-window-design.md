# Settings Window as the Main Window

## Goal

Make the existing settings interface the application's only main window. The
window opened by double-clicking `MouseLock.exe` will contain the lock toggle
and all existing settings. Remove the separate compact main window.

## Window and Lifecycle

- Convert the current settings window from a modal dialog with its own message
  loop into a normal application window managed by the existing main message
  loop.
- Preserve the settings controls, live configuration saves, binding selection,
  and current lock behavior.
- Move "Enable mouse locking" into the main settings window and connect it to
  the existing `SetEnabled` flow, including center-lock exit-hotkey validation.
- Remove the compact main-window controls, its Settings button, and the
  separate `OpenSettings` flow.
- Keep a single window handle as the owner of tray callbacks, timers, hooks,
  close behavior, and single-instance restoration.

## Startup and Tray Behavior

- Normal startup shows the settings main window.
- Silent startup keeps the window hidden and retains the existing tray
  notification behavior.
- Tray double-click, tray Settings, and a second process launch restore the
  same main window; a minimized window must be unminimized.
- Closing while the tray icon is visible hides the window. Closing while the
  tray icon is configured hidden exits the application.
- Keep the existing tray lock toggle, monitor switching, binding commands,
  and exit command.

## Hotkey Capture

- Preserve keyboard and mouse side-button shortcut recording, Escape-to-clear,
  modifier-only temporary unlock shortcuts, and immediate persistence.
- While a hotkey edit control has focus, global lock-toggle and monitor-jump
  shortcut matching/swallowing must be suppressed so the captured chord is
  recorded rather than executed.
- Resume global shortcut handling when focus leaves hotkey capture.
- Preserve temporary-unlock state reconciliation while the main window is
  open; window visibility must not be used as a substitute for capture focus.

## Validation

- Add focused tests for hotkey-capture suppression and restoration.
- Verify the settings window is the sole visible application window on normal
  startup and that silent startup remains hidden.
- Verify tray and second-instance restoration, including minimized state.
- Verify close-to-tray versus exit behavior for both tray visibility settings.
- Verify lock-toggle UI synchronization and center-lock exit-hotkey protection.
- Run the existing configuration, lock-safety, and window-action tests and a
  full Windows build.

## Out of Scope

- Redesigning the settings layout or changing existing settings semantics.
- Changing the default hotkeys, lock behavior, tray options, or configuration
  format.
- Code-signing or changing release/distribution behavior.
