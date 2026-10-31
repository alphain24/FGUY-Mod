// Online: the lobby (talks to server/fguy_lobby.py) behind the in-game ONLINE screens (src/ui/lobby.as2js).
//
// Flow: Main Menu > ONLINE connects to the lobby server. A player hosts a lobby (anyone can join, or
// code only) or joins one; everyone picks a character, costume and team, the host picks the mode, map
// and rules; when everyone's ready the host starts. The server then sends everyone the same snapshot of
// all choices, and every player's game restarts into a netplay session (netplay.cpp) through the
// server's UDP relay. In the session the menu script reads that snapshot (FGUYMOD_SessionConfig) and
// starts the match itself - identically on every PC, so the match is in sync from its first frame.
// A guest who left a match that still runs (or whose game closed in it) can get back in from its lobby: REJOIN MATCH
// starts the session game again, which catches up with the match (netplay.cpp) while this game shows how far it is.
// The restart isn't seen: this game shows the loading screen until the new one has the match running
// and shows itself (handoff.cpp), then closes.
//
// All copies must start identical, so the session uses the host's save file (sent through the
// server, stored in FGUYMod\online\) - each player's own save is never touched by online play.
//
// The menu asks for everything with FGUYMOD_LobbyState() (one text blob, see LobbyState below) a few
// times per second and acts with FGUYMOD_LobbyDo(command, args...).
//
// Settings ([Online] in FGUYMod.ini): Server=host:port (default port 7650; empty: the mod's - the address in the repo's
// server.txt, read at start by updater.cpp, so the server can move without a release),
// Name=<your name>.

#include "common.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

const int kProto = 3;
const char* kModVersion = "0.9.4";

struct Member {
    std::string name, character = "Peter", costume = "0", team = "0", input = "c";  // input: k keyboard, c controller
    bool ready = false;
    bool away = false;   // still in the last match (the lobby keeps their place)
    std::string rate = "60"; // the most frames per second their monitor shows
    std::string skin;        // the custom skin they wear: its head picture's id (cosmetics.cpp), "" none
    int rtt = 0;
};
bool g_serverSkins = false;   // (the server takes SKIN: its WELCOME says "skins")
std::string g_skinSent = "?"; // (the skin id I last told it)
struct LobbyInfo {
    std::string code, host, mode, map;
    int players = 0, max = 0;
    std::string mods = "-";   // the host's gameplay mods' names (+ separated; - none)
};

std::mutex g_lock;  // guards everything below (menu thread vs lobby thread)
enum class State { Off, Connecting, Connected, InLobby, Starting, Failed };
State g_state = State::Off;
std::string g_status;               // last error / info line
std::string g_code;                 // lobby code while in a lobby
bool g_private = false;
int g_mySlot = -1;
std::vector<Member> g_members;
struct ChatLine { int seq; std::string name, text; };
std::vector<ChatLine> g_chat;        // my lobby's chat, the last few lines (SAID)
int g_chatSeq = 0;
std::vector<LobbyInfo> g_lobbies;
std::string g_conf = "DM|FR|0|0|0";  // mode|map|time|score|ff, as the menu uses it
std::string g_confRate = "0";        // the match's frames per second (0 = auto: the lowest monitor among us)
std::string g_confNet = "1";         // the match's netcode: 0 classic (lockstep), 1 new (free-running - the default)
bool g_newLobby = false;              // (a lobby I just made: the new netcode set in it - the server's default is classic)
std::string g_own;                  // the host's characters and costumes: Peter:0.1.3;Lois:0.2 (the session
                                    // plays on the host's save, so everyone picks from it)
bool g_ready = false;
Member g_mine;                      // my picks (kept across lobbies)
SOCKET g_sock = INVALID_SOCKET;
HANDLE g_thread = nullptr;
volatile bool g_wantConnection = false;
std::string g_serverHost;
int g_serverPort = 7650;
std::string g_serverSetting;        // FGUYMod.ini's [Online] Server
bool g_modServer = true;            //   empty or one of the mod's own: the mod's server, wherever server.txt says
const char* kServer = "91.226.56.14:7650";   // the mod's lobby server if GitHub was never reachable
std::string g_name;
std::string g_saveB64;              // the host's save, received before START
std::string g_rejoin;               // "<code> <token> <slot>": the lobby to go back to after a match
std::string g_back;                 //   ours while the match runs (this game waits hidden: HandoffWait)
bool g_returning = false;           // back from a match, on the way into its lobby again
HANDLE g_returnEvent = nullptr;     // the session game: the match is over, come back (handoff.cpp)
HANDLE g_backEvent = nullptr;       //   set once we show again
int g_rtt = 0;
bool g_matchOpen = false;           // my lobby's match still runs and I can get back into it (INMATCH)
bool g_catchUpStart = false;        // this START gets me back into a running match: the session game catches up
std::string g_progress;             //   how far it is (the loading screen)
std::string g_notice;               // what the lobby says once we're back in it (a rejoin that didn't work...)

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

