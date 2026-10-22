// 配置模块实现：手写 JSON 解析/序列化 + 快捷键格式化。
#include "config.h"
#include "logger.h"

#include <windows.h>
#include <map>

namespace
{
    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            nullptr, 0, nullptr, nullptr);
        if (size <= 0)
            return {};
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr);
        return result;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};
        int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0)
            return {};
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size);
        return result;
    }
}

// ---------------- 极简 JSON DOM ----------------

struct JsonValue;

using JsonObject = std::map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    long long num = 0;
    std::string str;
    JsonArray arr;
    JsonObject obj;
};

namespace
{
    // ---------------- 解析 ----------------

    struct Parser
    {
        const std::string& s;
        size_t i = 0;

        void skipWs()
        {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
                ++i;
        }

        bool parseValue(JsonValue& out)
        {
            skipWs();
            if (i >= s.size())
                return false;
            char c = s[i];
            if (c == '{') return parseObject(out);
            if (c == '[') return parseArray(out);
            if (c == '"') { out.type = JsonValue::Type::String; return parseString(out.str); }
            if (c == 't' || c == 'f') return parseBool(out);
            if (c == 'n') return parseNull(out);
            if (c == '-' || (c >= '0' && c <= '9')) { out.type = JsonValue::Type::Number; return parseNumber(out.num); }
            return false;
        }

        bool parseObject(JsonValue& out)
        {
            out.type = JsonValue::Type::Object;
            out.obj.clear();
            ++i; // '{'
            skipWs();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            while (true)
            {
                skipWs();
                std::string key;
                if (i >= s.size() || s[i] != '"') return false;
                if (!parseString(key)) return false;
                skipWs();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                JsonValue v;
                if (!parseValue(v)) return false;
                out.obj[key] = std::move(v);
                skipWs();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }

        bool parseArray(JsonValue& out)
        {
            out.type = JsonValue::Type::Array;
            out.arr.clear();
            ++i; // '['
            skipWs();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            while (true)
            {
                JsonValue v;
                if (!parseValue(v)) return false;
                out.arr.push_back(std::move(v));
                skipWs();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return false;
            }
        }

        bool parseString(std::string& out)
        {
            ++i; // '"'
            out.clear();
            while (i < s.size())
            {
                char c = s[i];
                if (c == '"') { ++i; return true; }
                if (c == '\\')
                {
                    ++i;
                    if (i >= s.size()) return false;
                    char e = s[i++];
                    switch (e)
                    {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        // \uXXXX —— 仅解码 ASCII/BMP，足够本配置使用
                        if (i + 4 > s.size()) return false;
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k)
                        {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else return false;
                        }
                        // 按 UTF-8 编码写入
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800)
                        {
                            out += char(0xC0 | (cp >> 6));
                            out += char(0x80 | (cp & 0x3F));
                        }
                        else
                        {
                            out += char(0xE0 | (cp >> 12));
                            out += char(0x80 | ((cp >> 6) & 0x3F));
                            out += char(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return false;
                    }
                }
                else
                {
                    out += c;
                    ++i;
                }
            }
            return false;
        }

        bool parseBool(JsonValue& out)
        {
            if (s.compare(i, 4, "true") == 0) { out.type = JsonValue::Type::Bool; out.b = true; i += 4; return true; }
            if (s.compare(i, 5, "false") == 0) { out.type = JsonValue::Type::Bool; out.b = false; i += 5; return true; }
            return false;
        }

        bool parseNull(JsonValue& out)
        {
            if (s.compare(i, 4, "null") == 0) { out.type = JsonValue::Type::Null; i += 4; return true; }
            return false;
        }

        bool parseNumber(long long& out)
        {
            bool neg = false;
            if (i < s.size() && s[i] == '-') { neg = true; ++i; }
            long long v = 0;
            bool any = false;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; any = true; }
            if (!any) return false;
            out = neg ? -v : v;
            return true;
        }
    };

    // ---------------- 序列化 ----------------

