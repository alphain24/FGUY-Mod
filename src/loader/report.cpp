// Reports: when an online match goes out of sync, or the game crashed (found at the next start), the logs that
// tell why go into one zip in FGUYMod\reports\ - one file a player can send, instead of digging for logs.
//   <date>_desync.zip   this PC's match log (the session game's) and its lobby game's, and info.txt
//   <date>_crash.zip    the crashed game's log, and info.txt
// The ONLINE screen's REPORTS button opens the folder (FGUYMOD_OpenReports); after an out-of-sync match the lobby
// says a report was saved (FGUYMOD_ReportNew). The zip is made by Windows' own tar (Windows 10 and later).

#include "common.h"

#include <shellapi.h>

#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace {

std::wstring ReportsDir() {
    std::wstring dir = std::wstring(g_modDir) + L"\\reports";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t s[40];
    swprintf_s(s, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

bool Zip(const std::wstring& zip, const std::wstring& dir) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -a -c -f \"" + zip + L"\" -C \"" + dir + L"\" .";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

void WriteText(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, text.data(), (DWORD)text.size(), &put, nullptr);
    CloseHandle(f);
}

struct Job {
    std::wstring kind;            // desync / crash
    std::string info;             // info.txt
    std::vector<std::wstring> logs;   // full paths
    bool tellLobby;               // the lobby game says so after the match
};

void DeleteDir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

DWORD WINAPI MakeReport(LPVOID arg) {
    Job* job = static_cast<Job*>(arg);
    std::wstring dir = ReportsDir(), name = Stamp() + L"_" + job->kind, tmp = dir + L"\\tmp_" + name;
    CreateDirectoryW(tmp.c_str(), nullptr);
    for (const auto& log : job->logs) {
        const wchar_t* base = wcsrchr(log.c_str(), L'\\');
        CopyFileW(log.c_str(), (tmp + (base ? base : L"\\log.txt")).c_str(), FALSE);
    }
    WriteText(tmp + L"\\info.txt", job->info);
    std::wstring zip = dir + L"\\" + name + L".zip";
    bool ok = Zip(zip, tmp);
    DeleteDir(tmp);
    Log(L"Report: %s %s", ok ? L"saved" : L"COULDN'T be made -", zip.c_str());
    if (ok && job->tellLobby) {   // (the lobby game says so)
        char n8[96];
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, n8, sizeof(n8), nullptr, nullptr);
        WriteText(dir + L"\\new.txt", std::string(n8) + ".zip");
    }
    delete job;
    return 0;
}

std::wstring LogOf(DWORD pid) {
    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%s\\FGUYMod_%lu.log", g_modDir, pid);
    return p;
}

bool Running(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

// The log of a game that crashed: "CRASH at" in it.
bool HasCrash(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<char> buf(1 << 16);
    std::string carry;
    bool found = false;
    DWORD got = 0;
    while (!found && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0) {
        std::string chunk = carry + std::string(buf.data(), got);
        found = chunk.find("CRASH at") != std::string::npos;
        carry = chunk.size() > 16 ? chunk.substr(chunk.size() - 16) : chunk;
    }
    CloseHandle(f);
    return found;
}

}  // namespace

// netplay.cpp: the match went out of sync - this PC's report (in the background: the match is on its way out).
void ReportDesync(unsigned tick, const char* what) {
    static bool done = false;
    if (done) return;
    done = true;
    wchar_t kind[32];
    swprintf_s(kind, L"desync_player%d", NetplayLocalSlot() + 1);   // (one per player: several can share a folder)
    Job* job = new Job{kind, "", {LogOf(GetCurrentProcessId())}, true};
    DWORD lobby = HandoffLobbyPid();
    if (lobby) job->logs.push_back(LogOf(lobby));
    char info[512];
    sprintf_s(info, "FGUY Mod report: an online match went out of sync\r\nat tick %u (%s)\r\nthis PC: slot %d, process %lu "
                    "(lobby game %lu)\r\nSend this file to whoever looks into it - the other players' reports help too.\r\n",
              tick, what ? what : "", NetplayLocalSlot(), GetCurrentProcessId(), lobby);
    job->info = info;
    if (HANDLE t = CreateThread(nullptr, 0, MakeReport, job, 0, nullptr)) CloseHandle(t);
    else delete job;
}

// At start: games that crashed since (their logs say CRASH, they aren't running) - a report each, once.
void InitReports() {
    std::wstring dir = ReportsDir(), seenPath = dir + L"\\crashes_reported.txt";
    std::string seen;
    if (HANDLE f = CreateFileW(seenPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        f != INVALID_HANDLE_VALUE) {
        char buf[8192];
        DWORD got = 0;
        if (ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr)) seen.assign(buf, got);
        CloseHandle(f);
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(g_modDir) + L"\\FGUYMod_*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    std::string added;
    do {
        DWORD pid = wcstoul(fd.cFileName + 8, nullptr, 10);
        ULONGLONG written = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (!pid || pid == GetCurrentProcessId() || now - written > 3ULL * 24 * 3600 * 10000000ULL) continue;   // (3 days)
        char key[64];
        sprintf_s(key, "%lu@%llu;", pid, written);
        if (seen.find(key) != std::string::npos || Running(pid)) continue;
        std::wstring path = std::wstring(g_modDir) + L"\\" + fd.cFileName;
        if (!HasCrash(path)) continue;
        added += key;
        wchar_t kind[32];
        swprintf_s(kind, L"crash_%lu", pid);   // (several found at once: one each)
        Job* job = new Job{kind, "FGUY Mod report: the game crashed\r\nits log is in here (CRASH at ... and the lines before)\r\n",
                           {path}, false};
        if (HANDLE t = CreateThread(nullptr, 0, MakeReport, job, 0, nullptr)) CloseHandle(t);
        else delete job;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!added.empty()) {
        Log(L"Report: crashed games found - reports made");
        WriteText(seenPath, seen + added);
    }
}

// FGUYMOD_ReportCount: how many reports there are.
int ReportCount() {
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((ReportsDir() + L"\\*.zip").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do ++n; while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}

// FGUYMOD_ReportNew: a report saved during the last match ("" none) - said once.
const char* ReportNew() {
    static std::string name;
    name.clear();
    std::wstring path = ReportsDir() + L"\\new.txt";
    if (HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr); f != INVALID_HANDLE_VALUE) {
        char buf[128] = {};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(f);
        DeleteFileW(path.c_str());
        name = buf;
    }
    return name.c_str();
}

// FGUYMOD_OpenReports: the folder in Explorer.
void ReportOpenFolder() {
    std::wstring dir = ReportsDir();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
