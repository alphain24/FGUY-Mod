// FGUY Controller Look: the right stick turns the view the way modern shooters do it - a round deadzone, a response
// curve, the turn at once (no build-up), and a speed-up when the stick is held at the edge.
//
// How the game does it. A controller's poll (0x4b5670: XInputGetState) turns its sticks into floats through 0x4b4ec0
// (the four shorts LX LY RX RY; each axis through 0x4bba80): a deadzone of 7849 a side, on each axis alone (a square
// one, a quarter of the travel), the rest stretched to 0..1 ((|v| - 7849) / 24918; 24919 below zero). Then the camera
// (0x581d40, a camera method, every frame, for every player's camera): the stick
// (camera +0xc0 x, +0xc4 y, -1..1) is clipped to the unit circle and scaled by a*m + b*m^2 + c*m^3 (m its length; a, b,
// c the camera's settings [camera+0x34c] +0x88 / +0x8c / +0x90); the turn speed it asks for (times the settings'
// top speed, +0x7c yaw / +0x84 pitch, and the player's aim sensitivity, 0x4fd070) is then reached by a fixed
// acceleration (+0x78 / +0x80, times 0x4fce40) - 0x55cc60: the speed moves toward the asked one by accel * dt a frame,
// and drops to 0 only once the stick is under 5%. That build-up is the "turns faster the longer you hold it" and the
// floaty, late small corrections. The result - turn speeds, pitch [0] and yaw [1] - goes to camera +0x2c8, which the
// camera's update (0x580ab0) adds to its yaw / pitch (times camera +0xcc and dt). Keyboard / mouse (camera +0xc8 set)
// and a camera held still (+0x300) are left to the game.
//
// (Measured, the game's own: curve 0, 0, 1 - a cube; top speed 3.84 yaw / 2.09 pitch (rad/s), built up at 5.76 / 3.14
// rad/s^2 - two thirds of a second to full speed; the sensitivity factor 0.75 / 1 / 1.25.)
//
// This plugin: the right stick's raw values go to the game's converter remapped so that its own sums give a round
// deadzone of ours instead (the left stick - walking - as the game has it); and the camera's function runs, then
// (controller only) its speeds are written over: the same top speed and sensitivity, our curve, no build-up, the ramp
// at the edge. The stick and dt are the game's own (online: the match's synced input, the same on every PC) and the
// settings come from mod.ini (the same on everyone's in a lobby: Type=gameplay) - every PC turns every camera alike.
#include <windows.h>

#include <cmath>
#include <cstdio>

#include "../../../sdk/fguymod_plugin.h"

