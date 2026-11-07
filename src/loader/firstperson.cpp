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
#include <vector>

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
// the field of view's slider: kFovMin..kFovMax degrees by kFovStep (kFovGame: the game's own, kept as 0). Online each
// value has a code in the player's input: its place here + 1 (0: not said) - the first five are the old choices' codes.
const int kFovMin = 60, kFovMax = 120, kFovStep = 5, kFovGame = 80;
const int kFovs[] = {70, 0, 90, 100, 110, 60, 65, 75, 85, 95, 105, 115, 120};
const int kFovCount = 13;
const float kGlide = 0.25f;                  // seconds from one view to the other
const float kZoomed = 0.2f;                  // (a weapon's zoom: the view this much narrower is "all the way")
const uintptr_t kViewports = 0xb5a9b0, kViewportSize = 0x3c70;   // (the views: splitview.cpp)
// the eye, of the character's camera height (Peter's 2.2: 1.76 up, 0.22 ahead) - over the middle of their body: off to a
// side it came out past their collision at a wall (their gun is put in the view by motion.cpp, wherever the game holds it)
const float kEyeHeight = 0.8f, kEyeAhead = 0.1f, kEyeSide = 0;

using Update_t = void(__thiscall*)(void* camera, int a, float dt);
Update_t g_real = nullptr;
void* g_offsetsGame = nullptr;               // (the game's 0x647890: its first instruction, then a jump back)

// The switch: a key (V) for keyboard players, the Back button twice within kDoubleTap for controller players - read from
// the device the game plays the player from (online: their inputs, the same on every PC), as their camera updates.
const uintptr_t kInputHuman = 0xa1a29c;      // zPlayerInputHuman's vtable (the player's input: player+0x101c)
const int kInput = 0x101c, kInputDevice = 0x30, kDeviceType = 8, kPadButtons = 0x14, kKeys = 0x484;
bool g_shotLog = false;   // ([Debug] ShotLog: below)
const int kKeyboard = 8;                     // (device types: 8 keyboard / mouse, 4 a controller)
const WORD kBack = 0x0002;                   // (the game's controller bits: Back 2 - the scoreboard)
const float kDoubleTap = 0.4f;
int g_key = 'V';
WORD g_padButton = kBack;
bool g_default = false;                      // [Camera] FirstPerson: players start in first person
bool g_crosshair = true;                     // [Camera] Crosshair: one in the middle of a view in first person
int g_fov = 0;                               // [Camera] FirstPersonFOV: degrees (0: the game's) - the slider's
bool g_mineOnline = false;                   // online: my player in first person (my PC's - it goes with my input)

struct View {
    bool on = false;
    unsigned char* player = nullptr;                     // (the player object it was set up for)
    bool down = false;                                   // (the switch: held last time)
    float sinceTap = 1e9f;                               //   (seconds since the controller button's last press)
    float height = 0, forward = -99, pitch = 1.4f;     // (0 / -99: from the character's camera - kEyeHeight / kEyeAhead)
    float side = -99;                                    // (to their right; -99: kEyeSide)
    bool reloading = false;                              // (the game's upper body: FirstPersonArms)
    float blend = 0;                                     // 0 third person .. 1 first person
    int fov = 0;                                         // (their field of view in first person, degrees; 0 the game's)
    float wideAs = 0;                                    // (their view's width not aiming, tan across - FirstPersonArms)
    int viewport = -1;                                   // (their view)
    float offsets[16];                                   // (their camera offsets, 0x647890's)
    float eyeOff[3] = {0, 0, 0};                         // (their eye from their place, the lag taken out - EyeNow)
    float seen[2][3] = {};                               // (their place: the last two the game set, and when -
    LONGLONG seenAt[2] = {0, 0};                         //  EyeNow goes smoothly between them)
    bool haveEye = false;
    float drawnEye[3] = {}, drawnFw[3] = {};             // (the frame being drawn: its camera's eye and which way it
    bool haveDrawn = false;                              //  looks - FirstPersonViewmodel)
    float frameArms[26] = {};                            // (their arms' pose as this frame's first draw of them found
    bool haveFrameArms = false;                          //  it - the same for all its draws: FrameArms)
};
View g_view[8];
volatile LONG g_any = 0;
bool g_armsFollow = true;   // (research: "fpv armsfollow <0|1>" - the arms moved with the camera since posed: FollowX)
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
    // (the game's by the slider's share of its own: zooming the game narrows this very value - set outright, zoom did nothing at any
    // field of view but the game's)
    float ours = v.fov > 0 ? fov * (1 + ((float)v.fov / kFovGame - 1) * t) : fov;
    *pfov = ours;
    // no aim assist in first person: the game hands the camera the point on the enemy nearest the aim and a strength
    // each frame (0x62c110 -> 0x5707a0: camera +0x334 on, +0x338 the point, +0x344 / +0x348 the strength), and its
    // update pulls the view that way - the crosshair stuck to enemies. Turned off just before it updates (every PC
    // runs every player's camera, and knows who's in first person).
    if (v.blend > 0.5f) camera[kAssist] = 0;
    g_real(camera, a, dt);
    // no lag across the ground: the game's camera trails the player as they set off (its own smoothed copy of the point
    // it orbits - 0.6 behind at a run), and their own head and shoulders ran into the view. The point it orbits and the
    // eye moved to where it settles standing - their place, our offsets along the view (side, ahead) and up (the game's
    // height smoothing too: jumping, their body rose ahead of the eye into the view). The eye kept as an offset from
    // their place: the view is drawn - and their gun
    // held - from their place as it is when it's drawn (the game draws more often than it updates the camera: from
    // this update's eye, the view and gun stepped as they moved).
    float* orbit = reinterpret_cast<float*>(player + kOrbit);
    if (auto xf = *reinterpret_cast<const float* const*>(player + 0x38)) {
        float yaw = *reinterpret_cast<float*>(player + kCamYaw), fx = -sinf(yaw), fz = -cosf(yaw);
        float side = v.offsets[0], ahead = v.offsets[2];
        float dx = (xf[12] + fx * ahead - fz * side - orbit[0]) * t, dz = (xf[14] + fz * ahead + fx * side - orbit[2]) * t;
        float dy = (xf[13] + v.offsets[1] - orbit[1]) * t;
        orbit[0] += dx;
        orbit[1] += dy;
        orbit[2] += dz;
        orbit[6] += dx;
        orbit[7] += dy;
        orbit[8] += dz;
        for (int i = 0; i < 3; ++i) v.eyeOff[i] = orbit[6 + i] - xf[12 + i];
        v.haveEye = true;
    }
    *pmin = lo;
    *pmax = hi;
    *pfov = fov + (*pfov - ours);   // (the game's own change in the update kept - a weapon's zoom easing in and out:
                                    //  put back as it was, it never finished zooming out)
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
    g_shotLog = SettingInt(L"Debug", L"ShotLog", nullptr, 0) != 0;
    MotionShotLog(g_shotLog);
    MotionArmMoves(SettingInt(L"Camera", L"FirstPersonMoves", L"FGUYMOD_FPV_MOVES", 0), 0, false);
    MotionArmSize(SettingInt(L"Camera", L"FirstPersonArmSize", L"FGUYMOD_FPV_ARMSIZE", 100) / 100.0f);
    MotionArmFlat(SettingInt(L"Camera", L"FirstPersonArmFlat", L"FGUYMOD_FPV_ARMFLAT", 100) / 100.0f);
    int fov = SettingInt(L"Camera", L"FirstPersonFOV", nullptr, 0);
    if (fov) {   // (to the nearest place on the slider)
        fov = (fov + kFovStep / 2) / kFovStep * kFovStep;
        fov = fov < kFovMin ? kFovMin : fov > kFovMax ? kFovMax : fov;
        g_fov = fov == kFovGame ? 0 : fov;
    }
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
// The game's taunt (T, D-pad down; action 48): the gun put away, a routine of the whole body for 7 s - firing or moving cuts it short, the
// gun back by itself. Nothing an arm's view can show: their view goes behind them for it, as for an emote. [player+0x22e0]: 1 while it
// plays, 0 walking, firing, in a punch. (While it played the arms were held as for the idle fidget, whose layer it uses: a hand frozen
// in the air, no gun.)
bool Taunting(const View& v) { return v.player && *reinterpret_cast<const DWORD*>(v.player + 0x22e0) != 0; }
// [Debug] ShotLog=1 (test pipe: "fpv shotwatch 1"): player 1's left and right mouse buttons as the game reads them, and each round
// fired - which clicks fired nothing, first person or not
void ShotLog() {
    unsigned char* pls[8];
    if (GamePlayers(pls, 8) <= 0) return;
    static int last = -1, type = -1;
    static LONG buttons = 0;
    auto input = *reinterpret_cast<unsigned char**>(pls[0] + kInput);
    auto device = input && *reinterpret_cast<uintptr_t*>(input) == kInputHuman ? *reinterpret_cast<unsigned char**>(input + kInputDevice) : nullptr;
    int now = device ? *reinterpret_cast<int*>(device + kDeviceType) : 0;
    if (now != type) Log(L"Shot: player 1's controls are device type %d (8 keyboard / mouse, 4 a controller), keyboard mode %d", now, *reinterpret_cast<const unsigned char*>(0xacc4cd));
    type = now;
    auto layer = *reinterpret_cast<const unsigned char* const*>(pls[0] + 0x13cc);
    float aim = layer ? *reinterpret_cast<const float*>(layer + 0x50) : 0;
    int clip = layer ? *reinterpret_cast<const int*>(layer + 0x60) : -1, move = *reinterpret_cast<const int*>(pls[0] + 0x1a34);
    LONG held = *reinterpret_cast<const volatile LONG*>(0xaefae8);   // (the raw mouse poll's buttons - not in the key table)
    if (held != buttons) Log(L"Shot: mouse buttons %lx (aim %.2f clip %d move %d)", held, aim, clip, move);
    buttons = held;
    int rounds = *reinterpret_cast<const int*>(pls[0] + 0x4ffc);
    if (last >= 0 && rounds < last) Log(L"Shot: %d left in the clip (aim %.2f clip %d move %d)", rounds, aim, clip, move);
    last = rounds;
}
struct Pulled { float base[12], out[12]; bool have; float pull; ULONGLONG at; };
Pulled g_pulled[4];
const float kPullBack = 3.5f, kPullUp = 0.6f, kPullTime = 0.3f;

