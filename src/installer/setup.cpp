// FGUY Mod setup: one file for players. It carries the release zip (resource 1, tools/make_release.ps1 puts it in),
// finds the game's folder (next to itself, Steam's libraries, the usual places - or asks), and installs the mod
// there: dinput8.dll and the FGUYMod folder. Updating keeps the player's FGUYMod\FGUYMod.ini and their mods;
// uninstalling moves the mod to the Recycle Bin. A game folder in Program Files needs admin rights: then it starts
// itself again with them (Windows asks).
//   FGUYMod-<version>-Setup.exe [--install <folder>]   (--install: that folder, no questions - the admin restart)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

const wchar_t* kTitle = L"FGUY Mod setup";
std::wstring g_version = L"";   // from resource 2 (the release's version)

// A path Windows takes however long it is (\\?\ - a game folder deep inside other folders went past 260 characters).
std::wstring Long(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0 || p.size() < 3 || p[1] != L':') return p;
    return L"\\\\?\\" + p;
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(Long(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(Long(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool IsGameFolder(const std::wstring& d) { return !d.empty() && IsFile(d + L"\\FGUY.exe"); }
bool ModInstalled(const std::wstring& d) { return IsFile(d + L"\\dinput8.dll") && IsDir(d + L"\\FGUYMod"); }

std::wstring Parent(std::wstring p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = 0) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | view, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

// Steam's library folders (steamapps\libraryfolders.vdf: "path" "D:\\SteamLibrary" lines).
std::vector<std::wstring> SteamLibraries() {
    std::vector<std::wstring> out;
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) return out;
    for (auto& c : steam) if (c == L'/') c = L'\\';
    out.push_back(steam);
    HANDLE f = CreateFileW((steam + L"\\steamapps\\libraryfolders.vdf").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::string text(GetFileSize(f, nullptr), '\0');
    DWORD got = 0;
    ReadFile(f, &text[0], (DWORD)text.size(), &got, nullptr);
    CloseHandle(f);
    for (size_t at = 0; (at = text.find("\"path\"", at)) != std::string::npos;) {
        size_t a = text.find('"', at + 6), b = a == std::string::npos ? a : text.find('"', a + 1);
        if (b == std::string::npos) break;
        std::string p = text.substr(a + 1, b - a - 1);
        std::string clean;
        for (size_t i = 0; i < p.size(); ++i) {   // ("D:\\SteamLibrary": doubled backslashes)
            if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
            clean += p[i];
        }
        wchar_t w[1024];
        if (MultiByteToWideChar(CP_UTF8, 0, clean.c_str(), -1, w, 1024)) out.push_back(w);
        at = b + 1;
    }
    return out;
}

// Where the game probably is (the first folder with FGUY.exe), or "".
std::wstring FindGame() {
    std::vector<std::wstring> tries;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    tries.push_back(Parent(self));   // (the setup put in the game's folder)
    for (auto& lib : SteamLibraries()) tries.push_back(lib + L"\\steamapps\\common\\Family Guy Back to the Multiverse");
    // installed programs that say so
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
        HKEY un;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                          KEY_READ | view, &un) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD n = 256;
            if (RegEnumKeyExW(un, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + name;
            REGSAM rview = view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY;
            std::wstring display = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"DisplayName", rview);
            if (display.find(L"Family Guy") == std::wstring::npos) continue;
            std::wstring at = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"InstallLocation", rview);
            if (!at.empty()) tries.push_back(at);
        }
        RegCloseKey(un);
    }
    for (const wchar_t* base : {L"C:\\Program Files (x86)", L"C:\\Program Files", L"C:\\Games", L"D:\\Games"}) {
        tries.push_back(std::wstring(base) + L"\\Steam\\steamapps\\common\\Family Guy Back to the Multiverse");
        tries.push_back(std::wstring(base) + L"\\Activision\\Family Guy Back to the Multiverse");
        tries.push_back(std::wstring(base) + L"\\Family Guy Back to the Multiverse");
    }
    for (auto& t : tries) {
        while (!t.empty() && (t.back() == L'\\' || t.back() == L'/')) t.pop_back();
        if (IsGameFolder(t)) return t;
    }
    return L"";
}

std::wstring PickFolder(HWND owner) {
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Pick the game's folder (the one with FGUY.exe)");
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { out = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    d->Release();
    return out;
}

// Test mode (FGUYMOD_SETUP_TEST=<button id>,<button id>,...: the answers, in order; every message goes to
// FGUYMOD_SETUP_LOG instead of the screen).
bool TestAnswer(const std::wstring& heading, const std::wstring& text, int* answer) {
    static bool read = false, on = false;
    static std::vector<int> answers;
    static size_t next = 0;
    static std::wstring log;
    if (!read) {
        read = true;
        wchar_t env[256], path[MAX_PATH];
        if (GetEnvironmentVariableW(L"FGUYMOD_SETUP_TEST", env, 256)) {
            on = true;
            for (wchar_t* p = env; *p;) {
                answers.push_back(_wtoi(p));
                while (*p && *p != L',') ++p;
                if (*p) ++p;
            }
            if (GetEnvironmentVariableW(L"FGUYMOD_SETUP_LOG", path, MAX_PATH)) log = path;
        }
    }
    if (!on) return false;
    *answer = next < answers.size() ? answers[next++] : IDCANCEL;
    HANDLE f = log.empty() ? INVALID_HANDLE_VALUE
                           : CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::wstring line = heading + L" | " + text + L" -> " + std::to_wstring(*answer) + L"\r\n";
        char buf[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        DWORD put;
        if (n > 1) WriteFile(f, buf, n - 1, &put, nullptr);
        CloseHandle(f);
    }
    return true;
}

// A dialog with our own buttons; returns the chosen button's id (IDCANCEL: closed).
int Ask(const std::wstring& heading, const std::wstring& text, const std::vector<std::pair<int, std::wstring>>& buttons,
        PCWSTR icon = nullptr) {
    int test;
    if (TestAnswer(heading, text, &test)) return test;
    std::vector<TASKDIALOG_BUTTON> b;
    for (auto& x : buttons) b.push_back({x.first, x.second.c_str()});
    TASKDIALOGCONFIG c{sizeof(c)};
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.pszWindowTitle = kTitle;
    c.pszMainIcon = icon;
    c.pszMainInstruction = heading.c_str();
    c.pszContent = text.c_str();
    c.cButtons = (UINT)b.size();
    c.pButtons = b.data();
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    int chosen = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &chosen, nullptr, nullptr))) chosen = IDCANCEL;
    return chosen;
}
void Tell(const std::wstring& heading, const std::wstring& text, PCWSTR icon = TD_INFORMATION_ICON) {
    int test;
    if (TestAnswer(heading, text, &test)) return;
    TaskDialog(nullptr, nullptr, kTitle, heading.c_str(), text.c_str(), TDCBF_OK_BUTTON, icon, nullptr);
}

