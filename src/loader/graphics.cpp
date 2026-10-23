// Graphics settings: a sharper picture, texture filtering, anti-aliasing, sharpening and brightness.
//
// * Sharp picture: the game draws everything half a pixel off (Direct3D 9's pixel centres, which the console port never
//   made up for), so each of its full-screen passes (glow, ambient occlusion, depth of field, the final copy) blends
//   four texels where it meant one - the game's well-known blur (it has no anti-aliasing on PC: its MSAAType setting is
//   never used). Every vertex shader gets the half pixel back (the half-pixel fix, below): the picture as the consoles
//   draw it.
// * Texture filtering: the game's own bilinear, trilinear, or anisotropic 2x-16x for every other texture (floors and
//   walls seen at an angle stay sharp).
// * Anti-aliasing (FXAA), sharpening (contrast-adaptive) and brightness: our own pass on the finished frame, just
//   before it's shown. The shaders are compiled at start by Windows' own d3dcompiler_47.dll.
// Works the same on DXVK and plain Direct3D 9 (only the device's own methods). Settings: FGUYMod.ini [Graphics] -
// environment variables override them for test runs.
//   SharpPicture     = 1 | 0            (FGUYMOD_GFX_SHARP)
//   TextureFiltering = 0 the game's | 1 trilinear | 2, 4, 8, 16 anisotropic   (FGUYMOD_GFX_FILTER)
//   AntiAliasing     = 0 off | 1 FXAA   (FGUYMOD_GFX_AA)
//   Sharpening       = 0..100           (FGUYMOD_GFX_SHARPEN)
//   Brightness       = 0..100, 50 the game's   (FGUYMOD_GFX_BRIGHT)

#include "common.h"

#include <d3d9.h>
#include <d3dcommon.h>

#include <cmath>
#include <cstring>

namespace {

volatile int g_sharp = 1, g_filter = 16, g_aa = 0, g_sharpen = 0, g_bright = 50;
volatile bool g_statesDirty = false;   // (a filtering setting changed: every stage again, on the render thread)
IDirect3DDevice9* g_dev = nullptr;
UINT g_bbW = 0, g_bbH = 0;
DWORD g_maxAniso = 16;   // (the device's)

// ---- sampler states: the game's wishes, and what we put instead ------------------------------------------------
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetSamplerState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
SetTexture_t Real_SetTexture;
SetSamplerState_t Real_SetSamplerState;
const int kStages = 16;
DWORD g_want[kStages][3];      // (per stage: the game's MAG / MIN / MIP filter)
bool g_screen[kStages];        // (a screen-sized render target is set there: its filtering stays the game's)

int Slot(D3DSAMPLERSTATETYPE t) {
    return t == D3DSAMP_MAGFILTER ? 0 : t == D3DSAMP_MINFILTER ? 1 : t == D3DSAMP_MIPFILTER ? 2 : -1;
}
const D3DSAMPLERSTATETYPE kType[3] = {D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER};

// what a filter the game asks for becomes on a stage
DWORD Filter(int stage, int k, DWORD want) {
    if (g_screen[stage]) return want;
    int f = g_filter;
    if (k == 1 && want == D3DTEXF_LINEAR && f >= 2) return D3DTEXF_ANISOTROPIC;
    if (k == 2 && want == D3DTEXF_POINT && f >= 1) return D3DTEXF_LINEAR;
    return want;
}

void ApplyStage(IDirect3DDevice9* dev, int stage) {
    for (int k = 0; k < 3; ++k) Real_SetSamplerState(dev, stage, kType[k], Filter(stage, k, g_want[stage][k]));
}

void ResetStages(IDirect3DDevice9* dev) {   // (a new device, or one just reset: the default states)
    for (int s = 0; s < kStages; ++s) {
        g_want[s][0] = g_want[s][1] = D3DTEXF_POINT;
        g_want[s][2] = D3DTEXF_NONE;
        g_screen[s] = false;
        Real_SetSamplerState(dev, s, D3DSAMP_MAXANISOTROPY, g_filter >= 2 ? min((DWORD)g_filter, g_maxAniso) : 1);
    }
}

void* Patch(void* object, int slot, void* hook) {   // (a device method ours: the one it was)
    void** vtable = *reinterpret_cast<void***>(object);
    if (vtable[slot] == hook) return nullptr;
    DWORD old;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old);
    void* original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return original;
}

