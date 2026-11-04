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
#include <map>
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
    bool viewmodel = false;               // first person: only their arms left (their body hidden away), and their gun
    float gunHand[13] = {};               //   hand in the world, how far shrunk, the eye and the way the view looks they
                                          //   were posed for, the sway and bob in it - drawn as a shooter's
                                          //   (firstperson.cpp)
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
float g_armRight = 0.5f, g_armDown = 0.9f, g_armAhead = 1.1f;
float g_armZoomRight = 0.3f, g_armZoomDown = 1.2f;
bool g_armsOn = true, g_armPlaceTest = false;
bool g_armsHideBody = true;   // (research: "fpv body <0|1>" - the rest of their body hidden away in their own view)
const float kTiny = 0.001f;   // (what the hidden body is shrunk to)
float g_armScale = 0.35f;     // (the arms drawn as a shooter's: shrunk toward the eye - research: "fpv armscale <s>")
const float kArmNearest = 0.22f;   // (...but none of their bones nearer the eye than this)
// By character (their skeleton's size, and their right hand's bone where that's shared - Meg's and Death's): where
// their gun hand comes out instead, and whether it's the left hand (Chris's bow). Death low and far (his robe's
// sleeves hang wide), Quagmire further right (his shirt's sleeves).
// Dual: a gun in the other hand too (Carter's, Cleveland's) - that hand left as the game holds it, never put on the
// first gun (OffHand).
// Rifle: a two-handed long gun, its barrel from the gun hand to the front grip - pointed at the crosshair (the game's
// aiming hold is how it zooms: Quagmire's Tommy gun stood up by his face).
struct ArmPlace { int bones, hand; float right, down, ahead; bool left, dual, rifle; };
const ArmPlace kArmPlaces[] = {{61, 30, 0.5f, 1.5f, 1.6f, false, false},     // Death
                               {66, 47, 0.75f, 0.9f, 1.2f, false, false, true},    // Quagmire
                               {47, -1, 0.5f, 0.9f, 1.1f, true, false},    // Chris
                               {53, -1, 0.5f, 0.9f, 1.1f, false, true},    // Carter
                               {44, -1, 0.5f, 0.9f, 1.1f, false, true}};   // Cleveland

// a rotation by `deg` degrees about `axis` (unit)
M3 About(V3 axis, float deg) {
    float a = deg * 3.14159265f / 180, c = cosf(a), s = sinf(a), t = 1 - c, x = axis.x, y = axis.y, z = axis.z;
    return {{t * x * x + c, t * x * y - s * z, t * x * z + s * y,
             t * x * y + s * z, t * y * y + c, t * y * z - s * x,
             t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
}

const float kSteady = 0.4f;   // (s: standing steady before a stance's hold is taken)

const int kOffsets = 32;   // (the last poses' sway and bob kept - ArmState::offsets)
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
    V3 fw = {0, 0, 1}, sway = {0, 0, 0};  // (which way they looked last frame, the world; the arms' sway - the view's
    bool haveFw = false;                  //  space)
    float bobOn = 0, bobPhase = 0;        // (the bob: 0..1 how fast they go of a run, where in its stride)
    V3 lastEye = {0, 0, 0};               // (the eye last frame, the world - how fast they go)
    float k = 0;                          // (their size)
    double devLogAt = 0;
    float devMost = 0, devAngle = 0;      // (... the gun hand off its hold as shown, degrees)
    bool haveGrip = false;                // (the off hand on the gun in the game's relaxed carry - a rifle's front grip:
    M3 gripR;                             //  its turn and place against the gun hand)
    V3 gripAt = {0, 0, 0};
    V3 poseEye = {0, 0, 0}, poseOrg = {0, 0, 0}, poseHand = {0, 0, 0}, poseFw = {0, 0, 1};   // (research: the trace)
    double poseAt = 0;
    float stillFor = 0, moving = 1;       // (s stood still; 0..1 on the move - eased)
    float lastArmS = 0;                   // (research: armswhy)
    V3 headRest = {0, 0, 0}, gameBob = {0, 0, 0};   // (the head standing - the character's space; the bob - the view's)
    V3 gameBobIn = {0, 0, 0}, gameBobMid = {0, 0, 0};   //   (... held to a rate; half eased)
    bool haveHeadRest = false;
    float gameBobOn = 0;
    V3 offset = {0, 0, 0};                // (this pose's sway and bob, the view's space; the last poses' with their times)
    struct Timed { double t; V3 v; } offsets[kOffsets];
    int offsetsAt = 0, offsetsN = 0;
};
const float kStillFor = 0.35f;   // (s: stood still this long before the game's own moves of the arms are shown again)
// (a two-handed hold: the off hand within this of the gun hand in the game's relaxed carry - for an eye 1.8 up; let go
// of the gun while the gun hand is this far off its hold, degrees)
const float kGripNear = 0.6f, kGripFree = 12;
const float kAimFar = 20;   // (a two-handed gun pointed at the crosshair this far ahead - for an eye 1.8 up)
bool g_gunAim = true;       // (research: "fpv gunaim <0|1>")
// While the game eases between its stances (aiming, relaxed) its arms are between its two holds - the gun hand up
// to 25 degrees off both: how much of that's kept out (research: "fpv armsdev <0..1> [log 0|1]")
float g_devMix = 1;
bool g_devLog = false;
bool g_offHandOn = true;   // (research: "fpv offhand <0|1>")
int g_bobTrace = 0;        // (research: "fpv bobtrace <poses>" - the game's own bounce, each pose)
int g_orderTrace = 0;      // (research: "fpv order <n>" - when poses are built and frames drawn, which thread)
bool g_bobView = false;    //   (... "fpv bobtrace <poses> 1": the bob given the gun instead)
// how much of the head's bounce the gun's bob is: up and down, side to side (research: "fpv gamebob <up> <side>"); how
// fast it follows (s)
float g_gameBob[2] = {0.25f, 0.25f};
const float kGameBobEase = 0.04f, kGameBobRate = 0.25f;   // (s, each of two; the most a second, for an eye 1.8 up)
// the sway (for an eye 1.8 up): how far a radian a second of turning moves the arms, the most, how fast it eases (/s);
// the bob: how far a stride (two steps) goes, how fast a run is (a second - both for an eye 1.8 up), its side to side
// and up and down at a run
const float kSwayTurn = 0.012f, kSwayMost = 0.035f, kSwayEase = 10, kBobStride = 2.6f, kBobRun = 6, kBobSide = 0.006f,
            kBobUp = 0.008f;

// a's turn toward b by t (quaternions, the short way; normalized - near enough for small steps)
void Toward(const float* a, const float* b, float t, float* out) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -1.0f : 1.0f, len = 0;
    for (int c = 0; c < 4; ++c) { out[c] = a[c] + (s * b[c] - a[c]) * t; len += out[c] * out[c]; }
    len = len > 0 ? 1 / sqrtf(len) : 1;
    for (int c = 0; c < 4; ++c) out[c] *= len;
}
ArmState g_armState[4];
// a character's aiming hold (by their skeleton's size and their gun hand's bone - Meg's and Death's are the same size):
// the chest in the view, the gun hand on it, where the arms hang on it, the arms' pose. Kept in FGUYMod\fpv_holds.dat
// (loaded at start, saved when one is new): their arms are in their view from the start, no aiming first.
struct AimMemory {
    bool have = false;
    float chestQ[4], rootQ[2][4];
    V3 chestAt, hand, rootAt[2];
    std::vector<float> rot, pos;
};
std::map<int, AimMemory> g_aimMemory;
volatile bool g_aimDirty = false;
const DWORD kHoldsMagic = 0x31485046;   // "FPH1"

