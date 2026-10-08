// DirectInput tweaks.
//
// The game creates its keyboard/mouse devices through the dinput8.dll we replace. Fullscreen games
// commonly pass DISCL_NOWINKEY to SetCooperativeLevel, which disables the Windows key while the game
// is in front. In windowed and borderless modes we drop that flag so the Start menu works.
//
// COM methods are hooked by patching the vtable slots (shared by every object of that class).

#include "common.h"

#include <dinput.h>

namespace {

// IDirectInput8 vtable: 3 = CreateDevice. IDirectInputDevice8: 13 = SetCooperativeLevel.
const int kCreateDeviceSlot = 3;
const int kSetCooperativeLevelSlot = 13;

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
