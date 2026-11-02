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

// First person: their gun brought into their view (their own view only - a player on another screen or PC sees the
// game's pose). The gun arm's shoulder is held below and right of the eye and the arm bent so the hand sits low and to
// the right of the view (zoomed in: toward under the crosshair), turned as it was standing (the gun held level); onto
// that, what the game's hand does beyond the stance it's in - firing's kick, a reload - capped. While the game's arm
// does something else (a melee swing, a throw, a jump) the gun is held steady. Their head is shrunk away meanwhile.
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

// the gun hand's place from the eye, for a character whose eye is 1.8 up (research: "fpv arms <right> <down> <ahead>
// <0|1>")
float g_armRight = 0.62f, g_armDown = 1.0f, g_armAhead = 1.2f;   // (across and down: of the view's half - the same
                                                                 //  place on the screen at any width / field of view)
float g_armZoomRight = 0.25f, g_armZoomDown = 0.9f;   // (zoomed all the way: in under the crosshair - "fpv armszoom")
bool g_armsOn = true, g_armPlaceTest = false;
// By character - their skeleton's size - for the gun they start with: the hand turned after (degrees about the view's
// right, up, ahead - its gun pointing ahead, a little in toward the middle; Peter's drill held up, a pistol low) and how
// far down the view it sits (as g_armDown); whether the weapon is in their left hand. The others: a pistol's. (Meg's
// skeleton is Death's too.) The turn is a guess until the game's own aim is seen (ArmsInView).
struct ArmFit { int bones; float turn[3]; float down; bool left; };
const ArmFit kArmFits[] = {{59, {-20, 30, 0}, 1.0f, false},    // Peter (his drill's side in view, its bit toward the middle)
                           {71, {-70, 10, 0}, 1.0f, false},    // Brian
                           {61, {-20, 0, 0}, 0.75f, false},    // Meg
                           {47, {-70, -10, 0}, 1.0f, true}};   // Chris (his bow)
const ArmFit kArmFitElse = {0, {-70, 10, 0}, 1.0f, false};
const float kInward = 15;   // (the gun's turn from the game's aim: in toward the middle, degrees)
float g_armTurn[3] = {0, 0, 0};   // (research, "fpv armsturn": this for everyone)
bool g_armTurnTest = false;

