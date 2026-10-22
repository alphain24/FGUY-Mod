// Motion (research, for custom animation): a character's pose, as the game builds it each frame. The animation
// blend tree (0x522fb0) samples and blends the character's animations into a local pose - a rotation quaternion
// (x, y, z, w), a translation and a scale a bone, in its parent's space: glTF's own animation channels - and hands it
// to 0x4fac80 (cdecl: entity, rotations, translations, scales, bones), which builds each bone's model-space matrix
// (down the skeleton's parent table) into the frame's bone palette; times the inverse bind matrices (0x42c330), the
// palette skins the character's pieces on the CPU (0x4b02d0 a piece). The entity is the player's [player + 0x38]
// (its +0x1d4: the World::ModelPrototypeEntity). So, hooked there: a player's pose read (export), or ours instead
// (custom motion), with the game's own skinning, attachments and everything after it unchanged.
// MotionPlay: a motion on a player - the bones it moves (the others keep the game's own animation), blended in and
// out (emotes.cpp). Test pipe: "poserec <slot> <frames>" (the player's local pose each frame, to
// FGUYMod\motion_<slot>.bin), "posefreeze <slot>" (the pose now, kept), "poseplay <slot> <file>" (a motion, looped),
// "posestop <slot>", "findptr <value> <slot | address>" (research: where a value is, two pointers deep).
// motion_<slot>.bin: "FGMO", version 1, bones, frames (int32s); a frame: its time (s), then a bone: rotation x y z w,
// translation x y z, scale x y z (floats).
#include "common.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

const uintptr_t kPoseBuild = 0x4fac80;
const unsigned char kPoseStart[6] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};   // push ebp; mov ebp, esp; and esp, -8
using PoseBuild_t = void(__cdecl*)(unsigned char* ent, float* rot, float* pos, float* scale, int bones);
PoseBuild_t g_poseOriginal = nullptr;

const int kFloats = 10;   // a bone's: rotation 4, translation 3, scale 3

}  // namespace

struct MotionClip { int bones = 0; std::vector<float> times; std::vector<float> data; };   // data: frames x bones x 10

namespace {

using Motion = MotionClip;
const double kBlendIn = 0.15, kBlendOut = 0.25;   // (seconds)

struct Track {
    unsigned char* ent = nullptr;   // the player's entity, this frame
    int recFrames = 0;              // recording: frames still to record
    Motion rec;
    double recStart = 0;
    bool recThisFrame = false;
    bool freeze = false;            // freezing: the pose captured, kept
    std::vector<float> frozen;
    std::shared_ptr<const Motion> play;   // playing: a motion, on the bones it moves
    std::vector<bool> uses;
    bool loop = false;
    double playStart = 0, stopAt = 0;     // (stopAt: blending back to the game's own since then)
};
Track g_tracks[4];
CRITICAL_SECTION g_lock;

double Now() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
}

void Gather(const float* rot, const float* pos, const float* scale, int bones, float* out) {
    for (int b = 0; b < bones; ++b, out += kFloats) {
        memcpy(out, rot + b * 4, 16);
        memcpy(out + 4, pos + b * 3, 12);
        memcpy(out + 7, scale + b * 3, 12);
    }
}
void Scatter(const float* in, int bones, float* rot, float* pos, float* scale) {
    for (int b = 0; b < bones; ++b, in += kFloats) {
        memcpy(rot + b * 4, in, 16);
        memcpy(pos + b * 3, in + 4, 12);
        memcpy(scale + b * 3, in + 7, 12);
    }
}

// a motion's pose at time t (looped, or held at its end): between its two frames, the rotations normalized-lerped
// (the shorter way)
void Sample(const Motion& m, double t, bool loop, float* out) {
    size_t frames = m.times.size();
    double length = m.times.back() - m.times.front();
    t = length <= 0 ? 0 : loop ? fmod(t, length) : (t < length ? t : length);
    t += m.times.front();
    size_t i = 0;
    while (i + 2 < frames && m.times[i + 1] <= t) ++i;
    size_t j = frames > 1 ? i + 1 : i;
    float span = m.times[j] - m.times[i], k = span > 0 ? (float)((t - m.times[i]) / span) : 0.0f;
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    const float* a = &m.data[i * m.bones * kFloats];
    const float* b = &m.data[j * m.bones * kFloats];
    for (int bone = 0; bone < m.bones; ++bone, a += kFloats, b += kFloats, out += kFloats) {
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sign = dot < 0 ? -1.0f : 1.0f, len = 0;
        for (int c = 0; c < 4; ++c) { out[c] = a[c] + (sign * b[c] - a[c]) * k; len += out[c] * out[c]; }
        len = len > 0 ? 1.0f / sqrtf(len) : 1.0f;
        for (int c = 0; c < 4; ++c) out[c] *= len;
        for (int c = 4; c < kFloats; ++c) out[c] = a[c] + (b[c] - a[c]) * k;
    }
}

