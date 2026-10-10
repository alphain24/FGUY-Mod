// Online: the lobby (talks to server/fguy_lobby.py) and the in-game ONLINE page behind it.
//
// Flow: Main Menu > ONLINE connects to the lobby server. A player hosts a lobby or joins one from the
// list, others get ready, the host starts. Every player's game then restarts into a netplay session
// (netplay.cpp) that runs through the server's UDP relay: everyone boots to the main menu in lockstep,
// with every remote player acting as one more controller. From there the game's own menus do the
// rest (Story co-op drop-in, Multiplayer modes and their join screens).
//
// All copies must start identical, so the session uses the host's save file (sent through the
// server, stored in FGUYMod\online\) - each player's own save is never touched by online play.
//
// The menu side (tools/ui/make_menu_patch.py) shows rows named Online*; it asks us for each row's
// text (FGUYMOD_OnlineText) a few times per second and tells us about presses (FGUYMOD_OnlinePress).
//
// Settings ([Online] in FGUYMod.ini): Server=host:port (default port 7650), Name=<your name>.

#include "common.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

const int kProto = 1;
const char* kModVersion = "0.4";

struct Member {
    std::string name;
    bool ready = false;
    int rtt = 0;
};
struct LobbyInfo {
    std::string code, host;
    int players = 0, max = 0;
};

std::mutex g_lock;  // guards everything below (menu thread vs lobby thread)
enum class State { Off, Connecting, Connected, InLobby, Starting, Failed };
State g_state = State::Off;
std::string g_status;               // last error / info line
std::string g_code;                 // lobby code while in a lobby
int g_mySlot = -1;
std::vector<Member> g_members;
std::vector<LobbyInfo> g_lobbies;

bool g_ready = false;
SOCKET g_sock = INVALID_SOCKET;
HANDLE g_thread = nullptr;
volatile bool g_wantConnection = false;
std::string g_serverHost;
int g_serverPort = 7650;
std::string g_name;
std::string g_saveB64;              // the host's save, received before START

// ---- helpers ----------------------------------------------------------------------------------------
std::string Narrow(const wchar_t* w) {
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

std::string CleanName(std::string n) {
    std::string out;
    for (char c : n)
        if (isalnum((unsigned char)c) || c == '_' || c == '-') out += c;
    if (out.size() > 16) out.resize(16);
    return out.empty() ? "Player" : out;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return out;
        a = b + 1;
    }
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string Base64(const std::vector<unsigned char>& d) {
    std::string out;
    out.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        unsigned v = d[i] << 16 | (i + 1 < d.size() ? d[i + 1] << 8 : 0) | (i + 2 < d.size() ? d[i + 2] : 0);
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += i + 1 < d.size() ? kB64[v >> 6 & 63] : '=';
        out += i + 2 < d.size() ? kB64[v & 63] : '=';
    }
    return out;
}
std::vector<unsigned char> Unbase64(const std::string& s) {
    std::vector<unsigned char> out;
    unsigned v = 0;
    int bits = 0;
    for (char c : s) {
        const char* p = strchr(kB64, c);
        if (!p || !c) continue;
        v = v << 6 | (unsigned)(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)(v >> bits));
        }
    }
    return out;
}

bool ReadWholeFile(const wchar_t* path, std::vector<unsigned char>& out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    out.resize(size);
    bool ok = size == 0 || (ReadFile(f, out.data(), size, &got, nullptr) && got == size);
    CloseHandle(f);
    return ok;
}
bool WriteWholeFile(const wchar_t* path, const std::vector<unsigned char>& d) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = d.empty() || (WriteFile(f, d.data(), (DWORD)d.size(), &put, nullptr) && put == d.size());
    CloseHandle(f);
    return ok;
}