double OrderNow() {   // (research, "fpv order": the PC's clock, seconds)
    LARGE_INTEGER n, f;
    QueryPerformanceCounter(&n);
    QueryPerformanceFrequency(&f);
    return (double)n.QuadPart / f.QuadPart;
}

// (research: "fpv vmcount 1" - how many pieces of the gun each frame drew as the arms, left out, or couldn't place: a line
// whenever that changes from the frame before. A piece gone for a frame is a flicker)
bool g_vmCount = false;
int g_vmParts = 0, g_vmOutlines = 0, g_vmLeft = 0, g_vmNoArms = 0, g_vmFrames = 0;
int g_vmLast[4] = {-1, -1, -1, -1};
float g_vmLeftD = 0, g_vmLeftBig = 0, g_vmLeftS = 0;
float g_vmPiece[8][4];   // (this frame's pieces: how far from the gun hand, and how far in the view - ahead, right, up - from the eye)
int g_vmPieces = 0;
int g_vmWhy = 0;   // (research: why a draw of theirs wasn't taken as the arms)
int g_vmSkin[3] = {}, g_vmSkinLast = -1;   // (a frame's skinned draws: all, a player's, drawn as the arms)
void VmCountFrame() {
    ++g_vmFrames;
    if (g_vmSkin[1] > 0 && g_vmSkin[2] == 0) Log(L"FirstPerson: frame %d: a player's %d draws, none as the arms - last why %d", g_vmFrames, g_vmSkin[1], g_vmWhy);
    if (g_vmSkin[2] != g_vmSkinLast && (g_vmSkin[2] == 0 || g_vmSkinLast == 0)) {
        float h[26] = {};
        bool got = MotionViewmodel(0, h);
        Log(L"FirstPerson: skinned frame %d: all %d, a player's %d, as the arms %d (arms %d; hand %.2f %.2f %.2f eye %.2f %.2f %.2f s %.3f)", g_vmFrames, g_vmSkin[0],
            g_vmSkin[1], g_vmSkin[2], got ? 1 : 0, h[0], h[1], h[2], h[4], h[5], h[6], h[3]);
    }
    g_vmSkinLast = g_vmSkin[2];
    g_vmSkin[0] = g_vmSkin[1] = g_vmSkin[2] = 0;
    int now[4] = {g_vmParts, g_vmOutlines, g_vmLeft, g_vmNoArms};
    if (memcmp(now, g_vmLast, sizeof(now)) != 0) {
        Log(L"FirstPerson: vmcount frame %d: parts %d outlines %d left out %d (d %.3f big %.4f s %.3f) no arms %d", g_vmFrames, now[0],
            now[1], now[2], g_vmLeftD, g_vmLeftBig, g_vmLeftS, now[3]);
        for (int i = 0; i < g_vmPieces; ++i)
            Log(L"FirstPerson: vmpiece %d: %.3f from the hand; ahead %.3f right %.3f up %.3f", i, g_vmPiece[i][0], g_vmPiece[i][1], g_vmPiece[i][2], g_vmPiece[i][3]);
        memcpy(g_vmLast, now, sizeof(now));
    }
    g_vmParts = g_vmOutlines = g_vmLeft = g_vmNoArms = g_vmPieces = 0;
}
void FirstPersonFrameEnd() { if (g_vmCount) VmCountFrame(); }   // (d3d9.cpp: a frame's been drawn)
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
        if (p < 4) { float fw[3] = {m[2], m[6], m[10]}; MotionArmsTrace(p, eye, fw); }
        v.drawnEye[0] = m[3], v.drawnEye[1] = m[7], v.drawnEye[2] = m[11];
        v.drawnFw[0] = m[2], v.drawnFw[1] = m[6], v.drawnFw[2] = m[10];   // (its third axis: ahead)
        v.haveDrawn = true;
        v.haveFrameArms = false;
        if (MotionOrderTrace() > 0)
            Log(L"Order: %.5f camera (thread %lu) - eye %.4f %.4f %.4f", OrderNow(), GetCurrentThreadId(), m[3], m[7], m[11]);
    }
    Pulled& d = g_pulled[view];
    if (!d.have || memcmp(m, d.out, sizeof(d.out)) != 0) memcpy(d.base, m, sizeof(d.base));   // (a new one: the game's)
    if (g_shotLog) ShotLog();
    bool emoting = false;
    for (int p = 0; p < 8; ++p)
        if (g_view[p].viewport == view && g_view[p].blend > 0.5f) emoting = MotionPlaying(p) || Taunting(g_view[p]);
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

