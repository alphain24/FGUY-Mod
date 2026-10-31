// First person: a player's own camera at their character's eyes.
// The player's camera (zFGUYCamThirdPerson, at player+0xf148) orbits a point above the player, offset from them by its
// character's camera offsets for the camera mode they're in: 0x647890 (this = the player; ret 4) hands back a pointer to
// them - side +0x0, height above the feet +0x4, ahead along the view +0x8, distance +0xc (the getters 0x647a70 /
// 0x647ad0 / 0x647b30 / 0x647930 read them, blending between modes). They're the character's, shared by every player of
// it, so for a player in first person 0x647890 hands back a copy of their own instead: the orbit point at their eyes
// and the distance 0 - the view turns in place, pitching within the camera settings' limits ([camera+0x178] +0x94 /
// +0x98, widened while their camera updates: 0x588c40, the class's vtable slot 8). It's the game's own camera, so aim
// (through the screen's centre - 0x6568c0), hits and the view all come from the eyes. Online every PC runs every
// player's camera: who is in first person has to be the same on all of them.
// Their own body isn't drawn in their own view while they're in first person (graphics.cpp / cosmetics.cpp ask).
// Test pipe: "fpv <player> <0|1> [height] [forward] [pitch limit]", "fpv hide <0|1|2>", "fpv watch <hex address>".
#include "common.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

const uintptr_t kCamVtable = 0xa19c8c;       // zFGUYCamThirdPerson's vtable (its update, 0x588c40, in slot 8)
const int kUpdateSlot = 8;
const uintptr_t kOffsets = 0x647890;         // the player's camera offsets for a mode: cmp byte ptr [esp+4], 0 (5 bytes)
const unsigned char kOffsetsStart[5] = {0x80, 0x7c, 0x24, 0x04, 0x00};
const uintptr_t kPlayers = 0xbad988;         // the game's player list: -> 8 pointers, the count at +0x20
const int kCamera = 0xf148, kPlayerIndex = 0xdf8, kViewport = 0xdfc;
const int kCamSettings = 0x178, kPitchMin = 0x94, kPitchMax = 0x98;
const float kGlide = 0.25f;                  // seconds from one view to the other
// the eye, of the character's camera height (Peter's 2.2: 1.76 up, 0.22 ahead, 0.4 to the left - the gun, held at the right
// shoulder as they shoot, comes up at the bottom right of the view instead of across its middle)
const float kEyeHeight = 0.8f, kEyeAhead = 0.1f, kEyeSide = -0.18f;

using Update_t = void(__thiscall*)(void* camera, int a, float dt);
Update_t g_real = nullptr;
void* g_offsetsGame = nullptr;               // (the game's 0x647890: its first instruction, then a jump back)

// The switch: a key (V) for keyboard players, the Back button twice within kDoubleTap for controller players - read from
// the device the game plays the player from (online: their inputs, the same on every PC), as their camera updates.
const uintptr_t kInputHuman = 0xa1a29c;      // zPlayerInputHuman's vtable (the player's input: player+0x101c)
const int kInput = 0x101c, kInputDevice = 0x30, kDeviceType = 8, kPadButtons = 0x14, kKeys = 0x484;
const int kKeyboard = 8;                     // (device types: 8 keyboard / mouse, 4 a controller)
const WORD kBack = 0x0002;                   // (the game's controller bits: Back 2 - the scoreboard)
const float kDoubleTap = 0.4f;
int g_key = 'V';
WORD g_padButton = kBack;
bool g_default = false;                      // [Camera] FirstPerson: players start in first person
bool g_mineOnline = false;                   // online: my player in first person (my PC's - it goes with my input)

struct View {
    bool on = false;
    unsigned char* player = nullptr;                     // (the player object it was set up for)
    bool down = false;                                   // (the switch: held last time)
    float sinceTap = 1e9f;                               //   (seconds since the controller button's last press)
    float height = 0, forward = -1, pitch = 1.4f;      // (0 / -1: from the character's camera - kEyeHeight / kEyeAhead)
    float side = -99;                                    // (to their right; -99: kEyeSide)
    float blend = 0;                                     // 0 third person .. 1 first person
    int viewport = -1;                                   // (their view)
    float offsets[16];                                   // (their camera offsets, 0x647890's)
};
View g_view[8];
volatile LONG g_any = 0;
int g_hideMode = 1;   // their own body in their view: 1 clipped near the eyes (research: 0 as it is, 2 not drawn)
float g_near = 0.3f;  //   (how far ahead of the eyes it's clipped)

