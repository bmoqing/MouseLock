// 日志系统实现：线程安全的文件写入，带时间戳和级别标记
#include "logger.h"
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    CRITICAL_SECTION g_logLock;
    HANDLE g_logFile = INVALID_HANDLE_VALUE;
    bool g_initialized = false;
    std::wstring g_logPath;
    unsigned int g_writesSinceFlush = 0;
    constexpr LONGLONG kMaxLogSize = 5 * 1024 * 1024; // 5MB

    void RotateLogIfNeeded()
    {
        if (g_logFile == INVALID_HANDLE_VALUE)
            return;

        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(g_logFile, &fileSize) || fileSize.QuadPart < kMaxLogSize)
            return;

        // 关闭当前日志文件
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;

        // 重命名为 .old（覆盖旧的备份）
        std::wstring oldPath = g_logPath + L".old";
        DeleteFileW(oldPath.c_str());
        MoveFileW(g_logPath.c_str(), oldPath.c_str());

        // 创建新日志文件
        g_logFile = CreateFileW(g_logPath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            // 写入 BOM
            DWORD bytesWritten = 0;
            const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            WriteFile(g_logFile, bom, sizeof(bom), &bytesWritten, nullptr);
        }
    }
}

bool InitLogger(const std::wstring& logPath)
{
    if (g_initialized)
        return g_logFile != INVALID_HANDLE_VALUE;

    InitializeCriticalSection(&g_logLock);
    g_logPath = logPath;
    g_writesSinceFlush = 0;

    g_logFile = CreateFileW(logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    g_initialized = true;

    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        // 写入 BOM 标记（UTF-8）
        DWORD bytesWritten = 0;
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        LARGE_INTEGER fileSize;
        if (GetFileSizeEx(g_logFile, &fileSize) && fileSize.QuadPart == 0)
        {
            WriteFile(g_logFile, bom, sizeof(bom), &bytesWritten, nullptr);
        }
    }

    return g_logFile != INVALID_HANDLE_VALUE;
}

void ShutdownLogger()
{
    if (!g_initialized)
        return;

    EnterCriticalSection(&g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(g_logFile);
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_logLock);

    DeleteCriticalSection(&g_logLock);
    g_initialized = false;
    g_writesSinceFlush = 0;
}

void Log(LogLevel level, const wchar_t* format, ...)
{
    if (!g_initialized || g_logFile == INVALID_HANDLE_VALUE)
        return;

    EnterCriticalSection(&g_logLock);

    // 检查是否需要轮转日志
    RotateLogIfNeeded();
    if (g_logFile == INVALID_HANDLE_VALUE)
    {
        LeaveCriticalSection(&g_logLock);
        return;
    }

    // 获取时间戳
    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[32];
    sprintf_s(timestamp, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // 级别标记
    const char* levelStr = "";
    switch (level)
    {
    case LogLevel::Info:    levelStr = "[INFO] "; break;
    case LogLevel::Warning: levelStr = "[WARN] "; break;
    case LogLevel::Error:   levelStr = "[ERROR] "; break;
    }

    // 先计算所需长度，避免固定缓冲区截断较长的路径、窗口标题或诊断信息。
    va_list args;
    va_start(args, format);
    int required = _vscwprintf(format, args);
    va_end(args);
    if (required < 0 || required > 1024 * 1024)
    {
        LeaveCriticalSection(&g_logLock);
        return;
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
    va_start(args, format);
    int writtenChars = vswprintf_s(buffer.data(), buffer.size(), format, args);
    va_end(args);
    if (writtenChars < 0)
    {
        LeaveCriticalSection(&g_logLock);
        return;
    }

    // 转换为 UTF-8
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), writtenChars,
        nullptr, 0, nullptr, nullptr);
    if (utf8Size > 0)
    {
        std::string utf8Message(static_cast<size_t>(utf8Size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buffer.data(), writtenChars,
            utf8Message.data(), utf8Size, nullptr, nullptr);

        // 写入日志文件
        DWORD bytesWritten = 0;
        WriteFile(g_logFile, timestamp, static_cast<DWORD>(strlen(timestamp)), &bytesWritten, nullptr);
        WriteFile(g_logFile, levelStr, static_cast<DWORD>(strlen(levelStr)), &bytesWritten, nullptr);
        WriteFile(g_logFile, utf8Message.data(), static_cast<DWORD>(utf8Message.size()), &bytesWritten, nullptr);
        const char* newline = "\r\n";
        WriteFile(g_logFile, newline, 2, &bytesWritten, nullptr);
        ++g_writesSinceFlush;
        if (level != LogLevel::Info || g_writesSinceFlush >= 16)
        {
            FlushFileBuffers(g_logFile);
            g_writesSinceFlush = 0;
        }
    }

    LeaveCriticalSection(&g_logLock);
}
