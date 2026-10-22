// Emotes (the FGUY Emotes mod): emote packs (*.fgemote, made with the Skin Studio's Emotes tab) in the emotes folder of
// the mods that are on (and the loose FGUYMod\emotes) - a zip with emote.json (its name, character, the outfits of that
// character "models": [[lo, hi], ...], the game bones it moves "uses", "loop", "duration") and motion.bin (motion.cpp's
// format), both stored.
// Playing one: the emote wheel (below) - the emotes for the character you're playing, by name. Only the bones an emote
// moves leave the game's animation (a wave while walking is fine); one that loops or moves the hips / legs stops when
// you move. Picking the one playing again stops it. Online everyone sees it (netplay.cpp).
#include "common.h"

#include <xinput.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Emote {
    std::wstring file, from;                             // (its file name, the mod it's from - "" FGUYMod\emotes)
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

// (the same CRC-32 as netplay.cpp's: a pack someone plays online, found here by it - no need to fetch it)
unsigned Crc(const std::vector<unsigned char>& d) {
    unsigned c = 0xFFFFFFFFu;
    for (unsigned char b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return d.empty() ? 0 : c ^ 0xFFFFFFFFu;
}
struct LocalLock { LocalLock() { InitializeCriticalSection(&cs); } CRITICAL_SECTION cs; };
CRITICAL_SECTION& LocalCs() { static LocalLock l; return l.cs; }   // (the network thread reads g_local too)
std::vector<std::pair<unsigned, std::vector<unsigned char>>> g_local;

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

// the emotes there are - the Emotes mod's (and any other mod's that's on), then FGUYMod\emotes - read again when one
// changed (a file's time or size), at most every 2 s
void Scan() {
    ULONGLONG now = GetTickCount64();
    if (g_scannedAt && now - g_scannedAt < 2000) return;
    g_scannedAt = now;
    std::vector<Emote> found;
    for (const auto& folder : ModsContentFolders(L"emotes")) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder.first + L"\\*.fgemote").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (std::any_of(found.begin(), found.end(), [&](const Emote& e) { return e.file == fd.cFileName; })) continue;
            auto old = std::find_if(g_emotes.begin(), g_emotes.end(), [&](const Emote& e) {
                return e.file == fd.cFileName && e.from == folder.second && e.size == fd.nFileSizeLow &&
                       CompareFileTime(&e.written, &fd.ftLastWriteTime) == 0;
            });
            if (old != g_emotes.end()) { found.push_back(*old); continue; }
            Emote e;
            e.file = fd.cFileName;
            e.from = folder.second;
            e.size = fd.nFileSizeLow;
            e.written = fd.ftLastWriteTime;
            if (LoadEmote(folder.first + L"\\" + fd.cFileName, e)) {
                Log(L"Emotes: %s - \"%S\" for %S (%d bones, %.2f s%s)", e.file.c_str(), e.name.c_str(), e.character.c_str(),
                    MotionBones(*e.clip), MotionLength(*e.clip), e.loop ? L", loops" : L"");
                found.push_back(std::move(e));
            } else {
                Log(L"Emotes: %s isn't an emote pack this version can read", fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(), [](const Emote& a, const Emote& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    g_emotes.swap(found);
    EnterCriticalSection(&LocalCs());   // (what the network thread may look up: each pack by its CRC)
    g_local.clear();
    for (const Emote& e : g_emotes) g_local.push_back({Crc(e.data), e.data});
    LeaveCriticalSection(&LocalCs());
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

// the emotes for the character a player plays now: g_emotes' indexes, by name ("" none)
std::vector<int> EmotesOf(int slot) {
    std::vector<int> out;
    unsigned char* p = PlayerOf(slot);
    auto xf = p ? *reinterpret_cast<unsigned char**>(p + 0x38) : nullptr;
    unsigned char* model = xf && !IsBadReadPtr(xf + 0x1d4, 4) ? *reinterpret_cast<unsigned char**>(xf + 0x1d4) : nullptr;
    if (!model) return out;
    static unsigned char* known[4];   // (the outfit's id, found once a model: a walk through the scene's assets)
    static unsigned knownLo[4], knownHi[4];
    if (known[slot & 3] != model) {
        unsigned lo = 0, hi = 0;
        if (!CosmeticsPlayerModel(slot, 0, &lo, &hi)) return out;
        known[slot & 3] = model;
        knownLo[slot & 3] = lo;
        knownHi[slot & 3] = hi;
    }
    auto id = std::make_pair(knownLo[slot & 3], knownHi[slot & 3]);
    for (int i = 0; i < (int)g_emotes.size(); ++i)
        if (std::find(g_emotes[i].models.begin(), g_emotes[i].models.end(), id) != g_emotes[i].models.end()) out.push_back(i);
    return out;
}

void Start(int slot, int i) {
    if (i < 0 || i >= (int)g_emotes.size()) return;
    Playing& pl = g_playing[slot];
    bool online = NetplayEnabled() && slot == NetplayLocalSlot();
    if (pl.on && pl.emote == i && MotionPlaying(slot)) {   // (the one playing again: stop)
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

// ---- the emote wheel ------------------------------------------------------------------------------------------------
// Held Back (a controller) or B (the keyboard) opens it - in a level, when the character you play has emotes: 8 a
// page around a circle; the left stick / the mouse points at one, LB / RB / the mouse wheel turn the page; let go,
// and the one pointed at plays. While it's open the game doesn't get the left stick (the controller's wheel) or the
// mouse (the keyboard's). A short Back (the wheel not open yet) still reaches the game: its objective screen.
// The HUD draws it (pause.as2js FGUYEmotePoll) from FGUYMOD_EmoteWheel.
const int kPerPage = 8;
const DWORD kOpenAfter = 180;   // (ms Back is held before the wheel opens - a short press stays the game's)
struct Wheel {
    bool open = false, play = false;
    int by = 0, pad = 0, slot = 0;   // by: 1 a controller (pad), 2 the keyboard
    float x = 0, y = 0;              // where it points: the stick (-1..1), the mouse (counts, up +)
    int page = 0, turn = 0, sel = -1;
    std::vector<int> items;          // (the character's emotes, g_emotes' indexes)
};
Wheel g_wheel;
volatile LONG g_wheelOk = 0;
volatile bool g_testKey = false;     // (test pipe "emotekey": B held)         // (a wheel may open now: in a level, HUD up, no menu, emotes there)
CRITICAL_SECTION& WheelCs() { static LocalLock l; return l.cs; }
std::string g_wheelText = "0";

void WheelOpen(int by, int pad, int slot) {
    if (g_wheel.open) return;
    g_wheel = Wheel();
    g_wheel.open = true;
    g_wheel.by = by;
    g_wheel.pad = pad;
    g_wheel.slot = slot;
}

// (the render thread) what it shows: "1|selected|page|pages|by|name~name~..." - or "0"
void WheelUpdate() {
    EnterCriticalSection(&WheelCs());
    Wheel& w = g_wheel;
    if (w.open && w.items.empty()) w.items = EmotesOf(w.slot);
    if (w.open && w.items.empty()) w.open = false;
    std::string text = "0";
    if (w.open) {
        int pages = ((int)w.items.size() + kPerPage - 1) / kPerPage;
        if (w.turn) { w.page = ((w.page + w.turn) % pages + pages) % pages; w.turn = 0; w.sel = -1; }
        int first = w.page * kPerPage, n = (int)w.items.size() - first < kPerPage ? (int)w.items.size() - first : kPerPage;
        float len = sqrtf(w.x * w.x + w.y * w.y);
        if (len >= (w.by == 1 ? 0.45f : 40.0f)) {   // (pointing: the slice that way - 0 at the top, clockwise)
            float a = atan2f(w.x, w.y);
            if (a < 0) a += 6.2831853f;
            w.sel = (int)floorf(a / (6.2831853f / n) + 0.5f) % n;
        }
        if (w.by == 2 && len > 160.0f) { w.x *= 160.0f / len; w.y *= 160.0f / len; }   // (the mouse: no further out)
        char head[64];
        sprintf_s(head, "1|%d|%d|%d|%d|", w.sel, w.page, pages, w.by);
        text = head;
        for (int k = 0; k < n; ++k) {
            std::string name = g_emotes[w.items[first + k]].name;
            for (char& c : name) if (c == '|' || c == '~') c = ' ';
            text += (k ? "~" : "") + name;
        }
    }
    int play = -1, slot = w.slot;
    if (w.play) {   // (let go: the one pointed at)
        if (w.sel >= 0 && w.page * kPerPage + w.sel < (int)w.items.size()) play = w.items[w.page * kPerPage + w.sel];
        g_wheel = Wheel();
        text = "0";
    }
    g_wheelText = text;
    LeaveCriticalSection(&WheelCs());
    if (play >= 0) Start(slot, play);
}

}  // namespace

// a controller's state, before the game (or the lockstep) reads it: held Back is the emote wheel (see above)
void EmotesPad(DWORD pad, XINPUT_STATE* st) {
    if (pad >= 4 || !st) return;
    static bool held[4], opened[4];
    static ULONGLONG heldAt[4], backUntil[4];   // (backUntil: Back let go early - the game's own press until then)
    static WORD last[4];
    WORD& b = st->Gamepad.wButtons;
    WORD pressed = b & ~last[pad];
    last[pad] = b;
    ULONGLONG now = GetTickCount64();
    if ((b & XINPUT_GAMEPAD_BACK) && (held[pad] || g_wheelOk)) {
        if (!held[pad]) { held[pad] = true; opened[pad] = false; heldAt[pad] = now; }
        float lx = st->Gamepad.sThumbLX / 32767.0f, ly = st->Gamepad.sThumbLY / 32767.0f;
        EnterCriticalSection(&WheelCs());
        if (!opened[pad] && (now - heldAt[pad] >= kOpenAfter || lx * lx + ly * ly > 0.25f)) {
            opened[pad] = true;
            WheelOpen(1, (int)pad, SlotFor((int)pad));
        }
        if (opened[pad] && g_wheel.open && g_wheel.by == 1 && g_wheel.pad == (int)pad) {
            g_wheel.x = lx;
            g_wheel.y = ly;
            if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) --g_wheel.turn;
            if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) ++g_wheel.turn;
        }
        LeaveCriticalSection(&WheelCs());
        b &= ~(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
        if (opened[pad]) st->Gamepad.sThumbLX = st->Gamepad.sThumbLY = 0;   // (the stick is the wheel's: no walking)
        return;
    }
    if (held[pad]) {
        held[pad] = false;
        EnterCriticalSection(&WheelCs());
        if (opened[pad] && g_wheel.open && g_wheel.by == 1 && g_wheel.pad == (int)pad) g_wheel.play = true;
        LeaveCriticalSection(&WheelCs());
        if (!opened[pad]) backUntil[pad] = now + 60;
    }
    if (backUntil[pad] > now) b |= XINPUT_GAMEPAD_BACK;
}

// the mouse's movement (offline the game's raw poll, online what this PC sends): the keyboard's wheel open, it's the
// wheel's - true: taken (zeroed)
bool EmotesMouse(LONG* dx, LONG* dy, LONG* dz) {
    EnterCriticalSection(&WheelCs());
    bool mine = g_wheel.open && g_wheel.by == 2;
    if (mine) {
        g_wheel.x += *dx;
        g_wheel.y -= *dy;   // (the mouse's y grows downward)
        if (*dz) g_wheel.turn += *dz > 0 ? -1 : 1;
        *dx = *dy = *dz = 0;
    }
    LeaveCriticalSection(&WheelCs());
    return mine;
}

// FGUYMOD_EmoteWheel (the HUD, every poll): "1|selected|page|pages|by|name~name~..." - or "0" (closed)
const char* EmotesWheelText() {
    static std::string copy;
    EnterCriticalSection(&WheelCs());
    copy = g_wheelText;
    LeaveCriticalSection(&WheelCs());
    return copy.c_str();
}

// netplay.cpp: an emote pack someone plays, if it's here too (the same Emotes mod) - by its CRC
bool EmotesLocalPack(unsigned crc, std::vector<unsigned char>& out) {
    EnterCriticalSection(&LocalCs());
    bool found = false;
    for (const auto& l : g_local)
        if (l.first == crc) { out = l.second; found = true; break; }
    LeaveCriticalSection(&LocalCs());
    return found;
}

void EmotesRemote(int slot, const std::vector<unsigned char>* pack) {
    if (slot < 0 || slot >= 4) return;
    if (!g_remoteInit) { InitializeCriticalSection(&g_remoteLock); g_remoteInit = true; }
    EnterCriticalSection(&g_remoteLock);
    g_remote[slot].pending = true;
    if (pack) g_remote[slot].pack = *pack; else g_remote[slot].pack.clear();
    LeaveCriticalSection(&g_remoteLock);
}

void EmotesTestKey(bool down, int dx, int dy) {   // (test pipe "emotekey <0|1> [dx dy]": B, and the mouse moved)
    g_testKey = down;
    LONG x = dx, y = dy, z = 0;
    if (dx || dy) EmotesMouse(&x, &y, &z);
}

void EmotesRequest(int slot, int nth) {   // (test pipe "emote <slot> <n>": the character's nth emote)
    if (slot >= 0 && slot < 4 && nth >= 0) InterlockedExchange(&g_request[slot], nth);
}

// every frame (the render thread): B, the wheel, others' emotes, stopping when the player moves
void EmotesFrame() {
    Scan();   // (every 2 s at most: the mods' emotes - and, online, the packs here for the others' emotes)
    HWND wnd = DisplayMainWindow();
    bool front = wnd && GetForegroundWindow() == wnd;
    int me = SlotFor(0);
    bool hud = VideoHudUp(), menus = OnlinePauseMenusUp(), some = !EmotesOf(me < 0 ? 0 : me).empty();
    bool ok = hud && !menus && some;
    static int lastWhy = -1;
    int why = (hud ? 1 : 0) | (menus ? 2 : 0) | (some ? 4 : 0);
    if (why != lastWhy) {
        lastWhy = why;
        Log(L"Emotes: the wheel %s (HUD %s, a menu %s, emotes for this character %s)", ok ? L"can open" : L"can't open",
            hud ? L"up" : L"not up", menus ? L"up" : L"not up", some ? L"there" : L"none");
    }
    InterlockedExchange(&g_wheelOk, ok ? 1 : 0);
    static bool keyDown;
    bool down = (front && (GetAsyncKeyState('B') & 0x8000)) || g_testKey;
    EnterCriticalSection(&WheelCs());
    if (down && !keyDown && ok) WheelOpen(2, 0, me);
    if (!down && keyDown && g_wheel.open && g_wheel.by == 2) g_wheel.play = true;
    if (!ok && g_wheel.open && !g_wheel.play) g_wheel = Wheel();   // (a menu came up, the level ended...)
    LeaveCriticalSection(&WheelCs());
    keyDown = down;
    WheelUpdate();
    if (!g_remoteInit) { InitializeCriticalSection(&g_remoteLock); g_remoteInit = true; }
    for (int s = 0; s < 4; ++s) {
        LONG nth = InterlockedExchange(&g_request[s], -1);
        if (nth >= 0) {
            std::vector<int> mine = EmotesOf(s);
            if (nth < (int)mine.size()) Start(s, mine[nth]);
            else Log(L"Emotes: player %d - no emote %d for this character (%zu)", s, nth + 1, mine.size());
        }
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
