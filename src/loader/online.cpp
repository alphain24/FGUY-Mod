// Online: the lobby (talks to server/fguy_lobby.py) behind the in-game ONLINE screens (src/ui/lobby.as2js).
//
// Flow: Main Menu > ONLINE connects to the lobby server. A player hosts a lobby (anyone can join, or
// code only) or joins one; everyone picks a character, costume and team, the host picks the mode, map
// and rules; when everyone's ready the host starts. The server then sends everyone the same snapshot of
// all choices, and every player's game restarts into a netplay session (netplay.cpp) through the
// server's UDP relay. In the session the menu script reads that snapshot (FGUYMOD_SessionConfig) and
// starts the match itself - identically on every PC, so the match is in sync from its first frame.
// The restart isn't seen: this game shows the loading screen until the new one has the match running
// and shows itself (handoff.cpp), then closes.
//
// All copies must start identical, so the session uses the host's save file (sent through the
// server, stored in FGUYMod\online\) - each player's own save is never touched by online play.
//
// The menu asks for everything with FGUYMOD_LobbyState() (one text blob, see LobbyState below) a few
// times per second and acts with FGUYMOD_LobbyDo(command, args...).
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

const int kProto = 2;
const char* kModVersion = "0.5";

struct Member {
    std::string name, character = "Peter", costume = "0", team = "0";
    bool ready = false;
    int rtt = 0;
};
struct LobbyInfo {
    std::string code, host, mode, map;
    int players = 0, max = 0;
};

std::mutex g_lock;  // guards everything below (menu thread vs lobby thread)
enum class State { Off, Connecting, Connected, InLobby, Starting, Failed };
State g_state = State::Off;
std::string g_status;               // last error / info line
std::string g_code;                 // lobby code while in a lobby
bool g_private = false;
int g_mySlot = -1;
std::vector<Member> g_members;
std::vector<LobbyInfo> g_lobbies;
std::string g_conf = "DM|FR|0|0|0";  // mode|map|time|score|ff, as the menu uses it
bool g_ready = false;
Member g_mine;                      // my picks (kept across lobbies)
SOCKET g_sock = INVALID_SOCKET;
HANDLE g_thread = nullptr;
volatile bool g_wantConnection = false;
std::string g_serverHost;
int g_serverPort = 7650;
std::string g_name;
std::string g_saveB64;              // the host's save, received before START
int g_rtt = 0;

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

// Short codes we pass to the server (characters, modes, maps, numbers): letters, digits, _ and -.
std::string CleanToken(const std::string& s) {
    std::string out;
    for (char c : s)
        if (isalnum((unsigned char)c) || c == '_' || c == '-') out += c;
    if (out.size() > 24) out.resize(24);
    return out.empty() ? "0" : out;
}

// For the menu's text blob: no field or line separators inside a field.
std::string Field(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return out;
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
std::mutex g_sendLock;  // whole lines only (the save upload is one big line)
void SendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_sendLock);
    std::string l = line + "\n";
    for (size_t at = 0; g_sock != INVALID_SOCKET && at < l.size();) {
        int put = send(g_sock, l.data() + at, (int)(l.size() - at), 0);
        if (put <= 0) break;
        at += put;
    }
}

void SetStatus(State s, const std::string& text) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_state = s;
    g_status = text;
}

