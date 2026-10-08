#pragma once
// Install / update / uninstall into a game's exe folder. Only files the launcher created are ever replaced or
// removed: they are listed in RayRegenBridge.installed next to them, anything else already there is left alone.

#include "GameScan.h"

#include <filesystem>
#include <string>
#include <vector>

inline constexpr wchar_t ManifestName[] = L"RayRegenBridge.installed";

struct Manifest
{
    std::wstring version;
    std::vector<std::wstring> files; // names relative to the exe folder
};

bool ReadManifest(const std::filesystem::path& dir, Manifest& manifest);

struct ActionResult
{
    bool ok = false;
    std::wstring message;
};

// payload: the launcher's payload folder (OptiScaler.dll, OptiScaler.ini, AMD dlls)
std::wstring PayloadVersion(const std::filesystem::path& payload);
ActionResult Install(const GameInfo& game, const std::filesystem::path& payload);
ActionResult Uninstall(const GameInfo& game);
ActionResult CollectLogs(const GameInfo& game, std::filesystem::path& zip);
