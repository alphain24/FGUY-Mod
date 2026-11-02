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
// In first person their body faces the way they look, the camera doesn't trail them, and in their own view what's near
// their eyes and above their neck isn't drawn (cosmetics.cpp asks), nor their ink lines when they'd show (models.cpp).
// Test pipe: "fpv <player> <0|1> [height] [forward] [pitch limit]", "fpv hide <0|1|2|3> [near] [below the eyes]
// [neck]", "fpv face <0|1>", "fpv fov <degrees>", "fpv watch <hex address>".
#include "common.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const uintptr_t kCamVtable = 0xa19c8c;       // zFGUYCamThirdPerson's vtable (its update, 0x588c40, in slot 8)
const int kUpdateSlot = 8;
const uintptr_t kOffsets = 0x647890;         // the player's camera offsets for a mode: cmp byte ptr [esp+4], 0 (5 bytes)
const unsigned char kOffsetsStart[5] = {0x80, 0x7c, 0x24, 0x04, 0x00};
const uintptr_t kPlayers = 0xbad988;         // the game's player list: -> 8 pointers, the count at +0x20
const int kCamera = 0xf148, kPlayerIndex = 0xdf8, kViewport = 0xdfc;
const int kCamSettings = 0x178, kPitchMin = 0x94, kPitchMax = 0x98;
const int kAssist = 0x334;                   // (the camera's aim assist, this frame: on)
// (the settings' field of view, +0x88: the game's 1.396 (80 degrees) is about as wide on a 16:9 screen - 77 degrees across)
const int kFov = 0x88;
// First person's field of view: SETTINGS > CAMERA's choices, in degrees (0: the game's). Online a player's goes with
// their input as its index + 1 (KbmRaw.opts bits 4..6; 0: not said - the game's)
const int kFovs[] = {70, 0, 90, 100, 110};
const int kFovCount = 5;
const float kGlide = 0.25f;                  // seconds from one view to the other
const uintptr_t kViewports = 0xb5a9b0, kViewportSize = 0x3c70;   // (the views: splitview.cpp)
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
bool g_crosshair = true;                     // [Camera] Crosshair: one in the middle of a view in first person
int g_fov = 0;                               // [Camera] FirstPersonFOV: degrees (0: the game's) - kFovs' choices
bool g_mineOnline = false;                   // online: my player in first person (my PC's - it goes with my input)

struct View {
    bool on = false;
    unsigned char* player = nullptr;                     // (the player object it was set up for)
    bool down = false;                                   // (the switch: held last time)
    float sinceTap = 1e9f;                               //   (seconds since the controller button's last press)
    float height = 0, forward = -99, pitch = 1.4f;     // (0 / -99: from the character's camera - kEyeHeight / kEyeAhead)
    float side = -99;                                    // (to their right; -99: kEyeSide)
    float blend = 0;                                     // 0 third person .. 1 first person
    int fov = 0;                                         // (their field of view in first person, degrees; 0 the game's)
    int viewport = -1;                                   // (their view)
    float offsets[16];                                   // (their camera offsets, 0x647890's)
    float eyeOff[3] = {0, 0, 0};                         // (their eye from their place, the lag taken out - EyeNow)
    float seen[2][3] = {};                               // (their place: the last two the game set, and when -
    LONGLONG seenAt[2] = {0, 0};                         //  EyeNow goes smoothly between them)
    bool haveEye = false;
};
View g_view[8];
volatile LONG g_any = 0;
int g_hideMode = 1;   // their own body in their view: 1 clipped near / above the eyes (research: 0 as it is, 2 not drawn,
                      //   3 clipped near the eyes only)
float g_near = 0.3f;  //   (how far ahead of the eyes it's clipped)
float g_above = 0.15f; //   (and how far below the eyes from above - a hat's brim dips with the head as they walk)
float g_neck = -0.15f; //   (or from a little under their head bone, when that's lower: their head lunges as they set
                       //    off - the bone is at the neck, their chin and mouth below it)
bool g_face = true;   // their body turned the way they look (research: "fpv face 0" not)
const int kCamYaw = 0xf3b4;   // (the player's camera's yaw, radians)
const int kOrbit = 0xf3a8;    // (the camera's orbit block: the point it orbits, x y z, ...; its eye at +0x18)

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
    float eye = v.height > 0 ? v.height : game[1] * kEyeHeight, ahead = v.forward > -90 ? v.forward : game[1] * kEyeAhead;
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