// The host's ownership list: letters, digits and : ; . _ - only.
std::string CleanList(const std::string& s) {
    std::string out;
    for (char c : s)
        if (isalnum((unsigned char)c) || strchr(":;._-", c)) out += c;
    if (out.size() > 800) out.resize(800);
    return out;
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
    if (s == State::Failed) g_returning = false;
}

void SendPicks() {  // g_lock held
    SendLine("SET " + CleanToken(g_mine.character) + " " + CleanToken(g_mine.costume) + " " + CleanToken(g_mine.team) +
             " " + (g_mine.input == "k" ? "k" : "c"));
}

void StartSession(const std::vector<std::string>& f);
volatile bool g_startingMatch = false;   // the loading screen shows while the match's game boots hidden

void HandleLine(const std::string& line) {
    auto f = Split(line, ' ');
    const std::string& cmd = f[0];
    std::lock_guard<std::mutex> lock(g_lock);
    if (cmd == "WELCOME") {
        g_state = State::Connected;
        g_serverSkins = std::find(f.begin(), f.end(), "skins") != f.end();
        g_skinSent = "?";
        g_status.clear();
        DEVMODEW mode = {};   // the most frames per second this PC's monitor shows (the lobby's "auto" rate)
        mode.dmSize = sizeof(mode);
        int hz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                     ? (int)mode.dmDisplayFrequency : 60;
        SendLine("RATE " + std::to_string(hz));
        std::string mods = ModsOnlineKey();   // my gameplay mods: a lobby takes only players with the host's
        SendLine("MODS " + (mods.empty() ? std::string("-") : mods));
        if (!g_rejoin.empty()) {   // back from a match: our place in its lobby
            SendLine("REJOIN " + g_rejoin);
            Log(L"Online: back from the match - rejoining lobby %S", g_rejoin.c_str());
            g_rejoin.clear();
        }
    } else if (cmd == "LIST") {
        g_lobbies.clear();
        for (size_t i = 1; i < f.size(); ++i) {
            auto e = Split(f[i], ',');
            if (e.size() >= 6)
                g_lobbies.push_back({e[0], e[1], e[4], e[5], atoi(e[2].c_str()), atoi(e[3].c_str()), e.size() > 6 ? e[6] : "-"});
        }
    } else if (cmd == "LOBBY" && f.size() > 1) {
        g_state = State::InLobby;
        g_returning = false;
        g_own.clear();
        g_code = f[1];
        g_private = f.size() > 2 && f[2] == "1";
        g_chat.clear();   // (a new lobby: its own chat)
        g_ready = false;
        g_matchOpen = false;
        g_status = g_notice;
        g_notice.clear();
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
            m.away = e[2] == "2";
            m.rtt = atoi(e[3].c_str());
            m.character = e[4];
            m.costume = e[5];
            m.team = e[6];
            if (e.size() > 7) m.input = e[7] == "k" ? "k" : "c";
            if (e.size() > 8) m.rate = CleanToken(e[8]);
            if (e.size() > 9 && e[9] != "-") m.skin = CleanToken(e[9]);
            g_members.push_back(m);
        }
    } else if (cmd == "SAID" && f.size() > 3) {   // SAID <slot> <name> <text...>
        size_t at = line.find(' ', line.find(' ', line.find(' ') + 1) + 1);
        g_chat.push_back({++g_chatSeq, f[2], at == std::string::npos ? "" : line.substr(at + 1)});
        if (g_chat.size() > 8) g_chat.erase(g_chat.begin());
    } else if (cmd == "INMATCH" && f.size() > 1) {
        g_matchOpen = f[1] == "1";
    } else if (cmd == "OWN" && f.size() > 1) {
        g_own = CleanList(f[1]);
    } else if (cmd == "CONF" && f.size() >= 6) {
        g_conf = f[1] + "|" + f[2] + "|" + f[3] + "|" + f[4] + "|" + f[5];
        g_confRate = f.size() >= 7 ? CleanToken(f[6]) : "0";
        g_confNet = f.size() >= 8 && f[7] == "1" ? "1" : "0";
        if (g_newLobby && g_mySlot == 0) {   // (my new lobby: the new netcode, unless I pick classic)
            g_newLobby = false;
            if (g_confNet != "1") {
                g_confNet = "1";
                std::string send = "CONF";
                for (auto& v : Split(g_conf, '|')) send += " " + v;
                SendLine(send + " " + g_confRate + " " + g_confNet);
            }
        }
    } else if (cmd == "CLOSED") {
        g_returning = false;
        g_matchOpen = false;
        g_state = State::Connected;
        g_code.clear();
        g_members.clear();
        g_status = line.size() > 7 ? line.substr(7) : "The lobby was closed";
    } else if (cmd == "ERROR") {
        g_returning = false;
        g_status = line.size() > 6 ? line.substr(6) : "Error";
        Log(L"Online: the server says: %S", g_status.c_str());
    } else if (cmd == "SAVE" && f.size() > 1) {
        g_saveB64 = f[1];
    } else if (cmd == "START" && f.size() >= 9) {
        g_state = State::Starting;
        g_catchUpStart = f.size() >= 11 && f[10] == "catchup";   // (back into a running match)
        g_status = g_catchUpStart ? "Rejoining the match..." : "Starting the game...";
        g_progress.clear();
        StartSession(f);
    }
}