bool IsScreenTarget(IDirect3DBaseTexture9* tex) {
    if (!tex || tex->GetType() != D3DRTYPE_TEXTURE) return false;
    D3DSURFACE_DESC d;
    if (FAILED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d))) return false;
    return (d.Usage & D3DUSAGE_RENDERTARGET) && d.Width == g_bbW && d.Height == g_bbH;
}

HRESULT STDMETHODCALLTYPE Hook_SetSamplerState(IDirect3DDevice9* self, DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value) {
    int k = Slot(type);
    if (stage >= (DWORD)kStages || k < 0) return Real_SetSamplerState(self, stage, type, value);
    g_want[stage][k] = value;
    return Real_SetSamplerState(self, stage, type, Filter(stage, k, value));
}

HRESULT STDMETHODCALLTYPE Hook_SetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* tex) {
    HRESULT hr = Real_SetTexture(self, stage, tex);
    if (stage < (DWORD)kStages) {
        bool screen = IsScreenTarget(tex);
        if (screen != g_screen[stage]) {
            g_screen[stage] = screen;
            ApplyStage(self, stage);
        }
    }
    return hr;
}

// ---- the half-pixel fix (Sharp picture) ---------------------------------------------------------------------------
// Direct3D 9 puts pixel centres half a pixel off from where Direct3D 10+, OpenGL and the consoles put them. A console
// port that doesn't make up for it draws every full-screen pass half a pixel off - bilinear filtering then blends four
// texels where one was meant: the blur. The fix (as in aras-p.info's "Solving DX9 Half-Pixel Offset"): every vertex
// shader made to end with oPos.xy += c[kHalf].xy * oPos.w, c[kHalf] = (-1 / viewport width, 1 / viewport height) -
// everything lands where the console draws it. c[kHalf] = 0 is the game as it was: on / off at once.
const DWORD kHalf = 255;
int g_vsSeen = 0, g_vsPatched = 0;
float g_halfConst[4] = {0, 0, 0, 0};