// A listed player's index (player+0xdf8), or -1.
int IndexOf(unsigned char* player) {
    auto list = *reinterpret_cast<unsigned char***>(kPlayers);
    if (!list) return -1;
    int count = *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(list) + 0x20);
    for (int i = 0; i < count && i < 8; ++i)
        if (list[i] == player) {
            int index = *reinterpret_cast<int*>(player + kPlayerIndex);
            return index >= 0 && index < 8 ? index : -1;
        }
    return -1;
}

float Eased(float b) { return b * b * (3 - 2 * b); }

// 0x647890's answer for this player: the game's offsets, or (in first person) theirs.
const float* __cdecl Offsets(unsigned char* player, const float* game) {
    if (!g_any || !game) return game;
    int p = IndexOf(player);
    if (p < 0 || g_view[p].blend <= 0) return game;
    View& v = g_view[p];
    float t = Eased(v.blend);
    memcpy(v.offsets, game, sizeof(v.offsets));
    float eye = v.height > 0 ? v.height : game[1] * kEyeHeight, ahead = v.forward >= 0 ? v.forward : game[1] * kEyeAhead;
    float side = v.side > -90 ? v.side : game[1] * kEyeSide;
    v.offsets[0] = game[0] + (side - game[0]) * t;
    v.offsets[1] = game[1] + (eye - game[1]) * t;
    v.offsets[2] = game[2] + (ahead - game[2]) * t;
    v.offsets[3] = game[3] * (1 - t);
    return v.offsets;
}

// (0x647890's callers count on edx and the SSE registers coming back as they went in: the game's own never touches them)
__declspec(naked) void OffsetsStub() {
    __asm {
        push edx
        push ecx
        push dword ptr [esp + 0xc]   // its argument
        call g_offsetsGame           // (ret 4)
        pop ecx
        sub esp, 0x80
        movdqu [esp], xmm0
        movdqu [esp + 0x10], xmm1
        movdqu [esp + 0x20], xmm2
        movdqu [esp + 0x30], xmm3
        movdqu [esp + 0x40], xmm4
        movdqu [esp + 0x50], xmm5
        movdqu [esp + 0x60], xmm6
        movdqu [esp + 0x70], xmm7
        push ecx
        push eax
        push ecx
        call Offsets
        add esp, 8
        pop ecx
        movdqu xmm0, [esp]
        movdqu xmm1, [esp + 0x10]
        movdqu xmm2, [esp + 0x20]
        movdqu xmm3, [esp + 0x30]
        movdqu xmm4, [esp + 0x40]
        movdqu xmm5, [esp + 0x50]
        movdqu xmm6, [esp + 0x60]
        movdqu xmm7, [esp + 0x70]
        add esp, 0x80
        pop edx
        ret 4
    }
}

// The player pressed the switch (this camera update).
bool Switched(unsigned char* player, View& v, float dt) {
    auto input = *reinterpret_cast<unsigned char**>(player + kInput);
    if (!input || *reinterpret_cast<uintptr_t*>(input) != kInputHuman) return false;
    auto device = *reinterpret_cast<unsigned char**>(input + kInputDevice);
    if (!device) return false;
    bool keyboard = *reinterpret_cast<int*>(device + kDeviceType) == kKeyboard;
    bool down = keyboard ? (device[kKeys + g_key] & 0x80) != 0 : (*reinterpret_cast<WORD*>(device + kPadButtons) & g_padButton) != 0;
    bool pressed = down && !v.down;
    v.down = down;
    if (dt > 0 && dt < 1) v.sinceTap += dt;
    if (!pressed) return false;
    if (keyboard) return true;
    bool second = v.sinceTap <= kDoubleTap;
    v.sinceTap = second ? 1e9f : 0;
    return second;
}