// SETTINGS > CAMERA's field of view: the slider's row ("<percent along>|<degrees>"), a step along it
const char* FirstPersonFovSlider() {
    static char row[16];
    int degrees = g_fov ? g_fov : kFovGame;
    sprintf_s(row, "%d|%d", (degrees - kFovMin) * 100 / (kFovMax - kFovMin), degrees);
    return row;
}
void FirstPersonFovStep(int by) {
    int degrees = (g_fov ? g_fov : kFovGame) + (by < 0 ? -kFovStep : kFovStep);
    degrees = degrees < kFovMin ? kFovMin : degrees > kFovMax ? kFovMax : degrees;
    g_fov = degrees == kFovGame ? 0 : degrees;
    wchar_t path[MAX_PATH], val[8];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    swprintf_s(val, L"%d", g_fov);
    WritePrivateProfileStringW(L"Camera", L"FirstPersonFOV", val, path);
}
// online: mine as it goes with my input (its code), and a player's from theirs (every PC, the same tick)
int FirstPersonFovMine() {
    for (int i = 0; i < kFovCount; ++i) if (kFovs[i] == g_fov) return i + 1;
    return 2;   // (research's "fpv fov": off the slider - the game's)
}
void FirstPersonFovFromInput(int player, int code) {
    if (player < 0 || player >= 8) return;
    int fov = code >= 1 && code <= kFovCount ? kFovs[code - 1] : 0;
    if (fov != g_view[player].fov) Log(L"FirstPerson: player %d's field of view is %d (0: the game's)", player, fov);
    g_view[player].fov = fov;
}

// motion.cpp: a player whose gun is brought into their view - in first person (by how far), their view on this
// screen and drawn from their eyes (not emoting), alive: their eye and which way they look (from the eye to the
// point the camera orbits - its pitch too), in the world; whether they're standing still, their view's width, their
// weapon's zoom and the game's upper-body stance
int g_armsNoWhy = 0;   // (research: the last reason FirstPersonArms said no - "fpv armswhy" logs it)
int g_layerDump = 0;   // (research: "fpv layer <n>" - the upper body's blend block, the next n times)
bool g_armsRaw = false;   // (research: "fpv armsraw 1" - their arms as the game has them, none of ours: the truth to match)
bool FirstPersonArms(int slot, ArmsView* out) {
    g_armsNoWhy = 1;
    if (!g_any || slot < 0 || slot >= 4 || g_armsRaw) return false;
    View& v = g_view[slot];
    float r[4];
    g_armsNoWhy = 2;
    if (!v.on || v.blend <= 0 || !v.player || IndexOf(v.player) != slot || v.viewport < 0 || v.viewport >= 4) return false;
    g_armsNoWhy = g_pulled[v.viewport].pull > 0 ? 3 : (MotionPlaying(slot) ? 4 : 5);
    if (g_pulled[v.viewport].pull > 0 || MotionPlaying(slot) || !SplitViewRect(v.viewport, r)) return false;
    g_armsNoWhy = 6;
    if (NetplayPlayerHealth(v.player) <= 0) return false;
    const float* orbit = reinterpret_cast<const float*>(v.player + kOrbit);
    float f[3] = {orbit[0] - orbit[6], orbit[1] - orbit[7], orbit[2] - orbit[8]};
    float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    g_armsNoWhy = 7;
    if (len < 0.02f) return false;
    g_armsNoWhy = 8;
    if (!EyeNow(v, out->eye)) return false;
    g_armsNoWhy = 0;
    for (int i = 0; i < 3; ++i) out->forward[i] = f[i] / len;
    out->weight = Eased(v.blend);
    out->still = *reinterpret_cast<const int*>(v.player + 0x1a34) == 0;   // (its move state: 0 standing, 2 walking)
    // the view's projection (its viewport +0x164: 1 / tan across - negative - and up the diagonal)
    const float* proj = reinterpret_cast<const float*>(kViewports + v.viewport * kViewportSize + 0x164);
    out->tans[0] = 0.79f;
    out->tans[1] = 0.45f;
    if (!IsBadReadPtr(proj, 24) && std::fabs(proj[0]) > 0.1f && std::fabs(proj[5]) > 0.1f) {
        out->tans[0] = 1 / std::fabs(proj[0]);
        out->tans[1] = 1 / std::fabs(proj[5]);
    }
    // the upper body's blending ([player+0x13cc]): an action played over it (+0x78, -1 none; its weight +0x50 - firing,
    // zooming, reloading: the aiming stance, eased in and out) and the relaxed stance's weight (+0x104 - it gives way to
    // the aiming stance, and to a melee swing, a jump, a power-up's throw)
    out->aim = 0;
    out->rest = 1;
    out->action = -1;
    out->idle = 0;
    auto layer = *reinterpret_cast<const unsigned char* const*>(v.player + 0x13cc);
    if (layer && !IsBadReadPtr(layer, 0x110)) {
        float a = *reinterpret_cast<const int*>(layer + 0x78) >= 0 ? *reinterpret_cast<const float*>(layer + 0x50) : 0;
        float b = *reinterpret_cast<const float*>(layer + 0x104);
        out->action = *reinterpret_cast<const int*>(layer + 0x78);
        // (an idle fidget: stood still some 6 s, the game plays one over the relaxed stance - its weight down to 0.03 for
        // ~3 s, Peter's drill turned 64 degrees. Its own weight: +0x48, eased 0..1 (0 standing, firing, in a jump); +0x90
        // 2 while it plays, 3 otherwise)
        out->layer90 = *reinterpret_cast<const int*>(layer + 0x90);
        out->clip = *reinterpret_cast<const int*>(layer + 0x60);
        out->moveClip = *reinterpret_cast<const int*>(layer + 0x5c);
        v.reloading = out->clip >= 4 && out->clip <= 6;
        float iw = *reinterpret_cast<const float*>(layer + 0x48);
        out->idle = iw > 0 ? (iw < 1 ? iw : 1) : 0;
        if (g_layerDump > 0) {   // (research: the upper body's blend block, as words)
            --g_layerDump;
            wchar_t line[1400];
            int at = swprintf_s(line, L"FirstPerson: layer");
            for (int o = 0; o < 0x110 && at < 1300; o += 4) {
                DWORD word = *reinterpret_cast<const DWORD*>(layer + o);
                float as = *reinterpret_cast<const float*>(layer + o);
                if (std::fabs(as) > 1e-4f && std::fabs(as) < 1e5f) at += swprintf_s(line + at, 1400 - at, L" %x:%.3g", o, as);
                else at += swprintf_s(line + at, 1400 - at, L" %x:%lx", o, word);
            }
            Log(L"%s", line);
        }
        out->aim = a > 0 ? (a < 1 ? a : 1) : 0;
        out->rest = b > 0 ? (b < 1 ? b : 1) : 0;
    }
    // (their gun: +0xef58, 0 from the moment a melee swing starts until they aim or fire. Whether it's drawn wouldn't do:
    // lowered out of the view the game stops drawing it, and it would look gone for good)
    out->gunOut = *reinterpret_cast<const DWORD*>(v.player + 0xef58) != 0;
    out->rounds = *reinterpret_cast<const int*>(v.player + 0x4ffc);
    out->alone = true;
    for (int i = 0; i < 4; ++i) {
        float o[4];
        if (i != v.viewport && SplitViewRect(i, o)) out->alone = false;
    }
    // the weapon's zoom: the view narrower than it is when not aiming (zooming eases the aiming stance in first; the
    // game narrows the view's projection itself, not the camera's field of view) - all the way at a fifth narrower
    if (out->aim <= 0 || v.wideAs <= 0) v.wideAs = out->tans[0];
    float wide = out->tans[0] / v.wideAs;
    out->wide = wide < 0.2f ? 0.2f : (wide > 1 ? 1 : wide);
    out->zoom = (1 - out->wide) / kZoomed < 1 ? (1 - out->wide) / kZoomed : 1;
    return true;
}