namespace {

const FGUYModAPI* g_api = nullptr;

const uintptr_t kSticks = 0x4b4ec0;         // void __cdecl Sticks(out, short* lxlyrxry, count, center, min, max, -dz, dz)
const int kGameDeadzone = 7849;             // (its -dz / dz from the poll; the travel past it: 24918 up, 24919 down)
const uintptr_t kLook = 0x581d40;           // void __thiscall Look(camera, float dt, void*, void*, float* out)
const uintptr_t kSensSpeed = 0x4fd070;      // float __fastcall (camera): the aim sensitivity's top-speed factor
const int kStickX = 0xc0, kStickY = 0xc4;   // camera: the stick (-1..1)
const int kMouseMode = 0xc8;                // camera: (byte) keyboard / mouse
const int kHeld = 0x300;                    // camera: (byte) held still
const int kSettings = 0x34c;                // camera: its settings
const int kOrbitYaw = 0x26c;                // camera: the orbit block's yaw (block +0xc; radians) - research log only
const int kYawTop = 0x7c, kPitchTop = 0x84; // settings: top turn speeds
const int kCurveA = 0x88, kCurveB = 0x8c, kCurveC = 0x90;   // settings: the response polynomial

using Sticks_t = void(__cdecl*)(void* out, short* sticks, int count, int center, int min, int max, int negDz, int dz);
using Look_t = void(__fastcall*)(void* cam, void* edx, float dt, void* a, void* b, float* out);
using Sens_t = float(__fastcall*)(void* cam);
Sticks_t g_origSticks = nullptr;
Look_t g_origLook = nullptr;

// mod.ini [Settings], read once.
struct Settings {
    float inner = 0.08f, outer = 0.03f;   // deadzones (fractions of the stick's travel)
    float curve = 2.0f;                   // response exponent: 1 linear, 2 like CoD's "Standard"
    float speed = 1.0f;                   // top turn speed, times the game's
    float pitch = 1.0f;                   // up / down, times left / right
    float rampDelay = 0.15f;              // seconds at the edge before speeding up
    float rampTime = 0.35f;               // seconds to reach the full speed-up
    float rampBoost = 1.8f;               // yaw speed at the end of the ramp, times the top speed
    float rampPitch = 1.2f;               // the same for pitch
    float rampEdge = 0.95f;               // how far the stick counts as "at the edge"
    bool log = false;
    bool bypass = false;                  // research: the game's own look (with Log: to measure it)
    float trace = 0;                      // research: seconds of frame-by-frame trace (Trace)
} g_set;

float Pct(const wchar_t* key, int def) { return g_api->Setting(key, def) / 100.0f; }
float Ms(const wchar_t* key, int def) { return g_api->Setting(key, def) / 1000.0f; }

void ReadSettings() {
    g_set.inner = Pct(L"InnerDeadzone", 8);
    g_set.outer = Pct(L"OuterDeadzone", 3);
    g_set.curve = Pct(L"Curve", 200);
    g_set.speed = Pct(L"Speed", 100);
    g_set.pitch = Pct(L"PitchSpeed", 100);
    g_set.rampDelay = Ms(L"RampDelay", 150);
    g_set.rampTime = Ms(L"RampTime", 350);
    g_set.rampBoost = Pct(L"RampBoost", 180);
    g_set.rampPitch = Pct(L"RampPitchBoost", 120);
    g_set.rampEdge = Pct(L"RampEdge", 95);
    g_set.log = g_api->Setting(L"Log", 0) != 0;
    g_set.bypass = g_api->Setting(L"Bypass", 0) != 0;
    g_set.trace = (float)g_api->Setting(L"Trace", 0);
    // (sane limits: a bad mod.ini mustn't spin the view or freeze it)
    auto clamp = [](float& v, float lo, float hi) { v = v < lo ? lo : v > hi ? hi : v; };
    clamp(g_set.inner, 0, 0.5f);
    clamp(g_set.outer, 0, 0.3f);
    clamp(g_set.curve, 0.5f, 4);
    clamp(g_set.speed, 0.2f, 3);
    clamp(g_set.pitch, 0.2f, 3);
    clamp(g_set.rampDelay, 0, 2);
    clamp(g_set.rampTime, 0, 3);
    clamp(g_set.rampBoost, 1, 4);
    clamp(g_set.rampPitch, 1, 4);
    clamp(g_set.rampEdge, 0.5f, 1);
}

void Logf(const wchar_t* fmt, ...) {
    wchar_t line[400];
    va_list a;
    va_start(a, fmt);
    vswprintf_s(line, fmt, a);
    va_end(a);
    g_api->Log(line);
}

// Each camera's time at the edge of the stick (a few cameras: the players', the menus').
struct Ramp { void* cam; float held; };
Ramp g_ramps[16];

Ramp& RampOf(void* cam) {
    for (Ramp& r : g_ramps)
        if (r.cam == cam) return r;
    for (Ramp& r : g_ramps)
        if (!r.cam) { r = {cam, 0}; return r; }
    static int next = 0;   // (full: reuse the slots in turn)
    Ramp& r = g_ramps[next++ % 16];
    r = {cam, 0};
    return r;
}

float Smoothstep(float t) { return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t); }

template <class T> T At(void* base, int off) { return *reinterpret_cast<T*>(static_cast<unsigned char*>(base) + off); }