int RegType(DWORD t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
DWORD RegNum(DWORD t) { return t & 0x7FF; }
DWORD RegTok(int type, DWORD num) { return 0x80000000u | ((DWORD)(type & 7) << 28) | ((DWORD)(type & 0x18) << 8) | num; }

// the shader with the fix at its end - false: left as it is (vs_1_x, no position output, no free temp, c255 used)
bool PatchVertexShader(const DWORD* in, std::vector<DWORD>& out, const char** why) {
    DWORD ver = in[0];
    int major = (int)((ver >> 8) & 0xFF);
    if ((ver & 0xFFFF0000) != 0xFFFE0000 || major < 2) { *why = "not vs_2/3"; return false; }
    int posType = major >= 3 ? -1 : 4;   // (vs_2: oPos - vs_3: the output declared as position)
    DWORD posNum = 0;
    int maxTemp = -1;
    size_t i = 1, n = 0;
    for (; in[i] != 0x0000FFFF && n < 200000; ++n) {
        DWORD tok = in[i], op = tok & 0xFFFF;
        if (op == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }   // (a comment)
        DWORD len = (tok >> 24) & 0xF;
        if (op == 31) {   // dcl: usage, register
            if (major >= 3 && RegType(in[i + 2]) == 6 && (in[i + 1] & 0x1F) == 0 && ((in[i + 1] >> 16) & 0xF) == 0) {
                posType = 6;   // (D3DSPR_OUTPUT)
                posNum = RegNum(in[i + 2]);
            }
        } else if (op != 81 && op != 48 && op != 47) {   // (def / defi / defb: values, not registers)
            for (DWORD k = 1; k <= len; ++k) {
                DWORD t = in[i + k];
                if (!(t & 0x80000000)) continue;
                if (RegType(t) == 0 && (int)RegNum(t) > maxTemp) maxTemp = (int)RegNum(t);
                if (RegType(t) == 2 && RegNum(t) == kHalf) { *why = "uses c255"; return false; }
            }
        }
        i += 1 + len;
    }
    if (in[i] != 0x0000FFFF) { *why = "no end"; return false; }
    if (posType < 0) { *why = "no position output"; return false; }
    DWORD temp = (DWORD)(maxTemp + 1);
    if (temp >= (major >= 3 ? 32u : 12u)) { *why = "no free temp"; return false; }
    out.assign(in, in + i);
    for (size_t j = 1; j < out.size();) {   // every write to the position: to the temp instead
        DWORD tok = out[j], op = tok & 0xFFFF;
        if (op == 0xFFFE) { j += 1 + ((tok >> 16) & 0x7FFF); continue; }
        DWORD len = (tok >> 24) & 0xF;
        if (op != 31 && op != 81 && op != 48 && op != 47 && len >= 1) {
            DWORD d = out[j + 1];
            if ((d & 0x80000000) && RegType(d) == posType && RegNum(d) == posNum && !(d & 0x2000))
                out[j + 1] = RegTok(0, temp) | (d & 0x00FF0000);
        }
        j += 1 + len;
    }
    DWORD pos = RegTok(posType, posNum), t = RegTok(0, temp);
    out.insert(out.end(), {4u | (4u << 24), pos | (0x3u << 16), t | (0xFFu << 16), RegTok(2, kHalf) | (0xE4u << 16),
                           t | (0xE4u << 16)});                                 // mad oPos.xy, t.w, c255, t
    out.insert(out.end(), {1u | (2u << 24), pos | (0xCu << 16), t | (0xE4u << 16)});   // mov oPos.zw, t
    out.push_back(0x0000FFFF);
    return true;
}

using CreateVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetVSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
CreateVertexShader_t Real_CreateVertexShader;
SetViewport_t Real_SetViewport;
SetVSConstF_t Real_SetVSConstF;

void PutHalf(IDirect3DDevice9* dev, DWORD w, DWORD h) {
    bool on = g_sharp && w && h;
    g_halfConst[0] = on ? -1.0f / w : 0;
    g_halfConst[1] = on ? 1.0f / h : 0;
    Real_SetVSConstF(dev, kHalf, g_halfConst, 1);
}

HRESULT STDMETHODCALLTYPE Hook_CreateVertexShader(IDirect3DDevice9* self, const DWORD* code, IDirect3DVertexShader9** out) {
    std::vector<DWORD> patched;
    const char* why = "";
    ++g_vsSeen;
    if (code && PatchVertexShader(code, patched, &why)) {
        HRESULT hr = Real_CreateVertexShader(self, patched.data(), out);
        if (SUCCEEDED(hr)) { ++g_vsPatched; return hr; }
        why = "refused";
    }
    static int told = 0;
    if (code && told++ < 20) Log(L"Graphics: vertex shader %08lx left as it is (%S, from %p)", code[0], why, _ReturnAddress());
    return Real_CreateVertexShader(self, code, out);
}

HRESULT STDMETHODCALLTYPE Hook_SetViewport(IDirect3DDevice9* self, const D3DVIEWPORT9* vp) {
    HRESULT hr = Real_SetViewport(self, vp);
    if (vp) PutHalf(self, vp->Width, vp->Height);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SetVSConstF(IDirect3DDevice9* self, UINT start, const float* data, UINT count) {
    HRESULT hr = Real_SetVSConstF(self, start, data, count);
    if (start <= kHalf && start + count > kHalf) {   // (the game's own over ours: ours again)
        static int told = 0;
        if (told++ < 5) Log(L"Graphics: the game sets vertex constants %u-%u (over c%u)", start, start + count - 1, kHalf);
        Real_SetVSConstF(self, kHalf, g_halfConst, 1);
    }
    return hr;
}

// ---- our pass on the finished frame -----------------------------------------------------------------------------
const char kFxaa[] = R"(
sampler2D s : register(s0);
float4 px : register(c0);
float L(float2 uv) { return dot(tex2Dlod(s, float4(uv, 0, 0)).rgb, float3(0.299, 0.587, 0.114)); }
float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float3 rgbM = tex2Dlod(s, float4(uv, 0, 0)).rgb;
    float lM = dot(rgbM, float3(0.299, 0.587, 0.114));
    float lN = L(uv + float2(0, -px.y)), lS = L(uv + float2(0, px.y)), lE = L(uv + float2(px.x, 0)), lW = L(uv + float2(-px.x, 0));
    float hi = max(max(max(lN, lS), max(lE, lW)), lM), lo = min(min(min(lN, lS), min(lE, lW)), lM), range = hi - lo;
    if (range < max(0.0625, hi * 0.166)) return float4(rgbM, 1);
    float lNW = L(uv + float2(-px.x, -px.y)), lNE = L(uv + float2(px.x, -px.y));
    float lSW = L(uv + float2(-px.x, px.y)), lSE = L(uv + float2(px.x, px.y));
    float sub = (2 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12;
    sub = smoothstep(0, 1, saturate(abs(sub - lM) / range));
    sub = sub * sub * 0.25;   // (a little: the game's art is drawn lines - more smears them)
    float eH = abs(lNW + lNE - 2 * lN) + 2 * abs(lW + lE - 2 * lM) + abs(lSW + lSE - 2 * lS);
    float eV = abs(lNW + lSW - 2 * lW) + 2 * abs(lN + lS - 2 * lM) + abs(lNE + lSE - 2 * lE);
    bool horz = eH >= eV;
    float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
    float g1 = abs(l1 - lM), g2 = abs(l2 - lM);
    float stepLen = horz ? px.y : px.x, avg;
    if (g1 >= g2) { stepLen = -stepLen; avg = 0.5 * (l1 + lM); } else avg = 0.5 * (l2 + lM);
    float scaled = 0.25 * max(g1, g2);
    float2 c = uv;
    if (horz) c.y += stepLen * 0.5; else c.x += stepLen * 0.5;
    float2 off = horz ? float2(px.x, 0) : float2(0, px.y);
    float2 u1 = c - off, u2 = c + off;
    float e1 = L(u1) - avg, e2 = L(u2) - avg;
    bool r1 = abs(e1) >= scaled, r2 = abs(e2) >= scaled;
    float q[8] = {1, 1, 1.5, 2, 2, 4, 4, 8};
    [unroll] for (int i = 0; i < 8; i++) {
        if (!r1) { u1 -= off * q[i]; e1 = L(u1) - avg; r1 = abs(e1) >= scaled; }
        if (!r2) { u2 += off * q[i]; e2 = L(u2) - avg; r2 = abs(e2) >= scaled; }
    }
    float d1 = horz ? uv.x - u1.x : uv.y - u1.y, d2 = horz ? u2.x - uv.x : u2.y - uv.y;
    bool near1 = d1 < d2;
    float pixOff = -min(d1, d2) / (d1 + d2) + 0.5;
    bool ok = ((near1 ? e1 : e2) < 0) != (lM < avg);
    float f = max(ok ? pixOff : 0, sub);
    float2 fu = uv;
    if (horz) fu.y += f * stepLen; else fu.x += f * stepLen;
    return float4(tex2Dlod(s, float4(fu, 0, 0)).rgb, 1);
}
)";

// contrast-adaptive sharpening (after AMD's CAS) and brightness: c0 = 1/size, c1 = (sharpness 0..1, gamma)
const char kFinal[] = R"(
sampler2D s : register(s0);
float4 px : register(c0);
float4 opt : register(c1);
float3 T(float2 uv) { return tex2Dlod(s, float4(uv, 0, 0)).rgb; }
float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float3 e = T(uv);
    if (opt.x > 0) {
        float3 a = T(uv + float2(-px.x, -px.y)), b = T(uv + float2(0, -px.y)), c = T(uv + float2(px.x, -px.y));
        float3 d = T(uv + float2(-px.x, 0)), f = T(uv + float2(px.x, 0));
        float3 g = T(uv + float2(-px.x, px.y)), h = T(uv + float2(0, px.y)), i = T(uv + float2(px.x, px.y));
        float3 mn = min(min(min(d, e), min(f, b)), h);
        mn += min(mn, min(min(a, c), min(g, i)));
        float3 mx = max(max(max(d, e), max(f, b)), h);
        mx += max(mx, max(max(a, c), max(g, i)));
        float3 amp = sqrt(saturate(min(mn, 2 - mx) / max(mx, 0.0001)));
        float3 w = amp * (-1 / lerp(8, 5, opt.x));
        float3 lo = min(min(min(a, b), min(c, d)), min(min(e, f), min(min(g, h), i)));
        float3 hi = max(max(max(a, b), max(c, d)), max(max(e, f), max(max(g, h), i)));
        // (never past its neighbours: no light rim beside the game's black outlines)
        e = clamp((b * w + d * w + f * w + h * w + e) / (1 + 4 * w), lo, hi);
    }
    return float4(pow(e, opt.y), 1);
}
)";