// Identifies the game build + mod version: everyone in a lobby must match (lockstep needs the same code).
std::string GameKey() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::vector<unsigned char> d;
    unsigned crc = 0xFFFFFFFFu;
    if (ReadWholeFile(exe, d)) {
        static unsigned table[256];
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        for (unsigned char b : d) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    char key[48];
    sprintf_s(key, "%08x-%s", crc ^ 0xFFFFFFFFu, kModVersion);
    return key;
}

// ---- lobby connection (its own thread) ------------------------------------------------------------
void SendLine(const std::string& line) {
    std::string l = line + "\n";
    if (g_sock != INVALID_SOCKET) send(g_sock, l.data(), (int)l.size(), 0);
}

void SetStatus(State s, const std::string& text) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_state = s;
    g_status = text;
}

void StartSession(const std::vector<std::string>& f);

void HandleLine(const std::string& line) {
    auto f = Split(line, ' ');
    const std::string& cmd = f[0];
    std::lock_guard<std::mutex> lock(g_lock);
    if (cmd == "WELCOME") {
        g_state = State::Connected;
        g_status.clear();
    } else if (cmd == "LIST") {
        g_lobbies.clear();
        for (size_t i = 1; i < f.size(); ++i) {
            auto e = Split(f[i], ',');
            if (e.size() == 4) g_lobbies.push_back({e[0], e[1], atoi(e[2].c_str()), atoi(e[3].c_str())});
        }

    } else if (cmd == "LOBBY" && f.size() > 1) {
        g_state = State::InLobby;
        g_code = f[1];
        g_ready = false;
        g_status.clear();
    } else if (cmd == "MEMBERS" && f.size() > 1) {
        g_mySlot = atoi(f[1].c_str());
        g_members.clear();
        for (size_t i = 2; i < f.size(); ++i) {
            auto e = Split(f[i], ',');
            if (e.size() == 4) g_members.push_back({e[1], e[2] == "1", atoi(e[3].c_str())});
        }
    } else if (cmd == "CLOSED") {
        g_state = State::Connected;
        g_code.clear();
        g_members.clear();
        g_status = line.size() > 7 ? line.substr(7) : "The lobby was closed";
    } else if (cmd == "ERROR") {
        g_status = line.size() > 6 ? line.substr(6) : "Error";
    } else if (cmd == "SAVE" && f.size() > 1) {
        g_saveB64 = f[1];
    } else if (cmd == "START" && f.size() >= 7) {
        g_state = State::Starting;
        g_status = "Starting the game...";
        StartSession(f);
    }
}

DWORD WINAPI LobbyThread(LPVOID) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    while (g_wantConnection) {
        SetStatus(State::Connecting, "");
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char port[8];
        sprintf_s(port, "%d", g_serverPort);
        if (getaddrinfo(g_serverHost.c_str(), port, &hints, &res) != 0 || !res) {
            SetStatus(State::Failed, "Can't find the server " + g_serverHost);
            Sleep(3000);
            continue;
        }
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        DWORD timeout = 5000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
        bool ok = connect(s, res->ai_addr, (int)res->ai_addrlen) == 0;
        freeaddrinfo(res);
        if (!ok) {
            closesocket(s);
            SetStatus(State::Failed, "Can't reach the server " + g_serverHost);
            Sleep(3000);
            continue;
        }
        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        g_sock = s;
        Log(L"Online: connected to %S:%d as %S", g_serverHost.c_str(), g_serverPort, g_name.c_str());
        SendLine("HELLO " + std::to_string(kProto) + " " + GameKey() + " " + g_name);
        SendLine("LIST");

        std::string buf;
        ULONGLONG nextPing = 0, nextList = 0;
        int rtt = 0;
        while (g_wantConnection) {
            ULONGLONG now = GetTickCount64();
            if (now >= nextPing) {
                SendLine("PING " + std::to_string(now));
                nextPing = now + 2000;
            }
            State st;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                st = g_state;
            }
            if (st == State::Connected && now >= nextList) {
                SendLine("LIST");
                nextList = now + 2000;
            }
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv{0, 100000};
            int n = select(0, &rd, nullptr, nullptr, &tv);
            if (n < 0) break;
            if (n == 0) continue;
            char chunk[65536];
            int got = recv(s, chunk, sizeof(chunk), 0);
            if (got <= 0) break;
            buf.append(chunk, got);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.compare(0, 5, "PONG ") == 0) {
                    int sample = (int)(GetTickCount64() - _strtoui64(line.c_str() + 5, nullptr, 10));
                    rtt = rtt ? (rtt * 3 + sample) / 4 : sample;
                    SendLine("RTT " + std::to_string(rtt));
                    continue;
                }
                HandleLine(line);
            }
        }
        g_sock = INVALID_SOCKET;
        closesocket(s);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_state == State::Starting) return 0;
            g_code.clear();
            g_members.clear();
            if (g_wantConnection) {
                g_state = State::Failed;
                g_status = "Lost the connection to the server";
            }
        }
        if (g_wantConnection) Sleep(2000);
    }
    SetStatus(State::Off, "");
    return 0;
}

