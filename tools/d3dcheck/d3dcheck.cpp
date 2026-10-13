// Can this PC create graphics devices right now? (Test tools: after game copies were force-closed, some
// drivers refuse new Direct3D 9 devices for a while.) Tries Direct3D 9, Direct3D 9 on 12 and Direct3D 11, each
// on a hidden window, released again. Prints "ok" or "fail ..." per API; exit code 0 if Direct3D 9 works.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <cstdio>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

struct D3D9On12Args { BOOL Enable9On12; IUnknown* pD3D12Device; IUnknown* ppD3D12Queues[2]; UINT NumQueues; UINT NodeMask; };

HRESULT Try9(HWND wnd, bool on12) {
    IDirect3D9* d3d = nullptr;
    if (on12) {
        using F = IDirect3D9*(WINAPI*)(UINT, D3D9On12Args*, UINT);
        auto f = reinterpret_cast<F>(GetProcAddress(GetModuleHandleW(L"d3d9.dll"), "Direct3DCreate9On12"));
        D3D9On12Args a = {};
        a.Enable9On12 = TRUE;
        if (f) d3d = f(D3D_SDK_VERSION, &a, 1);
    } else {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
    }
    if (!d3d) return E_NOINTERFACE;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 320;
    pp.BackBufferHeight = 240;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (SUCCEEDED(hr)) dev->Release();
    d3d->Release();
    return hr;
}

int main() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FGUYModD3DCheck";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr,
                               wc.hInstance, nullptr);
    HRESULT h9 = Try9(wnd, false), h12 = Try9(wnd, true);
    ID3D11Device* dev11 = nullptr;
    HRESULT h11 = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev11,
                                    nullptr, nullptr);
    if (dev11) dev11->Release();
    DestroyWindow(wnd);
    printf("d3d9 %s %#lx, d3d9on12 %s %#lx, d3d11 %s %#lx\n", SUCCEEDED(h9) ? "ok" : "fail", h9,
           SUCCEEDED(h12) ? "ok" : "fail", h12, SUCCEEDED(h11) ? "ok" : "fail", h11);
    if (SUCCEEDED(h9)) printf("ok\n");
    return SUCCEEDED(h9) ? 0 : 1;
}
