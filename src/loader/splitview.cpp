// One full-screen view per PC instead of split screen (online play).
//
// The game lays out split screen in 0x640950(count, mode): for each viewport i it queues render-thread
// commands on that viewport's two render cameras (at 0xb5a9b0 + i*0x3c70, and +0x2c0):
//   * set rectangle: 0x4db0b0 queues 0x403480(camera, l, t, r, b)  - normalized edges, at +0x34..+0x40
//   * enable:        0x4d1860 queues 0x476390(camera, on)          - stored at +0x1e
// Both commands keep the function's address as a constant in the queuing code, so we point those two
// constants at our own functions. On the render thread we then give the local player's viewport the
// whole screen (0,0,1,1) and squeeze the other viewports into one corner pixel. Only drawing changes: every
// player's camera, input and game state keep running identically on every PC, so lockstep is unaffected.
// (Switching the other cameras off instead broke that: the game updates a camera - and the aim that follows
// it - only while it's on.)
//
// Better still, the other views aren't drawn at all: the render thread's frame (0x44e0f0) goes through its list
// of cameras twice - the scene, then the effects pass - and for the other players' viewport cameras we jump
// straight to the next one (DrawSkip). Drawing never changes the game: their cameras are still updated.
// [Netplay] DrawOtherViews=1 draws them squeezed instead.
//
// The HUD (one Scaleform movie for all players) follows along: tools/ui/make_hud_patch.py makes it lay out
// as single player with my viewport as its viewport 0, and the game's own HUD code, which writes values
// straight into HUD objects by path ("_root.Icon_<name>_<viewport>_<layout>_<player>", built in
// 0x97b360), gets our copy of that path builder with the same translation.
//
// Which viewport is "mine": the viewport of the player with my netplay slot (player +0xdf8 = player
// index, +0xdfc = viewport index). Test knob: env FGUYMOD_FULLVIEW=<viewport> forces it without netplay
// (local split screen with a second virtual pad).

#include "common.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

const uintptr_t kViewports = 0xb5a9b0, kViewportSize = 0x3c70, kSecondCamera = 0x2c0;
const uintptr_t kViewportCount = 0xb53e60;
const uintptr_t kRectImm = 0x4db0e3, kRectFn = 0x403480;      // imm32 inside "mov [eax+1C], 0x403480"
const uintptr_t kEnableImm = 0x4d188d, kEnableFn = 0x476390;  // imm32 inside "mov [eax+10], 0x476390"
const int kMaxViewports = 4;
bool g_menuWatch = false;   // (research: log the menus' view 0 changes once the ONLINE screens have been used)

using Rect_t = void(__thiscall*)(void* camera, float x, float y, float w, float h);
using Enable_t = void(__thiscall*)(void* camera, int on);
Rect_t Real_Rect = reinterpret_cast<Rect_t>(kRectFn);
Enable_t Real_Enable = reinterpret_cast<Enable_t>(kEnableFn);

int g_forced = -1;  // FGUYMOD_FULLVIEW
bool g_active = false;

int Count() { return *reinterpret_cast<volatile int*>(kViewportCount); }

// Viewport index of a render camera, or -1 if it isn't one of the split-screen cameras.
int ViewportOf(void* camera) {
    uintptr_t a = reinterpret_cast<uintptr_t>(camera);
    if (a < kViewports || a >= kViewports + kMaxViewports * kViewportSize) return -1;
    uintptr_t off = (a - kViewports) % kViewportSize;
    if (off != 0 && off != kSecondCamera) return -1;
    return (int)((a - kViewports) / kViewportSize);
}

void* Camera(int viewport, int which) {
    return reinterpret_cast<void*>(kViewports + viewport * kViewportSize + (which ? kSecondCamera : 0));
}

// My player's viewport (whether or not the game has set up that many viewports yet), -1 if unknown.
int MyPlayerViewport() {
    if (g_forced >= 0) return g_forced;
    int slot = NetplayLocalSlot();
    if (slot < 0) return -1;
    unsigned char* players[8];
    int n = GamePlayers(players, 8);  // the level's players: find the one with my slot's index
    for (int i = 0; i < n; ++i) {
        auto p = players[i];
        if (*reinterpret_cast<int*>(p + 0xdf8) == slot) {
            int vp = *reinterpret_cast<int*>(p + 0xdfc);
            if (vp >= 0 && vp < kMaxViewports) return vp;
        }
    }
    return -1;  // not in the level (yet): watch the host's view, with its HUD
}