void __cdecl Hook_PoseBuild(unsigned char* ent, float* rot, float* pos, float* scale, int bones) {
    EnterCriticalSection(&g_lock);
    for (Track& tr : g_tracks) {
        if (!ent || tr.ent != ent || bones <= 0 || bones > 255) continue;
        if (tr.recFrames > 0 && !tr.recThisFrame && (tr.rec.bones == 0 || tr.rec.bones == bones)) {
            if (tr.rec.bones == 0) { tr.rec.bones = bones; tr.recStart = Now(); }
            tr.rec.times.push_back((float)(Now() - tr.recStart));
            tr.rec.data.resize(tr.rec.data.size() + bones * kFloats);
            Gather(rot, pos, scale, bones, &tr.rec.data[tr.rec.data.size() - bones * kFloats]);
            tr.recThisFrame = true;
        }
        if (tr.freeze) {
            if (tr.frozen.empty()) { tr.frozen.resize(bones * kFloats); Gather(rot, pos, scale, bones, tr.frozen.data()); }
            if ((int)tr.frozen.size() == bones * kFloats) Scatter(tr.frozen.data(), bones, rot, pos, scale);
        } else if (tr.play && tr.play->bones != bones) {
            Log(L"Motion: a motion of %d bones doesn't fit this character (%d) - not played", tr.play->bones, bones);
            tr.play.reset();
        } else if (tr.play && tr.play->bones == bones) {
            double now = Now(), e = now - tr.playStart, length = tr.play->times.back() - tr.play->times.front();
            if (!tr.loop && !tr.stopAt && e >= length) tr.stopAt = tr.playStart + length;   // (its end: back to the game's)
            double w = e < kBlendIn ? e / kBlendIn : 1.0;
            if (tr.stopAt) w *= 1.0 - (now - tr.stopAt) / kBlendOut;
            if (w <= 0 && tr.stopAt) { tr.play.reset(); continue; }
            std::vector<float> mine(bones * kFloats), game(bones * kFloats);
            Sample(*tr.play, e, tr.loop, mine.data());
            Gather(rot, pos, scale, bones, game.data());
            float k = (float)(w > 1 ? 1 : w);
            for (int b = 0; b < bones; ++b) {   // (the bones it moves: from the game's pose toward its own by k)
                if (b < (int)tr.uses.size() && !tr.uses[b]) continue;
                float* g = &game[b * kFloats];
                const float* m = &mine[b * kFloats];
                float dot = g[0] * m[0] + g[1] * m[1] + g[2] * m[2] + g[3] * m[3], sign = dot < 0 ? -1.0f : 1.0f, len = 0;
                for (int c = 0; c < 4; ++c) { g[c] += (sign * m[c] - g[c]) * k; len += g[c] * g[c]; }
                len = len > 0 ? 1.0f / sqrtf(len) : 1.0f;
                for (int c = 0; c < 4; ++c) g[c] *= len;
                for (int c = 4; c < kFloats; ++c) g[c] += (m[c] - g[c]) * k;
            }
            Scatter(game.data(), bones, rot, pos, scale);
        }
    }
    LeaveCriticalSection(&g_lock);
    g_poseOriginal(ent, rot, pos, scale, bones);
}

bool SaveMotion(const Motion& m, const wchar_t* path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    int head[4] = {0x4f4d4746, 1, m.bones, (int)m.times.size()};   // "FGMO"
    fwrite(head, sizeof(head), 1, f);
    for (size_t i = 0; i < m.times.size(); ++i) {
        fwrite(&m.times[i], 4, 1, f);
        fwrite(&m.data[i * m.bones * kFloats], 4, m.bones * kFloats, f);
    }
    fclose(f);
    return true;
}

