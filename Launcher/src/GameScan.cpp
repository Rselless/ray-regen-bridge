#include "GameScan.h"
#include "Installer.h"
#include "Util.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <set>

namespace fs = std::filesystem;

namespace
{
// Proxy names OptiScaler can be loaded as, most compatible first
constexpr std::array ProxyNames = { L"winmm.dll", L"dxgi.dll", L"version.dll", L"dbghelp.dll", L"winhttp.dll" };

// Files other mods use to hook a game
constexpr std::array HookFiles = { L"dxgi.dll",   L"d3d12.dll",   L"d3d11.dll",  L"winmm.dll",
                                   L"version.dll", L"dinput8.dll", L"dbghelp.dll", L"winhttp.dll",
                                   L"nvngx.dll",  L"OptiScaler.dll" };

constexpr std::array NotGameExe = { L"unins", L"crash", L"report", L"setup", L"redist", L"launcher", L"updater",
                                    L"helper", L"prereq", L"cef", L"vc_", L"dxsetup", L"modmanager", L"easyanticheat",
                                    L"battleye", L"pers.exe", L"touchup", L"vconsole", L"ui.exe", L"injector",
                                    L"compiler" };

// Folders that hold installers and tools, never the game
constexpr std::array NotGameDir = { L"\\__installer", L"\\_commonredist", L"\\redist", L"\\installer", L"\\support",
                                    L"\\tools", L"\\easyanticheat", L"\\battleye", L"\\__overlay" };

bool LooksLikeGameExe(const fs::path& exe)
{
    auto name = Lower(exe.filename().wstring());
    auto dir = Lower(exe.parent_path().wstring());

    for (auto part : NotGameExe)
    {
        if (name.find(part) != std::wstring::npos)
            return false;
    }

    for (auto part : NotGameDir)
    {
        if (dir.find(part) != std::wstring::npos)
            return false;
    }

    return true;
}

// Exes and the folders with RR / Streamline dlls, at most a few levels deep (Unreal: Binaries\Win64)
struct FolderScan
{
    std::vector<fs::path> exes;
    std::set<fs::path> rrDirs;
    std::set<fs::path> streamlineDirs;
};

FolderScan ScanFolder(const fs::path& root)
{
    FolderScan scan;
    std::error_code ec;
    constexpr int MaxDepth = 5;

    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (it->is_directory(ec))
        {
            if (it.depth() >= MaxDepth)
                it.disable_recursion_pending();

            continue;
        }

        auto name = Lower(it->path().filename().wstring());

        if (it->path().extension() == L".exe" && it.depth() <= 4)
            scan.exes.push_back(it->path());
        else if (name == L"nvngx_dlssd.dll")
            scan.rrDirs.insert(it->path().parent_path());
        else if (name == L"sl.interposer.dll")
            scan.streamlineDirs.insert(it->path().parent_path());
    }

    return scan;
}

fs::path PickExe(const FolderScan& scan, const Profiles& profiles, const Profile*& profile)
{
    for (const auto& exe : scan.exes)
    {
        if ((profile = profiles.Find(exe.filename().wstring())) != nullptr)
            return exe;
    }

    // A game exe next to the Streamline / RR dlls, otherwise the largest game-looking exe
    fs::path best;
    uintmax_t bestSize = 0;
    bool bestNextToDlls = false;

    for (const auto& exe : scan.exes)
    {
        if (!LooksLikeGameExe(exe))
            continue;

        std::error_code ec;
        auto size = fs::file_size(exe, ec);
        bool nextToDlls = scan.streamlineDirs.contains(exe.parent_path()) || scan.rrDirs.contains(exe.parent_path());

        if (best.empty() || (nextToDlls && !bestNextToDlls) || (nextToDlls == bestNextToDlls && size > bestSize))
        {
            best = exe;
            bestSize = size;
            bestNextToDlls = nextToDlls;
        }
    }

    return best;
}

std::wstring DescribeHook(const fs::path& file)
{
    auto product = ProductName(file);
    return file.filename().wstring() + (product.empty() ? L"" : L" (" + product + L")");
}
} // namespace