// The lobby server to connect to: the player's own, or the mod's (the repo's server.txt - updater.cpp - or kServer).
void PickServer() {
    std::string s = g_modServer ? LobbyServerFromRepo() : g_serverSetting;
    if (s.empty()) s = kServer;
    size_t colon = s.rfind(':');
    g_serverHost = colon == std::string::npos ? s : s.substr(0, colon);
    g_serverPort = colon == std::string::npos ? 7650 : atoi(s.c_str() + colon + 1);
}

DWORD WINAPI LobbyThread(LPVOID) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    while (g_wantConnection) {
        SetStatus(State::Connecting, "");
        PickServer();   // (each try: the repo's address may have come in meanwhile)
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char port[8];
        sprintf_s(port, "%d", g_serverPort);
        if (getaddrinfo(g_serverHost.c_str(), port, &hints, &res) != 0 || !res) {
            SetStatus(State::Failed, "Can't find the lobby server");   // (its address isn't shown: screenshots, streams)
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
            SetStatus(State::Failed, "Lobby server offline - try again later");
            Sleep(3000);
            continue;
        }
        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        g_sock = s;
        Log(L"Online: connected to the lobby server as %S", g_name.c_str());   // (not its address: logs get shared)
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
    bool catchUp;   // rejoining a running match: the session game catches up first (it can take minutes)
};

void StartLobbyThread() {
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); }
    g_wantConnection = true;
    g_thread = CreateThread(nullptr, 0, LobbyThread, nullptr, 0, nullptr);
}

// Back into our lobby (a rejoin that didn't work, or our session game closed mid-match): `notice` is what it says then.
// Back from a match whose end everyone picked to play on (netplay.cpp RematchWrite: the session game's file for this
// game): "restart" / "next" for the lobby (lobby.as2js FGUYRematchTick readies / starts by itself), a minute at most.
std::string g_rematch;
ULONGLONG g_rematchAt = 0;

void BackToLobby(const std::string& notice, bool unpark) {
    bool connected;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_notice = notice;
        g_progress.clear();
        g_returning = true;
        g_status.clear();
        connected = g_sock != INVALID_SOCKET && g_wantConnection && g_state == State::Starting;   // (still: rejoining)
        if (connected) {
            g_state = State::Connected;
        } else {
            g_rejoin = g_back;
            g_state = State::Off;
            g_code.clear();
            g_members.clear();
        }
    }
    AudioSilenceGame(false);
    g_startingMatch = false;
    if (unpark) {
        HandoffMute(false);
        DisplayUnpark(true, nullptr, nullptr);
    }
    if (connected) SendLine("REJOIN " + g_back);
    else StartLobbyThread();
}

