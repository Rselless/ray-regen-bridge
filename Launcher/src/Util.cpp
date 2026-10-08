#include "Util.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return {};

    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int) text.size(), wide.data(), size);
    return wide;
}

std::wstring Lower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return (wchar_t) std::towlower(c); });
    return text;
}

namespace
{
std::wstring VersionString(const std::filesystem::path& file, const wchar_t* name)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);

    if (size == 0)
        return {};

    std::vector<BYTE> data(size);

    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()))
        return {};

    struct Translation
    {
        WORD language;
        WORD codePage;
    }* translations = nullptr;
    UINT length = 0;

    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**) &translations, &length) ||
        length < sizeof(Translation))
        return {};

    wchar_t key[128];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language, translations[0].codePage, name);

    wchar_t* value = nullptr;

    if (!VerQueryValueW(data.data(), key, (void**) &value, &length) || value == nullptr)
        return {};

    return value;
}
} // namespace

std::wstring ProductName(const std::filesystem::path& file) { return VersionString(file, L"ProductName"); }

std::wstring FileVersion(const std::filesystem::path& file) { return VersionString(file, L"FileVersion"); }

std::filesystem::path LauncherDir()
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}