std::vector<std::wstring> ImportedDlls(const fs::path& exe)
{
    std::vector<std::wstring> dlls;

    HANDLE file = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return dlls;

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const BYTE* base = mapping ? (const BYTE*) MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    LARGE_INTEGER size {};
    GetFileSizeEx(file, &size);

    auto valid = [&](uint64_t offset, uint64_t length) { return offset + length <= (uint64_t) size.QuadPart; };

    if (base != nullptr && valid(0, sizeof(IMAGE_DOS_HEADER)))
    {
        auto dos = (const IMAGE_DOS_HEADER*) base;

        if (dos->e_magic == IMAGE_DOS_SIGNATURE && valid(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64)))
        {
            auto nt = (const IMAGE_NT_HEADERS64*) (base + dos->e_lfanew);

            if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            {
                auto sections = IMAGE_FIRST_SECTION(nt);
                auto sectionCount = nt->FileHeader.NumberOfSections;

                // RVA to file offset through the section table, 0 when it isn't in any section
                auto toOffset = [&](DWORD rva) -> uint64_t
                {
                    for (WORD i = 0; i < sectionCount; i++)
                    {
                        const auto& s = sections[i];

                        if (rva >= s.VirtualAddress && rva < s.VirtualAddress + std::max(s.Misc.VirtualSize, s.SizeOfRawData))
                            return (uint64_t) rva - s.VirtualAddress + s.PointerToRawData;
                    }

                    return 0;
                };

                const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                uint64_t offset = dir.VirtualAddress ? toOffset(dir.VirtualAddress) : 0;

                for (; offset != 0 && valid(offset, sizeof(IMAGE_IMPORT_DESCRIPTOR));
                     offset += sizeof(IMAGE_IMPORT_DESCRIPTOR))
                {
                    auto desc = (const IMAGE_IMPORT_DESCRIPTOR*) (base + offset);

                    if (desc->Name == 0)
                        break;

                    auto nameOffset = toOffset(desc->Name);

                    if (nameOffset == 0 || !valid(nameOffset, 1))
                        continue;

                    std::string name;

                    for (auto p = (const char*) base + nameOffset;
                         *p != '\0' && valid((uint64_t) (p - (const char*) base), 1) && name.size() < 260; p++)
                        name += *p;

                    dlls.push_back(Lower(Utf8ToWide(name)));
                }
            }
        }
    }

    if (base != nullptr)
        UnmapViewOfFile(base);

    if (mapping != nullptr)
        CloseHandle(mapping);

    CloseHandle(file);
    return dlls;
}

GameInfo ScanGame(const std::wstring& name, const std::wstring& appId, const fs::path& root, const Profiles& profiles)
{
    GameInfo game;
    game.name = name;
    game.appId = appId;
    game.root = root;

    auto scan = ScanFolder(root);
    game.exe = PickExe(scan, profiles, game.profile);

    if (game.profile != nullptr && !game.profile->name.empty())
        game.name = game.profile->name;

    if (game.exe.empty())
    {
        game.blocker = L"No game exe found in this folder.";
        return game;
    }

    game.exeDir = game.exe.parent_path();
    game.hasRayReconstruction = !scan.rrDirs.empty();
    game.hasStreamline = !scan.streamlineDirs.empty();

    Manifest manifest;
    game.installed = ReadManifest(game.exeDir, manifest);
    game.installedVersion = manifest.version;
    std::set<std::wstring> ours;

    for (const auto& file : manifest.files)
        ours.insert(Lower(file));

    auto imports = ImportedDlls(game.exe);
    game.importsD3D12 = std::find(imports.begin(), imports.end(), L"d3d12.dll") != imports.end();

    // Hooks from other mods
    std::error_code ec;
    bool otherOptiScaler = false;

    for (auto file : HookFiles)
    {
        auto path = game.exeDir / file;

        if (!fs::exists(path, ec) || ours.contains(Lower(file)))
            continue;

        game.conflicts.push_back(DescribeHook(path));

        if (Lower(ProductName(path)).find(L"optiscaler") != std::wstring::npos)
            otherOptiScaler = true;
    }

    if (fs::exists(game.exeDir / L"OptiScaler.ini", ec) && !ours.contains(L"optiscaler.ini"))
        otherOptiScaler = true;

    if (otherOptiScaler)
        game.blocker = L"Another OptiScaler install is in this folder. Remove it first.";

    // Proxy: the profile's, the one already installed, or the first free name the exe imports, then the first free
    if (game.profile != nullptr && !game.profile->proxy.empty())
    {
        game.proxy = game.profile->proxy;
    }
    else
    {
        for (auto proxy : ProxyNames)
        {
            if (ours.contains(proxy))
            {
                game.proxy = proxy;
                break;
            }
        }

        for (int pass = 0; pass < 2 && game.proxy.empty(); pass++)
        {
            for (auto proxy : ProxyNames)
            {
                bool imported = std::find(imports.begin(), imports.end(), proxy) != imports.end();

                if ((pass == 1 || imported) && !fs::exists(game.exeDir / proxy, ec))
                {
                    game.proxy = proxy;
                    break;
                }
            }
        }
    }

    if (game.proxy.empty() && game.blocker.empty())
        game.blocker = L"Every proxy dll name is taken by another mod.";
    else if (game.blocker.empty() && fs::exists(game.exeDir / game.proxy, ec) && !ours.contains(game.proxy))
        game.blocker = game.proxy + L" already exists here and is not ours (" + DescribeHook(game.exeDir / game.proxy) +
                       L"). Remove or rename it first.";

    return game;
}
