// Cosmetics (skins): the game's own textures, changed in place - a skin pack's textures go into the Direct3D textures the
// game already made for them (so every model using one shows it, menus too; nothing on disk changes).
//
// Finding a texture: its asset (a Texture asset - the id the Skin Studio saves) is a node in the scene's asset tree
// ([0xb07b04]: a node holds the id at +0/+4, its children at +0x20 / (+0x24 & ~3), its object at +0x3c); the object is a
// World::TextureResourceEntity (vtable 0x9fb2b8), its [+0x28] +0x14 the IDirect3DTexture9.
#include "common.h"
#include <d3d9.h>
#include <intrin.h>
#include <algorithm>
#include <mutex>
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
// The device's own calls, seen: the textures set (stage 0), the vertex shader constants (a shadow copy - c0..c10 only:
// characters are skinned on the CPU, see motion.cpp), the draws. "texdraws <lo> <hi>": for one frame, each draw with
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
unsigned g_traceLo = 0, g_traceHi = 0;   // (texdraws: the texture asked for - 0 0: every draw at a player's)
std::vector<std::pair<std::wstring, int>> g_kinds;   // ("texdraws 0 2": the frame's draws by kind)
// (research: who fills a buffer - its locks' callers; "texdraws 0 2" watches the first triangle-strip draw's, the ink lines?)
using VbLock_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD);
VbLock_t Real_VbLock;
IDirect3DVertexBuffer9* g_watchVb = nullptr;
volatile LONG g_watchLocks = 0;
HRESULT STDMETHODCALLTYPE Hook_VbLock(IDirect3DVertexBuffer9* self, UINT off, UINT size, void** data, DWORD flags) {
    HRESULT hr = Real_VbLock(self, off, size, data, flags);
    if (self == g_watchVb && InterlockedIncrement(&g_watchLocks) <= 8) {
        wchar_t line[1024];
        int len = swprintf_s(line, L"Cosmetics: trace - the strips' buffer locked: offset %u, %u bytes, flags %#lx - callers:", off, size, flags);
        const DWORD* sp = reinterpret_cast<const DWORD*>(_AddressOfReturnAddress());
        for (int i = 0, found = 0; i < 400 && found < 14 && len < 960; ++i) {
            if (IsBadReadPtr(sp + i, 4)) break;
            if (sp[i] >= 0x401000 && sp[i] < 0x9f0000) { len += swprintf_s(line + len, 1024 - len, L" %#lx", sp[i]); ++found; }
        }
        Log(L"%s", line);
    }
    return hr;
}
void* Patch(void* object, int slot, void* hook);
int g_traceDraws = 0;
float g_tracePlayers[4][3];
int g_tracePlayerCount = 0;

// ---- skins: each player's own textures ----------------------------------------------------------------------------
// A skin pack (.fgskin, from the Skin Studio): a zip - skin.json (its textures: asset id, the game-ready copy's path)
// and, stored uncompressed, each texture's game-ready DDS (the game texture's own format, size and mip levels). A player
// wearing one: for each of its textures, a texture of ours with the pack's pixels; a draw of that player's character
// (its world translation, the vertex shader's c4..c6 .w, at that player's position) with the game's texture bound
// gets ours instead - for that draw only. Everyone else using that outfit keeps the game's.
struct SkinTex {
    unsigned lo = 0, hi = 0;
    std::vector<unsigned char> dds;
    bool recipe = false;                   // (format 2: made here from the game's own texture - skinbuild.cpp)
    std::vector<double> ops;               //   its recolors, 7 numbers each
    std::vector<unsigned char> overlay;    //   its own pixels (a PNG), or none
    IDirect3DTexture9* mine = nullptr;     // (ours: made on the render thread)
    IDirect3DBaseTexture9* game = nullptr; // (the game's, found through the asset tree - looked up again now and then)
};
struct Skin {
    std::wstring name;
    std::vector<SkinTex> textures;
};
Skin* g_skins[4] = {};                     // (by player index - the render thread's)
struct PendingSkin { int slot; std::vector<unsigned char> pack; std::wstring name; };
std::mutex g_headLock;
std::string g_head[4];   // (each player's skin's head picture: its movie - "" none)
std::vector<PendingSkin> g_pending;        // (from any thread, under g_lock)
struct Swap { IDirect3DTexture9* perSlot[4]; };
std::vector<std::pair<IDirect3DBaseTexture9*, Swap>> g_swaps;   // (the game's texture -> each player's own)
float g_playerPos[4][3];
bool g_playerHere[4];

