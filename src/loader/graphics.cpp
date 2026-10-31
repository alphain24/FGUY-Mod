// Graphics settings: a sharper picture, texture filtering, anti-aliasing, sharpening and brightness.
//
// * Sharp picture: the game draws everything half a pixel off (Direct3D 9's pixel centres, which the console port never
//   made up for), so each of its full-screen passes (glow, ambient occlusion, depth of field, the final copy) blends
//   four texels where it meant one - the game's well-known blur (it has no anti-aliasing on PC: its MSAAType setting is
//   never used - nor on an HD Xbox 360). Every vertex shader gets the half pixel back (the half-pixel fix, below), and
//   the characters' ink / shading ramp loses the mipmaps the port gave it (the ramp fix: the grey rings and grain round
//   eyes and creases): the picture as the consoles draw it.
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

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

volatile int g_sharp = 1, g_filter = 16, g_aa = 0, g_sharpen = 0, g_bright = 50;
volatile int g_lines = 0;   // (the ink lines' width: 0 auto - as on the console at any resolution - or 100-300 %)
volatile bool g_hdChars = true;   // (the characters' textures at twice their size - cosmetics.cpp)
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
bool g_ramp[kStages];          // (the ink / shading pass's ramp is set there: see the ramp fix)

// ---- the ramp fix (Sharp picture) ------------------------------------------------------------------------------
// The characters' ink lines and soft shading are one more pass over them, its strength read from a 16x16 L8 ramp the
// game makes at start (255 - 17 * max(x, y)). The Xbox 360 build makes it with one level (0x821b5830:
// CreateTexture(16, 16, 1, 0, L8, ...)), the PC port with five - and the pass reads the small ones (down to a 1x1 of
// grey 83) wherever its coordinates change fast: grey rings and grain round every eye, lens and crease. With Sharp
// picture the ramp's mip filter is none: only its top level, as on the console.
std::vector<IDirect3DBaseTexture9*> g_ramps;

bool IsRamp(IDirect3DBaseTexture9* tex) {
    if (!tex || std::find(g_ramps.begin(), g_ramps.end(), tex) == g_ramps.end()) return false;
    D3DSURFACE_DESC d;   // (still that texture, not a new one at the same address)
    return tex->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d)) &&
           d.Width == 16 && d.Height == 16 && d.Format == D3DFMT_L8;
}

int Slot(D3DSAMPLERSTATETYPE t) {
    return t == D3DSAMP_MAGFILTER ? 0 : t == D3DSAMP_MINFILTER ? 1 : t == D3DSAMP_MIPFILTER ? 2 : -1;
}
const D3DSAMPLERSTATETYPE kType[3] = {D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER};