DWORD WINAPI HandoffWait(LPVOID arg) {
    auto* h = static_cast<Handoff*>(arg);
    HANDLE both[2] = {h->ready, h->process};
    // (rejoining: the session game catches up first - how far it is, from its shared numbers)
    DWORD limit = h->catchUp ? 45 * 60000 : 240000, waited = 0, r;
    HANDLE cuMap = nullptr;
    CatchUpShared* cu = nullptr;
    for (;;) {
        r = WaitForMultipleObjects(2, both, FALSE, h->catchUp ? 500 : limit);
        if (r != WAIT_TIMEOUT || !h->catchUp) break;
        waited += 500;
        if (waited >= limit) break;
        if (!cu) {
            wchar_t name[64];
            swprintf_s(name, L"Local\\FGUYMod_CatchUp_%lu", h->pid);
            cuMap = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
            if (cuMap) cu = static_cast<CatchUpShared*>(MapViewOfFile(cuMap, FILE_MAP_READ, 0, 0, sizeof(CatchUpShared)));
        }
        if (cu && cu->hostTick > 0) {
            int pct = (int)((long long)cu->tick * 100 / cu->hostTick);
            if (pct > 99) pct = 99;
            std::lock_guard<std::mutex> lock(g_lock);
            g_progress = "Catching up with the match - " + std::to_string(pct) + "%";
        }
    }
    if (h->catchUp && r != WAIT_OBJECT_0) {   // the rejoin didn't work: back into the lobby
        std::string why = "it took too long";
        if (r == WAIT_OBJECT_0 + 1 && cu && cu->state == 2) {
            char reason[200];
            WideCharToMultiByte(CP_UTF8, 0, cu->reason, -1, reason, sizeof(reason), nullptr, nullptr);
            why = reason;
        } else if (r == WAIT_OBJECT_0 + 1) {
            why = "its game closed";
        } else {
            TerminateProcess(h->process, 1);
        }
        Log(L"Online: couldn't rejoin the match (%S) - back to its lobby", why.c_str());
        BackToLobby("Couldn't rejoin the match: " + why, false);
        if (cu) UnmapViewOfFile(cu);
        if (cuMap) CloseHandle(cuMap);
        CloseHandle(h->ready);
        CloseHandle(h->process);
        delete h;
        return 0;
    }
    if (cu) UnmapViewOfFile(cu);
    if (cuMap) CloseHandle(cuMap);
    if (r == WAIT_OBJECT_0 && g_returnEvent && g_backEvent && DisplayPark()) {
        // This game is the player's own (their save, settings and controllers): it waits hidden for the match
        // to end, and then the session game hands back to it (handoff.cpp HandoffGiveBack).
        Log(L"Online: the session game is showing the match - this game waits hidden until it's over");
        g_wantConnection = false;   // (the lobby keeps our place meanwhile: REJOIN after the match)
        HandoffMute(true);
        HANDLE wait[2] = {g_returnEvent, h->process};
        if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0) {
            RECT at;
            bool rect = DisplayWindowRectOf(h->pid, &at);   // (where the match was)
            {
                std::lock_guard<std::mutex> lock(g_lock);
                g_rejoin = g_back;
                g_returning = true;
                g_state = State::Off;
                g_status.clear();
                g_code.clear();
                g_members.clear();
            }
            Log(L"Online: the match is over - back to its lobby");
            AudioSilenceGame(false);
            g_startingMatch = false;
            HandoffMute(false);
            DisplayUnpark(true, rect ? &at : nullptr, g_backEvent);
            StartLobbyThread();
        } else {
            DWORD code = 0;
            GetExitCodeProcess(h->process, &code);
            if (code != 0) {   // it crashed (or was ended): back to the lobby - the match can be rejoined from there
                Log(L"Online: the session game closed unexpectedly (exit code %#lx) - back to the lobby", code);
                BackToLobby("Your game closed during the match - REJOIN MATCH gets you back in", true);
            } else {
                Log(L"Online: the session game closed without coming back - closing this game too");
                DisplayUnpark(false, nullptr, nullptr);
                VideoRequestQuit();
            }
        }
    } else if (r == WAIT_OBJECT_0 + 1) {
        DWORD code = 0;
        GetExitCodeProcess(h->process, &code);
        Log(L"Online: the session game (process %lu) closed before the match started (exit code %#lx)", h->pid, code);
        AudioSilenceGame(false);
        g_startingMatch = false;
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
    // After the match the session game hands back to this one (HandoffWait): our place in this lobby.
    g_back = g_code + " " + f[1] + " " + f[2];
    if (!g_returnEvent) {
        wchar_t name[64];
        swprintf_s(name, L"Local\\FGUYMod_LobbyReturn_%lu", GetCurrentProcessId());
        g_returnEvent = CreateEventW(nullptr, TRUE, FALSE, name);
        swprintf_s(name, L"Local\\FGUYMod_LobbyBack_%lu", GetCurrentProcessId());
        g_backEvent = CreateEventW(nullptr, TRUE, FALSE, name);
    }
    if (g_returnEvent) ResetEvent(g_returnEvent);
    if (g_backEvent) ResetEvent(g_backEvent);
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
    // They go into this game's own environment only for the start (the new process inherits it) and are put
    // back right after: this game isn't the session - left there, its menus later took it for one (the
    // session autopilot ran in the lobby game after an offline match, among others).
    struct Saved { std::wstring name, value; bool had; };
    std::vector<Saved> saved;
    auto keep = [&saved](const wchar_t* k) {
        for (auto& x : saved) if (x.name == k) return;
        wchar_t old[4096];
        DWORD n = GetEnvironmentVariableW(k, old, 4096);
        saved.push_back({k, n && n < 4096 ? std::wstring(old, n) : std::wstring(), n > 0 && n < 4096});
    };
    auto set = [&keep](const wchar_t* k, const std::string& v) {
        keep(k);
        SetEnvironmentVariableW(k, std::wstring(v.begin(), v.end()).c_str());
    };
    set(L"FGUYMOD_NET", "1");
    set(L"FGUYMOD_NET_SLOT", f[2]);
    set(L"FGUYMOD_NET_PLAYERS", f[3]);
    set(L"FGUYMOD_NET_DELAY", f[4]);
    set(L"FGUYMOD_NET_RELAY", g_serverHost + ":" + f[5]);
    set(L"FGUYMOD_NET_TOKEN", f[1]);
    set(L"FGUYMOD_NET_LOBBY", g_code);   // to come back to this lobby after the match
    set(L"FGUYMOD_NET_CATCHUP", g_catchUpStart ? "1" : "0");   // back into a running match: catch up first
    set(L"FGUYMOD_NET_RATE", f.size() >= 10 ? f[9] : std::string("60"));   // frames per second, everyone alike
    char testRate[8];   // test knob: FGUYMOD_TEST_RATE overrides it (every copy of a test alike)
    if (GetEnvironmentVariableA("FGUYMOD_TEST_RATE", testRate, sizeof(testRate))) set(L"FGUYMOD_NET_RATE", testRate);
    set(L"FGUYMOD_NET_LANG", f[6]);
    set(L"FGUYMOD_SESSION", f[7] + "|" + f[8]);  // conf: mode,map,time,score,ff | picks: char:costume:team;...
    {   // the netcode the host picked (conf's 7th field): the new one runs the match free (netplay.cpp) - test knob
        auto conf = Split(f[7], ',');   // FGUYMOD_TEST_NETFREE=1 forces it (every copy of a test alike)
        char forced[8] = "";
        bool free = (conf.size() > 6 && conf[6] == "1") || GetEnvironmentVariableA("FGUYMOD_TEST_NETFREE", forced, sizeof(forced)) > 0;
        bool anyMode = free && strcmp(forced, "all") == 0;   // (research: FGUYMOD_TEST_NETFREE=all - any mode)
        if ((conf.empty() || (conf[0] != "DM" && conf[0] != "HD" && conf[0] != "IN" && conf[0] != "CF" && conf[0] != "ST")) && !anyMode)
            free = false;   // (every mode: the multiplayer ones and story co-op)
        set(L"FGUYMOD_NET_FREE", free ? "1" : "0");
        // rejoining a free-running match: no catching up on the host's record - the session game loads the level on its
        // own and joins in (netplay.cpp FreeStart)
        set(L"FGUYMOD_NET_REJOIN", g_catchUpStart && free ? "1" : "0");
        if (g_catchUpStart && free) set(L"FGUYMOD_NET_CATCHUP", "0");
    }
    char testConf[128];   // test knob: FGUYMOD_TEST_CONF=mode,map,time,score,ff overrides the rules (every copy alike);
                          // with "|name:costume:team:input;..." the players too (their teams)
    if (GetEnvironmentVariableA("FGUYMOD_TEST_CONF", testConf, sizeof(testConf)))
        set(L"FGUYMOD_SESSION", strchr(testConf, '|') ? std::string(testConf) : testConf + ("|" + f[8]));
    keep(L"FGUYMOD_SAVEDIR");
    SetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", dirName);
    set(L"FGUYMOD_MY_SETTINGS", OnlineSettingsCapture());   // my own settings, not the host's save's
    {   // my custom skin (cosmetics.cpp): the match sends it to everyone
        std::wstring skin = CosmeticsMySkinPath();
        wchar_t test[MAX_PATH];   // (test knob FGUYMOD_TEST_SKIN=<slot>=<pack path>: that slot's player wears it)
        if (GetEnvironmentVariableW(L"FGUYMOD_TEST_SKIN", test, MAX_PATH) && _wtoi(test) == g_mySlot && wcschr(test, L'='))
            skin = wcschr(test, L'=') + 1;
        keep(L"FGUYMOD_MY_SKIN");
        SetEnvironmentVariableW(L"FGUYMOD_MY_SKIN", skin.empty() ? nullptr : skin.c_str());
    }
    std::string names;   // everyone's name by slot, for the match's notices ("Waiting for ...")
    for (size_t i = 0; i < g_members.size(); ++i) names += (i ? "|" : "") + g_members[i].name;
    if (g_catchUpStart && f.size() >= 12) names = f[11];   // (rejoining: the match's, as it started)
    set(L"FGUYMOD_NET_NAMES", names);
    set(L"FGUYMOD_PADMODE", "1");  // every player is a controller in a session
    set(L"FGUYMOD_HANDOFF", std::to_string(GetCurrentProcessId()));  // boot hidden, tell us when it shows
    RECT at;   // and show up exactly where this window is
    if (DisplayWindowRect(&at))
        set(L"FGUYMOD_HANDOFF_RECT", std::to_string(at.left) + "," + std::to_string(at.top) + "," +
                                         std::to_string(at.right) + "," + std::to_string(at.bottom));
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_gameDir, &si, &pi);
    for (auto& x : saved) SetEnvironmentVariableW(x.name.c_str(), x.had ? x.value.c_str() : nullptr);
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
        AudioSilenceGame(true);   // our menus' music / sounds don't play on under the loading screen
        g_startingMatch = true;   // (and this game shows only that, at 60 fps: the GPU for the booting game)
        AllowSetForegroundWindow(pi.dwProcessId);  // so it can come to the front when it shows
        wchar_t name[64];
        swprintf_s(name, L"Local\\FGUYMod_SessionReady_%lu", pi.dwProcessId);
        auto* h = new Handoff{pi.hProcess, CreateEventW(nullptr, TRUE, FALSE, name), pi.dwProcessId, g_catchUpStart};
        CloseHandle(CreateThread(nullptr, 0, HandoffWait, h, 0, nullptr));
    } else {
        g_status = "Couldn't restart the game (" + std::to_string(GetLastError()) + ")";
        g_state = State::Failed;
    }
}