int FirstPersonArmsNoWhy() { return g_armsNoWhy; }
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
// First person's arms and gun, drawn as a shooter's are (motion.cpp hides the rest of their body away): in front of
// everything (cosmetics.cpp squeezes their depth to the front), and with a field of view of their own (kArmTan) - their
// clip space x and y scaled by `scale`. Their arms are skinned on the CPU (their draws: a skinned one of theirs); the gun
// is a model of its own, at their gun hand (c4..c6: its place). c0..c3 must be a projection (z = A w + B).
extern int g_vmSeq;
const double kOffsetDelay = 0.012;   // (s: the sway and bob drawn this far behind the frame's time - FrameArms)
bool g_frameBob = true;               // (research: "fpv framebob <0|1> [<frames logged>]" - the sway and bob at the
int g_frameBobLog = 0;                //  frame's time)
// A player's arms as first person's, the same for every draw of a frame: the game builds their pose on its own thread,
// now and then between the frame's draws - a gun drawn from one pose and its outline from the next were a turn apart
// spinning (the outline's inside showed: black over the drill's face). False: not first person's.
bool FrameArms(int p, float* hand) {
    View& v = g_view[p];
    if (v.haveFrameArms) {
        memcpy(hand, v.frameArms, sizeof(v.frameArms));
        return true;
    }
    if (!MotionViewmodel(p, hand)) return false;
    // (the sway and bob as they are at this frame's time, for the pose's - kOffsetDelay behind, so between two poses)
    float now[3];
    double ft = OrderNow();
    float posed[3] = {hand[10], hand[11], hand[12]};
    bool got = g_frameBob && MotionOffsetAt(p, ft - kOffsetDelay, now);
    if (got)
        for (int i = 0; i < 3; ++i) hand[10 + i] = now[i] * hand[3] - hand[10 + i];
    else
        hand[10] = hand[11] = hand[12] = 0;
    if (g_frameBobLog > 0) {
        --g_frameBobLog;
        Log(L"FirstPerson: framebob %.5f - at the frame %.5f %.5f, posed %.5f %.5f, put in %.5f %.5f (%s)", ft,
            got ? now[0] * hand[3] : 0, got ? now[1] * hand[3] : 0, posed[0], posed[1], hand[10], hand[11],
            got ? L"got" : L"none");
    }
    if (MotionOrderTrace() > 0)
        Log(L"Order: %.5f snapshot (thread %lu) - eye %.4f %.4f %.4f; camera %.4f %.4f %.4f", OrderNow(),
            GetCurrentThreadId(), hand[4], hand[5], hand[6], v.drawnEye[0], v.drawnEye[1], v.drawnEye[2]);
    memcpy(v.frameArms, hand, sizeof(v.frameArms));
    v.haveFrameArms = true;
    return true;
}
// First person's arms moved with the camera since they were posed: the game builds the pose 0-10 ms before the frame
// is drawn - some frames not at all - the eye on the move meanwhile (1-3 cm walking: at the hand's half a metre 6% of
// it, never the same twice - the arms shook). Drawing the view from the eye they were posed for instead stood the
// view still on the frames without a pose (15 of 119 strafing). So the view is the game's, smooth, and the arms' points
// as that camera would have had them: p' = D P^-1 p (P, D: the eye and the view's axes posed for, drawn). The draw's
// space: the world (a character's skinned points, an outline's), or the model's (the gun's: c4..c6 its place, W) - the
// eye solved through c0, c1, c3 tells. M' = M X: X = D P^-1, or W^-1 D P^-1 W.
bool g_gunWorld = true;   // (research: "fpv gunworld 0" - by where the eye came out alone)
int g_followMask = 7;   // (research: "fpv followmask <bits>" - 1 a character's pieces, 2 a gun's, 4 outlines)
bool CosmeticsDrawSkinned();
void FollowX(const float (*c)[4], const View& v, const float* hand, float (*X)[4]) {
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) X[i][j] = i == j ? 1.0f : 0.0f;
    if (!v.haveDrawn || !g_armsFollow) return;
    int kind = GraphicsDrawingOutline() ? 4 : (CosmeticsDrawSkinned() ? 1 : 2);
    if (!(g_followMask & kind)) return;
    auto axes = [](const float* f, float (*b)[3]) {   // (columns right, up, ahead - motion.cpp's)
        float r[3] = {-f[2], 0, f[0]}, len = sqrtf(r[0] * r[0] + r[2] * r[2]);
        if (len < 1e-6f) { r[0] = 1, r[2] = 0, len = 1; }
        r[0] /= len, r[2] /= len;
        float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
        for (int i = 0; i < 3; ++i) b[i][0] = r[i], b[i][1] = u[i], b[i][2] = f[i];
    };
    // (the camera this draw is drawn with - its own eye and way ahead, from its own c0..c3: an outline shell is drawn
    // in a pass of its own, maybe with a camera a frame apart from the arms' - taken from the frame's, the outline of
    // the arms lagged behind them turning: black round the hand)
    float m[4][4], b[4], e[4];   // (the draw's eye: c0, c1, c3 . (x, y, z, 1) = 0)
    const int rows[3] = {0, 1, 3};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i][j] = c[rows[i]][j];
        m[i][3] = 0;
        b[i] = -c[rows[i]][3];
    }
    m[3][0] = m[3][1] = m[3][2] = 0, m[3][3] = 1, b[3] = 0;
    if (!Solve4(m, b, e)) return;
    float dx = e[0] - v.drawnEye[0], dy = e[1] - v.drawnEye[1], dz = e[2] - v.drawnEye[2];
    float off = dx * dx + dy * dy + dz * dz;
    // (a gun's c0..c3 are the world's too - its eye solves to the camera's. By where the eye came out alone, in places on every map the drawn
    // eye had gone elsewhere and the gun was taken for being in its own space: left at its real place, tiny by the crosshair, the hand empty)
    bool world = off < 1 || (kind == 2 && g_gunWorld);
    float W[3][4], ew[3], fw[3], cf[3] = {c[3][0], c[3][1], c[3][2]};   // (c3: the depth - its x y z, the way ahead)
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) W[i][j] = c[4 + i][j];
    for (int i = 0; i < 3; ++i) {
        ew[i] = world ? e[i] : W[i][0] * e[0] + W[i][1] * e[1] + W[i][2] * e[2] + W[i][3];
        fw[i] = world ? cf[i] : W[i][0] * cf[0] + W[i][1] * cf[1] + W[i][2] * cf[2];
    }
    float fp[3] = {hand[7], hand[8], hand[9]};
    float lf = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]), lp = sqrtf(fp[0] * fp[0] + fp[1] * fp[1] + fp[2] * fp[2]);
    if (lf < 1e-6f || lp < 1e-6f) return;
    for (int i = 0; i < 3; ++i) fw[i] /= lf, fp[i] /= lp;
    float bp[3][3], bd[3][3], T[3][4];
    axes(fp, bp);
    axes(fw, bd);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) T[i][j] = bd[i][0] * bp[j][0] + bd[i][1] * bp[j][1] + bd[i][2] * bp[j][2];
        T[i][3] = ew[i] - (T[i][0] * hand[4] + T[i][1] * hand[5] + T[i][2] * hand[6]);
        // (and the sway and bob as at this frame's time for the pose's: hand[10..12], the view's left, up, ahead)
        T[i][3] += -bd[i][0] * hand[10] + bd[i][1] * hand[11] + bd[i][2] * hand[12];
        T[i][3] -= bd[i][2] * hand[16];   // (kept ahead of the eye: drawn as from there - motion.cpp g_rig)
    }
    if (g_vmSeq > 0)
        Log(L"FirstPerson: seq   follow - %s, %s, eye %.3f from the frame's, moved %.3f", GraphicsDrawingOutline()
            ? L"outline" : L"model", world ? L"world" : L"model space", sqrtf(dx * dx + dy * dy + dz * dz),
            sqrtf(T[0][3] * T[0][3] + T[1][3] * T[1][3] + T[2][3] * T[2][3]));
    if (world) {
        memcpy(X, T, sizeof(T));
        return;
    }
    float Wi[3][4], TW[3][4];
    float det = W[0][0] * (W[1][1] * W[2][2] - W[1][2] * W[2][1]) - W[0][1] * (W[1][0] * W[2][2] - W[1][2] * W[2][0]) +
                W[0][2] * (W[1][0] * W[2][1] - W[1][1] * W[2][0]);
    if (std::fabs(det) < 1e-9f) return;
    float id = 1 / det;
    Wi[0][0] = (W[1][1] * W[2][2] - W[1][2] * W[2][1]) * id, Wi[0][1] = (W[0][2] * W[2][1] - W[0][1] * W[2][2]) * id;
    Wi[0][2] = (W[0][1] * W[1][2] - W[0][2] * W[1][1]) * id, Wi[1][0] = (W[1][2] * W[2][0] - W[1][0] * W[2][2]) * id;
    Wi[1][1] = (W[0][0] * W[2][2] - W[0][2] * W[2][0]) * id, Wi[1][2] = (W[0][2] * W[1][0] - W[0][0] * W[1][2]) * id;
    Wi[2][0] = (W[1][0] * W[2][1] - W[1][1] * W[2][0]) * id, Wi[2][1] = (W[0][1] * W[2][0] - W[0][0] * W[2][1]) * id;
    Wi[2][2] = (W[0][0] * W[1][1] - W[0][1] * W[1][0]) * id;
    for (int i = 0; i < 3; ++i) Wi[i][3] = -(Wi[i][0] * W[0][3] + Wi[i][1] * W[1][3] + Wi[i][2] * W[2][3]);
    auto mul = [](const float (*a)[4], const float (*bb)[4], float (*o)[4]) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j)
                o[i][j] = a[i][0] * bb[0][j] + a[i][1] * bb[1][j] + a[i][2] * bb[2][j] + (j == 3 ? a[i][3] : 0);
    };
    mul(T, W, TW);
    mul(Wi, TW, X);
}
// c0..c3 through X (M X), their x and y by `scale` (the arms' own field of view), and depth of their own: near
// kArmDepth[0] to far kArmDepth[1] across the depth (cosmetics.cpp draws them into the front kFront of it). The game's
// near plane would cut into the arms shrunk toward the eye; brought nearer, its depth was too coarse there - an outline
// shell's inside showed through a gun turned the camera's way (black over the drill's face).
// Then zoomed by hand[13] about their spot on the screen, hand[14..15]: x' = m x + (1 - m) S w - arms drawn from afar for
// their flat perspective, as big as ever on the screen.
const float kArmDepth[2] = {0.02f, 3.0f};
void Followed(const float (*c)[4], const float (*X)[4], float scale, const float* hand, float (*out)[4]) {
    for (int r = 0; r < 4; ++r) {
        for (int j = 0; j < 4; ++j)
            out[r][j] = c[r][0] * X[0][j] + c[r][1] * X[1][j] + c[r][2] * X[2][j] + (j == 3 ? c[r][3] : 0);
        if (r < 2) for (int j = 0; j < 4; ++j) out[r][j] *= scale;
    }
    float zoom = hand[13] > 0 ? hand[13] : 1;
    for (int r = 0; r < 2; ++r)
        for (int j = 0; j < 4; ++j) out[r][j] = zoom * out[r][j] + (1 - zoom) * hand[14 + r] * out[3][j];
    float n = kArmDepth[0], f = kArmDepth[1], A = f / (f - n), B = -n * f / (f - n);   // (z = A w + B: 0 at n, 1 at f)
    for (int j = 0; j < 4; ++j) out[2][j] = A * out[3][j] + (j == 3 ? B : 0);
}