bool CanWrite(const std::wstring& dir) {
    std::wstring probe = dir + L"\\fguymod_setup_probe.tmp";
    HANDLE f = CreateFileW(Long(probe).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

// The game running holds dinput8.dll: it can't be replaced then.
bool GameRunning(const std::wstring& dir) {
    std::wstring dll = dir + L"\\dinput8.dll";
    if (!IsFile(dll)) return false;
    HANDLE f = CreateFileW(Long(dll).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(f);
    return false;
}

bool WriteFileAll(const std::wstring& path, const void* data, DWORD size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, size, &put, nullptr) && put == size;
    CloseHandle(f);
    return ok;
}

// Copies the tree `from` into `to`, files replaced - except the player's own settings (FGUYMod\FGUYMod.ini) if
// they have one. False (and `failed` = the file) if a file couldn't be written.
bool CopyTree(const std::wstring& from, const std::wstring& to, const std::wstring& rel, std::wstring* failed) {
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring r = rel.empty() ? name : rel + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = CopyTree(from + L"\\" + name, to + L"\\" + name, r, failed) && ok;
        } else {
            if (_wcsicmp(r.c_str(), L"FGUYMod\\FGUYMod.ini") == 0 && IsFile(to + L"\\" + name)) continue;   // (theirs)
            if (!CopyFileW((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), FALSE)) {
                ok = false;
                if (failed && failed->empty()) *failed = r;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Runs System32's tar (Windows 10 and later: it unpacks zips) and waits. Its exit code (or -1).
int RunTar(const std::wstring& zip, const std::wstring& into) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + into + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// Restarts this setup with admin rights for `dir` (Windows asks the player) and waits for it. True if it ran.
bool InstallAsAdmin(const std::wstring& dir) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring args = L"--install \"" + dir + L"\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);
    return true;
}

// Windows Settings > Apps lists the mod (for this Windows user): removing it there runs FGUYMod\Uninstall.exe (this
// setup without the mod inside - tools/make_release.ps1 puts it in the zip).
const wchar_t* kAppsKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FGUYMod";

void Register(const std::wstring& dir) {
    std::wstring un = dir + L"\\FGUYMod\\Uninstall.exe";
    if (!IsFile(un)) return;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    auto str = [k](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    auto num = [k](const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), 4); };
    str(L"DisplayName", L"FGUY Mod (Family Guy: Back to the Multiverse)");
    str(L"DisplayVersion", g_version);
    str(L"Publisher", L"FGUY Mod");
    str(L"InstallLocation", dir);
    str(L"DisplayIcon", dir + L"\\FGUY.exe");
    str(L"UninstallString", L"\"" + un + L"\" --uninstall \"" + dir + L"\"");
    str(L"URLInfoAbout", L"https://github.com/alphain24/FGUY-Mod");
    num(L"NoModify", 1);
    num(L"NoRepair", 1);
    RegCloseKey(k);
}

void Unregister(const std::wstring& dir) {
    std::wstring at = RegString(HKEY_CURRENT_USER, kAppsKey, L"InstallLocation");
    if (!at.empty() && _wcsicmp(at.c_str(), dir.c_str()) == 0) RegDeleteTreeW(HKEY_CURRENT_USER, kAppsKey);
}

// Restarts this program with admin rights and these arguments (Windows asks the player). True if it ran.
bool RunAsAdmin(const std::wstring& args) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);
    return true;
}

