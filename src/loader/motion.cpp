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
    std::vector<float> last;              // the pose this frame, as built (models.cpp skins a new model with it)
    int lastBones = 0;
    bool haveHead = false;                // where their head is this frame, in the world (firstperson.cpp)
    bool armsInView = false;              // first person: their gun brought into their view, their head shrunk away
    float head[3] = {};
};
Track g_tracks[4];
volatile bool g_palArm[4];
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

// A bone's local transform (rotation x y z w, translation, scale) applied to a point.
void Apply(const float* rot, const float* pos, const float* scale, const float* in, float* out) {
    float x = rot[0], y = rot[1], z = rot[2], w = rot[3];
    float v[3] = {in[0] * scale[0], in[1] * scale[1], in[2] * scale[2]};
    float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                  2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                  2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    for (int i = 0; i < 3; ++i) out[i] = R[i * 3] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2] + pos[i];
}

// Where the character's head is, in the world, as posed: the head is the skeleton's bone with the most bones on it
// (the face's: Peter's 6, 11 of them - a hand has its fingers, fewer). The skeleton's parents: [[[[entity +0x1d4]
// +0x80] +0x283c]] a byte a bone (0xff the root); the pose's bone 0 is the entity's origin, its bone n + 1 the
// skeleton's n. Its place: down the chain of local transforms to the origin, then the entity's (rows right / up /
// forward / place).
bool HeadPlace(const unsigned char* ent, const float* rot, const float* pos, const float* scale, int bones, float* out) {
    auto at = [](const void* p, int off) -> const unsigned char* {
        if (!p || IsBadReadPtr(static_cast<const unsigned char*>(p) + off, 4)) return nullptr;
        return *reinterpret_cast<const unsigned char* const*>(static_cast<const unsigned char*>(p) + off);
    };
    const unsigned char* skel = at(at(at(ent, 0x1d4), 0x80), 0x283c);
    const unsigned char* parent = at(skel, 0);
    int n = bones - 1;
    if (!parent || n < 2 || n > 254 || IsBadReadPtr(parent, n)) return false;
    int children[255] = {}, head = -1;
    for (int b = 0; b < n; ++b)
        if (parent[b] != 0xff) {
            if (parent[b] >= b) return false;   // (parents come first - not a parent table)
            ++children[parent[b]];
        }
    for (int b = 0; b < n; ++b) if (head < 0 || children[b] > children[head]) head = b;
    if (children[head] < 6) return false;
    float p[3] = {0, 0, 0};
    for (int b = head + 1; b > 0; b = parent[b - 1] == 0xff ? 0 : parent[b - 1] + 1)
        Apply(rot + b * 4, pos + b * 3, scale + b * 3, p, p);
    Apply(rot, pos, scale, p, p);
    const float* m = reinterpret_cast<const float*>(ent);
    for (int i = 0; i < 3; ++i) out[i] = p[0] * m[i] + p[1] * m[4 + i] + p[2] * m[8 + i] + m[12 + i];
    return true;
}

// First person: their arms in their view (their own view only - a player on another screen or PC sees the game's
// pose). Both arms exactly as the game animates them - holding the gun, firing, reloading, a melee swing - moved
// together, as one piece, so that the chest they hang on sits in front of the eye where the game holds them in each
// stance (relaxed, aiming: measured standing still), placed so the gun hand comes out at the same spot on the screen.
// The run's bounce of the body doesn't reach them; nothing in the arms is bent. Their head is shrunk away meanwhile.
struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return sqrtf(Dot(a, a)); }
V3 Unit(V3 a) { float l = Len(a); return l > 1e-6f ? a * (1 / l) : V3{0, 0, 0}; }

struct M3 { float m[9]; };   // (row-major rotation)
M3 Mul(const M3& a, const M3& b) {
    M3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return r;
}
M3 Transposed(const M3& a) {
    M3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[j * 3 + i];
    return r;
}
V3 Mul(const M3& a, V3 v) {
    return {a.m[0] * v.x + a.m[1] * v.y + a.m[2] * v.z, a.m[3] * v.x + a.m[4] * v.y + a.m[5] * v.z,
            a.m[6] * v.x + a.m[7] * v.y + a.m[8] * v.z};
}
M3 FromQuat(const float* q) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    return {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
             2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
             2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
}
void ToQuat(const M3& a, float* q) {
    const float* m = a.m;
    float t = m[0] + m[4] + m[8];
    if (t > 0) {
        float s = sqrtf(t + 1) * 2;
        q[3] = s / 4; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1 + m[0] - m[4] - m[8]) * 2;
        q[3] = (m[7] - m[5]) / s; q[0] = s / 4; q[1] = (m[1] + m[3]) / s; q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1 + m[4] - m[0] - m[8]) * 2;
        q[3] = (m[2] - m[6]) / s; q[0] = (m[1] + m[3]) / s; q[1] = s / 4; q[2] = (m[5] + m[7]) / s;
    } else {
        float s = sqrtf(1 + m[8] - m[0] - m[4]) * 2;
        q[3] = (m[3] - m[1]) / s; q[0] = (m[2] + m[6]) / s; q[1] = (m[5] + m[7]) / s; q[2] = s / 4;
    }
}
// the rotation taking direction a to direction b (both unit), the shortest way
M3 FromTo(V3 a, V3 b) {
    V3 v = Cross(a, b);
    float c = Dot(a, b);
    if (c < -0.9999f) {   // (opposite: half a turn about any axis square to a)
        V3 axis = Unit(Cross(a, fabsf(a.x) < 0.9f ? V3{1, 0, 0} : V3{0, 1, 0}));
        return {{2 * axis.x * axis.x - 1, 2 * axis.x * axis.y, 2 * axis.x * axis.z,
                 2 * axis.y * axis.x, 2 * axis.y * axis.y - 1, 2 * axis.y * axis.z,
                 2 * axis.z * axis.x, 2 * axis.z * axis.y, 2 * axis.z * axis.z - 1}};
    }
    float k = 1 / (1 + c);
    return {{v.x * v.x * k + c, v.x * v.y * k - v.z, v.x * v.z * k + v.y,
             v.y * v.x * k + v.z, v.y * v.y * k + c, v.y * v.z * k - v.x,
             v.z * v.x * k - v.y, v.z * v.y * k + v.x, v.z * v.z * k + c}};
}

