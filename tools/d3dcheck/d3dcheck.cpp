// Can this PC create a Direct3D 9 device right now? (Test tools: after game copies were force-closed, some
// drivers refuse new devices for a while.) Creates one small windowed device on a hidden window, releases it.
// Prints "ok" / "fail <hr>", exit code 0 / 1.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")

int main() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FGUYModD3DCheck";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr,
                               wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("fail no-d3d9\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 320;
    pp.BackBufferHeight = 240;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (SUCCEEDED(hr)) dev->Release();
    d3d->Release();
    DestroyWindow(wnd);
    if (FAILED(hr)) { printf("fail %#lx\n", hr); return 1; }
    printf("ok\n");
    return 0;
}