// Installs (or updates) the mod in `dir`. Says how it went.
bool Install(const std::wstring& dir, bool quiet) {
    while (GameRunning(dir)) {
        int c = Ask(L"Close the game first", L"Family Guy: Back to the Multiverse is running from this folder - the "
                    L"mod can't be installed while it is. Close the game, then click Try again.",
                    {{100, L"Try again"}}, TD_WARNING_ICON);
        if (c != 100) return false;
    }
    if (!CanWrite(dir)) {
        if (quiet) {
            Tell(L"Couldn't install", L"Even with admin rights, this folder can't be written to:\n" + dir, TD_ERROR_ICON);
            return false;
        }
        return InstallAsAdmin(dir);   // (it says how it went itself)
    }
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(1), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* zip = mem ? LockResource(mem) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!zip || !size) { Tell(L"This setup is damaged", L"Please download it again.", TD_ERROR_ICON); return false; }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"FGUYMod_setup_" + std::to_wstring(GetCurrentProcessId());
    DeleteTree(work);
    CreateDirectoryW(work.c_str(), nullptr);
    std::wstring zipPath = work + L"\\mod.zip", stage = work + L"\\files";
    CreateDirectoryW(stage.c_str(), nullptr);
    bool ok = WriteFileAll(zipPath, zip, size) && RunTar(zipPath, stage) == 0 && IsFile(stage + L"\\dinput8.dll");
    std::wstring failed;
    bool updating = ModInstalled(dir);
    if (ok) ok = CopyTree(Long(stage), Long(dir), L"", &failed);
    DeleteTree(work);
    if (!ok) {
        Tell(L"Couldn't install the mod", failed.empty() ? L"Unpacking it failed. Please download the setup again."
                                                         : L"This file couldn't be written: " + failed +
                                                               L"\nIs the game (or a copy of it) still running?",
             TD_ERROR_ICON);
        return false;
    }
    Register(dir);
    Tell(updating ? L"FGUY Mod updated to " + g_version : L"FGUY Mod " + g_version + L" installed",
         L"Start the game as usual.\n\nPlay online: Main Menu > ONLINE.\nSeparate mods: Main Menu > MODS.\nSettings: "
         L"FGUYMod\\FGUYMod.ini in the game's folder (open it with Notepad).\n\nGame folder: " + dir);
    return true;
}

bool Uninstall(const std::wstring& dir, bool quiet = false) {
    if (!CanWrite(dir) && !quiet) return RunAsAdmin(L"--uninstall-admin \"" + dir + L"\"");   // (it asks itself)
    int c = Ask(L"Remove FGUY Mod?", L"dinput8.dll and the FGUYMod folder go to the Recycle Bin - with your separate "
                L"mods and reports in it. The game and its saves stay as they are.\n\n" + dir,
                {{100, L"Remove it"}}, TD_WARNING_ICON);
    if (c != 100) return false;
    while (GameRunning(dir)) {
        if (Ask(L"Close the game first", L"The game is running from this folder. Close it, then click Try again.",
                {{100, L"Try again"}}, TD_WARNING_ICON) != 100) return false;
    }
    std::wstring list = dir + L"\\dinput8.dll" + L'\0' + dir + L"\\FGUYMod" + L'\0';
    if (IsFile(dir + L"\\INSTALL.txt")) list += dir + L"\\INSTALL.txt" + L'\0';   // (ours too: it came with the mod)
    list += L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
        Tell(L"Couldn't remove the mod", L"Maybe it needs admin rights: delete dinput8.dll and the FGUYMod folder in\n" +
             dir + L"\nyourself.", TD_ERROR_ICON);
        return false;
    }
    Unregister(dir);
    Tell(L"FGUY Mod removed", L"It's in the Recycle Bin, if you change your mind.");
    return true;
}