// The viewport this PC should show full screen, or -1 for normal drawing (only one viewport).
int MyViewport() {
    int count = Count(), mine = MyPlayerViewport();
    return count > 1 && mine >= 0 && mine < count ? mine : -1;
}

// Spectating (SplitViewSpectateTick): dead in Multiverse Madness - nothing to do until the round is over - this PC
// shows a living teammate's view instead of mine (drawing, their name tags, what I hear; my HUD stays mine). Only
// what's drawn changes: every viewport's camera runs on every PC anyway.
volatile LONG g_spectate = -1;   // the viewport shown instead of mine (-1: mine)
volatile LONG g_spectateSlot = -1;
LONG g_shownApplied = -2;        // (render thread) the view the rectangles were last set for

// A cutscene playing (the game's current one, [0xbad99c], in its playing state +0xac 3): it drives the first view's
// camera - played on one screen that's the whole screen; online every PC shows that view meanwhile (the others showed
// their own player's - a cutscene played on the guest's PC unseen).
bool CutscenePlaying() {
    auto cin = *reinterpret_cast<unsigned char* volatile*>(0xbad99c);
    return cin && !IsBadReadPtr(cin + 0xac, 4) && *reinterpret_cast<int*>(cin + 0xac) == 3;
}

}  // namespace
bool SplitViewCutscenePlaying() { return CutscenePlaying(); }
namespace {

int ShownViewport() {
    int mine = MyViewport(), s = g_spectate;
    if (mine >= 0 && CutscenePlaying()) return 0;
    return mine >= 0 && s >= 0 && s < Count() ? s : mine;
}

// The other players' views: their cameras keep running (the game updates a player's camera - and with it
// where they aim - only while that view is on, so switching them off made each PC play them differently).
// They get exactly one pixel in the screen's corner, under my view's HUD (and normally aren't drawn at all -
// DrawSkip). Not more: a fixed 0.1% of the screen came out 2x1 at 1600x900 and 1920x1080, and the game's
// after-effects then asked for a 1x0 texture and crashed. And not the whole screen: not drawn or not, each
// camera still clears its part of the screen - mine would be black. One pixel also means the same camera shape
// on every PC, whatever its window size. (0.75 of a pixel, rounded up by the game to one.)
void OtherRect(void* camera) {
    int w = *reinterpret_cast<volatile int*>(0xaf2fbc), h = *reinterpret_cast<volatile int*>(0xaf2fb8);   // screen
    if (w < 16 || h < 16) w = h = 1000;
    Real_Rect(camera, 1.0f - 0.75f / w, 1.0f - 0.75f / h, 1.0f, 1.0f);
}
void SqueezeOthers(int mine) {
    int count = Count();
    for (int v = 0; v < count && v < kMaxViewports; ++v)
        if (v != mine)
            for (int k = 0; k < 2; ++k) OtherRect(Camera(v, k));
}

void __fastcall Hook_Rect(void* camera, void*, float x, float y, float w, float h) {
    int v = ViewportOf(camera), mine = v >= 0 ? MyViewport() : -1;
    if (mine < 0) {
        static int logged = 0;   // (research: the menus' own view rectangles - the main menu's black 3D)
        if (v == 0 && g_menuWatch && ++logged <= 30)
            Log(L"SplitView: the game sets view 0's rectangle %.3f %.3f %.3f %.3f", x, y, w, h);
        Real_Rect(camera, x, y, w, h);
        return;
    }
    static int lastMine = -2;
    if (mine != lastMine) {
        lastMine = mine;
        Log(L"SplitView: %d viewports - showing viewport %d full screen", Count(), mine);
    }
    int shown = ShownViewport();
    if (v == shown) Real_Rect(camera, 0, 0, 1, 1);
    else OtherRect(camera);
    SqueezeOthers(shown);
    g_shownApplied = shown;
}

void __fastcall Hook_Enable(void* camera, void*, int on) {
    static int logged = 0;
    if (ViewportOf(camera) == 0 && g_menuWatch && ++logged <= 30)
        Log(L"SplitView: the game turns view 0 %s", (on & 0xff) ? L"on" : L"off");
    Real_Enable(camera, on & 0xff);  // every view stays as the game wants it (see SqueezeOthers)
}

// ---- HUD object paths ---------------------------------------------------------------------------------
// bool __thiscall Hud::IconPath(int nameId, int player, char* out, int slot)   (0x97b360, ret 0x10)
// The original: find the player's viewport; none -> false. Then
//   slot < 0:        "_root.Icon_%s_%d_%d_%d"    name, viewport, layout ([hud+0x70]), player
//   name id 0:       "_root.Icon_%d_%d"          slot, player
//   otherwise:       "_root.Icon_%s%d_%d_%d_%d"  name, slot, viewport, layout, player
// (name = hud->vtable[0x78/4](nameId)). Online the HUD is the 4-way split screen's on every PC (see
// SplitViewHudMode), so ours writes every player's own objects - their real viewport, layout 4. The story's HUD
// has no 4-way layout: there it's the single-player one, with my viewport as its viewport 0 (layout 0).
const uintptr_t kIconPath = 0x97b360;
const int kHudLayout4Way = 4;   // the HUD's SPLIT_SCREEN_MODE_4WAY
bool g_hud4Way = true;          // the level's HUD has that layout (SplitViewHudHas4Way)
const unsigned char kIconPathStart[] = {0x53, 0x56, 0x8b, 0x35, 0x88, 0xd9, 0xba, 0x00};  // push ebx; push esi; mov esi,[0xbad988]
int SplitViewHudViewportImpl(int viewport);

bool __fastcall Hook_IconPath(void* hud, void*, int nameId, int player, char* out, int slot) {
    auto list = *reinterpret_cast<unsigned char**>(0xbad988);
    int count = *reinterpret_cast<int*>(list + 0x20);
    int vp = -1;
    for (int i = 0; i < count; ++i) {
        auto p = reinterpret_cast<unsigned char**>(list)[i];
        if (*reinterpret_cast<int*>(p + 0xdf8) == player && *reinterpret_cast<int*>(p + 0xdfc) != -1) {
            vp = *reinterpret_cast<int*>(p + 0xdfc);
            break;
        }
    }
    if (vp < 0) return false;
    vp = SplitViewHudViewportImpl(vp);
    if (vp < 0) return false;
    int layout = g_hud4Way ? kHudLayout4Way : 0;
    using Name_t = const char*(__thiscall*)(void* hud, int id);
    auto name = [&] { return reinterpret_cast<Name_t>((*reinterpret_cast<void***>(hud))[0x78 / 4])(hud, nameId); };
    if (slot < 0) sprintf_s(out, 128, "_root.Icon_%s_%d_%d_%d", name(), vp, layout, player);
    else if (nameId == 0) sprintf_s(out, 128, "_root.Icon_%d_%d", slot, player);
    else sprintf_s(out, 128, "_root.Icon_%s%d_%d_%d_%d", name(), slot, vp, layout, player);
    return true;
}

bool PatchJump(uintptr_t at, const unsigned char* expected, size_t n, void* to) {
    if (IsBadReadPtr((void*)at, n) || memcmp((void*)at, expected, n) != 0) {
        Log(L"SplitView: code at %p isn't what we expect (different game version?) - off", (void*)at);
        return false;
    }
    DWORD old;
    VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    auto b = reinterpret_cast<unsigned char*>(at);
    b[0] = 0xE9;
    *reinterpret_cast<int*>(b + 1) = (int)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return true;
}

// 0x6261c0 "is this a two-player vertical split" is used only by the camera, to narrow the field of view
// for a half-width view. Full screen wants the normal view, so it answers no. Every PC in a session
// patches it the same way, so the game stays identical on all of them.
const uintptr_t kIsVerticalSplit = 0x6261c0;
const unsigned char kIsVerticalSplitStart[] = {0x83, 0x3d, 0x60, 0x3e, 0xb5, 0x00, 0x02};  // cmp [0xb53e60], 2

// HUD markers (objective arrows, "PLAYER 2" tags): 0x97b880 turns a viewport position into HUD
// coordinates with an offset/scale per split layout, from the viewport count. Online the HUD is the 4-way
// one (zoomed into my quarter), so its count call answers 4 (display only).
const uintptr_t kMarkerCountCall = 0x97b885, kGetViewportCount = 0x625fe0;
bool g_markerFull = false;   // (my own view's markers: mapped as one full-screen view, see Hook_MarkerToHud)
int __cdecl MarkerViewportCount() { return g_markerFull ? 1 : MyViewport() >= 0 ? (g_hud4Way ? 4 : 1) : Count(); }

// The game makes those markers per viewing player (0x9909a0: find the viewer, project the point with their
// viewport's camera, then 0x97b880 maps it into HUD space). Full screen, the other viewers' markers would
// land on my screen too - my own name tag among them - so markers for any viewport but mine go off screen.
// 0x97b880: bool __thiscall (float* screenPoint, int viewport, float* hudOut)  ret 0xc; called once.
const uintptr_t kMarkerToHudCall = 0x990af7, kMarkerToHud = 0x97b880;
using MarkerToHud_t = void(__thiscall*)(void* hud, float* point, int viewport, float* out);
// My own view's markers (name tags over the others' heads): mapped as if mine were the only view - the whole
// screen, as in single player. (Mapped into my quarter of the 4-way layout they sat in the middle of the screen's
// top-left quarter, wherever the heads were: the markers' own layer isn't zoomed with the HUD.) The other views'
// markers go off screen.
volatile bool g_noMarkers = false;   // (my death menu's panel is up: no name tags over it - FGUYMOD_DeathPanel)
void __fastcall Hook_MarkerToHud(void* hud, void*, float* point, int viewport, float* out) {
    int mine = ShownViewport();   // (spectating: the shown view's name tags)
    if (g_noMarkers && mine >= 0 && viewport == mine) {
        reinterpret_cast<MarkerToHud_t>(kMarkerToHud)(hud, point, viewport, out);
        point[0] = point[1] = -10000.0f;
        if (out) out[0] = out[1] = -10000.0f;
        return;
    }
    if (mine >= 0 && viewport == mine) {
        g_markerFull = true;
        reinterpret_cast<MarkerToHud_t>(kMarkerToHud)(hud, point, viewport, out);
        g_markerFull = false;
        return;
    }
    reinterpret_cast<MarkerToHud_t>(kMarkerToHud)(hud, point, viewport, out);
    if (mine >= 0 && viewport != mine) {
        point[0] = point[1] = -10000.0f;
        if (out) out[0] = out[1] = -10000.0f;
    }
}

}  // namespace
void SplitViewNoMarkers(bool on) { g_noMarkers = on; }
namespace {

bool PatchCall(uintptr_t at, uintptr_t expectedTarget, void* to) {
    auto b = reinterpret_cast<unsigned char*>(at);
    if (IsBadReadPtr(b, 5) || b[0] != 0xE8 || at + 5 + *reinterpret_cast<int*>(b + 1) != expectedTarget) {
        Log(L"SplitView: call at %p isn't what we expect (different game version?) - off", (void*)at);
        return false;
    }
    DWORD old;
    VirtualProtect(b, 5, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<int*>(b + 1) = (int)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    VirtualProtect(b, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), b, 5);
    return true;
}

bool PatchBytes(uintptr_t at, const unsigned char* expected, size_t n, const unsigned char* with, size_t m) {
    if (IsBadReadPtr((void*)at, n) || memcmp((void*)at, expected, n) != 0) {
        Log(L"SplitView: code at %p isn't what we expect (different game version?) - off", (void*)at);
        return false;
    }
    DWORD old;
    VirtualProtect((void*)at, m, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, with, m);
    VirtualProtect((void*)at, m, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, m);
    return true;
}

bool PatchImm(uintptr_t at, uintptr_t expected, void* to) {
    auto p = reinterpret_cast<uintptr_t*>(at);
    if (IsBadReadPtr(p, 4) || *p != expected) {
        Log(L"SplitView: code at %p isn't what we expect (different game version?) - off", (void*)at);
        return false;
    }
    DWORD old;
    VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old);
    *p = reinterpret_cast<uintptr_t>(to);
    VirtualProtect(p, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    return true;
}

// ---- not drawing the other views (render thread) --------------------------------------------------------
// 0x44e327, scene pass, ecx = the camera:  mov ebx, ecx; mov edi, [ebx+290h]   -> next camera at 0x44eb0d
// 0x44ec4b, effects pass, edx = camera:    mov edi, edx; cmp dword [edi+8Ch], 1 (then jne at 0x44ec54)
//                                                                                -> next camera at 0x44ecae
const uintptr_t kScenePass = 0x44e327;   // back at 0x44e32f, next camera at 0x44eb0d (the stub)
const unsigned char kScenePassStart[] = {0x8b, 0xd9, 0x8b, 0xbb, 0x90, 0x02, 0x00, 0x00};
const uintptr_t kEffectsPass = 0x44ec4b;   // back at 0x44ec54, next camera at 0x44ecae (the stub)
const unsigned char kEffectsPassStart[] = {0x8b, 0xfa, 0x83, 0xbf, 0x8c, 0x00, 0x00, 0x00, 0x01};

bool __cdecl DrawSkip(void* camera) {
    int v = ViewportOf(camera);
    if (v < 0) return false;
    // Booting hidden (title, menus, the level loading - until its HUD is up): nobody sees these views, so no 3D
    // at all - the menus' movies still draw (they load while drawn). The same ticks on every PC (the HUD comes up
    // at the same tick everywhere), and drawing doesn't change the game anyway.
    if (HandoffBooting()) return true;
    int mine = MyViewport();
    if (mine < 0) return false;
    int shown = ShownViewport();
    if (shown != g_shownApplied) {   // (spectating began / ended: the shown view gets the screen now)
        g_shownApplied = shown;
        for (int k = 0; k < 2; ++k) Real_Rect(Camera(shown, k), 0, 0, 1, 1);
        SqueezeOthers(shown);
    }
    return v != shown;
}

__declspec(naked) void ScenePassStub() {
    __asm {
        pushad
        push ecx
        call DrawSkip
        add esp, 4
        test al, al
        popad                      // (leaves the flags alone)
        jnz skip
        mov ebx, ecx
        mov edi, dword ptr [ebx + 0x290]
        push 0x44e32f      // back
        ret
    skip:
        push 0x44eb0d      // the next camera
        ret
    }
}

__declspec(naked) void EffectsPassStub() {
    __asm {
        pushad
        push edx
        call DrawSkip
        add esp, 4
        test al, al
        popad
        jnz skip
        mov edi, edx
        cmp dword ptr [edi + 0x8c], 1
        push 0x44ec54      // back (push / ret leave the flags for its jne)
        ret
    skip:
        push 0x44ecae      // the next camera
        ret
    }
}

bool PatchJumpPadded(uintptr_t at, const unsigned char* expected, size_t n, void* to) {
    if (!PatchJump(at, expected, n, to)) return false;
    DWORD old;
    VirtualProtect((void*)(at + 5), n - 5, PAGE_EXECUTE_READWRITE, &old);
    memset((void*)(at + 5), 0x90, n - 5);   // (never run: the stub comes back after them)
    VirtualProtect((void*)(at + 5), n - 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
    return true;
}

// ---- my camera turns at once (online: the input delay) ---------------------------------------------------------
// Online my input reaches the game a little later (netplay.cpp's input delay - a quarter second on a long connection),
// and with it the camera's turn: the mouse felt like it dragged. The view on my screen doesn't wait: it's drawn turned
// already by what my inputs not played yet will turn it (netplay.cpp works that out every tick - SplitViewPredict),
// orbiting my player as the game's own camera does; the game's camera then gets there itself as those inputs play.
// Only the drawing changes - the game, its camera, my shots stay the match's.
// The render thread works out each camera's matrices from its place and turn at +0x94 (3 x 4: the axes, the place
// last), which the game hands it every frame: 0x48db10 (thiscall, the camera), starting
//   push ebp; mov ebp, esp; mov eax, [ebp+8]
// We turn that +0x94 first, for my view's two cameras. The game's camera never looks up or down (it stays level: the
// reticle moves instead), so the turn is about the up axis only.
const uintptr_t kCamUpdate = 0x48db10;   // back at 0x48db16
const unsigned char kCamUpdateStart[] = {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08};
volatile LONG g_predOn = 0;
float g_predTurn = 0, g_predX = 0, g_predZ = 0;   // radians still to come, around my player at (x, z)
SRWLOCK g_predLock = SRWLOCK_INIT;
struct Predicted { float base[12], out[12]; bool have; };
Predicted g_predicted[2];

void __cdecl PredictCamera(void* camera) {
    if (!g_predOn) return;
    int v = ShownViewport();
    if (v < 0 || v != MyViewport()) return;   // (spectating: their view as it is)
    int k = camera == Camera(v, 0) ? 0 : camera == Camera(v, 1) ? 1 : -1;
    if (k < 0) return;
    float* m = reinterpret_cast<float*>(static_cast<unsigned char*>(camera) + 0x94);
    Predicted& p = g_predicted[k];
    // How far off the model is (netplay.cpp's: measured at one frame rate - the game's turn per count isn't the same at
    // every one): each new place and turn from the game against the model's turn of the inputs played since the last.
    static float gain = 1, seen = 0, modelled = 0, lastAngle = 0;
    static unsigned lastTick = 0;
    if (!p.have || memcmp(m, p.out, sizeof(p.out)) != 0) {   // (a new one: the game's)
        memcpy(p.base, m, sizeof(p.base));
        if (k == 0) {
            float angle = std::atan2(p.base[2], p.base[0]);
            unsigned t = NetplayShownTick();
            if (lastTick && t > lastTick && t - lastTick < 32) {
                float model = NetplayModelTurn(lastTick, t), turned = angle - lastAngle;
                while (turned > 3.14159265f) turned -= 6.2831853f;
                while (turned < -3.14159265f) turned += 6.2831853f;
                if (std::fabs(model) > 0.003f && std::fabs(turned) < 1.0f) {
                    seen = seen * 0.97f + turned;
                    modelled = modelled * 0.97f + model;
                    if (std::fabs(modelled) > 0.05f) {
                        float g = seen / modelled;
                        gain = g < 0.2f ? 0.2f : g > 3.0f ? 3.0f : g;
                    }
                }
            }
            lastAngle = angle;
            lastTick = t;
        }
    }
    AcquireSRWLockShared(&g_predLock);
    float turn = g_predTurn * gain, px = g_predX, pz = g_predZ;
    ReleaseSRWLockShared(&g_predLock);
    memcpy(p.out, p.base, sizeof(p.out));
    if (turn != 0) {   // each axis's (x, z), and the place's around my player, turned by `turn`
        float c = std::cos(turn), s = std::sin(turn);
        for (int a = 0; a < 3; ++a) {
            float x = p.base[a], z = p.base[8 + a];
            p.out[a] = x * c + z * s;
            p.out[8 + a] = -x * s + z * c;
        }
        float x = p.base[3] - px, z = p.base[11] - pz;
        p.out[3] = px + x * c + z * s;
        p.out[11] = pz - x * s + z * c;
    }
    memcpy(m, p.out, sizeof(p.out));
    p.have = true;
    static int logged = 0;   // (the first turns: the game's angle and the one drawn, degrees)
    static float lastTurn = 0;
    if (k == 0 && std::fabs(turn - lastTurn) > 0.005f && logged < 12) {
        ++logged;
        Log(L"SplitView: view drawn turned %.2f degrees ahead (the game's camera at %.2f, drawn at %.2f; model x %.2f)", turn * 57.2958f,
            std::atan2(p.base[2], p.base[0]) * 57.2958f, std::atan2(p.out[2], p.out[0]) * 57.2958f, gain);
    }
    if (k == 0) lastTurn = turn;
}

__declspec(naked) void CamUpdateStub() {
    __asm {
        pushad
        push ecx
        call PredictCamera
        add esp, 4
        popad
        push ebp
        mov ebp, esp
        mov eax, dword ptr [ebp + 8]
        push 0x48db16      // back
        ret
    }
}

// ---- players' names ------------------------------------------------------------------------------------------
// On PC players have no gamertags (0x5549b0 "has a name" is a stub that says no), so the game names them
// sprintf(buf, "=MNU_PLAYER%d", player + 1) - "PLAYER 2" over their heads and in the results. Online those two
// calls write the player's lobby name instead (player index = slot online).
const uintptr_t kNameCall1 = 0x5a70ea, kNameCall2 = 0x5baca9, kSprintf = 0x9c34d6;
using Sprintf_t = int(__cdecl*)(char* buf, const char* fmt, ...);

int __cdecl PlayerNameText(char* buf, const char* fmt, int number) {
    const wchar_t* name = NetplayLobbyName(number - 1);
    if (name && name[0]) {
        int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, 40, nullptr, nullptr);
        if (n > 0) return n - 1;
    }
    return reinterpret_cast<Sprintf_t>(kSprintf)(buf, fmt, number);
}

}  // namespace