using D3DCompile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                      UINT, ID3DBlob**, ID3DBlob**);
IDirect3DPixelShader9 *g_fxaa = nullptr, *g_final = nullptr;
IDirect3DTexture9* g_copy = nullptr;    // (the frame, copied for a pass to read)
IDirect3DStateBlock9* g_state = nullptr;
bool g_shadersTried = false, g_broken = false;

IDirect3DPixelShader9* Compile(IDirect3DDevice9* dev, D3DCompile_t compile, const char* src, const char* name) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", "ps_3_0", 1 << 15 /* optimization level 3 */, 0,
                         &code, &errors);
    if (SUCCEEDED(hr) && code) dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
    if (!ps) Log(L"Graphics: the %S shader didn't compile (%#lx): %S", name, hr,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    if (code) code->Release();
    if (errors) errors->Release();
    return ps;
}

bool Ready(IDirect3DDevice9* dev) {
    if (g_broken) return false;
    if (!g_shadersTried) {
        g_shadersTried = true;
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = m ? reinterpret_cast<D3DCompile_t>(GetProcAddress(m, "D3DCompile")) : nullptr;
        if (!compile) { Log(L"Graphics: no d3dcompiler_47.dll - no anti-aliasing, sharpening or brightness"); g_broken = true; return false; }
        g_fxaa = Compile(dev, compile, kFxaa, "FXAA");
        g_final = Compile(dev, compile, kFinal, "sharpening");
        if (!g_fxaa || !g_final) { g_broken = true; return false; }
        Log(L"Graphics: shaders ready");
    }
    if (!g_copy && FAILED(dev->CreateTexture(g_bbW, g_bbH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_copy, nullptr))) {
        g_copy = nullptr;
        return false;
    }
    if (!g_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state))) { g_state = nullptr; return false; }
    return true;
}