std::vector<unsigned char> ReadAll(const wchar_t* path) {
    std::vector<unsigned char> d;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return d;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < 64 * 1024 * 1024) {
        d.resize(size);
        if (fread(d.data(), 1, size, f) != (size_t)size) d.clear();
    }
    fclose(f);
    return d;
}

unsigned char* PlayerEntity(int slot) {
    unsigned char* players[8];
    int n = GamePlayers(players, 8);
    for (int p = 0; p < n; ++p)
        if (*reinterpret_cast<int*>(players[p] + 0xdf8) == slot) return *reinterpret_cast<unsigned char**>(players[p] + 0x38);
    return nullptr;
}

// research: where a value is, from a start, following pointers (depth levels)
int g_found = 0;
void FindValue(const unsigned char* at, DWORD value, int bytes, int depth, std::wstring path) {
    for (int o = 0; o + 4 <= bytes && g_found < 40; o += 4) {
        if (IsBadReadPtr(at + o, 4)) return;
        DWORD v = *reinterpret_cast<const DWORD*>(at + o);
        wchar_t step[24];
        swprintf_s(step, L"+%#x", o);
        if (v == value) { ++g_found; Log(L"Motion: found %#lx at %s%s", value, path.c_str(), step); }
        if (depth > 0 && v >= 0x10000 && !(v & 3) && v != value && !IsBadReadPtr(reinterpret_cast<void*>(v), 4))
            FindValue(reinterpret_cast<const unsigned char*>(v), value, 0x200, depth - 1, L"[" + path + step + L"]");
    }
}

}  // namespace

std::shared_ptr<const MotionClip> MotionParse(const unsigned char* d, size_t size) {
    int head[4] = {};
    if (size < sizeof(head)) return nullptr;
    memcpy(head, d, sizeof(head));
    if (head[0] != 0x4f4d4746 || head[1] != 1 || head[2] <= 0 || head[2] > 255 || head[3] <= 0 || head[3] > 100000)
        return nullptr;
    size_t frame = 4 + (size_t)head[2] * kFloats * 4;
    if (size < sizeof(head) + frame * head[3]) return nullptr;
    auto m = std::make_shared<Motion>();
    m->bones = head[2];
    m->times.resize(head[3]);
    m->data.resize((size_t)head[3] * m->bones * kFloats);
    for (int i = 0; i < head[3]; ++i) {
        const unsigned char* at = d + sizeof(head) + frame * i;
        memcpy(&m->times[i], at, 4);
        memcpy(&m->data[(size_t)i * m->bones * kFloats], at + 4, frame - 4);
        if (i && !(m->times[i] >= m->times[i - 1])) return nullptr;   // (times only go forward)
    }
    return m;
}
int MotionBones(const MotionClip& m) { return m.bones; }
float MotionLength(const MotionClip& m) { return m.times.back() - m.times.front(); }

void MotionPlay(int slot, std::shared_ptr<const MotionClip> clip, const std::vector<bool>& uses, bool loop) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return;
    EnterCriticalSection(&g_lock);
    Track& tr = g_tracks[slot];
    tr.play = std::move(clip);
    tr.uses = uses;
    tr.loop = loop;
    tr.playStart = Now();
    tr.stopAt = 0;
    tr.freeze = false;
    LeaveCriticalSection(&g_lock);
}
void MotionStop(int slot) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return;
    EnterCriticalSection(&g_lock);
    if (g_tracks[slot].play && !g_tracks[slot].stopAt) g_tracks[slot].stopAt = Now();
    LeaveCriticalSection(&g_lock);
}
bool MotionPlaying(int slot) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return false;
    EnterCriticalSection(&g_lock);
    bool on = g_tracks[slot].play && !g_tracks[slot].stopAt;
    LeaveCriticalSection(&g_lock);
    return on;
}

