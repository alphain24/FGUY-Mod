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

// The other players' views: their cameras keep running (the game updates a player's camera - and with it
// where they aim - only while that view is on, so switching them off made each PC play them differently).
// They're just drawn into a single pixel in the screen's corner, under my view's HUD.
void SqueezeOthers(int mine) {
    int count = Count();
    for (int v = 0; v < count && v < kMaxViewports; ++v)
        if (v != mine)
            for (int k = 0; k < 2; ++k) Real_Rect(Camera(v, k), 0.999f, 0.999f, 1.0f, 1.0f);
}

void __fastcall Hook_Rect(void* camera, void*, float x, float y, float w, float h) {
    int v = ViewportOf(camera), mine = v >= 0 ? MyViewport() : -1;
    if (mine < 0) {
        Real_Rect(camera, x, y, w, h);
        return;
    }
    static int lastMine = -2;
    if (mine != lastMine) {
        lastMine = mine;
        Log(L"SplitView: %d viewports - showing viewport %d full screen", Count(), mine);
    }
    if (v == mine) Real_Rect(camera, 0, 0, 1, 1);
    else Real_Rect(camera, 0.999f, 0.999f, 1.0f, 1.0f);
    SqueezeOthers(mine);
}

void __fastcall Hook_Enable(void* camera, void*, int on) {
    Real_Enable(camera, on & 0xff);  // every view stays as the game wants it (see SqueezeOthers)
}

// ---- HUD object paths ---------------------------------------------------------------------------------
// bool __thiscall Hud::IconPath(int nameId, int player, char* out, int slot)   (0x97b360, ret 0x10)
// The original: find the player's viewport; none -> false. Then
//   slot < 0:        "_root.Icon_%s_%d_%d_%d"    name, viewport, layout ([hud+0x70]), player
//   name id 0:       "_root.Icon_%d_%d"          slot, player
//   otherwise:       "_root.Icon_%s%d_%d_%d_%d"  name, slot, viewport, layout, player
// (name = hud->vtable[0x78/4](nameId)). Ours writes viewport 0 / layout 0 for my player, and no path for
// the others (their HUD isn't shown).
const uintptr_t kIconPath = 0x97b360;
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
    int layout = 0;
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
// coordinates with an offset/scale per split layout, from the viewport count. Full screen = one viewport,
// so its count call answers 1 (display only).
const uintptr_t kMarkerCountCall = 0x97b885, kGetViewportCount = 0x625fe0;
int __cdecl MarkerViewportCount() { return MyViewport() >= 0 ? 1 : Count(); }

// The game makes those markers per viewing player (0x9909a0: find the viewer, project the point with their
// viewport's camera, then 0x97b880 maps it into HUD space). Full screen, the other viewers' markers would
// land on my screen too - my own name tag among them - so markers for any viewport but mine go off screen.
// 0x97b880: bool __thiscall (float* screenPoint, int viewport, float* hudOut)  ret 0xc; called once.
const uintptr_t kMarkerToHudCall = 0x990af7, kMarkerToHud = 0x97b880;
using MarkerToHud_t = void(__thiscall*)(void* hud, float* point, int viewport, float* out);
void __fastcall Hook_MarkerToHud(void* hud, void*, float* point, int viewport, float* out) {
    reinterpret_cast<MarkerToHud_t>(kMarkerToHud)(hud, point, viewport, out);
    int mine = MyViewport();
    if (mine >= 0 && viewport != mine) {
        point[0] = point[1] = -10000.0f;
        out[0] = out[1] = -10000.0f;
    }
}

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
    int mine = MyViewport();
    return mine >= 0 && v != mine;
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

}  // namespace

// The HUD (tools/ui/make_hud_patch.py) asks these before laying itself out.
int SplitViewHudMode(int mode) { return g_active ? 0 : mode; }
namespace {
int SplitViewHudViewportImpl(int viewport) { return SplitViewHudViewport(viewport); }
}
int SplitViewHudViewport(int viewport) {
    if (!g_active || viewport < 0) return viewport;
    int mine = MyPlayerViewport();
    if (mine < 0) return viewport;
    return viewport == mine ? 0 : -1;
}

// The online lobby shows the menu scene's 3D character (the Store's model) inside its centre panel: it
// draws viewport 0 into that part of the screen (the camera's rectangle is left, top, right, bottom as
// fractions of the screen; the lobby keeps the screen's shape, so the picture isn't stretched) and back to
// the whole screen when it closes. The rest of the screen is covered by the lobby.
void SplitViewMenuRect(float left, float top, float right, float bottom) {
    for (int k = 0; k < 2; ++k) Real_Rect(Camera(0, k), left, top, right, bottom);
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
        g_active = true;
        Log(L"SplitView: on - one full-screen view per PC%s", g_forced >= 0 ? L" (test: forced viewport)" : L"");
        if (SettingInt(L"Netplay", L"DrawOtherViews", nullptr, 0) == 0 &&
            PatchJumpPadded(kScenePass, kScenePassStart, sizeof(kScenePassStart), (void*)ScenePassStub) &&
            PatchJumpPadded(kEffectsPass, kEffectsPassStart, sizeof(kEffectsPassStart), (void*)EffectsPassStub))
            Log(L"SplitView: the other players' views aren't drawn");
    }
}
