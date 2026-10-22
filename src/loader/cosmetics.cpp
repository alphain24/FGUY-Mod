// Cosmetics (skins): the game's own textures, changed in place - a skin pack's textures go into the Direct3D textures the
// game already made for them (so every model using one shows it, menus too; nothing on disk changes).
//
// Finding a texture: its asset (a Texture asset - the id the Skin Studio saves) is a node in the scene's asset tree
// ([0xb07b04]: a node holds the id at +0/+4, its children at +0x20 / (+0x24 & ~3), its object at +0x3c); the object is a
// World::TextureResourceEntity (vtable 0x9fb2b8), its [+0x28] +0x14 the IDirect3DTexture9.
#include "common.h"
#include <d3d9.h>
#include <string>
#include <vector>

namespace {

const uintptr_t kAssetTree = 0xb07b04;
const ULONG_PTR kTextureEntityVtable = 0x9fb2b8;

unsigned char* FindAsset(unsigned lo, unsigned hi) {
    auto root = *reinterpret_cast<unsigned char* volatile*>(kAssetTree);
    std::vector<unsigned char*> stack{root};
    int seen = 0;
    while (!stack.empty() && seen < 400000) {
        unsigned char* n = stack.back();
        stack.pop_back();
        if (!n || IsBadReadPtr(n, 0x40)) continue;
        ++seen;
        if (*reinterpret_cast<unsigned*>(n) == lo && *reinterpret_cast<unsigned*>(n + 4) == hi)
            return *reinterpret_cast<unsigned char**>(n + 0x3c);
        stack.push_back(*reinterpret_cast<unsigned char**>(n + 0x20));
        stack.push_back(reinterpret_cast<unsigned char*>(*reinterpret_cast<uintptr_t*>(n + 0x24) & ~3u));
    }
    return nullptr;
}

IDirect3DTexture9* TextureOf(unsigned lo, unsigned hi) {
    unsigned char* e = FindAsset(lo, hi);
    if (!e || IsBadReadPtr(e, 0x30) || *reinterpret_cast<ULONG_PTR*>(e) != kTextureEntityVtable) return nullptr;
    auto res = *reinterpret_cast<unsigned char**>(e + 0x28);
    if (!res || IsBadReadPtr(res + 0x14, 4)) return nullptr;
    auto base = *reinterpret_cast<IDirect3DBaseTexture9**>(res + 0x14);
    if (!base || IsBadReadPtr(base, 4)) return nullptr;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex)))) return nullptr;
    tex->Release();   // (the game's - not ours to keep alive)
    return tex;
}

// test pipe: "texinfo <lo> <hi>" / "texfill <lo> <hi> <rrggbb>" - done on the render thread (CosmeticsFrame)
struct TestCmd { bool fill; unsigned lo, hi, rgb; };
CRITICAL_SECTION g_lock;
bool g_lockInit = false;
std::vector<TestCmd> g_tests;

void Lock() {
    if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }
    EnterCriticalSection(&g_lock);
}

void Fill(IDirect3DTexture9* tex, unsigned rgb) {
    unsigned short c565 = (unsigned short)((((rgb >> 16) & 0xff) >> 3) << 11 | (((rgb >> 8) & 0xff) >> 2) << 5 | ((rgb & 0xff) >> 3));
    for (DWORD level = 0; level < tex->GetLevelCount(); ++level) {
        D3DSURFACE_DESC d;
        if (FAILED(tex->GetLevelDesc(level, &d))) break;
        D3DLOCKED_RECT lr;
        if (FAILED(tex->LockRect(level, &lr, nullptr, 0))) { Log(L"Cosmetics: test - couldn't lock level %lu", level); break; }
        unsigned bw = (d.Width + 3) / 4, bh = (d.Height + 3) / 4;
        for (unsigned y = 0; y < bh; ++y) {
            unsigned char* row = static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch;
            for (unsigned x = 0; x < bw; ++x) {
                if (d.Format == D3DFMT_DXT1) {
                    unsigned char* b = row + x * 8;
                    memcpy(b, &c565, 2); memcpy(b + 2, &c565, 2); memset(b + 4, 0, 4);
                } else if (d.Format == D3DFMT_DXT5 || d.Format == D3DFMT_DXT3) {
                    unsigned char* b = row + x * 16;
                    memset(b, 0xff, 8);   // (alpha: opaque - DXT5: endpoints 255 / 255)
                    memcpy(b + 8, &c565, 2); memcpy(b + 10, &c565, 2); memset(b + 12, 0, 4);
                }
            }
        }
        tex->UnlockRect(level);
    }
}

// ---- which player a draw is of (research) -----------------------------------------------------------------------
// The device's own calls, seen: the textures set (stage 0), the vertex shader constants (a shadow copy - a skinned
// character's bone matrices go there before its draw), the draws. "texdraws <lo> <hi>": for one frame, each draw with
// that texture - and which player's position the constants' matrix translations are near.
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetVSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using DrawIndexed_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
SetTexture_t Real_SetTexture;
SetVSConstF_t Real_SetVSConstF;
DrawIndexed_t Real_DrawIndexed;
IDirect3DBaseTexture9* g_stage0 = nullptr;
IDirect3DBaseTexture9* g_stages[8] = {};
int g_traceAllDraws = 0, g_traceStageHit[8] = {};
float g_vsConst[256][4];
volatile LONG g_traceArm = 0;          // (the next frame: trace)
bool g_tracing = false;
IDirect3DBaseTexture9* g_traceTex = nullptr;
int g_traceDraws = 0;
float g_tracePlayers[4][3];
int g_tracePlayerCount = 0;

