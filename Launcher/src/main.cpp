// Ray Regen Bridge launcher: finds games, installs / updates / uninstalls the mod into them and collects logs.

#include "GameScan.h"
#include "Installer.h"
#include "Profiles.h"
#include "Steam.h"
#include "Util.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr wchar_t WindowClass[] = L"RayRegenBridgeLauncher";
constexpr wchar_t Title[] = L"Ray Regen Bridge";
constexpr UINT WM_APP_SCANNED = WM_APP + 1;

enum ControlId
{
    IdList = 100,
    IdDetails,
    IdStatus,
    IdOnlyRR,
    IdInstall,
    IdUninstall,
    IdLaunch,
    IdOpenFolder,
    IdCollectLogs,
    IdAddFolder,
    IdRefresh,
};

struct App
{
    HWND window = nullptr;
    HWND list = nullptr;
    HWND details = nullptr;
    HWND status = nullptr;
    HWND onlyRR = nullptr;
    std::vector<HWND> buttons;
    HFONT font = nullptr;

    Profiles profiles;
    fs::path payload;
    fs::path settings; // launcher.ini: extra game folders

    std::vector<GameInfo> games;   // everything scanned
    std::vector<size_t> shown;     // indices into games, in list order
    bool scanning = false;
    std::mutex scanMutex;
    std::vector<GameInfo> scanResult;
};

App app;

// ---- Settings ----

std::vector<fs::path> ExtraFolders()
{
    std::vector<fs::path> folders;
    int count = GetPrivateProfileIntW(L"Folders", L"Count", 0, app.settings.c_str());

    for (int i = 0; i < count; i++)
    {
        wchar_t buffer[MAX_PATH] {};
        GetPrivateProfileStringW(L"Folders", std::to_wstring(i).c_str(), L"", buffer, MAX_PATH, app.settings.c_str());

        if (buffer[0] != L'\0')
            folders.emplace_back(buffer);
    }

    return folders;
}

void AddExtraFolder(const fs::path& folder)
{
    auto folders = ExtraFolders();

    for (const auto& existing : folders)
    {
        if (Lower(existing.wstring()) == Lower(folder.wstring()))
            return;
    }

    WritePrivateProfileStringW(L"Folders", std::to_wstring(folders.size()).c_str(), folder.c_str(),
                               app.settings.c_str());
    WritePrivateProfileStringW(L"Folders", L"Count", std::to_wstring(folders.size() + 1).c_str(),
                               app.settings.c_str());
}

// ---- Text ----

std::wstring StatusText(const GameInfo& game)
{
    if (game.installed)
        return L"Installed (" + game.installedVersion + L")";

    if (!game.blocker.empty())
        return L"Blocked";

    return game.hasRayReconstruction ? L"Ready to install" : L"Not supported";
}

std::wstring DetailsText(const GameInfo& game)
{
    std::wstring text = game.name + L"\r\n\r\n";
    text += L"Folder: " + game.root.wstring() + L"\r\n";
    text += L"Exe: " + (game.exe.empty() ? L"(not found)" : game.exe.wstring()) + L"\r\n";
    text += L"Profile: " + std::wstring(game.profile ? L"yes" : L"no, detected automatically") + L"\r\n\r\n";

    text += L"Ray Reconstruction (nvngx_dlssd.dll): " + std::wstring(game.hasRayReconstruction ? L"yes" : L"no") +
            L"\r\n";
    text += L"Streamline: " + std::wstring(game.hasStreamline ? L"yes" : L"no") + L"\r\n";
    text += L"Imports d3d12.dll: " + std::wstring(game.importsD3D12 ? L"yes" : L"no (may still load it later)") +
            L"\r\n";
    text += L"Installs as: " + (game.proxy.empty() ? L"-" : game.proxy) + L"\r\n";

    if (!game.conflicts.empty())
    {
        text += L"\r\nOther mods hooked here:\r\n";

        for (const auto& conflict : game.conflicts)
            text += L"  " + conflict + L"\r\n";
    }

    if (!game.hasRayReconstruction && !game.exe.empty())
        text += L"\r\nThis game doesn't ship DLSS Ray Reconstruction, so it never asks for it and Ray Regen Bridge "
                L"has nothing to replace. Installing still gives you OptiScaler's upscaler.\r\n";

    if (game.profile != nullptr && !game.profile->notes.empty())
        text += L"\r\nNotes: " + game.profile->notes + L"\r\n";

    if (!game.blocker.empty())
        text += L"\r\nCan't install: " + game.blocker + L"\r\n";

    text += L"\r\nIn the game: turn on ray tracing or path tracing, pick DLSS as the upscaler and turn on Ray "
            L"Reconstruction. Press Insert for the OptiScaler menu, section Ray Regen Bridge.\r\n";
    return text;
}

