// Updates: the lobby game asks GitHub for the mod's latest release (FGUYMod.ini [Update] Repo=<owner>/<name>, default
// alphain24/FGUY-Mod; Repo= with nothing: no checks) and the ONLINE screen says when there's a newer one. Installing (the player asks): the release's
// FGUYMod-<version>.zip and its FGUYMod-<version>.zip.sha256 are downloaded, the zip's SHA-256 checked, the zip
// unpacked (Windows' own tar) and its files copied into the game folder - never over FGUYMod\FGUYMod.ini (the
// player's settings). The running dinput8.dll can't be overwritten, but it can be renamed: dinput8.dll.old (deleted at
// the next start), the new one in its place - the next start runs the new version.
// Test knob: FGUYMOD_UPDATE_URL=<url of a releases/latest JSON> (e.g. a local test server, http allowed).
//
// The mod's lobby server: the same check reads server.txt in the repo (its one address:port line), so the server can
// move without a release - kept in FGUYMod\lobby_server.txt for when GitHub can't be reached (some providers block
// raw.githubusercontent.com). online.cpp uses it unless the player named a server of their own.
// Test knob: FGUYMOD_SERVER_URL=<url of a server.txt>.

#include "common.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

std::mutex g_lock;
std::string g_state = "off";      // off, checking, current, available, downloading, ready, failed
std::string g_latest;             // the newest version
std::string g_message;            // what went wrong / where it's at
std::string g_zipUrl, g_shaUrl;
std::atomic<bool> g_busy{false};

void Set(const std::string& state, const std::string& message = "") {
    std::lock_guard<std::mutex> lock(g_lock);
    g_state = state;
    g_message = message;
}

bool Get(const std::wstring& url, std::vector<unsigned char>& out, std::string* error) {
    URL_COMPONENTS u = {sizeof(u)};
    wchar_t host[256], path[2048];
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) { *error = "bad address"; return false; }
    HINTERNET s = WinHttpOpen(L"FGUYMod-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { *error = "no internet"; return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    bool ok = r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json, */*\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA,
                                      0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr);
    DWORD code = 0, size = sizeof(code);
    if (ok) WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                                &size, WINHTTP_NO_HEADER_INDEX);
    if (ok && code != 200) { ok = false; *error = "the server said " + std::to_string(code); }
    else if (!ok) *error = "couldn't reach the server";
    while (ok) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; *error = "the download broke off"; break; }
        if (!avail) break;
        size_t at = out.size();
        out.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(r, out.data() + at, avail, &got)) { ok = false; *error = "the download broke off"; break; }
        out.resize(at + got);
        if (out.size() > 200u * 1024 * 1024) { ok = false; *error = "too big"; break; }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// The string value of "key": "..." after `from` in a JSON text ("" if none) - enough for GitHub's release JSON.
std::string JsonString(const std::string& json, const std::string& key, size_t from = 0, size_t* end = nullptr) {
    size_t k = json.find("\"" + key + "\"", from);
    if (k == std::string::npos) return "";
    size_t q = json.find('"', json.find(':', k) + 1);
    if (q == std::string::npos) return "";
    std::string v;
    size_t i = q + 1;
    for (; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;
        v += json[i];
    }
    if (end) *end = i;
    return v;
}

std::vector<int> Numbers(const std::string& v) {
    std::vector<int> n;
    int cur = -1;
    for (char ch : v) {
        if (isdigit((unsigned char)ch)) cur = (cur < 0 ? 0 : cur * 10) + (ch - '0');
        else if (cur >= 0) { n.push_back(cur); cur = -1; }
    }
    if (cur >= 0) n.push_back(cur);
    return n;
}

bool Newer(const std::string& a, const std::string& b) {   // a newer than b
    auto x = Numbers(a), y = Numbers(b);
    for (size_t i = 0; i < x.size() || i < y.size(); ++i) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p > q;
    }
    return false;
}

std::wstring Wide(const std::string& s) {
    std::wstring w(s.size(), 0);
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size());
    w.resize(n > 0 ? n : 0);
    return w;
}

std::wstring LatestUrl() {
    wchar_t env[512];
    if (GetEnvironmentVariableW(L"FGUYMOD_UPDATE_URL", env, 512)) return env;
    wchar_t repo[200];
    SettingString(L"Update", L"Repo", nullptr, L"alphain24/FGUY-Mod", repo, 200);   // (the mod's own; empty: no checks)
    if (!repo[0]) return L"";
    return L"https://api.github.com/repos/" + std::wstring(repo) + L"/releases/latest";
}

std::string g_lobbyServer;   // the mod's lobby server as server.txt names it ("": not known - online.cpp's own)

std::wstring LobbyServerFile() { return std::wstring(g_gameDir) + L"\\FGUYMod\\lobby_server.txt"; }

