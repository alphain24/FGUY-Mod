// Emotes (the emote mod): emote packs made with the Skin Studio's Emotes tab, in FGUYMod\emotes (*.fgemote) - a zip
// with emote.json (its name, character, the outfits of that character "models": [[lo, hi], ...], the game bones it
// moves "uses", "loop", "duration") and motion.bin (motion.cpp's format), both stored.
// Playing one: hold Back and press a D-pad direction (up, right, down, left: slots 1-4) on a controller - the game
// doesn't see those presses - or press F5-F8 on the keyboard. A slot is the nth emote switched on ([Emotes] in
// FGUYMod.ini, on unless switched off) for the character you're playing, by name. Only the bones an emote moves
// leave the game's animation (a wave while walking is fine); one that loops or moves the hips / legs stops when you
// move. Pressing its slot again stops it.
#include "common.h"

#include <xinput.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Emote {
    std::wstring file;
    std::string name, character;
    std::vector<std::pair<unsigned, unsigned>> models;   // (the character's outfits)
    std::vector<bool> uses;                              // (the game bones it moves)
    bool loop = false, legs = false;                     // (legs: it moves the root, hips or legs)
    std::shared_ptr<const MotionClip> clip;
    FILETIME written{};
    DWORD size = 0;
    std::vector<unsigned char> data;                     // (the pack: sent to the others online)
};
std::vector<Emote> g_emotes;
ULONGLONG g_scannedAt = 0;

volatile LONG g_request[4] = {-1, -1, -1, -1};   // (a slot's press, per player: 0-3, -1 none)
struct Playing { bool on = false; int emote = -1; float x = 0, z = 0; bool stopOnMove = false; };   // (emote: mine, -1 theirs)
Playing g_playing[4];
CRITICAL_SECTION g_remoteLock;   // (another player's emote, from the network: played on the next frame)
bool g_remoteInit = false;
struct Remote { bool pending = false; std::vector<unsigned char> pack; };   // (pack empty: stop)
Remote g_remote[4];

// a tiny reader for emote.json's few values (the Studio writes it; not a general JSON reader)
size_t Find(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t at = js.find(k);
    if (at == std::string::npos) return at;
    at = js.find(':', at + k.size());
    return at == std::string::npos ? at : js.find_first_not_of(" \t\r\n", at + 1);
}
std::string JsString(const std::string& js, const char* key) {
    size_t at = Find(js, key);
    if (at == std::string::npos || js[at] != '"') return "";
    size_t end = js.find('"', at + 1);
    return end == std::string::npos ? "" : js.substr(at + 1, end - at - 1);
}
bool JsBool(const std::string& js, const char* key) {
    size_t at = Find(js, key);
    return at != std::string::npos && js.compare(at, 4, "true") == 0;
}
std::vector<double> JsNumbers(const std::string& js, const char* key) {   // (an array's numbers, nested ones too)
    std::vector<double> out;
    size_t at = Find(js, key);
    if (at == std::string::npos || js[at] != '[') return out;
    int depth = 0;
    for (size_t i = at; i < js.size(); ++i) {
        char c = js[i];
        if (c == '[') ++depth;
        else if (c == ']') { if (--depth == 0) break; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            char* end = nullptr;
            out.push_back(strtod(js.c_str() + i, &end));
            i = end - js.c_str() - 1;
        }
    }
    return out;
}

std::wstring EmotesDir() { return std::wstring(g_modDir) + L"\\emotes"; }
std::wstring IniPath() { return std::wstring(g_modDir) + L"\\FGUYMod.ini"; }
bool EmoteOn(const std::wstring& file) {
    return GetPrivateProfileIntW(L"Emotes", file.c_str(), 1, IniPath().c_str()) != 0;
}

bool ParseEmote(const std::vector<unsigned char>& d, Emote& e) {
    std::vector<unsigned char> json, motion;
    if (!ZipRead(d, "emote.json", json) || !ZipRead(d, "motion.bin", motion)) return false;
    std::string js(json.begin(), json.end());
    e.name = JsString(js, "name");
    e.character = JsString(js, "character");
    e.loop = JsBool(js, "loop");
    std::vector<double> m = JsNumbers(js, "models");
    for (size_t i = 0; i + 1 < m.size(); i += 2) e.models.push_back({(unsigned)m[i], (unsigned)m[i + 1]});
    e.clip = MotionParse(motion.data(), motion.size());
    if (!e.clip || e.models.empty()) return false;
    std::vector<double> uses = JsNumbers(js, "uses");
    e.uses.assign(MotionBones(*e.clip), uses.empty());
    for (double u : uses)
        if (u >= 0 && u < e.uses.size()) e.uses[(size_t)u] = true;
    e.legs = JsBool(js, "legs") || (e.uses.size() > 1 && e.uses[1]);   // (game bone 1: the skeleton's root)
    return true;
}

bool LoadEmote(const std::wstring& path, Emote& e) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::vector<unsigned char> d(size < 32 * 1024 * 1024 ? size : 0);
    bool ok = !d.empty() && ReadFile(f, d.data(), size, &got, nullptr) && got == size;
    CloseHandle(f);
    if (!ok || !ParseEmote(d, e)) return false;
    e.data.swap(d);
    return true;
}