// what a filter the game asks for becomes on a stage
DWORD Filter(int stage, int k, DWORD want) {
    if (g_ramp[stage] && k == 2 && g_sharp) return D3DTEXF_NONE;
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
        g_ramp[s] = false;
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
        bool screen = IsScreenTarget(tex), ramp = IsRamp(tex);
        if (screen != g_screen[stage] || ramp != g_ramp[stage]) {
            g_screen[stage] = screen;
            g_ramp[stage] = ramp;
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
bool g_c254Used = false;            // (some shader of the game's has c254: the strokes' alpha left alone)
float g_inkAlphaConst[4] = {1, 0, 0, 0};

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
                if (RegType(t) == 2 && RegNum(t) == kHalf - 1 && !g_c254Used) {   // (c254: the strokes' alpha - see AddInkAlpha)
                    g_c254Used = true;
                    Log(L"Graphics: a vertex shader uses c254 - the ink strokes keep the game's alpha");
                }
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

// The ink strokes' vertex shader (projMat and the fog's constants): the game fades the strokes as the picture gets
// taller (their alpha about 720 / its height past 1080: 30% at 2400 lines); on the Xbox's 720p they're solid. With Line
// thickness other than the PC game's, its colour's alpha is made saturate(alpha * c254.x), c254.x = height / 720 -
// the console's solid outline (then the ink pixel shader widens it: see PatchInkShader).
std::string CtabNames(const DWORD* code);
const char kInkVsNames[] = "FogLerpParams,FogMode,HeightFogLerpParams,HeightFogMode,projMat";

bool AddInkAlpha(std::vector<DWORD>& code) {
    int colourIn = -1, colourOut = -1;   // (input texcoord2 - the D3DCOLOR - and output texcoord1, what the pixel's v1 is)
    for (size_t i = 1; i + 2 < code.size() && code[i] != 0x0000FFFF;) {
        DWORD tok = code[i], op = tok & 0xFFFF;
        if (op == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        DWORD len = (tok >> 24) & 0xF;
        if (op == 31 && (code[i + 1] & 0x1F) == 5) {
            DWORD index = (code[i + 1] >> 16) & 0xF;
            if (RegType(code[i + 2]) == 1 && index == 2) colourIn = (int)RegNum(code[i + 2]);
            if (RegType(code[i + 2]) == 6 && index == 1) colourOut = (int)RegNum(code[i + 2]);
        }
        i += 1 + len;
    }
    if (colourIn < 0 || colourOut < 0 || code.back() != 0x0000FFFF) return false;
    code.insert(code.end() - 1, {5u | (3u << 24), RegTok(6, colourOut) | (0x8u << 16) | (1u << 20),
                                 RegTok(1, colourIn) | (0xFFu << 16), RegTok(2, kHalf - 1) | (0x00u << 16)});   // mul_sat oN.w, vM.w, c254.x
    return true;
}

void PutHalf(IDirect3DDevice9* dev, DWORD w, DWORD h) {
    bool on = g_sharp && w && h;
    g_halfConst[0] = on ? -1.0f / w : 0;
    g_halfConst[1] = on ? 1.0f / h : 0;
    Real_SetVSConstF(dev, kHalf, g_halfConst, 1);
    g_inkAlphaConst[0] = g_lines != 100 && g_bbH > 720 ? g_bbH / 720.0f : 1.0f;
    if (!g_c254Used) Real_SetVSConstF(dev, kHalf - 1, g_inkAlphaConst, 1);
}

// (the constants are put again each time the strokes' vertex shader is set: a device reset - even one that fails, as
// a resolution change in borderless did - clears them all, and c254 = 0 drew every outline see-through)
std::vector<IDirect3DVertexShader9*> g_inkVs;
using SetVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
SetVertexShader_t Real_SetVertexShader;

HRESULT STDMETHODCALLTYPE Hook_SetVertexShader(IDirect3DDevice9* self, IDirect3DVertexShader9* vs) {
    HRESULT hr = Real_SetVertexShader(self, vs);
    if (vs && !g_c254Used && std::find(g_inkVs.begin(), g_inkVs.end(), vs) != g_inkVs.end()) {
        g_inkAlphaConst[0] = g_lines != 100 && g_bbH > 720 ? g_bbH / 720.0f : 1.0f;
        Real_SetVSConstF(self, kHalf - 1, g_inkAlphaConst, 1);
        Real_SetVSConstF(self, kHalf, g_halfConst, 1);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateVertexShader(IDirect3DDevice9* self, const DWORD* code, IDirect3DVertexShader9** out) {
    std::vector<DWORD> patched;
    const char* why = "";
    ++g_vsSeen;
    if (code && PatchVertexShader(code, patched, &why)) {
        bool ink = CtabNames(code) == kInkVsNames;
        if (ink) {
            if (AddInkAlpha(patched)) Log(L"Graphics: the ink strokes' vertex shader patched (their alpha)");
            else { Log(L"Graphics: the ink strokes' vertex shader isn't as expected - their alpha left as it is"); ink = false; }
        }
        HRESULT hr = Real_CreateVertexShader(self, patched.data(), out);
        if (SUCCEEDED(hr) && ink && out && *out) g_inkVs.push_back(*out);
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
    if (start <= kHalf - 1 && start + count > kHalf - 1 && !g_c254Used) Real_SetVSConstF(self, kHalf - 1, g_inkAlphaConst, 1);
    return hr;
}

// ---- research: the game's pixel shader constants by name ---------------------------------------------------------
// FGUYMOD_TEST_PSCONST=name[=v0,v1,v2,v3];name2... - each named constant (from the shaders' constant tables) is logged
// as the game sets it, and one given values is held at them (e.g. g_ssaoBlendFactor=0: the effects without it).
struct PsConst { char name[48]; bool hold; float v[4]; int told; };
struct PsWatch { IDirect3DPixelShader9* ps; int which; UINT reg; };
std::vector<PsConst> g_psConst;
std::vector<PsWatch> g_psWatch;
IDirect3DPixelShader9* g_curPs = nullptr;
using CreatePixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
CreatePixelShader_t Real_CreatePixelShader;
SetPixelShader_t Real_SetPixelShader;
SetPSConstF_t Real_SetPSConstF;

// a float constant's register in a shader's constant table (CTAB comment) - false: not there
bool CtabRegister(const DWORD* code, const char* name, UINT* reg) {
    for (size_t i = 1; code[i] != 0x0000FFFF && i < 200000;) {
        DWORD tok = code[i];
        if ((tok & 0xFFFF) != 0xFFFE) return false;   // (the table comes first, before any instruction)
        DWORD len = (tok >> 16) & 0x7FFF;
        if (len >= 8 && code[i + 1] == 0x42415443) {   // 'CTAB'
            const char* base = reinterpret_cast<const char*>(&code[i + 2]);
            DWORD count = *reinterpret_cast<const DWORD*>(base + 12), info = *reinterpret_cast<const DWORD*>(base + 16);
            for (DWORD c = 0; c < count; ++c) {
                const char* ci = base + info + c * 20;
                const char* n = base + *reinterpret_cast<const DWORD*>(ci);
                WORD set = *reinterpret_cast<const WORD*>(ci + 4), index = *reinterpret_cast<const WORD*>(ci + 6);
                if (set == 2 && strcmp(n, name) == 0) { *reg = index; return true; }   // (D3DXRS_FLOAT4)
            }
            return false;
        }
        i += 1 + len;
    }
    return false;
}

// FGUYMOD_TEST_PSTRACE=1: every 1000th frame, the pixel shaders it uses in order (by their constants' names), and
// the constant values of the watched ones as they're bound.
bool g_psTrace = false;
int g_frame = 0;
char g_psSkip[512] = "";                        // (FGUYMOD_TEST_PSSKIP, below)
std::vector<IDirect3DPixelShader9*> g_skipPs;
std::vector<std::pair<IDirect3DPixelShader9*, std::string>> g_psNames;

std::string CtabNames(const DWORD* code) {
    std::string s;
    for (size_t i = 1; code[i] != 0x0000FFFF && i < 200000;) {
        DWORD tok = code[i];
        if ((tok & 0xFFFF) != 0xFFFE) break;
        DWORD len = (tok >> 16) & 0x7FFF;
        if (len >= 8 && code[i + 1] == 0x42415443) {
            const char* base = reinterpret_cast<const char*>(&code[i + 2]);
            DWORD count = *reinterpret_cast<const DWORD*>(base + 12), info = *reinterpret_cast<const DWORD*>(base + 16);
            for (DWORD c = 0; c < count && s.size() < 300; ++c) {
                const char* ci = base + info + c * 20;
                if (!s.empty()) s += ",";
                s += base + *reinterpret_cast<const DWORD*>(ci);
            }
            break;
        }
        i += 1 + len;
    }
    return s.empty() ? "(no table)" : s;
}

void PutHeld(IDirect3DDevice9* dev) {
    for (const PsWatch& w : g_psWatch)
        if (w.ps == g_curPs && g_psConst[w.which].hold) Real_SetPSConstF(dev, w.reg, g_psConst[w.which].v, 1);
}

// The ink strokes - the characters' outlines, made by the game each frame as quads (the lines inside them, the
// smile, the creases, are in the models): their pixel shader (constants FogColor, diffuseMap, lightMap) draws
// ramp(uv0) * ramp(uv1) * the vertex's alpha, in the vertex's colour. Each uv pair's two values add up to the same
// number at every corner of a quad (uv0 along the stroke, uv1 across, adding up to 0), so ramp(max(u, v)) is a band:
// solid where both are <= 0, a one-unit soft edge either side. Patched to read the ramp at uv + c[kInk] (the two
// pairs' widening, negative): the solid middle grows, the soft edge stays as it is - a wider line, no softer. The quad
// holds a line up to about 3x the game's (uv1 runs to about +-2.4 at 4K, +-5 at 1080p).
const DWORD kInk = 1;
float g_inkConst[4] = {0, 0, 0, 0};
float g_inkTest[2] = {0, 0};   // (research, FGUYMOD_TEST_INK=a,b: what's taken off uv0 / uv1)
bool g_inkRed = false;
const float kInkRed[4] = {1, 0, 0, 0};
std::vector<IDirect3DPixelShader9*> g_inkPs;

bool PatchInkShader(const DWORD* in, std::vector<DWORD>& out) {
    if ((in[0] & 0xFFFF0000) != 0xFFFF0000 || ((in[0] >> 8) & 0xFF) != 3) return false;   // (ps_3_0 only)
    int maxTemp = -1;
    size_t i = 1, lastDcl = 0;
    for (; in[i] != 0x0000FFFF && i < 4096;) {
        DWORD tok = in[i], op = tok & 0xFFFF;
        if (op == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        DWORD len = (tok >> 24) & 0xF;
        if (op == 31) lastDcl = i + 1 + len;
        else if (op != 81 && op != 48 && op != 47)
            for (DWORD k = 1; k <= len; ++k) {
                DWORD t = in[i + k];
                if ((t & 0x80000000) && RegType(t) == 0 && (int)RegNum(t) > maxTemp) maxTemp = (int)RegNum(t);
                if ((t & 0x80000000) && RegType(t) == 2 && RegNum(t) >= kInk && RegNum(t) <= kInk + 3) return false;   // (in use)
            }
        i += 1 + len;
    }
    if (in[i] != 0x0000FFFF || !lastDcl) return false;
    DWORD temp = (DWORD)(maxTemp + 1), v0 = RegTok(1, 0), t = RegTok(0, temp);
    out.assign(in, in + lastDcl);
    out.insert(out.end(), {2u | (3u << 24), t | (0xFu << 16), v0 | (0xE4u << 16), RegTok(2, kInk) | (0xE4u << 16)});   // add rT, v0, c1
    for (size_t j = lastDcl; j < i;) {
        DWORD tok = in[j], op = tok & 0xFFFF;
        size_t len = op == 0xFFFE ? ((tok >> 16) & 0x7FFF) : ((tok >> 24) & 0xF);
        size_t at = out.size();
        out.insert(out.end(), in + j, in + j + 1 + len);
        if (op == 66 && len >= 3 && RegType(out[at + 2]) == 1 && RegNum(out[at + 2]) == 0)   // texld rX, v0(.swizzle), sN
            out[at + 2] = t | (out[at + 2] & 0x00FF0000);
        j += 1 + len;
    }
    if (g_inkRed)   // (research, FGUYMOD_TEST_INKRED: the strokes drawn red - which pixels are theirs)
        out.insert(out.end(), {1u | (2u << 24), RegTok(8, 0) | (0x7u << 16), RegTok(2, kInk + 3) | (0xE4u << 16)});   // mov oC0.xyz, c4
    out.push_back(0x0000FFFF);
    return true;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePixelShader(IDirect3DDevice9* self, const DWORD* code, IDirect3DPixelShader9** out) {
    if (code && CtabNames(code) == "FogColor,diffuseMap,lightMap") {
        std::vector<DWORD> patched;
        if (PatchInkShader(code, patched) && SUCCEEDED(Real_CreatePixelShader(self, patched.data(), out)) && out && *out) {
            g_inkPs.push_back(*out);
            if (g_psTrace) g_psNames.push_back({*out, CtabNames(code)});
            Log(L"Graphics: the ink strokes' pixel shader patched (their width)");
            return D3D_OK;
        }
        Log(L"Graphics: the ink strokes' pixel shader isn't as expected - left as it is");
    }
    HRESULT hr = Real_CreatePixelShader(self, code, out);
    if (SUCCEEDED(hr) && code && out && *out && g_psTrace) g_psNames.push_back({*out, CtabNames(code)});
    UINT skipReg;   // (a name, or "=" and a shader's whole list of constant names as the trace prints it)
    if (SUCCEEDED(hr) && code && out && *out && g_psSkip[0] &&
        (g_psSkip[0] == '=' ? CtabNames(code) == g_psSkip + 1 : CtabRegister(code, g_psSkip, &skipReg)))
        g_skipPs.push_back(*out);
    if (SUCCEEDED(hr) && code && out && *out)
        for (int k = 0; k < (int)g_psConst.size(); ++k) {
            UINT reg;
            if (!CtabRegister(code, g_psConst[k].name, &reg)) continue;
            g_psWatch.push_back({*out, k, reg});
            Log(L"Graphics test: pixel shader %p has %S at c%u", *out, g_psConst[k].name, reg);
        }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SetPixelShader(IDirect3DDevice9* self, IDirect3DPixelShader9* ps) {
    HRESULT hr = Real_SetPixelShader(self, ps);
    if (ps && std::find(g_inkPs.begin(), g_inkPs.end(), ps) != g_inkPs.end()) {
        // The strokes are a few pixels wide at any resolution (and fainter the taller it is: see AddInkAlpha); the
        // Xbox draws them at 720p and scales the picture up. A line's solid middle is 2c + 1 units of uv1 for c taken
        // off it: m times as wide for c = (m - 1) / 2. Auto, with their alpha made solid again: m = the picture's
        // height / 1600 (1 below that) - beside the Xbox at 2400 lines, 1.5 matched it.
        float m = g_lines ? g_lines / 100.0f : (g_bbH > 1600 ? g_bbH / 1600.0f : 1.0f);
        g_inkConst[0] = g_inkConst[1] = -g_inkTest[0];
        g_inkConst[2] = g_inkConst[3] = g_inkTest[1] ? -g_inkTest[1] : -(m - 1) * 0.5f;
        Real_SetPSConstF(self, kInk, g_inkConst, 1);
        if (g_inkRed) Real_SetPSConstF(self, kInk + 3, kInkRed, 1);
    }
    if (g_psTrace && g_frame % 1000 == 999 && ps && ps != g_curPs) {
        IDirect3DSurface9* rt = nullptr;
        D3DSURFACE_DESC d = {};
        if (SUCCEEDED(self->GetRenderTarget(0, &rt)) && rt) { rt->GetDesc(&d); rt->Release(); }
        const char* name = "?";
        for (auto& p : g_psNames) if (p.first == ps) name = p.second.c_str();
        Log(L"Graphics trace %d: -> %ux%u fmt %d  ps %p %S", g_frame, d.Width, d.Height, d.Format, ps, name);
    }
    g_curPs = ps;
    PutHeld(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SetPSConstF(IDirect3DDevice9* self, UINT start, const float* data, UINT count) {
    HRESULT hr = Real_SetPSConstF(self, start, data, count);
    if (start <= kInk && start + count > kInk && g_curPs && std::find(g_inkPs.begin(), g_inkPs.end(), g_curPs) != g_inkPs.end())
        Real_SetPSConstF(self, kInk, g_inkConst, 1);   // (the game's own over ours: ours again)
    for (const PsWatch& w : g_psWatch) {
        if (w.ps != g_curPs || w.reg < start || w.reg >= start + count) continue;
        PsConst& c = g_psConst[w.which];
        const float* v = data + (w.reg - start) * 4;
        if (c.told++ < 6) Log(L"Graphics test: the game sets %S = %g %g %g %g", c.name, v[0], v[1], v[2], v[3]);
        if (c.hold) Real_SetPSConstF(self, w.reg, c.v, 1);
    }
    return hr;
}

// FGUYMOD_TEST_PSSKIP=name: draws with a pixel shader that has a constant of that name are left out (e.g.
// projectShadowTexture: the scene without its projected shadows). The watched constants of a shader are logged at its
// first draw of a traced frame.
using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                                             D3DFORMAT, const void*, UINT);
DrawPrimitive_t Real_DrawPrimitive;
DrawIndexedPrimitive_t Real_DrawIndexedPrimitive;
DrawPrimitiveUP_t Real_DrawPrimitiveUP;
DrawIndexedPrimitiveUP_t Real_DrawIndexedPrimitiveUP;
IDirect3DPixelShader9* g_loggedPs = nullptr;
const char* g_drawKind = "";
UINT g_drawCount = 0;

// FGUYMOD_TEST_DUMPSHADER=<constant name>: at a traced frame, the first draw with a pixel shader that has it writes that
// shader and the vertex shader with it, disassembled (d3dcompiler_47's D3DDisassemble), to FGUYMod\shaders_<name>.txt
char g_dumpShader[64] = "";
bool g_shaderDumped = false;
void DumpShaders(IDirect3DDevice9* dev) {
    using D3DDisassemble_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto disasm = m ? reinterpret_cast<D3DDisassemble_t>(GetProcAddress(m, "D3DDisassemble")) : nullptr;
    if (!disasm) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\shaders_%S.txt", g_modDir, g_dumpShader);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") || !f) return;
    IDirect3DVertexShader9* vs = nullptr;
    dev->GetVertexShader(&vs);
    for (int k = 0; k < 2; ++k) {
        UINT size = 0;
        std::vector<unsigned char> code;
        if (k == 0 && g_curPs && SUCCEEDED(g_curPs->GetFunction(nullptr, &size))) { code.resize(size); g_curPs->GetFunction(code.data(), &size); }
        if (k == 1 && vs && SUCCEEDED(vs->GetFunction(nullptr, &size))) { code.resize(size); vs->GetFunction(code.data(), &size); }
        ID3DBlob* text = nullptr;
        fprintf(f, "==== %s shader (%u bytes)\n", k == 0 ? "pixel" : "vertex (as patched: the half-pixel fix at its end)", size);
        if (!code.empty() && SUCCEEDED(disasm(code.data(), code.size(), 0, nullptr, &text)) && text) {
            fwrite(text->GetBufferPointer(), 1, text->GetBufferSize() - 1, f);
            text->Release();
        }
        fprintf(f, "\n");
    }
    if (vs) vs->Release();
    // the draw's vertices: its declaration, then the first 24 (stream 0, as floats) and first 36 indices
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl) {
        D3DVERTEXELEMENT9 el[32];
        UINT n = 32;
        if (SUCCEEDED(decl->GetDeclaration(el, &n)))
            for (UINT e = 0; e < n && el[e].Stream != 0xFF; ++e)
                fprintf(f, "element: stream %u offset %u type %u usage %u index %u\n", el[e].Stream, el[e].Offset, el[e].Type,
                        el[e].Usage, el[e].UsageIndex);
        decl->Release();
    }
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    if (SUCCEEDED(dev->GetStreamSource(0, &vb, &offset, &stride)) && vb) {
        D3DVERTEXBUFFER_DESC vd;
        vb->GetDesc(&vd);
        void* p = nullptr;
        fprintf(f, "stream 0: stride %u offset %u, buffer %u bytes, usage %#lx pool %d\n", stride, offset, vd.Size, vd.Usage, vd.Pool);
        if (SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_READONLY)) && p) {
            for (UINT v = 0; v < 24 && offset + (v + 1) * stride <= vd.Size; ++v) {
                const float* fl = reinterpret_cast<const float*>(static_cast<const char*>(p) + offset + v * stride);
                fprintf(f, "v%02u:", v);
                for (UINT k = 0; k < stride / 4; ++k) fprintf(f, " %9.4f", fl[k]);
                fprintf(f, "\n");
            }
            if (stride == 32)   // (the strokes: per quad its colour, and each uv pair's sum - its band's solid width)
                for (UINT q = 0; q < 150 && offset + (q * 4 + 4) * stride <= vd.Size; ++q) {
                    const char* base = static_cast<const char*>(p) + offset + q * 4 * stride;
                    const float* a = reinterpret_cast<const float*>(base);
                    DWORD col = *reinterpret_cast<const DWORD*>(base + 12);
                    float minU0 = 1e9f, maxU0 = -1e9f, minU1 = 1e9f, maxU1 = -1e9f;
                    for (int c = 0; c < 4; ++c) {
                        const float* t = reinterpret_cast<const float*>(base + c * stride);
                        minU0 = min(minU0, t[4]); maxU0 = max(maxU0, t[4]); minU1 = min(minU1, t[6]); maxU1 = max(maxU1, t[6]);
                    }
                    fprintf(f, "quad %3u: colour %08lx  uv0 sum %7.3f range %6.2f..%6.2f  uv1 sum %7.3f range %6.2f..%6.2f  z %.3f\n", q,
                            col, a[4] + a[5], minU0, maxU0, a[6] + a[7], minU1, maxU1, a[2]);
                }
            vb->Unlock();
        }
        vb->Release();
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(dev->GetIndices(&ib)) && ib) {
        D3DINDEXBUFFER_DESC id;
        ib->GetDesc(&id);
        void* p = nullptr;
        if (id.Format == D3DFMT_INDEX16 && SUCCEEDED(ib->Lock(0, 0, &p, D3DLOCK_READONLY)) && p) {
            fprintf(f, "indices:");
            for (UINT k = 0; k < 36 && k * 2 < id.Size; ++k) fprintf(f, " %u", static_cast<const unsigned short*>(p)[k]);
            fprintf(f, "\n");
            ib->Unlock();
        }
        ib->Release();
    }
    fclose(f);
    Log(L"Graphics test: shaders written to %s", path);
}

bool SkipDraw(IDirect3DDevice9* dev) {
    if (g_psTrace && g_frame % 1000 == 999 && g_curPs != g_loggedPs) {
        g_loggedPs = g_curPs;
        if (g_dumpShader[0] && !g_shaderDumped)
            for (auto& p : g_psNames)
                if (p.first == g_curPs && p.second.find(g_dumpShader) != std::string::npos) { g_shaderDumped = true; DumpShaders(dev); }
        DWORD s[11];
        const D3DRENDERSTATETYPE kStates[11] = {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE,
                                                D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE,
                                                D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS};
        for (int k = 0; k < 11; ++k) dev->GetRenderState(kStates[k], &s[k]);
        D3DSURFACE_DESC t0 = {};
        IDirect3DBaseTexture9* tex = nullptr;
        if (SUCCEEDED(dev->GetTexture(0, &tex)) && tex) {
            if (tex->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &t0);
            tex->Release();
        }
        Log(L"Graphics trace %d:    blend %lu (%lu,%lu) alphatest %lu ref %lu func %lu  z %lu write %lu cull %lu bias %g slope %g  "
            L"tex0 %ux%u fmt %d", g_frame, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], *(float*)&s[9], *(float*)&s[10],
            t0.Width, t0.Height, t0.Format);
        void* frames[24];
        USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        wchar_t game[256] = L"";
        HMODULE exe = GetModuleHandleW(nullptr);
        for (USHORT k = 0, found = 0; k < n && found < 6; ++k) {   // (the game's own code: FGUY.exe's addresses)
            uintptr_t a = (uintptr_t)frames[k], base = (uintptr_t)exe;
            if (a < base || a >= base + 0x800000) continue;
            swprintf_s(game + wcslen(game), 256 - wcslen(game), L" %p", frames[k]);
            ++found;
        }
        Log(L"Graphics trace %d:    drawn by%s (%S, %u primitives)", g_frame, game, g_drawKind, g_drawCount);
        for (DWORD st = 0; st < 4; ++st) {
            IDirect3DBaseTexture9* t = nullptr;
            if (FAILED(dev->GetTexture(st, &t)) || !t) continue;
            D3DSURFACE_DESC td = {};
            if (t->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &td);
            DWORD levels = t->GetLevelCount(), lod = t->GetLOD(), f[3], bias, maxMip;
            t->Release();
            for (int k = 0; k < 3; ++k) dev->GetSamplerState(st, kType[k], &f[k]);
            dev->GetSamplerState(st, D3DSAMP_MIPMAPLODBIAS, &bias);
            dev->GetSamplerState(st, D3DSAMP_MAXMIPLEVEL, &maxMip);
            Log(L"Graphics trace %d:      stage %lu: %ux%u fmt %08x, %lu levels (lod %lu), filter %lu/%lu/%lu, lod bias %g, max mip %lu",
                g_frame, st, td.Width, td.Height, td.Format, levels, lod, f[0], f[1], f[2], *(float*)&bias, maxMip);
        }
        for (const PsWatch& w : g_psWatch) {
            if (w.ps != g_curPs) continue;
            float v[4] = {};
            dev->GetPixelShaderConstantF(w.reg, v, 1);
            Log(L"Graphics trace %d:    %S = %g %g %g %g", g_frame, g_psConst[w.which].name, v[0], v[1], v[2], v[3]);
        }
    }
    for (IDirect3DPixelShader9* p : g_skipPs) if (p == g_curPs) return true;
    return false;
}

// the ramp fix's other half: the game's 16x16 L8 ramp, made with more than one level (traced: every small L8)
using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                    IDirect3DTexture9**, HANDLE*);
CreateTexture_t Real_CreateTexture;
HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* self, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                             D3DPOOL pool, IDirect3DTexture9** out, HANDLE* share) {
    HRESULT hr = Real_CreateTexture(self, w, h, levels, usage, fmt, pool, out, share);
    if (SUCCEEDED(hr) && out && *out && fmt == D3DFMT_L8 && w == 16 && h == 16 && (*out)->GetLevelCount() > 1 &&
        std::find(g_ramps.begin(), g_ramps.end(), *out) == g_ramps.end()) {
        g_ramps.push_back(*out);
        static int told = 0;
        if (told++ < 3) Log(L"Graphics: the ink / shading ramp (16x16 L8, %lu levels) - its top level only with Sharp picture",
                            (*out)->GetLevelCount());
    }
    if (g_psTrace && fmt == D3DFMT_L8 && w <= 64 && h <= 64)
        Log(L"Graphics trace: CreateTexture %ux%u L8, %u levels asked, usage %#lx, pool %d", w, h, levels, usage, pool);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_DrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT a, UINT b) {
    g_drawKind = "DrawPrimitive"; g_drawCount = b;
    return SkipDraw(self) ? D3D_OK : Real_DrawPrimitive(self, t, a, b);
}
HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT d, UINT e) {
    g_drawKind = "DrawIndexedPrimitive"; g_drawCount = e;
    return SkipDraw(self) ? D3D_OK : Real_DrawIndexedPrimitive(self, t, a, b, c, d, e);
}
HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT a, const void* b, UINT c) {
    g_drawKind = "DrawPrimitiveUP"; g_drawCount = a;
    return SkipDraw(self) ? D3D_OK : Real_DrawPrimitiveUP(self, t, a, b, c);
}
HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c,
                                                      const void* d, D3DFORMAT e, const void* f, UINT g) {
    g_drawKind = "DrawIndexedPrimitiveUP"; g_drawCount = c;
    return SkipDraw(self) ? D3D_OK : Real_DrawIndexedPrimitiveUP(self, t, a, b, c, d, e, f, g);
}

