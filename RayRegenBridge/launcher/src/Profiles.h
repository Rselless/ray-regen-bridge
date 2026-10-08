#pragma once
// Per-game settings from profiles.ini (next to the launcher). Section name = the game's exe file name, lowercase.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct IniOverride
{
    std::wstring section;
    std::wstring key;
    std::wstring value;
};

struct Profile
{
    std::wstring exe;    // section name
    std::wstring name;   // display name
    std::wstring exeDir; // folder of the exe relative to the game root, informational
    std::wstring proxy;  // dll name OptiScaler is installed as
    std::vector<IniOverride> ini;
    std::wstring notes;
};

class Profiles
{
  public:
    void Load(const std::filesystem::path& file);
    const Profile* Find(const std::wstring& exeName) const;

  private:
    std::map<std::wstring, Profile> _profiles;
};
