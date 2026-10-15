// Example FGUY Mod plugin: says hello, then every few seconds logs where the players are. It changes nothing in the
// game (so mod.ini calls it cosmetic). Build: build.bat (Visual Studio, 32-bit), then copy mod.ini and
// build\example.dll into <game>\FGUYMod\mods\Example\ and switch it on in Main Menu > MODS.
#include <windows.h>

#include <cstdio>

#include "../fguymod_plugin.h"

static const FGUYModAPI* g_api = nullptr;

static void OnTick(void*) {
    static unsigned last = 0;
    unsigned tick = g_api->Tick();
    int every = g_api->Setting(L"LogEverySeconds", 10) * g_api->TickRate();
    if (every <= 0 || tick - last < (unsigned)every) return;
    last = tick;
    void* players[8];
    int n = g_api->GetPlayers(players, 8);
    for (int i = 0; i < n; ++i) {
        const float* pos = g_api->PlayerPosition(players[i]);
        if (!pos) continue;
        wchar_t line[160];
        swprintf_s(line, L"tick %u: player %d at (%.1f, %.1f, %.1f), health %.0f%s", tick, g_api->PlayerIndex(players[i]),
                   pos[0], pos[1], pos[2], g_api->PlayerHealth(players[i]), g_api->IsOnline() ? L" (online)" : L"");
        g_api->Log(line);
    }
}

extern "C" __declspec(dllexport) int FGUYModInit(const FGUYModAPI* api) {
    if (!api || api->version < 1) return 0;
    g_api = api;
    api->Log(L"hello from the example plugin");
    api->OnTick(OnTick, nullptr);
    return 1;
}