void LoadPsConstTest() {
    char spec[512];
    g_psTrace = GetEnvironmentVariableA("FGUYMOD_TEST_PSTRACE", spec, sizeof(spec)) != 0;
    GetEnvironmentVariableA("FGUYMOD_TEST_DUMPSHADER", g_dumpShader, sizeof(g_dumpShader));
    if (GetEnvironmentVariableA("FGUYMOD_TEST_INK", spec, sizeof(spec))) sscanf_s(spec, "%f,%f", &g_inkTest[0], &g_inkTest[1]);
    g_inkRed = GetEnvironmentVariableA("FGUYMOD_TEST_INKRED", spec, sizeof(spec)) != 0;
    if (GetEnvironmentVariableA("FGUYMOD_TEST_PSSKIP", g_psSkip, sizeof(g_psSkip)))
        Log(L"Graphics test: draws with pixel shaders that have %S are left out", g_psSkip);
    if (!GetEnvironmentVariableA("FGUYMOD_TEST_PSCONST", spec, sizeof(spec))) return;
    char* ctx = nullptr;
    for (char* item = strtok_s(spec, ";", &ctx); item; item = strtok_s(nullptr, ";", &ctx)) {
        PsConst c = {};
        char* eq = strchr(item, '=');
        if (eq) {
            *eq = 0;
            c.hold = sscanf_s(eq + 1, "%f,%f,%f,%f", &c.v[0], &c.v[1], &c.v[2], &c.v[3]) >= 1;
        }
        strncpy_s(c.name, item, _TRUNCATE);
        g_psConst.push_back(c);
        Log(L"Graphics test: watching pixel shader constant %S%s", c.name, c.hold ? L" (held)" : L"");
    }
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
    int l = SettingInt(L"Graphics", L"LineThickness", L"FGUYMOD_GFX_LINES", 0);
    g_lines = l <= 0 ? 0 : l < 100 ? 100 : l > 300 ? 300 : l;
    g_hdChars = SettingInt(L"Graphics", L"HDCharacters", L"FGUYMOD_GFX_HD", 1) != 0;
    Log(L"Graphics: sharp picture %s, texture filtering %d, anti-aliasing %s, sharpening %d, brightness %d, line thickness %d%s, "
        L"HD character textures %s", g_sharp ? L"on" : L"off", g_filter, g_aa ? L"FXAA" : L"off", g_sharpen, g_bright, g_lines,
        g_lines ? L"%" : L" (auto)", g_hdChars ? L"on" : L"off");
}

