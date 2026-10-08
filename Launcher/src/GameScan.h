#pragma once
// Works out how to install into one game folder: which exe, which proxy dll name, whether the game can request
// DLSS Ray Reconstruction at all, and what else is already hooked into it (ReShade, another OptiScaler...).

#include "Profiles.h"

#include <filesystem>
#include <string>
#include <vector>

struct GameInfo
{
    std::wstring name;
    std::wstring appId; // Steam only
    std::filesystem::path root;
    std::filesystem::path exe;
    std::filesystem::path exeDir; // where the files go
    const Profile* profile = nullptr;

    bool hasRayReconstruction = false; // ships nvngx_dlssd.dll
    bool hasStreamline = false;
    bool importsD3D12 = false;
    std::wstring proxy;                  // dll name to install as
    std::vector<std::wstring> conflicts; // other hooks found in exeDir, for the user
    std::wstring blocker;                // non-empty: install refused, with the reason

    bool installed = false;
    std::wstring installedVersion;
};

GameInfo ScanGame(const std::wstring& name, const std::wstring& appId, const std::filesystem::path& root,
                  const Profiles& profiles);

// Dll names imported by a PE file (import table only, lowercase)
std::vector<std::wstring> ImportedDlls(const std::filesystem::path& exe);