std::string g_text;  // returned to the menu (it copies the string)

const char* StateName() {
    bool between = g_state == State::Off || g_state == State::Connecting || g_state == State::Connected;
    if (g_returning && between && g_status.empty()) return "returning";   // back from a match, nearly in its lobby
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

// netplay.cpp: the match is over for us - the ONLINE screens (opened next) go back to its lobby.
void OnlineRejoin(const char* code, const char* token, int slot) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_rejoin = std::string(code) + " " + token + " " + std::to_string(slot);
    g_state = State::Off;   // a fresh connection (this game never had one)
}

void OnlineEnter() {
    OnlineMenuOpen(true);
    if (g_wantConnection) return;
    wchar_t server[256], name[64];
    SettingString(L"Online", L"Server", L"FGUYMOD_ONLINE_SERVER", L"", server, 256);
    // (FGUYMod.ini named the mod's server itself up to 0.9.4, and an update leaves it as it is: one of those is the
    // mod's server too, wherever it is now)
    g_serverSetting = Narrow(server);
    g_modServer = g_serverSetting.empty();
    for (const char* mod : {"91.226.56.14", "145.79.134.160", "87.76.191.217"})
        if (g_serverSetting.rfind(mod, 0) == 0) g_modServer = true;
    DWORD len = 64;
    wchar_t user[64] = L"Player";
    GetUserNameW(user, &len);
    SettingString(L"Online", L"Name", L"FGUYMOD_ONLINE_NAME", user, name, 64);
    if (!name[0]) wcscpy_s(name, user);
    g_name = CleanName(Narrow(name));
    // keyboard and mouse or controller online: [Online] Controls = auto | keyboard | controller
    // (auto: a controller if one is plugged in)
    wchar_t controls[16];
    SettingString(L"Online", L"Controls", L"FGUYMOD_ONLINE_CONTROLS", L"auto", controls, 16);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (_wcsicmp(controls, L"keyboard") == 0) g_mine.input = "k";
        else if (_wcsicmp(controls, L"controller") == 0) g_mine.input = "c";
        else g_mine.input = VPadRealPadConnected() ? "c" : "k";
    }
    StartLobbyThread();
}