void MotionFrame() {
    if (!g_poseOriginal) return;
    EnterCriticalSection(&g_lock);
    for (int s = 0; s < 4; ++s) {
        Track& tr = g_tracks[s];
        tr.ent = PlayerEntity(s);
        if (tr.recThisFrame && --tr.recFrames <= 0) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\motion_%d.bin", g_modDir, s);
            bool ok = SaveMotion(tr.rec, path);
            Log(L"Motion: player %d - %zu frames of %d bones %s %s", s, tr.rec.times.size(), tr.rec.bones,
                ok ? L"saved to" : L"NOT saved to", path);
            tr.rec = Motion();
            tr.recFrames = 0;
        }
        tr.recThisFrame = false;
    }
    LeaveCriticalSection(&g_lock);
}

void MotionCommand(const char* cmd, const char* a, const char* b) {
    if (!g_poseOriginal) { Log(L"Motion: not hooked"); return; }
    int slot = a ? atoi(a) : -1;
    bool forSlot = slot >= 0 && slot < 4;
    EnterCriticalSection(&g_lock);
    if (_stricmp(cmd, "poserec") == 0 && forSlot) {
        g_tracks[slot].rec = Motion();
        g_tracks[slot].recFrames = b ? atoi(b) : 60;
        Log(L"Motion: player %d - recording %d frames", slot, g_tracks[slot].recFrames);
    }
    if (_stricmp(cmd, "posefreeze") == 0 && forSlot) {
        g_tracks[slot].freeze = !(b && atoi(b) == 0);
        g_tracks[slot].frozen.clear();
        Log(L"Motion: player %d - %s", slot, g_tracks[slot].freeze ? L"pose frozen" : L"pose free");
    }
    if (_stricmp(cmd, "posestop") == 0 && forSlot) {
        if (g_tracks[slot].play && !g_tracks[slot].stopAt) g_tracks[slot].stopAt = Now();
        g_tracks[slot].freeze = false;
        Log(L"Motion: player %d - the game's own pose again", slot);
    }
    LeaveCriticalSection(&g_lock);
    if (_stricmp(cmd, "poseplay") == 0 && forSlot && b) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%S", g_modDir, b);
        std::vector<unsigned char> d = ReadAll(path);
        if (auto m = MotionParse(d.data(), d.size())) {
            Log(L"Motion: player %d - playing %s (%zu frames, %d bones)", slot, path, m->times.size(), m->bones);
            MotionPlay(slot, m, {}, true);
        } else {
            Log(L"Motion: can't read %s", path);
        }
    }
    if (_stricmp(cmd, "findptr") == 0 && a && b) {   // findptr <value> <player slot 0-3 | hex address>
        DWORD value = strtoul(a, nullptr, 16);
        const unsigned char* from = strlen(b) == 1 ? nullptr : reinterpret_cast<const unsigned char*>(strtoul(b, nullptr, 16));
        if (strlen(b) == 1) {
            unsigned char* players[8];
            int n = GamePlayers(players, 8);
            for (int p = 0; p < n; ++p)
                if (*reinterpret_cast<int*>(players[p] + 0xdf8) == atoi(b)) from = players[p];
        }
        g_found = 0;
        Log(L"Motion: looking for %#lx from %p", value, from);
        if (from) FindValue(from, value, 0x1000, 2, L"");
        Log(L"Motion: %d found", g_found);
    }
}

void InitMotion() {
    InitializeCriticalSection(&g_lock);
    auto at = reinterpret_cast<unsigned char*>(kPoseBuild);
    if (IsBadReadPtr(at, sizeof(kPoseStart)) || memcmp(at, kPoseStart, sizeof(kPoseStart)) != 0) {
        Log(L"Motion: pose code isn't what we expect (different game version?) - off");
        return;
    }
    auto t = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return;
    memcpy(t, kPoseStart, sizeof(kPoseStart));
    t[sizeof(kPoseStart)] = 0xE9;
    *reinterpret_cast<int*>(t + sizeof(kPoseStart) + 1) =
        (int)((kPoseBuild + sizeof(kPoseStart)) - (reinterpret_cast<uintptr_t>(t) + sizeof(kPoseStart) + 5));
    g_poseOriginal = reinterpret_cast<PoseBuild_t>(t);
    DWORD old;
    VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    *reinterpret_cast<int*>(at + 1) = (int)(reinterpret_cast<uintptr_t>(&Hook_PoseBuild) - (kPoseBuild + 5));
    at[5] = 0x90;
    VirtualProtect(at, 6, old, &old);
}