// Their eye now: their place and the eye's offset from the last camera update. The game moves them a few times a
// frame's worth less often than it draws (about 22 a second at 108 drawn): from their place as set, the view and their
// gun moved in steps. So their place goes from the last one set to the one before smoothly, a set's time behind.
bool EyeNow(View& v, float* eye) {
    auto xf = v.player && v.haveEye ? *reinterpret_cast<const float* const*>(v.player + 0x38) : nullptr;
    if (!xf) return false;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float* at = xf + 12;
    if (memcmp(at, v.seen[1], sizeof(v.seen[1])) != 0) {   // (a new place)
        float dx = at[0] - v.seen[1][0], dy = at[1] - v.seen[1][1], dz = at[2] - v.seen[1][2];
        bool jump = !v.seenAt[1] || dx * dx + dy * dy + dz * dz > 4;   // (a respawn, a teleport: straight there)
        memcpy(v.seen[0], jump ? at : v.seen[1], sizeof(v.seen[0]));
        v.seenAt[0] = jump ? now.QuadPart : v.seenAt[1];
        memcpy(v.seen[1], at, sizeof(v.seen[1]));
        v.seenAt[1] = now.QuadPart;
    }
    double gap = (double)(v.seenAt[1] - v.seenAt[0]) / freq.QuadPart, since = (double)(now.QuadPart - v.seenAt[1]) / freq.QuadPart;
    if (gap < 0.005) gap = 0.005;
    if (gap > 0.1) gap = 0.1;   // (standing still a while, then moving: no slow creep)
    float k = (float)(since / gap);
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    for (int i = 0; i < 3; ++i) eye[i] = v.seen[0][i] + (v.seen[1][i] - v.seen[0][i]) * k + v.eyeOff[i];
    return true;
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

// Their body turned the way they look (by `t` of the way): in third person a character faces where they last went, so
// standing and turning the view, it looked across their own shoulder and gun. Their heading ([+0x5c]+0x58, and +0x78
// the one it turns toward: 0..2 pi - the game builds their transforms from it each frame) set, and this frame's
// transforms (+0x38, +0x5c: rows right / up / forward / place) turned about up to match; their camera's yaw (+0xf3b4)
// looks along (-sin, -cos) on the ground. It's done as their camera updates: every PC runs every player's, from the
// same inputs.
void FaceView(unsigned char* player, float t) {
    auto first = *reinterpret_cast<float**>(player + 0x38);
    if (!first || IsBadWritePtr(first, 0x30)) return;
    float yaw = *reinterpret_cast<float*>(player + kCamYaw);
    float want = atan2f(-sinf(yaw), -cosf(yaw)), now = atan2f(first[8], first[10]);
    float by = want - now;
    while (by > 3.14159265f) by -= 6.2831853f;
    while (by < -3.14159265f) by += 6.2831853f;
    by *= t;
    if (std::fabs(by) < 1e-5f) return;
    auto heading = *reinterpret_cast<float**>(player + 0x5c);
    if (heading && !IsBadWritePtr(heading, 0x80)) {
        float h = now + by;
        while (h >= 6.2831853f) h -= 6.2831853f;
        while (h < 0) h += 6.2831853f;
        heading[0x58 / 4] = h;
        heading[0x78 / 4] = h;
    }
    float c = cosf(by), s = sinf(by);
    for (int at : {0x38, 0x5c}) {
        auto m = *reinterpret_cast<float**>(player + at);
        if (!m || IsBadWritePtr(m, 0x30)) continue;
        for (int row = 0; row < 3; ++row) {
            float* v = m + row * 4;
            float x = v[0], z = v[2];
            v[0] = x * c + z * s;
            v[2] = -x * s + z * c;
        }
    }
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
        v.haveEye = false;
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
    if (!online) v.fov = g_fov;   // (online: from their input - FirstPersonFovFromInput)
    if (!g_any) return g_real(camera, a, dt);
    float step = dt > 0 && dt < 1 ? dt / kGlide : 0;
    v.blend = v.on ? (v.blend + step < 1 ? v.blend + step : 1) : (v.blend - step > 0 ? v.blend - step : 0);
    v.viewport = *reinterpret_cast<int*>(player + kViewport);
    auto settings = *reinterpret_cast<unsigned char**>(camera + kCamSettings);
    if (v.blend <= 0 || !settings) return g_real(camera, a, dt);
    // looking up and down further than the game's 45 degrees, and their own field of view, while their camera updates
    // (the settings are shared)
    float* pmin = reinterpret_cast<float*>(settings + kPitchMin);
    float* pmax = reinterpret_cast<float*>(settings + kPitchMax);
    float* pfov = reinterpret_cast<float*>(settings + kFov);
    float lo = *pmin, hi = *pmax, fov = *pfov, t = Eased(v.blend);
    *pmin = lo + (-v.pitch - lo) * t;
    *pmax = hi + (v.pitch - hi) * t;
    if (v.fov > 0) *pfov = fov + (v.fov * 3.14159265f / 180 - fov) * t;
    // no aim assist in first person: the game hands the camera the point on the enemy nearest the aim and a strength
    // each frame (0x62c110 -> 0x5707a0: camera +0x334 on, +0x338 the point, +0x344 / +0x348 the strength), and its
    // update pulls the view that way - the crosshair stuck to enemies. Turned off just before it updates (every PC
    // runs every player's camera, and knows who's in first person).
    if (v.blend > 0.5f) camera[kAssist] = 0;
    g_real(camera, a, dt);
    // no lag across the ground: the game's camera trails the player as they set off (its own smoothed copy of the point
    // it orbits - 0.6 behind at a run), and their own head and shoulders ran into the view. The point it orbits and the
    // eye moved to where it settles standing - their place, our offsets along the view (side, ahead); the height keeps
    // the game's smoothing (a jump). The eye kept as an offset from their place: the view is drawn - and their gun
    // held - from their place as it is when it's drawn (the game draws more often than it updates the camera: from
    // this update's eye, the view and gun stepped as they moved).
    float* orbit = reinterpret_cast<float*>(player + kOrbit);
    if (auto xf = *reinterpret_cast<const float* const*>(player + 0x38)) {
        float yaw = *reinterpret_cast<float*>(player + kCamYaw), fx = -sinf(yaw), fz = -cosf(yaw);
        float side = v.offsets[0], ahead = v.offsets[2];
        float dx = (xf[12] + fx * ahead - fz * side - orbit[0]) * t, dz = (xf[14] + fz * ahead + fx * side - orbit[2]) * t;
        orbit[0] += dx;
        orbit[2] += dz;
        orbit[6] += dx;
        orbit[8] += dz;
        for (int i = 0; i < 3; ++i) v.eyeOff[i] = orbit[6 + i] - xf[12 + i];
        v.haveEye = true;
    }
    *pmin = lo;
    *pmax = hi;
    *pfov = fov;
    if (g_face) FaceView(player, t);
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
    g_crosshair = SettingInt(L"Camera", L"Crosshair", nullptr, 1) != 0;
    int fov = SettingInt(L"Camera", L"FirstPersonFOV", nullptr, 0);
    for (int f : kFovs) if (f == fov) g_fov = f;
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
    // their view drawn from their eye, the lag taken out (Hook_Update): the drawn camera is the game's own smoothed
    // copy - at a run 1.3 behind the eye, their own shoulders and back in their view
    for (int p = 0; p < 8; ++p) {
        View& v = g_view[p];
        float eye[3];
        if (v.viewport != view || v.blend <= 0 || !EyeNow(v, eye)) continue;
        float dx = eye[0] - m[3], dy = eye[1] - m[7], dz = eye[2] - m[11], t = Eased(v.blend);
        if (dx * dx + dy * dy + dz * dz > 9) continue;   // (not this view's camera: another pass's)
        m[3] += dx * t;
        m[7] += dy * t;
        m[11] += dz * t;
    }
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

// A player in first person whose own ink lines would show in their view: their head as far ahead as half way to their
// eye, along the view (setting off at a run it lunges past the eye, and the lines of their face - not clipped with
// their body, cosmetics.cpp: the game draws them apart - hung in front of them). With one view on the screen (online,
// alone), whenever they're in first person.
bool OwnLinesShow(int p, const View& v) {
    if (v.blend < 0.5f || p >= 4) return false;
    int shown = 0;
    float r[4];
    for (int i = 0; i < 4; ++i) shown += SplitViewRect(i, r) ? 1 : 0;
    if (shown <= 1) return true;
    float head[3];
    if (!MotionHead(p, head)) return false;
    const float* eye = reinterpret_cast<const float*>(v.player + kOrbit) + 6;
    float yaw = *reinterpret_cast<const float*>(v.player + kCamYaw), fx = -sinf(yaw), fz = -cosf(yaw);
    return (head[0] - eye[0]) * fx + (head[2] - eye[2]) * fz > -0.5f * v.offsets[2];
}

// models.cpp: no ink lines for something at (x, y, z) - a player's own while their view is drawn from behind them (the
// game makes a character's lines for its camera's place, at their eyes: from behind they'd scribble), or while they'd
// show in front of their eyes in first person (OwnLinesShow). The lines are made once for every view, so a partner on
// the same screen sees them without theirs meanwhile.
bool FirstPersonNoLines(float x, float y, float z) {
    for (int p = 0; p < 8; ++p) {
        const View& v = g_view[p];
        if (v.viewport < 0 || v.viewport >= 4 || !v.player) continue;
        if (g_pulled[v.viewport].pull <= 0.05f && !OwnLinesShow(p, v)) continue;
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

// SETTINGS > CAMERA's field of view: which of kFovs (cycled by `by`), its name
int FirstPersonFovIndex() {
    for (int i = 0; i < kFovCount; ++i) if (kFovs[i] == g_fov) return i;
    return 1;
}
const char* FirstPersonFovName() {
    static char name[24];
    if (g_fov) sprintf_s(name, "%d DEGREES", g_fov);
    else strcpy_s(name, "THE GAME'S (80)");
    return name;
}
void FirstPersonFovStep(int by) {
    g_fov = kFovs[(FirstPersonFovIndex() + (by < 0 ? -1 : 1) + kFovCount) % kFovCount];
    wchar_t path[MAX_PATH], val[8];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    swprintf_s(val, L"%d", g_fov);
    WritePrivateProfileStringW(L"Camera", L"FirstPersonFOV", val, path);
}
// online: mine as it goes with my input (its index + 1), and a player's from theirs (every PC, the same tick)
int FirstPersonFovMine() { return FirstPersonFovIndex() + 1; }
void FirstPersonFovFromInput(int player, int code) {
    if (player >= 0 && player < 8) g_view[player].fov = code >= 1 && code <= kFovCount ? kFovs[code - 1] : 0;
}

// motion.cpp: a player whose gun is brought into their view - in first person (by how far), their view on this
// screen and drawn from their eyes (not emoting), alive: their eye and which way they look (from the eye to the
// point the camera orbits - its pitch too), in the world; and whether they're standing still
bool FirstPersonArms(int slot, float* eye, float* forward, float* weight, bool* still, float* tans) {
    if (!g_any || slot < 0 || slot >= 4) return false;
    View& v = g_view[slot];
    float r[4];
    if (!v.on || v.blend <= 0 || !v.player || IndexOf(v.player) != slot || v.viewport < 0 || v.viewport >= 4) return false;
    if (g_pulled[v.viewport].pull > 0 || MotionPlaying(slot) || !SplitViewRect(v.viewport, r)) return false;
    if (NetplayPlayerHealth(v.player) <= 0) return false;
    const float* orbit = reinterpret_cast<const float*>(v.player + kOrbit);
    float f[3] = {orbit[0] - orbit[6], orbit[1] - orbit[7], orbit[2] - orbit[8]};
    float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len < 0.02f) return false;
    if (!EyeNow(v, eye)) return false;
    for (int i = 0; i < 3; ++i) forward[i] = f[i] / len;
    *weight = Eased(v.blend);
    *still = *reinterpret_cast<const int*>(v.player + 0x1a34) == 0;   // (its move state: 0 standing, 2 walking)
    // the view's projection (its viewport +0x164: 1 / tan across - negative - and up the diagonal)
    const float* proj = reinterpret_cast<const float*>(kViewports + v.viewport * kViewportSize + 0x164);
    tans[0] = 0.79f;
    tans[1] = 0.45f;
    if (!IsBadReadPtr(proj, 24) && std::fabs(proj[0]) > 0.1f && std::fabs(proj[5]) > 0.1f) {
        tans[0] = 1 / std::fabs(proj[0]);
        tans[1] = 1 / std::fabs(proj[5]);
    }
    return true;
}

bool FirstPersonCrosshair() { return g_crosshair; }
void FirstPersonSetCrosshair(bool on) {
    g_crosshair = on;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    WritePrivateProfileStringW(L"Camera", L"Crosshair", on ? L"1" : L"0", path);
}

// The HUD's crosshairs (pause.as2js FGUYCrosshairPoll): "x,y;..." - the middle of each view whose player is looking
// through their eyes (all the way in first person, standing, their view not drawn from behind), in 1/10000 of the
// screen. Shots go through a view's middle (0x6568c0), so that's where they land; "" none.
const char* FirstPersonCrosshairs() {
    static std::string s;
    s.clear();
    if (!g_any || !g_crosshair) return s.c_str();
    for (int p = 0; p < 8; ++p) {
        const View& v = g_view[p];
        if (!v.on || v.blend < 1 || v.viewport < 0 || v.viewport >= 4 || g_pulled[v.viewport].pull > 0) continue;
        if (!v.player || IndexOf(v.player) != p || NetplayPlayerHealth(v.player) <= 0) continue;   // (IndexOf: still in the level)
        float r[4];
        if (!SplitViewRect(v.viewport, r)) continue;
        char at[32];
        sprintf_s(at, "%d,%d;", (int)((r[0] + r[2]) * 5000), (int)((r[1] + r[3]) * 5000));
        s += at;
    }
    return s.c_str();
}

// Online (onlinesettings.cpp): mine goes out with my input; every PC sets everyone's from theirs, at the same tick.
bool FirstPersonMine() { return g_mineOnline; }
void FirstPersonFromInput(int player, bool on) {
    if (player < 0 || player >= 8 || g_view[player].on == on) return;
    g_view[player].on = on;
    if (on) g_any = 1;
    Log(L"FirstPerson: player %d in %s", player, on ? L"first person" : L"third person");
}

// Solves the 4x4 system m x = b (Gaussian elimination, partial pivoting); false if it has no one answer.
bool Solve4(float m[4][4], float b[4], float* x) {
    for (int c = 0; c < 4; ++c) {
        int r = c;
        for (int i = c + 1; i < 4; ++i) if (std::fabs(m[i][c]) > std::fabs(m[r][c])) r = i;
        if (std::fabs(m[r][c]) < 1e-9f) return false;
        for (int j = 0; j < 4; ++j) { float t = m[c][j]; m[c][j] = m[r][j]; m[r][j] = t; }
        float t = b[c]; b[c] = b[r]; b[r] = t;
        for (int i = c + 1; i < 4; ++i) {
            float f = m[i][c] / m[c][c];
            for (int j = c; j < 4; ++j) m[i][j] -= f * m[c][j];
            b[i] -= f * b[c];
        }
    }
    for (int i = 3; i >= 0; --i) {
        float s = b[i];
        for (int j = i + 1; j < 4; ++j) s -= m[i][j] * x[j];
        x[i] = s / m[i][i];
    }
    return true;
}

// A draw of a player's own body, in their own view while they're (more than half way) in first person: drawn with
// clip planes (`planes`, in clip space - how many returned), or -1 not drawn (research), 0 neither. `c` is the draw's
// vertex shader constants: c0..c3 its point's place on the screen (clip space) - a character's points are in the world
// already, skinned on the CPU (c4..c6, near their place, don't move them).
// 1. What's within `near` ahead of the eyes. From the clip space z and w (c2, c3: z = A w + B for any point, w the depth
//    ahead of the eye), depth >= near is  w - near (z - A w) / B >= 0.
// 2. What's above their neck: their head, and a hat (a brim held out past the first, seen from beneath - a skin's
//    model, its outline black). The height: their head bone's (motion.cpp - it goes down and ahead with their head as
//    they set off at a run), or g_above below the eye, the lower; the eye is where clip space x, y and w are all 0. The
//    plane, world height <= that, taken through c0..c3 to clip space.
int FirstPersonOwnDraw(const float (*c)[4], float* planes) {
    if (!g_any || !g_hideMode) return 0;
    int view = SplitViewDrawing();
    if (view < 0 || view >= 4 || g_pulled[view].pull > 0.05f) return 0;   // (drawn from behind: all of them)
    int p = CosmeticsDrawPlayer();
    if (p < 0 || p >= 8 || g_view[p].viewport != view || g_view[p].blend < 0.5f) return 0;
    if (g_hideMode == 2) return -1;
    const float* c2 = c[2];
    const float* c3 = c[3];
    int k = 0;   // (c2 = A c3 + (0, 0, 0, B): A from c3's largest of x, y, z)
    for (int i = 1; i < 3; ++i) if (std::fabs(c3[i]) > std::fabs(c3[k])) k = i;
    if (std::fabs(c3[k]) < 1e-6f) return 0;
    float A = c2[k] / c3[k], B = c2[3] - A * c3[3];
    for (int i = 0; i < 3; ++i) if (std::fabs(c2[i] - A * c3[i]) > 1e-3f * (std::fabs(c2[i]) + 1)) return 0;   // (not a projection)
    if (std::fabs(B) < 1e-9f) return 0;
    planes[0] = 0;
    planes[1] = 0;
    planes[2] = -g_near / B;
    planes[3] = 1 + g_near * A / B;
    if (g_hideMode != 1 || MotionArmsInView(p)) return 1;   // (their head shrunk away: their gun's up there - motion.cpp)
    // the eye in the draw's space: c0, c1, c3 . (x, y, z, 1) = 0 (the fourth row: z = 0 for the system's sake, replaced by
    // the eye's own 1 after)
    float m[4][4], b[4], eye[4];
    const int rows[3] = {0, 1, 3};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i][j] = c[rows[i]][j];
        m[i][3] = 0;
        b[i] = -c[rows[i]][3];
    }
    m[3][0] = m[3][1] = m[3][2] = 0;
    m[3][3] = 1;
    b[3] = 0;
    if (!Solve4(m, b, eye)) return 1;
    float cut = eye[1] - g_above, head[3];
    if (MotionHead(p, head) && head[1] + g_neck < cut) cut = head[1] + g_neck;
    // kept: cut - height >= 0, a = (0, -1, 0, cut) in the draw's space; in clip space q with q . (M x) = a . x for
    // every x, so M^T q = a
    float a[4] = {0, -1, 0, cut};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) m[i][j] = c[j][i];
    return Solve4(m, a, planes + 4) ? 2 : 1;
}

