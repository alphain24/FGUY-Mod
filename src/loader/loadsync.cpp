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

// (research: the tick each check turns "yes" - every PC must log the same ticks)
void NoteCheck(const wchar_t* what, bool now, bool* last) {
    if (now && !*last && NetplayEnabled()) Log(L"Online: tick %u - %s: yes", NetplayTick(), what);
    *last = now;
}

// 0x97fcd0 (this = 0xbadd60): both start-up flags at +0x402 / +0x403 (the second: the loading screen said
// SetReadyForTermination). Only watched.
using Flags_t = bool(__thiscall*)(void*);
const uintptr_t kStartFlags = 0x97fcd0;
bool __fastcall StartFlags(void* self, void*) {
    static bool last = false;
    bool r = reinterpret_cast<Flags_t>(kStartFlags)(self);
    NoteCheck(L"start-up flags", r, &last);
    return r;
}

bool __cdecl SoundBanksReady() {
    static bool last = false;
    bool ready = reinterpret_cast<Check_t>(kSoundBanksReady)();
    if (ready || !Online()) {
        NoteCheck(L"sound banks", ready, &last);
        return ready;
    }
    ULONGLONG from = GetTickCount64(), until = from + 5000;
    while (!ready && GetTickCount64() < until) {
        Sleep(1);
        ready = reinterpret_cast<Check_t>(kSoundBanksReady)();
    }
    Log(L"Online: tick %u - waited %llu ms for the sound banks%s", NetplayTick(), GetTickCount64() - from,
        ready ? L"" : L" - NOT ready (gave up)");
    NoteCheck(L"sound banks", ready, &last);
    return ready;
}

// The game's file reads: 0x484be0(handle) -> 0 none, 1 queued, 2 reading, 3 done, 4 failed (from the loader's
// slots, 0x4d88d0). Asked by the package loading progress (0x582ff0) and two more. Still queued / reading: wait
// for it (the reads are done by the loader's own thread). Should a wait ever not end, the reads need this thread
// after all - no more waiting this match (it would only stall).
using LoadState_t = int(__cdecl*)(DWORD handle);
const uintptr_t kLoadState = 0x484be0;
bool g_loadWaitBroken = false;

int __cdecl LoadState(DWORD handle) {
    int state = reinterpret_cast<LoadState_t>(kLoadState)(handle);
    if ((state != 1 && state != 2) || g_loadWaitBroken || !Online()) return state;
    ULONGLONG from = GetTickCount64(), until = from + 5000;
    while ((state == 1 || state == 2) && GetTickCount64() < until) {
        Sleep(1);
        state = reinterpret_cast<LoadState_t>(kLoadState)(handle);
    }
    static int logged = 0;
    if (state == 1 || state == 2) {
        g_loadWaitBroken = true;
        Log(L"Online: tick %u - a file read didn't finish in 5 s while waiting - not waiting for reads any more",
            NetplayTick());
    } else if (++logged <= 20) {
        Log(L"Online: tick %u - waited %llu ms for a file read", NetplayTick(), GetTickCount64() - from);
    }
    return state;
}

// The menus' movies: the game thread's UI manager (0x416220, each tick) hands each new movie to the render thread
// to load (flags +0x18: bit 0 loading, bit 1 loaded; done in 0x443fc0 while that frame is drawn). The game then
// starts the movie the first tick it sees it loaded - one tick or the next, depending on how quick the render
// thread is. So at the start of each session tick: a movie still loading is waited for (the render thread is on
// the previous tick's frame then, on its own).
void* volatile g_uiManager = nullptr;
extern "C" void __stdcall NoteUiManager(void* m) { g_uiManager = m; }

__declspec(naked) void UiManagerStub() {
    __asm {
        pushad
        push ecx
        call NoteUiManager
        popad
        push ebp
        mov ebp, esp
        sub esp, 8
        push 0x416226
        ret
    }
}