std::wstring Version() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const char* v = mem ? static_cast<const char*>(LockResource(mem)) : nullptr;
    DWORD n = res ? SizeofResource(nullptr, res) : 0;
    std::wstring out;
    for (DWORD i = 0; v && i < n && v[i] && v[i] != '\r' && v[i] != '\n'; ++i) out += (wchar_t)v[i];
    return out;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    g_version = Version();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && wcscmp(argv[1], L"--install") == 0) return Install(argv[2], true) ? 0 : 1;   // (the admin restart)
    if (argc >= 3 && wcscmp(argv[1], L"--uninstall-admin") == 0) return Uninstall(argv[2], true) ? 0 : 1;   // (same)

    // Removing (Windows Settings > Apps, or FGUYMod\Uninstall.exe opened): the mod's folder goes, and this program with
    // it if it runs from there - it can't while it runs, so a copy of it in the temp folder does the removing.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    bool hasMod = FindResourceW(nullptr, MAKEINTRESOURCEW(1), RT_RCDATA) != nullptr;
    std::wstring removeDir;
    if (argc >= 3 && wcscmp(argv[1], L"--uninstall") == 0) removeDir = argv[2];
    else if (!hasMod) removeDir = Parent(Parent(self));   // (Uninstall.exe, opened: <game>\FGUYMod\Uninstall.exe)
    if (!removeDir.empty()) {
        while (!removeDir.empty() && (removeDir.back() == L'\\' || removeDir.back() == L'/')) removeDir.pop_back();
        std::wstring inside = removeDir + L"\\FGUYMod\\";
        if (_wcsnicmp(self, inside.c_str(), inside.size()) == 0) {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            std::wstring copy = std::wstring(tmp) + L"FGUYMod-Uninstall.exe";
            if (CopyFileW(self, copy.c_str(), FALSE)) {
                std::wstring cmd = L"\"" + copy + L"\" --uninstall \"" + removeDir + L"\"";
                STARTUPINFOW si{sizeof(si)};
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    return 0;
                }
            }
        }
        if (!ModInstalled(removeDir)) {
            Tell(L"FGUY Mod isn't installed there", removeDir, TD_INFORMATION_ICON);
            Unregister(removeDir);
            return 0;
        }
        return Uninstall(removeDir) ? 0 : 1;
    }

    std::wstring dir = FindGame();
    for (;;) {
        if (dir.empty()) {
            int c = Ask(L"Where's the game?", L"Setup couldn't find Family Guy: Back to the Multiverse. Pick its folder - "
                        L"the one with FGUY.exe in it.", {{101, L"Pick the game's folder"}});
            if (c != 101) return 0;
            dir = PickFolder(nullptr);
            if (dir.empty()) continue;
            if (!IsGameFolder(dir)) {
                Tell(L"That's not the game's folder", L"There's no FGUY.exe in\n" + dir, TD_WARNING_ICON);
                dir.clear();
                continue;
            }
        }
        bool installed = ModInstalled(dir);
        std::vector<std::pair<int, std::wstring>> buttons = {
            {100, installed ? L"Update FGUY Mod to " + g_version : L"Install FGUY Mod " + g_version},
            {101, L"Choose another folder"}};
        if (installed) buttons.push_back({102, L"Remove FGUY Mod"});
        int c = Ask(installed ? L"FGUY Mod is installed here" : L"Found the game", dir, buttons);
        if (c == 100) { Install(dir, false); return 0; }
        if (c == 102) { Uninstall(dir); return 0; }
        if (c != 101) return 0;
        std::wstring picked = PickFolder(nullptr);
        if (picked.empty()) continue;
        if (!IsGameFolder(picked)) {
            Tell(L"That's not the game's folder", L"There's no FGUY.exe in\n" + picked, TD_WARNING_ICON);
            continue;
        }
        dir = picked;
    }
}