// Where the gun hand comes out in the view, as the game holds it: across and down, of the view's half (the same place
// on the screen at any width / field of view) - zoomed in all the way: in under the crosshair - and
// how far ahead at least, for an eye 1.8 up (research: "fpv arms <right> <down> <ahead> <0|1>", "fpv armszoom <right>
// <down>")
float g_armRight = 0.45f, g_armDown = 0.85f, g_armAhead = 0.9f;
float g_armZoomRight = 0.25f, g_armZoomDown = 1.0f;
bool g_armsOn = true, g_armPlaceTest = false;
// By character (their skeleton's size, and their right hand's bone where that's shared - Meg's and Death's): where
// their gun hand comes out instead, and whether it's the left hand (Chris's bow). Death low and far (his robe's
// sleeves hang wide), Quagmire further right (his shirt's sleeves).
// Dual: a gun in the other hand too (Carter's, Cleveland's) - that hand left as the game holds it, never put on the
// first gun (OffHand).
struct ArmPlace { int bones, hand; float right, down, ahead; bool left, dual; };
const ArmPlace kArmPlaces[] = {{61, 30, 0.5f, 1.5f, 1.6f, false, false},     // Death
                               {66, 47, 0.75f, 0.9f, 1.2f, false, false},    // Quagmire
                               {47, -1, 0.45f, 0.85f, 0.9f, true, false},    // Chris
                               {53, -1, 0.45f, 0.85f, 0.9f, false, true},    // Carter
                               {44, -1, 0.45f, 0.85f, 0.9f, false, true}};   // Cleveland

