#pragma once
// Shared bits for the loader's modules.
#include <windows.h>

extern wchar_t g_gameDir[MAX_PATH];  // folder containing FGUY.exe
extern wchar_t g_modDir[MAX_PATH];   // <game>\FGUYMod

void Log(const wchar_t* fmt, ...);
void OpenLog();

bool FileExists(const wchar_t* path);

// Loader settings live in FGUYMod\FGUYMod.ini; an environment variable of the same meaning wins
// over the file (used by test scripts). Returns the value from env, then file, then the default.
int SettingInt(const wchar_t* section, const wchar_t* key, const wchar_t* envVar, int def);
void SettingString(const wchar_t* section, const wchar_t* key, const wchar_t* envVar,
                   const wchar_t* def, wchar_t* out, DWORD outLen);

// One import-table hook. `fn` is a name or an ordinal (MAKEINTRESOURCEA).
struct HookSpec {
    const char* dll;
    const char* fn;
    void* hook;
    void** real;
};
void InstallHooks(HMODULE exe, HookSpec* hooks, size_t count);
template <size_t N>
void InstallHooks(HMODULE exe, HookSpec (&hooks)[N]) { InstallHooks(exe, hooks, N); }

// Modules
void InitConfigOverride(HMODULE exe);  // fguy.ini override + file-open log   (config.cpp)
void InitNetSpy(HMODULE exe);          // winsock + debug-print logging        (netspy.cpp)
void InitDisplay(HMODULE exe);         // windowed / borderless / resolution   (display.cpp)
void InitVirtualPad(HMODULE exe);      // injected controller input for tests  (vpad.cpp)
void InitDirectInput();                // Windows key in windowed modes        (dinput.cpp)
void OnDirectInputCreated(void* directInput);

void InitD3D9(HMODULE exe);            // Direct3D device creation retries      (d3d9.cpp)
void InitUi();                         // menu movie patches + menu calls      (ui.cpp)

// display.cpp
bool DisplayIsWindowed();              // windowed or borderless right now
int DisplayMenuIndex();                // Options > Screen > Display Mode support
bool DisplayMenuSelect(int index);     //   returns the game's own "windowed" flag for that choice
void DisplayMenuApply();
void DisplayMenuReset();
void DisplayOnChangeResolution(int width, int height, bool windowed);