bool GraphicsHdCharacters() { return g_hdChars; }
bool GraphicsDrawingInk() {   // (cosmetics.cpp: the ink strokes' shader is set - their own band texture, not for HD)
    return g_curPs && std::find(g_inkPs.begin(), g_inkPs.end(), g_curPs) != g_inkPs.end();
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
    if (void* o = Patch(dev, 92, (void*)Hook_SetVertexShader)) Real_SetVertexShader = (SetVertexShader_t)o;
    if (void* o = Patch(dev, 23, (void*)Hook_CreateTexture)) Real_CreateTexture = (CreateTexture_t)o;
    static bool testLoaded = false;
    if (!testLoaded) { testLoaded = true; LoadPsConstTest(); }
    // (the ink strokes' shader: always - the research switches below go through the same three)
    if (void* o = Patch(dev, 106, (void*)Hook_CreatePixelShader)) Real_CreatePixelShader = (CreatePixelShader_t)o;
    if (void* o = Patch(dev, 107, (void*)Hook_SetPixelShader)) Real_SetPixelShader = (SetPixelShader_t)o;
    if (void* o = Patch(dev, 109, (void*)Hook_SetPSConstF)) Real_SetPSConstF = (SetPSConstF_t)o;
    if (!g_psConst.empty() || g_psTrace || g_psSkip[0]) {   // (research only - see FGUYMOD_TEST_PSCONST)
        if (void* o = Patch(dev, 82, (void*)Hook_DrawIndexedPrimitive)) Real_DrawIndexedPrimitive = (DrawIndexedPrimitive_t)o;
        if (void* o = Patch(dev, 81, (void*)Hook_DrawPrimitive)) Real_DrawPrimitive = (DrawPrimitive_t)o;
        if (void* o = Patch(dev, 83, (void*)Hook_DrawPrimitiveUP)) Real_DrawPrimitiveUP = (DrawPrimitiveUP_t)o;
        if (void* o = Patch(dev, 84, (void*)Hook_DrawIndexedPrimitiveUP)) Real_DrawIndexedPrimitiveUP = (DrawIndexedPrimitiveUP_t)o;
    }
    PutHalf(dev, w, h);
    ResetStages(dev);
}