// a rotation by `deg` degrees about `axis` (unit)
M3 About(V3 axis, float deg) {
    float a = deg * 3.14159265f / 180, c = cosf(a), s = sinf(a), t = 1 - c, x = axis.x, y = axis.y, z = axis.z;
    return {{t * x * x + c, t * x * y - s * z, t * x * z + s * y,
             t * x * y + s * z, t * y * y + c, t * y * z - s * x,
             t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
}

const float kSteady = 0.4f;   // (s: standing steady before a stance's hold is taken)

// a player's arms in first person, kept between frames: their hands and head, and how the game holds the arms in each
// stance (relaxed, aiming) - the chest they hang on, in the view's space, and the gun hand against the chest
struct ArmState {
    const void* ent = nullptr;            // (whose: a new one - respawned, another character - takes it again)
    bool found = false, told = false;     // (their hands and head - found once, FindBody)
    int hands[2] = {-1, -1};              // (the right one, the left one)
    int head = -1;                        // (HeadAway shrinks it)
    bool have[2] = {false, false};        // (each stance's hold: relaxed, aiming)
    float chestQ[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    V3 chestAt[2] = {{0, 0, 0}, {0, 0, 0}};
    V3 hand[2] = {{0, 0, 0}, {0, 0, 0}};
    float stance = 0;                     // (0 relaxed .. 1 aiming - the last known: kept through a jump, a swing)
    double at = 0, steadySince = 0;
    float slow[4] = {0, 0, 0, 1};         // (the chest in the view, slowly - is it steady)
    V3 slowAt = {0, 0, 0};
    std::vector<float> aimRot, aimPos;    // (the arms' bones as the game holds the gun aiming - the pose taken with it,
    float rootQ[2][4];                    //  and where both arms hang on the chest then)
    V3 rootAt[2];
    float support = 0;                    // (0..1: the off hand on the gun - OffHand)
};

// a's turn toward b by t (quaternions, the short way; normalized - near enough for small steps)
void Toward(const float* a, const float* b, float t, float* out) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -1.0f : 1.0f, len = 0;
    for (int c = 0; c < 4; ++c) { out[c] = a[c] + (s * b[c] - a[c]) * t; len += out[c] * out[c]; }
    len = len > 0 ? 1 / sqrtf(len) : 1;
    for (int c = 0; c < 4; ++c) out[c] *= len;
}
ArmState g_armState[4];
// a character's aiming hold (by their skeleton's size), kept for the session: the chest in the view, the gun hand on
// it, the arms' pose
struct AimMemory {
    bool have = false;
    float chestQ[4], rootQ[2][4];
    V3 chestAt, hand, rootAt[2];
    std::vector<float> rot, pos;
};
AimMemory g_aimMemory[256];
// the off hand on a one-handed gun: how far left of the gun hand and under it (for an eye 1.8 up)
const float kGripLeft = 0.09f, kGripDown = 0.05f;

// A character's hands and head, from their skeleton's shape (`at`: the bones' places in the character's space, `eye`
// there too). The hands: a pair of bones each with four or more fingers on it (a cartoon hand has four; a finger a short
// chain of up to three bones), alike - as deep in the skeleton, as many bones on them; the gun's the right one (-x).
// The chest: where their arms meet. The head: of the chest's other bones, the one nearest the eye, then down its neck
// (while there's one bone on it) - Stewie's has only his eyes on it, Peter's his face's eleven. Neither "the bone with
// the most on it" (Meg's hips) nor "near the eye with the most on it" (Stewie's chest) is right on every skeleton.
bool FindBody(const unsigned char* parent, const int* children, int n, const V3* at, V3 eye, int* hands, int* head) {
    int size[255], depth[255];
    for (int b = 0; b < n; ++b) size[b] = 0;
    for (int b = n - 1; b >= 0; --b)
        if (parent[b] != 0xff) size[parent[b]] += size[b] + 1;   // (parents come before their bones)
    for (int b = 0; b < n; ++b) depth[b] = parent[b] == 0xff ? 0 : depth[parent[b]] + 1;
    bool hand[255] = {};
    for (int b = 0; b < n; ++b) {
        if (children[b] < 4) continue;
        bool fingers = true;
        for (int c = b + 1; c < n && fingers; ++c) if (parent[c] == b && size[c] > 2) fingers = false;
        hand[b] = fingers;
    }
    int h0 = -1, h1 = -1;
    for (int i = 0; i < n && h0 < 0; ++i)
        for (int j = i + 1; j < n && h0 < 0; ++j)
            if (hand[i] && hand[j] && depth[i] == depth[j] && size[i] == size[j]) h0 = i, h1 = j;
    if (h0 < 0) return false;
    bool up0[255] = {};
    for (int c = h0; c >= 0; c = parent[c] == 0xff ? -1 : parent[c]) up0[c] = true;
    int chest = h1;
    while (chest >= 0 && !up0[chest]) chest = parent[chest] == 0xff ? -1 : parent[chest];
    if (chest < 0) return false;
    // the right one: the arm that starts on the right of the chest (-x) - not the hand that's further right (holding a
    // rifle, Quagmire's left hand reaches across to its front)
    auto root = [&](int h) { int b = h; while (parent[b] != 0xff && parent[b] != chest) b = parent[b]; return b; };
    if (at[root(h1)].x < at[root(h0)].x) {
        int t = h0; h0 = h1; h1 = t;
        for (int b = 0; b < n; ++b) up0[b] = false;
        for (int c = h0; c >= 0; c = parent[c] == 0xff ? -1 : parent[c]) up0[c] = true;
    }
    int h = -1;
    for (int c = chest + 1; c < n; ++c) {
        if (parent[c] != chest || up0[c]) continue;
        bool arm = false;   // (the other arm)
        for (int d = h1; d >= 0 && !arm; d = parent[d] == 0xff ? -1 : parent[d]) arm = d == c;
        if (!arm && (h < 0 || Len(at[c] - eye) < Len(at[h] - eye))) h = c;
    }
    if (h < 0) return false;
    for (bool down = true; down;) {   // (down the neck)
        down = false;
        if (children[h] != 1) break;
        for (int c = h + 1; c < n; ++c) if (parent[c] == h) { h = c; down = true; break; }
    }
    hands[0] = h0;
    hands[1] = h1;
    *head = h;
    return true;
}

// An arm: its collarbone (on the chest), shoulder, elbow (the bone half way along it) and hand.
struct Arm { int clav = -1, shoulder = -1, elbow = -1, hand = -1; };

// `fingers`: the bone the fingers hang on; `head`: the head (the arm ends at the chain from the head down - the chest).
// The hand: up from the fingers while their parent branches (Stewie's wrist has the palm and more on it) - what the
// gun hangs on comes along.
bool FindArm(const unsigned char* parent, const int* children, int fingers, int head, const V3* at, Arm* arm) {
    bool spine[255] = {};
    for (int b = head; b >= 0; b = parent[b] == 0xff ? -1 : parent[b]) spine[b] = true;
    int hand = fingers;
    while (parent[hand] != 0xff && !spine[parent[hand]] && children[parent[hand]] >= 2) hand = parent[hand];
    int chain[16], len = 0;
    for (int b = hand; b >= 0 && len < 16; b = parent[b] == 0xff ? -1 : parent[b]) {
        chain[len++] = b;
        if (parent[b] == 0xff || spine[parent[b]]) break;   // (its parent is the chest)
    }
    if (len < 3 || len >= 16 || parent[chain[len - 1]] == 0xff) return false;
    // (no collarbone on some - Cleveland's, Chris's: the upper arm straight off the chest)
    int s = len >= 4 ? len - 2 : len - 1;
    arm->hand = hand;
    arm->clav = len >= 4 ? chain[len - 1] : -1;
    arm->shoulder = chain[s];
    float total = 0, half = 0;   // (the elbow: the bone nearest half the arm's length from the shoulder)
    for (int i = s; i > 0; --i) total += Len(at[chain[i - 1]] - at[chain[i]]);
    arm->elbow = chain[s - 1];
    float best = 1e9f;
    for (int i = s; i > 0; --i) {
        half += Len(at[chain[i - 1]] - at[chain[i]]);
        if (fabsf(half - total / 2) < best) { best = fabsf(half - total / 2); arm->elbow = chain[i - 1]; }
    }
    return arm->elbow != arm->hand && arm->elbow != arm->shoulder;
}

// The bones' rotations and places in the character's space again, from the pose (after it was changed).
void Posed(const unsigned char* parent, int n, const float* rot, const float* pos, M3* R, V3* at) {
    M3 root = FromQuat(rot);
    V3 rootAt = {pos[0], pos[1], pos[2]};
    for (int b = 0; b < n; ++b) {
        M3 l = FromQuat(rot + (b + 1) * 4);
        V3 t = {pos[(b + 1) * 3], pos[(b + 1) * 3 + 1], pos[(b + 1) * 3 + 2]};
        const M3& pr = parent[b] == 0xff ? root : R[parent[b]];
        V3 pa = parent[b] == 0xff ? rootAt : at[parent[b]];
        R[b] = Mul(pr, l);
        at[b] = pa + Mul(pr, t);
    }
}

// Bends an arm (shoulder, elbow; its collarbone forward if it can't reach) so its hand reaches `target` turned
// `handRot` (the character's space); `pole`: the way its elbow points.
void Reach(const unsigned char* parent, int n, const Arm& arm, V3 target, const M3& handRot, V3 pole, float* rot,
           float* pos, M3* R, V3* at) {
    auto local = [&](int b, const M3& model) {
        ToQuat(parent[b] == 0xff ? model : Mul(Transposed(R[parent[b]]), model), rot + (b + 1) * 4);
        Posed(parent, n, rot, pos, R, at);
    };
    V3 S = at[arm.shoulder];
    float L1 = Len(at[arm.elbow] - S), L2 = Len(at[arm.hand] - at[arm.elbow]), D = Len(target - S);
    if (L1 < 1e-3f || L2 < 1e-3f || D < 1e-3f) return;
    if (D > (L1 + L2) * 0.98f) {   // (out of reach: the arm's root forward by what's missing)
        int c = arm.clav >= 0 ? arm.clav : arm.shoulder, p = parent[c];
        V3 l = Mul(Transposed(R[p]), Unit(target - S) * (D - (L1 + L2) * 0.98f));
        pos[(c + 1) * 3] += l.x; pos[(c + 1) * 3 + 1] += l.y; pos[(c + 1) * 3 + 2] += l.z;
        Posed(parent, n, rot, pos, R, at);
        S = at[arm.shoulder];
        D = Len(target - S);
    }
    V3 x = Unit(target - S), y = Unit(pole - x * Dot(pole, x));
    float ca = (L1 * L1 + D * D - L2 * L2) / (2 * L1 * D);
    ca = ca < -1 ? -1 : (ca > 1 ? 1 : ca);
    V3 E = S + (x * ca + y * sqrtf(1 - ca * ca)) * L1;
    local(arm.shoulder, Mul(FromTo(Unit(at[arm.elbow] - S), Unit(E - S)), R[arm.shoulder]));
    local(arm.elbow, Mul(FromTo(Unit(at[arm.hand] - at[arm.elbow]), Unit(target - at[arm.elbow])), R[arm.elbow]));
    local(arm.hand, handRot);
}

// The off hand of a one-handed hold - Meg's, Brian's pistol, which the game keeps down at their side, out of the view:
// on the gun with the gun hand, its arm bent to reach. While the game's own moves bring it into the view (a reload),
// those. Not for an off hand in the view anyway (Carter's and Cleveland's second gun, Death's sleeve, a rifle's front
// grip).
void OffHand(ArmState& st, const ArmsView& view, const unsigned char* parent, int n, const Arm& a, const Arm& a2,
             float k, V3 eye, const M3& V, float held, float dt, float* rot, float* pos, M3* R, V3* at) {
    int bones = n + 1;
    Posed(parent, n, rot, pos, R, at);
    // in the view? (its place in the view's space: x left, y up, z ahead)
    V3 v = Mul(Transposed(V), at[a2.hand] - eye);
    bool inView = v.z > 0.05f * k && fabsf(v.x) < v.z * view.tans[0] && v.y > -v.z * view.tans[1];
    float want = inView ? 0.0f : 1.0f, step = dt > 0 && dt < 0.5f ? dt / 0.15f : 0;
    (void)held;
    st.support = want > st.support ? (st.support + step < want ? st.support + step : want)
                                   : (st.support - step > want ? st.support - step : want);
    if (st.support <= 0) return;
    std::vector<float> before(rot, rot + bones * 4), beforePos(pos, pos + bones * 3);
    // the gun hand mirrored across the view's up-and-ahead plane (the skeleton's hands mirror each other), just left of
    // it and under it: the two clasped round the grip, palms together (the game never brings it nearer than an arm's
    // length - nothing of its own to take)
    V3 target = at[a.hand] + Mul(V, V3{kGripLeft * k, -kGripDown * k, 0});
    M3 X = {{-1, 0, 0, 0, 1, 0, 0, 0, 1}};
    M3 Mirror = Mul(Mul(V, X), Transposed(V));
    M3 handRot = Mul(Mul(Mirror, R[a.hand]), Mirror);
    V3 pole = Mul(V, V3{1.0f, -1.0f, 0});   // (its elbow out to the left and down)
    Reach(parent, n, a2, target, handRot, pole, rot, pos, R, at);
    float s = st.support * st.support * (3 - 2 * st.support);
    for (int b = 0; b < bones; ++b) {
        Toward(&before[b * 4], rot + b * 4, s, rot + b * 4);
        for (int c = 0; c < 3; ++c) pos[b * 3 + c] = beforePos[b * 3 + c] + (pos[b * 3 + c] - beforePos[b * 3 + c]) * s;
    }
}

bool ArmsInView(int slot, const unsigned char* ent, float* rot, float* pos, float* scale, int bones) {
    if (slot < 0 || slot >= 4) return false;
    ArmState& st = g_armState[slot];
    if (st.ent != ent) st = ArmState(), st.ent = ent;
    ArmsView view;
    if (!g_armsOn || !FirstPersonArms(slot, &view) || view.weight <= 0) {
        st.at = 0;   // (the steadiness measured afresh next time; the holds kept)
        return false;
    }
    float w = view.weight;
    auto at = [](const void* p, int off) -> const unsigned char* {
        if (!p || IsBadReadPtr(static_cast<const unsigned char*>(p) + off, 4)) return nullptr;
        return *reinterpret_cast<const unsigned char* const*>(static_cast<const unsigned char*>(p) + off);
    };
    const unsigned char* parent = at(at(at(at(ent, 0x1d4), 0x80), 0x283c), 0);
    int n = bones - 1;
    if (!parent || n < 8 || n > 254 || IsBadReadPtr(parent, n)) return false;
    int children[255] = {};
    for (int b = 0; b < n; ++b)
        if (parent[b] != 0xff) {
            if (parent[b] >= b) return false;
            ++children[parent[b]];
        }
    // the bones in the character's space (the pose's bone 0, the entity's origin, left out: it's where the
    // character's space is put in the world - with the entity's transform below)
    std::vector<M3> R(n);
    std::vector<V3> place(n);
    M3 root = FromQuat(rot);
    V3 rootAt = {pos[0], pos[1], pos[2]};
    for (int b = 0; b < n; ++b) {
        M3 l = FromQuat(rot + (b + 1) * 4);
        V3 t = {pos[(b + 1) * 3], pos[(b + 1) * 3 + 1], pos[(b + 1) * 3 + 2]};
        const M3& pr = parent[b] == 0xff ? root : R[parent[b]];
        V3 pa = parent[b] == 0xff ? rootAt : place[parent[b]];
        R[b] = Mul(pr, l);
        place[b] = pa + Mul(pr, t);
    }
    // the view in the character's space (the entity: rows right / up / forward / place)
    const float* m = reinterpret_cast<const float*>(ent);
    V3 ax[3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}}, org = {m[12], m[13], m[14]};
    V3 e = V3{view.eye[0], view.eye[1], view.eye[2]} - org, f = {view.forward[0], view.forward[1], view.forward[2]};
    V3 eye = {Dot(e, ax[0]), Dot(e, ax[1]), Dot(e, ax[2])}, F = Unit({Dot(f, ax[0]), Dot(f, ax[1]), Dot(f, ax[2])});
    V3 Rt = Unit(Cross(F, {0, 1, 0})), U = Cross(Rt, F);   // (the view's right and up; the character's right is -x)
    float k = eye.y / 1.8f;   // (the character's size)
    if (!st.found) {
        st.found = FindBody(parent, children, n, place.data(), eye, st.hands, &st.head);
        if (!st.found) {
            if (!st.told) Log(L"Motion: player %d (%d bones): no hands found - the game's arms", slot, bones);
            st.told = true;
            return false;
        }
    }
    // the right hand: the one further right (-x) as the game holds them - looked at again until a hold is taken (found
    // as they came in, mid-move, Quagmire's rifle hand came out the other one)
    if (!st.have[0] && !st.have[1] && place[st.hands[1]].x < place[st.hands[0]].x) {
        int t = st.hands[0]; st.hands[0] = st.hands[1]; st.hands[1] = t;
    }
    const ArmPlace* place0 = nullptr;
    for (const ArmPlace& c : kArmPlaces)
        if (c.bones == bones && (c.hand < 0 || c.hand == st.hands[0])) place0 = &c;
    bool left = place0 && place0->left;
    int gun = st.hands[left ? 1 : 0], other = st.hands[left ? 0 : 1];
    Arm a, a2;
    if (!FindArm(parent, children, gun, st.head, place.data(), &a) ||
        !FindArm(parent, children, other, st.head, place.data(), &a2)) {
        if (!st.told) Log(L"Motion: player %d (%d bones): no arms found (hands %d, %d, head %d) - the game's arms", slot,
                          bones, gun, other, st.head);
        st.told = true;
        return false;
    }
    int roots[2] = {a.clav >= 0 ? a.clav : a.shoulder, a2.clav >= 0 ? a2.clav : a2.shoulder};
    int chest = parent[roots[0]];
    // the chest in the view's space (V: the view's axes in the character's space - left, up, ahead: a turn; right, up,
    // ahead would be a mirror), and the gun hand against the chest
    M3 V = {{-Rt.x, U.x, F.x, -Rt.y, U.y, F.y, -Rt.z, U.z, F.z}};
    M3 Vt = Transposed(V);
    float cq[4];
    ToQuat(Mul(Vt, R[chest]), cq);
    V3 cAt = Mul(Vt, place[chest] - eye);
    V3 cHand = Mul(Transposed(R[chest]), place[a.hand] - place[chest]);
    // a stance's hold, taken standing steady in it, looking about level (the chest still in the view for a while)
    double now = Now();
    float dt = (float)(now - st.at);
    if (st.at == 0 || dt > 0.5f) {
        for (int c = 0; c < 4; ++c) st.slow[c] = cq[c];
        st.slowAt = cAt;
    } else {
        float t = 1 - expf(-dt * 6);
        Toward(st.slow, cq, t, st.slow);
        st.slowAt = st.slowAt + (cAt - st.slowAt) * t;
    }
    st.at = now;
    float d = fabsf(cq[0] * st.slow[0] + cq[1] * st.slow[1] + cq[2] * st.slow[2] + cq[3] * st.slow[3]);
    float ang = 2 * acosf(d > 1 ? 1 : d) * 57.2958f;
    if (!view.still || fabsf(F.y) > 0.2f || Len(cAt - st.slowAt) > 0.01f * k || ang > 2) st.steadySince = now;
    bool steady = st.steadySince > 0 && now - st.steadySince > kSteady;
    for (int s = 0; s < 2; ++s)
        if (steady && (s ? view.aim : view.rest) > 0.98f) {
            if (!st.have[s])
                Log(L"Motion: player %d (%d bones): %s hold taken (hands %d, %d, chest %d, head %d)", slot, bones,
                    s ? L"aiming" : L"relaxed", gun, other, chest, st.head);
            for (int c = 0; c < 4; ++c) st.chestQ[s][c] = cq[c];
            st.chestAt[s] = cAt;
            st.hand[s] = cHand;
            st.have[s] = true;
            if (s) {
                st.aimRot.assign(rot, rot + bones * 4);
                st.aimPos.assign(pos, pos + bones * 3);
                for (int i = 0; i < 2; ++i) {
                    ToQuat(Mul(Transposed(R[chest]), R[roots[i]]), st.rootQ[i]);
                    st.rootAt[i] = Mul(Transposed(R[chest]), place[roots[i]] - place[chest]);
                }
            }
        }
    // (a character's aiming hold, once seen this session, kept for them - a respawn, the next match)
    AimMemory& mem = g_aimMemory[bones];
    if (st.have[1] && !st.aimRot.empty()) {
        if (!mem.have) {
            mem.have = true;
            memcpy(mem.chestQ, st.chestQ[1], sizeof(mem.chestQ));
            memcpy(mem.rootQ, st.rootQ, sizeof(mem.rootQ));
            mem.chestAt = st.chestAt[1];
            mem.hand = st.hand[1];
            mem.rootAt[0] = st.rootAt[0];
            mem.rootAt[1] = st.rootAt[1];
            mem.rot = st.aimRot;
            mem.pos = st.aimPos;
        }
    } else if (mem.have) {
        memcpy(st.chestQ[1], mem.chestQ, sizeof(mem.chestQ));
        memcpy(st.rootQ, mem.rootQ, sizeof(mem.rootQ));
        st.chestAt[1] = mem.chestAt;
        st.hand[1] = mem.hand;
        st.rootAt[0] = mem.rootAt[0];
        st.rootAt[1] = mem.rootAt[1];
        st.aimRot = mem.rot;
        st.aimPos = mem.pos;
        st.have[1] = true;
    }
    if (!st.have[1] || st.aimRot.empty()) return false;   // (until then, the game's arms - their relaxed carry, about
                                                          //  their head or hip, out of the view or across it)
    // In the relaxed stance - standing about, running - the game's relaxed carry (Peter's drill up beside his head,
    // a pistol at the hip) is out of or across the view: their arms hold the gun as the game does aiming instead,
    // ready, steady. Firing, zooming, reloading (the aiming stance), a melee swing, a jump, a power-up's throw (neither
    // stance): the game's own moves. Between, eased as the game eases its stances (by the relaxed stance's weight).
    std::vector<float> game(rot, rot + bones * 4), gamePos(pos, pos + bones * 3);   // (the game's, for easing in / out)
    bool armBone[255] = {};
    for (int b = 0; b < n; ++b)
        for (int c = b; c >= 0; c = parent[c] == 0xff ? -1 : parent[c])
            if (c == roots[0] || c == roots[1]) { armBone[b] = c != b; break; }
    // (on the move - running, a jump - the game plays its own arm swing over both stances: held so too)
    float held = st.aimRot.size() == (size_t)bones * 4 ? (view.still ? view.rest : 1 - view.aim) : 0;
    if (held > 0) {
        for (int b = 0; b < n; ++b) {
            if (!armBone[b]) continue;
            float* q = rot + (b + 1) * 4;
            Toward(q, &st.aimRot[(b + 1) * 4], held, q);
            for (int c = 0; c < 3; ++c) pos[(b + 1) * 3 + c] += (st.aimPos[(b + 1) * 3 + c] - pos[(b + 1) * 3 + c]) * held;
        }
    }
    // the hold for the stance they're in: aiming once it's been seen (the arms are held so, or doing the game's
    // moves from it); until then the relaxed one, between the two as the game eases from one to the other
    if (view.aim + view.rest > 0.05f) st.stance = view.aim / (view.aim + view.rest);
    float s = st.have[1] && !st.aimRot.empty() ? 1.0f : st.stance;
    int i0 = st.have[0] ? 0 : 1, i1 = st.have[1] ? 1 : 0;
    float hq[4];
    Toward(st.chestQ[i0], st.chestQ[i1], s, hq);
    M3 H = FromQuat(hq);
    V3 hAt = st.chestAt[i0] + (st.chestAt[i1] - st.chestAt[i0]) * s;
    V3 hHand = st.hand[i0] + (st.hand[i1] - st.hand[i0]) * s;
    // where that puts the gun hand in the view, and where it should come out: the whole hold moved by the difference
    // (zoomed in, further ahead too - as big on the screen)
    V3 hand = hAt + Mul(H, hHand);
    float z = view.zoom, side = left ? -1.0f : 1.0f;
    bool mine = place0 && !g_armPlaceTest;
    float right0 = mine ? place0->right : g_armRight, down0 = mine ? place0->down : g_armDown;
    float ahead0 = mine ? place0->ahead : g_armAhead;
    float zoomDown = g_armZoomDown > down0 ? g_armZoomDown : down0;   // (Death's sleeves stay low zoomed in too)
    float right = right0 + (g_armZoomRight - right0) * z, down = down0 + (zoomDown - down0) * z;
    float depth = (hand.z > ahead0 * k ? hand.z : ahead0 * k) / view.wide;
    V3 want = {-side * right * view.tans[0] * depth, -down * view.tans[1] * depth, depth};   // (x: to the left)
    M3 chestNew = Mul(V, H);
    V3 chestNewAt = eye + Mul(V, hAt + want - hand);
    // both arms hung on the chest there - where the game hangs them now, or (held) as it does aiming - all that's on
    // them comes along
    M3 Ct = Transposed(R[chest]);
    for (int i = 0; i < 2; ++i) {
        int r = roots[i], p = parent[r];
        float lq[4];
        ToQuat(Mul(Ct, R[r]), lq);
        V3 lAt = Mul(Ct, place[r] - place[chest]);
        if (held > 0) {
            Toward(lq, st.rootQ[i], held, lq);
            lAt = lAt + (st.rootAt[i] - lAt) * held;
        }
        M3 newR = Mul(chestNew, FromQuat(lq));
        V3 newAt = chestNewAt + Mul(chestNew, lAt);
        M3 Pt = Transposed(R[p]);
        ToQuat(Mul(Pt, newR), rot + (r + 1) * 4);
        V3 l = Mul(Pt, newAt - place[p]);
        pos[(r + 1) * 3] = l.x;
        pos[(r + 1) * 3 + 1] = l.y;
        pos[(r + 1) * 3 + 2] = l.z;
    }
    if (!place0 || !place0->dual) OffHand(st, view, parent, n, a, a2, k, eye, V, held, dt, rot, pos, R.data(), place.data());
    if (w < 1) {   // (easing in and out: from the game's pose toward ours by w)
        for (int b = 0; b < bones; ++b) {
            float* q = rot + b * 4;
            const float* g = &game[b * 4];
            float dq = q[0] * g[0] + q[1] * g[1] + q[2] * g[2] + q[3] * g[3], sg = dq < 0 ? -1.0f : 1.0f, len = 0;
            for (int c = 0; c < 4; ++c) { q[c] = g[c] + (sg * q[c] - g[c]) * w; len += q[c] * q[c]; }
            len = len > 0 ? 1 / sqrtf(len) : 1;
            for (int c = 0; c < 4; ++c) q[c] *= len;
            for (int c = 0; c < 3; ++c) pos[b * 3 + c] = gamePos[b * 3 + c] + (pos[b * 3 + c] - gamePos[b * 3 + c]) * w;
        }
    }
    (void)scale;
    return true;
}

// In their own view in first person their head (and what's on it: a hat, the face's bones) is shrunk to nothing -
// their gun goes where the clip under the head was, and the head never comes back between the eye and the view (a
// gun's kick letting their arm go, the head and hat came up round the eye: the view went black). A single skinning
// for every view: a partner on the same screen sees them without it meanwhile. (true: shrunk - firstperson.cpp asks)
bool HeadAway(int slot, const unsigned char* ent, float* scale, int bones) {
    ArmsView view;
    if (slot < 0 || slot >= 4 || !FirstPersonArms(slot, &view) || view.weight < 0.5f) return false;
    // (the head ArmsInView found - it runs first)
    const ArmState& st = g_armState[slot];
    if (st.ent != ent || st.head < 0 || st.head >= bones - 1) return false;
    scale[(st.head + 1) * 3] = scale[(st.head + 1) * 3 + 1] = scale[(st.head + 1) * 3 + 2] = 0;
    return true;
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
        ArmsInView(static_cast<int>(&tr - g_tracks), ent, rot, pos, scale, bones);
        tr.armsInView = HeadAway(static_cast<int>(&tr - g_tracks), ent, scale, bones);
        tr.last.resize(bones * kFloats);
        Gather(rot, pos, scale, bones, tr.last.data());
        tr.lastBones = bones;
        tr.haveHead = FirstPersonAny() && HeadPlace(ent, rot, pos, scale, bones, tr.head);
    }
    LeaveCriticalSection(&g_lock);
    g_poseOriginal(ent, rot, pos, scale, bones);
    for (int s = 0; s < 4; ++s)   // (research: where its bone matrices went - pipe "posepal <slot>")
        if (g_palArm[s] && g_tracks[s].ent == ent) {
            g_palArm[s] = false;
            const DWORD* at = reinterpret_cast<const DWORD*>(ent + 0x1d8);
            Log(L"Motion: player %d's entity %p +0x1d8: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", s, ent, at[0], at[1],
                at[2], at[3], at[4], at[5], at[6], at[7]);
            for (int k = 0; k < 8; ++k) {
                const float* m = reinterpret_cast<const float*>(at[k]);
                if (at[k] < 0x10000 || IsBadReadPtr(m, 96)) continue;
                Log(L"Motion:   [+%#x] %p: bone 0 %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f", 0x1d8 + k * 4, m,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            }
        }
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
// models.cpp: a player's pose this frame (a bone: rotation x y z w, translation, scale - local), false: none yet
bool MotionPose(int slot, std::vector<float>& out, int* bones) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return false;
    EnterCriticalSection(&g_lock);
    bool have = g_tracks[slot].lastBones > 0 && g_tracks[slot].ent;
    if (have) { out = g_tracks[slot].last; *bones = g_tracks[slot].lastBones; }
    LeaveCriticalSection(&g_lock);
    return have;
}

void MotionArmsTest(float right, float down, float ahead, bool on) {
    g_armRight = right;
    g_armDown = down;
    g_armAhead = ahead;
    g_armsOn = on;
    g_armPlaceTest = true;
    Log(L"Motion: test - gun hand %.2f right, %.2f down, %.2f ahead of the eye (%s)", right, down, ahead, on ? L"on" : L"off");
}
void MotionArmsZoomTest(float right, float down) {
    g_armZoomRight = right;
    g_armZoomDown = down;
    Log(L"Motion: test - gun hand zoomed in %.2f right, %.2f down", right, down);
}

// firstperson.cpp: a player's gun was brought into their view (their head shrunk away) this frame
bool MotionArmsInView(int slot) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return false;
    EnterCriticalSection(&g_lock);
    bool on = g_tracks[slot].armsInView && g_tracks[slot].ent;
    LeaveCriticalSection(&g_lock);
    return on;
}

// firstperson.cpp: where a player's head is this frame (the world), false: not known
bool MotionHead(int slot, float* at) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return false;
    EnterCriticalSection(&g_lock);
    bool have = g_tracks[slot].haveHead && g_tracks[slot].ent;
    if (have) memcpy(at, g_tracks[slot].head, sizeof(g_tracks[slot].head));
    LeaveCriticalSection(&g_lock);
    return have;
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
    if (_stricmp(cmd, "posepal") == 0 && forSlot) g_palArm[slot] = true;
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
