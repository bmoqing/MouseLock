// 设置对话框：快捷键、启动选项与开机自启。
#pragma once

#include <windows.h>
#include <functional>
#include "config.h"

// 显示设置对话框（模态）。所有改动通过 onChanged 实时提交（已写回并生效）。
void ShowSettingsDialog(HWND parent, const AppConfig& current,
                        const std::function<void(const AppConfig&)>& onChanged);