// ---- starting the session -----------------------------------------------------------------------------
// START <token> <slot> <players> <delay> <udp port> <lang>. Called with g_lock held.
void StartSession(const std::vector<std::string>& f) {
    // The session's save folder: FGUYMod\online (test copies on one PC: online_<their test folder>).
    wchar_t dirName[80] = L"online", test[64];
    if (GetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", test, 64) && _wcsnicmp(test, L"online", 6) != 0)
        swprintf_s(dirName, L"online_%s", test);
    wchar_t dir[MAX_PATH], save[MAX_PATH];
    swprintf_s(dir, L"%s\\%s", g_modDir, dirName);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(save, L"%s\\player0_default.sav", dir);
    std::vector<unsigned char> data;
    if (g_mySlot == 0) {
        wchar_t mine[MAX_PATH];
        swprintf_s(mine, L"%s\\player0_default.sav", ConfigSaveDir());
        ReadWholeFile(mine, data);
    } else {
        data = Unbase64(g_saveB64);
    }
    if (data.empty()) DeleteFileW(save);  // no save shared: everyone starts a fresh profile
    else WriteWholeFile(save, data);

    // Restart the game with the session's settings in its environment (netplay.cpp reads them).
    auto set = [](const wchar_t* k, const std::string& v) {
        SetEnvironmentVariableW(k, std::wstring(v.begin(), v.end()).c_str());
    };
    set(L"FGUYMOD_NET", "1");
    set(L"FGUYMOD_NET_SLOT", f[2]);
    set(L"FGUYMOD_NET_PLAYERS", f[3]);
    set(L"FGUYMOD_NET_DELAY", f[4]);
    set(L"FGUYMOD_NET_RELAY", g_serverHost + ":" + f[5]);
    set(L"FGUYMOD_NET_TOKEN", f[1]);
    set(L"FGUYMOD_NET_LANG", f[6]);
    SetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", dirName);
    set(L"FGUYMOD_PADMODE", "1");  // every player is a controller in a session
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_gameDir, &si, &pi)) {
        Log(L"Online: session %S, slot %S of %S, input delay %S - restarting the game (new process %lu)",
            f[1].c_str(), f[2].c_str(), f[3].c_str(), f[4].c_str(), pi.dwProcessId);
        wchar_t pids[MAX_PATH], env[8];
        if (GetEnvironmentVariableW(L"FGUYMOD_TESTINPUT", env, 8)) {  // test copies: let stop_tests find it
            swprintf_s(pids, L"%s\\test_pids.txt", g_modDir);
            HANDLE h = CreateFileW(pids, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
            char line[32];
            int n = sprintf_s(line, "%lu\r\n", pi.dwProcessId);
            DWORD put;
            if (h != INVALID_HANDLE_VALUE) { WriteFile(h, line, n, &put, nullptr); CloseHandle(h); }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        VideoRequestQuit();
    } else {
        g_status = "Couldn't restart the game (" + std::to_string(GetLastError()) + ")";
        g_state = State::Failed;
    }
}

// ---- the ONLINE page ------------------------------------------------------------------------------------
std::string g_text;  // returned to the menu (it copies the string)

std::string MemberText(int slot) {
    if (slot >= (int)g_members.size()) return "-";
    const Member& m = g_members[slot];
    std::string t = m.name;
    if (slot == g_mySlot) t += " (you)";
    t += slot == 0 ? "   HOST" : m.ready ? "   READY" : "   not ready";
    if (m.rtt) t += "   " + std::to_string(m.rtt) + " ms";
    return t;
}

}  // namespace

