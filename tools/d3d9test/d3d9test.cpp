// Creates a Direct3D 9 device with the same parameters as FGUY.exe, to tell game problems apart from
// PC / driver problems. Prints the result of each variant.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")

int main() {
    WNDCLASSW wc = {0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"d3d9test"};
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(L"d3d9test", L"d3d9test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 976, 579,
                             nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    printf("Direct3DCreate9: %p, adapters %u\n", (void*)d3d, d3d ? d3d->GetAdapterCount() : 0);
    if (!d3d) return 1;
    D3DDISPLAYMODE dm;
    d3d->GetAdapterDisplayMode(0, &dm);
    printf("desktop mode %ux%u @%u format %d\n", dm.Width, dm.Height, dm.RefreshRate, dm.Format);
    struct { const char* name; D3DFORMAT bb; DWORD flags; BOOL depth; } tests[] = {
        {"game: A8R8G8B8, HWVP|PURE|MT, D24S8", D3DFMT_A8R8G8B8, 0x54, TRUE},
        {"X8R8G8B8 back buffer", D3DFMT_X8R8G8B8, 0x54, TRUE},
        {"UNKNOWN back buffer", D3DFMT_UNKNOWN, 0x54, TRUE},
        {"software vertex processing", D3DFMT_A8R8G8B8, D3DCREATE_SOFTWARE_VERTEXPROCESSING, TRUE},
        {"no depth buffer", D3DFMT_A8R8G8B8, 0x54, FALSE},
    };
    for (auto& t : tests) {
        D3DPRESENT_PARAMETERS pp = {};
        pp.BackBufferWidth = 960;
        pp.BackBufferHeight = 540;
        pp.BackBufferFormat = t.bb;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = wnd;
        pp.Windowed = TRUE;
        pp.EnableAutoDepthStencil = t.depth;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.Flags = t.depth ? D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL : 0;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9* dev = nullptr;
        HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, t.flags, &pp, &dev);
        printf("%-40s -> %#lx\n", t.name, hr);
        if (dev) dev->Release();
    }
    d3d->Release();
    DestroyWindow(wnd);
    return 0;
}