// The HUD (tools/ui/make_hud_patch.py) asks these before laying itself out. Online it's the 4-way split
// screen's HUD on every PC, with every player's real viewport: each player's menus (death menu: continue /
// change character / weapons -> respawn) and screens run on every PC, as in split screen - they're part of
// the game (shown only on the player's own PC, they made the match go out of sync). Each PC then shows only
// its own quarter, zoomed to the whole screen (a quarter of the screen has the screen's shape:
// SplitViewHudZoomViewport, pause.as2js).
//
// The story's HUD (hudCampaign.gfx) has no 4-way layout - with it forced there, nothing showed at all - and no
// death menu, so there it's the single-player layout (0) with my viewport as its viewport 0 and nobody else's,
// unzoomed (as the online HUD was before the 4-way one).
int SplitViewHudMode(int mode) { return g_active ? (g_hud4Way ? 4 : 0) : mode; }
void SplitViewHudHas4Way(bool has) {
    if (has != g_hud4Way) Log(L"SplitView: the level's HUD %s", has ? L"has the 4-way layout" : L"has no 4-way layout - the single-player one, mine");
    g_hud4Way = has;
}
namespace {
int SplitViewHudViewportImpl(int viewport) { return SplitViewHudViewport(viewport); }
}
int SplitViewHudViewport(int viewport) {
    if (!g_active || g_hud4Way || viewport < 0) return viewport;
    int mine = MyPlayerViewport();
    if (mine < 0) return viewport;
    return viewport == mine ? 0 : -1;
}
// The quarter of the HUD to show full screen (my viewport), -1: the HUD as it is.
int SplitViewHudZoomViewport() { return g_active && g_hud4Way ? MyPlayerViewport() : -1; }
// My player's viewport online (the one this PC shows full screen - audio.cpp's listener), -1: offline / not yet.
int SplitViewMyViewport() { return g_active ? MyPlayerViewport() : -1; }
int SplitViewShownViewport() { return g_active && MyViewport() >= 0 ? ShownViewport() : SplitViewMyViewport(); }

