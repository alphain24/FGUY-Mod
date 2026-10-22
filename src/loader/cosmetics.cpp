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

}  // namespace

void CosmeticsTest(bool fill, unsigned lo, unsigned hi, unsigned rgb) {
    Lock();
    g_tests.push_back({fill, lo, hi, rgb});
    LeaveCriticalSection(&g_lock);
}

// d3d9.cpp, the render thread after each Present
void CosmeticsFrame(IDirect3DDevice9* dev) {
    (void)dev;
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