// The raw value the game's converter turns into `d` (-1..1): past its deadzone by d of the travel.
short GameRaw(float d) {
    if (d == 0) return 0;
    int v = d > 0 ? kGameDeadzone + (int)lroundf(d * (32767 - kGameDeadzone))
                  : -kGameDeadzone + (int)lroundf(d * (32768 - kGameDeadzone));
    return (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// A controller's sticks as floats: the right one with our round deadzone (diagonals as straight pushes), the rest of
// the travel stretched to 0..1 - no jump at its edge.
void __cdecl Sticks(void* out, short* sticks, int count, int center, int min, int max, int negDz, int dz) {
    if (g_set.bypass || !sticks || count < 2 || negDz != -kGameDeadzone || dz != kGameDeadzone) {
        g_origSticks(out, sticks, count, center, min, max, negDz, dz);
        return;
    }
    short s[4] = {sticks[0], sticks[1], sticks[2], sticks[3]};
    float x = s[2] / 32767.0f, y = s[3] / 32767.0f;
    float m = sqrtf(x * x + y * y);
    float t = (m - g_set.inner) / (1 - g_set.outer - g_set.inner);
    if (m <= g_set.inner || t <= 0) {
        s[2] = s[3] = 0;
    } else {
        if (t > 1) t = 1;
        s[2] = GameRaw(x / m * t);
        s[3] = GameRaw(y / m * t);
    }
    g_origSticks(out, s, count, center, min, max, negDz, dz);
}

void Trace(void* cam, float m, float dt);

void __fastcall Look(void* cam, void* edx, float dt, void* a, void* b, float* out) {
    g_origLook(cam, edx, dt, a, b, out);
    if (!cam || !out || At<char>(cam, kMouseMode) || At<char>(cam, kHeld)) return;
    unsigned char* set = At<unsigned char*>(cam, kSettings);
    if (!set) return;

    // (the deadzone is in already - Sticks)
    float sx = At<float>(cam, kStickX), sy = At<float>(cam, kStickY);
    float m = sqrtf(sx * sx + sy * sy);
    float game[2] = {out[0], out[1]};
    Ramp& ramp = RampOf(cam);

    float t = m;
    if (g_set.bypass) {
        ramp.held = 0;
    } else if (t <= 0) {
        out[0] = out[1] = 0;
        ramp.held = 0;
    } else {
        if (t > 1) t = 1;
        float c = powf(t, g_set.curve);   // the curve on the length, the direction kept
        float dx = sx / m, dy = sy / m;   // (m > 0 here)

        // The edge ramp: held at the edge a while, the speed eases up to the boost.
        if (t >= g_set.rampEdge && dt > 0 && dt < 0.5f) ramp.held += dt;
        else ramp.held = 0;
        float k = g_set.rampTime > 0 ? Smoothstep((ramp.held - g_set.rampDelay) / g_set.rampTime)
                                     : (ramp.held > g_set.rampDelay ? 1.0f : 0.0f);
        float boostYaw = 1 + (g_set.rampBoost - 1) * k, boostPitch = 1 + (g_set.rampPitch - 1) * k;

        // The game's top speed at full push: its polynomial at 1, its settings' top speed, the player's sensitivity.
        float full = At<float>(set, kCurveA) + At<float>(set, kCurveB) + At<float>(set, kCurveC);
        float sens = reinterpret_cast<Sens_t>(kSensSpeed)(cam);
        float yawTop = At<float>(set, kYawTop) * sens * full * g_set.speed;
        float pitchTop = At<float>(set, kPitchTop) * sens * full * g_set.speed * g_set.pitch;
        out[1] = dx * c * yawTop * boostYaw;
        out[0] = dy * c * pitchTop * boostPitch;
    }

    if (g_set.log) {   // (research: the game's speeds against ours, a few times a second while the stick moves)
        static DWORD last = 0;
        static float lastYaw = 0;
        DWORD now = GetTickCount();
        float yaw = At<float>(cam, kOrbitYaw);
        if (m > 0.01f && now - last >= 250) {
            float turned = yaw - lastYaw;   // (since the last line: how fast the view really turned)
            while (turned > 3.14159265f) turned -= 6.2831853f;
            while (turned < -3.14159265f) turned += 6.2831853f;
            Logf(L"cam %p dt %.4f stick %.3f,%.3f (|%.3f|) game %.3f,%.3f ours %.3f,%.3f ramp %.2f s, turned %.0f deg/s",
                 cam, dt, sx, sy, m, game[1], game[0], out[1], out[0], ramp.held,
                 now - last < 400 ? turned * 57.29578f * 1000.0f / (now - last) : 0.0f);
            last = now;
            lastYaw = yaw;
        }
        static void* seen[8];
        bool known = false;
        for (void*& s : seen)
            if (s == set) { known = true; break; }
        if (!known) {
            for (void*& s : seen)
                if (!s) { s = set; break; }
            Logf(L"camera %p settings %p: curve %.3f %.3f %.3f, top yaw %.3f pitch %.3f, accel yaw %.3f pitch %.3f, "
                 L"sensitivity factor %.3f", cam, set, At<float>(set, kCurveA), At<float>(set, kCurveB),
                 At<float>(set, kCurveC), At<float>(set, kYawTop), At<float>(set, kPitchTop), At<float>(set, 0x78),
                 At<float>(set, 0x80), reinterpret_cast<Sens_t>(kSensSpeed)(cam));
        }
    }
    Trace(cam, m, dt);
}

// ---- research: a frame-by-frame trace (mod.ini Trace=<seconds>) ---------------------------------------------------
// Pushing the right stick past half (after it rested a second) records, for that many seconds, every camera update
// ('L': the game thread, its dt) and every frame drawn ('F': the render thread, after Present): the time, the camera's
// yaw / pitch and eye, the first player's place. Then trace_<n>.csv in the mod's folder.
struct TraceRow { char kind; LONGLONG at; float dt, yaw, pitch, eye[3], pos[3]; };
const int kTraceRows = 8192;
TraceRow g_trace[kTraceRows];
volatile LONG g_traceN = 0;
void* volatile g_traceCam = nullptr;
LONGLONG g_traceFrom = 0, g_traceUntil = 0, g_restSince = 0;
int g_traceFiles = 0;

LONGLONG Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
LONGLONG QpcFreq() { static LONGLONG f = 0; if (!f) { LARGE_INTEGER t; QueryPerformanceFrequency(&t); f = t.QuadPart; } return f; }

void TraceAdd(char kind, void* cam, float dt) {
    LONG i = InterlockedIncrement(&g_traceN) - 1;
    if (i >= kTraceRows) return;
    TraceRow& r = g_trace[i];
    r.kind = kind;
    r.at = Qpc();
    r.dt = dt;
    r.yaw = At<float>(cam, kOrbitYaw);
    r.pitch = At<float>(cam, kOrbitYaw + 4);
    for (int k = 0; k < 3; ++k) r.eye[k] = At<float>(cam, kOrbitYaw + 0xc + k * 4);
    void* players[8];
    const float* xf = nullptr;
    if (g_api->GetPlayers(players, 8) > 0 && players[0]) xf = At<const float*>(players[0], 0x38);
    for (int k = 0; k < 3; ++k) r.pos[k] = xf ? xf[12 + k] : 0;
}

void TraceWrite() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\trace_%d.csv", g_api->modFolder, ++g_traceFiles);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") || !f) return;
    fprintf(f, "kind,ms,dt,yaw,pitch,eye_x,eye_y,eye_z,pos_x,pos_y,pos_z\n");
    LONG n = g_traceN < kTraceRows ? g_traceN : kTraceRows;
    for (LONG i = 0; i < n; ++i) {
        const TraceRow& r = g_trace[i];
        fprintf(f, "%c,%.3f,%.5f,%.6f,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", r.kind,
                (r.at - g_traceFrom) * 1000.0 / QpcFreq(), r.dt, r.yaw, r.pitch, r.eye[0], r.eye[1], r.eye[2], r.pos[0],
                r.pos[1], r.pos[2]);
    }
    fclose(f);
    Logf(L"trace: %ld rows -> %s", n, path);
}