// every entry's name (a stored zip), false: not one
bool ZipNames(const std::vector<unsigned char>& z, std::vector<std::string>& names) {
    if (z.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22; i + 22 + 65535 >= z.size(); --i) {
        if (*reinterpret_cast<const unsigned*>(&z[i]) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return false;
    unsigned count = *reinterpret_cast<const unsigned short*>(&z[eocd + 10]);
    size_t cd = *reinterpret_cast<const unsigned*>(&z[eocd + 16]);
    for (unsigned e = 0; e < count; ++e) {
        if (cd + 46 > z.size() || *reinterpret_cast<const unsigned*>(&z[cd]) != 0x02014b50) return false;
        unsigned short nameLen = *reinterpret_cast<const unsigned short*>(&z[cd + 28]);
        unsigned short extra = *reinterpret_cast<const unsigned short*>(&z[cd + 30]);
        unsigned short comment = *reinterpret_cast<const unsigned short*>(&z[cd + 32]);
        if (cd + 46 + nameLen > z.size()) return false;
        names.emplace_back(reinterpret_cast<const char*>(&z[cd + 46]), nameLen);
        cd += 46 + nameLen + extra + comment;
    }
    return true;
}

bool ZipEntry(const std::vector<unsigned char>& z, const std::string& want, std::vector<unsigned char>& out) {
    // (stored entries only - the Skin Studio stores what the mod reads)
    if (z.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22; i + 1 > 0 && i + 22 + 65535 >= z.size(); --i) {
        if (*reinterpret_cast<const unsigned*>(&z[i]) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return false;
    unsigned count = *reinterpret_cast<const unsigned short*>(&z[eocd + 10]);
    size_t cd = *reinterpret_cast<const unsigned*>(&z[eocd + 16]);
    for (unsigned e = 0; e < count && cd + 46 <= z.size(); ++e) {
        if (*reinterpret_cast<const unsigned*>(&z[cd]) != 0x02014b50) return false;
        unsigned short method = *reinterpret_cast<const unsigned short*>(&z[cd + 10]);
        unsigned size = *reinterpret_cast<const unsigned*>(&z[cd + 20]);
        unsigned short nameLen = *reinterpret_cast<const unsigned short*>(&z[cd + 28]);
        unsigned short extra = *reinterpret_cast<const unsigned short*>(&z[cd + 30]);
        unsigned short comment = *reinterpret_cast<const unsigned short*>(&z[cd + 32]);
        size_t local = *reinterpret_cast<const unsigned*>(&z[cd + 42]);
        std::string name(reinterpret_cast<const char*>(&z[cd + 46]), nameLen);
        if (name == want) {
            if (method != 0 || local + 30 > z.size()) return false;
            size_t data = local + 30 + *reinterpret_cast<const unsigned short*>(&z[local + 26]) +
                          *reinterpret_cast<const unsigned short*>(&z[local + 28]);
            if (data + size > z.size()) return false;
            out.assign(z.begin() + data, z.begin() + data + size);
            return true;
        }
        cd += 46 + nameLen + extra + comment;
    }
    return false;
}

// the numbers of the array at `at` ('[' there), nested ones too
std::vector<double> Numbers(const std::string& s, size_t at) {
    std::vector<double> out;
    int depth = 0;
    for (size_t i = at; i < s.size(); ++i) {
        char c = s[i];
        if (c == '[') ++depth;
        else if (c == ']') { if (--depth == 0) break; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            char* end = nullptr;
            out.push_back(strtod(s.c_str() + i, &end));
            i = end - s.c_str() - 1;
        }
    }
    return out;
}

// skin.json, as the Skin Studio writes it. Format 2: under "textures", each texture's "id": [lo, hi], its "ops"
// (recolors) and its "overlay" (a PNG's path, or none) - no game art in it; format 1: each texture's "dds": "<path>"
// then "id" (the whole texture, game-ready)
Skin* ParsePack(const std::vector<unsigned char>& pack, const std::wstring& name) {
    std::vector<unsigned char> js;
    if (!ZipEntry(pack, "skin.json", js)) return nullptr;
    std::string s(js.begin(), js.end());
    auto skin = new Skin{name, {}};
    size_t format = s.find("\"format\"");
    if (format != std::string::npos && atoi(s.c_str() + s.find(':', format) + 1) >= 2) {
        size_t at = s.find("\"textures\"");
        while (at != std::string::npos && (at = s.find("\"id\"", at + 1)) != std::string::npos) {
            size_t next = s.find("\"id\"", at + 1), ops = s.find("\"ops\"", at), ov = s.find("\"overlay\"", at);
            size_t pic = s.find("\"picture\": true", at);
            if (pic != std::string::npos && pic < next) continue;   // (its picture in the skin menus: not the character's)
            SkinTex t;
            t.recipe = true;
            std::vector<double> id = Numbers(s, s.find('[', at));
            if (id.size() != 2) break;
            t.lo = (unsigned)id[0];
            t.hi = (unsigned)id[1];
            if (ops != std::string::npos && ops < next) t.ops = Numbers(s, s.find('[', ops));
            if (ov != std::string::npos && ov < next) {
                size_t q1 = s.find('"', s.find(':', ov) + 1), q2 = s.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) ZipEntry(pack, s.substr(q1 + 1, q2 - q1 - 1), t.overlay);
            }
            skin->textures.push_back(std::move(t));
        }
        if (skin->textures.empty()) { delete skin; return nullptr; }
        return skin;
    }
    size_t at = 0;
    while ((at = s.find("\"dds\"", at)) != std::string::npos) {
        size_t q1 = s.find('"', s.find(':', at) + 1), q2 = s.find('"', q1 + 1);
        size_t id = s.find("\"id\"", q2);
        if (q1 == std::string::npos || q2 == std::string::npos || id == std::string::npos) break;
        size_t br = s.find('[', id);
        SkinTex t;
        if (sscanf_s(s.c_str() + br, "[%u, %u]", &t.lo, &t.hi) != 2 && sscanf_s(s.c_str() + br, "[%u,%u]", &t.lo, &t.hi) != 2) break;
        if (ZipEntry(pack, s.substr(q1 + 1, q2 - q1 - 1), t.dds) && t.dds.size() > 128) skin->textures.push_back(std::move(t));
        at = br;
    }
    if (skin->textures.empty()) { delete skin; return nullptr; }
    return skin;
}

// ours: the game texture's format, filled from the DDS's levels
IDirect3DTexture9* MakeTexture(IDirect3DDevice9* dev, const SkinTex& t) {
    if (t.dds.size() < 128) return nullptr;   // (a recipe's: not made yet - the game's texture isn't here yet)
    const unsigned char* d = t.dds.data();
    if (*reinterpret_cast<const unsigned*>(d) != 0x20534444) return nullptr;
    unsigned h = *reinterpret_cast<const unsigned*>(d + 12), w = *reinterpret_cast<const unsigned*>(d + 16);
    unsigned mips = max(1u, *reinterpret_cast<const unsigned*>(d + 28));
    unsigned pf = *reinterpret_cast<const unsigned*>(d + 80), four = *reinterpret_cast<const unsigned*>(d + 84);
    D3DFORMAT fmt = !(pf & 4) ? D3DFMT_A8R8G8B8 : four == MAKEFOURCC('D', 'X', 'T', '1') ? D3DFMT_DXT1 : D3DFMT_DXT5;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(w, h, mips, 0, fmt, D3DPOOL_MANAGED, &tex, nullptr))) return nullptr;
    size_t o = 128;
    for (unsigned l = 0; l < mips && l < tex->GetLevelCount(); ++l) {
        unsigned lw = max(1u, w >> l), lh = max(1u, h >> l);
        unsigned bw = max(1u, (lw + 3) / 4), bh = max(1u, (lh + 3) / 4);
        unsigned rowBytes = fmt == D3DFMT_A8R8G8B8 ? lw * 4 : bw * (fmt == D3DFMT_DXT1 ? 8 : 16);
        unsigned rows = fmt == D3DFMT_A8R8G8B8 ? lh : bh;
        if (o + (size_t)rowBytes * rows > t.dds.size()) break;
        D3DLOCKED_RECT lr;
        if (FAILED(tex->LockRect(l, &lr, nullptr, 0))) break;
        for (unsigned r = 0; r < rows; ++r) memcpy(static_cast<unsigned char*>(lr.pBits) + r * lr.Pitch, d + o + (size_t)r * rowBytes, rowBytes);
        tex->UnlockRect(l);
        o += (size_t)rowBytes * rows;
    }
    return tex;
}

void RebuildSwaps() {
    g_swaps.clear();
    for (int s = 0; s < 4; ++s) {
        if (!g_skins[s]) continue;
        for (SkinTex& t : g_skins[s]->textures) {
            if (!t.game || !t.mine) continue;
            auto it = std::find_if(g_swaps.begin(), g_swaps.end(), [&](const auto& p) { return p.first == t.game; });
            if (it == g_swaps.end()) { g_swaps.push_back({t.game, Swap{}}); it = g_swaps.end() - 1; }
            it->second.perSlot[s] = t.mine;
        }
    }
}

// which player this draw is of: the one whose position its world translation (c4..c6 .w) is nearest, within 3 units
int DrawPlayer() {
    int best = -1;
    float bd = 9.0f;
    for (int p = 0; p < 4; ++p) {
        if (!g_playerHere[p]) continue;
        float dx = g_vsConst[4][3] - g_playerPos[p][0], dy = g_vsConst[5][3] - g_playerPos[p][1], dz = g_vsConst[6][3] - g_playerPos[p][2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = p; }
    }
    return best;
}

HRESULT STDMETHODCALLTYPE Hook_SetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* tex) {
    if (stage == 0) g_stage0 = tex;
    if (stage < 8) g_stages[stage] = tex;
    return Real_SetTexture(self, stage, tex);
}
HRESULT STDMETHODCALLTYPE Hook_SetVSConstF(IDirect3DDevice9* self, UINT reg, const float* data, UINT count) {
    if (data && reg < 256) memcpy(g_vsConst[reg], data, sizeof(float) * 4 * min(count, 256 - reg));
    return Real_SetVSConstF(self, reg, data, count);
}
HRESULT DrawIndexedSkinned(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base, UINT minIdx, UINT verts, UINT start,
                           UINT prims);
