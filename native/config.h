// 配置模块：配置结构、手写 JSON 读写、快捷键格式化。
#pragma once

#include <string>
#include <vector>

// 修饰键（与旧 C# 版 HoldModifier 的字符串枚举保持一致）
enum class Modifier
{
    Control = 0,
    Alt = 1,
    Shift = 2
};

// 一个快捷键：若干修饰键 + 一个主键（VK 码，0 表示仅修饰键）
// key == -1 表示“未设置”（对应旧版 null）
struct Hotkey
{
    std::vector<Modifier> modifiers;
    int key = 0; // 0 = 无主键；-1 = 未设置
};

// 是否已设置（未设置时不参与匹配与格式化）
inline bool IsSet(const Hotkey& hk) { return hk.key != -1; }

// 应用配置（对应旧 config.json 结构）
struct AppConfig
{
    Hotkey unlock;    // 长按临时解锁
    Hotkey jumpLeft;  // 跳转上一屏幕
    Hotkey jumpRight; // 跳转下一屏幕
    bool startSilent = false;
    bool autoStart = false;
};

// 从 JSON 文本加载配置；失败或缺失时回退默认值。
AppConfig LoadConfig(const std::wstring& path);

// 将配置写回 JSON 文本（格式上与旧版可互相读取）。
void SaveConfig(const std::wstring& path, const AppConfig& cfg);

// 把快捷键格式化为可读文本（如 "Ctrl + Alt + ←"）。
std::wstring FormatHotkey(const Hotkey& hk);