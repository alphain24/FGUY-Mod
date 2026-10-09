// DirectInput tweaks.
//
// The game creates its keyboard/mouse devices through the dinput8.dll we replace. Fullscreen games
// commonly pass DISCL_NOWINKEY to SetCooperativeLevel, which disables the Windows key while the game
// is in front. In windowed and borderless modes we drop that flag so the Start menu works.
//
// During netplay every input must come through the netplay layer (so all PCs see the same thing), so
// DirectInput devices (keyboard, mouse, DirectInput pads) report nothing to the game.
//
// COM methods are hooked by patching the vtable slots (shared by every object of that class).

#include "common.h"

#include <dinput.h>

namespace {

// IDirectInput8 vtable: 3 = CreateDevice.
// IDirectInputDevice8: 9 = GetDeviceState, 10 = GetDeviceData, 13 = SetCooperativeLevel.
const int kCreateDeviceSlot = 3;
const int kGetDeviceStateSlot = 9;
const int kGetDeviceDataSlot = 10;
const int kSetCooperativeLevelSlot = 13;

using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD size, void* data);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD objSize, void* data, DWORD* inOut, DWORD flags);
GetDeviceState_t Real_GetDeviceState;
GetDeviceData_t Real_GetDeviceData;

HRESULT STDMETHODCALLTYPE Hook_GetDeviceState(void* self, DWORD size, void* data) {
    HRESULT hr = Real_GetDeviceState(self, size, data);
    if (NetplayEnabled() && data) {
        ZeroMemory(data, size);
        return DI_OK;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_GetDeviceData(void* self, DWORD objSize, void* data, DWORD* inOut, DWORD flags) {
    HRESULT hr = Real_GetDeviceData(self, objSize, data, inOut, flags);
    if (NetplayEnabled() && inOut) {
        *inOut = 0;
        return DI_OK;
    }
    return hr;
}

using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(void* self, REFGUID, void** device, void* outer);
using SetCooperativeLevel_t = HRESULT(STDMETHODCALLTYPE*)(void* self, HWND, DWORD flags);
CreateDevice_t Real_CreateDevice;
SetCooperativeLevel_t Real_SetCooperativeLevel;

bool g_allowWinKeySetting = true;

void* PatchVtable(void* object, int slot, void* hook) {
    void** vtable = *reinterpret_cast<void***>(object);
    if (vtable[slot] == hook) return nullptr;  // already patched
    DWORD old;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old);
    void* original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return original;
}

const wchar_t* DeviceName(REFGUID guid) {
    if (guid == GUID_SysKeyboard) return L"keyboard";
    if (guid == GUID_SysMouse) return L"mouse";
    return L"other device";
}

HRESULT STDMETHODCALLTYPE Hook_SetCooperativeLevel(void* self, HWND wnd, DWORD flags) {
    DWORD newFlags = flags;
    if (g_allowWinKeySetting && DisplayIsWindowed()) newFlags &= ~DISCL_NOWINKEY;
    Log(L"Input: SetCooperativeLevel flags %#lx%s", flags,
        newFlags != flags ? L" -> Windows key allowed" : L"");
    return Real_SetCooperativeLevel(self, wnd, newFlags);
}

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(void* self, REFGUID guid, void** device, void* outer) {
    HRESULT hr = Real_CreateDevice(self, guid, device, outer);
    Log(L"Input: DirectInput %s created (hr=%#lx)", DeviceName(guid), hr);
    if (SUCCEEDED(hr) && device && *device) {
        if (void* orig = PatchVtable(*device, kSetCooperativeLevelSlot, (void*)Hook_SetCooperativeLevel))
            Real_SetCooperativeLevel = (SetCooperativeLevel_t)orig;
        if (void* orig = PatchVtable(*device, kGetDeviceStateSlot, (void*)Hook_GetDeviceState))
            Real_GetDeviceState = (GetDeviceState_t)orig;
        if (void* orig = PatchVtable(*device, kGetDeviceDataSlot, (void*)Hook_GetDeviceData))
            Real_GetDeviceData = (GetDeviceData_t)orig;
    }
    return hr;
}

}  // namespace

void InitDirectInput() {
    g_allowWinKeySetting = SettingInt(L"Input", L"AllowWindowsKey", nullptr, 1) != 0;
}

void OnDirectInputCreated(void* directInput) {
    if (!directInput) return;
    if (void* orig = PatchVtable(directInput, kCreateDeviceSlot, (void*)Hook_CreateDevice))
        Real_CreateDevice = (CreateDevice_t)orig;
}