void SendPicks() {  // g_lock held
    SendLine("SET " + CleanToken(g_mine.character) + " " + CleanToken(g_mine.costume) + " " + CleanToken(g_mine.team));
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
            if (e.size() == 6) g_lobbies.push_back({e[0], e[1], e[4], e[5], atoi(e[2].c_str()), atoi(e[3].c_str())});
        }
    } else if (cmd == "LOBBY" && f.size() > 1) {
        g_state = State::InLobby;
        g_code = f[1];
        g_private = f.size() > 2 && f[2] == "1";
        g_ready = false;
        g_status.clear();
        SendPicks();
    } else if (cmd == "MEMBERS" && f.size() > 1) {
        g_mySlot = atoi(f[1].c_str());
        g_members.clear();
        for (size_t i = 2; i < f.size(); ++i) {
            auto e = Split(f[i], ',');
            if (e.size() < 7) continue;
            Member m;
            m.name = e[1];
            m.ready = e[2] == "1";
            m.rtt = atoi(e[3].c_str());
            m.character = e[4];
            m.costume = e[5];
            m.team = e[6];
            g_members.push_back(m);
        }
    } else if (cmd == "CONF" && f.size() >= 6) {
        g_conf = f[1] + "|" + f[2] + "|" + f[3] + "|" + f[4] + "|" + f[5];
    } else if (cmd == "CLOSED") {
        g_state = State::Connected;
        g_code.clear();
        g_members.clear();
        g_status = line.size() > 7 ? line.substr(7) : "The lobby was closed";
    } else if (cmd == "ERROR") {
        g_status = line.size() > 6 ? line.substr(6) : "Error";
    } else if (cmd == "SAVE" && f.size() > 1) {
        g_saveB64 = f[1];
    } else if (cmd == "START" && f.size() >= 9) {
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
                    g_rtt = rtt;
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
// The old game's side of the handoff: wait for the session to show itself (then close), or to die.
struct Handoff {
    HANDLE process, ready;
    DWORD pid;
};

DWORD WINAPI HandoffWait(LPVOID arg) {
    auto* h = static_cast<Handoff*>(arg);
    HANDLE both[2] = {h->ready, h->process};
    DWORD r = WaitForMultipleObjects(2, both, FALSE, 240000);
    if (r == WAIT_OBJECT_0 + 1) {
        DWORD code = 0;
        GetExitCodeProcess(h->process, &code);
        Log(L"Online: the session game (process %lu) closed before the match started (exit code %#lx)", h->pid, code);
        SetStatus(State::Failed, "The match couldn't start - please try again");
        g_wantConnection = false;
    } else {
        Log(L"Online: %s - closing this game", r == WAIT_OBJECT_0 ? L"the session game is showing the match"
                                                                    : L"the session game took too long");
        DisplayHideWindow();
        VideoRequestQuit();
    }
    CloseHandle(h->ready);
    CloseHandle(h->process);
    delete h;
    return 0;
}

// START <token> <slot> <players> <delay> <udp port> <lang> <conf> <picks>. Called with g_lock held.
void StartSession(const std::vector<std::string>& f) {
    // The session's save folder: FGUYMod\online (test copies on one PC: online_<their test folder>).
    // (worked out once: starting sets FGUYMOD_SAVEDIR itself, and a failed start can be retried)
    static wchar_t dirName[80] = L"";
    if (!dirName[0]) {
        wchar_t test[64];
        wcscpy_s(dirName, L"online");
        if (GetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", test, 64) && _wcsnicmp(test, L"online", 6) != 0)
            swprintf_s(dirName, L"online_%s", test);
    }
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

    // Restart the game with the session's settings in its environment (netplay.cpp and the menu read them).
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
    set(L"FGUYMOD_SESSION", f[7] + "|" + f[8]);  // conf: mode,map,time,score,ff | picks: char:costume:team;...
    SetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", dirName);
    set(L"FGUYMOD_PADMODE", "1");  // every player is a controller in a session
    set(L"FGUYMOD_HANDOFF", std::to_string(GetCurrentProcessId()));  // boot hidden, tell us when it shows
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_gameDir, &si, &pi);
    SetEnvironmentVariableW(L"FGUYMOD_HANDOFF", nullptr);
    if (started) {
        Log(L"Online: session %S, slot %S of %S, input delay %S, %S %S - starting it hidden (new process %lu)",
            f[1].c_str(), f[2].c_str(), f[3].c_str(), f[4].c_str(), f[7].c_str(), f[8].c_str(), pi.dwProcessId);
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
        AllowSetForegroundWindow(pi.dwProcessId);  // so it can come to the front when it shows
        wchar_t name[64];
        swprintf_s(name, L"Local\\FGUYMod_SessionReady_%lu", pi.dwProcessId);
        auto* h = new Handoff{pi.hProcess, CreateEventW(nullptr, TRUE, FALSE, name), pi.dwProcessId};
        CloseHandle(CreateThread(nullptr, 0, HandoffWait, h, 0, nullptr));
    } else {
        g_status = "Couldn't restart the game (" + std::to_string(GetLastError()) + ")";
        g_state = State::Failed;
    }
}

std::string g_text;  // returned to the menu (it copies the string)

const char* StateName() {
    switch (g_state) {
        case State::Connecting: return "connecting";
        case State::Connected: return "online";
        case State::InLobby: return "lobby";
        case State::Starting: return "starting";
        case State::Failed: return "failed";
        default: return "off";
    }
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

// Everything the ONLINE screens show, one item per line, fields separated by '|':
//   S|<state>|<status>|<code>|<my slot>|<private>|<my name>|<server>|<my ping>
//        state: off, connecting, online, lobby, starting, failed
//   C|<mode>|<map>|<time>|<score>|<ff>                      the lobby's match settings
//   M|<slot>|<name>|<ready>|<ping>|<character>|<costume>|<team>    one per player in my lobby
//   L|<code>|<host>|<players>|<max>|<mode>|<map>            one per open lobby (when not in one)
const char* LobbyState() {
    std::lock_guard<std::mutex> lock(g_lock);
    bool inLobby = g_state == State::InLobby || g_state == State::Starting;
    g_text = "S|" + std::string(StateName()) + "|" + Field(g_status) + "|" + g_code + "|" + std::to_string(g_mySlot) +
             "|" + (g_private ? "1" : "0") + "|" + Field(g_name) + "|" + Field(g_serverHost) + "|" +
             std::to_string(g_rtt) + "\n";
    g_text += "C|" + g_conf + "\n";
    if (inLobby) {
        for (size_t i = 0; i < g_members.size(); ++i) {
            const Member& m = g_members[i];
            g_text += "M|" + std::to_string(i) + "|" + Field(m.name) + "|" + (m.ready ? "1" : "0") + "|" +
                      std::to_string(m.rtt) + "|" + Field(m.character) + "|" + Field(m.costume) + "|" + Field(m.team) + "\n";
        }
    } else {
        for (const LobbyInfo& l : g_lobbies)
            g_text += "L|" + l.code + "|" + Field(l.host) + "|" + std::to_string(l.players) + "|" +
                      std::to_string(l.max) + "|" + Field(l.mode) + "|" + Field(l.map) + "\n";
    }
    return g_text.c_str();
}

// host <max> <private>   join <code>   leave   ready   start   pick <char> <costume> <team>
// conf <mode> <map> <time> <score> <ff>
void LobbyDo(const char* command, const char* a, const char* b, const char* c, const char* d, const char* e) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::string cmd = command ? command : "";
    g_status.clear();
    if (cmd == "host" && g_state == State::Connected) {
        SendLine("HOST " + CleanToken(a) + " " + CleanToken(b));
    } else if (cmd == "join" && g_state == State::Connected) {
        std::string code = CleanToken(a);
        for (char& ch : code) ch = (char)toupper((unsigned char)ch);
        SendLine("JOIN " + code);
    } else if (cmd == "leave" && g_state == State::InLobby) {
        SendLine("LEAVE");
        g_state = State::Connected;
        g_code.clear();
        g_members.clear();
        SendLine("LIST");
    } else if (cmd == "ready" && g_state == State::InLobby && g_mySlot > 0) {
        g_ready = !g_ready;
        SendLine(g_ready ? "READY 1" : "READY 0");
    } else if (cmd == "start" && g_state == State::InLobby && g_mySlot == 0) {
        wchar_t mine[MAX_PATH];
        swprintf_s(mine, L"%s\\player0_default.sav", ConfigSaveDir());
        std::vector<unsigned char> data;
        if (ReadWholeFile(mine, data) && !data.empty()) SendLine("SAVE " + Base64(data));
        SendLine("START");
    } else if (cmd == "pick") {
        g_mine.character = CleanToken(a);
        g_mine.costume = CleanToken(b);
        g_mine.team = CleanToken(c);
        if (g_state == State::InLobby) {
            if (g_mySlot >= 0 && g_mySlot < (int)g_members.size()) {  // show it right away
                g_members[g_mySlot].character = g_mine.character;
                g_members[g_mySlot].costume = g_mine.costume;
                g_members[g_mySlot].team = g_mine.team;
            }
            SendPicks();
        }
    } else if (cmd == "conf" && g_state == State::InLobby && g_mySlot == 0) {
        g_conf = CleanToken(a) + "|" + CleanToken(b) + "|" + CleanToken(c) + "|" + CleanToken(d) + "|" + CleanToken(e);
        SendLine("CONF " + CleanToken(a) + " " + CleanToken(b) + " " + CleanToken(c) + " " + CleanToken(d) + " " +
                 CleanToken(e));
    }
}

// In a session: the snapshot of everyone's choices ("mode,map,time,score,ff|char:costume:team;..."), else "".
const char* SessionConfig() {
    static char buf[512];
    if (!GetEnvironmentVariableA("FGUYMOD_SESSION", buf, sizeof(buf))) buf[0] = 0;
    return buf;
}
