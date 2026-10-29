// Research (FGUYMOD_TEST_POOLTRACE=1): which D3DPOOL_DEFAULT resources are still alive when a Reset is refused
// (INVALIDCALL - Direct3D 9 only resets a device none of those are left on), and which of the game's code made them.
// Every DEFAULT-pool texture, buffer and surface made through the device is kept in a list until its last Release;
// PoolTraceDump (d3d9.cpp, on a failed Reset) writes out what is left.
#include "common.h"

#include <d3d9.h>
#include <intrin.h>

#include <map>

namespace {

struct Live {
    void* from;      // the code that made it (the game's, past the mod's own hooks)
    char kind;       // t texture, v volume, c cube, b vertex buffer, i index buffer, r render target, d depth, o offscreen
    UINT w, h;
    DWORD usage;
    int format;
};
CRITICAL_SECTION g_lock;
std::map<void*, Live> g_live;
bool g_on = false;

void Add(void* p, const Live& l) {
    EnterCriticalSection(&g_lock);
    g_live[p] = l;
    LeaveCriticalSection(&g_lock);
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

// Release, per kind of object (each has its own vtable)
using Release_t = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
Release_t g_release[8];
template <int K>
ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* self) {
    ULONG n = g_release[K](self);
    if (n == 0) {
        EnterCriticalSection(&g_lock);
        g_live.erase(self);
        LeaveCriticalSection(&g_lock);
    }
    return n;
}
template <int K>
void Watch(void* object) {
    if (!g_release[K])
        if (void* o = Patch(object, 2, (void*)Hook_Release<K>)) g_release[K] = (Release_t)o;
}

using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                    IDirect3DTexture9**, HANDLE*);
using CreateVolume_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                   IDirect3DVolumeTexture9**, HANDLE*);
using CreateCube_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                 IDirect3DCubeTexture9**, HANDLE*);
using CreateVB_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**,
                                               HANDLE*);
using CreateIB_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**,
                                               HANDLE*);
using CreateRT_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                               IDirect3DSurface9**, HANDLE*);
using CreateOff_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**,
                                                HANDLE*);
CreateTexture_t Real_CreateTexture;
CreateVolume_t Real_CreateVolume;
CreateCube_t Real_CreateCube;
CreateVB_t Real_CreateVB;
CreateIB_t Real_CreateIB;
CreateRT_t Real_CreateRT, Real_CreateDS;
CreateOff_t Real_CreateOff;

HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* self, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f,
                                             D3DPOOL pool, IDirect3DTexture9** out, HANDLE* share) {
    HRESULT hr = Real_CreateTexture(self, w, h, levels, usage, f, pool, out, share);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_DEFAULT) {
        Watch<0>(*out);
        Add(*out, {_ReturnAddress(), 't', w, h, usage, (int)f});
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hook_CreateVolume(IDirect3DDevice9* self, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
                                            D3DFORMAT f, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* share) {
    HRESULT hr = Real_CreateVolume(self, w, h, d, levels, usage, f, pool, out, share);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_DEFAULT) {
        Watch<1>(*out);
        Add(*out, {_ReturnAddress(), 'v', w, h, usage, (int)f});
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hook_CreateCube(IDirect3DDevice9* self, UINT size, UINT levels, DWORD usage, D3DFORMAT f,
                                          D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* share) {
    HRESULT hr = Real_CreateCube(self, size, levels, usage, f, pool, out, share);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_DEFAULT) {
        Watch<2>(*out);
        Add(*out, {_ReturnAddress(), 'c', size, size, usage, (int)f});
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hook_CreateVB(IDirect3DDevice9* self, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                        IDirect3DVertexBuffer9** out, HANDLE* share) {
    HRESULT hr = Real_CreateVB(self, len, usage, fvf, pool, out, share);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_DEFAULT) {
        Watch<3>(*out);
        Add(*out, {_ReturnAddress(), 'b', len, 0, usage, (int)fvf});
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hook_CreateIB(IDirect3DDevice9* self, UINT len, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                        IDirect3DIndexBuffer9** out, HANDLE* share) {
    HRESULT hr = Real_CreateIB(self, len, usage, f, pool, out, share);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_DEFAULT) {
        Watch<4>(*out);
        Add(*out, {_ReturnAddress(), 'i', len, 0, usage, (int)f});
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hook_CreateRT(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                        DWORD q, BOOL lockable, IDirect3DSurface9** out, HANDLE* share) {
    HRESULT hr = Real_CreateRT(self, w, h, f, ms, q, lockable, out, share);
    if (SUCCEEDED(hr) && out && *out) {
        Watch<5>(*out);
        Add(*out, {_ReturnAddress(), 'r', w, h, (DWORD)ms, (int)f});
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hook_CreateDS(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms,
                                        DWORD q, BOOL discard, IDirect3DSurface9** out, HANDLE* share) {
    HRESULT hr = Real_CreateDS(self, w, h, f, ms, q, discard, out, share);
    if (SUCCEEDED(hr) && out && *out) {
        Watch<5>(*out);
        Add(*out, {_ReturnAddress(), 'd', w, h, (DWORD)ms, (int)f});
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hook_CreateOff(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT f, D3DPOOL pool,
                                         IDirect3DSurface9** out, HANDLE* share) {
    HRESULT hr = Real_CreateOff(self, w, h, f, pool, out, share);
    if (SUCCEEDED(hr) && out && *out && pool == D3DPOOL_DEFAULT) {
        Watch<5>(*out);
        Add(*out, {_ReturnAddress(), 'o', w, h, 0, (int)f});
    }
    return hr;
}

}  // namespace

// d3d9.cpp: the device, after every other hook on it (so the code these see calling is the game's)
void PoolTraceInstall(IDirect3DDevice9* dev) {
    wchar_t v[4];
    if (g_on || !GetEnvironmentVariableW(L"FGUYMOD_TEST_POOLTRACE", v, 4)) return;
    g_on = true;
    InitializeCriticalSection(&g_lock);
    if (void* o = Patch(dev, 23, (void*)Hook_CreateTexture)) Real_CreateTexture = (CreateTexture_t)o;
    if (void* o = Patch(dev, 24, (void*)Hook_CreateVolume)) Real_CreateVolume = (CreateVolume_t)o;
    if (void* o = Patch(dev, 25, (void*)Hook_CreateCube)) Real_CreateCube = (CreateCube_t)o;
    if (void* o = Patch(dev, 26, (void*)Hook_CreateVB)) Real_CreateVB = (CreateVB_t)o;
    if (void* o = Patch(dev, 27, (void*)Hook_CreateIB)) Real_CreateIB = (CreateIB_t)o;
    if (void* o = Patch(dev, 28, (void*)Hook_CreateRT)) Real_CreateRT = (CreateRT_t)o;
    if (void* o = Patch(dev, 29, (void*)Hook_CreateDS)) Real_CreateDS = (CreateRT_t)o;
    if (void* o = Patch(dev, 36, (void*)Hook_CreateOff)) Real_CreateOff = (CreateOff_t)o;
    Log(L"Pool trace: on - the device's D3DPOOL_DEFAULT resources are kept track of");
}

// d3d9.cpp: a Reset was refused - what is still alive
void PoolTraceDump() {
    if (!g_on) return;
    EnterCriticalSection(&g_lock);
    Log(L"Pool trace: %u D3DPOOL_DEFAULT resources alive", (unsigned)g_live.size());
    int n = 0;
    for (auto& e : g_live) {
        if (++n > 40) break;
        const Live& l = e.second;
        Log(L"Pool trace:   %c %p %ux%u usage %#lx format %d, made from %p", l.kind, e.first, l.w, l.h, l.usage, l.format,
            l.from);
    }
    LeaveCriticalSection(&g_lock);
}