void ReleaseTargets() {
    if (g_copy) { g_copy->Release(); g_copy = nullptr; }
    if (g_state) { g_state->Release(); g_state = nullptr; }
}

// one pass: the back buffer copied out, drawn back through the shader
void Pass(IDirect3DDevice9* dev, IDirect3DSurface9* back, IDirect3DPixelShader9* ps, const float* opt) {
    IDirect3DSurface9* copy = nullptr;
    if (FAILED(g_copy->GetSurfaceLevel(0, &copy))) return;
    dev->StretchRect(back, nullptr, copy, nullptr, D3DTEXF_NONE);
    copy->Release();
    float px[4] = {1.0f / g_bbW, 1.0f / g_bbH, 0, 0};
    dev->SetPixelShaderConstantF(0, px, 1);
    if (opt) dev->SetPixelShaderConstantF(1, opt, 1);
    dev->SetPixelShader(ps);
    Real_SetTexture(dev, 0, g_copy);
    Real_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    Real_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    Real_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    struct V { float x, y, z, w, u, v; };
    float w = (float)g_bbW - 0.5f, h = (float)g_bbH - 0.5f;   // (half a pixel back: texels and pixels line up)
    V q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w, -0.5f, 0, 1, 1, 0}, {-0.5f, h, 0, 1, 0, 1}, {w, h, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

}  // namespace

void GraphicsLoadSettings() {
    g_sharp = SettingInt(L"Graphics", L"SharpPicture", L"FGUYMOD_GFX_SHARP", 1) != 0;
    int f = SettingInt(L"Graphics", L"TextureFiltering", L"FGUYMOD_GFX_FILTER", 16);
    g_filter = f >= 16 ? 16 : f >= 8 ? 8 : f >= 4 ? 4 : f >= 2 ? 2 : f >= 1 ? 1 : 0;
    g_aa = SettingInt(L"Graphics", L"AntiAliasing", L"FGUYMOD_GFX_AA", 0) ? 1 : 0;
    int s = SettingInt(L"Graphics", L"Sharpening", L"FGUYMOD_GFX_SHARPEN", 0);
    g_sharpen = s < 0 ? 0 : s > 100 ? 100 : s;
    int b = SettingInt(L"Graphics", L"Brightness", L"FGUYMOD_GFX_BRIGHT", 50);
    g_bright = b < 0 ? 0 : b > 100 ? 100 : b;
    Log(L"Graphics: sharp picture %s, texture filtering %d, anti-aliasing %s, sharpening %d, brightness %d", g_sharp ? L"on" : L"off",
        g_filter, g_aa ? L"FXAA" : L"off", g_sharpen, g_bright);
}