bool MovieLoading(unsigned char* manager) {
    auto head = manager + 0xc;
    int guard = 0;
    for (auto node = *reinterpret_cast<unsigned char**>(manager + 0x10); node && node != head && guard < 256;
         node = *reinterpret_cast<unsigned char**>(node + 4), ++guard) {
        auto entry = *reinterpret_cast<unsigned char**>(node + 8);
        if (entry && (*reinterpret_cast<volatile unsigned char*>(entry + 0x18) & 3) == 1) return true;
    }
    return false;
}

bool PatchStart(uintptr_t at, const unsigned char* expected, size_t n, void* to) {
    auto b = reinterpret_cast<unsigned char*>(at);
    if (memcmp(b, expected, n) != 0) return false;
    DWORD old;
    VirtualProtect(b, n, PAGE_EXECUTE_READWRITE, &old);
    b[0] = 0xE9;
    *reinterpret_cast<int*>(b + 1) = (int)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    for (size_t i = 5; i < n; ++i) b[i] = 0x90;
    VirtualProtect(b, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), b, n);
    return true;
}

// 0x4217b0: is any of the loader's jobs ([0xaef408]'s list) still running (state 3 / 4)? The boot (0x9a8643)
// and the level load (0x67d2a7) go on only once it says no. Still running: wait for it (same safety as above).
using Busy_t = bool(__cdecl*)();
const uintptr_t kLoaderBusy = 0x4217b0;
bool g_busyWaitBroken = false;

bool __cdecl LoaderBusy() {
    bool busy = reinterpret_cast<Busy_t>(kLoaderBusy)();
    if (!busy || g_busyWaitBroken || !Online()) return busy;
    ULONGLONG from = GetTickCount64(), until = from + 5000;
    while (busy && GetTickCount64() < until) {
        Sleep(1);
        busy = reinterpret_cast<Busy_t>(kLoaderBusy)();
    }
    if (busy) {
        g_busyWaitBroken = true;
        Log(L"Online: tick %u - the loader stayed busy 5 s while waiting - not waiting for it any more", NetplayTick());
    } else {
        Log(L"Online: tick %u - waited %llu ms for the loader", NetplayTick(), GetTickCount64() - from);
    }
    return busy;
}

// A player's look (0x6a71f0, every tick): if their model's materials have a certain flag (0x62c170 - looked up
// once per model, from the materials as the render side has them: one PC can say yes where another says no, it
// depends how far each PC's graphics had got), the game picks a random variation every so often for the model
// (player +0x20f0 -> its node's +0x130, used nowhere else). That took one of the game's random numbers on one PC
// only - every random thing after it came out different (a match went out of sync at a kill, the respawn spot
// differed). Online that pick takes its own numbers instead; it's only looks.
using Random01_t = float(__cdecl*)();
const uintptr_t kRandom01 = 0x4f69a0;
float __cdecl LookRandom() {
    if (!Online()) return reinterpret_cast<Random01_t>(kRandom01)();
    static unsigned s = 0x9e3779b9u ^ GetCurrentProcessId();
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (s >> 8) * (1.0f / 16777216.0f);
}

// A sound-driven effect's timer (0x5c0370: a character's mouth / flicker while one of its sounds plays - the
// component at an enemy's +0x860): FMOD's "event started / finished" callbacks (0x67e240 -> 0x5f5270) switch it on
// and off, and those come as the sound really plays - at a different tick on each PC. While on, it draws random
// numbers (intervals between a and b, 0x424c60, and a chance, 0x4f69a0): in Multiverse Madness the enemies' sounds
// made the PCs' random numbers part a few seconds in, and the match with them. Online it takes its own numbers;
// it's only looks.
// A level's movie (0x98a2c0 plays one, then says so: 0xbad9e8 = 1). The story's levels start with one, and while
// it's due the game holds all its sound (the master category's reason 8, set when the movie's asked for; taken off
// by the level's frame once "played" is said, 0x6a68f5). Sessions boot with NoMovies (config.cpp - nobody sits
// through the logos), so the movie was skipped (0x98a2c8: [0xbad9d8] set -> straight out) without saying so: online
// story co-op had no sound at all. Online a skipped movie says "played" too, as playing it would.
extern "C" void __cdecl MovieSkipped() {
    if (!NetplayEnabled()) return;
    *reinterpret_cast<volatile BYTE*>(0xbad9e8) = 1;
    static int logged = 0;
    if (++logged <= 5) Log(L"Online: tick %u - a level movie skipped (no movies in sessions) - its sound hold let go", NetplayTick());
}
__declspec(naked) void MovieSkipStub() {
    __asm {
        pushad
        pushfd
        call MovieSkipped
        popfd
        popad
        push 0x98a3d6
        ret
    }
}