void OnlineEnter() {
    if (g_wantConnection) return;
    wchar_t server[256], name[64];
    SettingString(L"Online", L"Server", L"FGUYMOD_ONLINE_SERVER", L"127.0.0.1:7650", server, 256);
    DWORD len = 64;
    wchar_t user[64] = L"Player";
    GetUserNameW(user, &len);
    SettingString(L"Online", L"Name", L"FGUYMOD_ONLINE_NAME", user, name, 64);
    if (!name[0]) wcscpy_s(name, user);
    std::string s = Narrow(server);
    size_t colon = s.rfind(':');
    g_serverHost = colon == std::string::npos ? s : s.substr(0, colon);
    g_serverPort = colon == std::string::npos ? 7650 : atoi(s.c_str() + colon + 1);
    g_name = CleanName(Narrow(name));
    g_wantConnection = true;
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); }
    g_thread = CreateThread(nullptr, 0, LobbyThread, nullptr, 0, nullptr);
}

void OnlineExit() {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_state == State::Starting) return;
        if (g_state == State::InLobby) SendLine("LEAVE");
    }
    g_wantConnection = false;
}

// The page's five rows change meaning with the state. Not in a lobby:
//   1 STATUS   2 HOST GAME   3 JOIN GAME (list: open games)   4 SERVER   5 YOUR NAME
// In a lobby:
//   1 LOBBY <code>   2 START GAME (host) / READY (others)   3 PLAYERS (list)   4 LEAVE LOBBY   5 YOUR NAME
namespace {
enum class Row { Status, Host, Join, Server, Name, Lobby, Start, Ready, Players, Leave, None };

Row RowAt(const char* row) {  // called with g_lock held
    int k = row && strncmp(row, "Online", 6) == 0 ? row[6] - '0' : 0;
    bool inLobby = g_state == State::InLobby || g_state == State::Starting;
    switch (k) {
        case 1: return inLobby ? Row::Lobby : Row::Status;
        case 2: return !inLobby ? Row::Host : g_mySlot == 0 ? Row::Start : Row::Ready;
        case 3: return inLobby ? Row::Players : Row::Join;
        case 4: return inLobby ? Row::Leave : Row::Server;
        case 5: return Row::Name;
        default: return Row::None;
    }
}

std::vector<std::string> ListEntries() {  // the list row's choices (g_lock held)
    std::vector<std::string> out;
    if (g_state == State::InLobby || g_state == State::Starting) {
        for (size_t i = 0; i < g_members.size(); ++i) out.push_back(MemberText((int)i));
    } else {
        for (const LobbyInfo& l : g_lobbies)
            out.push_back(l.host + "'s game  (" + std::to_string(l.players) + "/" + std::to_string(l.max) + ")");
    }
    if (out.empty()) out.push_back(g_state == State::Connected ? "No open games right now - host one!" : "-");
    return out;
}
int g_listIndex = 0;  // the list row's current choice
}  // namespace

const char* OnlineLabel(const char* row) {
    std::lock_guard<std::mutex> lock(g_lock);
    static const char* names[] = {"STATUS", "HOST GAME", "JOIN GAME", "SERVER", "YOUR NAME", "LOBBY",
                                  "START GAME", "READY", "PLAYERS", "LEAVE LOBBY", ""};
    Row r = RowAt(row);
    g_text = names[(int)r];
    if (r == Row::Lobby) g_text += " " + g_code;
    if (r == Row::Players) g_text += " (" + std::to_string(g_members.size()) + "/4)";
    return g_text.c_str();
}

