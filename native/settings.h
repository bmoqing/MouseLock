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

// 显示设置对话框（模态）。所有改动通过 onChanged 实时提交（已写回并生效）。
void ShowSettingsDialog(HWND parent, const AppConfig& current,
                        const std::vector<HWND>& boundWindows,
                        const std::vector<DWORD>& boundProcesses,
                        const std::function<void(AppConfig&)>& onChanged,
                        const BindingChangedCallback& onBindingsChanged);