std::wstring ServerUrl() {
    wchar_t env[512];
    if (GetEnvironmentVariableW(L"FGUYMOD_SERVER_URL", env, 512)) return env;
    wchar_t repo[200];
    SettingString(L"Update", L"Repo", nullptr, L"alphain24/FGUY-Mod", repo, 200);
    if (!repo[0]) return L"";
    return L"https://raw.githubusercontent.com/" + std::wstring(repo) + L"/HEAD/server.txt";
}

// server.txt's address: its first line that isn't empty or a # comment, if it's an address:port ("" if not).
std::string ServerLine(const std::string& text) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
        size_t from = 0;
        while (from < line.size() && isspace((unsigned char)line[from])) ++from;
        line = line.substr(from);
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);   // (a UTF-8 byte-order mark)
        if (line.empty() || line[0] == '#') continue;
        size_t colon = line.rfind(':');
        bool ok = line.size() <= 100 && colon != 0 && line.find_first_not_of(
                      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:") == std::string::npos;
        if (ok && colon != std::string::npos) {
            int port = atoi(line.c_str() + colon + 1);
            ok = colon + 1 < line.size() && line.find(':') == colon && port > 0 && port < 65536;
        }
        return ok ? line : "";
    }
    return "";
}

void CheckServer() {
    std::wstring url = ServerUrl();
    if (url.empty()) return;
    std::vector<unsigned char> body;
    std::string error;
    if (!Get(url, body, &error)) {
        Log(L"Online: couldn't read the lobby server's address from GitHub (%S) - the last one known", error.c_str());
        return;
    }
    std::string line = ServerLine(std::string(body.begin(), body.end()));
    if (line.empty()) {
        Log(L"Online: server.txt has no address:port line - the last one known");
        return;
    }
    std::lock_guard<std::mutex> lock(g_lock);
    if (line == g_lobbyServer) return;
    Log(L"Online: the mod's lobby server is %S (server.txt)", line.c_str());
    g_lobbyServer = line;
    FILE* f = nullptr;
    if (_wfopen_s(&f, LobbyServerFile().c_str(), L"wb") == 0 && f) {
        fputs((line + "\n").c_str(), f);
        fclose(f);
    }
}

DWORD WINAPI CheckThread(LPVOID) {
    CheckServer();   // (first: the ONLINE screen may want it soon)
    std::vector<unsigned char> body;
    std::string error;
    if (LatestUrl().empty()) {
        g_busy = false;
        Set("off");
        return 0;
    }
    if (!Get(LatestUrl(), body, &error)) {
        Log(L"Update: the check failed - %S", error.c_str());
        Set("failed", "Couldn't check for updates (" + error + ")");
        g_busy = false;
        return 0;
    }
    std::string json(body.begin(), body.end());
    std::string tag = JsonString(json, "tag_name");
    std::string zip, sha;
    for (size_t at = 0;;) {
        size_t end = 0;
        std::string url = JsonString(json, "browser_download_url", at, &end);
        if (url.empty()) break;
        at = end;
        std::string file = url.substr(url.find_last_of('/') + 1);
        if (file.rfind("FGUYMod-", 0) != 0) continue;
        if (file.size() > 4 && file.compare(file.size() - 4, 4, ".zip") == 0) zip = url;
        if (file.size() > 11 && file.compare(file.size() - 11, 11, ".zip.sha256") == 0) sha = url;
    }
    std::lock_guard<std::mutex> lock(g_lock);
    g_latest = tag;
    g_zipUrl = zip;
    g_shaUrl = sha;
    if (!tag.empty() && Newer(tag, ModVersion()) && !zip.empty() && !sha.empty()) {
        g_state = "available";
        g_message = "";
        Log(L"Update: %S is out (this is %S)", tag.c_str(), ModVersion());
    } else {
        g_state = "current";
        Log(L"Update: up to date (%S; the latest is %S)", ModVersion(), tag.empty() ? "unknown" : tag.c_str());
    }
    g_busy = false;
    return 0;
}

std::string Sha256Hex(const std::vector<unsigned char>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32] = {};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, const_cast<unsigned char*>(data.data()), (ULONG)data.size(), 0) == 0 &&
        BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
        char b[3];
        for (unsigned char d : digest) { sprintf_s(b, "%02x", d); hex += b; }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool Run(const std::wstring& cmd, const std::wstring& dir) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