void OnlineExit() {
    OnlineMenuOpen(false);
    SplitViewMenuCheck();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_state == State::Starting) return;
        if (g_state == State::InLobby) SendLine("LEAVE");
    }
    g_wantConnection = false;
}

// Everything the ONLINE screens show, one item per line, fields separated by '|':
//   S|<state>|<status>|<code>|<my slot>|<private>|<my name>|<server>|<my ping>|<match open>|<catching up>
//        match open 1: my lobby's match still runs and I can get back into it; catching up: how far, rejoining
//        state: off, connecting, online, lobby, starting, failed, returning (back from a match)
//   C|<mode>|<map>|<time>|<score>|<ff>|<rate>|<net>         the lobby's match settings (net 1: the new netcode)
//   O|<char>:<costume>.<costume>;...                         what the host's save has (in a lobby)
//   M|<slot>|<name>|<ready>|<ping>|<character>|<costume>|<team>|<input k/c>|<rate>|<skin>   one per player in my lobby
//        skin: the head picture of the custom skin they wear, if this PC has that skin too - "<movie>|l|r|b" with ~ for
//        | (cosmetics.cpp CosmeticsSkinHead), "" none
//   L|<code>|<host>|<players>|<max>|<mode>|<map>|<mods>     one per open lobby (when not in one; mods: names + -)
//   T|<number>|<name>|<text>                                  my lobby's chat, oldest first (the last 8 lines)
const char* LobbyState() {
    std::lock_guard<std::mutex> lock(g_lock);
    bool inLobby = g_state == State::InLobby || g_state == State::Starting;
    g_text = "S|" + std::string(StateName()) + "|" + Field(g_status) + "|" + g_code + "|" + std::to_string(g_mySlot) +
             "|" + (g_private ? "1" : "0") + "|" + Field(g_name) + "|" + Field(std::string()) + "|"   /* (the server's address: not to the menu) */ +
             std::to_string(g_rtt) + "|" + (g_matchOpen && g_state == State::InLobby ? "1" : "0") + "|" +
             Field(g_state == State::Starting ? g_progress : std::string()) + "\n";
    g_text += "C|" + g_conf + "|" + g_confRate + "|" + g_confNet + "\n";
    if (inLobby && !g_own.empty()) g_text += "O|" + g_own + "\n";
    if (inLobby) {
        std::string mine = CosmeticsMySkinKey();   // (my skin: told to the server when it changes - and shown for me
        if (g_serverSkins && g_state == State::InLobby && mine != g_skinSent) {   //  whatever the server knows)
            SendLine("SKIN " + (mine.empty() ? std::string("-") : mine));
            g_skinSent = mine;
        }
        for (size_t i = 0; i < g_members.size(); ++i) {
            const Member& m = g_members[i];
            std::string head = CosmeticsSkinHead((int)i == g_mySlot ? mine : m.skin);
            std::replace(head.begin(), head.end(), '|', '~');
            g_text += "M|" + std::to_string(i) + "|" + Field(m.name) + "|" + (m.away ? "2" : m.ready ? "1" : "0") + "|" +
                      std::to_string(m.rtt) + "|" + Field(m.character) + "|" + Field(m.costume) + "|" + Field(m.team) +
                      "|" + m.input + "|" + m.rate + "|" + Field(head) + "\n";
        }
        for (const ChatLine& t : g_chat) g_text += "T|" + std::to_string(t.seq) + "|" + Field(t.name) + "|" + Field(t.text) + "\n";
    } else {
        for (const LobbyInfo& l : g_lobbies)
            g_text += "L|" + l.code + "|" + Field(l.host) + "|" + std::to_string(l.players) + "|" +
                      std::to_string(l.max) + "|" + Field(l.mode) + "|" + Field(l.map) + "|" + Field(l.mods) + "\n";
    }
    return g_text.c_str();
}