// FGUYMod\emotes, read again when it changed (a file's time or size) - at most every 2 s
void Scan() {
    ULONGLONG now = GetTickCount64();
    if (g_scannedAt && now - g_scannedAt < 2000) return;
    g_scannedAt = now;
    std::vector<Emote> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((EmotesDir() + L"\\*.fgemote").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { g_emotes.clear(); return; }
    do {
        auto old = std::find_if(g_emotes.begin(), g_emotes.end(), [&](const Emote& e) {
            return e.file == fd.cFileName && e.size == fd.nFileSizeLow && CompareFileTime(&e.written, &fd.ftLastWriteTime) == 0;
        });
        if (old != g_emotes.end()) { found.push_back(*old); continue; }
        Emote e;
        e.file = fd.cFileName;
        e.size = fd.nFileSizeLow;
        e.written = fd.ftLastWriteTime;
        if (LoadEmote(EmotesDir() + L"\\" + fd.cFileName, e)) {
            Log(L"Emotes: %s - \"%S\" for %S (%d bones, %.2f s%s)", e.file.c_str(), e.name.c_str(), e.character.c_str(),
                MotionBones(*e.clip), MotionLength(*e.clip), e.loop ? L", loops" : L"");
            found.push_back(std::move(e));
        } else {
            Log(L"Emotes: %s isn't an emote pack this version can read", fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const Emote& a, const Emote& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    g_emotes.swap(found);
}

unsigned char* PlayerOf(int slot) {
    unsigned char* players[8];
    int n = GamePlayers(players, 8);
    for (int i = 0; i < n; ++i)
        if (*reinterpret_cast<int*>(players[i] + 0xdf8) == slot) return players[i];
    return nullptr;
}
bool Where(int slot, float* x, float* z) {
    unsigned char* p = PlayerOf(slot);
    auto xf = p ? *reinterpret_cast<unsigned char**>(p + 0x38) : nullptr;
    if (!xf || IsBadReadPtr(xf + 0x30, 12)) return false;
    *x = *reinterpret_cast<float*>(xf + 0x30);
    *z = *reinterpret_cast<float*>(xf + 0x38);
    return true;
}

void Start(int slot, int nth) {
    Scan();
    unsigned lo = 0, hi = 0;
    if (!CosmeticsPlayerModel(slot, 0, &lo, &hi)) return;
    std::vector<int> mine;   // (the emotes switched on for this character)
    for (int i = 0; i < (int)g_emotes.size(); ++i) {
        const Emote& e = g_emotes[i];
        if (!EmoteOn(e.file)) continue;
        if (std::find(e.models.begin(), e.models.end(), std::make_pair(lo, hi)) != e.models.end()) mine.push_back(i);
    }
    if (nth >= (int)mine.size()) {
        Log(L"Emotes: player %d - no emote in slot %d for this character (%zu on)", slot, nth + 1, mine.size());
        return;
    }
    int i = mine[nth];
    Playing& pl = g_playing[slot];
    bool online = NetplayEnabled() && slot == NetplayLocalSlot();
    if (pl.on && pl.emote == i && MotionPlaying(slot)) {   // (its slot again: stop)
        MotionStop(slot);
        pl.on = false;
        if (online) NetplayEmote(nullptr);
        return;
    }
    const Emote& e = g_emotes[i];
    MotionPlay(slot, e.clip, e.uses, e.loop);
    pl = {true, i, 0, 0, e.loop || e.legs};
    Where(slot, &pl.x, &pl.z);
    if (online) NetplayEmote(&e.data);   // (everyone's PC plays it on my player too)
    Log(L"Emotes: player %d - \"%S\"", slot, e.name.c_str());
}

// another player's emote (online), on this PC: as theirs plays it
void PlayRemote(int slot, const std::vector<unsigned char>& pack) {
    Playing& pl = g_playing[slot];
    if (pack.empty()) {
        if (pl.on) MotionStop(slot);
        pl.on = false;
        return;
    }
    Emote e;
    if (!ParseEmote(pack, e)) { Log(L"Emotes: player %d's emote pack can't be read here", slot); return; }
    MotionPlay(slot, e.clip, e.uses, e.loop);
    pl = {true, -1, 0, 0, e.loop || e.legs};
    Where(slot, &pl.x, &pl.z);
    Log(L"Emotes: player %d (online) - \"%S\"", slot, e.name.c_str());
}

// whose presses: offline a controller is its player (pad n: slot n), the keyboard player 0; online, mine
int SlotFor(int pad) {
    int mine = NetplayLocalSlot();
    return mine >= 0 ? mine : pad;
}

}  // namespace

// a controller's state, before the game (or the lockstep) reads it: Back + a D-pad direction is an emote - neither
// reaches the game. Back is kept from the game while it's held; let go without a direction, the game gets it then
// (a short press of its own).
void EmotesPad(DWORD pad, XINPUT_STATE* st) {
    if (pad >= 4 || !st) return;
    const WORD dpad = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT;
    static bool held[4], combo[4];
    static WORD last[4];
    static ULONGLONG backUntil[4];   // (Back let go alone: the game's own press, until then)
    WORD& b = st->Gamepad.wButtons;
    WORD pressed = b & ~last[pad];
    last[pad] = b;
    if (b & XINPUT_GAMEPAD_BACK) {
        if (!held[pad]) { held[pad] = true; combo[pad] = false; }
        const WORD dirs[4] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT};
        for (int k = 0; k < 4; ++k)
            if (pressed & dirs[k]) {
                combo[pad] = true;
                InterlockedExchange(&g_request[SlotFor((int)pad) & 3], k);
            }
        b &= ~(XINPUT_GAMEPAD_BACK | dpad);
        return;
    }
    if (held[pad]) {
        held[pad] = false;
        if (!combo[pad]) backUntil[pad] = GetTickCount64() + 60;
    }
    if (backUntil[pad] > GetTickCount64()) b |= XINPUT_GAMEPAD_BACK;
}

void EmotesRemote(int slot, const std::vector<unsigned char>* pack) {
    if (slot < 0 || slot >= 4) return;
    if (!g_remoteInit) { InitializeCriticalSection(&g_remoteLock); g_remoteInit = true; }
    EnterCriticalSection(&g_remoteLock);
    g_remote[slot].pending = true;
    if (pack) g_remote[slot].pack = *pack; else g_remote[slot].pack.clear();
    LeaveCriticalSection(&g_remoteLock);
}

// FGUYMOD_EmoteList (the MODS page): "<file>|<name>|<character>|<on>|<slot: 1-4 for its character, 0 none>\n..."
const char* EmotesList() {
    static std::string text;
    g_scannedAt = 0;   // (fresh)
    Scan();
    text.clear();
    std::vector<std::pair<std::string, int>> count;   // (a character's emotes switched on so far: its slots)
    for (const Emote& e : g_emotes) {
        char f[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, e.file.c_str(), -1, f, sizeof(f), nullptr, nullptr);
        std::string n = e.name, c = e.character;
        for (char& ch : n) if (ch == '|' || ch == '\n') ch = ' ';
        for (char& ch : c) if (ch == '|' || ch == '\n') ch = ' ';
        bool on = EmoteOn(e.file);
        int slot = 0;
        if (on) {
            auto it = std::find_if(count.begin(), count.end(), [&](const auto& x) { return x.first == e.character; });
            if (it == count.end()) { count.push_back({e.character, 0}); it = count.end() - 1; }
            slot = ++it->second <= 4 ? it->second : 0;
        }
        text += std::string(f) + "|" + n + "|" + c + "|" + (on ? "1" : "0") + "|" + std::to_string(slot) + "\n";
    }
    return text.c_str();
}

// FGUYMOD_EmoteToggle(<file>): on / off
void EmotesToggle(const char* file) {
    wchar_t w[MAX_PATH] = L"";
    if (file) MultiByteToWideChar(CP_UTF8, 0, file, -1, w, MAX_PATH);
    if (!w[0] || wcschr(w, L'\\') || wcschr(w, L'/')) return;
    bool on = EmoteOn(w);
    WritePrivateProfileStringW(L"Emotes", w, on ? L"0" : L"1", IniPath().c_str());
    Log(L"Emotes: %s %s", w, on ? L"off" : L"on");
}

void EmotesRequest(int slot, int nth) {   // (test pipe "emote <slot> <1-4>")
    if (slot >= 0 && slot < 4 && nth >= 0 && nth < 4) InterlockedExchange(&g_request[slot], nth);
}

// every frame (the render thread): F5-F8, the presses, stopping when the player moves
void EmotesFrame() {
    static bool keyDown[4];
    HWND wnd = DisplayMainWindow();
    bool front = wnd && GetForegroundWindow() == wnd;
    for (int k = 0; k < 4; ++k) {
        bool down = front && (GetAsyncKeyState(VK_F5 + k) & 0x8000);
        if (down && !keyDown[k]) InterlockedExchange(&g_request[SlotFor(0) & 3], k);
        keyDown[k] = down;
    }
    if (!g_remoteInit) { InitializeCriticalSection(&g_remoteLock); g_remoteInit = true; }
    for (int s = 0; s < 4; ++s) {
        LONG nth = InterlockedExchange(&g_request[s], -1);
        if (nth >= 0) Start(s, nth);
        EnterCriticalSection(&g_remoteLock);
        Remote r;
        std::swap(r, g_remote[s]);
        LeaveCriticalSection(&g_remoteLock);
        if (r.pending) PlayRemote(s, r.pack);
        Playing& pl = g_playing[s];
        if (!pl.on) continue;
        if (!MotionPlaying(s)) { pl.on = false; continue; }
        float x, z;   // (moving stops it - on every PC alike: the same player, the same game)
        if (pl.stopOnMove && Where(s, &x, &z) && (x - pl.x) * (x - pl.x) + (z - pl.z) * (z - pl.z) > 0.25f * 0.25f) {
            MotionStop(s);
            pl.on = false;
        }
    }
}