HRESULT STDMETHODCALLTYPE Hook_DrawIndexed(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base, UINT minIdx,
                                           UINT verts, UINT start, UINT prims) {
    IDirect3DIndexBuffer9* putBack = nullptr;   // (models.cpp put its own index buffer in for this draw: theirs after)
    if (ModelsWorn()) {   // (a player wearing a new character: theirs drawn as ours - models.cpp)
        HRESULT hr = D3D_OK;
        if (ModelsDraw(self, DrawPlayer(), reinterpret_cast<ModelsRealDraw>(Real_DrawIndexed), &hr, &putBack)) return hr;
    }
    HRESULT hr = DrawIndexedSkinned(self, type, base, minIdx, verts, start, prims);
    if (putBack) self->SetIndices(putBack);
    return hr;
}

// the game's draw - a player's skin's textures in place of the game's, for that draw
HRESULT DrawIndexedSkinned(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base, UINT minIdx, UINT verts, UINT start,
                           UINT prims) {
    if (g_tracing) {
        ++g_traceAllDraws;
        for (int st = 0; st < 8; ++st) if (g_stages[st] == g_traceTex) ++g_traceStageHit[st];
    }
    bool traceAll = g_traceLo == 0 && (g_traceHi == 0 || g_traceHi == 1);   // ("texdraws 0 0": every draw at a player's
    bool traced = false;                                                    //  position; "0 1": every skinned one)
    if (g_tracing && g_traceLo == 0 && g_traceHi == 2) {   // ("0 2": the frame's draws, counted by kind)
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        D3DVERTEXBUFFER_DESC vd{};
        if (SUCCEEDED(self->GetStreamSource(0, &vb, &off, &stride)) && vb) { vb->GetDesc(&vd); vb->Release(); }
        wchar_t k[96];
        swprintf_s(k, L"type %d stride %u %s%s", (int)type, stride, (vd.Usage & D3DUSAGE_DYNAMIC) ? L"dynamic" : L"static",
                   DrawPlayer() >= 0 ? L" at a player" : L"");
        if (type == D3DPT_TRIANGLESTRIP && stride == 24 && (vd.Usage & D3DUSAGE_DYNAMIC) && !g_watchVb) {
            IDirect3DVertexBuffer9* v = nullptr;
            UINT o2, s2;
            if (SUCCEEDED(self->GetStreamSource(0, &v, &o2, &s2)) && v) {
                if (void* o = Patch(v, 11, (void*)Hook_VbLock)) Real_VbLock = (VbLock_t)o;
                g_watchVb = v;   // (kept: the locks of the next frames)
                g_watchLocks = 0;
                v->Release();
            }
        }
        auto it = std::find_if(g_kinds.begin(), g_kinds.end(), [&](const auto& x) { return x.first == k; });
        if (it == g_kinds.end()) g_kinds.push_back({k, 1}); else ++it->second;
    }
    if (g_tracing && g_traceLo == 0 && g_traceHi == 1) {
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        if (SUCCEEDED(self->GetStreamSource(0, &vb, &off, &stride)) && vb) {
            D3DVERTEXBUFFER_DESC vd{};
            vb->GetDesc(&vd);
            vb->Release();
            traced = stride == 12 && (vd.Usage & D3DUSAGE_DYNAMIC);
            if (traced && g_traceDraws < 40) {   // (and which registers hold a player's position - a matrix's translation)
                std::wstring where;
                for (int r = 0; r + 2 < 256; ++r)
                    for (int p = 0; p < g_tracePlayerCount; ++p) {
                        float dx = g_vsConst[r][3] - g_tracePlayers[p][0], dy = g_vsConst[r + 1][3] - g_tracePlayers[p][1],
                              dz = g_vsConst[r + 2][3] - g_tracePlayers[p][2];
                        if (dx * dx + dy * dy + dz * dz < 4.0f) { wchar_t w[24]; swprintf_s(w, L" c%d~p%d", r, p); where += w; }
                    }
                IDirect3DIndexBuffer9* ib = nullptr;
                self->GetIndices(&ib);
                if (ib) ib->Release();
                Log(L"Cosmetics: trace - skinned draw at stream offset %u, %u vertices, indices %p, c4..c6 w %.1f %.1f %.1f;%s",
                    off, verts, ib, g_vsConst[4][3], g_vsConst[5][3], g_vsConst[6][3], where.empty() ? L" no player" : where.c_str());
            }
        }
    } else {
        traced = traceAll ? DrawPlayer() >= 0
                          : g_traceTex && (g_stages[0] == g_traceTex || g_stages[1] == g_traceTex || g_stages[2] == g_traceTex);
    }
    if (g_tracing && traced && g_traceDraws < 40) {
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
        if (g_traceDraws <= (traceAll ? 40 : 3)) {   // (what a character's draw is made of: its vertex layout, streams, triangles, textures)
            std::wstring d;
            wchar_t b[160];
            IDirect3DVertexDeclaration9* decl = nullptr;
            if (SUCCEEDED(self->GetVertexDeclaration(&decl)) && decl) {
                D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
                UINT n = 0;
                if (SUCCEEDED(decl->GetDeclaration(el, &n)))
                    for (UINT i = 0; i < n && el[i].Stream != 0xff; ++i) {
                        swprintf_s(b, L" [s%u +%u type %u usage %u.%u]", el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage,
                                   el[i].UsageIndex);
                        d += b;
                    }
                decl->Release();
            }
            for (UINT s = 0; s < 4; ++s) {
                IDirect3DVertexBuffer9* vb = nullptr;
                UINT off = 0, stride = 0;
                if (SUCCEEDED(self->GetStreamSource(s, &vb, &off, &stride)) && vb) {
                    D3DVERTEXBUFFER_DESC vd{};
                    vb->GetDesc(&vd);
                    swprintf_s(b, L" | stream %u %p +%u stride %u (%u bytes, usage %#lx)", s, vb, off, stride, vd.Size, vd.Usage);
                    d += b;
                    vb->Release();
                }
            }
            IDirect3DIndexBuffer9* ib = nullptr;
            if (SUCCEEDED(self->GetIndices(&ib)) && ib) {
                D3DINDEXBUFFER_DESC id{};
                ib->GetDesc(&id);
                swprintf_s(b, L" | indices %p (%u bytes, format %u) base %d first %u start %u", ib, id.Size, (unsigned)id.Format, base,
                           minIdx, start);
                d += b;
                ib->Release();
            }
            for (int st = 0; st < 4; ++st) { swprintf_s(b, L" | tex%d %p", st, g_stages[st]); d += b; }
            Log(L"Cosmetics: trace - draw %d is%s", g_traceDraws, d.c_str());
        }
    }
    if (!g_swaps.empty()) {   // a skinned player's character: their own textures for this draw
        IDirect3DBaseTexture9* restore[3] = {};
        int player = -2;
        for (int st = 0; st < 3; ++st) {
            IDirect3DBaseTexture9* bound = g_stages[st];
            if (!bound) continue;
            for (const auto& sw : g_swaps) {
                if (sw.first != bound) continue;
                if (player == -2) player = DrawPlayer();
                if (player >= 0 && sw.second.perSlot[player]) {
                    restore[st] = bound;
                    Real_SetTexture(self, st, sw.second.perSlot[player]);
                }
                break;
            }
        }
        if (player != -2) {
            HRESULT hr = Real_DrawIndexed(self, type, base, minIdx, verts, start, prims);
            for (int st = 0; st < 3; ++st) if (restore[st]) Real_SetTexture(self, st, restore[st]);
            return hr;
        }
    }
    return Real_DrawIndexed(self, type, base, minIdx, verts, start, prims);
}

