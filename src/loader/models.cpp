// Models (the FGUY Cosmetics mod's new characters): a model pack (.fgmodel - tools/re/glb2fgmodel.py, later the Skin
// Studio) worn by a player is drawn instead of their character. The pack: a stored zip - model.json (its name, the
// outfits it replaces "models": [[lo, hi], ...]), mesh.bin (parts, vertices moved by up to 4 bones of that character's
// skeleton, triangles, the skeleton's parents and inverse bind matrices) and its textures (PNGs: its own art).
// Each frame: the player's pose as the game built it (motion.cpp MotionPose: emotes too) makes the bone matrices -
// as the game's 0x4fac80 does (a bone: its parent's times its local rotation, translation, scale) - which skin the
// model's vertices on the CPU into a buffer of ours, as the game does its own (motion.cpp). Then a draw of that player's
// skinned character (cosmetics.cpp: its position stream is the game's per-frame buffer, the draw at that player's
// position) is ours instead: the game's own shader, camera and lights, our vertices (the same layout: position,
// normal as TEXCOORD1, uv as TEXCOORD0), our textures on stage 1; their character's other pieces aren't drawn.
// Test pipe: "modelload <slot> <file.fgmodel | ''>".
#include "common.h"

#include <d3d9.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

struct Vert { float pos[3], nrm[3], uv[2]; unsigned char bone[4], weight[4]; };
struct Part { unsigned first, count, texture; };
struct Model {
    std::wstring name;
    int skel = 0;                          // its skeleton's bones (the game's pose has one more: the root)
    std::vector<int> parent;
    std::vector<float> invBind;            // skel x 12
    std::vector<Part> parts;
    std::vector<Vert> verts;
    std::vector<unsigned short> idx;
    std::vector<std::vector<unsigned char>> pngs;
    // (the render thread's)
    std::vector<IDirect3DTexture9*> tex;
    IDirect3DVertexBuffer9* skinned = nullptr;   // positions, then normals - every frame
    IDirect3DVertexBuffer9* uv = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    bool made = false, failed = false;
    unsigned frame = 0;                    // (skinned for this frame)
};
Model* g_worn[4] = {};
struct Pending { int slot; std::vector<unsigned char> pack; std::wstring name; };
std::vector<Pending> g_pending;
CRITICAL_SECTION& Cs() { static struct L { L() { InitializeCriticalSection(&c); } CRITICAL_SECTION c; } l; return l.c; }
IDirect3DVertexDeclaration9* g_decl = nullptr;
unsigned g_frame = 1;
UINT g_firstOffset[4];                     // (this frame: the stream offset of the first piece of theirs drawn - its
unsigned g_firstFrame[4];                  //  next draws, another pass, are ours again; their other pieces skipped)

void Free(Model* m) {
    if (!m) return;
    for (IDirect3DTexture9* t : m->tex) if (t) t->Release();
    if (m->skinned) m->skinned->Release();
    if (m->uv) m->uv->Release();
    if (m->ib) m->ib->Release();
    delete m;
}

Model* Parse(const std::vector<unsigned char>& pack, const std::wstring& name) {
    std::vector<unsigned char> mesh, json;
    if (!ZipRead(pack, "mesh.bin", mesh) || !ZipRead(pack, "model.json", json) || mesh.size() < 24) return nullptr;
    const unsigned char* d = mesh.data();
    int head[6];
    memcpy(head, d, sizeof(head));
    if (head[0] != 0x444d4746 || head[1] != 1) return nullptr;   // "FGMD"
    int S = head[2], P = head[3], V = head[4], I = head[5];
    size_t need = 24 + S + (size_t)S * 48 + (size_t)P * 12 + (size_t)V * sizeof(Vert) + (size_t)I * 2;
    if (S <= 0 || S > 254 || P <= 0 || V <= 0 || V > 65535 || I <= 0 || mesh.size() < need) return nullptr;
    auto m = new Model;
    m->name = name;
    m->skel = S;
    size_t o = 24;
    for (int s = 0; s < S; ++s) m->parent.push_back((signed char)d[o + s]);
    o += S;
    m->invBind.resize((size_t)S * 12);
    memcpy(m->invBind.data(), d + o, (size_t)S * 48);
    o += (size_t)S * 48;
    m->parts.resize(P);
    memcpy(m->parts.data(), d + o, (size_t)P * 12);
    o += (size_t)P * 12;
    m->verts.resize(V);
    memcpy(m->verts.data(), d + o, (size_t)V * sizeof(Vert));
    o += (size_t)V * sizeof(Vert);
    m->idx.resize(I);
    memcpy(m->idx.data(), d + o, (size_t)I * 2);
    for (const Part& p : m->parts)
        if ((size_t)p.first + p.count > (size_t)I) { delete m; return nullptr; }
    for (unsigned short i : m->idx)
        if (i >= V) { delete m; return nullptr; }
    for (int s = 0; s < S; ++s)   // (parents before their children: one pass makes the matrices)
        if (m->parent[s] >= s) { delete m; return nullptr; }
    std::string js(json.begin(), json.end());   // its textures: "textures": ["textures/0.png", ...]
    size_t at = js.find("\"textures\"");
    if (at != std::string::npos)
        for (size_t q1 = js.find('"', js.find('[', at) + 1); q1 != std::string::npos && q1 < js.find(']', at); ) {
            size_t q2 = js.find('"', q1 + 1);
            std::vector<unsigned char> png;
            ZipRead(pack, js.substr(q1 + 1, q2 - q1 - 1), png);
            m->pngs.push_back(std::move(png));
            q1 = js.find('"', q2 + 1);
        }
    return m;
}