    void writeString(std::string& out, const std::string& s)
    {
        out += '"';
        for (char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
            }
        }
        out += '"';
    }

    void writeValue(std::string& out, const JsonValue& v, int indent);

    void writeIndent(std::string& out, int indent)
    {
        for (int k = 0; k < indent; ++k)
            out += ' ';
    }

    void writeObject(std::string& out, const JsonObject& obj, int indent)
    {
        if (obj.empty()) { out += "{}"; return; }
        out += "{\n";
        size_t n = obj.size();
        size_t idx = 0;
        for (const auto& [k, v] : obj)
        {
            writeIndent(out, indent + 2);
            writeString(out, k);
            out += ": ";
            writeValue(out, v, indent + 2);
            if (++idx < n) out += ",";
            out += "\n";
        }
        writeIndent(out, indent);
        out += "}";
    }

    void writeArray(std::string& out, const JsonArray& arr, int indent)
    {
        if (arr.empty()) { out += "[]"; return; }
        out += "[\n";
        size_t idx = 0;
        for (const auto& v : arr)
        {
            writeIndent(out, indent + 2);
            writeValue(out, v, indent + 2);
            if (++idx < arr.size()) out += ",";
            out += "\n";
        }
        writeIndent(out, indent);
        out += "]";
    }

    void writeValue(std::string& out, const JsonValue& v, int indent)
    {
        switch (v.type)
        {
        case JsonValue::Type::Null: out += "null"; break;
        case JsonValue::Type::Bool: out += v.b ? "true" : "false"; break;
        case JsonValue::Type::Number: out += std::to_string(v.num); break;
        case JsonValue::Type::String: writeString(out, v.str); break;
        case JsonValue::Type::Array: writeArray(out, v.arr, indent); break;
        case JsonValue::Type::Object: writeObject(out, v.obj, indent); break;
        }
    }

    // ---------------- 取值辅助 ----------------

    const JsonValue* find(const JsonObject& o, const char* key)
    {
        auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }

    Modifier modifierFromString(const std::string& s)
    {
        if (s == "Alt") return Modifier::Alt;
        if (s == "Shift") return Modifier::Shift;
        return Modifier::Control; // 默认，也覆盖 "Control"
    }

    bool parseHotkey(const JsonValue* v, Hotkey& out)
    {
        if (!v || v->type != JsonValue::Type::Object)
            return false;
        const JsonValue* mods = find(v->obj, "Modifiers");
        out.modifiers.clear();
        if (mods && mods->type == JsonValue::Type::Array)
        {
            for (const auto& m : mods->arr)
                if (m.type == JsonValue::Type::String)
                    out.modifiers.push_back(modifierFromString(m.str));
        }
        const JsonValue* key = find(v->obj, "Key");
        out.key = (key && key->type == JsonValue::Type::Number) ? (int)key->num : 0;
        return true;
    }

    JsonValue hotkeyToJson(const Hotkey& hk)
    {
        JsonValue obj;
        obj.type = JsonValue::Type::Object;
        obj.obj["Modifiers"].type = JsonValue::Type::Array;
        for (auto m : hk.modifiers)
        {
            JsonValue s;
            s.type = JsonValue::Type::String;
            s.str = (m == Modifier::Alt) ? "Alt" : (m == Modifier::Shift) ? "Shift" : "Control";
            obj.obj["Modifiers"].arr.push_back(std::move(s));
        }
        obj.obj["Key"].type = JsonValue::Type::Number;
        obj.obj["Key"].num = hk.key;
        return obj;
    }

    // ---------------- 快捷键格式化 ----------------

    std::wstring modifierText(Modifier m)
    {
        switch (m)
        {
        case Modifier::Control: return L"Ctrl";
        case Modifier::Alt: return L"Alt";
        case Modifier::Shift: return L"Shift";
        }
        return L"";
    }

    std::wstring vkToText(int vk)
    {
        if (vk == VK_XBUTTON1) return L"鼠标侧键4";
        if (vk == VK_XBUTTON2) return L"鼠标侧键5";
        switch (vk)
        {
        case 0x25: return L"←";
        case 0x27: return L"→";
        case 0x26: return L"↑";
        case 0x28: return L"↓";
        case 0x20: return L"空格";
        case 0x09: return L"Tab";
        case 0x0D: return L"Enter";
        case 0x1B: return L"Esc";
        case 0x08: return L"Backspace";
        case 0x2E: return L"Delete";
        case 0x2D: return L"Insert";
        case 0x24: return L"Home";
        case 0x23: return L"End";
        case 0x21: return L"PageUp";
        case 0x22: return L"PageDown";
        }
        if (vk >= 0x30 && vk <= 0x39) return std::wstring(1, wchar_t(vk));
        if (vk >= 0x41 && vk <= 0x5A) return std::wstring(1, wchar_t(vk));
        if (vk >= 0x70 && vk <= 0x87) return L"F" + std::to_wstring(vk - 0x6F);
        // 其余可打印键（标点、小键盘等）：按当前键盘布局映射为字符
        UINT c = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0xFFFF;
        if (c >= 0x20 && c < 0xD800)
            return std::wstring(1, (wchar_t)c);
        return L"键 " + std::to_wstring(vk);
    }

    constexpr LONGLONG kMaxConfigFileSize = 1024 * 1024;

    AppConfig DefaultConfig()
    {
        AppConfig cfg;
        cfg.unlock.modifiers = { Modifier::Control };
        cfg.unlock.key = 0;
        cfg.jumpLeft.modifiers = { Modifier::Control, Modifier::Alt };
        cfg.jumpLeft.key = 0x25;
        cfg.jumpRight.modifiers = { Modifier::Control, Modifier::Alt };
        cfg.jumpRight.key = 0x27;
        return cfg;
    }

    void PreserveCorruptConfig(const std::wstring& path)
    {
        for (int index = 0; index < 100; ++index)
        {
            std::wstring backup = path + L".bad";
            if (index > 0)
                backup += L"." + std::to_wstring(index);

            SetLastError(ERROR_SUCCESS);
            if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES)
                continue;
            if (GetLastError() != ERROR_FILE_NOT_FOUND)
                continue;

            if (CopyFileW(path.c_str(), backup.c_str(), TRUE))
            {
                LOG_WARN(L"配置文件疑似损坏，已保留副本：%s", backup.c_str());
                return;
            }

            LOG_WARN(L"配置文件疑似损坏，但无法创建副本 %s (错误码 %d)",
                backup.c_str(), GetLastError());
            return;
        }

        LOG_WARN(L"配置文件疑似损坏，但可用的 .bad 副本名称已耗尽：%s", path.c_str());
    }
}

