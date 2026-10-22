// RAII 句柄包装器：自动管理 Windows 句柄生命周期，防止资源泄漏
#pragma once

#include <windows.h>
#include <utility>

// 通用句柄包装器
template<typename T, typename Deleter>
class unique_handle
{
public:
    unique_handle() : m_handle(GetInvalidValue()), m_deleter() {}

    explicit unique_handle(T handle, Deleter deleter = Deleter())
        : m_handle(handle), m_deleter(deleter) {}

    ~unique_handle()
    {
        Close();
    }

    // 禁止拷贝
    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    // 支持移动
    unique_handle(unique_handle&& other) noexcept
        : m_handle(other.m_handle), m_deleter(std::move(other.m_deleter))
    {
        other.m_handle = GetInvalidValue();
    }

    unique_handle& operator=(unique_handle&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            m_handle = other.m_handle;
            m_deleter = std::move(other.m_deleter);
            other.m_handle = GetInvalidValue();
        }
        return *this;
    }

    T get() const { return m_handle; }

    explicit operator bool() const { return IsValid(); }

    bool IsValid() const
    {
        return m_handle != GetInvalidValue() && m_handle != nullptr;
    }

    void Close()
    {
        if (IsValid())
        {
            m_deleter(m_handle);
            m_handle = GetInvalidValue();
        }
    }

    T Release()
    {
        T handle = m_handle;
        m_handle = GetInvalidValue();
        return handle;
    }

private:
    T m_handle;
    Deleter m_deleter;

    static T GetInvalidValue()
    {
        if constexpr (std::is_same_v<T, HANDLE>)
            return INVALID_HANDLE_VALUE;
        else
            return nullptr;
    }
};

// 进程句柄删除器
struct process_handle_deleter
{
    void operator()(HANDLE h) const
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
            CloseHandle(h);
    }
};

// 文件句柄删除器
struct file_handle_deleter
{
    void operator()(HANDLE h) const
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
            CloseHandle(h);
    }
};

// 查找句柄删除器
struct find_handle_deleter
{
    void operator()(HANDLE h) const
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
            FindClose(h);
    }
};

// 便捷类型定义
using unique_process_handle = unique_handle<HANDLE, process_handle_deleter>;
using unique_file_handle = unique_handle<HANDLE, file_handle_deleter>;
using unique_find_handle = unique_handle<HANDLE, find_handle_deleter>;
