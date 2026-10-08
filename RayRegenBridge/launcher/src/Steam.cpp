#include "Steam.h"
#include "Util.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
// Values of every "key" "value" line in a Valve KeyValues text file (nesting is ignored, keys can repeat)
std::vector<std::pair<std::string, std::string>> ReadKeyValues(const fs::path& file)
{
    std::vector<std::pair<std::string, std::string>> values;
    std::ifstream in(file);
    std::string line;

    while (std::getline(in, line))
    {
        std::vector<std::string> quoted;
        size_t pos = 0;

        while (quoted.size() < 2)
        {
            auto start = line.find('"', pos);
            if (start == std::string::npos)
                break;

            std::string text;
            size_t i = start + 1;

            for (; i < line.size() && line[i] != '"'; i++)
            {
                // \\ in paths
                if (line[i] == '\\' && i + 1 < line.size())
                    i++;

                text += line[i];
            }

            quoted.push_back(text);
            pos = i + 1;
        }

        if (quoted.size() == 2)
            values.emplace_back(quoted[0], quoted[1]);
    }

    return values;
}

fs::path SteamPath()
{
    wchar_t buffer[MAX_PATH] {};
    DWORD size = sizeof(buffer);

    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buffer,
                     &size) == ERROR_SUCCESS)
        return fs::path(buffer);

    return {};
}
} // namespace

std::vector<fs::path> FindSteamLibraries()
{
    std::vector<fs::path> libraries;
    std::set<std::wstring> seen;

    auto add = [&](const fs::path& path)
    {
        std::error_code ec;
        auto canonical = fs::weakly_canonical(path, ec);
        auto key = Lower(canonical.wstring());

        if (!ec && fs::exists(canonical / L"steamapps", ec) && seen.insert(key).second)
            libraries.push_back(canonical);
    };

    auto steam = SteamPath();

    if (steam.empty())
        return libraries;

    add(steam);

    for (const auto& [key, value] : ReadKeyValues(steam / L"steamapps" / L"libraryfolders.vdf"))
    {
        if (key == "path")
            add(fs::path(Utf8ToWide(value)));
    }

    return libraries;
}

std::vector<SteamGame> FindSteamGames()
{
    std::vector<SteamGame> games;

    for (const auto& library : FindSteamLibraries())
    {
        std::error_code ec;

        for (const auto& entry : fs::directory_iterator(library / L"steamapps", ec))
        {
            const auto file = entry.path().filename().wstring();

            if (file.rfind(L"appmanifest_", 0) != 0 || entry.path().extension() != L".acf")
                continue;

            SteamGame game;
            std::string installDir;

            for (const auto& [key, value] : ReadKeyValues(entry.path()))
            {
                if (key == "appid" && game.appId.empty())
                    game.appId = Utf8ToWide(value);
                else if (key == "name" && game.name.empty())
                    game.name = Utf8ToWide(value);
                else if (key == "installdir" && installDir.empty())
                    installDir = value;
            }

            // 228980: Steamworks Common Redistributables, not a game
            if (installDir.empty() || game.appId == L"228980")
                continue;

            game.installDir = library / L"steamapps" / L"common" / Utf8ToWide(installDir);

            // Manifests can outlive their game folder (the PRAGMATA case: only mod leftovers there)
            if (fs::exists(game.installDir, ec))
                games.push_back(std::move(game));
        }
    }

    std::sort(games.begin(), games.end(),
              [](const SteamGame& a, const SteamGame& b) { return Lower(a.name) < Lower(b.name); });
    return games;
}
