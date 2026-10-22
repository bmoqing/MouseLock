// 轻量级日志系统
#pragma once

#include <string>

enum class LogLevel
{
    Info,
    Warning,
    Error
};

bool InitLogger(const std::wstring& logPath);
void Log(LogLevel level, const wchar_t* format, ...);
void ShutdownLogger();

#define LOG_INFO(fmt, ...) Log(LogLevel::Info, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) Log(LogLevel::Warning, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Log(LogLevel::Error, fmt, ##__VA_ARGS__)