void Trace(void* cam, float m, float dt) {
    if (g_set.trace <= 0) return;
    LONGLONG now = Qpc();
    if (g_traceCam) {
        if (cam != g_traceCam) return;
        TraceAdd('L', cam, dt);
        if (now >= g_traceUntil) {
            g_traceCam = nullptr;
            TraceWrite();
            g_restSince = 0;
        }
        return;
    }
    if (m < 0.1f) {
        if (!g_restSince) g_restSince = now;
    } else if (m > 0.5f && g_restSince && now - g_restSince > QpcFreq()) {
        g_traceN = 0;
        g_traceFrom = now;
        g_traceUntil = now + (LONGLONG)(g_set.trace * QpcFreq());
        g_traceCam = cam;
        TraceAdd('L', cam, dt);
    } else if (m >= 0.1f) {
        g_restSince = 0;
    }
}

void TraceFrame(void*) {
    if (void* cam = g_traceCam) TraceAdd('F', cam, 0);
}

}  // namespace

extern "C" __declspec(dllexport) int FGUYModInit(const FGUYModAPI* api) {
    if (!api || api->version < 1) return 0;
    g_api = api;
    ReadSettings();
    // (Sticks starts push ebp / mov ebp,esp / push ecx / push edi, Look sub esp,1c / push esi / push edi: the jumps
    //  take whole instructions)
    if (!api->Hook(reinterpret_cast<void*>(kSticks), 0, reinterpret_cast<void*>(&Sticks),
                   reinterpret_cast<void**>(&g_origSticks))) {
        api->Log(L"couldn't hook the controller's sticks (0x4b4ec0) - not this game's version?");
        return 0;
    }
    if (!api->Hook(reinterpret_cast<void*>(kLook), 0, reinterpret_cast<void*>(&Look),
                   reinterpret_cast<void**>(&g_origLook))) {
        api->Log(L"couldn't hook the camera's stick look (0x581d40) - not this game's version?");
        return 0;
    }
    if (g_set.trace > 0) api->OnFrame(TraceFrame, nullptr);
    Logf(L"on: deadzone %.0f%% / %.0f%%, curve %.2f, speed %.0f%% (pitch %.0f%%), ramp after %.0f ms over %.0f ms to "
         L"%.0f%% (pitch %.0f%%)", g_set.inner * 100, g_set.outer * 100, g_set.curve, g_set.speed * 100,
         g_set.pitch * 100, g_set.rampDelay * 1000, g_set.rampTime * 1000, g_set.rampBoost * 100,
         g_set.rampPitch * 100);
    return 1;
}