// (research, while tracing: the other draw calls - which of them draw at a player's position)
using DrawPrim_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawPrimUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                                    const void*, UINT);
DrawPrim_t Real_DrawPrim;
DrawPrimUP_t Real_DrawPrimUP;
DrawIndexedUP_t Real_DrawIndexedUP;
int g_traceOther = 0;
void TraceOther(const wchar_t* what, D3DPRIMITIVETYPE type, UINT count, UINT stride) {
    if (!g_tracing || g_traceOther >= 40) return;
    ++g_traceOther;
    Log(L"Cosmetics: trace - %s: type %d, %u primitives, stride %u, c4..c6 w %.1f %.1f %.1f (player %d)", what, (int)type, count, stride,
        g_vsConst[4][3], g_vsConst[5][3], g_vsConst[6][3], DrawPlayer());
}
HRESULT STDMETHODCALLTYPE Hook_DrawPrim(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    TraceOther(L"DrawPrimitive", type, count, 0);
    return Real_DrawPrim(self, type, start, count);
}
HRESULT STDMETHODCALLTYPE Hook_DrawPrimUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    TraceOther(L"DrawPrimitiveUP", type, count, stride);
    return Real_DrawPrimUP(self, type, count, data, stride);
}
HRESULT STDMETHODCALLTYPE Hook_DrawIndexedUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minIdx, UINT verts, UINT count,
                                             const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
    TraceOther(L"DrawIndexedPrimitiveUP", type, count, stride);
    return Real_DrawIndexedUP(self, type, minIdx, verts, count, idx, fmt, data, stride);
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


}  // namespace

bool ZipList(const std::vector<unsigned char>& zip, std::vector<std::string>& names) { return ZipNames(zip, names); }

bool ZipRead(const std::vector<unsigned char>& zip, const std::string& name, std::vector<unsigned char>& out) {
    return ZipEntry(zip, name, out);
}

IDirect3DTexture9* CosmeticsTextureFromDds(IDirect3DDevice9* dev, const std::vector<unsigned char>& dds) {
    SkinTex t;
    t.dds = dds;
    return MakeTexture(dev, t);
}
int CosmeticsDrawPlayer() { return DrawPlayer(); }

void CosmeticsDeviceCreated(IDirect3DDevice9* dev) {
    if (void* o = Patch(dev, 65, (void*)Hook_SetTexture)) Real_SetTexture = (SetTexture_t)o;
    if (void* o = Patch(dev, 94, (void*)Hook_SetVSConstF)) Real_SetVSConstF = (SetVSConstF_t)o;
    if (void* o = Patch(dev, 82, (void*)Hook_DrawIndexed)) Real_DrawIndexed = (DrawIndexed_t)o;
    if (void* o = Patch(dev, 81, (void*)Hook_DrawPrim)) Real_DrawPrim = (DrawPrim_t)o;
    if (void* o = Patch(dev, 83, (void*)Hook_DrawPrimUP)) Real_DrawPrimUP = (DrawPrimUP_t)o;
    if (void* o = Patch(dev, 84, (void*)Hook_DrawIndexedUP)) Real_DrawIndexedUP = (DrawIndexedUP_t)o;
}

// A pack's name and the model (outfit) it's for (skin.json: "name": "..", "modelId": [lo, hi]) - false: not a pack.
bool CosmeticsPackInfo(const std::vector<unsigned char>& pack, std::string* name, unsigned* lo, unsigned* hi) {
    std::vector<unsigned char> js;
    if (!ZipEntry(pack, "skin.json", js)) return false;
    std::string s(js.begin(), js.end());
    if (name) {
        size_t n = s.find("\"name\"");
        size_t q1 = n == std::string::npos ? n : s.find('"', s.find(':', n) + 1);
        size_t q2 = q1 == std::string::npos ? q1 : s.find('"', q1 + 1);
        *name = q2 == std::string::npos ? "" : s.substr(q1 + 1, q2 - q1 - 1);
    }
    size_t m = s.find("\"modelId\"");
    if (m == std::string::npos) return false;   // (made before the Skin Studio saved it: can't be matched to an outfit)
    size_t br = s.find('[', m);
    return br != std::string::npos && (sscanf_s(s.c_str() + br, "[%u, %u]", lo, hi) == 2 || sscanf_s(s.c_str() + br, "[%u,%u]", lo, hi) == 2);
}

// The model a player wears now ([[player +0x38] +0x1d4]: its World::ModelPrototypeEntity) - its asset id, through the
// asset tree. which: the player's index (+0xdf8), or -1 - the list's n-th player instead (the menus' 3D characters).
bool CosmeticsPlayerModel(int which, int nth, unsigned* lo, unsigned* hi) {
    unsigned char* players[8];
    int n = GamePlayers(players, 8);
    unsigned char* p = nullptr;
    for (int i = 0; i < n; ++i)
        if ((which >= 0 && *reinterpret_cast<int*>(players[i] + 0xdf8) == which) || (which < 0 && i == nth)) p = players[i];
    if (!p) return false;
    auto inst = *reinterpret_cast<unsigned char**>(p + 0x38);
    if (!inst || IsBadReadPtr(inst + 0x1d4, 4)) return false;
    unsigned char* model = *reinterpret_cast<unsigned char**>(inst + 0x1d4);
    if (!model) return false;
    auto root = *reinterpret_cast<unsigned char* volatile*>(kAssetTree);
    std::vector<unsigned char*> stack{root};
    int seen = 0;
    while (!stack.empty() && seen < 400000) {
        unsigned char* node = stack.back();
        stack.pop_back();
        if (!node || IsBadReadPtr(node, 0x40)) continue;
        ++seen;
        if (*reinterpret_cast<unsigned char**>(node + 0x3c) == model) {
            *lo = *reinterpret_cast<unsigned*>(node);
            *hi = *reinterpret_cast<unsigned*>(node + 4);
            return true;
        }
        stack.push_back(*reinterpret_cast<unsigned char**>(node + 0x20));
        stack.push_back(reinterpret_cast<unsigned char*>(*reinterpret_cast<uintptr_t*>(node + 0x24) & ~3u));
    }
    return false;
}

// A player wears a skin pack (any thread; the pack's bytes - empty: none). Made ready on the render thread.
void CosmeticsSetSkin(int slot, const std::vector<unsigned char>& pack, const wchar_t* name) {
    if (slot < 0 || slot >= 4) return;
    Lock();
    g_pending.push_back({slot, pack, name ? name : L""});
    LeaveCriticalSection(&g_lock);
}
// test pipe "skinload <slot> <file.fgskin>" (empty path: off)
void CosmeticsLoadSkinFile(int slot, const wchar_t* path) {
    std::vector<unsigned char> d;
    if (path && *path) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) { Log(L"Cosmetics: no skin pack %s", path); return; }
        DWORD size = GetFileSize(f, nullptr), got = 0;
        d.resize(size);
        ReadFile(f, d.data(), size, &got, nullptr);
        CloseHandle(f);
    }
    CosmeticsSetSkin(slot, d, path);
}