void __fastcall Hook_Update(unsigned char* camera, void*, int a, float dt) {
    unsigned char* player = camera - kCamera;
    int p = IndexOf(player);
    if (p < 0) return g_real(camera, a, dt);
    View& v = g_view[p];
    bool online = NetplayEnabled();
    if (v.player != player) {   // a new player there (a level started): their starting view, straight away
        v.player = player;
        auto input = *reinterpret_cast<unsigned char**>(player + kInput);
        bool human = input && *reinterpret_cast<uintptr_t*>(input) == kInputHuman;
        if (!online) v.on = human && g_default;   // (online: from their input - FirstPersonFromInput)
        v.blend = v.on ? 1.0f : 0.0f;
        v.down = false;
        v.sinceTap = 1e9f;
        if (v.on) g_any = 1;
    }
    if (Switched(player, v, dt)) {
        if (!online) {
            v.on = !v.on;
            if (v.on) g_any = 1;
            Log(L"FirstPerson: player %d switched to %s", p, v.on ? L"first person" : L"third person");
        } else if (p == NetplayLocalSlot()) {   // (mine: it goes out with my input, and every PC switches me at the same tick)
            g_mineOnline = !g_mineOnline;
            Log(L"FirstPerson: switching to %s", g_mineOnline ? L"first person" : L"third person");
        }
    }
    if (!g_any) return g_real(camera, a, dt);
    float step = dt > 0 && dt < 1 ? dt / kGlide : 0;
    v.blend = v.on ? (v.blend + step < 1 ? v.blend + step : 1) : (v.blend - step > 0 ? v.blend - step : 0);
    v.viewport = *reinterpret_cast<int*>(player + kViewport);
    auto settings = *reinterpret_cast<unsigned char**>(camera + kCamSettings);
    if (v.blend <= 0 || !settings) return g_real(camera, a, dt);
    // looking up and down further than the game's 45 degrees, while their camera updates (the settings are shared)
    float* pmin = reinterpret_cast<float*>(settings + kPitchMin);
    float* pmax = reinterpret_cast<float*>(settings + kPitchMax);
    float lo = *pmin, hi = *pmax, t = Eased(v.blend);
    *pmin = lo + (-v.pitch - lo) * t;
    *pmax = hi + (v.pitch - hi) * t;
    g_real(camera, a, dt);
    *pmin = lo;
    *pmax = hi;
}

// (research: which code reads an address - a read watchpoint on the game's thread only; the places found are kept in
// the handler and logged from the camera update)
DWORD g_gameThread = 0, g_watchAt = 0;
DWORD g_seen[32], g_seenCaller[32];
volatile LONG g_seenCount = 0, g_watchHits = 0;
LONG CALLBACK WatchReads(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    c->Dr6 = 0;
    if (InterlockedIncrement(&g_watchHits) > 5000) c->Dr7 &= ~3u;   // (enough)
    for (LONG i = 0; i < g_seenCount; ++i) if (g_seen[i] == c->Eip) return EXCEPTION_CONTINUE_EXECUTION;
    LONG n = g_seenCount;
    if (n < 32) {
        DWORD caller = 0;
        const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
        for (int i = 0; i < 64 && !caller; ++i) if (sp[i] >= 0x401000 && sp[i] < 0x9f0000) caller = sp[i];
        g_seen[n] = c->Eip;
        g_seenCaller[n] = caller;
        g_seenCount = n + 1;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
DWORD WINAPI SetWatch(LPVOID) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, g_gameThread);
    if (!h) return 0;
    SuspendThread(h);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(h, &c)) {
        c.Dr0 = g_watchAt;
        c.Dr7 = (c.Dr7 & ~0xf0003u) | 1u | (0xfu << 16);   // slot 0 on, reads and writes, 4 bytes
        SetThreadContext(h, &c);
    }
    ResumeThread(h);
    CloseHandle(h);
    return 0;
}
void __fastcall Hook_UpdateWatching(unsigned char* camera, void* edx, int a, float dt) {
    g_gameThread = GetCurrentThreadId();
    static int logged = 0;
    while (logged < g_seenCount) {
        int i = logged++;
        Log(L"FirstPerson: watch - %#lx read by code before %#lx (called from %#lx)", g_watchAt, g_seen[i], g_seenCaller[i]);
    }
    Hook_Update(camera, edx, a, dt);
}

}  // namespace