bool g_vmLog = false;   // (research: "fpv vmlog <0|1>", "fpv vmdump <n>": the next n of a player's unskinned draws)
int g_vmDump = 0;
bool ViewmodelDraw(const float (*c)[4], bool skinned, float (*out)[4]);
int g_vmSeq = 0;            // (research: "fpv vmseq <n>" - the next n draws, in order)
bool FirstPersonViewmodel(const float (*c)[4], bool skinned, float (*out)[4]) {
    bool vm = ViewmodelDraw(c, skinned, out);
    if (skinned) {
        ++g_vmSkin[0];
        if (CosmeticsDrawPlayer() >= 0) ++g_vmSkin[1];
        if (vm) ++g_vmSkin[2];
    }
    if (g_vmSeq > 0) {
        --g_vmSeq;
        bool origin = c[4][3] == 0 && c[5][3] == 0 && c[6][3] == 0;
        Log(L"FirstPerson: seq - %s%s%s player %d%s", vm ? L"VIEWMODEL " : L"", skinned ? L"skinned" : L"unskinned",
            GraphicsDrawingOutline() ? L" outline" : L"", CosmeticsDrawPlayer(), origin ? L" at the origin" : L"");
    }
    return vm;
}
// (a reload's old drum: let go by the game at the gun, near the eye with the shrunk arm - but at its full size, a wall of black
// across the view with its inside showing. Not drawn while that near; it's seen falling from there)
const float kDropNear = 1.2f;
bool g_skipDraw = false;
bool FirstPersonSkipDraw() { return g_skipDraw; }
bool g_gunByHand = true;   // (research: "fpv gunbyhand 0")
int g_vmPlayer = -1;
bool FirstPersonInked() { int p = g_vmPlayer; return p >= 0 && p < 4 && g_view[p].haveFrameArms && g_view[p].frameArms[16] > 0; }
bool ViewmodelDraw(const float (*c)[4], bool skinned, float (*out)[4]) {
    g_skipDraw = false;
    if (!g_any) return false;
    int view = SplitViewDrawing();
    if (view < 0 || view >= 4 || g_pulled[view].pull > 0.05f) return false;
    int p = CosmeticsDrawPlayer();
    float hand[26];
    if (g_vmDump > 0 && MotionViewmodel(0, hand) && (GraphicsDrawingOutline() || (p == 0 && !skinned))) {
        --g_vmDump;
        Log(L"FirstPerson: vm dump - %s%s player %d at %.2f %.2f %.2f (hand %.2f %.2f %.2f) rows %.2f %.2f %.2f / %.2f "
            L"%.2f %.2f / %.2f %.2f %.2f", GraphicsDrawingOutline() ? L"outline " : L"", skinned ? L"skinned" : L"unskinned",
            p, c[4][3], c[5][3], c[6][3], hand[0], hand[1], hand[2], c[4][0], c[4][1], c[4][2], c[5][0], c[5][1], c[5][2],
            c[6][0], c[6][1], c[6][2]);
    }
    if (g_vmLog && MotionViewmodel(0, hand)) {   // (every draw: the nearest the gun hand, skinned and not)
        static DWORD last = 0;
        static int seen = 0, nearP[2] = {-9, -9};
        static float nearest[2] = {1e9f, 1e9f}, at[2][3], big[2];
        float d[3] = {c[4][3] - hand[0], c[5][3] - hand[1], c[6][3] - hand[2]};
        float dd = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        int s = skinned ? 1 : 0;
        static int mine = 0;
        if (skinned && p == 0) ++mine;
        ++seen;
        if (dd < nearest[s]) {
            nearest[s] = dd, nearP[s] = p, big[s] = c[4][0] * c[4][0] + c[4][1] * c[4][1] + c[4][2] * c[4][2];
            for (int i = 0; i < 3; ++i) at[s][i] = c[4 + i][3];
        }
        if (GetTickCount() - last > 500) {
            Log(L"FirstPerson: vm - %d draws (%d skinned of player 0), gun hand (%.2f %.2f %.2f); nearest unskinned %.2f (%.2f %.2f %.2f, player %d, "
                L"scale^2 %.2f), skinned %.2f (%.2f %.2f %.2f, player %d, scale^2 %.2f)", seen, mine, hand[0], hand[1], hand[2],
                nearest[0], at[0][0], at[0][1], at[0][2], nearP[0], big[0], nearest[1], at[1][0], at[1][1], at[1][2],
                nearP[1], big[1]);
            last = GetTickCount(), seen = 0, mine = 0, nearest[0] = nearest[1] = 1e9f;
        }
    }
    // (whose draw it is goes by the player's feet, within 3 - but a gun kept ahead of the eye is 2.2 from them and more: past that, in places,
    // it wasn't theirs and was left where it really is, tiny by the crosshair, the hand empty. Known by being at a hand instead)
    if (!skinned && g_gunByHand && (p < 0 || p >= 4 || g_view[p].viewport != view))
        for (int q = 0; q < 4; ++q) {
            float h2[26];
            if (g_view[q].viewport != view || g_view[q].blend < 0.5f || !FrameArms(q, h2)) continue;
            float e0 = c[4][3] - h2[0], e1 = c[5][3] - h2[1], e2 = c[6][3] - h2[2];
            if (e0 * e0 + e1 * e1 + e2 * e2 < 36 * h2[3] * h2[3]) { p = q; break; }
        }
    g_vmPlayer = p;
    if (p < 0 || p >= 8 || g_view[p].viewport != view || g_view[p].blend < 0.5f || p >= 4) { g_vmWhy = 1; return false; }
    if (!FrameArms(p, hand)) {
        g_vmWhy = 2;
        if (!skinned) ++g_vmNoArms;
        return false;
    }
    if (!skinned) {   // (a gun: a model on their gun hand, shrunk with it - its own place can be well off the hand,
                      //  Peter's drill's 1.3 at full size; the sky dome is at the eye too, full size and unturned)
        float d[3] = {c[4][3] - hand[0], c[5][3] - hand[1], c[6][3] - hand[2]};
        float big = c[4][0] * c[4][0] + c[4][1] * c[4][1] + c[4][2] * c[4][2], s = hand[3];
        bool unturned = std::fabs(c[4][0] - 1) < 1e-3f && std::fabs(c[5][1] - 1) < 1e-3f && std::fabs(c[6][2] - 1) < 1e-3f;
        // (as far as the off hand goes: what it holds in a reload is a model on it - left out, it floated by itself)
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 36 * s * s || big > 2.25f * s * s || unturned) {
            if (!unturned) {
                ++g_vmLeft;
                g_vmLeftD = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), g_vmLeftBig = big, g_vmLeftS = s;
            }
            float e[3] = {c[4][3] - hand[4], c[5][3] - hand[5], c[6][3] - hand[6]};
            if (!unturned && g_view[p].reloading && e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < kDropNear * kDropNear) g_skipDraw = true;
            static ULONGLONG lastSaid = 0;
            if (!unturned && GetTickCount64() - lastSaid > 5000 && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 225 * s * s) {
                lastSaid = GetTickCount64();
                Log(L"FirstPerson: something by the gun hand left out - %.2f off (shrunk %.3f), scale %.3f", sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), s, sqrtf(big));
            }
            static DWORD lastOut = 0;
            if (g_vmLog && !unturned && GetTickCount() - lastOut > 50) {
                lastOut = GetTickCount();
                Log(L"FirstPerson: vm a model of theirs left out - %.2f from the gun hand (shrunk %.2f), scale %.2f%s",
                    sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), s, sqrtf(big), GraphicsDrawingOutline() ? L", outline" : L"");
            }
            if (g_vmDump > 0) {
                --g_vmDump;
                Log(L"FirstPerson: vm left out - %.2f from the gun hand (shrunk %.2f), scale %.2f%s%s", sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]),
                    s, sqrtf(big), unturned ? L", unturned" : L"", GraphicsDrawingOutline() ? L", outline" : L"");
            }
            return false;
        }
        ++(GraphicsDrawingOutline() ? g_vmOutlines : g_vmParts);
        if (g_vmCount && !GraphicsDrawingOutline() && g_vmPieces < 8) {
            const float* f = hand + 7;
            float r[3] = {-f[2], 0, f[0]}, lr = sqrtf(r[0] * r[0] + r[2] * r[2]);
            if (lr > 1e-6f) r[0] /= lr, r[2] /= lr;
            float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
            float e[3] = {c[4][3] - hand[4], c[5][3] - hand[5], c[6][3] - hand[6]};
            float* q = g_vmPiece[g_vmPieces++];
            q[0] = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            q[1] = e[0] * f[0] + e[1] * f[1] + e[2] * f[2];
            q[2] = e[0] * r[0] + e[1] * r[1] + e[2] * r[2];
            q[3] = e[0] * u[0] + e[1] * u[1] + e[2] * u[2];
        }
        static DWORD lastAxes = 0;
        if (g_vmLog && g_view[p].haveDrawn && GetTickCount() - lastAxes > 300) {   // (the gun's axes in the view)
            lastAxes = GetTickCount();
            const float* f = g_view[p].drawnFw;
            float r[3] = {-f[2], 0, f[0]}, lr = sqrtf(r[0] * r[0] + r[2] * r[2]);
            r[0] /= lr, r[2] /= lr;
            float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
            float ax[3][3];
            for (int j = 0; j < 3; ++j) {
                float col[3] = {c[4][j], c[5][j], c[6][j]}, l = sqrtf(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
                ax[j][0] = (col[0] * r[0] + col[1] * r[1] + col[2] * r[2]) / l;
                ax[j][1] = (col[0] * u[0] + col[1] * u[1] + col[2] * u[2]) / l;
                ax[j][2] = (col[0] * f[0] + col[1] * f[1] + col[2] * f[2]) / l;
            }
            Log(L"FirstPerson: gun axes in the view (right up ahead) - x %.2f %.2f %.2f, y %.2f %.2f %.2f, z %.2f %.2f %.2f",
                ax[0][0], ax[0][1], ax[0][2], ax[1][0], ax[1][1], ax[1][2], ax[2][0], ax[2][1], ax[2][2]);
            {   // (and in the hand's: columns of G = H^T A, H the hand's axes in the view, A the gun's - left, up, ahead)
                const float* Hm = hand + 17;
                float G[3][3];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        G[r][c] = Hm[0 * 3 + r] * -ax[c][0] + Hm[1 * 3 + r] * ax[c][1] + Hm[2 * 3 + r] * ax[c][2];
                Log(L"FirstPerson: gun in hand %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", G[0][0], G[0][1], G[0][2], G[1][0], G[1][1], G[1][2], G[2][0], G[2][1], G[2][2]);
            }
        }
    }
    const float* c2 = c[2];
    const float* c3 = c[3];
    int k = 0;
    for (int i = 1; i < 3; ++i) if (std::fabs(c3[i]) > std::fabs(c3[k])) k = i;
    g_vmWhy = 3;
    if (std::fabs(c3[k]) < 1e-6f) return false;
    float A = c2[k] / c3[k];
    g_vmWhy = 4;
    for (int i = 0; i < 3; ++i) if (std::fabs(c2[i] - A * c3[i]) > 1e-3f * (std::fabs(c2[i]) + 1)) return false;
    g_vmWhy = 0;
    const float* proj = reinterpret_cast<const float*>(kViewports + view * kViewportSize + 0x164);
    float scale = IsBadReadPtr(proj, 4) || std::fabs(proj[0]) < 0.1f ? 1.0f : 1 / std::fabs(proj[0]) / kArmTan;
    float X[3][4];
    FollowX(c, g_view[p], hand, X);
    Followed(c, X, scale, hand, out);
    return true;
}