// netplay.cpp, every tick of a session (the game thread): spectate when dead in Multiverse Madness - 3 s after my
// player went down, the first teammate still standing; mine again once I'm back (or SETTINGS > Spectate off).
void SplitViewSpectateTick(int rate) {
    static int deadTicks = 0;
    int want = -1, wantSlot = -1, slot = NetplayLocalSlot();
    const char* cfg = SessionConfig();
    bool horde = cfg && strncmp(cfg, "HD,", 3) == 0;
    unsigned char* players[8];
    int n = horde && slot >= 0 && OnlineSpectate() ? GamePlayers(players, 8) : 0;
    unsigned char* me = nullptr;
    for (int i = 0; i < n; ++i)
        if (*reinterpret_cast<int*>(players[i] + 0xdf8) == slot) me = players[i];
    deadTicks = me && NetplayPlayerHealth(me) <= 0.0f ? deadTicks + 1 : 0;
    if (me && deadTicks > 3 * (rate > 0 ? rate : 60)) {
        for (int i = 0; i < n && want < 0; ++i) {
            int s = *reinterpret_cast<int*>(players[i] + 0xdf8), vp = *reinterpret_cast<int*>(players[i] + 0xdfc);
            if (players[i] != me && s >= 0 && s < 4 && vp >= 0 && vp < kMaxViewports && NetplayPlayerHealth(players[i]) > 0.0f &&
                NetplayLobbyName(s)[0]) {
                want = vp;
                wantSlot = s;
            }
        }
    }
    if (want != g_spectate) {
        if (want >= 0) Log(L"SplitView: spectating player %d (viewport %d) - I'm down", wantSlot, want);
        else Log(L"SplitView: my own view again");
        InterlockedExchange(&g_spectateSlot, wantSlot);
        InterlockedExchange(&g_spectate, want);
    }
}