void InitFirstPerson() {
    g_default = SettingInt(L"Camera", L"FirstPerson", L"FGUYMOD_FIRSTPERSON", 0) != 0;
    g_mineOnline = g_default;
    wchar_t key[16];
    SettingString(L"Camera", L"FirstPersonKey", nullptr, L"V", key, 16);
    if (key[0] >= L'a' && key[0] <= L'z') key[0] -= 32;
    if ((key[0] >= L'A' && key[0] <= L'Z') || (key[0] >= L'0' && key[0] <= L'9')) g_key = key[0];
    auto slot = reinterpret_cast<void**>(kCamVtable) + kUpdateSlot;
    if (*slot != reinterpret_cast<void*>(0x588c40) || memcmp(reinterpret_cast<void*>(kOffsets), kOffsetsStart, 5) != 0) {
        Log(L"FirstPerson: the game's camera code isn't where it should be - off");
        return;
    }
    // the game's 0x647890, run from here: its first instruction, then a jump back after it
    auto tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return;
    memcpy(tramp, kOffsetsStart, 5);
    tramp[5] = 0xe9;
    *reinterpret_cast<int*>(tramp + 6) = static_cast<int>(kOffsets + 5 - reinterpret_cast<uintptr_t>(tramp + 10));
    FlushInstructionCache(GetCurrentProcess(), tramp, 16);
    g_offsetsGame = tramp;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(kOffsets), 5, PAGE_EXECUTE_READWRITE, &old);
    auto at = reinterpret_cast<unsigned char*>(kOffsets);
    at[0] = 0xe9;
    *reinterpret_cast<int*>(at + 1) = static_cast<int>(reinterpret_cast<uintptr_t>(&OffsetsStub) - (kOffsets + 5));
    VirtualProtect(reinterpret_cast<void*>(kOffsets), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    g_real = reinterpret_cast<Update_t>(*slot);
    *slot = reinterpret_cast<void*>(Hook_UpdateWatching);
    VirtualProtect(slot, sizeof(void*), old, &old);
}

bool FirstPersonAny() { return g_any != 0; }

// A player in first person who's emoting (motion.cpp - online it starts at different times on each PC, so it never
// touches the game's camera): their view is drawn from behind them while it plays - the place the game hands the
// view's camera (splitview.cpp, its +0x94: 3 x 4, the axes in the columns, the place last), moved back along the view.
struct Pulled { float base[12], out[12]; bool have; float pull; ULONGLONG at; };
Pulled g_pulled[4];
const float kPullBack = 3.5f, kPullUp = 0.6f, kPullTime = 0.3f;

void FirstPersonDrawnCamera(int view, float* m) {
    if (view < 0 || view >= 4) return;
    Pulled& d = g_pulled[view];
    if (!d.have || memcmp(m, d.out, sizeof(d.out)) != 0) memcpy(d.base, m, sizeof(d.base));   // (a new one: the game's)
    bool emoting = false;
    for (int p = 0; p < 8; ++p)
        if (g_view[p].viewport == view && g_view[p].blend > 0.5f) emoting = MotionPlaying(p);
    ULONGLONG now = GetTickCount64();
    float dt = d.at ? (now - d.at) / 1000.0f : 0;
    d.at = now;
    float step = dt > 0 && dt < 1 ? dt / kPullTime : 0;
    d.pull = emoting ? (d.pull + step < 1 ? d.pull + step : 1) : (d.pull - step > 0 ? d.pull - step : 0);
    if (d.pull <= 0) { d.have = false; return; }
    float t = Eased(d.pull);
    memcpy(d.out, d.base, sizeof(d.out));
    d.out[3] -= d.base[2] * kPullBack * t;   // (back along the view: its third axis)
    d.out[7] -= d.base[6] * kPullBack * t - kPullUp * t;
    d.out[11] -= d.base[10] * kPullBack * t;
    memcpy(m, d.out, sizeof(d.out));
    d.have = true;
}

// models.cpp: no ink lines for something at (x, y, z) - a player's own while their view is drawn from behind them (the
// game makes a character's lines for its camera's place, at their eyes: from behind they'd scribble; the lines are made
// once for every view, so a partner on the same screen sees them without theirs meanwhile)
bool FirstPersonNoLines(float x, float y, float z) {
    for (int p = 0; p < 8; ++p) {
        const View& v = g_view[p];
        if (v.viewport < 0 || v.viewport >= 4 || g_pulled[v.viewport].pull <= 0.05f || !v.player) continue;
        auto xf = *reinterpret_cast<unsigned char* const*>(v.player + 0x38);
        if (!xf || IsBadReadPtr(xf + 0x30, 12)) continue;
        const float* at = reinterpret_cast<const float*>(xf + 0x30);
        float dx = x - at[0], dy = y - at[1], dz = z - at[2];
        if (dx * dx + dz * dz < 2.25f && dy > -0.5f && dy < 3.0f) return true;   // (them and what they hold)
    }
    return false;
}

bool FirstPersonDefault() { return g_default; }
void FirstPersonSetDefault(bool on) {
    g_default = on;
    g_mineOnline = on;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    WritePrivateProfileStringW(L"Camera", L"FirstPerson", on ? L"1" : L"0", path);
}

