// 设置主窗口的快捷键、启动选项与开机自启控件。
#pragma once

#include <windows.h>
#include <functional>
#include <vector>
#include "config.h"
#include "resource.h"

using BindingChangedCallback = std::function<void(const std::vector<HWND>&,
                                                   const std::vector<DWORD>&,
                                                   const std::vector<std::wstring>&,
                                                   const std::vector<std::wstring>&)>;

struct SettingsCallbacks
{
    std::function<void(AppConfig&)> onChanged;
    BindingChangedCallback onBindingsChanged;
    std::function<void(bool)> onEnabledChanged;
    std::function<void(bool)> onHotkeyCaptureChanged;
};

inline bool IsSettingsCheckboxControlId(int id)
{
    switch (id)
    {
    case IDC_CHK_ENABLED:
    case IDC_CHK_SILENT:
    case IDC_CHK_AUTOSTART:
    case IDC_CHK_CENTER:
    case IDC_CHK_BINDING:
    case IDC_CHK_HIDE_TRAY_ICON:
    case IDC_CHK_SHOW_HIDDEN:
        return true;
    default:
        return false;
    }
}

void CreateSettingsControls(HWND parent, const AppConfig& current, bool enabled,
                            const std::vector<HWND>& boundWindows,
                            const std::vector<DWORD>& boundProcesses,
                            const SettingsCallbacks& callbacks);
void SyncSettingsControls(const AppConfig& config, bool enabled);
void DestroySettingsControls();
bool HandleSettingsMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           LRESULT& result);