// FGUYMOD_Spectating (the HUD): the name of the player whose view this PC shows ("" mine).
const wchar_t* SplitViewSpectating() {
    int s = g_spectateSlot;
    return g_spectate >= 0 && s >= 0 ? NetplayLobbyName(s) : L"";
}

// The online lobby shows the menu scene's 3D character (the Store's model) inside its centre panel: it
// draws viewport 0 into that part of the screen (the camera's rectangle is left, top, right, bottom as
// fractions of the screen; the lobby keeps the screen's shape, so the picture isn't stretched) and back to
// the whole screen when it closes. The rest of the screen is covered by the lobby.
void SplitViewMenuRect(float left, float top, float right, float bottom) {
    g_menuWatch = true;
    for (int k = 0; k < 2; ++k) Real_Rect(Camera(0, k), left, top, right, bottom);
}

// Leaving the ONLINE screens (online.cpp): how the menus' view 0 stands a moment later (research: the main menu's
// 3D went black after going in and out quickly with Esc) - and if it's still the lobby's panel, the whole screen.
void SplitViewMenuCheck() {
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        for (int pass = 0; pass < 2; ++pass) {
            Sleep(pass ? 2500 : 600);
            auto c = static_cast<unsigned char*>(Camera(0, 0));
            const float* r = reinterpret_cast<const float*>(c + 0x34);
            Log(L"SplitView: after ONLINE (%.1f s): view 0 %.3f %.3f %.3f %.3f, %s", pass ? 3.1 : 0.6, r[0], r[1], r[2],
                r[3], c[0x1e] ? L"on" : L"off");
            if (!pass || NetplayEnabled()) continue;
            if (r[0] != 0 || r[1] != 0 || r[2] != 1 || r[3] != 1) {
                Log(L"SplitView: view 0 still in the lobby's panel - the whole screen again");
                for (int k = 0; k < 2; ++k) Real_Rect(Camera(0, k), 0, 0, 1, 1);
            }
            for (int k = 0; k < 2; ++k) {   // (the camera's own switch, 0x476390: +0x1e, and +0x294 "changed")
                auto cam = static_cast<unsigned char*>(Camera(0, k));
                if (cam[0x1e]) continue;
                Log(L"SplitView: view 0 (camera %d) left off - on again", k);
                cam[0x1e] = 1;
                if (*reinterpret_cast<int*>(cam + 0x290)) cam[0x294] = 1;
            }
        }
        return 0;
    }, nullptr, 0, nullptr));
}