// ---- List ----

GameInfo* Selected()
{
    int row = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
    return row >= 0 && row < (int) app.shown.size() ? &app.games[app.shown[row]] : nullptr;
}

void UpdateButtons()
{
    auto game = Selected();

    EnableWindow(GetDlgItem(app.window, IdInstall), game && game->blocker.empty() && !app.scanning);
    SetDlgItemTextW(app.window, IdInstall, game && game->installed ? L"Update" : L"Install");
    EnableWindow(GetDlgItem(app.window, IdUninstall), game && game->installed && !app.scanning);
    EnableWindow(GetDlgItem(app.window, IdLaunch), game && !game->appId.empty());
    EnableWindow(GetDlgItem(app.window, IdOpenFolder), game && !game->exeDir.empty());
    EnableWindow(GetDlgItem(app.window, IdCollectLogs), game && !game->exeDir.empty());
    SetWindowTextW(app.details, game ? DetailsText(*game).c_str() : L"Select a game.");
}

void FillList()
{
    std::wstring selectedRoot;

    if (auto game = Selected())
        selectedRoot = game->root.wstring();

    bool onlyRR = Button_GetCheck(app.onlyRR) == BST_CHECKED;
    app.shown.clear();

    for (size_t i = 0; i < app.games.size(); i++)
    {
        if (!onlyRR || app.games[i].hasRayReconstruction || app.games[i].installed || app.games[i].profile)
            app.shown.push_back(i);
    }

    // Installed first, then the ones that can use it
    std::stable_sort(app.shown.begin(), app.shown.end(),
                     [](size_t a, size_t b)
                     {
                         auto rank = [](const GameInfo& g)
                         { return g.installed ? 0 : g.hasRayReconstruction ? 1 : g.profile ? 2 : 3; };
                         return rank(app.games[a]) < rank(app.games[b]);
                     });

    ListView_DeleteAllItems(app.list);
    int selectRow = -1;

    for (size_t row = 0; row < app.shown.size(); row++)
    {
        auto& game = app.games[app.shown[row]];
        LVITEMW item { LVIF_TEXT };
        item.iItem = (int) row;
        item.pszText = game.name.data();
        ListView_InsertItem(app.list, &item);

        std::wstring rr = game.hasRayReconstruction ? L"Yes" : L"No";
        ListView_SetItemText(app.list, (int) row, 1, rr.data());
        auto status = StatusText(game);
        ListView_SetItemText(app.list, (int) row, 2, status.data());

        if (game.root.wstring() == selectedRoot)
            selectRow = (int) row;
    }

    if (selectRow >= 0)
    {
        ListView_SetItemState(app.list, selectRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(app.list, selectRow, FALSE);
    }

    UpdateButtons();
}

void Rescan()
{
    if (app.scanning)
        return;

    app.scanning = true;
    SetWindowTextW(app.status, L"Looking for games...");
    UpdateButtons();

    std::thread(
        []
        {
            std::vector<GameInfo> games;

            for (const auto& steam : FindSteamGames())
                games.push_back(ScanGame(steam.name, steam.appId, steam.installDir, app.profiles));

            for (const auto& folder : ExtraFolders())
                games.push_back(ScanGame(folder.filename().wstring(), L"", folder, app.profiles));

            {
                std::scoped_lock lock(app.scanMutex);
                app.scanResult = std::move(games);
            }

            PostMessageW(app.window, WM_APP_SCANNED, 0, 0);
        })
        .detach();
}

// Rescan just one game after an action, keeping the list order
void RefreshGame(GameInfo& game)
{
    game = ScanGame(game.name, game.appId, game.root, app.profiles);
    FillList();
}

// ---- Actions ----

void ShowResult(const ActionResult& result, const wchar_t* action)
{
    MessageBoxW(app.window, result.message.c_str(), action, result.ok ? MB_ICONINFORMATION : MB_ICONWARNING);
    SetWindowTextW(app.status, (std::wstring(action) + (result.ok ? L": done" : L": failed")).c_str());
}

void OnInstall()
{
    auto game = Selected();

    if (game == nullptr)
        return;

    if (!game->hasRayReconstruction &&
        MessageBoxW(app.window,
                    L"This game doesn't ship DLSS Ray Reconstruction, so Ray Regen Bridge will never run in it. "
                    L"Install OptiScaler anyway?",
                    Title, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    ShowResult(Install(*game, app.payload), game->installed ? L"Update" : L"Install");
    RefreshGame(*game);
}

void OnUninstall()
{
    auto game = Selected();

    if (game == nullptr ||
        MessageBoxW(app.window, (L"Remove Ray Regen Bridge from " + game->name + L"?").c_str(), Title,
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    ShowResult(Uninstall(*game), L"Uninstall");
    RefreshGame(*game);
}

void OnLaunch()
{
    if (auto game = Selected(); game && !game->appId.empty())
        ShellExecuteW(app.window, L"open", (L"steam://rungameid/" + game->appId).c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
}

void OnOpenFolder()
{
    if (auto game = Selected(); game && !game->exeDir.empty())
        ShellExecuteW(app.window, L"open", game->exeDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OnCollectLogs()
{
    auto game = Selected();

    if (game == nullptr)
        return;

    fs::path zip;
    auto result = CollectLogs(*game, zip);
    ShowResult(result, L"Collect logs");

    if (result.ok)
        ShellExecuteW(app.window, L"open", L"explorer.exe", (L"/select,\"" + zip.wstring() + L"\"").c_str(), nullptr,
                      SW_SHOWNORMAL);
}

void OnAddFolder()
{
    IFileOpenDialog* dialog = nullptr;

    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return;

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Game folder (where the game is installed)");

    IShellItem* item = nullptr;
    PWSTR path = nullptr;

    if (SUCCEEDED(dialog->Show(app.window)) && SUCCEEDED(dialog->GetResult(&item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
    {
        AddExtraFolder(path);
        CoTaskMemFree(path);
        Rescan();
    }

    if (item)
        item->Release();

    dialog->Release();
}

// ---- Window ----

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0)
{
    HWND hwnd = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, app.window,
                                (HMENU) (INT_PTR) id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM) app.font, TRUE);
    return hwnd;
}

void Layout()
{
    RECT rc;
    GetClientRect(app.window, &rc);
    UINT dpi = GetDpiForWindow(app.window);
    auto s = [dpi](int v) { return MulDiv(v, dpi, 96); };

    int margin = s(10), buttonH = s(30), statusH = s(20);
    int width = rc.right - 2 * margin;
    int listW = width * 55 / 100;
    int top = margin + s(24);
    int bottom = rc.bottom - margin - buttonH - margin - statusH;

    MoveWindow(app.onlyRR, margin, margin, listW, s(20), TRUE);
    MoveWindow(app.list, margin, top, listW, bottom - top, TRUE);
    MoveWindow(app.details, margin * 2 + listW, top, width - listW - margin, bottom - top, TRUE);

    int x = margin;
    int y = bottom + margin;
    int buttonW = (width - margin * ((int) app.buttons.size() - 1)) / (int) app.buttons.size();

    for (auto button : app.buttons)
    {
        MoveWindow(button, x, y, buttonW, buttonH, TRUE);
        x += buttonW + margin;
    }

    MoveWindow(app.status, margin, y + buttonH + s(4), width, statusH, TRUE);

    ListView_SetColumnWidth(app.list, 0, listW - s(220));
    ListView_SetColumnWidth(app.list, 1, s(70));
    ListView_SetColumnWidth(app.list, 2, s(140));
}

void CreateControls()
{
    NONCLIENTMETRICSW metrics { sizeof(metrics) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, GetDpiForWindow(app.window));
    app.font = CreateFontIndirectW(&metrics.lfMessageFont);

    app.onlyRR = MakeControl(WC_BUTTONW, L"Only games with Ray Reconstruction", BS_AUTOCHECKBOX | WS_TABSTOP, IdOnlyRR);
    Button_SetCheck(app.onlyRR, BST_CHECKED);

    app.list = MakeControl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IdList,
                           WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    const wchar_t* columns[] = { L"Game", L"RR", L"Status" };

    for (int i = 0; i < 3; i++)
    {
        LVCOLUMNW column { LVCF_TEXT | LVCF_WIDTH };
        column.pszText = (LPWSTR) columns[i];
        column.cx = 100;
        ListView_InsertColumn(app.list, i, &column);
    }

    app.details = MakeControl(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, IdDetails,
                              WS_EX_CLIENTEDGE);

    const std::pair<int, const wchar_t*> buttons[] = { { IdInstall, L"Install" },
                                                       { IdUninstall, L"Uninstall" },
                                                       { IdLaunch, L"Launch (Steam)" },
                                                       { IdOpenFolder, L"Open folder" },
                                                       { IdCollectLogs, L"Collect logs" },
                                                       { IdAddFolder, L"Add game folder..." },
                                                       { IdRefresh, L"Refresh" } };

    for (const auto& [id, text] : buttons)
        app.buttons.push_back(MakeControl(WC_BUTTONW, text, BS_PUSHBUTTON | WS_TABSTOP, id));

    app.status = MakeControl(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, IdStatus);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        app.window = hwnd;
        CreateControls();
        Layout();
        Rescan();
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_GETMINMAXINFO:
    {
        UINT dpi = GetDpiForWindow(hwnd);
        auto info = (MINMAXINFO*) lParam;
        info->ptMinTrackSize = { MulDiv(900, dpi, 96), MulDiv(480, dpi, 96) };
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        // Read-only edit: keep a white background
        if ((HWND) lParam == app.details)
            return (LRESULT) GetSysColorBrush(COLOR_WINDOW);
        break;

    case WM_APP_SCANNED:
    {
        {
            std::scoped_lock lock(app.scanMutex);
            app.games = std::move(app.scanResult);
        }

        app.scanning = false;
        FillList();

        size_t rr = std::count_if(app.games.begin(), app.games.end(),
                                  [](const GameInfo& g) { return g.hasRayReconstruction; });
        SetWindowTextW(app.status, (std::to_wstring(app.games.size()) + L" games found, " + std::to_wstring(rr) +
                                    L" with Ray Reconstruction. Payload: " + PayloadVersion(app.payload))
                                       .c_str());
        return 0;
    }

    case WM_NOTIFY:
    {
        auto header = (NMHDR*) lParam;

        if (header->idFrom == IdList && header->code == LVN_ITEMCHANGED)
            UpdateButtons();

        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IdOnlyRR:
            FillList();
            break;
        case IdInstall:
            OnInstall();
            break;
        case IdUninstall:
            OnUninstall();
            break;
        case IdLaunch:
            OnLaunch();
            break;
        case IdOpenFolder:
            OnOpenFolder();
            break;
        case IdCollectLogs:
            OnCollectLogs();
            break;
        case IdAddFolder:
            OnAddFolder();
            break;
        case IdRefresh:
            if (!app.scanning)
            {
                // Games point into the profiles, drop them before reloading
                app.games.clear();
                FillList();
                app.profiles.Load(LauncherDir() / L"profiles.ini");
                Rescan();
            }
            break;
        }
        return 0;

    case WM_DPICHANGED:
    {
        auto rect = (RECT*) lParam;
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

// RayRegenBridge.exe --scan <file>: writes what the launcher detects for every game, no window
int ScanToFile(const fs::path& file)
{
    std::wstring text;

    for (const auto& steam : FindSteamGames())
    {
        auto game = ScanGame(steam.name, steam.appId, steam.installDir, app.profiles);
        text += L"==== " + StatusText(game) + L"\r\n" + DetailsText(game) + L"\r\n";
    }

    HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);

    if (out == INVALID_HANDLE_VALUE)
        return 1;

    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int) text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int) text.size(), utf8.data(), size, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(out, utf8.data(), (DWORD) utf8.size(), &written, nullptr);
    CloseHandle(out);
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc == 3 && std::wstring(argv[1]) == L"--scan")
    {
        app.profiles.Load(LauncherDir() / L"profiles.ini");
        int result = ScanToFile(argv[2]);
        LocalFree(argv);
        return result;
    }

    // --install / --uninstall <game folder>: the same actions without the window, exit code 0 on success
    if (argc == 3 && (std::wstring(argv[1]) == L"--install" || std::wstring(argv[1]) == L"--uninstall"))
    {
        app.profiles.Load(LauncherDir() / L"profiles.ini");
        fs::path folder = argv[2];
        auto game = ScanGame(folder.filename().wstring(), L"", folder, app.profiles);
        auto result = std::wstring(argv[1]) == L"--install" ? Install(game, LauncherDir() / L"payload")
                                                              : Uninstall(game);
        LocalFree(argv);
        return result.ok ? 0 : 1;
    }

    LocalFree(argv);

    INITCOMMONCONTROLSEX controls { sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);

    app.payload = LauncherDir() / L"payload";
    app.settings = LauncherDir() / L"launcher.ini";
    app.profiles.Load(LauncherDir() / L"profiles.ini");

    WNDCLASSEXW wc { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = WindowClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, WindowClass, Title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(1100, dpi, 96), MulDiv(640, dpi, 96), nullptr, nullptr, instance, nullptr);
    ShowWindow(hwnd, show);

    MSG msg;

    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return 0;
}