// (the render thread) its buffers and textures
void Make(IDirect3DDevice9* dev, Model* m) {
    m->made = true;
    UINT V = (UINT)m->verts.size();
    if (FAILED(dev->CreateVertexBuffer(V * 24, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m->skinned, nullptr)) ||
        FAILED(dev->CreateVertexBuffer(V * 8, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m->uv, nullptr)) ||
        FAILED(dev->CreateIndexBuffer((UINT)m->idx.size() * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &m->ib, nullptr))) {
        m->failed = true;
        return;
    }
    void* p = nullptr;
    if (SUCCEEDED(m->uv->Lock(0, 0, &p, 0))) {
        float* f = static_cast<float*>(p);
        for (UINT i = 0; i < V; ++i) { f[i * 2] = m->verts[i].uv[0]; f[i * 2 + 1] = m->verts[i].uv[1]; }
        m->uv->Unlock();
    }
    if (SUCCEEDED(m->ib->Lock(0, 0, &p, 0))) {
        memcpy(p, m->idx.data(), m->idx.size() * 2);
        m->ib->Unlock();
    }
    for (const auto& png : m->pngs) {
        std::vector<unsigned char> dds;
        m->tex.push_back(!png.empty() && SkinPngToDds(png, dds) ? CosmeticsTextureFromDds(dev, dds) : nullptr);
    }
    if (!g_decl) {
        D3DVERTEXELEMENT9 el[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {1, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},   // (the normal, as the game's)
            {2, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()};
        dev->CreateVertexDeclaration(el, &g_decl);
    }
    Log(L"Models: %s - %zu vertices, %zu triangles, %zu parts, %zu textures, %d bones", m->name.c_str(), m->verts.size(),
        m->idx.size() / 3, m->parts.size(), m->tex.size(), m->skel);
}

// 3x4 row-major: a = b * c
void Mul(float* a, const float* b, const float* c) {
    float r[12];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = b[i * 4] * c[j] + b[i * 4 + 1] * c[4 + j] + b[i * 4 + 2] * c[8 + j] + (j == 3 ? b[i * 4 + 3] : 0);
    memcpy(a, r, sizeof(r));
}

// a bone's local matrix from its rotation (x y z w), translation and scale: T * R * S
void Local(const float* trs, float* m) {
    float x = trs[0], y = trs[1], z = trs[2], w = trs[3];
    float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                  2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                  2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = R[i * 3 + j] * trs[7 + j];
        m[i * 4 + 3] = trs[4 + i];
    }
}

// the model's vertices, as the player stands this frame, into its buffer
bool Skin(int slot, Model* m) {
    std::vector<float> pose;
    int bones = 0;
    if (!MotionPose(slot, pose, &bones) || bones != m->skel + 1) return false;
    std::vector<float> world((size_t)bones * 12), skin((size_t)m->skel * 12);
    Local(&pose[0], &world[0]);   // (the game's bone 0: the character's root)
    for (int s = 0; s < m->skel; ++s) {
        float local[12];
        Local(&pose[(size_t)(s + 1) * 10], local);
        int par = m->parent[s] >= 0 ? m->parent[s] + 1 : 0;
        Mul(&world[(size_t)(s + 1) * 12], &world[(size_t)par * 12], local);
        Mul(&skin[(size_t)s * 12], &world[(size_t)(s + 1) * 12], &m->invBind[(size_t)s * 12]);
    }
    void* p = nullptr;
    UINT V = (UINT)m->verts.size();
    if (FAILED(m->skinned->Lock(0, 0, &p, 0))) return false;
    float* pos = static_cast<float*>(p);
    float* nrm = pos + V * 3;
    for (UINT i = 0; i < V; ++i) {
        const Vert& v = m->verts[i];
        float op[3] = {}, on[3] = {};
        for (int k = 0; k < 4; ++k) {
            if (!v.weight[k] || v.bone[k] >= m->skel) continue;
            const float* M = &skin[(size_t)v.bone[k] * 12];
            float w = v.weight[k] / 255.0f;
            for (int r = 0; r < 3; ++r) {
                op[r] += w * (M[r * 4] * v.pos[0] + M[r * 4 + 1] * v.pos[1] + M[r * 4 + 2] * v.pos[2] + M[r * 4 + 3]);
                on[r] += w * (M[r * 4] * v.nrm[0] + M[r * 4 + 1] * v.nrm[1] + M[r * 4 + 2] * v.nrm[2]);
            }
        }
        memcpy(pos + i * 3, op, 12);
        memcpy(nrm + i * 3, on, 12);
    }
    m->skinned->Unlock();
    return true;
}

}  // namespace