// A character's outline shells are drawn in a pass of their own, later - unskinned, their points in the world, nothing
// saying whose. While a player's arms are drawn as first person's, whatever of a shell is within kShellNear of their eye
// is their arms' (nothing else comes that near): that part drawn as the arms are (with the camera, their field of view,
// in front - `inner`, its clip plane: within), the rest as the game draws it (`farPlane`: beyond). False: a shell as it is.
const float kShellNear = 1.0f;
// (arms kept ahead of the eye - Peter's, motion.cpp g_rig - have ink of their own: the part of a shell within reach is left out, not
// drawn as the arms. Inside-out black copies of them, in front of everything)
bool g_shellInner = true;
bool FirstPersonShellInner() { return g_shellInner; }
bool g_shellSplit = true;   // (research: "fpv shells <0|1>")
bool FirstPersonShell(const float (*c)[4], float (*inner)[4], float* nearPlane, float* farPlane) {
    if (!g_any || !g_shellSplit || !GraphicsDrawingOutline() || c[4][3] != 0 || c[5][3] != 0 || c[6][3] != 0) return false;
    int view = SplitViewDrawing();
    if (view < 0 || view >= 4 || g_pulled[view].pull > 0.05f) return false;
    float hand[26];
    int p = 0;
    for (; p < 4; ++p)
        if (g_view[p].viewport == view && g_view[p].blend >= 0.5f && FrameArms(p, hand)) break;
    if (p == 4) return false;
    const float* c2 = c[2];
    const float* c3 = c[3];
    int k = 0;
    for (int i = 1; i < 3; ++i) if (std::fabs(c3[i]) > std::fabs(c3[k])) k = i;
    if (std::fabs(c3[k]) < 1e-6f) return false;
    float A = c2[k] / c3[k], B = c2[3] - A * c3[3];
    for (int i = 0; i < 3; ++i) if (std::fabs(c2[i] - A * c3[i]) > 1e-3f * (std::fabs(c2[i]) + 1)) return false;
    if (std::fabs(B) < 1e-9f) return false;
    const float* proj = reinterpret_cast<const float*>(kViewports + view * kViewportSize + 0x164);
    float scale = IsBadReadPtr(proj, 4) || std::fabs(proj[0]) < 0.1f ? 1.0f : 1 / std::fabs(proj[0]) / kArmTan;
    // (in front with the arms, their near plane nearer - motion.cpp keeps their hidden body, a dot of outline, behind
    // the eye: an arm's length ahead it came out over the gun, black)
    float X[3][4];
    FollowX(c, g_view[p], hand, X);
    Followed(c, X, scale, hand, inner);
    g_shellInner = hand[16] <= 0;
    // (clip planes, in clip space: q . (x, y, z, w) >= 0 kept; with z = A w + B, depth w - d >= 0 is (0, 0, -d / B,
    // 1 + d A / B) - and d - w >= 0 its negative. Within: as the arms' own depth draws it.)
    float an = kArmDepth[1] / (kArmDepth[1] - kArmDepth[0]), bn = -kArmDepth[0] * an;
    nearPlane[0] = nearPlane[1] = 0;
    nearPlane[2] = kShellNear / bn;
    nearPlane[3] = -(1 + kShellNear * an / bn);
    farPlane[0] = farPlane[1] = 0;
    farPlane[2] = -kShellNear / B;
    farPlane[3] = 1 + kShellNear * A / B;
    return true;
}