// host <max> <private>   join <code>   leave   ready   start   pick <char> <costume> <team> [<input k/c>]
// conf <mode> <map> <time> <score> <ff>   own <list> (host)
void LobbyDo(const char* command, const char* a, const char* b, const char* c, const char* d, const char* e) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::string cmd = command ? command : "";
    g_status.clear();
    if (cmd == "host" || cmd == "join" || cmd == "ready" || cmd == "start" || cmd == "pick")
        Log(L"Online: lobby action %S %S %S", cmd.c_str(), a ? a : "", cmd == "pick" && b ? b : "");
    if (cmd == "host" && g_state == State::Connected) {
        g_newLobby = true;
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
    } else if (cmd == "rejoin" && g_state == State::InLobby && g_matchOpen) {   // back into the running match
        Log(L"Online: lobby action rejoin");
        SendLine("BACKIN");
    } else if (cmd == "ready" && g_state == State::InLobby && g_mySlot > 0 && !g_matchOpen) {
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
        if (d && (d[0] == 'k' || d[0] == 'c')) g_mine.input = d[0] == 'k' ? "k" : "c";
        if (g_state == State::InLobby) {
            if (g_mySlot >= 0 && g_mySlot < (int)g_members.size()) {  // show it right away
                g_members[g_mySlot].character = g_mine.character;
                g_members[g_mySlot].costume = g_mine.costume;
                g_members[g_mySlot].team = g_mine.team;
                g_members[g_mySlot].input = g_mine.input;
            }
            SendPicks();
        }
    } else if (cmd == "own" && g_state == State::InLobby && g_mySlot == 0) {
        g_own = CleanList(a);
        char test[800];  // test knob: pretend the host's save has only this
        if (GetEnvironmentVariableA("FGUYMOD_TEST_OWN", test, sizeof(test))) g_own = CleanList(test);
        if (!g_own.empty()) SendLine("OWN " + g_own);
    } else if (cmd == "conf" && g_state == State::InLobby && g_mySlot == 0) {
        g_conf = CleanToken(a) + "|" + CleanToken(b) + "|" + CleanToken(c) + "|" + CleanToken(d) + "|" + CleanToken(e);
        SendLine("CONF " + CleanToken(a) + " " + CleanToken(b) + " " + CleanToken(c) + " " + CleanToken(d) + " " +
                 CleanToken(e) + " " + g_confRate + " " + g_confNet);
    } else if (cmd == "say" && g_state == State::InLobby && a) {   // a chat line (UTF-8)
        std::string text;
        for (const char* p = a; *p && text.size() < 300; ++p)
            if ((unsigned char)*p >= 0x20 && *p != '|') text += *p;
        if (!text.empty()) SendLine("SAY " + text);
    } else if (cmd == "rate" && g_state == State::InLobby && g_mySlot == 0) {   // the match's frame rate (0 auto)
        g_confRate = CleanToken(a);
        std::string line = "CONF";
        for (auto& v : Split(g_conf, '|')) line += " " + v;
        SendLine(line + " " + g_confRate + " " + g_confNet);
    } else if (cmd == "net" && g_state == State::InLobby && g_mySlot == 0) {   // the match's netcode (0 classic, 1 new)
        g_confNet = a && a[0] == '1' ? "1" : "0";
        std::string line = "CONF";
        for (auto& v : Split(g_conf, '|')) line += " " + v;
        SendLine(line + " " + g_confRate + " " + g_confNet);
    }
}