// ---- the lobby: my skin -------------------------------------------------------------------------------------------
// Skin packs live in FGUYMod\skins (*.fgskin). The lobby's character choice offers the ones made for the outfit its 3D
// preview shows (the menu's player 2), and the one picked shows on the preview at once; the match gets it
// (FGUYMOD_MY_SKIN, online.cpp) and sends it to everyone (netplay.cpp). The pick is remembered per outfit (FGUYMod.ini
// [Skins] <model lo>_<model hi>=<file>).
namespace {
std::string JsonField(const std::vector<unsigned char>& pack, const char* key) {   // (skin.json's "key": "value")
    std::vector<unsigned char> js;
    if (!ZipEntry(pack, "skin.json", js)) return "";
    std::string s(js.begin(), js.end()), k = std::string("\"") + key + "\"";
    size_t at = s.find(k);
    if (at == std::string::npos) return "";
    size_t q1 = s.find('"', s.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : s.find('"', q1 + 1);
    return q2 == std::string::npos ? "" : s.substr(q1 + 1, q2 - q1 - 1);
}
struct PackEntry { std::wstring file; std::string name; unsigned lo, hi; FILETIME written; std::string who; std::wstring path, from;
                   std::string icon, head;   // (its pictures' movies - icons.cpp: "" none; head "<movie>|l|r|b")
                   std::string pic;          // (its outfit as the game's skin pickers name it: Costume_<Character><Outfit>.tif)
                   std::string recipe; };    // (its picture made here from the game's own - Recipe's key: "" none)
std::vector<PackEntry> g_packs;

// A skin's picture made from the outfit's own in the game (skin.json's texture entry "picture": true - the Skin Studio's
// PICTURE): its recolors and overlay on the game's texture, made on the render thread once that's loaded (icons.cpp
// then makes its movie). No game art in the pack - the picture is this PC's own game's, changed.
struct Recipe { std::string key; unsigned lo = 0, hi = 0; std::vector<double> ops; std::vector<unsigned char> overlay; bool done = false; };
std::mutex g_recipeLock;
std::vector<Recipe> g_recipes;

std::string PictureRecipe(const std::vector<unsigned char>& pack) {   // (its key - "" none)
    std::vector<unsigned char> js;
    if (!ZipEntry(pack, "skin.json", js)) return "";
    std::string s(js.begin(), js.end());
    size_t pic = s.find("\"picture\": true");
    if (pic == std::string::npos) return "";
    size_t at = s.rfind("\"id\"", pic);
    if (at == std::string::npos) return "";
    std::vector<double> id = Numbers(s, s.find('[', at));
    if (id.size() != 2) return "";
    Recipe r;
    r.lo = (unsigned)id[0];
    r.hi = (unsigned)id[1];
    size_t ops = s.find("\"ops\"", at), next = s.find("\"id\"", at + 1);
    if (ops != std::string::npos && ops < next) r.ops = Numbers(s, s.find('[', ops));
    size_t ov = s.find("\"overlay\"", at);
    if (ov != std::string::npos && ov < next) {
        size_t q1 = s.find('"', s.find(':', ov) + 1), q2 = s.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) ZipEntry(pack, s.substr(q1 + 1, q2 - q1 - 1), r.overlay);
    }
    unsigned h = 2166136261u;   // (its key: what it's made of)
    auto mix = [&](const void* p, size_t n) { for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(p)[i]) * 16777619u; };
    mix(&r.lo, 4); mix(&r.hi, 4);
    if (!r.ops.empty()) mix(r.ops.data(), r.ops.size() * sizeof(double));
    if (!r.overlay.empty()) mix(r.overlay.data(), r.overlay.size());
    char key[16];
    sprintf_s(key, "p%08x", h);
    r.key = key;
    std::lock_guard<std::mutex> l(g_recipeLock);
    if (!std::any_of(g_recipes.begin(), g_recipes.end(), [&](const Recipe& x) { return x.key == r.key; })) g_recipes.push_back(r);
    return r.key;
}

// a pack's tile picture for the menus: its own (icon.png), else its picture made here once it is ("" none yet)
std::string IconOf(const PackEntry& e) {
    if (!e.icon.empty()) return e.icon;
    if (e.recipe.empty()) return "";
    std::string url = "fguyicon_" + e.recipe + ".gfx";
    return IconsReady(url) ? url : "";
}

// skin.json's "headFace": [left, right, bottom] as "|l|r|b" ("" none)
std::string HeadFace(const std::vector<unsigned char>& pack) {
    std::vector<unsigned char> js;
    if (!ZipEntry(pack, "skin.json", js)) return "";
    std::string j(js.begin(), js.end());
    size_t at = j.find("\"headFace\"");
    std::vector<double> f = at == std::string::npos ? std::vector<double>() : Numbers(j, j.find('[', at));
    if (f.size() != 3) return "";
    char t[96];
    sprintf_s(t, "|%.4f|%.4f|%.4f", f[0], f[1], f[2]);
    return t;
}
// a head picture's id: its movie's key ("fguyicon_<key>.gfx|...")
std::string HeadKey(const std::string& head) {
    size_t a = head.find("fguyicon_"), b = head.find(".gfx");
    return a == std::string::npos || b == std::string::npos ? "" : head.substr(a + 9, b - a - 9);
}
ULONGLONG g_packsAt = 0;
std::wstring g_myPick;              // (the file picked for the outfit shown - empty: none)
unsigned g_myPickLo = 0, g_myPickHi = 0;
std::string g_optionsText;

// a pack's file (by its name: the first of that name - the mods' before the loose folder's), "" none
std::wstring PackPath(const std::wstring& file) {
    for (const PackEntry& e : g_packs) if (e.file == file) return e.path;
    return L"";
}

std::vector<unsigned char> ReadAll(const std::wstring& path) {
    std::vector<unsigned char> d;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return d;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    if (size != INVALID_FILE_SIZE && size < 32 * 1024 * 1024) {
        d.resize(size);
        if (!ReadFile(f, d.data(), size, &got, nullptr) || got != size) d.clear();
    }
    CloseHandle(f);
    return d;
}

