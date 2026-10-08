// Direct3D 9 fixes.
//
// The game sometimes fails to create its Direct3D device at startup and then crashes using the
// missing device (FGUY.exe +0x876e9, NULL device pointer). We wrap IDirect3D9::CreateDevice: on
// failure we log the reason and retry for a few seconds before giving the error back to the game.

#include "common.h"

#include <d3d9.h>

namespace {

const int kCreateDeviceSlot = 16;  // IDirect3D9 vtable

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
Direct3DCreate9_t Real_Direct3DCreate9;
CreateDevice_t Real_CreateDevice;

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                            DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    D3DPRESENT_PARAMETERS original = *pp;  // CreateDevice may modify it; retry with what the game asked
    HRESULT hr = E_FAIL;
    for (int attempt = 1; attempt <= 20; ++attempt) {
        *pp = original;
        hr = Real_CreateDevice(self, adapter, type, focus, flags, pp, out);
        Log(L"D3D: CreateDevice %ux%u %s -> %#lx%s", original.BackBufferWidth, original.BackBufferHeight,
            original.Windowed ? L"windowed" : L"fullscreen", hr, attempt > 1 ? L" (retry)" : L"");
        if (SUCCEEDED(hr)) break;
        Sleep(250);
    }
    return hr;
}

IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d3d = Real_Direct3DCreate9(sdk);
    if (d3d) {
        void** vtable = *reinterpret_cast<void***>(d3d);
        if (vtable[kCreateDeviceSlot] != (void*)Hook_CreateDevice) {
            DWORD old;
            VirtualProtect(&vtable[kCreateDeviceSlot], sizeof(void*), PAGE_READWRITE, &old);
            Real_CreateDevice = (CreateDevice_t)vtable[kCreateDeviceSlot];
            vtable[kCreateDeviceSlot] = (void*)Hook_CreateDevice;
            VirtualProtect(&vtable[kCreateDeviceSlot], sizeof(void*), old, &old);
        }
    }
    return d3d;
}

}  // namespace

void InitD3D9(HMODULE exe) {
    HookSpec hooks[] = {
        {"d3d9.dll", "Direct3DCreate9", (void*)Hook_Direct3DCreate9, (void**)&Real_Direct3DCreate9},
    };
    InstallHooks(exe, hooks);
}