// d3d9.cpp: the game's device (before the skins' hooks: theirs go through ours)
void GraphicsDeviceCreated(IDirect3DDevice9* dev, UINT w, UINT h) {
    GraphicsLoadSettings();
    g_dev = dev;
    g_bbW = w;
    g_bbH = h;
    D3DCAPS9 caps = {};
    if (SUCCEEDED(dev->GetDeviceCaps(&caps)) && caps.MaxAnisotropy) g_maxAniso = caps.MaxAnisotropy;
    if (void* o = Patch(dev, 65, (void*)Hook_SetTexture)) Real_SetTexture = (SetTexture_t)o;
    if (void* o = Patch(dev, 69, (void*)Hook_SetSamplerState)) Real_SetSamplerState = (SetSamplerState_t)o;
    // (the shaders always get the fix, so Sharp picture switches at once: off is c255 = 0)
    if (void* o = Patch(dev, 91, (void*)Hook_CreateVertexShader)) Real_CreateVertexShader = (CreateVertexShader_t)o;
    if (void* o = Patch(dev, 47, (void*)Hook_SetViewport)) Real_SetViewport = (SetViewport_t)o;
    if (void* o = Patch(dev, 94, (void*)Hook_SetVSConstF)) Real_SetVSConstF = (SetVSConstF_t)o;
    PutHalf(dev, w, h);
    ResetStages(dev);
}

void GraphicsBeforeReset() { ReleaseTargets(); }

void GraphicsAfterReset(IDirect3DDevice9* dev, UINT w, UINT h) {
    g_bbW = w;
    g_bbH = h;
    if (Real_SetSamplerState) ResetStages(dev);
}

// d3d9.cpp, every Present (before the frame is shown): our passes
void GraphicsPresent(IDirect3DDevice9* dev) {
    static int frames = 0;
    if (++frames == 600) Log(L"Graphics: %d of %d vertex shaders have the half-pixel fix", g_vsPatched, g_vsSeen);
    if (g_statesDirty && Real_SetSamplerState) {
        g_statesDirty = false;
        for (int s = 0; s < kStages; ++s) {
            Real_SetSamplerState(dev, s, D3DSAMP_MAXANISOTROPY, g_filter >= 2 ? min((DWORD)g_filter, g_maxAniso) : 1);
            ApplyStage(dev, s);
        }
    }
    bool aa = g_aa != 0, final = g_sharpen > 0 || g_bright != 50;
    if ((!aa && !final) || !Real_SetTexture || !Ready(dev)) return;
    IDirect3DSurface9 *back = nullptr, *rt = nullptr, *depth = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return;
    g_state->Capture();
    dev->GetRenderTarget(0, &rt);
    dev->GetDepthStencilSurface(&depth);
    dev->SetRenderTarget(0, back);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, g_bbW, g_bbH, 0, 1};
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    Real_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    Real_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    Real_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    if (aa) Pass(dev, back, g_fxaa, nullptr);
    if (final) {
        float opt[4] = {g_sharpen / 100.0f, powf(1.6f, (50 - g_bright) / 50.0f), 0, 0};
        Pass(dev, back, g_final, opt);
    }
    g_state->Apply();
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(depth);
    for (int s = 0; s < kStages; ++s) ApplyStage(dev, s);   // (what we put for the game's wishes, again)
    if (rt) rt->Release();
    if (depth) depth->Release();
    back->Release();
}