void HoldsPath(wchar_t* path) { swprintf_s(path, MAX_PATH, L"%s\\fpv_holds.dat", g_modDir); }
void SaveHolds() {   // (the lock held)
    std::vector<unsigned char> out;
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const unsigned char*)p, (const unsigned char*)p + n); };
    DWORD count = 0;
    for (auto& e : g_aimMemory) count += e.second.have;
    put(&kHoldsMagic, 4);
    put(&count, 4);
    for (auto& e : g_aimMemory) {
        const AimMemory& m = e.second;
        if (!m.have) continue;
        int key = e.first, nr = (int)m.rot.size(), np = (int)m.pos.size();
        put(&key, 4);
        put(m.chestQ, sizeof(m.chestQ));
        put(m.rootQ, sizeof(m.rootQ));
        put(&m.chestAt, sizeof(V3));
        put(&m.hand, sizeof(V3));
        put(m.rootAt, sizeof(m.rootAt));
        put(&nr, 4);
        put(m.rot.data(), nr * sizeof(float));
        put(&np, 4);
        put(m.pos.data(), np * sizeof(float));
    }
    wchar_t path[MAX_PATH];
    HoldsPath(path);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, out.data(), (DWORD)out.size(), &wrote, nullptr);
    CloseHandle(f);
    Log(L"Motion: %lu characters' first-person holds saved to %s", count, path);
}
void LoadHolds() {
    wchar_t path[MAX_PATH];
    HoldsPath(path);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::vector<unsigned char> in(size < (1u << 24) ? size : 0);
    ReadFile(f, in.data(), (DWORD)in.size(), &got, nullptr);
    CloseHandle(f);
    size_t at = 0;
    auto get = [&](void* p, size_t n) {
        if (at + n > got) return false;
        memcpy(p, in.data() + at, n);
        at += n;
        return true;
    };
    DWORD magic = 0, count = 0;
    if (!get(&magic, 4) || magic != kHoldsMagic || !get(&count, 4)) return;
    for (DWORD i = 0; i < count && i < 64; ++i) {
        AimMemory m;
        int key = 0, nr = 0, np = 0;
        if (!get(&key, 4) || !get(m.chestQ, sizeof(m.chestQ)) || !get(m.rootQ, sizeof(m.rootQ)) ||
            !get(&m.chestAt, sizeof(V3)) || !get(&m.hand, sizeof(V3)) || !get(m.rootAt, sizeof(m.rootAt)) ||
            !get(&nr, 4) || nr < 0 || nr > 1024) return;
        m.rot.resize(nr);
        if (!get(m.rot.data(), nr * sizeof(float)) || !get(&np, 4) || np < 0 || np > 768) return;
        m.pos.resize(np);
        if (!get(m.pos.data(), np * sizeof(float))) return;
        m.have = true;
        g_aimMemory[key] = m;
    }
    Log(L"Motion: %lu characters' first-person holds loaded", count);
}
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