// netplay.cpp, every tick: how far my view still turns from my inputs not played yet (radians - 0, or `on` false:
// as it is), around my player at (x, z).
void SplitViewPredict(bool on, float turn, float x, float z) {
    AcquireSRWLockExclusive(&g_predLock);
    g_predTurn = on ? turn : 0.0f;
    g_predX = x;
    g_predZ = z;
    ReleaseSRWLockExclusive(&g_predLock);
    InterlockedExchange(&g_predOn, on ? 1 : 0);
}

// FGUYMOD_MenuCamera: where the menus' 3D camera (view 0's) is - "x|y|z". The ONLINE screens check it's the Store's
// (sometimes the Store camera didn't take: the lobby's panel showed the empty living room).
const char* SplitViewMenuCamera() {
    static char buf[64];
    auto c = static_cast<const float*>(Camera(0, 0));
    const float* pos = reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(c) + 0x200);
    sprintf_s(buf, "%.3f|%.3f|%.3f", pos[0], pos[1], pos[2]);
    static float last[3] = {1e9f, 1e9f, 1e9f};
    static int logged = 0;
    float d = std::fabs(pos[0] - last[0]) + std::fabs(pos[1] - last[1]) + std::fabs(pos[2] - last[2]);
    if (d > 0.5f && logged < 60) {
        ++logged;
        Log(L"SplitView: menu camera at %S", buf);
        memcpy(last, pos, sizeof(last));
    }
    return buf;
}