// This (lobby) game shows the online loading screen while the match's game boots hidden (d3d9.cpp: 60 fps then).
bool OnlineStartingMatch() { return g_startingMatch; }

// In a session: the snapshot of everyone's choices ("mode,map,time,score,ff|char:costume:team;..."), else "".
const char* SessionConfig() {
    static char buf[512];
    if (!GetEnvironmentVariableA("FGUYMOD_SESSION", buf, sizeof(buf))) buf[0] = 0;
    return buf;
}

// The mod's version (updater.cpp compares it with the latest release).
const char* ModVersion() { return kModVersion; }

const char* OnlineRematchPending() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\rematch_%lu.txt", g_modDir, GetCurrentProcessId());
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") == 0 && f) {
        char buf[16] = {};
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        DeleteFileW(path);
        g_rematch = strcmp(buf, "next") == 0 ? "next" : "restart";
        g_rematchAt = GetTickCount64();
        Log(L"Online: back for the next match (%S) - the lobby readies / starts it by itself", g_rematch.c_str());
    }
    if (!g_rematch.empty() && GetTickCount64() - g_rematchAt > 60000) g_rematch.clear();
    return g_rematch.c_str();
}
void OnlineRematchDone() {
    if (!g_rematch.empty()) Log(L"Online: the next match's start - done here");
    g_rematch.clear();
}