const char* OnlineText(const char* row) {
    std::lock_guard<std::mutex> lock(g_lock);
    bool host = g_mySlot == 0;
    switch (RowAt(row)) {
        case Row::Status:
            if (!g_status.empty()) g_text = g_status;
            else if (g_state == State::Connecting) g_text = "Connecting to the server...";
            else if (g_state == State::Connected) g_text = "Online - host a game or join one";
            else g_text = "Not connected";
            break;
        case Row::Host: g_text = g_state == State::Connected ? "Press A to create a game" : "-"; break;
        case Row::Join: {
            auto e = ListEntries();
            g_text = e[g_listIndex < (int)e.size() ? g_listIndex : 0];
            if (g_state == State::Connected && !g_lobbies.empty()) g_text += "   - A to join";
            break;
        }
        case Row::Server: g_text = g_serverHost + ":" + std::to_string(g_serverPort); break;
        case Row::Name: g_text = g_name + "   (change it in FGUYMod.ini)"; break;
        case Row::Lobby:
            if (!g_status.empty()) g_text = g_status;
            else g_text = host ? "You are the host" : "Host: " + (g_members.empty() ? "?" : g_members[0].name);
            break;
        case Row::Start: {
            bool all = g_members.size() >= 2;
            for (size_t i = 1; i < g_members.size(); ++i) all &= g_members[i].ready;
            g_text = g_state == State::Starting ? "Starting..." : g_members.size() < 2 ? "Waiting for players to join"
                     : all ? "Everyone's ready - press A to start" : "Waiting for everyone to be ready";
            break;
        }
        case Row::Ready: g_text = g_state == State::Starting ? "Starting..." : g_ready ? "Yes - waiting for the host"
                                                                                       : "No - press A when you're ready";
            break;
        case Row::Players: {
            auto e = ListEntries();
            g_text = e[g_listIndex < (int)e.size() ? g_listIndex : 0];
            break;
        }
        case Row::Leave: g_text = host ? "Close this game" : "Back to the game list"; break;
        default: g_text = ""; break;
    }
    return g_text.c_str();
}

const char* OnlineList() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_text.clear();
    for (const std::string& e : ListEntries()) g_text += (g_text.empty() ? "" : "|") + e;
    return g_text.c_str();
}

int OnlineGet(const char*) {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_listIndex;
}

void OnlineSet(const char*, int value) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (value >= 0) g_listIndex = value;
}

void OnlinePress(const char* row) {
    std::lock_guard<std::mutex> lock(g_lock);
    Row r = RowAt(row);
    g_status.clear();
    switch (r) {
        case Row::Host:
            if (g_state == State::Connected) SendLine("HOST 4");
            break;
        case Row::Join:
            if (g_state == State::Connected && g_listIndex < (int)g_lobbies.size())
                SendLine("JOIN " + g_lobbies[g_listIndex].code);
            break;
        case Row::Ready:
            if (g_state == State::InLobby) {
                g_ready = !g_ready;
                SendLine(g_ready ? "READY 1" : "READY 0");
            }
            break;
        case Row::Start:
            if (g_state == State::InLobby) {
                wchar_t mine[MAX_PATH];
                swprintf_s(mine, L"%s\\player0_default.sav", ConfigSaveDir());
                std::vector<unsigned char> data;
                if (ReadWholeFile(mine, data) && !data.empty()) SendLine("SAVE " + Base64(data));
                SendLine("START");
            }
            break;
        case Row::Leave:
            if (g_state == State::InLobby) {
                SendLine("LEAVE");
                g_state = State::Connected;
                g_code.clear();
                g_members.clear();
                g_listIndex = 0;
                SendLine("LIST");
            }
            break;
        default: break;
    }
}
