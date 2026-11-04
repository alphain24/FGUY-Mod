// Steady clock: offline, the game's main thread sees time move on by exactly one frame a tick, so everything moves
// the same amount from one frame to the next.
//
// The game runs a tick on its main thread (it reads the controllers, then updates everything with the time since the
// last tick) while the render thread presents the one before. Its time comes from QueryPerformanceCounter, read
// whenever its tick gets there - so its dt follows when the main thread happened to run, not when frames are shown:
// measured (the Controller Look mod's trace), on average right but frame to frame +-1.2 ms off, the view's turn uneven
// by 19% at ~140 fps. Here the main thread's reads give the tick's own time instead: the last tick's plus the frame
// interval the pacing holds (d3d9.cpp SteadySetInterval; 0 = not paced: the real gaps, smoothed), pulled gently back
// toward real time (a sixteenth of the difference a tick) and put straight back on it after a stall (a load, a
// hitch: off by more than four frames). Within a tick every read gives the same time, then (a loop waiting for time
// to pass: past a thousand reads) one count more each - netplay.cpp's fixed clock does the same.
//
// Only offline (online the lockstep has its own fixed clock: netplay.cpp), only the main thread, only
// QueryPerformanceCounter (the game's dt; its millisecond clocks are left alone). [Display] SteadyFrames=0 turns it
// off (and d3d9.cpp's steady frame rate with it).

#include "common.h"

namespace {

bool g_on = false;
DWORD g_main = 0;
decltype(&QueryPerformanceCounter) Real_QPC = nullptr;
LONGLONG g_now = 0;                  // the main thread's time this tick (QPC counts)
LONGLONG g_lastOut = 0;              // (the last time handed out: never backwards)
LONGLONG g_reads = 0;                // reads this tick
LONGLONG g_lastReal = 0;             // real time at the last tick
double g_smoothGap = 0;              // real gaps, smoothed (not paced)
volatile LONGLONG g_interval = 0;    // the frame interval the pacing holds (QPC counts; 0: none - the render thread's)

BOOL WINAPI Hook_QPC(LARGE_INTEGER* out) {
    if (!out || !g_on || !g_now || GetCurrentThreadId() != g_main) return Real_QPC(out);
    const LONGLONG kExactReads = 1000;
    LONGLONG n = ++g_reads;
    LONGLONG t = g_now + (n > kExactReads ? n - kExactReads : 0);
    if (t <= g_lastOut) t = g_lastOut + 1;
    g_lastOut = t;
    out->QuadPart = t;
    return TRUE;
}

}  // namespace

bool SteadyClockOn() { return g_on; }

// d3d9.cpp: the frame interval its pacing holds now (QPC counts), 0 if it isn't pacing.
void SteadySetInterval(LONGLONG counts) { InterlockedExchange64(&g_interval, counts); }

// vpad.cpp: a controller poll - the first of a tick on the main thread starts the tick's time.
void SteadyTick() {
    if (!g_on || GetCurrentThreadId() != g_main) return;
    LARGE_INTEGER r;
    Real_QPC(&r);
    LONGLONG real = r.QuadPart;
    if (!g_now) {
        g_now = g_lastOut = g_lastReal = real;
        return;
    }
    LONGLONG gap = real - g_lastReal;
    g_lastReal = real;
    if (gap > 0) g_smoothGap = g_smoothGap > 0 ? g_smoothGap * 0.9 + gap * 0.1 : (double)gap;
    LONGLONG held = InterlockedCompareExchange64(&g_interval, 0, 0);   // (64 bits from the other thread: whole)
    LONGLONG step = held > 0 ? held : (LONGLONG)g_smoothGap;
    if (step <= 0) step = 1;
    LONGLONG next = g_now + step;
    LONGLONG off = real - next;
    if (off > 4 * step || off < -4 * step) {   // a stall (a load, a hitch): back on real time at once
        next = real;
    } else {
        next += off / 16;                      // drift back toward real time, gently
    }
    if (next <= g_lastOut) next = g_lastOut + 1;
    g_now = next;
    g_reads = 0;
}

void InitSteadyClock(HMODULE exe) {
    if (NetplayEnabled()) return;   // (online: the lockstep's own clock)
    if (!SettingInt(L"Display", L"SteadyFrames", L"FGUYMOD_STEADY", 1)) {
        Log(L"Steady: off (SteadyFrames=0) - the game's own clock and frame times");
        return;
    }
    g_main = GetCurrentThreadId();   // (DllMain runs on the game's main thread)
    HookSpec hooks[] = {
        {"kernel32.dll", "QueryPerformanceCounter", (void*)Hook_QPC, (void**)&Real_QPC},
    };
    InstallHooks(exe, hooks);
    g_on = Real_QPC != nullptr;
    Log(L"Steady: %s", g_on ? L"the game's clock moves one frame a tick" : L"couldn't hook the game's clock");
}
