// 错误处理辅助函数：统一的 Win32 API 错误检查和日志记录模式
#pragma once

#include <windows.h>
#include "logger.h"

// ============ Win32 API 错误处理 ============

/**
 * @brief 记录 Win32 API 错误到日志
 * @param operation 操作描述（如 "GetCursorPos"）
 * @param errorCode 错误码（通常来自 GetLastError()）
 */
inline void LogWin32Error(const wchar_t* operation, DWORD errorCode)
{
    LOG_ERROR(L"%s 失败 (错误码 %d)", operation, errorCode);
}

/**
 * @brief 记录 Win32 API 警告到日志
 * @param operation 操作描述
 * @param errorCode 错误码
 */
inline void LogWin32Warning(const wchar_t* operation, DWORD errorCode)
{
    LOG_WARN(L"%s 失败 (错误码 %d)", operation, errorCode);
}

/**
 * @brief 检查 BOOL 返回值，失败时记录错误
 * @param result API 返回值
 * @param operation 操作描述
 * @return true 如果成功，false 如果失败
 *
 * @example
 * if (!CheckWin32(GetCursorPos(&pt), L"GetCursorPos"))
 *     return false;
 */
inline bool CheckWin32(BOOL result, const wchar_t* operation)
{
    if (!result)
    {
        LogWin32Error(operation, GetLastError());
        return false;
    }
    return true;
}

/**
 * @brief 检查 BOOL 返回值，失败时记录警告
 * @param result API 返回值
 * @param operation 操作描述
 * @return true 如果成功，false 如果失败
 */
inline bool CheckWin32Warn(BOOL result, const wchar_t* operation)
{
    if (!result)
    {
        LogWin32Warning(operation, GetLastError());
        return false;
    }
    return true;
}

/**
 * @brief 检查句柄是否有效，无效时记录错误
 * @param handle API 返回的句柄
 * @param operation 操作描述
 * @return true 如果有效，false 如果无效
 *
 * @example
 * HANDLE h = OpenProcess(...);
 * if (!CheckHandle(h, L"OpenProcess"))
 *     return;
 */
inline bool CheckHandle(HANDLE handle, const wchar_t* operation)
{
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
    {
        LogWin32Error(operation, GetLastError());
        return false;
    }
    return true;
}

/**
 * @brief 检查指针是否有效，无效时记录错误
 * @param pointer API 返回的指针
 * @param operation 操作描述
 * @return true 如果有效，false 如果无效
 */
template<typename T>
inline bool CheckPointer(T* pointer, const wchar_t* operation)
{
    if (pointer == nullptr)
    {
        LogWin32Error(operation, GetLastError());
        return false;
    }
    return true;
}

// ============ HRESULT 错误处理 ============

/**
 * @brief 检查 HRESULT 返回值
 * @param hr HRESULT 返回值
 * @param operation 操作描述
 * @return true 如果成功（SUCCEEDED(hr)），false 如果失败
 */
inline bool CheckHResult(HRESULT hr, const wchar_t* operation)
{
    if (FAILED(hr))
    {
        LOG_ERROR(L"%s 失败 (HRESULT 0x%08X)", operation, hr);
        return false;
    }
    return true;
}

// ============ 断言和前置条件检查 ============

/**
 * @brief 断言条件为真，否则记录错误并返回 false
 * @param condition 要检查的条件
 * @param message 错误消息
 * @return 条件的值
 *
 * @example
 * if (!Require(hwnd != nullptr, L"窗口句柄不能为空"))
 *     return;
 */
inline bool Require(bool condition, const wchar_t* message)
{
    if (!condition)
    {
        LOG_ERROR(L"断言失败: %s", message);
    }
    return condition;
}

/**
 * @brief 警告条件为真，条件为假时记录警告但返回 true
 * @param condition 要检查的条件
 * @param message 警告消息
 * @return true（始终）
 */
inline bool Warn(bool condition, const wchar_t* message)
{
    if (!condition)
    {
        LOG_WARN(L"警告: %s", message);
    }
    return true;
}
