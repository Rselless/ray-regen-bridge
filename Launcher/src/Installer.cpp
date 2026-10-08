#include "Installer.h"
#include "Util.h"

#include <Windows.h>
#include <ShlObj.h>

#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace
{
// AMD dlls copied only when the game doesn't ship its own (Crimson Desert and The Witcher 3 do)
constexpr std::array AmdDlls = { L"amd_fidelityfx_denoiser_dx12.dll", L"amd_fidelityfx_loader_dx12.dll",
                                 L"amd_fidelityfx_upscaler_dx12.dll" };

bool WriteManifest(const fs::path& dir, const Manifest& manifest)
{
    std::wofstream out(dir / ManifestName, std::ios::trunc);

    if (!out)
        return false;

    out << L"; Ray Regen Bridge launcher: files it installed here, removed by Uninstall\n";
    out << L"Version=" << manifest.version << L"\n";

    for (const auto& file : manifest.files)
        out << L"File=" << file << L"\n";

    return (bool) out;
}

bool Copy(const fs::path& from, const fs::path& to, std::wstring& error)
{
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);

    if (ec)
        error = L"Could not copy " + to.filename().wstring() + L": " + Utf8ToWide(ec.message()) +
                L" (is the game running?)";

    return !ec;
}
} // namespace

bool ReadManifest(const fs::path& dir, Manifest& manifest)
{
    std::wifstream in(dir / ManifestName);

    if (!in)
        return false;

    std::wstring line;

    while (std::getline(in, line))
    {
        if (line.rfind(L"Version=", 0) == 0)
            manifest.version = line.substr(8);
        else if (line.rfind(L"File=", 0) == 0 && line.size() > 5)
            manifest.files.push_back(line.substr(5));
    }

    return true;
}

std::wstring PayloadVersion(const fs::path& payload)
{
    std::error_code ec;
    auto file = payload / L"version.txt";

    if (fs::exists(file, ec))
    {
        std::wifstream in(file);
        std::wstring version;
        std::getline(in, version);

        if (!version.empty())
            return version;
    }

    auto time = fs::last_write_time(payload / L"OptiScaler.dll", ec);

    if (ec)
        return L"unknown";

    auto local = std::chrono::zoned_time(std::chrono::current_zone(),
                                         std::chrono::floor<std::chrono::minutes>(
                                             std::chrono::clock_cast<std::chrono::system_clock>(time)));
    return std::format(L"build {:%Y-%m-%d %H:%M}", local.get_local_time());
}

ActionResult Install(const GameInfo& game, const fs::path& payload)
{
    if (!game.blocker.empty())
        return { false, game.blocker };

    std::error_code ec;

    if (!fs::exists(payload / L"OptiScaler.dll", ec) || !fs::exists(payload / L"OptiScaler.ini", ec))
        return { false, L"The payload folder next to the launcher is missing OptiScaler.dll or OptiScaler.ini." };

    Manifest manifest;
    ReadManifest(game.exeDir, manifest);
    std::set<std::wstring> ours;

    for (const auto& file : manifest.files)
        ours.insert(Lower(file));

    Manifest updated;
    updated.version = PayloadVersion(payload);
    std::wstring error;
    std::wstring report;

    // Write the manifest first and after every file, so an interrupted install can still be uninstalled
    auto record = [&](const std::wstring& file)
    {
        updated.files.push_back(file);
        WriteManifest(game.exeDir, updated);
    };

    // Files of a previous install that this one doesn't replace stay ours
    for (const auto& file : manifest.files)
    {
        auto lower = Lower(file);
        bool replaced = lower == game.proxy || lower == L"optiscaler.ini";

        for (auto dll : AmdDlls)
            replaced |= lower == dll;

        if (!replaced && fs::exists(game.exeDir / file, ec))
            updated.files.push_back(file);
    }

    // OptiScaler itself, as the proxy dll
    if (!Copy(payload / L"OptiScaler.dll", game.exeDir / game.proxy, error))
        return { false, error };

    record(game.proxy);
    report += L"OptiScaler installed as " + game.proxy + L"\n";

    // The ini: kept on update so the user's settings survive
    auto ini = game.exeDir / L"OptiScaler.ini";

    if (!fs::exists(ini, ec))
    {
        if (!Copy(payload / L"OptiScaler.ini", ini, error))
            return { false, error };

        report += L"OptiScaler.ini added\n";
    }
    else
    {
        report += L"OptiScaler.ini kept (your settings)\n";
    }

    record(L"OptiScaler.ini");

    for (auto dll : AmdDlls)
    {
        auto target = game.exeDir / dll;

        if (!fs::exists(payload / dll, ec))
            continue;

        if (fs::exists(target, ec) && !ours.contains(dll))
        {
            report += std::wstring(dll) + L": the game's own copy is used (" + FileVersion(target) + L")\n";
            continue;
        }

        if (!Copy(payload / dll, target, error))
            return { false, error };

        record(dll);
        report += std::wstring(dll) + L" added\n";
    }

    // Game specific settings from the profile
    if (game.profile != nullptr)
    {
        for (const auto& item : game.profile->ini)
        {
            WritePrivateProfileStringW(item.section.c_str(), item.key.c_str(), item.value.c_str(), ini.c_str());
            report += L"[" + item.section + L"] " + item.key + L"=" + item.value + L"\n";
        }
    }

    WriteManifest(game.exeDir, updated);
    return { true, report };
}

ActionResult Uninstall(const GameInfo& game)
{
    Manifest manifest;

    if (!ReadManifest(game.exeDir, manifest))
        return { false, L"Nothing installed by the launcher here." };

    std::error_code ec;
    std::wstring report;

    // OptiScaler's log is ours too
    manifest.files.push_back(L"OptiScaler.log");

    for (const auto& file : manifest.files)
    {
        auto path = game.exeDir / file;

        if (!fs::exists(path, ec))
            continue;

        if (!fs::remove(path, ec))
            return { false, L"Could not remove " + file + L": " + Utf8ToWide(ec.message()) + L" (is the game running?)" };

        report += L"Removed " + file + L"\n";
    }

    fs::remove(game.exeDir / ManifestName, ec);
    return { true, report };
}

ActionResult CollectLogs(const GameInfo& game, fs::path& zip)
{
    std::error_code ec;
    std::vector<std::wstring> files;

    for (auto file : { L"OptiScaler.log", L"OptiScaler.ini", ManifestName })
    {
        if (fs::exists(game.exeDir / file, ec))
            files.push_back(file);
    }

    if (files.empty())
        return { false, L"No log or ini in " + game.exeDir.wstring() };

    PWSTR desktop = nullptr;

    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        return { false, L"Desktop folder not found." };

    auto now = std::chrono::zoned_time(std::chrono::current_zone(), std::chrono::system_clock::now());
    zip = fs::path(desktop) /
          std::format(L"RRB-report-{}-{:%Y%m%d-%H%M%S}.zip", game.exe.stem().wstring(), now.get_local_time());
    CoTaskMemFree(desktop);

    // tar.exe ships with Windows 10 1803+, -a picks the format from the .zip extension
    std::wstring command = L"tar.exe -a -c -f \"" + zip.wstring() + L"\" -C \"" + game.exeDir.wstring() + L"\"";

    for (const auto& file : files)
        command += L" \"" + file + L"\"";

    STARTUPINFOW si { sizeof(si) };
    PROCESS_INFORMATION pi {};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
        return { false, L"Could not start tar.exe." };

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode != 0 || !fs::exists(zip, ec))
        return { false, L"tar.exe failed to write the zip." };

    return { true, L"Saved " + zip.wstring() };
}
