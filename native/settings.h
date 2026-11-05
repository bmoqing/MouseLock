// 设置对话框：快捷键、启动选项与开机自启。
#pragma once

#include <windows.h>
#include <functional>
#include <vector>
#include "config.h"

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

void CreateSettingsControls(HWND parent, const AppConfig& current, bool enabled,
                            const std::vector<HWND>& boundWindows,
                            const std::vector<DWORD>& boundProcesses,
                            const SettingsCallbacks& callbacks);
void SyncSettingsControls(const AppConfig& config, bool enabled);
void DestroySettingsControls();
bool HandleSettingsMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           LRESULT& result);