// Test pipe: fpv <player> <0|1> [height] [forward] [pitch limit]   /   fpv hide <0|1|2|3> [near] [below the eyes] [neck]
// /   fpv face <0|1>   /   fpv fov <degrees>   /   fpv watch <hex address>
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
    if (_stricmp(p, "arms") == 0) {   // fpv arms <right> <down> <ahead> <0|1> - the gun hand's place (motion.cpp)
        float v[3] = {(float)atof(on), 0.32f, 0.42f};
        char* s = strtok_s(nullptr, " \t", &ctx);
        if (s) v[1] = (float)atof(s);
        if ((s = strtok_s(nullptr, " \t", &ctx)) != nullptr) v[2] = (float)atof(s);
        s = strtok_s(nullptr, " \t", &ctx);
        MotionArmsTest(v[0], v[1], v[2], !s || atoi(s) != 0);
        return;
    }
    if (_stricmp(p, "armsturn") == 0) {   // fpv armsturn <pitch> <yaw> <roll> - the gun hand turned after (degrees)
        float v[3] = {(float)atof(on), 0, 0};
        for (int i = 1; i < 3; ++i) if (char* s = strtok_s(nullptr, " \t", &ctx)) v[i] = (float)atof(s);
        MotionArmsTurnTest(v[0], v[1], v[2]);
        return;
    }
    if (_stricmp(p, "face") == 0) {   // fpv face <0|1> - their body turned the way they look
        g_face = atoi(on) != 0;
        Log(L"FirstPerson: test - body turned the way they look: %d", g_face);
        return;
    }
    if (_stricmp(p, "fov") == 0) {   // fpv fov <degrees> (0: the game's) - this PC's, not kept
        g_fov = atoi(on);
        Log(L"FirstPerson: test - field of view %d", g_fov);
        return;
    }
    if (_stricmp(p, "hide") == 0) {   // fpv hide <0|1|2|3> [near] [below the eyes] [neck]
        g_hideMode = atoi(on);
        if (char* s = strtok_s(nullptr, " \t", &ctx)) g_near = (float)atof(s);
        if (char* s = strtok_s(nullptr, " \t", &ctx)) g_above = (float)atof(s);
        if (char* s = strtok_s(nullptr, " \t", &ctx)) g_neck = (float)atof(s);
        Log(L"FirstPerson: test - their own body in their view: %s (%.2f, %.2f)", g_hideMode == 2 ? L"not drawn" :
            g_hideMode == 3 ? L"clipped near the eyes" : g_hideMode ? L"clipped near / above the eyes" : L"as it is", g_near, g_above);
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