// ---- the SETTINGS page (the main menu: src/ui/lobby.as2js FGUYDrawGfx) -------------------------------------------
// Three pages of rows: FGUYMOD_GfxRows(page) -> "label~value~kind~hint\n..." (kind: c a choice, s a 0-100 slider,
// l a link - A opens the game's own page for it), FGUYMOD_GfxChange(page, row, delta: -1 / +1 / 0 for A) -> what the
// menu does then: 0 nothing, 1 the game's Screen page, 2 the game's Options. Changes show at once and are saved.
namespace {
std::string g_rows;
void Save(const wchar_t* section, const wchar_t* key, int v) {
    wchar_t path[MAX_PATH], val[16];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    swprintf_s(val, L"%d", v);
    WritePrivateProfileStringW(section, key, val, path);
}
const int kFilters[] = {0, 1, 2, 4, 8, 16};
const char* kFilterNames[] = {"THE GAME'S (BILINEAR)", "TRILINEAR", "ANISOTROPIC 2X", "ANISOTROPIC 4X", "ANISOTROPIC 8X",
                              "ANISOTROPIC 16X"};
const char* kRateNames[] = {"VSYNC", "30 FPS", "60 FPS", "120 FPS", "144 FPS", "240 FPS", "UNCAPPED"};
int FilterIndex() {
    for (int i = 0; i < 6; ++i) if (kFilters[i] == g_filter) return i;
    return 5;
}
int Step(int v, int delta, int by) {
    v += (delta < 0 ? -by : by);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}
}  // namespace

bool GraphicsMenuNew() { return SettingInt(L"Interface", L"MenuStyle", nullptr, 1) != 0; }

const char* GraphicsMenuRows(int page) {
    g_rows.clear();
    char v[96];
    auto row = [&](const char* label, const std::string& value, char kind, const char* hint) {
        g_rows += std::string(label) + "~" + value + "~" + kind + "~" + hint + "\n";
    };
    if (page == 0) {
        int w = 0, h = 0, mode = DisplayMenuIndex();
        DisplayRenderSize(&w, &h);
        sprintf_s(v, "%s  %d x %d", mode == 0 ? "WINDOWED" : mode == 2 ? "BORDERLESS" : "FULLSCREEN", w, h);
        row("Screen mode and resolution", v, 'l', "Opens the game's screen options (they ask before they keep a new size)");
        int r = VideoFrameRateIndex();
        row("Frame rate", kRateNames[r >= 0 && r < 7 ? r : 0], 'c', "VSync: no tearing. A limit or uncapped: smoother on fast screens");
        sprintf_s(v, "%d", (int)g_bright);
        row("Brightness", v, 's', "50 is the game's own");
    } else if (page == 1) {
        row("Sharp picture", g_sharp ? "ON" : "OFF", 'c', "Fixes the game's blur: it draws half a pixel off. Off: the game as it was");
        row("Anti-aliasing", g_aa ? "FXAA" : "OFF", 'c', "Smooths jagged edges - most at 1080p and below (at 4K they're tiny). Off is sharpest");
        row("Texture filtering", kFilterNames[FilterIndex()], 'c', "Sharper ground far off, seen at a low angle (a small change in this game)");
        sprintf_s(v, "%d", (int)g_sharpen);
        row("Sharpening", v, 's', "Crisper lines and textures. 0: none");
    } else {
        row("Menu style", GraphicsMenuNew() ? "NEW" : "CLASSIC", 'c', "Classic: Options opens the game's own options (these settings are in it too)");
        row("More options", "AUDIO, CONTROLS, LANGUAGE", 'l', "The game's own options");
    }
    return g_rows.c_str();
}

int GraphicsMenuChange(int page, int row, int delta) {
    int dir = delta < 0 ? -1 : 1;
    if (page == 0) {
        if (row == 0) return 1;
        if (row == 1) VideoSetFrameRate((VideoFrameRateIndex() + dir + 7) % 7);
        if (row == 2 && delta) { g_bright = Step(g_bright, delta, 5); Save(L"Graphics", L"Brightness", g_bright); }
    } else if (page == 1) {
        if (row == 0) { g_sharp = !g_sharp; Save(L"Graphics", L"SharpPicture", g_sharp); g_statesDirty = true; }
        if (row == 1) { g_aa = !g_aa; Save(L"Graphics", L"AntiAliasing", g_aa); }
        if (row == 2) {
            g_filter = kFilters[(FilterIndex() + dir + 6) % 6];
            Save(L"Graphics", L"TextureFiltering", g_filter);
            g_statesDirty = true;
        }
        if (row == 3 && delta) { g_sharpen = Step(g_sharpen, delta, 10); Save(L"Graphics", L"Sharpening", g_sharpen); }
    } else {
        if (row == 0) Save(L"Interface", L"MenuStyle", GraphicsMenuNew() ? 0 : 1);
        if (row == 1) return 2;
    }
    return 0;
}