void ScanPacks() {
    ULONGLONG now = GetTickCount64();
    if (g_packsAt && now - g_packsAt < 3000) return;
    g_packsAt = now;
    std::vector<PackEntry> out;
    for (const auto& folder : ModsContentFolders(L"skins")) {   // (the Cosmetics mod's skins, then FGUYMod\skins)
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder.first + L"\\*.fgskin").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring path = folder.first + L"\\" + fd.cFileName;
            if (std::any_of(out.begin(), out.end(), [&](const PackEntry& e) { return e.file == fd.cFileName; })) continue;
            auto known = std::find_if(g_packs.begin(), g_packs.end(), [&](const PackEntry& e) {
                return e.path == path && CompareFileTime(&e.written, &fd.ftLastWriteTime) == 0; });
            if (known != g_packs.end()) { out.push_back(*known); continue; }   // (unchanged: not read again)
            PackEntry e{fd.cFileName, "", 0, 0, fd.ftLastWriteTime, "", path, folder.second};
            std::vector<unsigned char> d = ReadAll(path);
            if (CosmeticsPackInfo(d, &e.name, &e.lo, &e.hi)) {
                std::vector<unsigned char> png;
                if (ZipEntry(d, "icon.png", png)) e.icon = IconsAdd(png);
                else e.recipe = PictureRecipe(d);
                png.clear();
                if (ZipEntry(d, "head.png", png)) e.head = IconsAdd(png) + HeadFace(d);
                e.who = JsonField(d, "character");   // ("Stewie - Default": the MODS page)
                std::string outfit = JsonField(d, "outfit");
                if (!e.who.empty() && !outfit.empty())   // (the game's pictures call Adam West "West")
                    e.pic = "Costume_" + (e.who == "AdamWest" ? std::string("West") : e.who) + outfit + ".tif";
                if (!outfit.empty()) e.who += " - " + outfit;
                out.push_back(e);
            }
            else Log(L"Cosmetics: %s isn't a skin pack this version can use (made by an older Skin Studio?)", path.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_packs.swap(out);
}

std::wstring IniPath() { return std::wstring(g_modDir) + L"\\FGUYMod.ini"; }

// the file picked for an outfit ("" none) - FGUYMod.ini [Skins] <lo>_<hi>
std::wstring PickFor(unsigned lo, unsigned hi) {
    wchar_t key[32], file[MAX_PATH] = L"";
    swprintf_s(key, L"%x_%x", lo, hi);
    GetPrivateProfileStringW(L"Skins", key, L"", file, MAX_PATH, IniPath().c_str());
    return file;
}
void SetPickFor(unsigned lo, unsigned hi, const std::wstring& file) {
    wchar_t key[32];
    swprintf_s(key, L"%x_%x", lo, hi);
    WritePrivateProfileStringW(L"Skins", key, file.empty() ? nullptr : file.c_str(), IniPath().c_str());
}

// Offline (no session): every player whose outfit has a skin switched on wears it - the menus' characters too -
// unless the game's own skin pickers say otherwise for that player (g_offlineChoice: the join screens' pick, the
// store's preview - src/ui/costumes.as2js). Looked at a few times a second (the render thread); a pack read only when
// what a player should wear changes.
std::wstring g_offlineWorn[4];
std::mutex g_choiceLock;
std::wstring g_offlineChoice[4];   // ("" the outfit's own pick, "-" none, else that pack - if it's for their outfit)
void OfflineSkins() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 300) return;
    last = now;
    ScanPacks();
    unsigned char* players[8];
    int n = GamePlayers(players, 8);
    bool seen[4] = {};
    for (int i = 0; i < n; ++i) {
        int slot = *reinterpret_cast<int*>(players[i] + 0xdf8);
        if (slot < 0 || slot >= 4) continue;
        seen[slot] = true;
        unsigned lo = 0, hi = 0;
        std::wstring want;
        if (CosmeticsPlayerModel(slot, 0, &lo, &hi)) {
            std::wstring f = PickFor(lo, hi), chosen;
            {
                std::lock_guard<std::mutex> l(g_choiceLock);
                chosen = g_offlineChoice[slot];
            }
            if (chosen == L"-") f.clear();
            else if (!chosen.empty() && std::any_of(g_packs.begin(), g_packs.end(), [&](const PackEntry& e) {
                         return e.file == chosen && e.lo == lo && e.hi == hi; }))
                f = chosen;
            if (!f.empty() && std::any_of(g_packs.begin(), g_packs.end(), [&](const PackEntry& e) {
                    return e.file == f && e.lo == lo && e.hi == hi; }))
                want = f;
        }
        if (want == g_offlineWorn[slot]) continue;
        g_offlineWorn[slot] = want;
        CosmeticsLoadSkinFile(slot, want.empty() ? L"" : PackPath(want).c_str());
    }
    for (int s = 0; s < 4; ++s)
        if (!seen[s] && !g_offlineWorn[s].empty()) { g_offlineWorn[s].clear(); CosmeticsLoadSkinFile(s, L""); }
}
std::string g_listText;
}  // namespace

void CosmeticsOfflineTick() { OfflineSkins(); }

// ---- the game's own skin pickers, offline (src/ui/costumes.as2js) --------------------------------------------------
namespace {
std::wstring Wide(const char* s) {
    wchar_t w[MAX_PATH] = L"";
    if (s) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH);
    return w;
}
std::string Narrow(const std::wstring& w) {
    char s[MAX_PATH * 3] = "";
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s, sizeof(s), nullptr, nullptr);
    return s;
}
std::string g_offlineText, g_answer;
}  // namespace

// FGUYMOD_OfflineSkins: "<outfit's picture>~<file>~<name>~<icon movie>;..." - every skin pack here
const char* CosmeticsOfflineSkins() {
    ScanPacks();
    g_offlineText.clear();
    for (const PackEntry& e : g_packs) {
        if (e.pic.empty()) continue;
        std::string n = e.name;
        for (char& c : n) if (c == '~' || c == ';') c = ' ';
        g_offlineText += e.pic + "~" + Narrow(e.file) + "~" + n + "~" + IconOf(e) + ";";
    }
    return g_offlineText.c_str();
}

// FGUYMOD_OfflineSkinPick(player, file | "-" none | "" the outfit's own): what that player wears (a join screen's pick,
// player 2: the store's preview)
void CosmeticsOfflineSkinPick(int player, const char* file) {
    if (player < 0 || player >= 4) return;
    std::lock_guard<std::mutex> l(g_choiceLock);
    g_offlineChoice[player] = Wide(file);
}

// FGUYMOD_OfflineSkinFor(player, outfit's picture): the skin that player wears with that outfit ("" none)
const char* CosmeticsOfflineSkinFor(int player, const char* pic) {
    g_answer.clear();
    std::wstring chosen;
    {
        std::lock_guard<std::mutex> l(g_choiceLock);
        if (player >= 0 && player < 4) chosen = g_offlineChoice[player];
    }
    if (chosen == L"-") return g_answer.c_str();
    for (const PackEntry& e : g_packs)
        if (e.pic == (pic ? pic : "") && !chosen.empty() && e.file == chosen) return (g_answer = Narrow(e.file)).c_str();
    for (const PackEntry& e : g_packs)
        if (e.pic == (pic ? pic : "") && PickFor(e.lo, e.hi) == e.file) return (g_answer = Narrow(e.file)).c_str();
    return g_answer.c_str();
}

// FGUYMOD_StoreSkinOf(outfit's picture): the skin that outfit wears (its MODS page ON) - FGUYMOD_StoreSkinPick(that,
// file | ""): it from now on
const char* CosmeticsStoreSkinOf(const char* pic) {
    g_answer.clear();
    for (const PackEntry& e : g_packs)
        if (e.pic == (pic ? pic : "") && PickFor(e.lo, e.hi) == e.file) return (g_answer = Narrow(e.file)).c_str();
    return g_answer.c_str();
}
void CosmeticsStoreSkinPick(const char* pic, const char* file) {
    std::wstring w = Wide(file);
    for (const PackEntry& e : g_packs) {
        if (e.pic != (pic ? pic : "")) continue;
        if (!w.empty() && e.file != w) continue;
        SetPickFor(e.lo, e.hi, w);
        Log(L"Cosmetics: the store - %S wears %s", e.pic.c_str(), w.empty() ? L"no skin" : w.c_str());
        if (!w.empty()) return;
    }
}

