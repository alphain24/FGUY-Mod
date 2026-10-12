// Online: the level's start-up the same tick on every PC.
//
// Starting a level, the game (0x6a6660, called each tick) waits on things that load in the background and asks
// every tick whether they're done - so a PC with a faster disk / CPU gets "done" a few ticks before another, and
// from then on its level runs those ticks ahead: the players' cameras start (0x640900) at another tick, particle
// emitters and the random numbers after them come out shifted - the match goes out of sync at the first
// shot that hits. In a session these checks wait until the loading really is done, so the answer is "done" the
// first time they ask - on every PC. (Lockstep keeps the others waiting meanwhile, as for any slow tick.)
//   0x6a66ac call 0x619530   the sound preload thread ("zSoundAsset_Preload") is finished: [0xb5a360] == 0
//   0x6a66e9 call 0x6370d0   the sound banks (FMOD event projects) are loaded
// Offline nothing changes.
#include "common.h"

namespace {

bool PatchCall(uintptr_t at, uintptr_t expected, void* to) {
    auto b = reinterpret_cast<unsigned char*>(at);
    if (b[0] != 0xE8 || at + 5 + *reinterpret_cast<int*>(b + 1) != expected) return false;
    DWORD old;
    VirtualProtect(b, 5, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<int*>(b + 1) = (int)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    VirtualProtect(b, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), b, 5);
    return true;
}

using Check_t = bool(__cdecl*)();
const uintptr_t kSoundPreloadDone = 0x619530, kSoundBanksReady = 0x6370d0;

bool Online() { return NetplayEnabled() && GetCurrentThreadId() == NetplayGameThread(); }

bool __cdecl SoundPreloadDone() {
    if (Online() && *reinterpret_cast<volatile int*>(0xb5a360) != 0) {
        ULONGLONG from = GetTickCount64(), until = from + 15000;
        while (*reinterpret_cast<volatile int*>(0xb5a360) != 0 && GetTickCount64() < until) Sleep(1);
        Log(L"Online: tick %u - waited %llu ms for the sound preload%s", NetplayTick(), GetTickCount64() - from,
            *reinterpret_cast<volatile int*>(0xb5a360) ? L" - NOT done (gave up)" : L"");
    }
    return reinterpret_cast<Check_t>(kSoundPreloadDone)();
}

bool __cdecl SoundBanksReady() {
    bool ready = reinterpret_cast<Check_t>(kSoundBanksReady)();
    if (ready || !Online()) return ready;
    ULONGLONG from = GetTickCount64(), until = from + 5000;
    while (!ready && GetTickCount64() < until) {
        Sleep(1);
        ready = reinterpret_cast<Check_t>(kSoundBanksReady)();
    }
    Log(L"Online: tick %u - waited %llu ms for the sound banks%s", NetplayTick(), GetTickCount64() - from,
        ready ? L"" : L" - NOT ready (gave up)");
    return ready;
}

}  // namespace

void InitLoadSync() {
    int ok = PatchCall(0x6a66ac, kSoundPreloadDone, (void*)SoundPreloadDone) +
             PatchCall(0x6a66e9, kSoundBanksReady, (void*)SoundBanksReady);
    Log(L"Online: level start-up waits for its loading in sessions (%d of 2 checks)", ok);
}
