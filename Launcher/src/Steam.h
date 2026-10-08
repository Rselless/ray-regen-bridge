#pragma once
// Steam library discovery: libraries from libraryfolders.vdf, installed games from their appmanifest_*.acf files.

#include <filesystem>
#include <string>
#include <vector>

struct SteamGame
{
    std::wstring appId;
    std::wstring name;
    std::filesystem::path installDir; // steamapps\common\<installdir>
};

std::vector<std::filesystem::path> FindSteamLibraries();
std::vector<SteamGame> FindSteamGames();