void ModelsLoad(int slot, const std::vector<unsigned char>& pack, const wchar_t* name) {
    if (slot < 0 || slot >= 4) return;
    EnterCriticalSection(&Cs());
    g_pending.push_back({slot, pack, name ? name : L""});
    LeaveCriticalSection(&Cs());
}

void ModelsLoadFile(int slot, const wchar_t* path) {
    std::vector<unsigned char> d;
    if (path && *path) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(f, nullptr), got = 0;
            d.resize(size < 64 * 1024 * 1024 ? size : 0);
            if (!ReadFile(f, d.data(), (DWORD)d.size(), &got, nullptr) || got != d.size()) d.clear();
            CloseHandle(f);
        }
        if (d.empty()) { Log(L"Models: can't read %s", path); return; }
    }
    ModelsLoad(slot, d, path);
}

bool ModelsWorn() {
    for (Model* m : g_worn) if (m) return true;
    return false;
}

// cosmetics.cpp, after each frame (the render thread): new models in
void ModelsFrame(IDirect3DDevice9* dev) {
    ++g_frame;
    std::vector<Pending> pending;
    EnterCriticalSection(&Cs());
    pending.swap(g_pending);
    LeaveCriticalSection(&Cs());
    for (Pending& p : pending) {
        Free(g_worn[p.slot]);
        g_worn[p.slot] = p.pack.empty() ? nullptr : Parse(p.pack, p.name);
        if (!p.pack.empty() && !g_worn[p.slot]) Log(L"Models: %s isn't a model pack this version can use", p.name.c_str());
        if (g_worn[p.slot]) Make(dev, g_worn[p.slot]);
        Log(L"Models: player %d wears %s", p.slot, g_worn[p.slot] ? p.name.c_str() : L"their own character");
    }
}

// cosmetics.cpp's DrawIndexedPrimitive: a worn model's player's skinned character - ours instead. True: done (hr set)
bool ModelsDraw(IDirect3DDevice9* dev, int player, ModelsRealDraw real, HRESULT* hr) {
    if (player < 0 || player >= 4 || !g_worn[player] || g_worn[player]->failed || !g_decl) return false;
    Model* m = g_worn[player];
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(0, &vb, &off, &stride)) || !vb) return false;
    D3DVERTEXBUFFER_DESC d{};
    vb->GetDesc(&d);
    vb->Release();
    if (stride != 12 || !(d.Usage & D3DUSAGE_DYNAMIC)) return false;   // (not a skinned character's: a weapon, a prop)
    if (g_firstFrame[player] != g_frame) { g_firstFrame[player] = g_frame; g_firstOffset[player] = off; }
    *hr = D3D_OK;
    if (off != g_firstOffset[player]) return true;   // (another piece of theirs: ours has it all)
    if (m->frame != g_frame) {
        if (!Skin(player, m)) return false;   // (no pose: their own character, this frame)
        m->frame = g_frame;
    }
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    IDirect3DVertexBuffer9* oldVb[3] = {};
    UINT oldOff[3] = {}, oldStride[3] = {};
    IDirect3DIndexBuffer9* oldIb = nullptr;
    IDirect3DBaseTexture9* oldTex = nullptr;
    dev->GetVertexDeclaration(&oldDecl);
    for (UINT s = 0; s < 3; ++s) dev->GetStreamSource(s, &oldVb[s], &oldOff[s], &oldStride[s]);
    dev->GetIndices(&oldIb);
    dev->GetTexture(1, &oldTex);
    UINT V = (UINT)m->verts.size();
    dev->SetVertexDeclaration(g_decl);
    dev->SetStreamSource(0, m->skinned, 0, 12);
    dev->SetStreamSource(1, m->skinned, V * 12, 12);
    dev->SetStreamSource(2, m->uv, 0, 8);
    dev->SetIndices(m->ib);
    for (const Part& p : m->parts) {
        if (p.texture < m->tex.size() && m->tex[p.texture]) dev->SetTexture(1, m->tex[p.texture]);
        *hr = real(dev, (int)D3DPT_TRIANGLELIST, 0, 0, V, p.first, p.count / 3);
    }
    dev->SetVertexDeclaration(oldDecl);
    for (UINT s = 0; s < 3; ++s) dev->SetStreamSource(s, oldVb[s], oldOff[s], oldStride[s]);
    dev->SetIndices(oldIb);
    dev->SetTexture(1, oldTex);
    if (oldDecl) oldDecl->Release();
    for (UINT s = 0; s < 3; ++s) if (oldVb[s]) oldVb[s]->Release();
    if (oldIb) oldIb->Release();
    if (oldTex) oldTex->Release();
    return true;
}