using Between_t = float(__cdecl*)(float a, float b);
const uintptr_t kBetween = 0x424c60;
float __cdecl EffectBetween(float a, float b) {
    if (!Online()) return reinterpret_cast<Between_t>(kBetween)(a, b);
    float r = LookRandom();
    return (1.0f - r) * a + r * b;
}

}  // namespace

// netplay.cpp, the start of every session tick (game thread).
void LoadSyncTickStart() {
    auto manager = static_cast<unsigned char*>(g_uiManager);
    if (!manager || !MovieLoading(manager)) return;
    ULONGLONG from = GetTickCount64(), until = from + 3000;
    while (MovieLoading(manager) && GetTickCount64() < until) Sleep(1);
    static int logged = 0;
    if (++logged <= 30)
        Log(L"Online: tick %u - waited %llu ms for a menu movie to load%s", NetplayTick(), GetTickCount64() - from,
            MovieLoading(manager) ? L" - still loading (gave up)" : L"");
}

void InitLoadSync() {
    static const unsigned char kUiManagerStart[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08};
    if (!PatchStart(0x416220, kUiManagerStart, sizeof(kUiManagerStart), (void*)UiManagerStub))
        Log(L"Online: the UI manager isn't what we expect - menu movies not waited for");
    int ok = PatchCall(0x6a66ac, kSoundPreloadDone, (void*)SoundPreloadDone) +
             PatchCall(0x6a66e9, kSoundBanksReady, (void*)SoundBanksReady) +
             PatchCall(0x583021, kLoadState, (void*)LoadState) + PatchCall(0x6085f1, kLoadState, (void*)LoadState) +
             PatchCall(0x608680, kLoadState, (void*)LoadState) + PatchCall(0x6a66d6, kStartFlags, (void*)StartFlags) +
             PatchCall(0x6a670d, kStartFlags, (void*)StartFlags) + PatchCall(0x67d2a7, kLoaderBusy, (void*)LoaderBusy) +
             PatchCall(0x9a8643, kLoaderBusy, (void*)LoaderBusy);
    Log(L"Online: level start-up waits for its loading in sessions (%d of 9 checks)", ok);
    if (!PatchCall(0x6a724d, kRandom01, (void*)LookRandom))
        Log(L"Online: the player look's random pick isn't what we expect - left as it is");
    int fx = PatchCall(0x5c0549, kBetween, (void*)EffectBetween) + PatchCall(0x5c0584, kBetween, (void*)EffectBetween) +
             PatchCall(0x5c05eb, kBetween, (void*)EffectBetween) + PatchCall(0x5c0608, kBetween, (void*)EffectBetween) +
             PatchCall(0x5c0554, kRandom01, (void*)LookRandom);
    if (fx != 5) Log(L"Online: the sound effects' timer isn't what we expect (%d of 5) - left as it is", fx);
    // 0x98a2ce: jne 0x98a3d6 (no movies: out) - to MovieSkipStub
    static const unsigned char kNoMovieJump[] = {0x0f, 0x85, 0x02, 0x01, 0x00, 0x00};
    auto jump = reinterpret_cast<unsigned char*>(0x98a2ce);
    if (!memcmp(jump, kNoMovieJump, sizeof(kNoMovieJump))) {
        DWORD old;
        VirtualProtect(jump, 6, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<int*>(jump + 2) = (int)(reinterpret_cast<uintptr_t>(&MovieSkipStub) - (0x98a2ce + 6));
        VirtualProtect(jump, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), jump, 6);
    } else {
        Log(L"Online: the level movie code isn't what we expect - a skipped movie may leave the sound held");
    }
}