std::string Narrow(const std::wstring& w) {
    char b[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

// Every file under `from` into the game folder (its path there the same), but the player's FGUYMod.ini.
int CopyTree(const std::wstring& from, const std::wstring& rel, std::string* error) {
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring src = from + L"\\" + fd.cFileName, r = rel + fd.cFileName;
        std::wstring dst = std::wstring(g_gameDir) + L"\\" + r;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CreateDirectoryW(dst.c_str(), nullptr);
            n += CopyTree(src, r + L"\\", error);
            continue;
        }
        if (_wcsicmp(r.c_str(), L"FGUYMod\\FGUYMod.ini") == 0) continue;   // (the player's settings stay theirs)
        if (_wcsicmp(r.c_str(), L"dinput8.dll") == 0) {   // (running: out of the way, the new one in its place)
            std::wstring old = dst + L".old";
            DeleteFileW(old.c_str());
            MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) ++n;
        else if (error->empty()) *error = "couldn't write " + Narrow(r);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

DWORD WINAPI InstallThread(LPVOID) {
    std::string zipUrl, shaUrl, version, error;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        zipUrl = g_zipUrl;
        shaUrl = g_shaUrl;
        version = g_latest;
    }
    Set("downloading", "Downloading " + version + "...");
    std::vector<unsigned char> zip, sha;
    if (!Get(Wide(shaUrl), sha, &error) || !Get(Wide(zipUrl), zip, &error)) {
        Set("failed", "The download failed (" + error + ")");
        g_busy = false;
        return 0;
    }
    std::string want(sha.begin(), sha.end());
    want = want.substr(0, want.find_first_of(" \t\r\n"));
    for (char& ch : want) ch = (char)tolower((unsigned char)ch);
    std::string got = Sha256Hex(zip);
    if (want.size() != 64 || got != want) {
        Log(L"Update: %S's checksum doesn't match (%S, should be %S) - not installed", version.c_str(), got.c_str(), want.c_str());
        Set("failed", "The download was damaged - not installed");
        g_busy = false;
        return 0;
    }
    std::wstring dir = std::wstring(g_modDir) + L"\\update", stage = dir + L"\\stage", file = dir + L"\\update.zip";
    CreateDirectoryW(dir.c_str(), nullptr);
    DeleteTree(stage);   // (an earlier attempt's)
    CreateDirectoryW(stage.c_str(), nullptr);
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD put = 0;
    bool wrote = f != INVALID_HANDLE_VALUE && WriteFile(f, zip.data(), (DWORD)zip.size(), &put, nullptr) && put == zip.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    if (!wrote || !Run(L"\"" + std::wstring(sys) + L"\\tar.exe\" -x -f \"" + file + L"\" -C \"" + stage + L"\"", dir)) {
        Set("failed", "Couldn't unpack the update");
        g_busy = false;
        return 0;
    }
    int n = CopyTree(stage, L"", &error);
    Log(L"Update: %S installed - %d files%s%S", version.c_str(), n, error.empty() ? L"" : L", but ", error.c_str());
    if (!error.empty()) Set("failed", "The update was only partly installed (" + error + ")");
    else Set("ready", "Update " + version + " installed - restart the game");
    g_busy = false;
    return 0;
}

}  // namespace

// At start (the player's own game, not a match's or a test's): last update's leftovers away, and a check.
void InitUpdater() {
    std::wstring old = std::wstring(g_gameDir) + L"\\dinput8.dll.old";
    DeleteFileW(old.c_str());
    FILE* f = nullptr;
    if (_wfopen_s(&f, LobbyServerFile().c_str(), L"rb") == 0 && f) {   // (the address server.txt named last time)
        char buf[256] = {};
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        g_lobbyServer = ServerLine(buf);
    }
    wchar_t v[8];
    bool session = GetEnvironmentVariableW(L"FGUYMOD_SESSION", v, 8) || GetEnvironmentVariableW(L"FGUYMOD_HANDOFF", v, 8);
    if (session || IsBot() || (LatestUrl().empty() && ServerUrl().empty())) return;
    g_busy = true;
    if (!LatestUrl().empty()) Set("checking");
    if (HANDLE t = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr)) CloseHandle(t);
}

// online.cpp: the mod's lobby server as the repo's server.txt names it (or named last time), "" if never known.
std::string LobbyServerFromRepo() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_lobbyServer;
}

// FGUYMOD_UpdateState: "<state>|<latest version>|<message>"  (state: off checking current available downloading ready
// failed).
const char* UpdateState() {
    static std::string out;
    std::lock_guard<std::mutex> lock(g_lock);
    out = g_state + "|" + g_latest + "|" + g_message;
    for (size_t i = out.find('\n'); i != std::string::npos; i = out.find('\n', i)) out[i] = ' ';
    return out.c_str();
}

// FGUYMOD_UpdateInstall: download and install the newer version (in the background).
void UpdateInstall() {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_state != "available" && g_state != "failed") return;
        if (g_zipUrl.empty()) return;
    }
    if (g_busy.exchange(true)) return;
    if (HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr)) CloseHandle(t);
    else g_busy = false;
}
