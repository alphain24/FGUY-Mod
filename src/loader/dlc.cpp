// Peter Griffin's Man Boob Mega Sweat Pack: the game's DLC (consoles, and Steam from 2013-01-14 until the game left
// Steam in December 2014 - it can't be bought any more). Its content is all in the PC files already; owning it only
// sets four "content" flags, 8 to 11 (0x967790 answers "is content n owned?" from bits of 0xba9288 / 0xba9289).
// The game asks Steam about one DLC app, 223691 (0x693ac0: if owned, contents 8, 9, 10 and 11 are all set), so the
// pack is one unlock on PC:
//   8  - The Great Eggscape (SSC1, the bonus challenge: FCHL_GetChallenges 0x65fd50 drops it unless owned)
//   9  - costumes marked DLC 1 in the costume table (0x66f100 / 0x5f6cb0 / 0x66ede0 map DLC 1/2/3 to content 9/10/11)
//   10 - costumes marked DLC 2   (9 and 10: the Humiliation Pack - Stewie's diaper, pink Brian - and the Opening
//                                 Number Pack - the gold tuxedos)
//   11 - the Evil Pack: Evil Stewie and Evil Mayor West with their own melee weapons ("HasEvilDLC" in the menus)
// Each content's case in 0x967790 (mov al, [flags]; shr al, n; and al, 1; ret) becomes mov al, 1; ret.
// Found from Jim's BTTM modding guide (the Eggscape lead).
#include "common.h"

namespace {
struct Content {
    int n;
    uintptr_t at;
    unsigned char was[11];
    size_t len;
    const wchar_t* what;
};
const Content kContents[] = {
    {8, 0x9677f6, {0xA0, 0x89, 0x92, 0xBA, 0x00, 0x24, 0x01, 0xC3}, 8, L"The Great Eggscape"},
    {9, 0x9677fe, {0xA0, 0x89, 0x92, 0xBA, 0x00, 0xD0, 0xE8, 0x24, 0x01, 0xC3}, 10, L"DLC costumes 1"},
    {10, 0x967808, {0xA0, 0x89, 0x92, 0xBA, 0x00, 0xC0, 0xE8, 0x02, 0x24, 0x01, 0xC3}, 11, L"DLC costumes 2"},
    {11, 0x967813, {0xA0, 0x89, 0x92, 0xBA, 0x00, 0xC0, 0xE8, 0x03, 0x24, 0x01, 0xC3}, 11, L"the Evil Pack"},
};
const unsigned char kOwned[] = {0xB0, 0x01, 0xC3};   // mov al, 1; ret
}  // namespace

void InitDlc() {
    for (const auto& c : kContents) {
        auto at = reinterpret_cast<unsigned char*>(c.at);
        if (IsBadReadPtr(at, c.len) || memcmp(at, c.was, c.len) != 0) {
            Log(L"DLC: content %d's check isn't what we expect (another game version?) - %s stays locked", c.n, c.what);
            continue;
        }
        DWORD old;
        VirtualProtect(at, sizeof(kOwned), PAGE_EXECUTE_READWRITE, &old);
        memcpy(at, kOwned, sizeof(kOwned));
        VirtualProtect(at, sizeof(kOwned), old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, sizeof(kOwned));
        Log(L"DLC: content %d owned - %s", c.n, c.what);
    }
}
