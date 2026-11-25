// The Great Eggscape: the console versions' DLC challenge (SSC1 - "The Great Eggscape - Escape", Chickens in Space).
// Its level is in the PC game (FGUY/Levels/SSC1.ho, 53 MB, built with the rest), its name and its players are in the
// game's own settings (ScenePlayerMapping = SSC1 PLYS PLYS) - only the challenge list leaves it out: the function that
// fills it (0x65fd50, FCHL_GetChallenges) drops the last challenge unless "content 8" is owned (0x967790(8): bit 0 of
// the owned-content byte 0xba9289, which nothing on PC ever sets - that DLC was the consoles'). So content 8 answers
// "owned": its case in 0x967790 (mov al, [0xba9289]; and al, 1; ret) becomes mov al, 1; ret. Content 8 is asked for
// in one other place (0x693ac0, refreshing all the flags - its answer unused). Found from Jim's BTTM modding guide.
#include "common.h"

namespace {
const uintptr_t kContent8 = 0x9677f6;
const unsigned char kContent8Was[] = {0xA0, 0x89, 0x92, 0xBA, 0x00, 0x24, 0x01, 0xC3};   // mov al, [0xba9289]; and al, 1; ret
const unsigned char kOwned[] = {0xB0, 0x01, 0xC3};                                     // mov al, 1; ret
}  // namespace

void InitEggscape() {
    auto at = reinterpret_cast<unsigned char*>(kContent8);
    if (IsBadReadPtr(at, sizeof(kContent8Was)) || memcmp(at, kContent8Was, sizeof(kContent8Was)) != 0) {
        Log(L"Eggscape: the content check isn't what we expect (another game version?) - the challenge stays hidden");
        return;
    }
    DWORD old;
    VirtualProtect(at, sizeof(kOwned), PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, kOwned, sizeof(kOwned));
    VirtualProtect(at, sizeof(kOwned), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kOwned));
    Log(L"Eggscape: The Great Eggscape challenge unlocked (content 8 owned)");
}
