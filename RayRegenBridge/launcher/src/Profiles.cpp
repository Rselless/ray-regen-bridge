#include "Profiles.h"
#include "Util.h"

#include <Windows.h>

#include <sstream>
#include <vector>

namespace
{
std::wstring ReadString(const std::filesystem::path& file, const std::wstring& section, const wchar_t* key)
{
    std::vector<wchar_t> buffer(4096);
    GetPrivateProfileStringW(section.c_str(), key, L"", buffer.data(), (DWORD) buffer.size(), file.c_str());
    return buffer.data();
}

// "Section.Key=value;Section.Key=value"
std::vector<IniOverride> ParseOverrides(const std::wstring& text)
{
    std::vector<IniOverride> overrides;
    std::wstringstream stream(text);
    std::wstring item;

    while (std::getline(stream, item, L';'))
    {
        auto dot = item.find(L'.');
        auto equals = item.find(L'=');

        if (dot == std::wstring::npos || equals == std::wstring::npos || equals < dot)
            continue;

        overrides.push_back({ item.substr(0, dot), item.substr(dot + 1, equals - dot - 1), item.substr(equals + 1) });
    }

    return overrides;
}
} // namespace

void Profiles::Load(const std::filesystem::path& file)
{
    _profiles.clear();

    std::vector<wchar_t> names(65536);
    DWORD length = GetPrivateProfileSectionNamesW(names.data(), (DWORD) names.size(), file.c_str());

    for (const wchar_t* name = names.data(); name < names.data() + length && *name != L'\0';
         name += wcslen(name) + 1)
    {
        Profile profile;
        profile.exe = Lower(name);
        profile.name = ReadString(file, name, L"Name");
        profile.exeDir = ReadString(file, name, L"ExeDir");
        profile.proxy = Lower(ReadString(file, name, L"Proxy"));
        profile.ini = ParseOverrides(ReadString(file, name, L"Ini"));
        profile.notes = ReadString(file, name, L"Notes");
        _profiles[profile.exe] = std::move(profile);
    }
}

const Profile* Profiles::Find(const std::wstring& exeName) const
{
    auto it = _profiles.find(Lower(exeName));
    return it == _profiles.end() ? nullptr : &it->second;
}