void GraphicsBeforeReset() { ReleaseTargets(); }

void GraphicsAfterReset(IDirect3DDevice9* dev, UINT w, UINT h) {
    g_bbW = w;
    g_bbH = h;
    if (Real_SetSamplerState) ResetStages(dev);
    if (Real_SetVSConstF) PutHalf(dev, w, h);   // (a reset clears every constant: ours again)
}

// d3d9.cpp, every Present (before the frame is shown): our passes
void GraphicsPresent(IDirect3DDevice9* dev) {
    static int frames = 0;
    if (++frames == 600) Log(L"Graphics: %d of %d vertex shaders have the half-pixel fix", g_vsPatched, g_vsSeen);
    g_frame = frames;
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
const int kLines[] = {0, 100, 150, 200, 250, 300};
const char* kLineNames[] = {"AUTO (AS ON THE CONSOLE)", "100% (THE PC GAME'S)", "150%", "200%", "250%", "300%"};
int LinesIndex() {
    for (int i = 0; i < 6; ++i) if (kLines[i] == g_lines) return i;
    return 0;
}
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
        row("Line thickness", kLineNames[LinesIndex()], 'c', "The characters' outlines. Auto: as on the console at any resolution (the PC game's fade at 4K)");
        row("HD character textures", g_hdChars ? "ON" : "OFF", 'c', "The characters' textures at twice their size, made on this PC: smoother lines up close");
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
        if (row == 4) {   // (at the next stroke drawn: the constant is put whenever their shader is set)
            g_lines = kLines[(LinesIndex() + dir + 6) % 6];
            Save(L"Graphics", L"LineThickness", g_lines);
        }
        if (row == 5) { g_hdChars = !g_hdChars; Save(L"Graphics", L"HDCharacters", g_hdChars); }
    } else {
        if (row == 0) Save(L"Interface", L"MenuStyle", GraphicsMenuNew() ? 0 : 1);
        if (row == 1) return 2;
    }
    return 0;
}