// ---------------- 对外接口 ----------------

AppConfig LoadConfig(const std::wstring& path)
{
    AppConfig cfg = DefaultConfig();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            LOG_WARN(L"读取配置文件失败，将使用默认配置：%s (错误码 %d)", path.c_str(), error);
        return cfg;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(h, &fileSize))
    {
        DWORD error = GetLastError();
        CloseHandle(h);
        LOG_WARN(L"读取配置文件大小失败，将使用默认配置：%s (错误码 %d)", path.c_str(), error);
        PreserveCorruptConfig(path);
        return cfg;
    }
    if (fileSize.QuadPart < 0 || fileSize.QuadPart > kMaxConfigFileSize)
    {
        CloseHandle(h);
        LOG_WARN(L"配置文件超过允许大小，将使用默认配置：%s (大小 %lld 字节)",
            path.c_str(), fileSize.QuadPart);
        PreserveCorruptConfig(path);
        return cfg;
    }

    DWORD size = static_cast<DWORD>(fileSize.QuadPart);
    std::string text(size, '\0');
    DWORD read = 0;
    BOOL readOk = size == 0 || ReadFile(h, text.data(), size, &read, nullptr);
    CloseHandle(h);
    if (!readOk || read != size)
    {
        DWORD error = readOk ? ERROR_READ_FAULT : GetLastError();
        LOG_WARN(L"读取配置文件内容不完整，将使用默认配置：%s (已读 %lu/%lu，错误码 %d)",
            path.c_str(), read, size, error);
        PreserveCorruptConfig(path);
        return cfg;
    }

    JsonValue root;
    Parser p{ text };
    if (!p.parseValue(root))
    {
        LOG_WARN(L"配置文件 JSON 解析失败，将使用默认配置：%s", path.c_str());
        PreserveCorruptConfig(path);
        return cfg;
    }
    p.skipWs();
    if (root.type != JsonValue::Type::Object || p.i != text.size())
    {
        LOG_WARN(L"配置文件 JSON 根节点或尾部内容无效，将使用默认配置：%s", path.c_str());
        PreserveCorruptConfig(path);
        return cfg;
    }

    if (const JsonValue* v = find(root.obj, "UnlockHotkey"))
        if (v->type == JsonValue::Type::Null)
            cfg.unlock.key = -1; // 标记为“未设置”
        else
            parseHotkey(v, cfg.unlock);

    if (const JsonValue* v = find(root.obj, "JumpLeft"))
        if (v->type == JsonValue::Type::Null)
            cfg.jumpLeft.key = -1;
        else
            parseHotkey(v, cfg.jumpLeft);

    if (const JsonValue* v = find(root.obj, "JumpRight"))
        if (v->type == JsonValue::Type::Null)
            cfg.jumpRight.key = -1;
        else
            parseHotkey(v, cfg.jumpRight);

    if (const JsonValue* v = find(root.obj, "StartSilent"))
        if (v->type == JsonValue::Type::Bool)
            cfg.startSilent = v->b;
    if (const JsonValue* v = find(root.obj, "AutoStart"))
        if (v->type == JsonValue::Type::Bool)
            cfg.autoStart = v->b;
    if (const JsonValue* v = find(root.obj, "CenterLock"))
        if (v->type == JsonValue::Type::Bool)
            cfg.centerLock = v->b;
    if (const JsonValue* v = find(root.obj, "ForegroundBinding"))
        if (v->type == JsonValue::Type::Bool)
            cfg.foregroundBinding = v->b;
    if (const JsonValue* v = find(root.obj, "BoundWindowPaths"))
        if (v->type == JsonValue::Type::Array)
            for (const auto& item : v->arr)
                if (item.type == JsonValue::Type::String)
                {
                    std::wstring path = Utf8ToWide(item.str);
                    if (!path.empty())
                        cfg.boundWindowPaths.push_back(std::move(path));
                }
    if (const JsonValue* v = find(root.obj, "BoundProcessPaths"))
        if (v->type == JsonValue::Type::Array)
            for (const auto& item : v->arr)
                if (item.type == JsonValue::Type::String)
                {
                    std::wstring path = Utf8ToWide(item.str);
                    if (!path.empty())
                        cfg.boundProcessPaths.push_back(std::move(path));
                }

    // 旧版本把同一个应用拆成窗口路径和进程路径保存。两者运行时效果相同，
    // 升级后统一合并到应用路径，避免设置页重复显示。
    for (const auto& path : cfg.boundProcessPaths)
    {
        bool exists = false;
        for (const auto& existing : cfg.boundWindowPaths)
            if (CompareStringOrdinal(existing.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
            {
                exists = true;
                break;
            }
        if (!exists)
            cfg.boundWindowPaths.push_back(path);
    }
    cfg.boundProcessPaths.clear();

    return cfg;
}

bool SaveConfig(const std::wstring& path, const AppConfig& cfg)
{
    JsonValue root;
    root.type = JsonValue::Type::Object;

    auto makeHotkey = [](const Hotkey* hk) -> JsonValue {
        if (!hk)
        {
            JsonValue n; n.type = JsonValue::Type::Null; return n;
        }
        return hotkeyToJson(*hk);
    };

    root.obj["UnlockHotkey"] = cfg.unlock.key == -1 ? makeHotkey(nullptr) : hotkeyToJson(cfg.unlock);
    root.obj["JumpLeft"] = cfg.jumpLeft.key == -1 ? makeHotkey(nullptr) : hotkeyToJson(cfg.jumpLeft);
    root.obj["JumpRight"] = cfg.jumpRight.key == -1 ? makeHotkey(nullptr) : hotkeyToJson(cfg.jumpRight);
    root.obj["StartSilent"].type = JsonValue::Type::Bool;
    root.obj["StartSilent"].b = cfg.startSilent;
    root.obj["AutoStart"].type = JsonValue::Type::Bool;
    root.obj["AutoStart"].b = cfg.autoStart;
    root.obj["CenterLock"].type = JsonValue::Type::Bool;
    root.obj["CenterLock"].b = cfg.centerLock;
    root.obj["ForegroundBinding"].type = JsonValue::Type::Bool;
    root.obj["ForegroundBinding"].b = cfg.foregroundBinding;
    root.obj["BoundWindowPaths"].type = JsonValue::Type::Array;
    for (const auto& path : cfg.boundWindowPaths)
    {
        JsonValue item;
        item.type = JsonValue::Type::String;
        item.str = WideToUtf8(path);
        root.obj["BoundWindowPaths"].arr.push_back(std::move(item));
    }
    // 保留旧字段作为空数组，便于旧版本读取；新版本只使用统一的应用路径。
    root.obj["BoundProcessPaths"].type = JsonValue::Type::Array;

    std::string out;
    writeValue(out, root, 0);

    std::wstring tempPath = path + L".tmp";
    HANDLE h = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR(L"配置保存失败：无法创建临时文件 %s (错误码 %d)",
            tempPath.c_str(), GetLastError());
        return false;
    }

    DWORD written = 0;
    bool success = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) != FALSE &&
        written == out.size();
    if (success && !FlushFileBuffers(h))
        success = false;
    DWORD writeError = success ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);

    if (!success)
    {
        DeleteFileW(tempPath.c_str());
        LOG_ERROR(L"配置保存失败：写入临时文件失败 (错误码 %d)", writeError);
        return false;
    }

    if (!MoveFileExW(tempPath.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DWORD moveError = GetLastError();
        DeleteFileW(tempPath.c_str());
        LOG_ERROR(L"配置保存失败：替换正式文件失败 (错误码 %d)", moveError);
        return false;
    }

    LOG_INFO(L"配置已保存");
    return true;
}

std::wstring FormatHotkey(const Hotkey& hk)
{
    if (!IsSet(hk))
        return L"";
    std::wstring res;
    bool first = true;
    auto addPart = [&](const std::wstring& p) {
        if (!first) res += L" + ";
        res += p;
        first = false;
    };
    static const Modifier order[] = { Modifier::Control, Modifier::Alt, Modifier::Shift };
    for (auto m : order)
    {
        for (auto h : hk.modifiers)
            if (h == m) { addPart(modifierText(m)); break; }
    }
    if (hk.key != 0)
    {
        auto t = vkToText(hk.key);
        if (!t.empty())
            addPart(t);
    }
    return res;
}