// a rotation by `deg` degrees about `axis` (unit)
M3 About(V3 axis, float deg) {
    float a = deg * 3.14159265f / 180, c = cosf(a), s = sinf(a), t = 1 - c, x = axis.x, y = axis.y, z = axis.z;
    return {{t * x * x + c, t * x * y - s * z, t * x * z + s * y,
             t * x * y + s * z, t * y * y + c, t * y * z - s * x,
             t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
}

// the game's hand movement let through at most, turned (degrees) and moved (for an eye 1.8 up): standing, what it does
// beyond the stance it's in (a shot's kick, a reload); on the move, only its quick part (the run's swing mostly kept out)
const float kActTurn = 30, kActMove = 0.12f, kMoveTurn = 8, kMoveMove = 0.03f;

// a rotation no further than `deg` degrees (the same axis)
M3 Limited(const M3& r, float deg) {
    float q[4];
    ToQuat(r, q);
    if (q[3] < 0) for (float& c : q) c = -c;
    float half = acosf(q[3] > 1 ? 1 : q[3]), most = deg * 3.14159265f / 360;
    if (half <= most) return r;
    float s = sinf(half), k = sinf(most) / (s > 1e-6f ? s : 1);
    float l[4] = {q[0] * k, q[1] * k, q[2] * k, cosf(most)};
    return FromQuat(l);
}

const float kSteady = 0.4f;   // (s: the hand steady before a hold is taken)
const float kMoving = 0.2f;   // (s: from standing's measure of the hand's movement to moving's)

// a player's gun hand in first person, kept between frames: its slow average against the upper arm (turn, place), the
// game's hold in its two stances (relaxed, aiming - taken standing steady in each), and its turn in the view
struct ArmState {
    const void* ent = nullptr;            // (whose: a new one - respawned, another character - takes it again)
    bool have = false, haveTurn = false, aimTurn = false, told = false;
    bool found = false;                   // (their hands and head - found once, FindBody)
    int hands[2] = {-1, -1};              // (the gun's - the right one - and the other)
    int head = -1;                        // (HeadAway shrinks it)
    int twoHands = -1;                    // (the other hand on the gun too: 1 / 0, -1 not known yet)
    double at = 0;
    float slow[4] = {0, 0, 0, 1};
    V3 slowAt = {0, 0, 0};
    M3 turn = {};
    bool haveHold[2] = {false, false};    // (the hand against the upper arm as the gun is held: relaxed, aiming)
    float holdQ[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    V3 holdAt[2] = {{0, 0, 0}, {0, 0, 0}};
    float moving = 0;                     // (0 standing .. 1 on the move)
    float inward = 0;                     // (the gun's turn in toward the middle, degrees - undone zooming in)
    double steadySince = 0;               // (since when the hand is steady, standing - a hold taken only then)
};

// a's turn toward b by t (quaternions, the short way; normalized - near enough for small steps)
void Toward(const float* a, const float* b, float t, float* out) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -1.0f : 1.0f, len = 0;
    for (int c = 0; c < 4; ++c) { out[c] = a[c] + (s * b[c] - a[c]) * t; len += out[c] * out[c]; }
    len = len > 0 ? 1 / sqrtf(len) : 1;
    for (int c = 0; c < 4; ++c) out[c] *= len;
}
ArmState g_armState[4];

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

// Bends an arm (shoulder, elbow) so its hand reaches `target`, its rotation `handRot` (the character's space); `pole`
// the way its elbow points. R / at: each bone's rotation and place in the character's space (updated down the arm).
void ReachArm(const unsigned char* parent, int n, const Arm& arm, V3 target, const M3& handRot, V3 pole, float* rot,
              float* pos, M3* R, V3* at, const V3* shoulderAt = nullptr) {
    auto local = [&](int b, const M3& model) {   // a bone's new rotation in the character's space -> its local one
        M3 l = parent[b] == 0xff ? model : Mul(Transposed(R[parent[b]]), model);
        ToQuat(l, rot + (b + 1) * 4);
    };
    auto redo = [&](int from) {   // the bones down from `from` (after it) again from their parents
        for (int b = from + 1; b < n; ++b) {
            if (parent[b] == 0xff) continue;
            int p = parent[b];
            M3 l = FromQuat(rot + (b + 1) * 4);
            V3 t = {pos[(b + 1) * 3], pos[(b + 1) * 3 + 1], pos[(b + 1) * 3 + 2]};
            R[b] = Mul(R[p], l);
            at[b] = at[p] + Mul(R[p], t);
        }
    };
    const int c = arm.clav >= 0 ? arm.clav : arm.shoulder;   // (what's moved to move the shoulder: the collarbone)
    if (shoulderAt) {   // (the shoulder held there: the collarbone moved - the walk lifts and swings it)
        int p = parent[c];
        V3 l = Mul(Transposed(R[p]), *shoulderAt - at[arm.shoulder]);
        pos[(c + 1) * 3] += l.x; pos[(c + 1) * 3 + 1] += l.y; pos[(c + 1) * 3 + 2] += l.z;
        redo(p);
    }
    V3 S = at[arm.shoulder];
    float L1 = Len(at[arm.elbow] - S), L2 = Len(at[arm.hand] - at[arm.elbow]), D = Len(target - S);
    if (L1 < 1e-3f || L2 < 1e-3f) return;
    if (D > (L1 + L2) * 0.98f) {   // (out of reach: the collarbone forward by what's missing)
        V3 shift = Unit(target - S) * (D - (L1 + L2) * 0.98f);
        int p = parent[c];
        V3 l = Mul(Transposed(R[p]), shift);
        pos[(c + 1) * 3] += l.x; pos[(c + 1) * 3 + 1] += l.y; pos[(c + 1) * 3 + 2] += l.z;
        redo(p);
        S = at[arm.shoulder];
        D = Len(target - S);
    }
    V3 x = Unit(target - S);
    V3 y = Unit(pole - x * Dot(pole, x));
    float ca = (L1 * L1 + D * D - L2 * L2) / (2 * L1 * D);
    ca = ca < -1 ? -1 : (ca > 1 ? 1 : ca);
    V3 E = S + (x * ca + y * sqrtf(1 - ca * ca)) * L1;
    M3 turn = FromTo(Unit(at[arm.elbow] - S), Unit(E - S));   // the upper arm toward the new elbow
    local(arm.shoulder, Mul(turn, R[arm.shoulder]));
    redo(parent[arm.shoulder]);
    turn = FromTo(Unit(at[arm.hand] - at[arm.elbow]), Unit(target - at[arm.elbow]));   // the forearm to the hand's place
    local(arm.elbow, Mul(turn, R[arm.elbow]));
    redo(parent[arm.elbow]);
    local(arm.hand, handRot);
    redo(parent[arm.hand]);
}

bool ArmsInView(int slot, const unsigned char* ent, float* rot, float* pos, float* scale, int bones) {
    if (slot < 0 || slot >= 4) return false;
    ArmState& st = g_armState[slot];
    if (st.ent != ent) st = ArmState(), st.ent = ent;
    ArmsView view;
    if (!g_armsOn || !FirstPersonArms(slot, &view) || view.weight <= 0) {
        st.have = false;   // (the slow average afresh next time; the gun's turn and the holds kept)
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
    // the gun's hand: the one further right (-x) as the game holds the gun - looked at again until the hold is taken
    // (standing relaxed): found as they came in, mid-move, Quagmire's rifle hand came out the other one
    if (!st.haveTurn && place[st.hands[1]].x < place[st.hands[0]].x) {
        int t = st.hands[0]; st.hands[0] = st.hands[1]; st.hands[1] = t;
    }
    const ArmFit* fit = &kArmFitElse;
    for (const ArmFit& c : kArmFits) if (c.bones == bones) fit = &c;
    // (a weapon in the left hand - Chris's bow: that one brought into the view, at its bottom left - everything to the
    // right below on the left instead, Rs)
    int head = st.head, gun = st.hands[fit->left ? 1 : 0], other = st.hands[fit->left ? 0 : 1];
    float side = fit->left ? -1.0f : 1.0f;
    V3 Rs = Rt * side;
    Arm a, a2;
    if (gun < 0 || !FindArm(parent, children, gun, head, place.data(), &a)) {
        if (!st.told) Log(L"Motion: player %d (%d bones): no gun arm found (hand bone %d, head %d) - the game's arms", slot,
                          bones, gun, head);
        st.told = true;
        return false;
    }
    // the hand at the bottom right of the view - zooming in, toward under the crosshair (the game eases its zoom in),
    // and further ahead as the view narrows: the gun as big on the screen (as near, it filled a third of it)
    float ahead = g_armAhead * k / view.wide, z = view.zoom;
    float down0 = g_armPlaceTest ? g_armDown : fit->down;
    float right = g_armRight + (g_armZoomRight - g_armRight) * z, down = down0 + (g_armZoomDown - down0) * z;
    V3 target = eye + Rs * (right * view.tans[0] * ahead) - U * (down * view.tans[1] * ahead) + F * ahead;
    // The hand's turn in the view (B) is held, as a shooter's gun is: the game's hand, its forearm turned to look
    // ahead (then the character's turn), taken once standing steady in the relaxed stance. Onto it: what the game's
    // hand does beyond how it holds the gun - a shot's kick, a reload (below).
    M3 V = {{Rt.x, U.x, F.x, Rt.y, U.y, F.y, Rt.z, U.z, F.z}};   // (the view's axes in the character's space)
    M3 Q = Mul(Transposed(R[a.shoulder]), R[a.hand]);
    V3 P = Mul(Transposed(R[a.shoulder]), place[a.hand] - place[a.shoulder]);
    double now = Now();
    if (!st.have || now - st.at > 0.5) {
        st.have = true;
        ToQuat(Q, st.slow);
        st.slowAt = P;
    } else {
        float t = 1 - expf(-(float)(now - st.at) * 6);
        float q[4], d, len = 0;
        ToQuat(Q, q);
        d = q[0] * st.slow[0] + q[1] * st.slow[1] + q[2] * st.slow[2] + q[3] * st.slow[3];
        for (int c = 0; c < 4; ++c) { st.slow[c] += ((d < 0 ? -q[c] : q[c]) - st.slow[c]) * t; len += st.slow[c] * st.slow[c]; }
        for (int c = 0; c < 4; ++c) st.slow[c] /= sqrtf(len);
        st.slowAt = st.slowAt + (P - st.slowAt) * t;
    }
    double since = now - st.at;
    st.at = now;
    // the hand steady standing still (no walk swing, no flinch, no shot's kick) - the hold is taken only after a while
    // of that: taken mid-flinch, the drill pointed at the sky
    float steadyAng;
    {
        float q[4];
        ToQuat(Q, q);
        float d = fabsf(q[0] * st.slow[0] + q[1] * st.slow[1] + q[2] * st.slow[2] + q[3] * st.slow[3]);
        steadyAng = 2 * acosf(d > 1 ? 1 : d) * 57.2958f;
    }
    if (!view.still || Len(P - st.slowAt) > 0.015f * k || steadyAng > 3 || st.steadySince == 0) st.steadySince = now;
    bool steady = now - st.steadySince > kSteady;
    // the game's hold in each stance (the aiming one: as it fires, zooms, reloads), taken standing steady in it - kept
    // up while that lasts
    for (int s = 0; s < 2; ++s)
        if (steady && (s ? view.aim : view.rest) > 0.98f) {
            ToQuat(Q, st.holdQ[s]);
            st.holdAt[s] = P;
            st.haveHold[s] = true;
        }
    // The gun's turn from the game's own aim, once it's been seen held steady: aiming, the game points every gun - any
    // character's, any weapon - at the crosshair (from their shoulder: near enough the view's way). Turned in toward the
    // middle a little (its side in view). Until then, a guess from the relaxed stance below.
    if (steady && view.aim > 0.98f && !st.aimTurn && !g_armTurnTest) {
        st.turn = Mul(Transposed(V), Mul(About(U, kInward * side), R[a.hand]));
        st.inward = kInward * side;
        st.haveTurn = st.aimTurn = true;
        Log(L"Motion: player %d (%d bones): gun hold taken from the game's aim", slot, bones);
    }
    if (!st.haveTurn) {   // (until then, the game's arms)
        if (!steady || view.rest < 0.98f) return false;
        M3 turn = FromTo(Unit(place[a.hand] - place[a.elbow]), F);
        const float* by = g_armTurnTest ? g_armTurn : fit->turn;
        turn = Mul(Mul(Mul(About(F, by[2]), About(U, by[1])), About(Rt, by[0])), turn);
        st.turn = Mul(Transposed(V), Mul(turn, R[a.hand]));
        st.inward = by[1];
        st.haveTurn = true;
        Log(L"Motion: player %d (%d bones): gun hold taken - hand bone %d (shoulder %d, elbow %d), other hand %d, head %d, "
            L"turned %.0f / %.0f / %.0f", slot, bones, a.hand, a.shoulder, a.elbow, other, head, by[0], by[1], by[2]);
    }
    // What the game's hand does beyond how it holds the gun. Standing: its turn and place against the upper arm less
    // the hold of the stance it's in (between the two as the game eases from one to the other - the hand swings 45-60
    // degrees between them, and that's not the gun moving; a stance not seen held yet: the slow average) - a shot's
    // kick, a reload. On the move: less the slow average (the run swings the arm about: only its quick part). And none
    // of it while the arm does something else - a melee swing, a jump, a power-up's throw: the gun held steady.
    float stance = view.aim + view.rest > 0.05f ? view.aim / (view.aim + view.rest) : 0;
    float held = view.aim + view.rest < 1 ? view.aim + view.rest : 1;
    const float* hq[2];
    V3 ha[2];
    for (int s = 0; s < 2; ++s) {
        hq[s] = st.haveHold[s] ? st.holdQ[s] : st.slow;
        ha[s] = st.haveHold[s] ? st.holdAt[s] : st.slowAt;
    }
    float refQ[4], actQ[4], moveQ[4], qa[4], one[4] = {0, 0, 0, 1};
    Toward(hq[0], hq[1], stance, refQ);
    V3 refAt = ha[0] + (ha[1] - ha[0]) * stance;
    float step = (float)(since > 0 && since < 0.5 ? since : 0) / kMoving;
    st.moving = view.still ? (st.moving - step > 0 ? st.moving - step : 0) : (st.moving + step < 1 ? st.moving + step : 1);
    // (the aiming stance not seen held yet: little of it - its own swing in from the relaxed one isn't the gun moving)
    bool guess = !st.haveHold[1] && stance > 0;
    float actTurn = guess ? kMoveTurn : kActTurn, actMove = guess ? kMoveMove : kActMove;
    ToQuat(Limited(Mul(Transposed(FromQuat(refQ)), Q), actTurn), actQ);
    ToQuat(Limited(Mul(Transposed(FromQuat(st.slow)), Q), kMoveTurn), moveQ);
    Toward(actQ, moveQ, st.moving, qa);
    Toward(one, qa, held, qa);
    V3 act = P - refAt, mv = P - st.slowAt;
    if (Len(act) > actMove * k) act = Unit(act) * (actMove * k);
    if (Len(mv) > kMoveMove * k) mv = Unit(mv) * (kMoveMove * k);
    V3 kick = (act + (mv - act) * st.moving) * held;
    // (zoomed in, under the crosshair: the gun pointing straight ahead - its turn in toward the middle undone; the
    // view's axes are a mirror of the character's: about its up, the other way round)
    M3 handRot = Mul(Mul(Mul(V, About({0, 1, 0}, st.inward * z)), st.turn), FromQuat(qa));
    target = target + Mul(R[a.shoulder], kick);
    // The other hand: on the gun too (a rifle - seen so standing relaxed, decided once): along with it, where the game
    // has it on the gun. Otherwise the game's, but taken down under the view's bottom left as the aiming stance comes
    // in: aiming, the game brings it up to steady the gun (Lois's open hand across the view). Relaxed, it's left where
    // the game has it (Death's staff is in it).
    bool second = other >= 0 && FindArm(parent, children, other, head, place.data(), &a2);
    if (st.twoHands < 0 && second && steady && view.rest > 0.98f) st.twoHands = Len(place[other] - place[gun]) < 0.35f * k;
    bool both = second && st.twoHands == 1;
    V3 target2 = target;
    M3 handRot2 = second ? R[a2.hand] : M3{};
    if (both) {
        M3 onGun = Mul(handRot, Transposed(R[a.hand]));
        target2 = target + Mul(onGun, place[a2.hand] - place[a.hand]);
        handRot2 = Mul(onGun, R[a2.hand]);
    } else if (second) {
        V3 low = eye - Rs * (0.6f * view.tans[0] * ahead) - U * (1.6f * view.tans[1] * ahead) + F * (ahead * 0.7f);
        target2 = place[a2.hand] + (low - place[a2.hand]) * view.aim;
    }
    std::vector<float> game(rot, rot + bones * 4), gamePos(pos, pos + bones * 3);
    // the gun arm's shoulder held below and to the right of the eye, by how far its arm reaches (a shooter's arm
    // doesn't go up and down with their walk; the upper arm swung into the view)
    float reach = Len(place[a.elbow] - place[a.shoulder]) + Len(place[a.hand] - place[a.elbow]);
    V3 shoulderAt = target - F * (reach * 0.75f) - U * (reach * 0.25f) + Rs * (reach * 0.1f);
    ReachArm(parent, n, a, target, handRot, Rs * 0.6f - U, rot, pos, R.data(), place.data(), &shoulderAt);
    if (second) ReachArm(parent, n, a2, target2, handRot2, Rs * -0.6f - U, rot, pos, R.data(), place.data());
    if (w < 1) {   // (easing in and out: from the game's pose toward ours by w)
        for (int b = 0; b < bones; ++b) {
            float* q = rot + b * 4;
            const float* g = &game[b * 4];
            float d = q[0] * g[0] + q[1] * g[1] + q[2] * g[2] + q[3] * g[3], s = d < 0 ? -1.0f : 1.0f, len = 0;
            for (int c = 0; c < 4; ++c) { q[c] = g[c] + (s * q[c] - g[c]) * w; len += q[c] * q[c]; }
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

void MotionArmsTurnTest(float pitch, float yaw, float roll) {
    g_armTurnTest = true;
    g_armTurn[0] = pitch;
    g_armTurn[1] = yaw;
    g_armTurn[2] = roll;
    for (ArmState& st : g_armState) st.haveTurn = false;   // (taken again now)
    Log(L"Motion: test - gun hand turned %.0f / %.0f / %.0f degrees", pitch, yaw, roll);
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