void InitSplitView() {
    wchar_t env[16];
    if (GetEnvironmentVariableW(L"FGUYMOD_FULLVIEW", env, 16)) g_forced = _wtoi(env);
    bool online = NetplayEnabled() && SettingInt(L"Netplay", L"FullScreenView", L"FGUYMOD_FULLSCREENVIEW", 1) != 0;
    if (!online && g_forced < 0) return;
    if (PatchImm(kRectImm, kRectFn, (void*)Hook_Rect) && PatchImm(kEnableImm, kEnableFn, (void*)Hook_Enable) &&
        PatchJump(kIconPath, kIconPathStart, sizeof(kIconPathStart), (void*)Hook_IconPath)) {
        static const unsigned char no[] = {0x31, 0xc0, 0xc3};  // xor eax, eax; ret
        PatchBytes(kIsVerticalSplit, kIsVerticalSplitStart, sizeof(kIsVerticalSplitStart), no, sizeof(no));
        PatchCall(kMarkerCountCall, kGetViewportCount, (void*)MarkerViewportCount);
        PatchCall(kMarkerToHudCall, kMarkerToHud, (void*)Hook_MarkerToHud);
        PatchCall(kNameCall1, kSprintf, (void*)PlayerNameText);   // players' names: their lobby names
        PatchCall(kNameCall2, kSprintf, (void*)PlayerNameText);
        g_active = true;
        Log(L"SplitView: on - one full-screen view per PC%s", g_forced >= 0 ? L" (test: forced viewport)" : L"");
        if (SettingInt(L"Netplay", L"DrawOtherViews", nullptr, 0) == 0 &&
            PatchJumpPadded(kScenePass, kScenePassStart, sizeof(kScenePassStart), (void*)ScenePassStub) &&
            PatchJumpPadded(kEffectsPass, kEffectsPassStart, sizeof(kEffectsPassStart), (void*)EffectsPassStub))
            Log(L"SplitView: the other players' views aren't drawn");
        // ([Online] InstantCamera=0: my view turns only as the match's camera does)
        if (online && SettingInt(L"Online", L"InstantCamera", L"FGUYMOD_INSTANTCAMERA", 1) != 0 &&
            PatchJumpPadded(kCamUpdate, kCamUpdateStart, sizeof(kCamUpdateStart), (void*)CamUpdateStub))
            Log(L"SplitView: my view turns at once (not after the input delay)");
    }
}
