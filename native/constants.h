// 全局常量定义：避免魔法数字，提高代码可读性
#pragma once

#include <cstddef>

// ============ 缓冲区大小 ============

// Windows 类名最大长度
// 注：MSDN 未明确限制，但 256 字符是安全的经验值
constexpr size_t kMaxWindowClassNameLength = 256;

// 窗口标题合理长度
// 注：超长标题会被截断，256 字符足够显示
constexpr size_t kMaxWindowTitleLength = 256;

// Windows 路径最大长度
// 注：Windows 10 1607+ 支持长路径（需启用），最长 32767 字符
// 传统 MAX_PATH 只有 260，但许多程序实际路径超过此限制
constexpr size_t kMaxWindowsPathLength = 32767;

// 进程枚举缓冲区的最大条目数，避免异常情况下无限扩容
constexpr size_t kMaxProcessEnumerationEntries = 65536;

// ============ 窗口尺寸 ============

// 主窗口尺寸
constexpr int kMainWindowWidth = 360;
constexpr int kMainWindowHeight = 150;

// 设置对话框尺寸
constexpr int kSettingsWindowWidth = 800;
constexpr int kSettingsWindowHeight = 760;
constexpr int kSettingsWindowMinWidth = 800;  // 最小宽度
constexpr int kSettingsWindowMinHeight = 600; // 低分辨率下允许缩小到的最小高度

// ============ 控件布局 ============

// 标准控件间距
constexpr int kControlMargin = 16;
constexpr int kControlSpacing = 8;

// 标准按钮尺寸
constexpr int kButtonWidth = 100;
constexpr int kButtonHeight = 28;

// 标准编辑框高度
constexpr int kEditBoxHeight = 23;

// 列表框尺寸（设置窗口）
constexpr int kListBoxWidth = 360;
constexpr int kWindowListHeight = 120;
constexpr int kProcessListHeight = 120;
constexpr int kBindingListHeight = 322;

// ============ 时间相关 ============

// 前台窗口采样次数
// 注：窗口切换时需要多次采样以确保前台窗口稳定
constexpr int kForegroundStableSamples = 3;

// 进程等待超时（毫秒）
// 注：等待 schtasks.exe 执行完成的最长时间
constexpr int kProcessWaitTimeoutMs = 5000;

// ============ 键盘相关 ============

// GetAsyncKeyState 按下状态位掩码
constexpr int kAsyncKeyStatePressedMask = 0x8000;