HRESULT STDMETHODCALLTYPE Hook_SetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* tex) {
    if (stage == 0) g_stage0 = tex;
    if (stage < 8) g_stages[stage] = tex;
    return Real_SetTexture(self, stage, tex);
}
HRESULT STDMETHODCALLTYPE Hook_SetVSConstF(IDirect3DDevice9* self, UINT reg, const float* data, UINT count) {
    if (data && reg < 256) memcpy(g_vsConst[reg], data, sizeof(float) * 4 * min(count, 256 - reg));
    return Real_SetVSConstF(self, reg, data, count);
}
HRESULT STDMETHODCALLTYPE Hook_DrawIndexed(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base, UINT minIdx,
                                           UINT verts, UINT start, UINT prims) {
    if (g_tracing) {
        ++g_traceAllDraws;
        for (int st = 0; st < 8; ++st) if (g_stages[st] == g_traceTex) ++g_traceStageHit[st];
    }
    if (g_tracing && g_traceTex && (g_stages[0] == g_traceTex || g_stages[1] == g_traceTex || g_stages[2] == g_traceTex) && g_traceDraws < 40) {
        ++g_traceDraws;
        std::wstring hits;
        for (int r = 0; r + 2 < 256; ++r)   // (a 3x4 row-major matrix: translation in each row's w)
            for (int p = 0; p < g_tracePlayerCount; ++p) {
                float dx = g_vsConst[r][3] - g_tracePlayers[p][0], dy = g_vsConst[r + 1][3] - g_tracePlayers[p][1],
                      dz = g_vsConst[r + 2][3] - g_tracePlayers[p][2];
                if (dx * dx + dy * dy + dz * dz < 4.0f) {
                    wchar_t b[48];
                    swprintf_s(b, L" c%d~p%d", r, p);
                    hits += b;
                    r += 2;
                    break;
                }
            }
        Log(L"Cosmetics: trace - draw %d: %u vertices, %u triangles; near players:%s", g_traceDraws, verts, prims,
            hits.empty() ? L" none" : hits.c_str());
    }
    return Real_DrawIndexed(self, type, base, minIdx, verts, start, prims);
}

void* Patch(void* object, int slot, void* hook) {
    void** vtable = *reinterpret_cast<void***>(object);
    if (vtable[slot] == hook) return nullptr;
    DWORD old;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old);
    void* original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return original;
}

unsigned g_traceLo = 0, g_traceHi = 0;

}  // namespace

void CosmeticsDeviceCreated(IDirect3DDevice9* dev) {
    if (void* o = Patch(dev, 65, (void*)Hook_SetTexture)) Real_SetTexture = (SetTexture_t)o;
    if (void* o = Patch(dev, 94, (void*)Hook_SetVSConstF)) Real_SetVSConstF = (SetVSConstF_t)o;
    if (void* o = Patch(dev, 82, (void*)Hook_DrawIndexed)) Real_DrawIndexed = (DrawIndexed_t)o;
}

void CosmeticsTrace(unsigned lo, unsigned hi) {
    g_traceLo = lo; g_traceHi = hi;
    InterlockedExchange(&g_traceArm, 1);
}

void CosmeticsTest(bool fill, unsigned lo, unsigned hi, unsigned rgb) {
    Lock();
    g_tests.push_back({fill, lo, hi, rgb});
    LeaveCriticalSection(&g_lock);
}

// d3d9.cpp, the render thread after each Present
void CosmeticsFrame(IDirect3DDevice9* dev) {
    (void)dev;
    if (g_tracing) {   // (a frame traced: done)
        g_tracing = false;
        Log(L"Cosmetics: trace - %d draws with the texture this frame (%d draws in all; with it on stages 0-3: %d %d %d %d)",
            g_traceDraws, g_traceAllDraws, g_traceStageHit[0], g_traceStageHit[1], g_traceStageHit[2], g_traceStageHit[3]);
    }
    if (InterlockedExchange(&g_traceArm, 0)) {
        g_traceTex = TextureOf(g_traceLo, g_traceHi);
        unsigned char* players[4];
        g_tracePlayerCount = GamePlayers(players, 4);
        for (int p = 0; p < g_tracePlayerCount; ++p) {
            auto xf = *reinterpret_cast<unsigned char**>(players[p] + 0x38);
            if (xf) memcpy(g_tracePlayers[p], xf + 0x30, 12);
            Log(L"Cosmetics: trace - player %d at %.1f %.1f %.1f", p, g_tracePlayers[p][0], g_tracePlayers[p][1], g_tracePlayers[p][2]);
        }
        g_traceDraws = g_traceAllDraws = 0;
        memset(g_traceStageHit, 0, sizeof(g_traceStageHit));
        g_tracing = g_traceTex != nullptr;
        Log(L"Cosmetics: trace - the texture is %p", g_traceTex);
        if (!g_traceTex) Log(L"Cosmetics: trace - no texture %x:%x here", g_traceLo, g_traceHi);
    }
    if (!g_lockInit) return;
    std::vector<TestCmd> tests;
    Lock();
    tests.swap(g_tests);
    LeaveCriticalSection(&g_lock);
    for (const TestCmd& t : tests) {
        IDirect3DTexture9* tex = TextureOf(t.lo, t.hi);
        if (!tex) { Log(L"Cosmetics: test - no texture %x:%x here", t.lo, t.hi); continue; }
        D3DSURFACE_DESC d{};
        tex->GetLevelDesc(0, &d);
        Log(L"Cosmetics: test - texture %x:%x: %ux%u, format %08x, %lu levels, pool %d", t.lo, t.hi, d.Width, d.Height,
            (unsigned)d.Format, tex->GetLevelCount(), (int)d.Pool);
        if (t.fill) { Fill(tex, t.rgb); Log(L"Cosmetics: test - filled with %06x", t.rgb); }
    }
}
