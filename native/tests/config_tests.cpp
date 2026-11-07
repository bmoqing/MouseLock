#include "../config.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

namespace
{
    std::wstring TestConfigPath()
    {
        wchar_t tempDirectory[MAX_PATH]{};
        DWORD length = GetTempPathW(static_cast<DWORD>(std::size(tempDirectory)), tempDirectory);
        if (length == 0 || length >= std::size(tempDirectory))
            return {};

        return std::wstring(tempDirectory, length) +
            L"MouseLock-config-tests-" + std::to_wstring(GetCurrentProcessId()) + L".json";
    }

    bool WriteText(const std::wstring& path, const char* text)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD size = static_cast<DWORD>(std::strlen(text));
        DWORD written = 0;
        bool success = WriteFile(file, text, size, &written, nullptr) && written == size;
        CloseHandle(file);
        return success;
    }

    std::string ReadText(const std::wstring& path)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {};

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
            size.QuadPart > 1024 * 1024)
        {
            CloseHandle(file);
            return {};
        }

        std::string text(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        bool success = text.empty() || (ReadFile(file, text.data(),
            static_cast<DWORD>(text.size()), &read, nullptr) && read == text.size());
        CloseHandle(file);
        return success ? text : std::string{};
    }

    bool TestHiddenTrayPreferenceRoundTrips()
    {
        const std::wstring path = TestConfigPath();
        if (path.empty())
            return false;

        bool success = false;
        do
        {
            if (!WriteText(path, "{\"HideTrayIcon\":true}"))
                break;
            AppConfig config = LoadConfig(path);
            if (!SaveConfig(path, config))
                break;
            if (ReadText(path).find("\"HideTrayIcon\": true") == std::string::npos)
                break;

            if (!WriteText(path, "{}"))
                break;
            config = LoadConfig(path);
            if (!SaveConfig(path, config))
                break;
            if (ReadText(path).find("\"HideTrayIcon\": false") == std::string::npos)
                break;

            success = true;
        } while (false);

        DeleteFileW(path.c_str());
        DeleteFileW((path + L".tmp").c_str());
        return success;
    }

    bool TestLockStateDefaultsUnlockedWhenMissing()
    {
        const std::wstring path = TestConfigPath();
        if (path.empty())
            return false;

        bool success = false;
        if (WriteText(path, "{}"))
        {
            const AppConfig config = LoadConfig(path);
            success = !config.enabled && !config.enabledStateConfigured;
        }
        DeleteFileW(path.c_str());
        DeleteFileW((path + L".tmp").c_str());
        return success;
    }

    bool TestLockStateRoundTrips()
    {
        const std::wstring path = TestConfigPath();
        if (path.empty())
            return false;

        bool success = false;
        do
        {
            AppConfig config;
            config.enabled = true;
            if (!SaveConfig(path, config))
                break;
            config = LoadConfig(path);
            if (!config.enabled || !config.enabledStateConfigured)
                break;

            config.enabled = false;
            if (!SaveConfig(path, config))
                break;
            config = LoadConfig(path);
            if (config.enabled || !config.enabledStateConfigured)
                break;

            success = true;
        } while (false);

        DeleteFileW(path.c_str());
        DeleteFileW((path + L".tmp").c_str());
        return success;
    }
}

int wmain()
{
    if (!TestHiddenTrayPreferenceRoundTrips() ||
        !TestLockStateDefaultsUnlockedWhenMissing() ||
        !TestLockStateRoundTrips())
    {
        std::fprintf(stderr,
            "FAIL: configuration preferences must round-trip and have safe defaults\n");
        return 1;
    }

    std::puts("PASS: configuration preferences round-trip and have safe defaults");
    return 0;
}