// FGUYMOD_SkinList (the MODS page): "<file>|<name>|<character - outfit>|<on>\n..."
const char* CosmeticsSkinList() {
    g_packsAt = 0;   // (fresh)
    ScanPacks();
    g_listText.clear();
    for (const PackEntry& e : g_packs) {
        char f[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, e.file.c_str(), -1, f, sizeof(f), nullptr, nullptr);
        std::string n = e.name, w = e.who;
        if (!e.from.empty()) {   // ("Stewie - Default (FGUY Cosmetics)")
            char m[256];
            WideCharToMultiByte(CP_UTF8, 0, e.from.c_str(), -1, m, sizeof(m), nullptr, nullptr);
            w += std::string(" (") + m + ")";
        }
        for (char& c : n) if (c == '|' || c == '\n') c = ' ';
        for (char& c : w) if (c == '|' || c == '\n') c = ' ';
        g_listText += std::string(f) + "|" + n + "|" + w + "|" + (PickFor(e.lo, e.hi) == e.file ? "1" : "0") + "\n";
    }
    return g_listText.c_str();
}

// FGUYMOD_SkinToggle(<file>): that skin on for its outfit (the outfit's other one off), or off
void CosmeticsSkinToggle(const char* file) {
    wchar_t w[MAX_PATH] = L"";
    if (file) MultiByteToWideChar(CP_UTF8, 0, file, -1, w, MAX_PATH);
    for (const PackEntry& e : g_packs) {
        if (e.file != w) continue;
        bool on = PickFor(e.lo, e.hi) == e.file;
        SetPickFor(e.lo, e.hi, on ? L"" : e.file);
        if (e.lo == g_myPickLo && e.hi == g_myPickHi) g_myPickLo = g_myPickHi = 0;   // (the lobby reads it again)
        Log(L"Cosmetics: skin %s %s", e.file.c_str(), on ? L"off" : L"on");
        return;
    }
}

// FGUYMOD_SkinOptions: "<model lo>_<model hi>|<picked file>|<name>~<file>~<icon movie>;..." - the packs for the outfit
// the lobby's preview shows (none known: "|" only)
const char* CosmeticsSkinOptions() {
    ScanPacks();
    unsigned lo = 0, hi = 0;
    g_optionsText.clear();
    if (!CosmeticsPlayerModel(2, 0, &lo, &hi)) return g_optionsText.c_str();
    if (lo != g_myPickLo || hi != g_myPickHi) {   // (another outfit: its remembered pick)
        wchar_t key[32], file[MAX_PATH] = L"";
        swprintf_s(key, L"%x_%x", lo, hi);
        GetPrivateProfileStringW(L"Skins", key, L"", file, MAX_PATH, IniPath().c_str());
        g_myPick = file;
        g_myPickLo = lo; g_myPickHi = hi;
        bool there = std::any_of(g_packs.begin(), g_packs.end(), [&](const PackEntry& e) { return e.file == g_myPick && e.lo == lo && e.hi == hi; });
        if (!there) g_myPick.clear();
        CosmeticsLoadSkinFile(2, g_myPick.empty() ? L"" : PackPath(g_myPick).c_str());   // (the preview)
    }
    char buf[64];
    sprintf_s(buf, "%x_%x|", lo, hi);
    g_optionsText = buf;
    char pick[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, g_myPick.c_str(), -1, pick, sizeof(pick), nullptr, nullptr);
    g_optionsText += std::string(pick) + "|";
    for (const PackEntry& e : g_packs) {
        if (e.lo != lo || e.hi != hi) continue;
        char f[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, e.file.c_str(), -1, f, sizeof(f), nullptr, nullptr);
        std::string n = e.name;
        for (char& c : n) if (c == '~' || c == ';' || c == '|') c = ' ';
        g_optionsText += n + "~" + f + "~" + IconOf(e) + ";";
    }
    return g_optionsText.c_str();
}

// FGUYMOD_SkinPick(<file> or ""): my skin for the outfit the preview shows
void CosmeticsSkinPick(const char* file) {
    wchar_t w[MAX_PATH] = L"";
    if (file) MultiByteToWideChar(CP_UTF8, 0, file, -1, w, MAX_PATH);
    unsigned lo = 0, hi = 0;
    if (!CosmeticsPlayerModel(2, 0, &lo, &hi)) return;
    g_myPick = w;
    g_myPickLo = lo; g_myPickHi = hi;
    wchar_t key[32];
    swprintf_s(key, L"%x_%x", lo, hi);
    WritePrivateProfileStringW(L"Skins", key, g_myPick.empty() ? nullptr : g_myPick.c_str(), IniPath().c_str());
    CosmeticsLoadSkinFile(2, g_myPick.empty() ? L"" : PackPath(g_myPick).c_str());
    Log(L"Cosmetics: my skin for this outfit: %s", g_myPick.empty() ? L"none" : g_myPick.c_str());
}

// online.cpp, starting a match: my skin pack's path (the one picked for the outfit the preview shows), or ""
std::wstring CosmeticsMySkinPath() {
    unsigned lo = 0, hi = 0;
    if (g_myPick.empty() || !CosmeticsPlayerModel(2, 0, &lo, &hi) || lo != g_myPickLo || hi != g_myPickHi) return L"";
    return PackPath(g_myPick);
}

// In a match (netplay.cpp, the online pause menu's Skin row): the skins here for the outfit a player wears now, and
// the one remembered for it (FGUYMod.ini [Skins], as the lobby picks: "" none) - false: their outfit isn't known
bool CosmeticsSkinChoices(int slot, std::vector<SkinChoice>* out, std::wstring* picked) {
    unsigned lo = 0, hi = 0;
    if (!CosmeticsPlayerModel(slot, 0, &lo, &hi)) return false;
    ScanPacks();
    out->clear();
    for (const PackEntry& e : g_packs)
        if (e.lo == lo && e.hi == hi) out->push_back({e.file, e.path, e.name});
    std::wstring f = PickFor(lo, hi);
    *picked = std::any_of(out->begin(), out->end(), [&](const SkinChoice& c) { return c.file == f; }) ? f : L"";
    return true;
}
void CosmeticsRememberSkin(int slot, const std::wstring& file) {   // (that outfit's pick from now on)
    unsigned lo = 0, hi = 0;
    if (CosmeticsPlayerModel(slot, 0, &lo, &hi)) SetPickFor(lo, hi, file);
}

// online.cpp, the lobby: the head picture's id of my skin for the outfit the preview shows ("" none), and a head
// picture by its id - if a pack here has it ("" not)
std::string CosmeticsMySkinKey() {
    static std::wstring lastPick;   // (the preview's model can be unknown for a moment - a character loading: the
    static std::string lastKey;     //  same pick, the same answer)
    unsigned lo = 0, hi = 0;
    if (g_myPick.empty()) return lastPick.clear(), lastKey.clear(), "";
    if (!CosmeticsPlayerModel(2, 0, &lo, &hi)) return g_myPick == lastPick ? lastKey : "";
    if (lo != g_myPickLo || hi != g_myPickHi) return "";
    lastPick = g_myPick;
    lastKey.clear();
    for (const PackEntry& e : g_packs) if (e.file == g_myPick) lastKey = HeadKey(e.head);
    return lastKey;
}
std::string CosmeticsSkinHead(const std::string& key) {
    if (key.empty()) return "";
    for (const PackEntry& e : g_packs) if (!e.head.empty() && HeadKey(e.head) == key) return e.head;
    return "";
}