// Online (onlinesettings.cpp): mine goes out with my input; every PC sets everyone's from theirs, at the same tick.
bool FirstPersonMine() { return g_mineOnline; }
void FirstPersonFromInput(int player, bool on) {
    if (player < 0 || player >= 8 || g_view[player].on == on) return;
    g_view[player].on = on;
    if (on) g_any = 1;
    Log(L"FirstPerson: player %d in %s", player, on ? L"first person" : L"third person");
}

// A draw of a player's own body, in their own view while they're (more than half way) in first person: 1 - drawn with
// a clip plane `near` ahead of the eyes (`plane`, in clip space), 2 - not drawn (research), 0 - neither.
// The plane comes from the draw's projection: its clip space z and w (c2, c3: z = A w + B for any point, w the depth
// ahead of the eye), so depth >= near is  w - near (z - A w) / B >= 0.
int FirstPersonOwnDraw(const float* c2, const float* c3, float* plane) {
    if (!g_any || !g_hideMode) return 0;
    int view = SplitViewDrawing();
    if (view < 0 || view >= 4 || g_pulled[view].pull > 0.05f) return 0;   // (drawn from behind: all of them)
    int p = CosmeticsDrawPlayer();
    if (p < 0 || p >= 8 || g_view[p].viewport != view || g_view[p].blend < 0.5f) return 0;
    if (g_hideMode == 2) return 2;
    int k = 0;   // (c2 = A c3 + (0, 0, 0, B): A from c3's largest of x, y, z)
    for (int i = 1; i < 3; ++i) if (std::fabs(c3[i]) > std::fabs(c3[k])) k = i;
    if (std::fabs(c3[k]) < 1e-6f) return 0;
    float A = c2[k] / c3[k], B = c2[3] - A * c3[3];
    for (int i = 0; i < 3; ++i) if (std::fabs(c2[i] - A * c3[i]) > 1e-3f * (std::fabs(c2[i]) + 1)) return 0;   // (not a projection)
    if (std::fabs(B) < 1e-9f) return 0;
    plane[0] = 0;
    plane[1] = 0;
    plane[2] = -g_near / B;
    plane[3] = 1 + g_near * A / B;
    return 1;
}

// Test pipe: fpv <player> <0|1> [height] [forward] [pitch limit]   /   fpv hide <0|1|2>   /   fpv watch <hex address>
void FirstPersonCommand(const char* args) {
    char buf[128];
    strncpy_s(buf, args ? args : "", _TRUNCATE);
    char* ctx = nullptr;
    char* p = strtok_s(buf, " \t", &ctx);
    char* on = strtok_s(nullptr, " \t", &ctx);
    if (!p || !on) return;
    if (_stricmp(p, "watch") == 0) {   // which code reads it (the game's thread)
        g_watchAt = strtoul(on, nullptr, 16);
        static bool handler = false;
        if (!handler) { AddVectoredExceptionHandler(1, WatchReads); handler = true; }
        if (HANDLE t = CreateThread(nullptr, 0, SetWatch, nullptr, 0, nullptr)) CloseHandle(t);
        Log(L"FirstPerson: watch - reads of %#lx on the game's thread", g_watchAt);
        return;
    }
    if (_stricmp(p, "hide") == 0) {   // fpv hide <0|1|2> [near]
        g_hideMode = atoi(on);
        if (char* s = strtok_s(nullptr, " \t", &ctx)) g_near = (float)atof(s);
        Log(L"FirstPerson: test - their own body in their view: %s (%.2f)", g_hideMode == 2 ? L"not drawn" : g_hideMode ? L"clipped near the eyes" : L"as it is", g_near);
        return;
    }
    int i = atoi(p);
    if (i < 0 || i >= 8) return;
    View& v = g_view[i];
    v.on = atoi(on) != 0;
    if (v.on) g_any = 1;
    if (char* s = strtok_s(nullptr, " \t", &ctx)) v.height = (float)atof(s);
    if (char* s = strtok_s(nullptr, " \t", &ctx)) v.forward = (float)atof(s);
    if (char* s = strtok_s(nullptr, " \t", &ctx)) v.pitch = (float)atof(s);
    if (char* s = strtok_s(nullptr, " \t", &ctx)) v.side = (float)atof(s);
    Log(L"FirstPerson: player %d %s (eyes %.2f up, %.2f ahead; pitch to %.2f)", i, v.on ? L"in first person" : L"in third person",
        v.height, v.forward, v.pitch);
}