const float kPullHide = 0.6f;
int FirstPersonOwnDraw(const float (*c)[4], float* planes) {
    if (!g_any || !g_hideMode) return 0;
    int view = SplitViewDrawing();
    // (the view going behind them or coming back: through their head, the inside of a hat filling the screen, black - theirs not drawn
    // until it's clear of them. -2: left out, whatever it is. The sky dome's at the camera too: unturned, full size)
    if (view >= 0 && view < 4 && g_pulled[view].pull > 0.05f && g_pulled[view].pull < kPullHide) {
        if (GraphicsDrawingOutline()) return -2;
        int mine = CosmeticsDrawPlayer();
        bool sky = std::fabs(c[4][0] - 1) < 1e-3f && std::fabs(c[5][1] - 1) < 1e-3f && std::fabs(c[6][2] - 1) < 1e-3f;
        if (mine >= 0 && mine < 8 && g_view[mine].viewport == view && !sky) return -2;
        return 0;
    }
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
// (research: "fpv snap <0..3>" keeps player 0's memory, "fpv diff" logs what's the same in 0 and 2, the same in 1 and 3, and
// differs between them - a state that goes with what was done between the snaps)
const size_t kSnapSize = 0x12000;
std::vector<unsigned char> g_snap[4];
void SnapTake(int i) {
    auto player = g_view[0].player;
    if (i < 0 || i > 3 || !player) return;
    g_snap[i].assign(kSnapSize, 0);
    for (size_t o = 0; o < kSnapSize; o += 0x100)
        if (!IsBadReadPtr(player + o, 0x100)) memcpy(g_snap[i].data() + o, player + o, 0x100);
    Log(L"FirstPerson: snap %d taken", i);
}
void SnapDiff() {
    for (auto& s : g_snap) if (s.size() != kSnapSize) return;
    int shown = 0;
    for (size_t o = 0; o < kSnapSize && shown < 80; o += 4) {
        DWORD v[4];
        for (int i = 0; i < 4; ++i) memcpy(&v[i], g_snap[i].data() + o, 4);
        if (v[0] == v[2] && v[1] == v[3] && v[0] != v[1]) {
            Log(L"FirstPerson: snap +%04zx %08lx -> %08lx", o, v[0], v[1]);
            ++shown;
        }
    }
    Log(L"FirstPerson: snap diff done (%d)", shown);
}
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
    if (_stricmp(p, "body") == 0) { MotionArmsBodyTest(atoi(on) == 0); return; }   // fpv body <0|1> - 1: shown
    if (_stricmp(p, "gunaim") == 0) { MotionGunAimTest(atoi(on) != 0); return; }   // fpv gunaim <0|1>
    if (_stricmp(p, "offhand") == 0) { MotionOffHandTest(atoi(on) != 0); return; }   // fpv offhand <0|1>
    if (_stricmp(p, "framebob") == 0) {   // fpv framebob <0|1> [<frames logged>]
        char* s = strtok_s(nullptr, " \t", &ctx);
        g_frameBob = atoi(on) != 0;
        g_frameBobLog = s ? atoi(s) : 0;
        return;
    }
    if (_stricmp(p, "order") == 0) { MotionOrderTest(atoi(on)); return; }   // fpv order <poses>
    if (_stricmp(p, "layer") == 0) { g_layerDump = atoi(on); return; }   // fpv layer <n>
    if (_stricmp(p, "snap") == 0) { SnapTake(atoi(on)); return; }   // fpv snap <0..3>
    if (_stricmp(p, "diff") == 0) { SnapDiff(); return; }   // fpv diff
    if (_stricmp(p, "vmcount") == 0) { g_vmCount = atoi(on) != 0; return; }   // fpv vmcount <0|1>
    if (_stricmp(p, "offbones") == 0) { MotionOffBones(); return; }   // fpv offbones 1
    if (_stricmp(p, "rig") == 0) {   // fpv rig <hands ahead> <shoulder right> <down> <ahead> <shrink> <kept ahead>
        float values[6] = {(float)atof(on), 0, 0, 0, 0, 0};
        int count = 1;
        while (count < 6) {
            char* v = strtok_s(nullptr, " \t", &ctx);
            if (!v) break;
            values[count++] = (float)atof(v);
        }
        MotionRig(values, count);
        return;
    }
    if (_stricmp(p, "fingers") == 0) {   // fpv fingers <relaxed> <tip> <grip> <tip> <fist> <tip>
        float values[6] = {(float)atof(on), 0, 0, 0, 0, 0};
        int count = 1;
        while (count < 6) {
            char* v = strtok_s(nullptr, " \t", &ctx);
            if (!v) break;
            values[count++] = (float)atof(v);
        }
        MotionFingers(values, count);
        return;
    }
    if (_stricmp(p, "less") == 0) {   // fpv less <reload> <walk>
        char* w = strtok_s(nullptr, " \t", &ctx);
        MotionLess((float)atof(on), w ? (float)atof(w) : 0);
        return;
    }
    if (_stricmp(p, "thumb") == 0) {   // fpv thumb <x y z> <x y z>: on the grip, in the left hand's axes
        float values[6] = {(float)atof(on), 0, 0, 0, 0, 0};
        for (int i = 1; i < 6; ++i) {
            char* v = strtok_s(nullptr, " \t", &ctx);
            if (!v) break;
            values[i] = (float)atof(v);
        }
        MotionGripThumb(values);
        return;
    }
    if (_stricmp(p, "kick") == 0) {   // fpv kick <back> <up> <pitch> <yaw>
        float values[4] = {(float)atof(on), 0, 0, 0};
        int count = 1;
        while (count < 4) {
            char* v = strtok_s(nullptr, " \t", &ctx);
            if (!v) break;
            values[count++] = (float)atof(v);
        }
        MotionKick(values, count);
        return;
    }
    if (_stricmp(p, "shots") == 0) { MotionShotLog(atoi(on) != 0); return; }
    if (_stricmp(p, "shotwatch") == 0) { g_shotLog = atoi(on) != 0; MotionShotLog(g_shotLog); return; }
    if (_stricmp(p, "nearblur") == 0) { GraphicsAimNearBlur(atoi(on) != 0); return; }   // fpv nearblur <0|1>
    if (_stricmp(p, "framelog") == 0) { GraphicsFrameLog(atoi(on)); return; }   // fpv framelog <frames>
    if (_stricmp(p, "gunworld") == 0) { g_gunWorld = atoi(on) != 0; return; }
    if (_stricmp(p, "gunbyhand") == 0) { g_gunByHand = atoi(on) != 0; return; }
    if (_stricmp(p, "hidedown") == 0) {   // fpv hidedown <y> <ahead>
        char* a = strtok_s(nullptr, " \t", &ctx);
        MotionHideDown((float)atof(on), a ? (float)atof(a) : 0.4f);
        return;
    }
    if (_stricmp(p, "punch") == 0) {   // fpv punch <up> <in> <twist> <thumb +-1>
        float values[4] = {(float)atof(on), 0, 0, 0};
        int count = 1;
        while (count < 4) {
            char* v = strtok_s(nullptr, " \t", &ctx);
            if (!v) break;
            values[count++] = (float)atof(v);
        }
        MotionPunchAim(values, count);
        return;
    }
    if (_stricmp(p, "ink") == 0) {   // fpv ink <px at 720 lines> <depth bias> <slope>
        char* b = strtok_s(nullptr, " \t", &ctx);
        char* s = strtok_s(nullptr, " \t", &ctx);
        CosmeticsArmInkTest((float)atof(on), b ? (float)atof(b) : 3e-5f, s ? (float)atof(s) : 3);
        return;
    }
    if (_stricmp(p, "armsraw") == 0) { g_armsRaw = atoi(on) != 0; return; }   // fpv armsraw <0|1>
    if (_stricmp(p, "armflat") == 0) { MotionArmFlat((float)atof(on)); return; }   // fpv armflat <1: their own>
    if (_stricmp(p, "armsize") == 0) { MotionArmSize((float)atof(on)); return; }   // fpv armsize <1: as set>
    if (_stricmp(p, "calm") == 0) { MotionArmCalm((float)atof(on)); return; }   // fpv calm <0..1>
    if (_stricmp(p, "pose") == 0) {   // fpv pose <0 relaxed, 1 aiming, 2 zoomed, 3 reloading, 4 hands empty> <right> <down> <pitch> <yaw> <roll> [off x, y, way]
        float values[13];
        int count = 0;
        while (count < 13) {
            char* s = strtok_s(nullptr, " \t", &ctx);
            if (!s) break;
            values[count++] = (float)atof(s);
        }
        MotionArmPose(atoi(on), values, count);
        return;
    }
    if (_stricmp(p, "moves") == 0) {   // fpv moves <0|1|2> [<swing>] [<log 0|1>] - their arms on the move
        char* s1 = strtok_s(nullptr, " \t", &ctx);
        char* s2 = s1 ? strtok_s(nullptr, " \t", &ctx) : nullptr;
        MotionArmMoves(atoi(on), s1 ? (float)atof(s1) : 0, s2 && atoi(s2) != 0);
        return;
    }
    if (_stricmp(p, "armswhy") == 0) { MotionArmsWhyTest(atoi(on) != 0); return; }   // fpv armswhy <0|1>
    if (_stricmp(p, "followmask") == 0) { g_followMask = atoi(on); return; }   // fpv followmask <bits>
    if (_stricmp(p, "bobtrace") == 0) {   // fpv bobtrace <poses> [1: the gun's bob]
        char* s = strtok_s(nullptr, " \t", &ctx);
        MotionBobTraceTest(atoi(on), s && atoi(s) != 0);
        return;
    }
    if (_stricmp(p, "gamebob") == 0) {   // fpv gamebob <up> <side>
        char* s = strtok_s(nullptr, " \t", &ctx);
        MotionGameBobTest((float)atof(on), s ? (float)atof(s) : (float)atof(on));
        return;
    }
    if (_stricmp(p, "shells") == 0) { g_shellSplit = atoi(on) != 0; return; }   // fpv shells <0|1>
    if (_stricmp(p, "armsfollow") == 0) { g_armsFollow = atoi(on) != 0; return; }   // fpv armsfollow <0|1>
    if (_stricmp(p, "armstrace") == 0) { MotionArmsTraceTest(atoi(on)); return; }   // fpv armstrace <frames>
    if (_stricmp(p, "armscale") == 0) { MotionArmsScaleTest((float)atof(on)); return; }   // fpv armscale <s>
    if (_stricmp(p, "vmseq") == 0) { g_vmSeq = atoi(on); return; }
    if (_stricmp(p, "vmdump") == 0) { g_vmDump = atoi(on); return; }
    if (_stricmp(p, "vmlog") == 0) { g_vmLog = atoi(on) != 0; return; }   // fpv vmlog <0|1> - the gun's draws
    if (_stricmp(p, "armsdev") == 0) {   // fpv armsdev <0..1> [<log 0|1>] - the gun hand kept on its hold between stances
        char* s = strtok_s(nullptr, " \t", &ctx);
        MotionArmsDevTest((float)atof(on), s && atoi(s) != 0);
        return;
    }
    if (_stricmp(p, "armszoom") == 0) {   // fpv armszoom <right> <down> - the gun hand's place zoomed in
        char* s = strtok_s(nullptr, " \t", &ctx);
        MotionArmsZoomTest((float)atof(on), s ? (float)atof(s) : 0.75f);
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