// FGUYMOD_HeadIcon (the HUD's SetHeadIcon - pause.as2js FGUYHeadIcon): the head picture of the skin a player wears
// ("<movie>|<face left>|<face right>|<face bottom>", fractions of the picture - or the movie alone, or "")
const char* CosmeticsHeadIcon(int player) {
    static std::string out;
    std::lock_guard<std::mutex> l(g_headLock);
    out = player >= 0 && player < 4 ? g_head[player] : "";
    return out.c_str();
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
void CosmeticsOfflineTick();
void CosmeticsFrame(IDirect3DDevice9* dev) {
    (void)dev;
    if (!NetplayEnabled()) CosmeticsOfflineTick();   // (offline: the skins switched on - MODS page)
    if (g_tracing) {   // (a frame traced: done)
        g_tracing = false;
        Log(L"Cosmetics: trace - %d draws with the texture this frame (%d draws in all; with it on stages 0-3: %d %d %d %d)",
            g_traceDraws, g_traceAllDraws, g_traceStageHit[0], g_traceStageHit[1], g_traceStageHit[2], g_traceStageHit[3]);
        for (const auto& k : g_kinds) Log(L"Cosmetics: trace - %d draws of %s", k.second, k.first.c_str());
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
        g_traceDraws = g_traceAllDraws = g_traceOther = 0;
        memset(g_traceStageHit, 0, sizeof(g_traceStageHit));
        g_tracing = g_traceTex != nullptr || (g_traceLo == 0 && g_traceHi <= 2);
        g_kinds.clear();
        Log(L"Cosmetics: trace - the texture is %p", g_traceTex);
        if (!g_tracing) Log(L"Cosmetics: trace - no texture %x:%x here", g_traceLo, g_traceHi);
    }
    ModelsFrame(dev);   // (new characters: models.cpp)
    static ULONGLONG lastPictures = 0;   // (skins' pictures made from the game's own, once their textures are loaded)
    if (GetTickCount64() - lastPictures > 1500) {
        lastPictures = GetTickCount64();
        std::vector<Recipe*> todo;
        {
            std::lock_guard<std::mutex> l(g_recipeLock);
            for (Recipe& r : g_recipes) if (!r.done) todo.push_back(&r);
        }
        for (Recipe* r : todo) {
            IDirect3DTexture9* tex = TextureOf(r->lo, r->hi);
            if (!tex) continue;
            std::vector<unsigned char> rgba;
            unsigned w = 0, h = 0;
            r->done = true;
            if (!SkinBuildRgba(tex, r->ops, r->overlay, rgba, &w, &h)) { Log(L"Cosmetics: a skin's picture couldn't be made"); continue; }
            while (w > 128 && h > 64 && !(w & 1) && !(h & 1)) {   // (halved: about the menus' size, fewer shapes)
                std::vector<unsigned char> half((size_t)(w / 2) * (h / 2) * 4);
                for (unsigned y = 0; y < h / 2; ++y)
                    for (unsigned x = 0; x < w / 2; ++x)
                        for (int k = 0; k < 4; ++k) {
                            const unsigned char* p = &rgba[((size_t)(y * 2) * w + x * 2) * 4 + k];
                            half[((size_t)y * (w / 2) + x) * 4 + k] = (unsigned char)((p[0] + p[4] + p[w * 4] + p[w * 4 + 4] + 2) / 4);
                        }
                rgba.swap(half);
                w /= 2; h /= 2;
            }
            IconsAddPixels(r->key, rgba, w, h);
            Log(L"Cosmetics: a skin's picture made here from the game's %x:%x (%zu recolors%s), %ux%u", r->lo, r->hi,
                r->ops.size() / 7, r->overlay.empty() ? L"" : L", own pixels", w, h);
        }
    }
    if (!g_lockInit) return;
    std::vector<TestCmd> tests;
    std::vector<PendingSkin> pending;
    Lock();
    tests.swap(g_tests);
    pending.swap(g_pending);
    LeaveCriticalSection(&g_lock);
    bool changed = false;
    for (PendingSkin& p : pending) {   // a player's skin: ours made from the pack
        if (Skin* old = g_skins[p.slot]) {
            for (SkinTex& t : old->textures) if (t.mine) t.mine->Release();
            delete old;
            g_skins[p.slot] = nullptr;
        }
        // (a skin pack can carry a model pack - "model.fgmodel": outfit pieces worn with it, models.cpp - put on and
        //  taken off with the skin; online it comes along in the same pack)
        {   // (its head for the HUD - icons.cpp)
            std::vector<unsigned char> head;
            std::string icon = !p.pack.empty() && ZipEntry(p.pack, "head.png", head) ? IconsAdd(head) + HeadFace(p.pack) : "";
            std::lock_guard<std::mutex> l(g_headLock);
            g_head[p.slot] = icon;
        }
        static bool skinModel[4] = {};
        std::vector<unsigned char> model;
        if (!p.pack.empty() && ZipEntry(p.pack, "model.fgmodel", model)) {
            ModelsLoad(p.slot, model, (p.name + L" (its model)").c_str());
            skinModel[p.slot] = true;
        } else if (skinModel[p.slot]) {
            ModelsLoad(p.slot, {}, L"");
            skinModel[p.slot] = false;
        }
        if (!p.pack.empty()) {
            Skin* s = ParsePack(p.pack, p.name);
            if (s) for (SkinTex& t : s->textures) t.mine = MakeTexture(dev, t);
            g_skins[p.slot] = s;
            Log(L"Cosmetics: player %d wears %s (%d textures)%s%s", p.slot, p.name.c_str(), s ? (int)s->textures.size() : 0,
                s ? L"" : L" - not a skin pack", model.empty() ? L"" : L", with its model");
        } else {
            Log(L"Cosmetics: player %d wears no skin", p.slot);
        }
        changed = true;
    }
    static ULONGLONG lastLookup = 0;   // (the game's textures, looked up now and then: a level loads new ones)
    ULONGLONG now = GetTickCount64();
    if (changed || now - lastLookup > 2000) {
        lastLookup = now;
        bool any = false;
        for (Skin* s : g_skins)
            if (s) for (SkinTex& t : s->textures) {
                IDirect3DTexture9* g = TextureOf(t.lo, t.hi);
                if (g != t.game) { t.game = g; changed = true; }
                if (t.recipe && g && !t.mine) {   // (made from this player's own game texture, once it's here)
                    if (SkinBuildTexture(g, t.ops, t.overlay, t.dds)) t.mine = MakeTexture(dev, t);
                    Log(L"Cosmetics: skin texture %x:%x made here (%zu recolors%s)%s", t.lo, t.hi, t.ops.size() / 7,
                        t.overlay.empty() ? L"" : L", own pixels", t.mine ? L"" : L" - FAILED");
                    t.dds.clear();
                    t.dds.shrink_to_fit();
                    changed = true;
                }
                any = true;
            }
        if (changed) RebuildSwaps();
        if (!any) g_swaps.clear();
    }
    if (!g_swaps.empty() || ModelsWorn()) {   // where the players are (for which player a draw is of)
        unsigned char* players[8];
        int n = GamePlayers(players, 8);
        memset(g_playerHere, 0, sizeof(g_playerHere));
        for (int i = 0; i < n; ++i) {
            int slot = *reinterpret_cast<int*>(players[i] + 0xdf8);
            auto xf = *reinterpret_cast<unsigned char**>(players[i] + 0x38);
            if (slot < 0 || slot >= 4 || !xf || IsBadReadPtr(xf + 0x30, 12)) continue;
            memcpy(g_playerPos[slot], xf + 0x30, 12);
            g_playerHere[slot] = true;
        }
    }
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