// a hand against the chest: its arm's root there (rq, rAt), down the arm by the pose's locals
void HandOn(const unsigned char* parent, int root, int hand, const float* rq, V3 rAt, const float* rot, const float* pos,
            M3* outR, V3* outP) {
    int chain[64], c = 0;
    for (int b = hand; b != root && b != 0xff && c < 64; b = parent[b]) chain[c++] = b;
    M3 r = FromQuat(rq);
    V3 p = rAt;
    for (int i = c - 1; i >= 0; --i) {
        int b = chain[i];
        p = p + Mul(r, V3{pos[(b + 1) * 3], pos[(b + 1) * 3 + 1], pos[(b + 1) * 3 + 2]});
        r = Mul(r, FromQuat(rot + (b + 1) * 4));
    }
    *outR = r;
    *outP = p;
}
// a turn as axis x angle (radians), and back
V3 RotVec(const M3& m) {
    float q[4];
    ToQuat(m, q);
    if (q[3] < 0) for (int c = 0; c < 4; ++c) q[c] = -q[c];
    float s = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (s < 1e-6f) return {0, 0, 0};
    return V3{q[0], q[1], q[2]} * (2 * atan2f(s, q[3]) / s);
}
M3 FromRotVec(V3 v) {
    float a = Len(v);
    if (a < 1e-6f) return {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
    return About(v * (1 / a), a * 57.2958f);
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
    // (a two-handed hold: on the gun but while the game's moves take the gun hand off its hold - a reload, a swing)
    if (st.haveGrip) want = st.devAngle < kGripFree ? 1.0f : 0.0f;
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
    if (st.haveGrip) {   // (a two-handed hold seen in the game's own carry: the off hand where it held the gun then)
        target = at[a.hand] + Mul(R[a.hand], st.gripAt);
        handRot = Mul(R[a.hand], st.gripR);
    }
    V3 pole = Mul(V, V3{1.0f, -1.0f, 0});   // (its elbow out to the left and down)
    Reach(parent, n, a2, target, handRot, pole, rot, pos, R, at);
    float s = st.support * st.support * (3 - 2 * st.support);
    for (int b = 0; b < bones; ++b) {
        Toward(&before[b * 4], rot + b * 4, s, rot + b * 4);
        for (int c = 0; c < 3; ++c) pos[b * 3 + c] = beforePos[b * 3 + c] + (pos[b * 3 + c] - beforePos[b * 3 + c]) * s;
    }
}

bool ArmsInViewOnce(int slot, const unsigned char* ent, float* rot, float* pos, float* scale, int bones, bool* viewmodel,
                    float* gunHand, int* why);
// (research, "fpv armswhy <0|1>": why a player's arms aren't first person's this pose - logged as it changes)
bool g_armsWhyLog = false;
int g_armsWhy[4] = {-1, -1, -1, -1};
bool ArmsInView(int slot, const unsigned char* ent, float* rot, float* pos, float* scale, int bones, bool* viewmodel,
                float* gunHand) {
    int why = 0;
    bool on = ArmsInViewOnce(slot, ent, rot, pos, scale, bones, viewmodel, gunHand, &why);
    if (slot >= 0 && slot < 4 && g_armsWhyLog && why != g_armsWhy[slot]) {
        static const wchar_t* kWhy[] = {L"first person's", L"not in first person (FirstPersonArms)", L"no skeleton",
                                        L"no hands found", L"no arms found", L"no aiming hold yet", L"a new entity"};
        static const wchar_t* kNo[] = {L"weight 0", L"first person off", L"not their view", L"view pulled back",
                                       L"emoting", L"no view on the screen", L"dead", L"no look", L"no eye"};
        if (why >= 10) Log(L"Motion: player %d's arms - not first person's: %s", slot, why - 10 < 9 ? kNo[why - 10] : L"?");
        else Log(L"Motion: player %d's arms - %s", slot, why >= 0 && why < 7 ? kWhy[why] : L"?");
        g_armsWhy[slot] = why;
    }
    return on;
}
bool ArmsInViewOnce(int slot, const unsigned char* ent, float* rot, float* pos, float* scale, int bones, bool* viewmodel,
                    float* gunHand, int* why) {
    if (slot < 0 || slot >= 4) return false;
    ArmState& st = g_armState[slot];
    if (st.ent != ent) {
        if (g_armsWhyLog) Log(L"Motion: player %d's arms - a new entity %p (was %p)", slot, ent, st.ent);
        st = ArmState(), st.ent = ent;
    }
    ArmsView view;
    if (!g_armsOn || !FirstPersonArms(slot, &view) || view.weight <= 0) {
        st.at = 0;   // (the steadiness measured afresh next time; the holds kept)
        *why = 10 + FirstPersonArmsNoWhy();   // (10 + FirstPersonArms' reason - 10: in, but its weight 0)
        return false;
    }
    *why = 2;
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
    // (the character's size - from their eye's height standing in first person, not aiming, kept: the eye goes lower
    // aiming, Stewie's by a third, and the arms came nearer with it; still going into first person it's higher -
    // Quagmire's arms came out small)
    if (w >= 0.999f && view.still && view.aim < 0.01f) st.k = eye.y / 1.8f;
    float k = st.k > 0 ? st.k : eye.y / 1.8f;
    if (!st.found) {
        st.found = FindBody(parent, children, n, place.data(), eye, st.hands, &st.head);
        if (!st.found) {
            if (!st.told) Log(L"Motion: player %d (%d bones): no hands found - the game's arms", slot, bones);
            st.told = true;
            *why = 3;
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
        *why = 4;
        return false;
    }
    int roots[2] = {a.clav >= 0 ? a.clav : a.shoulder, a2.clav >= 0 ? a2.clav : a2.shoulder};
    int chest = parent[roots[0]];
    if (g_bobTrace > 0 && !g_bobView) {   // (research: the game's own bounce - the chest and head against their place)
        --g_bobTrace;
        V3 c = place[chest], hd = st.head >= 0 ? place[st.head] : V3{0, 0, 0};
        Log(L"Motion: bob %.4f - chest %.4f %.4f %.4f, head %.4f %.4f %.4f, still %d rest %.2f aim %.2f", Now(), c.x, c.y,
            c.z, hd.x, hd.y, hd.z, view.still ? 1 : 0, view.rest, view.aim);
    }
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
            V3 off = place[other] - place[gun];
            // (the relaxed carry two-handed - not Chris's bow: his other hand on its string)
            if (!s && Len(off) < kGripNear * k && (!place0 || (!place0->dual && !place0->left))) {
                M3 Gt = Transposed(R[gun]);
                if (!st.haveGrip)
                    Log(L"Motion: player %d (%d bones): two-handed - the off hand on the gun %.2f from the gun hand", slot,
                        bones, Len(off));
                st.gripR = Mul(Gt, R[other]);
                st.gripAt = Mul(Gt, off);
                st.haveGrip = true;
            }
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
    AimMemory& mem = g_aimMemory[bones * 256 + st.hands[0]];
    if (st.have[1] && !st.aimRot.empty()) {
        if (!mem.have) {
            mem.have = true;
            g_aimDirty = true;
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
    *why = 5;
    if (!st.have[1] || st.aimRot.empty()) return false;   // (until then, the game's arms - their relaxed carry, about
                                                          //  their head or hip, out of the view or across it)
    *why = 0;
    // In the relaxed stance - standing about, running - the game's relaxed carry (Peter's drill up beside his head,
    // a pistol at the hip) is out of or across the view: their arms hold the gun as the game does aiming instead,
    // ready, steady. Firing, zooming, reloading (the aiming stance), a melee swing, a jump, a power-up's throw (neither
    // stance): the game's own moves. Between, eased as the game eases its stances (by the relaxed stance's weight).
    std::vector<float> game(rot, rot + bones * 4), gamePos(pos, pos + bones * 3);   // (the game's, for easing in / out)
    bool armBone[255] = {};
    for (int b = 0; b < n; ++b)
        for (int c = b; c >= 0; c = parent[c] == 0xff ? -1 : parent[c])
            if (c == roots[0] || c == roots[1]) { armBone[b] = c != b; break; }
    // (on the move - running, a jump - the game plays its own arm swing over both stances: held so too. Stopping, the
    // game's relaxed stance comes back over ~0.2 s, and its "still" flickers on now and then mid-walk: the move's
    // swing held out until they've stood still a while - taken at once, Peter's drill flew up by his head at each stop.)
    float stepDt = dt > 0 && dt < 0.25f ? dt : 0;
    st.stillFor = view.still ? st.stillFor + stepDt : 0;
    float movingWant = view.still && st.stillFor > kStillFor ? 0.0f : 1.0f;
    st.moving = movingWant > st.moving ? movingWant : st.moving + (movingWant - st.moving) * (1 - expf(-stepDt / 0.1f));
    float act = 1 - view.aim - view.rest;   // (neither stance: a melee swing, a power-up's throw - or the move's swing,
    act = act < 0 ? 0 : act;                //  or an idle fidget: held out, eased - it came in over ~0.2 s)
    // (the fidget's weight is the game's own, eased: as much of the rest held out - the most of 1 at its full)
    float idleOut = view.idle * 4 < 1 ? view.idle * 4 : 1;
    float heldOut = st.moving > idleOut ? st.moving : idleOut;
    float held = st.aimRot.size() == (size_t)bones * 4 ? view.rest + act * heldOut : 0;
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
    // where that puts the gun hand in the view, and where it should come out: the whole hold moved by the difference.
    // Their arms have a field of view of their own (kArmTan - drawn so: firstperson.cpp), so they're placed in it.
    V3 hand = hAt + Mul(H, hHand);
    float z = view.zoom, side = left ? -1.0f : 1.0f;
    ArmsView av = view;
    av.tans[0] = kArmTan;
    av.tans[1] = kArmTan * view.tans[1] / view.tans[0];
    bool mine = place0 && !g_armPlaceTest;
    float right0 = mine ? place0->right : g_armRight, down0 = mine ? place0->down : g_armDown;
    float ahead0 = mine ? place0->ahead : g_armAhead;
    float zoomDown = g_armZoomDown > down0 ? g_armZoomDown : down0;   // (Death's sleeves stay low zoomed in too)
    float right = right0 + (g_armZoomRight - right0) * z, down = down0 + (zoomDown - down0) * z;
    float depth = hand.z > ahead0 * k ? hand.z : ahead0 * k;
    V3 want = {-side * right * av.tans[0] * depth, -down * av.tans[1] * depth, depth};   // (x: to the left)
    // Sway and bob, as a shooter's gun has: turning, the arms lag a little behind the view (and settle); on the move, a
    // gentle bob of their own (the game's run swing is held out - above).
    {
        V3 fw = {view.forward[0], view.forward[1], view.forward[2]};
        float step = dt > 0 && dt < 0.25f ? dt : 0;
        if (step > 0 && st.haveFw) {
            V3 rw = Unit(Cross(fw, {0, 1, 0}));
            float turnRight = Dot(fw - st.fw, rw) / step, turnUp = (fw.y - st.fw.y) / step;   // (radians a second)
            V3 to = {turnRight * kSwayTurn * k, -turnUp * kSwayTurn * k, 0};   // (x: left - turning right, it lags left)
            float lim = kSwayMost * k, len = Len(to);
            if (len > lim) to = to * (lim / len);
            float t = 1 - expf(-step * kSwayEase);
            st.sway = st.sway + (to - st.sway) * t;
            // the bob by how far they actually go (along the ground, eased): a stride's figure of eight - side to side
            // once, up and down twice - all smooth, its size by their speed of a run
            V3 we = {view.eye[0], view.eye[1], view.eye[2]};
            float moved = sqrtf((we.x - st.lastEye.x) * (we.x - st.lastEye.x) + (we.z - st.lastEye.z) * (we.z - st.lastEye.z));
            if (moved > 1.0f) moved = 0;   // (a respawn, a teleport)
            float speed = moved / step / k, of = speed / kBobRun;
            of = of > 1 ? 1 : of;
            st.bobOn += (of - st.bobOn) * (1 - expf(-step / 0.15f));
            st.bobPhase = fmodf(st.bobPhase + moved / k / kBobStride * 6.2832f, 6.2832f);
        }
        st.fw = fw;
        st.haveFw = true;
        st.lastEye = {view.eye[0], view.eye[1], view.eye[2]};
        // The bob is the game's own: their head's bounce in its walk and run (against where it is standing - in the
        // view's terms: side to side, up and down), a part of it, smoothed (the run's pose jumps 1 cm now and then).
        // Its steps and speed as the game plays them: walking ~8 cm up and down twice a stride, running ~28 cm.
        V3 bob = {0, 0, 0};
        if (st.head >= 0 && st.head < n) {
            V3 hd = place[st.head];
            bool rest = view.still && st.stillFor > kStillFor && view.idle <= 0 && view.aim < 0.01f;
            if (!st.haveHeadRest) st.headRest = hd, st.haveHeadRest = true;
            else if (rest && step > 0) st.headRest = st.headRest + (hd - st.headRest) * (1 - expf(-step / 0.3f));
            V3 d = Mul(Transposed(V), hd - st.headRest);   // (the view's left, up, ahead)
            V3 to = {d.x * g_gameBob[1], d.y * g_gameBob[0], 0};
            float lim = 0.06f * k;
            if (Len(to) > lim) to = to * (lim / Len(to));
            if (step > 0) {
                // (the game's head jumps now and then - its run 1-12 cm from one pose to the next: the target held to
                // kGameBobRate, then eased twice - its speed smooth too, no kink at a jump)
                V3 dv = to - st.gameBobIn;
                float most = kGameBobRate * k * step;
                if (Len(dv) > most) dv = dv * (most / Len(dv));
                st.gameBobIn = st.gameBobIn + dv;
                float t = 1 - expf(-step / kGameBobEase);
                st.gameBobMid = st.gameBobMid + (st.gameBobIn - st.gameBobMid) * t;
                st.gameBob = st.gameBob + (st.gameBobMid - st.gameBob) * t;
            }
            float on = view.still ? 0.0f : 1.0f;   // (standing: none - a turn on the spot bends the neck)
            st.gameBobOn += (on - st.gameBobOn) * (step > 0 ? 1 - expf(-step / 0.15f) : 1);
            bob = st.gameBob * (st.gameBobOn * (1 - z));
        }
        if (g_bobTrace > 0 && g_bobView) {
            --g_bobTrace;
            Log(L"Motion: gamebob %.4f %.4f %.4f (step %.4f, head %.4f %.4f %.4f)", Now(), bob.x, bob.y, step,
                st.head >= 0 ? place[st.head].x : 0, st.head >= 0 ? place[st.head].y : 0, st.head >= 0 ? place[st.head].z : 0);
        }
        // (the sway and bob given the pose, kept with its time: the frames draw them as they are at their own time -
        // the pose is built on the game's thread at its own times, a frame showing one pose twice or none: a bob in
        // steps. firstperson.cpp FrameArms takes this pose's out and puts in the frame's - MotionOffsetAt)
        st.offset = st.sway + bob;
        st.offsets[st.offsetsAt] = {now, st.offset};
        st.offsetsAt = (st.offsetsAt + 1) % kOffsets;
        if (st.offsetsN < kOffsets) ++st.offsetsN;
        want = want + st.offset;
    }
    M3 chestNew = Mul(V, H);
    V3 chestNewAt = eye + Mul(V, hAt + want - hand);
    // both arms against the chest - where the game hangs them now, or (held) as it does aiming
    M3 Ct = Transposed(R[chest]);
    float lq[2][4];
    V3 lAt[2];
    for (int i = 0; i < 2; ++i) {
        int r = roots[i];
        ToQuat(Mul(Ct, R[r]), lq[i]);
        lAt[i] = Mul(Ct, place[r] - place[chest]);
        if (held > 0) {
            Toward(lq[i], st.rootQ[i], held, lq[i]);
            lAt[i] = lAt[i] + (st.rootAt[i] - lAt[i]) * held;
        }
    }
    // The gun hand off its aiming hold (against the chest): a shot's kick, a reload, a melee swing as the game plays
    // them - but while the game eases between its stances (its arms then between its two holds, the gun swung up to
    // 25 degrees), kept on the hold. Both arms moved together by what's kept out: the off hand goes on as it does.
    {
        M3 gR, hR;
        V3 gP, hP;
        HandOn(parent, roots[0], a.hand, lq[0], lAt[0], rot, pos, &gR, &gP);
        HandOn(parent, roots[0], a.hand, st.rootQ[0], st.rootAt[0], st.aimRot.data(), st.aimPos.data(), &hR, &hP);
        V3 dr = RotVec(Mul(gR, Transposed(hR))), dp = (gP - hP) * (1 / k);
        float otherHold = view.rest + act * heldOut;   // (the other hold: relaxed, or the move's swing - held out)
        float mix = 4 * view.aim * (otherHold > 1 ? 1 : otherHold) * g_devMix;
        // (zoomed in, the gun held steady where it's placed - the game's own raising it to the eye, Quagmire's Tommy gun
        // stood up by his face, pointed it at the sky)
        float pass = (1 - (mix > 1 ? 1 : mix)) * (1 - z);
        V3 fr = dr * pass, fp = dp * pass;
        st.devAngle = Len(fr) * 57.3f;
        if (g_devLog) {   // (the most the gun was off its hold in each tenth of a second)
            float shown = Len(fr) * 57.3f;
            st.devMost = shown > st.devMost ? shown : st.devMost;
            if (now - st.devLogAt > 0.1) {
                st.devLogAt = now;
                Log(L"Motion: dev aim %.2f rest %.2f still %d moving %.2f idle %.2f [%d] held %.2f - hand %.3f turn %.1f deg, pass %.2f, shown most %.1f deg",
                    view.aim, view.rest, view.still ? 1 : 0, st.moving, view.idle, view.layer90, held, Len(dp),
                    Len(dr) * 57.3f, pass, st.devMost);
                st.devMost = 0;
            }
        }
        M3 Tr = Mul(FromRotVec(fr), hR);
        V3 Tp = hP + fp * k;
        if (st.haveGrip && g_gunAim && place0 && place0->rifle) {
            // A two-handed gun pointed at the crosshair: its barrel runs from the gun hand to the front grip (the off
            // hand in the game's carry), turned about the gun hand (its hold's - the game's moves still on top) to point
            // from where the hand is in the view at the crosshair far ahead. (Quagmire's hold - the game's aiming one,
            // as it zooms - stands his Tommy gun up by his face.)
            M3 Cn = Mul(V, H);   // (the chest's axes in the character's space)
            V3 barrel = Mul(Cn, Mul(hR, Unit(st.gripAt)));
            V3 handView = want, aimAt = {0, 0, kAimFar * k};
            V3 to = Mul(V, Unit(aimAt - handView));
            M3 turn = FromTo(Unit(barrel), to);
            Tr = Mul(Mul(Mul(Transposed(Cn), turn), Cn), Tr);
        }
        M3 Cr = Mul(Tr, Transposed(gR));
        V3 Cp = Tp - Mul(Cr, gP);
        for (int i = 0; i < 2; ++i) {
            M3 q = Mul(Cr, FromQuat(lq[i]));
            ToQuat(q, lq[i]);
            lAt[i] = Cp + Mul(Cr, lAt[i]);
        }
    }
    // both arms hung on the chest placed in the view - all that's on them comes along
    for (int i = 0; i < 2; ++i) {
        int r = roots[i], p = parent[r];
        M3 newR = Mul(chestNew, FromQuat(lq[i]));
        V3 newAt = chestNewAt + Mul(chestNew, lAt[i]);
        M3 Pt = Transposed(R[p]);
        ToQuat(Mul(Pt, newR), rot + (r + 1) * 4);
        V3 l = Mul(Pt, newAt - place[p]);
        pos[(r + 1) * 3] = l.x;
        pos[(r + 1) * 3 + 1] = l.y;
        pos[(r + 1) * 3 + 2] = l.z;
    }
    if ((!place0 || !place0->dual) && g_offHandOn) OffHand(st, av, parent, n, a, a2, k, eye, V, held, dt, rot, pos, R.data(), place.data());
    // Their own view the only one on the screen, fully in first person: the rest of their body hidden away - all of it
    // shrunk into the chest the arms hang on (the pose's root at kTiny, the arms' roots grown back by its inverse: they
    // and all on them as they are). Split screen, the partner sees them whole (the game's pose: one for every view).
    Posed(parent, n, rot, pos, R.data(), place.data());
    V3 hc = place[a.hand];
    bool viewmodelNow = w >= 0.999f && view.alone && g_armsHideBody;
    float armS = 1;
    if (viewmodelNow && g_armScale != 1) {
        // Drawn as a shooter's: both arms (and the gun on them) shrunk toward the eye - the same on the screen, but
        // within reach of the eye: the gun, a model of its own drawn where it is, came out past a wall stood up against
        // (behind a door - not drawn) at an arm's length.
        // (no nearer than kArmNearest: a bone that near the camera, the game stops drawing the character - Stewie's
        // hand, his eye low aiming. Shrunk toward the eye, more or less is the same on the screen.)
        float nearest = 1e9f;
        for (int b = 0; b < n; ++b)
            if (armBone[b] || b == roots[0] || b == roots[1]) nearest = fminf(nearest, Len(place[b] - eye));
        armS = g_armScale;
        if (nearest * armS < kArmNearest) armS = nearest > kArmNearest ? kArmNearest / nearest : 1;
        if (g_armsWhyLog && fabsf(armS - st.lastArmS) > 0.02f) {
            int nb = -1;
            for (int b = 0; b < n; ++b)
                if ((armBone[b] || b == roots[0] || b == roots[1]) && fabsf(Len(place[b] - eye) - nearest) < 1e-5f) nb = b;
            Log(L"Motion: player %d's arms shrunk %.2f (bone %d %.3f from the eye; size %.2f)", slot, armS, nb, nearest, k);
            st.lastArmS = armS;
        }
        for (int i = 0; i < 2; ++i) {
            int r = roots[i], p = parent[r];
            V3 l = Mul(Transposed(R[p]), eye + (place[r] - eye) * armS - place[p]);
            pos[(r + 1) * 3] = l.x;
            pos[(r + 1) * 3 + 1] = l.y;
            pos[(r + 1) * 3 + 2] = l.z;
            for (int c = 0; c < 3; ++c) scale[(r + 1) * 3 + c] *= armS;
        }
        hc = eye + (hc - eye) * armS;
        Posed(parent, n, rot, pos, R.data(), place.data());   // (the roots' places: the arms' bones beyond, unscaled)
    }
    {   // (their gun hand, in the world - firstperson.cpp finds the gun's draw by it; hiding the body leaves it there)
        V3 hw = org + ax[0] * hc.x + ax[1] * hc.y + ax[2] * hc.z;
        gunHand[0] = hw.x;
        gunHand[1] = hw.y;
        gunHand[2] = hw.z;
        gunHand[3] = armS;
        for (int c = 0; c < 3; ++c) gunHand[4 + c] = view.eye[c], gunHand[7 + c] = view.forward[c];
        gunHand[10] = st.offset.x * armS, gunHand[11] = st.offset.y * armS, gunHand[12] = st.offset.z * armS;
        st.poseEye = {view.eye[0], view.eye[1], view.eye[2]};
        st.poseOrg = org;
        st.poseHand = hw;
        st.poseFw = Unit({view.forward[0], view.forward[1], view.forward[2]});
        st.poseAt = now;
        if (g_devLog && now - st.devLogAt < 0.001)
            Log(L"Motion: eye (%.2f %.2f %.2f) gun hand (%.2f %.2f %.2f), pose root (%.2f %.2f %.2f) %.2f %.2f %.2f %.2f",
                view.eye[0], view.eye[1], view.eye[2], hw.x, hw.y, hw.z, pos[0], pos[1], pos[2], rot[0], rot[1],
                rot[2], rot[3]);
    }
    if (viewmodelNow) {
        M3 rr[2];
        V3 ra[2];
        for (int i = 0; i < 2; ++i) { rr[i] = R[roots[i]]; ra[i] = place[roots[i]]; }
        // (the skeleton's top bones - the game's pose leaves its own root entry, 0, out - shrunk, at the chest)
        // (kept behind the eye and below it, out of sight: at an arm's length ahead, the dot of outline it still has
        // peeked out by the gun hand; at the eye, the game stopped drawing Stewie altogether - the camera inside him)
        M3 base = FromQuat(rot);
        V3 hideAt = eye + Mul(V, V3{0, -0.3f * k, -0.8f * k});
        V3 baseAt = {pos[0], pos[1], pos[2]}, top = Mul(Transposed(base), hideAt - baseAt);
        for (int b = 0; b < n; ++b)
            if (parent[b] == 0xff) {
                pos[(b + 1) * 3] = top.x;
                pos[(b + 1) * 3 + 1] = top.y;
                pos[(b + 1) * 3 + 2] = top.z;
                for (int c = 0; c < 3; ++c) scale[(b + 1) * 3 + c] = kTiny;
            }
        // (the arms' parents now: their rotations as they were, their places and scale through the top's - the game's
        // way: a bone's translation in its parent's space, scaled by the parent's scale)
        std::vector<float> sc(n);
        Posed(parent, n, rot, pos, R.data(), place.data());
        for (int b = 0; b < n; ++b) {
            V3 t = {pos[(b + 1) * 3], pos[(b + 1) * 3 + 1], pos[(b + 1) * 3 + 2]};
            if (parent[b] == 0xff) {
                sc[b] = scale[(b + 1) * 3];
                place[b] = baseAt + Mul(base, t);
            } else {
                sc[b] = sc[parent[b]] * scale[(b + 1) * 3];
                place[b] = place[parent[b]] + Mul(R[parent[b]], t) * sc[parent[b]];
            }
        }
        for (int i = 0; i < 2; ++i) {
            int r = roots[i], p = parent[r];
            V3 l = Mul(Transposed(R[p]), ra[i] - place[p]) * (1 / sc[p]);
            ToQuat(Mul(Transposed(R[p]), rr[i]), rot + (r + 1) * 4);
            pos[(r + 1) * 3] = l.x;
            pos[(r + 1) * 3 + 1] = l.y;
            pos[(r + 1) * 3 + 2] = l.z;
            for (int c = 0; c < 3; ++c) scale[(r + 1) * 3 + c] /= sc[p];
        }
        *viewmodel = true;
    }
    if (g_devLog && now - st.devLogAt < 0.001) {   // (research: the bones as drawn - scaled - against the eye)
        std::vector<M3> FR(n);
        std::vector<V3> FP(n);
        std::vector<float> FS(n);
        M3 base = FromQuat(rot);
        V3 baseAt = {pos[0], pos[1], pos[2]}, lo = {1e9f, 1e9f, 1e9f}, hi = {-1e9f, -1e9f, -1e9f};
        float nearest = 1e9f;
        int nb = -1;
        for (int b = 0; b < n; ++b) {
            V3 t = {pos[(b + 1) * 3], pos[(b + 1) * 3 + 1], pos[(b + 1) * 3 + 2]};
            M3 l = FromQuat(rot + (b + 1) * 4);
            if (parent[b] == 0xff) FR[b] = Mul(base, l), FP[b] = baseAt + Mul(base, t), FS[b] = scale[(b + 1) * 3];
            else
                FR[b] = Mul(FR[parent[b]], l), FP[b] = FP[parent[b]] + Mul(FR[parent[b]], t) * FS[parent[b]],
                FS[b] = FS[parent[b]] * scale[(b + 1) * 3];
            float dd = Len(FP[b] - eye);
            if (dd < nearest) nearest = dd, nb = b;
            lo = {fminf(lo.x, FP[b].x), fminf(lo.y, FP[b].y), fminf(lo.z, FP[b].z)};
            hi = {fmaxf(hi.x, FP[b].x), fmaxf(hi.y, FP[b].y), fmaxf(hi.z, FP[b].z)};
        }
        bool inside = eye.x > lo.x && eye.x < hi.x && eye.y > lo.y && eye.y < hi.y && eye.z > lo.z && eye.z < hi.z;
        Log(L"Motion: bones - nearest the eye %d at %.2f (size %.2f), eye %s their box (%.2f..%.2f, %.2f..%.2f, %.2f..%.2f; eye %.2f %.2f %.2f)",
            nb, nearest, k, inside ? L"inside" : L"outside", lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, eye.x, eye.y, eye.z);
    }
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
        tr.viewmodel = false;
        ArmsInView(static_cast<int>(&tr - g_tracks), ent, rot, pos, scale, bones, &tr.viewmodel, tr.gunHand);
        if (g_orderTrace > 0 && &tr == g_tracks) {
            --g_orderTrace;
            Log(L"Order: %.5f pose (thread %lu) - eye %.4f %.4f %.4f", Now(), GetCurrentThreadId(), tr.gunHand[4],
                tr.gunHand[5], tr.gunHand[6]);
        }
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
void MotionArmsBodyTest(bool hide) {
    g_armsHideBody = hide;
    Log(L"Motion: test - the rest of the body %s in first person", hide ? L"hidden" : L"shown");
}
// research ("fpv armstrace <frames>"): each frame drawn, the camera's eye against the one the arms' pose was built for
int g_armTrace = 0;
void MotionArmsTraceTest(int frames) { g_armTrace = frames; }
void MotionArmsTrace(int slot, const float* eyeDraw, const float* fwDraw) {
    if (g_armTrace <= 0 || slot < 0 || slot >= 4) return;
    --g_armTrace;
    const ArmState& st = g_armState[slot];
    V3 e = {eyeDraw[0], eyeDraw[1], eyeDraw[2]}, fd = Unit({fwDraw[0], fwDraw[1], fwDraw[2]}), fp = st.poseFw;
    // the gun hand in the view (right, up, ahead): as drawn with the camera now (uncorrected), and posed (corrected)
    auto inView = [](V3 at, V3 f) { V3 r = Unit(Cross(f, {0, 1, 0})), u = Cross(r, f); return V3{Dot(at, r), Dot(at, u), Dot(at, f)}; };
    V3 raw = inView(st.poseHand - e, fd), fixed = inView(st.poseHand - st.poseEye, fp);
    Log(L"Motion: trace %.4f - pose %.1f ms ago, eye moved %.3f; hand in the view %.4f %.4f %.4f, posed %.4f %.4f %.4f",
        Now(), (Now() - st.poseAt) * 1000, Len(e - st.poseEye), raw.x, raw.y, raw.z, fixed.x, fixed.y, fixed.z);
}
void MotionOffHandTest(bool on) { g_offHandOn = on; }
void MotionOrderTest(int n) { g_orderTrace = n; }
int MotionOrderTrace() { return g_orderTrace; }
void MotionBobTraceTest(int poses, bool view) { g_bobTrace = poses; g_bobView = view; }
void MotionGameBobTest(float up, float side) {
    g_gameBob[0] = up;
    g_gameBob[1] = side;
    Log(L"Motion: test - the gun's bob %.2f of the head's up and down, %.2f side to side", up, side);
}
void MotionArmsWhyTest(bool on) { g_armsWhyLog = on; }
void MotionGunAimTest(bool on) { g_gunAim = on; }
void MotionArmsScaleTest(float s) {
    g_armScale = s > 0.01f ? s : 1;
    Log(L"Motion: test - the arms in first person drawn at %.2f toward the eye", g_armScale);
}
void MotionArmsDevTest(float mix, bool log) {
    g_devMix = mix;
    g_devLog = log;
    Log(L"Motion: test - the gun hand between stances kept on its hold by %.2f%s", mix, log ? L", logged" : L"");
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

// firstperson.cpp: a player's arms' sway and bob at time t (the PC's clock, s; the view's space: left, up, ahead) - from
// the poses' either side of it, kept with their times; false: none kept
bool MotionOffsetAt(int slot, double t, float* out) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return false;
    EnterCriticalSection(&g_lock);
    const ArmState& st = g_armState[slot];
    bool have = st.offsetsN > 0;
    if (have) {
        const ArmState::Timed* a = nullptr;   // (the newest at or before t, and the one after it)
        const ArmState::Timed* b = nullptr;
        for (int i = 0; i < st.offsetsN; ++i) {
            const ArmState::Timed& o = st.offsets[(st.offsetsAt - 1 - i + 2 * kOffsets) % kOffsets];
            if (o.t <= t) { a = &o; break; }
            b = &o;
        }
        V3 v;
        if (!a) v = b->v;                   // (older than all kept: the oldest)
        else if (!b) v = a->v;              // (newer than all: the newest)
        else {
            double k = b->t > a->t ? (t - a->t) / (b->t - a->t) : 1;
            v = a->v + (b->v - a->v) * (float)k;
        }
        out[0] = v.x, out[1] = v.y, out[2] = v.z;
    }
    LeaveCriticalSection(&g_lock);
    return have;
}

// firstperson.cpp: a player's arms are first person's this frame (the rest of them hidden away) - their gun hand, and
// (hand[3]) how far they're shrunk toward the eye, (hand[4..9]) the eye and the view's way ahead they were posed for
bool MotionViewmodel(int slot, float* hand) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return false;
    EnterCriticalSection(&g_lock);
    bool on = g_tracks[slot].viewmodel && g_tracks[slot].ent;
    if (on) memcpy(hand, g_tracks[slot].gunHand, sizeof(g_tracks[slot].gunHand));
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
    if (g_aimDirty) {
        g_aimDirty = false;
        SaveHolds();
    }
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
    LoadHolds();
}
