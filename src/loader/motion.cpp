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
    static const int kArmRing = 8;
    float ring[kArmRing][34] = {};        //   (the last poses given out, newest at ringAt - 1: a frame draws one of them, not always the newest - MotionViewmodelOf)
    int ringSeq[kArmRing] = {}, ringN = 0, ringAt = 0;
    float ringOrg[kArmRing][3] = {};      //   (where the character stood for each, and where their hidden body was put: a frame's draws say which pose they're of by them)
    float ringHide[kArmRing][3] = {};
    float ringHideM[kArmRing][3] = {};
    float skinned[kArmRing][34] = {};     //   (the poses the game's last skinning of their pieces fits, newest first: what the frame's draws are - Hook_SkinPiece.
    float skinnedOrg[kArmRing][3] = {};   //    Walking straight ahead the hidden body is put the same place every time: where the character stood tells them apart)
    int skinnedSeq[kArmRing] = {}, skinnedN = 0;
    bool ringHaveHide[kArmRing] = {};
    float gunHandNew[34] = {};            //   (the pose being built: gunHand once its bones are done - Hook_PoseBuild)
    float gunHand[34] = {};               //   hand in the world, how far shrunk, the eye and the way the view looks they
                                          //   were posed for, the sway and bob in it, the zoom and its spot on the
                                          //   screen - drawn as a shooter's (firstperson.cpp)
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
// The least turn that takes the way a to the way b. As two half turns - about a, then about the way halfway between them: the same turn, and steady however
// near opposite they are. (It was 1 / (1 + their cosine): ways within a degree or two of opposite, and a hair off unit length as they are after a few
// matrices, gave a turn that scaled as well - an arm solved from a pose that pointed the other way had its forearm ten times its length for a few poses.)
M3 FromTo(V3 a, V3 b) {
    a = Unit(a), b = Unit(b);
    auto half = [](V3 u) { return M3{{2 * u.x * u.x - 1, 2 * u.x * u.y, 2 * u.x * u.z, 2 * u.y * u.x, 2 * u.y * u.y - 1, 2 * u.y * u.z, 2 * u.z * u.x, 2 * u.z * u.y, 2 * u.z * u.z - 1}}; };
    V3 s = a + b;
    float l = Len(s);
    if (l < 0.02f) return half(Unit(Cross(a, fabsf(a.x) < 0.9f ? V3{1, 0, 0} : V3{0, 1, 0})));   // (opposite: half a turn about any axis square to a)
    return Mul(half(s * (1 / l)), half(a));
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
// [Camera] FirstPersonMoves - their arms on the move: 0 the aiming hold, steady; 1 the hold and the game's swing of the
// gun hand on it (g_armSwing of it; kSwingAverage s its average, at most kSwingMost - radians, for an eye 1.8 up); 2 the
// game's arms as it plays them, its carry too; 3 third person's arms all the time, standing too: the game's pose of both
// arms untouched on a steady chest, the off hand free. (Research: "fpv moves <0|1|2|3> [<swing>] [<log 0|1>]")
int g_armMoves = 0;
float g_armSwing = 0.5f;
float g_armSize = 1;   // ([Camera] FirstPersonArmSize, percent: how big their arms look - research: "fpv armsize <1 = as set>")
float g_armFlat = 1;   // ([Camera] FirstPersonArmFlat, percent: 100 their own perspective, less flatter - "fpv armflat <1 = as set>")
const float kAimLift = 0.45f;
const float kStanceEase = 0.13f, kZoomEase = 0.14f;   // (s: the gun onto a new stance, in under the crosshair)
float g_armCalm = 0.8f;   // (third person's arms on the move: how far toward the relaxed carry - "fpv calm <0..1>")
// (Peter's gun, relaxed / aiming: pitch, yaw, roll - "fpv gunturn <6 numbers>". Aiming it's turned in, as a shooter's is:
// straight ahead, flat, all that shows of it is its back end.)
// Peter's arms in the view (third person's arms), a pose for each stance - relaxed, aiming, zoomed, reloading, hands empty (a
// melee swing and after: no turn, the swing goes where the game sends it; turned, it came up from the shoulder, upside
// down, the cut of the shoulder in the view) - eased between them:
// the gun hand on the screen (right, down: the share of the way to the edge), both arms and the gun turned as one about it
// (degrees in the view: pitch, yaw, roll - the game's grip as it is, seen from the side it can be seen from: along the barrel
// the gun's drum hid the hand), the off hand on the screen, the way from its shoulder to it (degrees: 90 up, 180 right).
// "fpv pose <stance> <right> <down> <pitch> <yaw> <roll> <off x> <off y> <off way>"
// A pose: the gun hand's place on the screen (right, down), the gun's way from straight ahead and upright (pitch, yaw, roll in the view:
// -pitch up, +yaw in), the off hand's place, a bend of the gun hand's wrist if wanted, and the off arm's forearm up from ahead and to the
// right (degrees), its thumb turned from straight up to the right. In the off hand's axes the fingers are x, the thumb +z, the palm -y.
struct ArmPose { float right, down, pitch, yaw, roll, leftX, leftY, wristP, wristY, wristR, offUp, offIn, offTwist; };
const int kArmPoseFloats = 13;
bool g_offBones = false;
// The gun arm with its hands empty (a punch): aimed like the off arm - up, in, twist of the thumb, and which way the hand's z is the
// thumb. As the game plays it the arm swings past the lens: stood on end, then lying across the view with the fist sideways.
float g_punchAim[4] = {35, 25, 12, -1};   // (the right hand's a mirror of the left: fingers -x, thumb -z, palm +y - the palm faces left, thumb up)
// Where the hidden body's kept: ahead of the eye (for an eye 1.8 up), and how far down the screen it comes out, -1 its bottom edge
// (research: "fpv hidedown <y> <ahead>"). A bone that near the camera and the game draws none of the character, arms and gun with it:
// just under the view, 0.4 ahead, a punch's lunge (17 m/s, a frame's 0.28 m at 60 fps) closed the gap - both arms gone for 3-5
// frames of every punch at 60 fps, none at 120. Peter's is far below instead, as far from the eye but out of a lunge's way: the same
// punches in one launch, 33 frames of 1500 with no arms under the view, none here. (Farther ahead did it too, but what's stretched
// from his shoulders to it then lay over the sleeves.)
// A shot's kick, Peter's: the game's own is taken out zoomed in (the gun held on the crosshair) and small otherwise. Each round fired - the
// clip going down - throws the gun back and up, alternately to each side, and it comes back: how far back, up (for an eye 1.8 up), the
// barrel up and aside (degrees). "fpv kick <back> <up> <pitch> <yaw>"
float g_kick[4] = {0.16f, 0.025f, 5, 1.5f};
const float kKickIn = 0.012f, kKickOut = 0.085f;   // (s: how fast it's thrown; how fast it comes back)
bool g_shotLog = false;
float g_hideAhead = 0.4f;
// Peter's arms as a shooter's. Placed in the view at their own size, as a person's are - the hand half an arm's length ahead, the
// shoulder beside and below the eye - and seen from there, so a gun pointing straight ahead looks it: drawn flat from afar it showed
// its back and had to be leaned over to be seen, and an arm from afar shows where it ends. What's drawn is kept away from the camera
// (a bone that near and the game draws none of the character): shrunk by g_rig[4] and g_rig[5] ahead of the eye, the draw's own eye
// moved up with it (firstperson.cpp FollowX) - the same on the screen.
// How far ahead the hands are; the shoulder right, down, ahead of the eye (for an eye 1.8 up); the shrink; how far ahead it's kept.
// (his drill's as long as his body: at a person's arm's length the view was inside it. The arm's 0.58 - the shoulder comes forward
// with the hand, wide and low enough to stay off the screen. Kept within kShellNear of the eye, gun and all: its outline's split there)
// (the game's own jump is held out of the arms - below - and they went stiff through a jump and a landing: a lift of their own taking off and a dip landing, a spring)
const float kJumpLift = 1.75f, kJumpDip = 3.0f, kJumpFloat = 0.03f, kJumpSpring = 11.0f, kJumpDamp = 0.75f, kJumpPitch = 0.9f, kJumpAimed = 0.7f, kJumpMinAir = 0.12f;
float g_landHold = 0.45f;   // (s the arms stay held after a landing - research: "fpv landhold <s>". Held for as long as the relaxed stance was away, on the move they never came back: stiff until they stopped)
const float kSettleIn = 0.25f, kSettleAfterShot = 0.35f;   // (s)
const float kZoomClipEase = 0.035f;   // (s: the gun animation coming through while zoomed, and going)
// The off arm while aiming and firing (its place is fixed on the screen, none of the gun's moves reach it). In halves of the screen:
float g_offLive[6] = {
    0.016f, 0.024f,   // on the move: side to side, up and down, at a run
    0.0035f,          // standing: a breath
    0.95f,            // a shot's push on the spring
    5.0f, 2.2f,       // a shot: degrees of pitch, of roll, for the spring at 0.03
};
const float kOffKickSpring = 24, kOffKickDamp = 0.42f, kOffKickBack = 0.6f, kOffBreathHz = 0.3f, kOffZoomed = 0.6f, kOffKickMostV = 1.6f, kOffStride = 4.4f;
bool g_clip10Full = true;   // (research: "fpv clip10full 0")
bool g_clip10Held = true;   // (research: "fpv clip10 0")
int g_reloadLog = 0;   // (research: "fpv reloadlog <n>" - where the hands are on the screen, the next n poses of a reload)
int g_jumpLog = 0;   // (research: "fpv jumplog <n>" - the next n poses' jump state)
const float kAirLess = 0.85f;   // (in the air: the game's arms flung about, the gun down at the bottom of the view)
const float kPunchLess = 0.3f, kActionLess = 0.8f;   // (a punch's; an upper-body action's - a power-up used, a weapon changed: the gun flung over the view)
// (how much of the game's own move is taken out: of a reload's from the gun - the off hand keeps its work on the gun, the two moved
// as one - and of the walk's from the off hand, which it swung off the bottom of the view. Research: "fpv less <reload> <walk>")
float g_reloadLess = 0.9f, g_walkLess = 0.7f;
const float kOffFollow = 0.25f;   // (hands empty the off arm's hold is where it's been lately: the game holds it elsewhere then)
// Columns a, b square to it, the third; and an arm's forearm and thumb aimed in the view (left, up, ahead) - `inward` to which way
// its hand comes across: -1 the left arm (to the right), 1 the right.
M3 FrameOf(V3 a, V3 b) {
    V3 c = Unit(Cross(a, b)), d = Cross(c, a);
    return M3{{a.x, d.x, c.x, a.y, d.y, c.y, a.z, d.z, c.z}};
}
void ArmAim(float upDeg, float inDeg, float twistDeg, float inward, V3* t1, V3* t2) {
    float up = upDeg * 0.0174533f, in = inDeg * 0.0174533f, tw = twistDeg * 0.0174533f;
    *t1 = {inward * sinf(in) * cosf(up), sinf(up), cosf(in) * cosf(up)};
    V3 tUp = Unit(V3{0, 1, 0} - *t1 * t1->y), tRight = Unit(Cross(*t1, tUp));
    *t2 = tUp * cosf(tw) - tRight * (inward * sinf(tw));
}
const float kReloadEase = 0.08f, kEmptyEase = 0.06f, kEmptyDownEase = 0.14f, kAirEase = 0.03f, kAirOut = 0.12f;
const float kSwingSpeed = 1.2f;   // (their gun hand moving this fast against the chest, a second, in their size: a swing -
const float kSwingFor = 0.3f;     //  and for this long after it. What a swing is once the gun's gone: no action says so)
bool g_swingLog = false;
const float kSwingAverage = 0.35f, kSwingMostTurn = 0.25f, kSwingMostMove = 0.08f, kSwingSettle = 0.3f;
const float kArmNearest = 0.4f;    // (...but none of their bones nearer the eye than this: at 0.22 Brian aiming wasn't drawn)
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
// A weapon's own holds: the game poses each its own way (a minigun two-handed, low) and what's drawn is built off the stance's hold - with the
// first weapon's, every other one came out by the whole difference, the minigun on its side with the forearm across the view.
struct WeaponHold {
    bool have[2] = {false, false};
    float chestQ[2][4], rootQ[2][4], relaxRootQ[2][4];
    V3 chestAt[2], hand[2], rootAt[2], relaxRootAt[2];
    std::vector<float> aimRot, aimPos, relaxRot, relaxPos;
};
const unsigned long long kFirstWeapon = 0x0000014a00000004ull;   // (Peter's: the Nail Gun, what the hold kept between sessions is)
bool g_weaponHolds = true;   // (research: "fpv weaponholds 0")
bool g_aimCarry = true;      // (research: "fpv aimcarry 0")
// (a two-handed weapon's draw: from behind the back, up over the head and down to the hip. At its full size both arms stand in the middle of the view, the sleeve
// a third of the screen for a fifth of a second: this much less of it - it comes in tipped up from the side. "fpv drawless <0..1>")
float g_drawLess = 0.6f;
float g_gripOn = 1;   // (research: "fpv gripon 0")
// Hands empty - a melee swing, and after it until they aim or fire. The game's punch is a haymaker: the fist up by the head, round and across the body to the
// other hip, the whole body lunging - and then the arms hang at the sides. From the eye none of that reads: the arms as they hang are under the view (the gun
// arm was lowered out of it, the off hand left low in the corner over the gauge), the fist beside the head is behind the eye, and at its reach it's below the
// bottom edge - what showed of a punch was a forearm standing in the view. So both fists are held up in the view, a guard, and each goes from there by what
// the game's hand does from where it hangs, less of it: the punch up at the side, out to the middle, back. The arms come up bent from under the corners (his
// arm's 0.6 long for an eye 1.8 up: a shoulder farther from the fist than that is drawn after it, the sleeve up into the view); thumbs up, fingers curled.
// How far out and down the fists are held (of the view's half) and how far ahead; how much of the game's gun hand's move across, up and ahead; each
// shoulder's place from the eye (out, down, ahead); the thumb's lean in; the fingers' curl (base, tip); how much of the off hand's move (it comes up across
// the chest in the game: over the fist that's punching, here); how long the hands take to follow the game's (its punch starts from one pose to the next:
// the fist up by the head at once). ("fpv fist <16 values>", "fpv fists 0")
float g_fist[16] = {0.5f, 0.62f, 1.0f, 0.25f, 0.35f, 0.2f, 0.55f, 0.62f, 0.72f, 0.25f, 75, 85, 0.2f, 0.2f, 0.3f, 0.03f};
bool g_fists = true;
float g_offUpper[4] = {0.2f, -1, -0.35f, 0.12f};   // (a two-handed weapon's off arm: its upper arm's way from the elbow, in the view - left, up, ahead - and how far under the view's edge its end is. "fpv offupper")
int g_weaponLog = 0;
// A weapon's own look in the view. The two-handed ones - the rocket launcher, the chaingun, the Power Driller: carried at the hip, both hands on the handle along
// the top - are one piece: the off arm goes with the gun arm as the game has the two, through a reload and the draw as well, the game's moves at their full size.
// Their poses: where the gun hand is on the screen and how the weapon points, held and zoomed in ("fpv wpose <which> <0 held | 1 zoomed> <right> <down> <pitch> <yaw> <roll>").
// How far ahead its gun hand is (for an eye 1.8 up; the rear grip - the off hand's on the handle 0.85 farther on), where the gun arm comes from (its shoulder
// from its hand, in the view: left, up, ahead), and the off arm's elbow from its hand. ("fpv wrig <which> <ahead> <gun arm: left up ahead> <off elbow: left up ahead>")
// The off arm: in third person its forearm lies across the belly, from the left, to the handle - the game stretches it 1.7 times to get there (its bones' places).
// Solved from a shoulder near the hand it came out a stump on the weapon under a sleeve a third of the screen: the wrist bent back on itself, the hand hidden.
// Its elbow is put down by the screen's lower left instead, off its edge, and the forearm stretched from there to the hand the way the game stretches it: a bare
// forearm in from the corner to the handle, the sleeve out of the view.
// The off hand's grip. In the game's pose it lies on the weapon ahead of the handle frame, fingers along it, palm down - from behind, a stump on the weapon:
// nothing of a hand to see, nothing held. In the view it's on the handle instead, across it: fingers over the top and down the far side, the back of the hand to
// the eye. Its place from where the game has it (the weapon's axes: left, up, along it; for an eye 1.8 up), its turn from lying straight across (degrees: about
// the weapon's up, the fingers tipped down, rolled about them), the fingers' curl (base, tip). While the game has the hand at its hold: off the weapon - a reload -
// it's the game's. ("fpv wgrip <which> <left> <up> <along> <turn> <tip> <roll> <curl> <curl at the tips>")
struct WeaponLook { unsigned long long id; bool twoHand; ArmPose hold, zoomed; float rig[7]; float grip[8]; };
WeaponLook g_weaponLooks[] = {
    {0x0000014a00000009ull, true, {0.86f, 1.25f, -9, 22, 4, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40}, {0.6f, 1.2f, -8, 15, 2, -0.6f, -0.72f, 0, 0, 0, 35, 25, 40}, {1.1f, -0.25f, -0.3f, -0.4f, 0.45f, 0.2f, -0.08f}, {0.2f, -0.14f, -0.2f, 0, 40, 0, 45, 80}},   // the rocket launcher
    {0x0000014a00000020ull, true, {0.86f, 1.25f, -9, 22, 4, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40}, {0.6f, 1.2f, -8, 15, 2, -0.6f, -0.72f, 0, 0, 0, 35, 25, 40}, {1.1f, -0.25f, -0.3f, -0.4f, 0.45f, 0.2f, -0.08f}, {0.2f, -0.14f, -0.2f, 0, 40, 0, 45, 80}},   // the chaingun
    {0x0000014a00000152ull, true, {0.86f, 1.25f, -9, 22, 4, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40}, {0.6f, 1.2f, -8, 15, 2, -0.6f, -0.72f, 0, 0, 0, 35, 25, 40}, {1.1f, -0.25f, -0.3f, -0.4f, 0.45f, 0.2f, -0.08f}, {0.2f, -0.14f, -0.2f, 0, 40, 0, 45, 80}},   // the Power Driller
};
const WeaponLook* LookOf(unsigned long long id) {
    for (const WeaponLook& l : g_weaponLooks) if (l.id == id) return &l;
    return nullptr;
}
// A character's arms as a shooter's (third person's arms, FirstPersonMoves 3), by their skeleton's size: the poses for their stances (relaxed, aiming, zoomed,
// reloading, hands empty swinging, hands empty), the rig (how far ahead the hands are; the shoulder right, down, ahead of the eye, for an eye 1.8 up; the
// shrink; how far ahead it's kept), the gun's axes in the gun hand ("fpv vmlog 1": "gun in hand", aiming), how far down the hidden body's kept.
// Peter: his fingers shaped, fists with his hands empty, each weapon its own hold. A first weapon held in both hands (Lois's) has a look of its own, as
// Peter's hip weapons have - `plainOff`: the off arm as the game has it on the gun, from a shoulder of its own under the view (the look's rig: the gun
// arm's shoulder from its hand, then the off arm's from its hand - left, up, ahead), nothing reshaped.
// `asGame` (Lois): the game's own animation, all of it. One fixed turn takes her arms from her chest to the view - the one that points the gun ahead when she
// aims - and everything else is what the game plays against her chest: the carry she runs with (the gun lowered across her), the run's swing, the gun
// animation on the 1 key, a reload, the irons, a jump. Nothing calmed, none of it taken out, no bob or kick of the view's own on top. Each arm keeps the
// game's elbow and wrist; from the elbow its upper arm goes down out of the view (the look's rig: the way from the elbow, in the view - left, up, ahead -
// for the gun arm, then the off arm). g_asGame: research, "fpv asgame <calm 0..1> <less 0..1> <bob 0|1> <strafe calm 0|1>" - how much of the old way back.
struct CharRig { int bones; ArmPose pose[6]; float rig[6]; M3 gunInHand; float hideDown; bool fingers, fists, weapons, plainOff; WeaponLook first; bool asGame; };
float g_asGame[4] = {0, 0, 0, 0};
CharRig g_charRigs[] = {
    {59, {{0.55f, 0.75f, -6, 24, -4, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40},
          {0.55f, 0.75f, -6, 24, -4, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40},
          {0.42f, 0.9f, -8, 20, -2, -0.6f, -0.72f, 0, 0, 0, 35, 25, 40},   // (zoomed: the off hand in the view, at the lower left)
          {0.5f, 0.65f, -6, 24, -4, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40},
          {0.62f, 0.6f, 0, 0, 0, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40},
          {1.1f, 2.6f, 0, 0, 0, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40}},   // (hands empty, not swinging: the gun arm down out of the view)
     {1.3f, 0.5f, 0.5f, 0.9f, 0.1f, 1.2f}, {{-0.0271f, 0.0270f, -0.9993f, 0.9292f, -0.3680f, -0.0351f, -0.3686f, -0.9295f, -0.0152f}}, -3.43f, true, true, true, false, {}, false},   // Peter
    {50, {{0.66f, 0.9f, -1, 2, 0, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40},
          {0.66f, 0.9f, -1, 2, 0, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40},
          {0.34f, 0.8f, -1, 1, 0, -0.6f, -0.72f, 0, 0, 0, 35, 25, 40},
          {0.66f, 0.9f, -1, 2, 0, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40},
          {0.36f, 0.35f, 0, 0, 0, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40},   // (hands empty: an iron in each, as the game swings and carries them - her chest
          {0.36f, 0.35f, 0, 0, 0, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40}},  //  under the middle of the view, raised till both are in it)
     {1.0f, 0.45f, 0.5f, 0.8f, 0.1f, 1.2f}, {{0.0124f, -0.0016f, -0.9999f, 0.9999f, 0.0002f, 0.0124f, 0.0002f, -1.0f, 0.0016f}}, -3.43f, false, false, false, true,
     // (her gun straight ahead from the lower right - its stock to the eye, the drum in her off hand - and under the crosshair zoomed in)
     {0, true, {0.66f, 0.9f, -1, 2, 0, -0.62f, -0.8f, 0, 0, 0, 35, 25, 40}, {0.34f, 0.8f, -1, 1, 0, -0.6f, -0.72f, 0, 0, 0, 35, 25, 40}, {1.0f, -0.35f, -1.0f, -0.45f, 0.35f, -1.0f, -0.45f}, {0, 0, 0, 0, 0, 0, 0, 0}}, true},   // Lois
};
CharRig* g_rigEdit = &g_charRigs[0];   // (whose the research commands change: player 0's, as they play)
CharRig* RigOf(int bones) {
    for (CharRig& r : g_charRigs) if (r.bones == bones) return &r;
    return nullptr;
}
// (a weapon's holds in fpv_holds.dat: the character's key, and the weapon's id on top - the first weapon's the character's own)
int WeaponKey(int base, unsigned long long id) { return !id || id == kFirstWeapon ? base : base + (int)(((unsigned)id & 0x7fff) << 16); }
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
    V3 poseHideM = {0, 0, 0};             // (the same, in the character's own space)
    V3 poseHide = {0, 0, 0};              // (where their hidden body was put, in the world: by the eye and the way it looks - MotionViewmodelOf)
    bool haveHide = false;
    V3 poseEye = {0, 0, 0}, poseOrg = {0, 0, 0}, poseHand = {0, 0, 0}, poseFw = {0, 0, 1};   // (research: the trace)
    double poseAt = 0;
    float stillFor = 0, moving = 1;       // (s stood still; 0..1 on the move - eased)
    float lastArmS = 0;                   // (research: armswhy)
    float leftFree = 1, leftS = 0;        // (Peter's off hand: 0..1 free of the gun; the stance it's placed for, eased)
    float offHoldQ[4] = {};               // (...hands empty: the turn that aims it and where its hand's been lately)
    V3 offHoldAt = {0, 0, 0};
    bool haveOffHold = false;
    int rounds = 0;                       // (the clip, for each shot's kick; how far kicked, where it's going, which way aside)
    bool haveRounds = false;
    float kickNow = 0, kickTo = 0, kickSide = 1;
    float actionNow = 0;                  // (an upper-body action going on, eased)
    float offFixQ[4] = {};                // (...what's between its turn with the gun and that one, at rest)
    V3 offFixAt = {0, 0, 0};
    bool haveOffFix = false;
    float stanceNow = 0, zoomNow = 0;     // (third person's arms: the aiming stance, the zoom - eased)
    float reloadNow = 0, emptyNow = 0;    // (Peter: reloading, hands empty - eased)
    float emptyDown = 0, swingLeft = 0;   // (...the empty gun arm lowered, eased; how long a swing's still counted)
    float airNow = 0;                     // (in the air - eased)
    float jumpF = 0, jumpV = 0, jumpNow = 0, airFor = 0;   // (the arms' own lift on a jump and dip on landing: a spring, parts of half the screen's height)
    bool wasAir = false;
    float landFor = 0;                    // (s since they left the air)
    float settleFor = 0;                  // (s of clip 10)
    float sinceShot = 99;                 // (s since their last round)
    float withNow = 0;                    // (research: how far the off arm goes with the gun arm)
    float zoomClipNow = 0;                // (0..1: the gun animation on the 1 key let through while zoomed)
    unsigned long long weapon = 0;        // (the weapon in hand)
    std::map<unsigned long long, WeaponHold> holds;
    bool pending[2] = {false, false};     // (this weapon's hold for the stance not seen yet)
    float clip11For = 0;                  // (s in the aiming clip)
    float drawNow = 0;                    // (0..1: a two-handed weapon's draw playing)
    M3 gunInHandNow = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};   // (the weapon's axes in the gun hand's, this pose)
    V3 fistRest[2] = {{0, 0, 0}, {0, 0, 0}};   // (hands empty: where the game carries each hand, from the eye - to the body's left, up, ahead of it)
    bool haveFistRest = false;
    float fistPunch = 0;   // (a punch on, and this long after it)
    float fistUp = 0;      // (the fists up into the view, eased)
    V3 fistD[2] = {{0, 0, 0}, {0, 0, 0}};   // (how far each is from where it hangs, eased)
    float gripNow = 0;                    // (0..1: the off hand on the handle, ours)
    M3 firstHold = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};   // (the first weapon's aiming hand in the view's axes)
    bool haveFirstHold = false;
    float offKickF = 0, offKickV = 0;     // (the off arm's spring: a shot rocks it)
    float offBreath = 0, offPhase = 0;    // (where in a breath it is; in its own stride - slower than the head's bob)
    V3 lastHand = {0, 0, 0};              // (...the gun hand against the chest at the last pose)
    float lastAim = 0;                    // (the aiming stance last pose; the gun as shown, and what's easing out of it)
    bool haveShown = false;
    M3 shownR;
    V3 shownP = {0, 0, 0}, offR = {0, 0, 0}, offP = {0, 0, 0};
    std::vector<float> relaxRot, relaxPos;   // (the arms' bones in the relaxed carry, and where both arms hang on the chest)
    float relaxRootQ[2][4];
    V3 relaxRootAt[2];
    V3 swingAvgR = {0, 0, 0}, swingAvgP = {0, 0, 0};   // (the game's gun hand off the hold, over a stride - its turn, place)
    bool haveSwing = false;
    V3 swingR = {0, 0, 0}, swingP = {0, 0, 0};   // (the swing given the gun hand, eased)
    float swingFor = 0;                   // (s on the move)
    double swingLogAt = 0;
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
    bool haveRelax = false;   // (the relaxed carry: third person's arms hang on it - after the aiming holds in the file)
    float relaxChestQ[4], relaxRootQ[2][4];
    V3 relaxChestAt, relaxHand, relaxRootAt[2];
    std::vector<float> relaxRot, relaxPos;
};
std::map<int, AimMemory> g_aimMemory;
bool g_holdsLearn = false;   // (research: "fpv holdslearn 1" - a weapon's hold taken again and written, each time it's taken standing steady)
volatile bool g_aimDirty = false;
const DWORD kHoldsMagic = 0x31485046;   // "FPH1"
const DWORD kRelaxMagic = 0x31525046;   // "FPR1"

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
    DWORD relaxed = 0;
    for (auto& e : g_aimMemory) relaxed += e.second.haveRelax;
    put(&kRelaxMagic, 4);
    put(&relaxed, 4);
    for (auto& e : g_aimMemory) {
        const AimMemory& m = e.second;
        if (!m.haveRelax) continue;
        int key = e.first, nr = (int)m.relaxRot.size(), np = (int)m.relaxPos.size();
        put(&key, 4);
        put(m.relaxChestQ, sizeof(m.relaxChestQ));
        put(m.relaxRootQ, sizeof(m.relaxRootQ));
        put(&m.relaxChestAt, sizeof(V3));
        put(&m.relaxHand, sizeof(V3));
        put(m.relaxRootAt, sizeof(m.relaxRootAt));
        put(&nr, 4);
        put(m.relaxRot.data(), nr * sizeof(float));
        put(&np, 4);
        put(m.relaxPos.data(), np * sizeof(float));
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
    DWORD relaxed = 0;
    if (!get(&magic, 4) || magic != kRelaxMagic || !get(&relaxed, 4)) return;   // (an older file: the aiming holds only)
    for (DWORD i = 0; i < relaxed && i < 64; ++i) {
        int key = 0, nr = 0, np = 0;
        AimMemory r;
        if (!get(&key, 4) || !get(r.relaxChestQ, sizeof(r.relaxChestQ)) || !get(r.relaxRootQ, sizeof(r.relaxRootQ)) ||
            !get(&r.relaxChestAt, sizeof(V3)) || !get(&r.relaxHand, sizeof(V3)) || !get(r.relaxRootAt, sizeof(r.relaxRootAt)) ||
            !get(&nr, 4) || nr < 0 || nr > 1024) return;
        r.relaxRot.resize(nr);
        if (!get(r.relaxRot.data(), nr * sizeof(float)) || !get(&np, 4) || np < 0 || np > 768) return;
        r.relaxPos.resize(np);
        if (!get(r.relaxPos.data(), np * sizeof(float))) return;
        AimMemory& m = g_aimMemory[key];
        memcpy(m.relaxChestQ, r.relaxChestQ, sizeof(m.relaxChestQ));
        memcpy(m.relaxRootQ, r.relaxRootQ, sizeof(m.relaxRootQ));
        m.relaxChestAt = r.relaxChestAt;
        m.relaxHand = r.relaxHand;
        m.relaxRootAt[0] = r.relaxRootAt[0];
        m.relaxRootAt[1] = r.relaxRootAt[1];
        m.relaxRot = r.relaxRot;
        m.relaxPos = r.relaxPos;
        m.haveRelax = true;
    }
    Log(L"Motion: %lu characters' relaxed carries loaded", relaxed);
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
// Where an arm's elbow goes, its shoulder at S and its hand at T: S nearer the hand if the arm wouldn't reach, farther if it's too near to fold.
bool ArmElbow(float la, float lb, V3* S, V3 T, V3 pole, V3* E) {
    V3 d = T - *S;
    float L = Len(d), most = (la + lb) * 0.995f, least = fabsf(la - lb) + 0.25f * (la + lb);
    if (L < 1e-4f) return false;
    if (L > most) *S = T - d * (most / L), L = most;
    else if (L < least) *S = T - d * (least / L), L = least;
    V3 u = Unit(T - *S);
    float ca = (la * la + L * L - lb * lb) / (2 * la * L);
    ca = ca > 1 ? 1 : (ca < -1 ? -1 : ca);
    float sa = sqrtf(1 - ca * ca);
    V3 pr = Unit(pole - u * Dot(pole, u));
    *E = *S + u * (la * ca) + pr * (la * sa);
    return true;
}
// An arm solved from its shoulder to its hand: the hand stays where it is, turned as it is; the shoulder goes to S - nearer the hand
// if the arm wouldn't reach, so nothing's ever stretched - and the elbow bends toward `pole`. Forearm and upper arm keep the twist the
// game's pose has between them and the hand, so only the joints bend. `straight`: how much the hand lines up with the forearm.
void ArmIk(const unsigned char* parent, int n, const Arm& a, int root, V3 S, V3 pole, float straight, float* rot, float* pos, M3* R, V3* at, const V3* handAt = nullptr) {
    int sh = a.shoulder, el = a.elbow, h = a.hand, up = parent[root];
    V3 T = handAt ? *handAt : at[h];
    M3 Hr = R[h];
    float la = Len(at[el] - at[sh]), lb = Len(at[h] - at[el]);
    if (la < 1e-4f || lb < 1e-4f) return;
    M3 Lf = Mul(Transposed(R[el]), R[h]), Lu = Mul(Transposed(R[sh]), R[el]);
    V3 axF = Mul(Transposed(R[el]), Unit(at[h] - at[el])), axU = Mul(Transposed(R[sh]), Unit(at[el] - at[sh]));
    V3 axH = Mul(Transposed(R[h]), Unit(at[h] - at[el]));
    V3 E;
    if (!ArmElbow(la, lb, &S, T, pole, &E)) return;
    V3 fd = Unit(T - E), ud = Unit(E - S);
    if (straight > 0.001f) {
        float q0[4], q1[4];
        ToQuat(Hr, q0);
        ToQuat(Mul(FromTo(Mul(Hr, axH), fd), Hr), q1);
        Toward(q0, q1, straight > 1 ? 1 : straight, q0);
        Hr = FromQuat(q0);
    }
    M3 F0 = Mul(Hr, Transposed(Lf)), Rf = Mul(FromTo(Mul(F0, axF), fd), F0);
    M3 U0 = Mul(Rf, Transposed(Lu)), Ru = Mul(FromTo(Mul(U0, axU), ud), U0);
    V3 l = Mul(Transposed(R[up]), at[root] + (S - at[sh]) - at[up]);
    pos[(root + 1) * 3] = l.x;
    pos[(root + 1) * 3 + 1] = l.y;
    pos[(root + 1) * 3 + 2] = l.z;
    Posed(parent, n, rot, pos, R, at);
    ToQuat(Mul(Transposed(R[parent[sh]]), Ru), rot + (sh + 1) * 4);
    Posed(parent, n, rot, pos, R, at);
    ToQuat(Mul(Transposed(R[parent[el]]), Rf), rot + (el + 1) * 4);
    Posed(parent, n, rot, pos, R, at);
    ToQuat(Mul(Transposed(R[parent[h]]), Hr), rot + (h + 1) * 4);
    Posed(parent, n, rot, pos, R, at);
}
// Peter's fingers, shaped. The game's hands are made to be seen small: its relaxed hand has each finger curled its own way, its grip the
// middle and ring fingers at 50 degrees and the first straight - this close they looked crooked. So the three curl together. His hand's
// bones: the hand, then a finger's two bones three times, the thumb's two, the palm; in the hand's axes the left's fingers are +x, its
// palm -y, its thumb +z, the right's the mirror of that. Curl of the first bone, then of the second on it, relaxed and on the grip; each
// finger more than the one before, and how far they fan out. (All alike and side by side they closed up into a mitten: nothing between
// them to see. A punch keeps the game's own fist.)
float g_fingers[6] = {30, 38, 35, 25, 12, 0.2f};
// (the grip's thicker than his fingers can go round: curled more, or left as the game has them, the tips came out through its far side and the thumb went through it into the gun. So the fingers lie round its front edge, the thumb along its outside. In the left hand's axes, mirrored)
V3 g_gripThumb[2] = {{0.8f, -0.05f, 0.6f}, {0.95f, -0.05f, 0.3f}};
void FingerBend(const unsigned char* parent, int hand, int base, int tip, V3 d1, V3 d2, float w, float* rot, const float* pos, const M3* R) {
    V3 way = Unit(V3{pos[(tip + 1) * 3], pos[(tip + 1) * 3 + 1], pos[(tip + 1) * 3 + 2]});   // (along the finger, in its bones' axes)
    M3 Rb = Mul(FromTo(Mul(R[base], way), Mul(R[hand], Unit(d1))), R[base]);
    M3 Rt = Mul(FromTo(Mul(R[tip], way), Mul(R[hand], Unit(d2))), R[tip]);
    float q[4];
    ToQuat(Mul(Transposed(R[parent[base]]), Rb), q);
    Toward(rot + (base + 1) * 4, q, w, rot + (base + 1) * 4);
    ToQuat(Mul(Transposed(Rb), Rt), q);
    Toward(rot + (tip + 1) * 4, q, w, rot + (tip + 1) * 4);
}
// `mirror`: 1 the left hand, -1 the right. `thumb`: its two bones' ways in the left hand's axes, or none: as the game has it.
void HandShape(const unsigned char* parent, int n, int hand, float mirror, float base, float tip, float fan, const V3* thumb, float w, float* rot, float* pos, M3* R, V3* at) {
    if (w < 0.001f) return;
    w = w > 1 ? 1 : w;
    for (int f = 0; f < 3; ++f) {
        float more = (f - 1) * g_fingers[4], out = (1 - f) * fan;   // (the first finger the thumb's side: +z)
        auto curled = [&](float deg) { float r = deg * 0.0174533f; return V3{mirror * cosf(r), -mirror * sinf(r), mirror * out}; };
        FingerBend(parent, hand, hand + 1 + 2 * f, hand + 2 + 2 * f, curled(base + more), curled(base + tip + 2 * more), w, rot, pos, R);
    }
    if (thumb) FingerBend(parent, hand, hand + 7, hand + 8, thumb[0] * mirror, thumb[1] * mirror, w, rot, pos, R);
    Posed(parent, n, rot, pos, R, at);
}
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
    if (g_weaponLog > 0 && bones == 59 && !IsBadReadPtr(ent, 0x1d8)) {   // (research: Peter's hands as the game poses them, third person too)
        auto ptr = [](const unsigned char* p0, int off) -> const unsigned char* { return p0 && !IsBadReadPtr(p0 + off, 4) ? *reinterpret_cast<const unsigned char* const*>(p0 + off) : nullptr; };
        const unsigned char* par = ptr(ptr(ptr(ptr(ent, 0x1d4), 0x80), 0x283c), 0);
        if (par && !IsBadReadPtr(par, 58)) {
            std::vector<M3> R2(58);
            std::vector<V3> P2(58);
            Posed(par, 58, rot, pos, R2.data(), P2.data());
            auto len = [&](int b) { return Len(V3{pos[(b + 1) * 3], pos[(b + 1) * 3 + 1], pos[(b + 1) * 3 + 2]}); };
            V3 rel = Mul(Transposed(R2[40]), P2[24] - P2[40]);
            Log(L"Diag: any view - hands %.3f apart; the off hand in the gun hand's axes %.3f %.3f %.3f; off arm's bones 21 %.3f 22 %.3f 23 %.3f 24 %.3f; gun arm's 37 %.3f 38 %.3f 39 %.3f 40 %.3f",
                Len(P2[24] - P2[40]), rel.x, rel.y, rel.z, len(21), len(22), len(23), len(24), len(37), len(38), len(39), len(40));
        }
    }
    if (!g_armsOn || !FirstPersonArms(slot, &view) || view.weight <= 0) {
        if (g_weaponLog > 0) --g_weaponLog;
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
    if (g_weaponLog > 0) Log(L"Diag: entity rows %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f at %.4f %.4f %.4f", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
    V3 e = V3{view.eye[0], view.eye[1], view.eye[2]} - org, f = {view.forward[0], view.forward[1], view.forward[2]};
    V3 eye = {Dot(e, ax[0]), Dot(e, ax[1]), Dot(e, ax[2])}, F = Unit({Dot(f, ax[0]), Dot(f, ax[1]), Dot(f, ax[2])});
    V3 Rt = Unit(Cross(F, {0, 1, 0})), U = Cross(Rt, F);   // (the view's right and up; the character's right is -x)
    // (the character's size - from their eye's height standing in first person, not aiming, kept: the eye goes lower
    // aiming, Stewie's by a third, and the arms came nearer with it; still going into first person it's higher -
    // Quagmire's arms came out small)
    // (and all the way relaxed: easing back from aiming the eye is still low - Peter's size came out 0.80, not 0.98)
    // (and the highest seen: pressing to aim the eye starts down before the stance does - Brian's 0.71 came out 0.53)
    if (w >= 0.999f && view.still && view.aim < 0.01f && view.rest > 0.98f && eye.y / 1.8f > st.k) st.k = eye.y / 1.8f;
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
    V3 rawHand[2] = {place[a.hand], place[a2.hand]};   // (as the game has them this pose)
    float rawRot[2][8][4], rawPos[2][8][3];   // (...and each arm's bones' own turns and places, from the hand up to the shoulder's)
    int rawBones[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const Arm& arm = i ? a2 : a;
        for (int c = arm.hand; c >= 0 && c < n && c != arm.shoulder && parent[c] != 0xff && rawBones[i] < 8; c = parent[c]) {
            memcpy(rawPos[i][rawBones[i]], pos + (c + 1) * 3, sizeof(float) * 3);
            memcpy(rawRot[i][rawBones[i]++], rot + (c + 1) * 4, sizeof(float) * 4);
        }
    }
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
    if (!st.haveFirstHold && bones == 59) {   // (the first weapon's aiming hand, from its kept hold: what every hold taken is set against)
        auto first = g_aimMemory.find(bones * 256 + st.hands[0]);
        if (first != g_aimMemory.end() && first->second.have && first->second.rot.size() == (size_t)bones * 4) {
            const AimMemory& fm = first->second;
            M3 hr;
            V3 hp;
            HandOn(parent, roots[0], a.hand, fm.rootQ[0], fm.rootAt[0], fm.rot.data(), fm.pos.data(), &hr, &hp);
            st.firstHold = Mul(FromQuat(fm.chestQ), hr), st.haveFirstHold = true;
        }
    }
    // (a weapon changed: its holds if it's been seen, or the last weapon's until its own are - right after it's drawn the game holds it in its aiming stance a second)
    if (g_weaponHolds && bones == 59 && view.weapon && view.weapon != st.weapon) {
        auto keep = [&](WeaponHold& h) {
            for (int i = 0; i < 2; ++i) {
                h.have[i] = st.have[i] && !st.pending[i];
                memcpy(h.chestQ[i], st.chestQ[i], sizeof(h.chestQ[i])); memcpy(h.rootQ[i], st.rootQ[i], sizeof(h.rootQ[i])); memcpy(h.relaxRootQ[i], st.relaxRootQ[i], sizeof(h.relaxRootQ[i]));
                h.chestAt[i] = st.chestAt[i], h.hand[i] = st.hand[i], h.rootAt[i] = st.rootAt[i], h.relaxRootAt[i] = st.relaxRootAt[i];
            }
            h.aimRot = st.aimRot, h.aimPos = st.aimPos, h.relaxRot = st.relaxRot, h.relaxPos = st.relaxPos;
        };
        if (st.weapon) keep(st.holds[st.weapon]);
        auto it = st.holds.find(view.weapon);
        bool known = it != st.holds.end();
        for (int i = 0; i < 2; ++i) st.pending[i] = !known || !it->second.have[i];
        if (!st.weapon && view.weapon == kFirstWeapon) st.pending[0] = st.pending[1] = false;
        if (g_holdsLearn && view.weapon != kFirstWeapon) known = false, st.pending[0] = st.pending[1] = true;   // (research: taken afresh)
        else if (!known) {   // (kept from another session, or come with the mod: fpv_holds.dat)
            auto m = g_aimMemory.find(WeaponKey(bones * 256 + st.hands[0], view.weapon));
            if (m != g_aimMemory.end() && m->second.have && m->second.rot.size() == (size_t)bones * 4 && st.haveFirstHold && view.weapon != kFirstWeapon) {
                M3 hr;
                V3 hp;
                HandOn(parent, roots[0], a.hand, m->second.rootQ[0], m->second.rootAt[0], m->second.rot.data(), m->second.pos.data(), &hr, &hp);
                float offBy = Len(RotVec(Mul(Mul(FromQuat(m->second.chestQ), hr), Transposed(st.firstHold)))) * 57.3f;
                if (offBy > 40) {
                    Log(L"Motion: weapon %08x:%08x - its kept hold is %.0f degrees from the first weapon's: left out, taken again", (unsigned)view.weapon, (unsigned)(view.weapon >> 32), offBy);
                    m->second.have = false;
                }
            }
            if (m != g_aimMemory.end() && m->second.have && m->second.rot.size() == (size_t)bones * 4) {
                const AimMemory& am = m->second;
                memcpy(st.chestQ[1], am.chestQ, sizeof(am.chestQ));
                memcpy(st.rootQ, am.rootQ, sizeof(st.rootQ));
                st.chestAt[1] = am.chestAt, st.hand[1] = am.hand, st.rootAt[0] = am.rootAt[0], st.rootAt[1] = am.rootAt[1];
                st.aimRot = am.rot, st.aimPos = am.pos;
                st.have[1] = true, st.pending[1] = false;
                if (view.weapon != kFirstWeapon) st.pending[0] = false;   // (carried as it's aimed)
                else if (am.haveRelax && am.relaxRot.size() == (size_t)bones * 4) {
                    memcpy(st.chestQ[0], am.relaxChestQ, sizeof(am.relaxChestQ));
                    memcpy(st.relaxRootQ, am.relaxRootQ, sizeof(st.relaxRootQ));
                    st.chestAt[0] = am.relaxChestAt, st.hand[0] = am.relaxHand, st.relaxRootAt[0] = am.relaxRootAt[0], st.relaxRootAt[1] = am.relaxRootAt[1];
                    st.relaxRot = am.relaxRot, st.relaxPos = am.relaxPos;
                    st.have[0] = true, st.pending[0] = false;
                }
                if (g_shotLog) Log(L"Motion: weapon %08x:%08x - its holds from fpv_holds.dat", (unsigned)view.weapon, (unsigned)(view.weapon >> 32));
            }
        }
        if (known) {
            const WeaponHold& h = it->second;
            for (int i = 0; i < 2; ++i) {
                if (!h.have[i]) continue;
                memcpy(st.chestQ[i], h.chestQ[i], sizeof(h.chestQ[i]));
                st.chestAt[i] = h.chestAt[i], st.hand[i] = h.hand[i];
            }
            if (h.have[1]) { st.aimRot = h.aimRot, st.aimPos = h.aimPos; memcpy(st.rootQ, h.rootQ, sizeof(st.rootQ)); st.rootAt[0] = h.rootAt[0], st.rootAt[1] = h.rootAt[1]; }
            if (h.have[0]) { st.relaxRot = h.relaxRot, st.relaxPos = h.relaxPos; memcpy(st.relaxRootQ, h.relaxRootQ, sizeof(st.relaxRootQ)); st.relaxRootAt[0] = h.relaxRootAt[0], st.relaxRootAt[1] = h.relaxRootAt[1]; }
        }
        if (g_shotLog) Log(L"Motion: weapon %08x:%08x in hand (%s)", (unsigned)view.weapon, (unsigned)(view.weapon >> 32), known ? L"seen before" : L"its holds not seen yet");
        st.weapon = view.weapon;
        st.haveRounds = false;   // (another clip: not a shot)
        st.offR = {0, 0, 0}, st.offP = {0, 0, 0}, st.haveShown = false;   // (another weapon comes in as its draw has it: not eased from where the last one was held - it rose from there first)
    }
    st.clip11For = view.clip == 11 ? st.clip11For + (dt > 0 && dt < 0.25f ? dt : 0) : 0;
    // (right after it's drawn some weapons are held another way for a moment, the game's stance saying aiming all the same - 140 degrees off where it's aimed after. All of
    // them aim within 20 degrees of the first's: anything far from that isn't its aiming hold)
    // (and the draw - the weapon's key - says aiming too, the body turned any way from the view: a hold taken then had the chest 180 degrees round, the weapon
    // backwards from then on. Only in the aiming clip, a moment into it, the body the way they look)
    bool aimPlausible = view.clip == 11 && st.clip11For > 0.25f && view.turned < 0.15f;
    if (st.haveFirstHold) {
        float q[4];
        M3 hr;
        V3 hp;
        ToQuat(Mul(Transposed(R[chest]), R[roots[0]]), q);
        HandOn(parent, roots[0], a.hand, q, Mul(Transposed(R[chest]), place[roots[0]] - place[chest]), rot, pos, &hr, &hp);
        float offBy = Len(RotVec(Mul(Mul(FromQuat(cq), hr), Transposed(st.firstHold)))) * 57.3f;
        if (g_weaponLog > 0) Log(L"Diag: hold check - clip %d for %.2f s, aim %.2f rest %.2f, turned %.2f, the game's hand %.0f deg from the first weapon's aiming hand, steady %d pending %d%d", view.clip, st.clip11For, view.aim, view.rest, view.turned, offBy, steady ? 1 : 0, st.pending[0], st.pending[1]);
        if (g_weaponLog > 0) {   // (research: both arms as the game has them, in the view - left, up, ahead, for an eye 1.8 up)
            auto vw = [&](int b) { return Mul(Transposed(V), place[b] - eye) * (1 / k); };
            V3 gs = vw(a.shoulder), ge = vw(a.elbow), gh = vw(a.hand), os = vw(a2.shoulder), oe = vw(a2.elbow), oh = vw(a2.hand), hd = st.head >= 0 ? vw(st.head) : V3{0, 0, 0}, ch = vw(chest);
            Log(L"Diag: game arms - gun: shoulder %.2f %.2f %.2f elbow %.2f %.2f %.2f hand %.2f %.2f %.2f; off: shoulder %.2f %.2f %.2f elbow %.2f %.2f %.2f hand %.2f %.2f %.2f; head %.2f %.2f %.2f chest %.2f %.2f %.2f",
                gs.x, gs.y, gs.z, ge.x, ge.y, ge.z, gh.x, gh.y, gh.z, os.x, os.y, os.z, oe.x, oe.y, oe.z, oh.x, oh.y, oh.z, hd.x, hd.y, hd.z, ch.x, ch.y, ch.z);
        }
        if (g_weaponLog > 0) {   // (research: the off arm's bones' scale in the game's pose, and where they are with it - a bone's translation is scaled by its parents')
            wchar_t line[900] = L"";
            int at2 = 0;
            for (int c = a2.hand; c >= 0 && c < n && at2 < 800; c = parent[c] == 0xff ? -1 : parent[c]) {
                float acc = 1;
                for (int q = parent[c]; q != 0xff && q >= 0 && q < n; q = parent[q]) acc *= scale[(q + 1) * 3];
                at2 += swprintf_s(line + at2, 900 - at2, L" %d: scale %.3f %.3f %.3f (parents' %.3f)", c, scale[(c + 1) * 3], scale[(c + 1) * 3 + 1], scale[(c + 1) * 3 + 2], acc);
                if (c == chest) break;
            }
            Log(L"Diag: off arm scales -%s; root entry %.3f", line, scale[0]);
        }
        aimPlausible = aimPlausible && offBy < 40;
    }
    bool sure[2] = {steady && w >= 0.999f && view.rest > 0.98f && (g_armMoves != 3 || !st.have[0]), steady && w >= 0.999f && view.aim > 0.98f && aimPlausible};
    for (int s = 0; s < 2; ++s)
        // (all the way in first person: just switched, the eye's still behind them - a hold taken then, aiming as they
        // switched, had the arms 5 times as far off, small, until it was taken again)
        // (third person's arms hang on the relaxed hold: taken once, or the arms shift each time it's taken)
        if (sure[s] ||
            (st.pending[s] && w >= 0.999f && fabsf(F.y) < 0.35f && (s ? view.aim > 0.98f && view.clip == 11 && st.clip11For > 0.25f && view.zoom < 0.01f && aimPlausible : view.rest > 0.98f && view.still))) {
            if (st.pending[s] && g_shotLog) Log(L"Motion: weapon %08x:%08x - its %s hold taken", (unsigned)view.weapon, (unsigned)(view.weapon >> 32), s ? L"aiming" : L"relaxed");
            st.pending[s] = false;
            if (!st.have[s])
                Log(L"Motion: player %d (%d bones): %s hold taken (hands %d, %d, chest %d, head %d)", slot, bones,
                    s ? L"aiming" : L"relaxed", gun, other, chest, st.head);
            if (g_armsWhyLog && st.have[s] && Len(cAt - st.chestAt[s]) > 0.05f * k)
                Log(L"Motion: player %d's %s hold taken again - chest %.2f %.2f %.2f (was %.2f %.2f %.2f), blend %.2f", slot,
                    s ? L"aiming" : L"relaxed", cAt.x, cAt.y, cAt.z, st.chestAt[s].x, st.chestAt[s].y, st.chestAt[s].z, w);
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
            if (!s) {   // (the relaxed carry too: third person's arms calm toward it on the move)
                st.relaxRot.assign(rot, rot + bones * 4);
                st.relaxPos.assign(pos, pos + bones * 3);
                for (int i = 0; i < 2; ++i) {
                    ToQuat(Mul(Transposed(R[chest]), R[roots[i]]), st.relaxRootQ[i]);
                    st.relaxRootAt[i] = Mul(Transposed(R[chest]), place[roots[i]] - place[chest]);
                }
            }
            if (s) {
                st.aimRot.assign(rot, rot + bones * 4);
                st.aimPos.assign(pos, pos + bones * 3);
                for (int i = 0; i < 2; ++i) {
                    ToQuat(Mul(Transposed(R[chest]), R[roots[i]]), st.rootQ[i]);
                    st.rootAt[i] = Mul(Transposed(R[chest]), place[roots[i]] - place[chest]);
                }
                // (another weapon's: kept for the next session when it's taken standing still, looking level, not zoomed in)
                if (g_weaponHolds && bones == 59 && st.weapon && st.weapon != kFirstWeapon && view.still && fabsf(F.y) < 0.12f && view.zoom < 0.01f) {
                    AimMemory& wm = g_aimMemory[WeaponKey(bones * 256 + st.hands[0], st.weapon)];
                    if (!wm.have || g_holdsLearn) {
                        V3 gv = Mul(Transposed(V), place[gun] - eye) * (1 / k), ov = Mul(Transposed(V), place[other] - eye) * (1 / k);
                        Log(L"Motion: weapon %08x:%08x - its aiming hold kept (hands %.2f apart; in the view, left up ahead: gun hand %.2f %.2f %.2f, off hand %.2f %.2f %.2f)", (unsigned)st.weapon, (unsigned)(st.weapon >> 32),
                                          Len(place[other] - place[gun]) / k, gv.x, gv.y, gv.z, ov.x, ov.y, ov.z);
                        wm.have = true;
                        memcpy(wm.chestQ, st.chestQ[1], sizeof(wm.chestQ));
                        memcpy(wm.rootQ, st.rootQ, sizeof(wm.rootQ));
                        wm.chestAt = st.chestAt[1], wm.hand = st.hand[1], wm.rootAt[0] = st.rootAt[0], wm.rootAt[1] = st.rootAt[1];
                        wm.rot = st.aimRot, wm.pos = st.aimPos;
                        g_aimDirty = true;
                    }
                }
            }
        }
    // (every weapon but the first - the SMG, the rocket launcher, the chaingun, the Power Driller, the saw blade thrower: the game carries them low, or turned right round, out of the
    // view, the off hand on them in the middle of it. Carried from the view as they're aimed, always)
    bool aimCarry = g_aimCarry && g_weaponHolds && bones == 59 && st.weapon && st.weapon != kFirstWeapon;
    if (aimCarry && !st.pending[1] && st.have[1] && !st.aimRot.empty()) {
        st.relaxRot = st.aimRot, st.relaxPos = st.aimPos;
        memcpy(st.relaxRootQ, st.rootQ, sizeof(st.relaxRootQ));
        st.relaxRootAt[0] = st.rootAt[0], st.relaxRootAt[1] = st.rootAt[1];
        memcpy(st.chestQ[0], st.chestQ[1], sizeof(st.chestQ[0]));
        st.chestAt[0] = st.chestAt[1], st.hand[0] = st.hand[1];
        st.have[0] = true, st.pending[0] = false;
    }
    // (a character's aiming hold, once seen this session, kept for them - a respawn, the next match)
    AimMemory& mem = g_aimMemory[bones * 256 + st.hands[0]];
    bool firstWeapon = !g_weaponHolds || !view.weapon || view.weapon == kFirstWeapon || bones != 59;
    if (st.have[1] && !st.aimRot.empty()) {
        if (!mem.have && firstWeapon) {
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
    if (st.have[0] && !st.relaxRot.empty()) {   // (the relaxed carry likewise: right from the start, no standing about first)
        if (!mem.haveRelax && firstWeapon) {
            mem.haveRelax = true;
            g_aimDirty = true;
            memcpy(mem.relaxChestQ, st.chestQ[0], sizeof(mem.relaxChestQ));
            memcpy(mem.relaxRootQ, st.relaxRootQ, sizeof(mem.relaxRootQ));
            mem.relaxChestAt = st.chestAt[0];
            mem.relaxHand = st.hand[0];
            mem.relaxRootAt[0] = st.relaxRootAt[0];
            mem.relaxRootAt[1] = st.relaxRootAt[1];
            mem.relaxRot = st.relaxRot;
            mem.relaxPos = st.relaxPos;
        }
    } else if (mem.haveRelax && g_armMoves == 3) {
        memcpy(st.chestQ[0], mem.relaxChestQ, sizeof(mem.relaxChestQ));
        memcpy(st.relaxRootQ, mem.relaxRootQ, sizeof(mem.relaxRootQ));
        st.chestAt[0] = mem.relaxChestAt;
        st.hand[0] = mem.relaxHand;
        st.relaxRootAt[0] = mem.relaxRootAt[0];
        st.relaxRootAt[1] = mem.relaxRootAt[1];
        st.relaxRot = mem.relaxRot;
        st.relaxPos = mem.relaxPos;
        st.have[0] = true;
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
    float moveOut = g_armMoves == 2 ? 0 : st.moving;   // (2: the game's own arms on the move, as it plays them)
    float heldOut = moveOut > idleOut ? moveOut : idleOut;
    float held = st.aimRot.size() == (size_t)bones * 4 && g_armMoves != 3 ? view.rest + act * heldOut : 0;
    if (held > 0) {
        for (int b = 0; b < n; ++b) {
            if (!armBone[b]) continue;
            float* q = rot + (b + 1) * 4;
            Toward(q, &st.aimRot[(b + 1) * 4], held, q);
            for (int c = 0; c < 3; ++c) pos[(b + 1) * 3 + c] += (st.aimPos[(b + 1) * 3 + c] - pos[(b + 1) * 3 + c]) * held;
        }
    }
    // (third person's arms on the move: the game's walk and run toward the relaxed carry - g_armCalm of the way, its swing
    // and its rhythm what's left of it; firing, zooming and reloading are the aiming stance, left as they are)
    // (in the air all the way: the game's jump has the arms flung about - the gun out of the view, the off hand up across it)
    // (and landing: half a second with the relaxed stance still away, its dip took the gun out of the view too)
    st.landFor = view.moveClip >= 0x14 && view.moveClip <= 0x17 ? 0 : st.landFor + stepDt;
    bool landing = st.airNow > 0.05f && view.rest < 0.5f && view.aim < 0.01f && st.landFor < g_landHold;
    // (0x16 a jump standing, 0x17 on the move; 0x14 and 0x15 the falls they go on to after 3/4 s: the arms flung out of the view,
    // the off hand turned)
    float inAir = (view.moveClip >= 0x14 && view.moveClip <= 0x17) || landing ? 1.0f : 0.0f;
    st.airNow += (inAir - st.airNow) * (stepDt > 0 ? 1 - expf(-stepDt / (inAir > 0 ? kAirEase : kAirOut)) : 1);
    CharRig* chr0 = g_armMoves == 3 ? RigOf(bones) : nullptr;
    bool asGame = chr0 && chr0->asGame;
    float calmed = act * st.moving * g_armCalm * (asGame ? g_asGame[0] : 1.0f);
    // (the idle fidget too, third person's arms: stood still it threw the off arm straight up and the gun about every few seconds -
    // the off arm and most of the gun gone at its height - and the view has no use for it. Its weight is the game's, eased)
    if (idleOut * (1 - view.aim) > calmed) calmed = idleOut * (1 - view.aim);
    calmed += ((1 - view.aim) - calmed) * st.airNow * (asGame ? g_asGame[0] : 1.0f);
    // (strafing, their body turned from the way they look: the game's sideways run has the gun turned with the chest - none of it)
    { float side = view.turned / 0.5f; calmed += ((1 - view.aim) - calmed) * (side < 1 ? side : 1) * (asGame ? g_asGame[3] : 1.0f); }
    float calm = g_armMoves == 3 && st.relaxRot.size() == (size_t)bones * 4 ? calmed : 0;
    if (calm > 0) {
        for (int b = 0; b < n; ++b) {
            if (!armBone[b]) continue;
            float* q = rot + (b + 1) * 4;
            Toward(q, &st.relaxRot[(b + 1) * 4], calm, q);
            for (int c = 0; c < 3; ++c) pos[(b + 1) * 3 + c] += (st.relaxPos[(b + 1) * 3 + c] - pos[(b + 1) * 3 + c]) * calm;
        }
    }
    // the hold for the stance they're in: aiming once it's been seen (the arms are held so, or doing the game's
    // moves from it); until then the relaxed one, between the two as the game eases from one to the other
    if (view.aim + view.rest > 0.05f) st.stance = view.aim / (view.aim + view.rest);
    float s = st.have[1] && !st.aimRot.empty() ? 1.0f : st.stance;
    // (third person's arms: hung on the chest as it stands relaxed, and aiming as it aims, by the game's own easing - or
    // firing and zooming the gun left the view)
    // (eased: the spot the arms are zoomed about goes with the stance - moved at once, the gun jumped aside as aiming began)
    // (the chest itself goes with the pose at once: eased too, the aiming pose hung off the relaxed chest, swung wide)
    float sSpot = s;
    if (g_armMoves == 3 && st.have[0]) {
        float stance = aimCarry ? 1.0f : view.aim;
        st.stanceNow += (stance - st.stanceNow) * (stepDt > 0 ? 1 - expf(-stepDt / kStanceEase) : 1);
        s = stance;
        sSpot = st.stanceNow;
    }
    int i0 = st.have[0] ? 0 : 1, i1 = st.have[1] ? 1 : 0;
    float hq[4];
    // (as the game has it: one hold for every stance - the chest and the hand as she aims. What her chest is turned by from there, about the upright, is
    // left on it: she aims with a shoulder back and carries the irons square - hung on the aiming chest the right iron was at the eye, the left far off)
    float sHold = asGame && st.have[1] ? 1.0f : s;
    Toward(st.chestQ[i0], st.chestQ[i1], sHold, hq);
    M3 H = FromQuat(hq);
    V3 hAt = st.chestAt[i0] + (st.chestAt[i1] - st.chestAt[i0]) * sHold;
    V3 hHand = st.hand[i0] + (st.hand[i1] - st.hand[i0]) * sHold;
    // where that puts the gun hand in the view, and where it should come out: the whole hold moved by the difference.
    // Their arms have a field of view of their own (kArmTan - drawn so: firstperson.cpp), so they're placed in it.
    V3 hand = hAt + Mul(H, hHand);
    M3 H0 = H;
    V3 hPivot = hHand;
    if (asGame) {
        float dq[4];
        ToQuat(Mul(R[chest], Transposed(H0)), dq);
        float l = sqrtf(dq[1] * dq[1] + dq[3] * dq[3]);
        if (l > 1e-4f) {
            float yq[4] = {0, dq[1] / l, 0, dq[3] / l};
            H = Mul(FromQuat(yq), H0);
            hPivot = Mul(Transposed(H), Mul(H0, hHand));
            if (g_weaponLog > 0) Log(L"Diag: chest turned %.1f deg from where she aims (aim %.2f rest %.2f gun %d clip %d)", 2 * atan2f(yq[1], yq[3]) * 57.2958f, view.aim, view.rest, view.gunOut ? 1 : 0, view.clip);
        }
    }
    float z = view.zoom, side = left ? -1.0f : 1.0f;
    if (g_armMoves == 3) {   // (the gun in under the crosshair slower than the game's lens: at its speed it leapt across the view)
        st.zoomNow += (z - st.zoomNow) * (stepDt > 0 ? 1 - expf(-stepDt / kZoomEase) : 1);
        z = st.zoomNow;
    }
    ArmsView av = view;
    av.tans[0] = kArmTan;
    av.tans[1] = kArmTan * view.tans[1] / view.tans[0];
    bool mine = place0 && !g_armPlaceTest;
    float right0 = mine ? place0->right : g_armRight, down0 = mine ? place0->down : g_armDown;
    float ahead0 = mine ? place0->ahead : g_armAhead;
    float zoomDown = g_armZoomDown > down0 ? g_armZoomDown : down0;   // (Death's sleeves stay low zoomed in too)
    float right = right0 + (g_armZoomRight - right0) * z, down = down0 + (zoomDown - down0) * z;
    // (third person's arms, flat: a gun pointing ahead stays at the hand, no perspective drawing it up the view - aiming,
    // the hand higher, or it's under the screen)
    if (g_armMoves == 3 && g_armFlat < 1) down *= 1 - (1 - g_armFlat) * kAimLift * sSpot * (1 - 0.6f * z);   // (zoomed less: not over the crosshair)
    CharRig* chr = g_armMoves == 3 ? RigOf(bones) : nullptr;
    bool posed = chr != nullptr;   // (Peter, Lois: their poses, by the stance as it's eased)
    if (chr && slot == 0) g_rigEdit = chr;
    ArmPose pp{};
    bool settling = false;
    const WeaponLook* look = posed && g_weaponHolds && chr->weapons ? LookOf(st.weapon) : nullptr;
    if (posed && !look && chr->first.twoHand) look = &chr->first;
    bool twoHand = look && look->twoHand;
    if (posed) {
        // (a reload is three clips: out with the old, in with the new, and back up - then 0xb, the aiming stance)
        bool reloading = view.clip >= 4 && view.clip <= 6;
        st.reloadNow += ((reloading ? 1.0f : 0.0f) - st.reloadNow) * (stepDt > 0 ? 1 - expf(-stepDt / kReloadEase) : 1);
        st.emptyNow += ((view.gunOut ? 0.0f : 1.0f) - st.emptyNow) * (stepDt > 0 ? 1 - expf(-stepDt / kEmptyEase) : 1);
        st.sinceShot += stepDt;
        if (st.haveRounds && view.rounds < st.rounds) {
            st.sinceShot = 0;
            st.offKickV = st.offKickV + g_offLive[3] > kOffKickMostV ? kOffKickMostV : st.offKickV + g_offLive[3];
            st.kickTo = st.kickTo + 1 > 1.4f ? 1.4f : st.kickTo + 1;
            st.kickSide = -st.kickSide;
            if (g_shotLog) Log(L"Motion: shot - %d rounds, aim %.2f rest %.2f clip %d gun %d", view.rounds, view.aim, view.rest, view.clip, view.gunOut ? 1 : 0);
        }
        st.rounds = view.rounds, st.haveRounds = true;
        if (stepDt > 0) {
            st.kickNow += (st.kickTo - st.kickNow) * (1 - expf(-stepDt / kKickIn));
            st.kickTo *= expf(-stepDt / kKickOut);
        }
        // (a power-up used, a weapon changed: clips 7..10. Not the action alone: firing and zooming are action 4 too, clips 11 and 2)
        bool tossing = view.clip >= 7 && view.clip <= (g_clip10Full ? 9 : 10);   // (10 is the gun animation on the 1 key: at its full strength, not a power-up's fifth of it)
        // (clip 10, the gun animation on the 1 key, about a second: its first pose has the gun pulled in at the chest. Through at full strength for the frames before the ease
        // on tossing took hold, the gun came out huge at the side of the view - and firing cuts it short, so that was all there was of it. Faded in over kSettleIn right after
        // a shot, only: pressed by itself its snap at the start is the animation - faded, it didn't feel the same)
        settling = g_clip10Held && view.clip == 10 && view.aim > 0.9f && (st.settleFor > 0 || st.sinceShot < kSettleAfterShot);
        st.settleFor = settling ? st.settleFor + stepDt : 0;
        st.actionNow += ((tossing ? 1.0f : 0.0f) - st.actionNow) * (stepDt > 0 ? 1 - expf(-stepDt / kReloadEase) : 1);
        const float* p0 = &chr->pose[0].right;
        const float* p1 = &chr->pose[1].right;
        const float* p2 = &chr->pose[2].right;
        const float* p3 = &chr->pose[3].right;
        const float* p4 = &chr->pose[4].right;
        const float* p5 = &chr->pose[5].right;
        if (look) p0 = p1 = p3 = &look->hold.right, p2 = &look->zoomed.right;
        st.swingLeft = view.gunOut ? kSwingFor : (st.swingLeft > stepDt ? st.swingLeft - stepDt : 0);
        st.emptyDown += ((st.swingLeft > 0 ? 0.0f : 1.0f) - st.emptyDown) * (stepDt > 0 ? 1 - expf(-stepDt / kEmptyDownEase) : 1);
        float* to = &pp.right;
        for (int i = 0; i < kArmPoseFloats; ++i) {
            float v = p0[i] + (p1[i] - p0[i]) * sSpot;
            v += (p2[i] - v) * z;
            v += (p3[i] - v) * st.reloadNow;
            v += (p4[i] - v) * st.emptyNow;
            to[i] = v + (p5[i] - v) * st.emptyNow * st.emptyDown;
        }
        right = pp.right, down = pp.down;
    }
    // ([Camera] FirstPersonArmFlat: farther off, at the same spot on the screen, their perspective flat - their moves look
    // as they do from afar, not swung past a lens - and shrunk as much more toward the eye below, so as near as ever: walls
    // stay out of them. Drawn zoomed about that spot, to FirstPersonArmSize: firstperson.cpp Followed.)
    float armFlat = g_armFlat > 0.05f ? g_armFlat : 1, armLook = g_armSize > 0.05f ? g_armSize : 1;
    if (posed) armFlat = armLook = 1;   // (Peter: in true perspective, g_rig)
    // (the spot they're zoomed about. Peter: the middle of the screen. The game leaves out each piece of what it draws that's
    // outside its view, from where it really is: shrunk by the eye at the hand's own spot, down in the corner, the gun's pieces
    // were at the view's edge - moving, a piece went over it for a frame, the drum gone, the gun flickering. Zoomed about
    // the middle they're well inside it, and so is the muzzle's flash, at the muzzle: it came out by the hand)
    float spotRight = right, spotDown = down;
    if (posed) {
        float zoomed = armLook / armFlat;
        right /= zoomed, down /= zoomed;
        spotRight = spotDown = 0;
    }
    float armFar = 1 / armFlat, armMove = 1 / armLook;   // (the bob and swing as far on the screen at any size)
    float depth = (hand.z > ahead0 * k ? hand.z : ahead0 * k) * armFar;
    if (posed) depth = (look ? look->rig[0] : chr->rig[0]) * k;
    V3 want = {-side * right * av.tans[0] * depth, -down * av.tans[1] * depth, depth};   // (x: to the left)
    if (posed) want = want + V3{0, g_kick[1] * k * st.kickNow, -g_kick[0] * k * st.kickNow};
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
            st.offPhase = fmodf(st.offPhase + moved / k / kOffStride * 6.2832f, 6.2832f);
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
            V3 off = Mul(Transposed(V), hd - st.headRest);   // (the view's left, up, ahead)
            V3 to = {off.x * g_gameBob[1], off.y * g_gameBob[0], 0};
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
        V3 jump = {0, 0, 0};
        if (posed) {
            bool air = view.moveClip >= 0x14 && view.moveClip <= 0x17;   // (0x16, 0x17 a jump; 0x14, 0x15 the falls after it, or off a ledge)
            if (air && !st.wasAir && view.moveClip >= 0x16) st.jumpV += kJumpLift;
            if (!air && st.wasAir && st.airFor > kJumpMinAir) {
                float hard = 0.6f + 0.6f * st.airFor;   // (the longer the fall the harder, up to a limit)
                st.jumpV -= kJumpDip * (hard > 1.2f ? 1.2f : hard);
            }
            st.airFor = air ? st.airFor + step : 0;
            st.wasAir = air;
            if (step > 0) {
                float target = air && st.airFor > 0.25f ? kJumpFloat : 0;   // (falling, they float up a little)
                st.jumpV += (kJumpSpring * kJumpSpring * (target - st.jumpF) - 2 * kJumpDamp * kJumpSpring * st.jumpV) * step;
                st.jumpF += st.jumpV * step;
            }
            st.jumpNow = st.jumpF * (1 - kJumpAimed * z);
            if (g_jumpLog > 0) { --g_jumpLog; Log(L"Diag: jump clip %x air %d airNow %.3f rest %.2f aim %.2f landFor %.2f F %.4f", view.moveClip, air ? 1 : 0, st.airNow, view.rest, view.aim, st.landFor, st.jumpF); }
            jump.y = st.jumpNow * av.tans[1] * depth;
        }
        st.offset = (st.sway + (bob + jump) * (asGame ? g_asGame[2] : 1.0f)) * armMove;
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
        if (calm > 0) {
            Toward(lq[i], st.relaxRootQ[i], calm, lq[i]);
            lAt[i] = lAt[i] + (st.relaxRootAt[i] - lAt[i]) * calm;
        }
    }
    V3 rightShift = {0, 0, 0};
    M3 gunCr = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};   // (what the gun arm's moved by; where it hung)
    V3 gunCp = {0, 0, 0}, gameAt = lAt[0];
    float gameQ[4];
    memcpy(gameQ, lq[0], sizeof(gameQ));
    // The gun hand off its aiming hold (against the chest): a shot's kick, a reload, a melee swing as the game plays
    // them - but while the game eases between its stances (its arms then between its two holds, the gun swung up to
    // 25 degrees), kept on the hold. Both arms moved together by what's kept out: the off hand goes on as it does.
    {
        M3 gR, hR;
        V3 gP, hP;
        HandOn(parent, roots[0], a.hand, lq[0], lAt[0], rot, pos, &gR, &gP);
        HandOn(parent, roots[0], a.hand, st.rootQ[0], st.rootAt[0], st.aimRot.data(), st.aimPos.data(), &hR, &hP);
        if (stepDt > 0 && Len(gP - st.lastHand) / stepDt / k > kSwingSpeed) st.swingLeft = kSwingFor;
        st.lastHand = gP;
        V3 dr = RotVec(Mul(gR, Transposed(hR))), dp = (gP - hP) * (1 / k);
        float otherHold = view.rest + act * heldOut;   // (the other hold: relaxed, or the move's swing - held out)
        float mix = g_armMoves == 3 ? 0 : 4 * view.aim * (otherHold > 1 ? 1 : otherHold) * g_devMix;   // (3: only zoomed)
        // (zoomed in, the gun held steady where it's placed - the game's own raising it to the eye, Quagmire's Tommy gun
        // stood up by his face, pointed it at the sky)
        // (but for the gun animation on the 1 key: held steady too, pressed zoomed it did nothing at all)
        st.zoomClipNow += ((view.clip == 10 && g_clip10Full ? 1.0f : 0.0f) - st.zoomClipNow) * (stepDt > 0 ? 1 - expf(-stepDt / kZoomClipEase) : 1);
        float pass = (1 - (mix > 1 ? 1 : mix)) * (1 - z * (1 - st.zoomClipNow));
        if (aimCarry) pass *= view.aim > st.emptyNow ? view.aim : st.emptyNow;   // (none of the game's carry; a reload, a shot, a punch still come through)
        V3 fr = dr * pass, fp = dp * pass;
        st.devAngle = Len(fr) * 57.3f;
        // (FirstPersonMoves=1: the game's own swing of the gun hand on the move, on the aiming hold - the hand in the
        // game's pose against its average over a stride, so its rhythm and its size, not the carry it swings about)
        V3 swingR = {0, 0, 0}, swingP = {0, 0, 0};
        {
            float rq[4];
            M3 wR;
            V3 wP;
            ToQuat(Mul(Ct, R[roots[0]]), rq);
            HandOn(parent, roots[0], a.hand, rq, Mul(Ct, place[roots[0]] - place[chest]), game.data(), gamePos.data(), &wR, &wP);
            V3 sr = RotVec(Mul(wR, Transposed(hR))), sp = (wP - hP) * (1 / k);
            // (setting off, the game's arms go from the relaxed carry to the move's: its average catches up first, quickly -
            // only then the swing, eased in; stopping, eased out)
            bool swinging = !view.still && view.aim < 0.01f && view.idle <= 0;
            st.swingFor = swinging ? st.swingFor + stepDt : 0;
            bool settled = st.swingFor > kSwingSettle;
            float eased = stepDt > 0 ? 1 - expf(-stepDt / (settled ? kSwingAverage : 0.05f)) : 0;
            if (!st.haveSwing) st.swingAvgR = sr, st.swingAvgP = sp, st.haveSwing = true;
            st.swingAvgR = st.swingAvgR + (sr - st.swingAvgR) * eased;
            st.swingAvgP = st.swingAvgP + (sp - st.swingAvgP) * eased;
            V3 toR = {0, 0, 0}, toP = {0, 0, 0};
            if (settled) {
                toR = sr - st.swingAvgR, toP = sp - st.swingAvgP;
                if (Len(toR) > kSwingMostTurn) toR = toR * (kSwingMostTurn / Len(toR));
                if (Len(toP) > kSwingMostMove) toP = toP * (kSwingMostMove / Len(toP));
                float gain = g_armSwing * (1 - z);
                toR = toR * gain, toP = toP * gain;
            }
            float ease = stepDt > 0 ? 1 - expf(-stepDt / (settled ? 0.04f : 0.12f)) : 1;
            st.swingR = st.swingR + (toR - st.swingR) * ease;
            st.swingP = st.swingP + (toP - st.swingP) * ease;
            swingR = st.swingR, swingP = st.swingP;
            if (g_swingLog && now - st.swingLogAt > 0.02) {
                st.swingLogAt = now;
                Log(L"Motion: swing %.4f %.2f deg %.4f m (carry %.1f deg, on %d)", now, Len(swingR) * 57.3f, Len(swingP),
                    Len(sr) * 57.3f, settled ? 1 : 0);
            }
        }
        if (g_armMoves == 1) fr = fr + swingR, fp = fp + swingP * armMove;
        if (g_devLog) {   // (the most the gun was off its hold in each tenth of a second)
            float shown = Len(fr) * 57.3f;
            st.devMost = shown > st.devMost ? shown : st.devMost;
            if (now - st.devLogAt > 0.1) {
                st.devLogAt = now;
                Log(L"Motion: dev action %d clip %d under %x air %.2f gun %d reload %.2f empty %.2f aim %.2f rest %.2f still %d moving %.2f idle %.2f [%d] held %.2f - hand %.3f turn %.1f deg, pass %.2f, shown most %.1f deg; depth %.2f k %.2f eye up %.2f blend %.2f",
                    view.action, view.clip, view.moveClip, st.airNow, view.gunOut ? 1 : 0, st.reloadNow, st.emptyNow, view.aim, view.rest, view.still ? 1 : 0, st.moving, view.idle, view.layer90, held, Len(dp),
                    Len(dr) * 57.3f, pass, st.devMost, depth, k, eye.y, w);
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
            if (g_armMoves == 3) turn = FromRotVec(RotVec(turn) * z);   // (third person's arms: only zoomed)
            Tr = Mul(Mul(Mul(Transposed(Cn), turn), Cn), Tr);
        }
        if (posed) {
            // (his gun turned in the view: the gun arm, all that's on it, about the gun hand)
            // (the hand's moves off its stance's place too: a reload's, turned as the gun is, stays in the view)
            // (the stance's hold, as the game eases between them, turned so the gun points as the pose says - pitch, yaw, roll from
            // straight ahead and upright; all the game does from the hold comes through on top)
            float qHold[4];
            ToQuat(hR, qHold);
            if (st.relaxRot.size() == (size_t)bones * 4) {
                M3 relR;
                V3 relP;
                float qRel[4];
                HandOn(parent, roots[0], a.hand, st.relaxRootQ[0], st.relaxRootAt[0], st.relaxRot.data(), st.relaxPos.data(), &relR, &relP);
                ToQuat(relR, qRel);
                if (!asGame) Toward(qRel, qHold, s, qHold);   // (as the game has it: relaxed, the gun's where her carry has it against her aim)
            } else {
                // (no relaxed hold yet - it's taken standing steady and looking level, 34 s into a first game: the gun 65 degrees off and
                // the hand out of the view until then. Held as it's held aiming till there is one. The game's own relaxed hand wouldn't do:
                // it's on another chest, the gun came out huge and turned)
                Tr = hR;
                Tp = hP;
            }
            // (a reload swings the gun past the view, the drill being what it is, and a punch's fist goes across it: less of them)
            // (at once as the draw starts - eased in, its first frames came through whole - and eased out after)
            st.drawNow = twoHand && view.clip == 10 ? 1.0f : st.drawNow * (stepDt > 0 ? expf(-stepDt / 0.08f) : 1.0f);
            float less = (1 - g_drawLess * st.drawNow) * (1 - (twoHand ? 0.0f : g_reloadLess) * st.reloadNow) * (1 - kPunchLess * st.emptyNow) * (1 - kActionLess * st.actionNow) * (1 - kAirLess * st.airNow) * (settling ? (st.settleFor < kSettleIn ? st.settleFor / kSettleIn : 1.0f) : 1.0f), qNow[4];
            if (asGame) less = 1 - (1 - less) * g_asGame[1];
            ToQuat(Tr, qNow);
            Toward(qHold, qNow, less, qNow);
            Tr = FromQuat(qNow);
            Tp = hHand + (Tp - hHand) * less;
            M3 Rv0 = Mul(Mul(About({0, 0, 1}, pp.roll), About({0, 1, 0}, pp.yaw + g_kick[3] * st.kickSide * st.kickNow)), About({1, 0, 0}, pp.pitch - g_kick[2] * st.kickNow - st.jumpNow * kJumpPitch));
            M3 gunInHand = chr->gunInHand;
            if (g_weaponHolds && chr->weapons) {   // (the game aims them all at the crosshair: a weapon's aiming hand against the first's is how differently it's held)
                M3 aimHand = Mul(FromQuat(st.chestQ[1]), hR);
                if (st.weapon == kFirstWeapon && !st.haveFirstHold) st.firstHold = aimHand, st.haveFirstHold = true;
                else if (st.haveFirstHold && !st.pending[1]) gunInHand = Mul(Mul(Transposed(aimHand), st.firstHold), chr->gunInHand);
                if (g_weaponLog > 0 && st.haveFirstHold) {
                    V3 diff = RotVec(Mul(aimHand, Transposed(st.firstHold)));
                    Log(L"Diag: weapon %08x clip %d aim %.2f rest %.2f zoom %.2f pending %d%d - aim hold %.0f deg from the first's, game's hand %.0f deg %.2f off, pass %.2f", (unsigned)st.weapon, view.clip, view.aim, view.rest, z, st.pending[0], st.pending[1], Len(diff) * 57.3f, Len(dr) * 57.3f, Len(dp), pass);
                }
            }
            st.gunInHandNow = gunInHand;
            M3 Rv = Mul(Rv0, Transposed(Mul(Mul(H0, FromQuat(qHold)), gunInHand)));
            M3 Rc = Mul(Mul(Transposed(H), Rv), H);
            Tr = Mul(Rc, Tr);
            Tp = hPivot + Mul(Rc, Tp - hPivot);
            // (hands empty: its forearm and thumb aimed, about the hand - its path the game's, the arm coming from the corner
            // of the view behind it. Pushed ahead as the off arm is, so the shoulder gets no nearer the eye)
            if (st.emptyNow > 0.001f && !asGame) {
                M3 eR;
                V3 eP, t1, t2;
                HandOn(parent, roots[0], a.elbow, lq[0], lAt[0], rot, pos, &eR, &eP);
                ArmAim(g_punchAim[0], g_punchAim[1], g_punchAim[2], 1, &t1, &t2);
                M3 toHand = Mul(Tr, Transposed(gR));
                V3 fore = Unit(Mul(H, Mul(toHand, gP - eP))), thumb = Mul(H, Mul(Tr, V3{0, 0, g_punchAim[3]}));
                M3 one = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
                float qNone[4], qAim[4];
                ToQuat(one, qNone);
                ToQuat(Mul(FrameOf(t1, t2), Transposed(FrameOf(fore, thumb))), qAim);
                Toward(qNone, qAim, st.emptyNow, qAim);
                Tr = Mul(Mul(Mul(Transposed(H), FromQuat(qAim)), H), Tr);
            }
        }
        M3 Cr = Mul(Tr, Transposed(gR));
        V3 Cp = Tp - Mul(Cr, gP);
        gunCr = Cr, gunCp = Cp;
        memcpy(gameQ, lq[0], sizeof(gameQ));
        gameAt = lAt[0];
        for (int i = 0; i < (g_armMoves == 3 ? 1 : 2); ++i) {   // (third person's arms: the off hand its own)
            M3 q = Mul(Cr, FromQuat(lq[i]));
            ToQuat(q, lq[i]);
            lAt[i] = Cp + Mul(Cr, lAt[i]);
        }
    }
    // (Peter, third person's arms: the off arm moved up into the view as it is - the game's moves on it, its hand in its
    // relaxed carry at its pose's place on the screen; it hangs below the view otherwise)
    V3 leftShift = {0, 0, 0};
    float offStraight = 0;
    if (posed && st.relaxRot.size() == (size_t)bones * 4) {
        // (from where the game holds it relaxed and aiming, between them as the stance eases - the aiming stance has the
        // arm elsewhere: turned by the relaxed arm's turn, it lay along the bottom of the view)
        // (aiming has nothing for the off arm to do, and the game's aiming arm is elsewhere: it jumped up at every shot. Kept as it is
        // relaxed - but for a reload, or a power-up thrown with it)
        st.leftS = 0;
        float relaxed = twoHand ? 0.0f : s * (1 - st.reloadNow) * (1 - st.actionNow);   // (both hands on it: the off arm as the game has it)
        if (relaxed > 0.001f) {
            for (int b = 0; b < n; ++b) {
                bool on = false;
                for (int c = b; c >= 0; c = parent[c] == 0xff ? -1 : parent[c]) if (c == roots[1]) { on = c != b; break; }
                if (!on) continue;
                float* q = rot + (b + 1) * 4;
                Toward(q, &st.relaxRot[(b + 1) * 4], relaxed, q);
                for (int c = 0; c < 3; ++c) pos[(b + 1) * 3 + c] += (st.relaxPos[(b + 1) * 3 + c] - pos[(b + 1) * 3 + c]) * relaxed;
            }
            Toward(lq[1], st.relaxRootQ[1], relaxed, lq[1]);
            lAt[1] = lAt[1] + (st.relaxRootAt[1] - lAt[1]) * relaxed;
        }
        M3 lr;
        V3 lp, lpAim;
        HandOn(parent, roots[1], a2.hand, st.relaxRootQ[1], st.relaxRootAt[1], st.relaxRot.data(), st.relaxPos.data(), &lr, &lp);
        HandOn(parent, roots[1], a2.hand, st.rootQ[1], st.rootAt[1], st.aimRot.data(), st.aimPos.data(), &lr, &lpAim);
        lp = lp + (lpAim - lp) * st.leftS;
        V3 lRoot = st.relaxRootAt[1] + (st.rootAt[1] - st.relaxRootAt[1]) * st.leftS;
        // (the turn that aims the forearm and the thumb: from each stance's arm, between them as it eases)
        V3 t1, t2;
        ArmAim(pp.offUp, pp.offIn, pp.offTwist, -1, &t1, &t2);
        auto aimed = [&](const float* rq, V3 rAt, const float* r, const float* p, float* q) {
            M3 hr, er;
            V3 hp, ep;
            HandOn(parent, roots[1], a2.hand, rq, rAt, r, p, &hr, &hp);
            HandOn(parent, roots[1], a2.elbow, rq, rAt, r, p, &er, &ep);
            ToQuat(Mul(FrameOf(t1, t2), Transposed(FrameOf(Unit(Mul(H, hp - ep)), Mul(H, Mul(hr, V3{0, 0, 1}))))), q);
        };
        float qRelax[4], qAim[4], qTurn[4];
        aimed(st.relaxRootQ[1], st.relaxRootAt[1], st.relaxRot.data(), st.relaxPos.data(), qRelax);
        aimed(st.rootQ[1], st.rootAt[1], st.aimRot.data(), st.aimPos.data(), qAim);
        Toward(qRelax, qAim, st.leftS, qTurn);
        // (hands empty the game holds this arm elsewhere, and the turn worked out from its holds with the gun left it high, hand up:
        // from where it's been lately instead. Its quick moves still come through, then it settles at its pose)
        {
            float qNow[4], follow = stepDt > 0 ? 1 - expf(-stepDt / kOffFollow) : 1;
            M3 hr;
            V3 hNow;
            aimed(lq[1], lAt[1], rot, pos, qNow);
            HandOn(parent, roots[1], a2.hand, lq[1], lAt[1], rot, pos, &hr, &hNow);
            if (!st.haveOffHold) {
                memcpy(st.offHoldQ, qNow, sizeof(qNow));
                st.offHoldAt = hNow, st.haveOffHold = true;
            } else {
                Toward(st.offHoldQ, qNow, follow, st.offHoldQ);
                st.offHoldAt = st.offHoldAt + (hNow - st.offHoldAt) * follow;
            }
            // (and to the look it has at rest with the gun: the arm the game gives there isn't quite its hold, so the turn from the
            // hold doesn't aim it exactly - aimed exactly, it came back low, a big sleeve. What's between the two kept while at rest)
            if (st.emptyNow < 0.01f && view.still && view.idle <= 0 && view.aim < 0.01f) {
                float qFix[4];
                ToQuat(Mul(FromQuat(qTurn), Transposed(FromQuat(qNow))), qFix);
                if (!st.haveOffFix) memcpy(st.offFixQ, qFix, sizeof(qFix)), st.offFixAt = hNow - lp, st.haveOffFix = true;
                else {
                    Toward(st.offFixQ, qFix, follow, st.offFixQ);
                    st.offFixAt = st.offFixAt + (hNow - lp - st.offFixAt) * follow;
                }
            }
            float qHold[4];
            memcpy(qHold, st.offHoldQ, sizeof(qHold));
            if (st.haveOffFix) ToQuat(Mul(FromQuat(st.offFixQ), FromQuat(st.offHoldQ)), qHold);
            Toward(qTurn, qHold, st.emptyNow, qTurn);
            lp = lp + (st.offHoldAt - st.offFixAt - lp) * st.emptyNow;
            // (kept relaxed: to the look it has at rest, as above. In the air too - the arm's put on its hold there, and aimed exactly it
            // turned to the side)
            float kept = relaxed > calm ? relaxed : calm;
            if (kept > 0.001f && st.haveOffFix) {
                float qLook[4];
                ToQuat(Mul(FromQuat(st.offFixQ), FromQuat(qTurn)), qLook);
                Toward(qTurn, qLook, kept, qTurn);
                lp = lp - st.offFixAt * kept;
            }
        }
        V3 lv = hAt + want - hand + Mul(H, lp);
        float m = armLook / armFlat, sx = side * spotRight, sy = -spotDown;
        float tx = (pp.leftX - (1 - m) * sx) / m, ty = (pp.leftY - (1 - m) * sy) / m;
        // (the arm reaches ahead from its hand back to its shoulder: the hand as much farther off, the shoulder no nearer the eye
        // than the gun hand - nearer, both arms were shrunk less for it, the gun jumping and the hand gone)
        float lz = chr->rig[0] * k;
        leftShift = {-tx * av.tans[0] * lz - lv.x, ty * av.tans[1] * lz - lv.y, lz - lv.z};
        // (hanging, raised into the view it's a limb across the middle: turned over about its hand, it comes up from the
        // bottom, hand up. Only while it's free: reaching for the gun - a reload - back as the game has it)
        M3 cr;
        V3 lNow, rNow;
        HandOn(parent, roots[1], a2.hand, lq[1], lAt[1], rot, pos, &cr, &lNow);
        HandOn(parent, roots[0], a.hand, gameQ, gameAt, rot, pos, &cr, &rNow);   // (as the game has them: its hand to the gun)
        float free = (Len(lNow - rNow) / k - 0.5f) / 0.4f;
        free = free < 0 ? 0 : (free > 1 ? 1 : free);
        st.leftFree += (free - st.leftFree) * (stepDt > 0 ? 1 - expf(-stepDt / 0.1f) : 1);
        // (reaching for the gun, a reload: it goes with the gun arm, as one - its hand gets to the gun where it's been moved)
        // (hands empty there's no gun to reach for; nor kept relaxed, aiming: it looked near the gun and stayed with it, up by the view's edge)
        float reaching = (1 - st.leftFree) * (1 - st.emptyNow) * (1 - relaxed);
        float with = asGame ? 1.0f : (twoHand ? 1 - st.emptyNow : (reaching > st.reloadNow ? reaching : st.reloadNow));
        float gunQ[4];
        ToQuat(Mul(gunCr, FromQuat(lq[1])), gunQ);
        V3 gunAt = gunCp + Mul(gunCr, lAt[1]);
        leftShift = leftShift * (1 - with);
        st.withNow = with;
        if (g_weaponLog > 0) { --g_weaponLog; Log(L"Diag: off arm relaxed %.2f free %.2f reaching %.2f with %.2f air %.2f action %.2f", relaxed, st.leftFree, reaching, with, st.airNow, st.actionNow); }
        leftShift.y += st.jumpNow * av.tans[1] * depth * armMove * (1 - with);   // (with the gun arm through a jump and a landing - on the gun it has it already)
        // (aimed, the off arm's place is fixed on the screen: none of the gun's sway or bob reached it and a shot did nothing to it - it hung there, a picture of
        // an arm. It sways with the gun now, steps as they walk, breathes standing, and a shot rocks it on a spring, a little after the gun's own kick)
        float offLive = st.stanceNow * (1 - st.reloadNow) * (1 - st.actionNow) * (1 - st.emptyNow) * (1 - with);
        float offPitch = 0, offRoll = 0;
        if (stepDt > 0) {
            st.offKickV += (-kOffKickSpring * kOffKickSpring * st.offKickF - 2 * kOffKickDamp * kOffKickSpring * st.offKickV) * stepDt;
            st.offKickF += st.offKickV * stepDt;
            st.offBreath = fmodf(st.offBreath + stepDt * kOffBreathHz * 6.2832f, 6.2832f);
        }
        if (offLive > 0.001f) {
            float size = 1 - (1 - kOffZoomed) * z;
            float gait = sqrtf(st.bobOn > 0 ? st.bobOn : 0) * size;
            float breath = g_offLive[2] * (1 - st.bobOn) * size;
            V3 live = st.offset;   // (the gun's sway and bob, as they are)
            live.y -= st.jumpNow * av.tans[1] * depth * armMove;   // (the jump's already on it)
            live.x += sinf(st.offPhase) * g_offLive[0] * gait * av.tans[0] * depth * armMove;
            live.y += (cosf(st.offPhase * 2) * g_offLive[1] * gait + sinf(st.offBreath) * breath + st.offKickF) * av.tans[1] * depth * armMove;
            live.z -= st.offKickF * kOffKickBack * av.tans[1] * depth;
            leftShift = leftShift + live * offLive;
            offPitch = -st.offKickF / 0.03f * g_offLive[4] * offLive;
            offRoll = st.offKickF / 0.03f * g_offLive[5] * st.kickSide * offLive;
        }
        offStraight = 1 - with;
        if (offPitch != 0 || offRoll != 0) ToQuat(Mul(Mul(About({0, 0, 1}, offRoll), About({1, 0, 0}, offPitch)), FromQuat(qTurn)), qTurn);
        M3 Cl = Mul(Mul(Transposed(H), FromQuat(qTurn)), H);
        leftShift = leftShift - Mul(H, Mul(Cl, lNow - lp)) * (g_walkLess * st.moving * (1 - with) * (1 - st.emptyNow * (1 - st.emptyDown)));
        M3 turned = Mul(Cl, FromQuat(lq[1]));
        ToQuat(turned, lq[1]);
        lAt[1] = lp + Mul(Cl, lAt[1] - lp);
        Toward(lq[1], gunQ, with, lq[1]);
        lAt[1] = lAt[1] + (gunAt - lAt[1]) * with;
    }
    // (third person's arms: the game goes from aiming to relaxed in under 0.1 s, the gun whipped across the view: held as it
    // was while the stance changes, then eased over kStanceEase onto the new one)
    if (g_armMoves == 3) {
        M3 gNowR;
        V3 gNowP;
        HandOn(parent, roots[0], a.hand, lq[0], lAt[0], rot, pos, &gNowR, &gNowP);
        M3 Rv = Mul(H, gNowR);
        V3 Pv = hAt + want - hand + Mul(H, gNowP);
        bool changing = fabsf(view.aim - st.lastAim) > 0.002f;
        st.lastAim = view.aim;
        if (changing && st.haveShown) {
            st.offR = RotVec(Mul(st.shownR, Transposed(Rv)));
            st.offP = st.shownP - Pv;
        } else if (stepDt > 0) {
            float keep = expf(-stepDt / kStanceEase);
            st.offR = st.offR * keep;
            st.offP = st.offP * keep;
        }
        M3 oR = FromRotVec(st.offR);
        st.shownR = Mul(oR, Rv);
        st.shownP = Pv + st.offP;
        st.haveShown = true;
        M3 C0 = Mul(Mul(Transposed(H), oR), H);
        M3 turned = Mul(C0, FromQuat(lq[0]));
        ToQuat(turned, lq[0]);
        lAt[0] = gNowP + Mul(C0, lAt[0] - gNowP) + Mul(Transposed(H), st.offP);
        if (twoHand) {   // (both hands on it: the off arm eased with the gun arm, one piece)
            M3 both = Mul(C0, FromQuat(lq[1]));
            ToQuat(both, lq[1]);
            lAt[1] = gNowP + Mul(C0, lAt[1] - gNowP) + Mul(Transposed(H), st.offP);
        }
    }
    // both arms hung on the chest placed in the view - all that's on them comes along
    for (int i = 0; i < 2; ++i) {
        int r = roots[i], p = parent[r];
        M3 newR = Mul(chestNew, FromQuat(lq[i]));
        V3 newAt = chestNewAt + Mul(chestNew, lAt[i]);
        newAt = newAt + Mul(V, i == 1 ? leftShift : rightShift);
        M3 Pt = Transposed(R[p]);
        ToQuat(Mul(Pt, newR), rot + (r + 1) * 4);
        V3 l = Mul(Pt, newAt - place[p]);
        pos[(r + 1) * 3] = l.x;
        pos[(r + 1) * 3 + 1] = l.y;
        pos[(r + 1) * 3 + 2] = l.z;
    }
    if ((!place0 || !place0->dual) && g_offHandOn && g_armMoves != 3) OffHand(st, av, parent, n, a, a2, k, eye, V, held, dt, rot, pos, R.data(), place.data());
    // Their own view the only one on the screen, fully in first person: the rest of their body hidden away - all of it
    // shrunk into the chest the arms hang on (the pose's root at kTiny, the arms' roots grown back by its inverse: they
    // and all on them as they are). Split screen, the partner sees them whole (the game's pose: one for every view).
    Posed(parent, n, rot, pos, R.data(), place.data());
    if (posed) {   // (the wrists: each hand turned on its forearm, all that's on it along)
        auto bend = [&](int hand, float p, float y, float r) {
            if (hand < 0 || parent[hand] == 0xff || (p == 0 && y == 0 && r == 0)) return;
            M3 inView = Mul(Mul(About({0, 0, 1}, r), About({0, 1, 0}, y)), About({1, 0, 0}, p));
            M3 turned = Mul(Mul(Mul(V, inView), Transposed(V)), R[hand]);
            ToQuat(Mul(Transposed(R[parent[hand]]), turned), rot + (hand + 1) * 4);
        };
        bend(a.hand, pp.wristP, pp.wristY, pp.wristR);
        Posed(parent, n, rot, pos, R.data(), place.data());
        // (both arms from their shoulders, out of the view beside and below the eye, to where their hands are)
        float sx = chr->rig[1] * k, sy = -chr->rig[2] * k, sz = chr->rig[3] * k;
        V3 gunRoot = {-sx, sy, sz};
        if (twoHand) {   // (a hip weapon's rear grip is low and near: the arm from behind and beside it, not from a shoulder ahead of it)
            V3 g = Mul(Transposed(V), place[a.hand] - eye);
            gunRoot = gunRoot + (V3{g.x + look->rig[1] * k, g.y + look->rig[2] * k, g.z + look->rig[3] * k} - gunRoot) * (1 - st.emptyNow);
        }
        // (as the game has them: each arm's elbow and wrist left where the game's pose has them - solved from a shoulder put under the view the forearm came up
        // from below and the wrist took the difference, the off hand bent 70 degrees sideways on the drum. Only the upper arm's turned: from the elbow down and
        // back out of the view, its length)
        auto fromElbow = [&](const Arm& arm, int root, const float* way) {
            V3 E0 = place[arm.elbow], T0 = place[arm.hand];
            float la = Len(place[arm.elbow] - place[arm.shoulder]);
            V3 S = E0 + Mul(V, Unit(V3{way[0], way[1], way[2]})) * la;
            ArmIk(parent, n, arm, root, S, E0 - (S + T0) * 0.5f, 0, rot, pos, R.data(), place.data(), &T0);
        };
        if (chr->asGame && look) fromElbow(a, roots[0], look->rig + 1);
        else ArmIk(parent, n, a, roots[0], eye + Mul(V, gunRoot), Mul(V, V3{-0.7f, -1, -0.1f}), st.emptyNow, rot, pos, R.data(), place.data());
        // (the off hand at the gun is across the view from its shoulder: too far to reach, the shoulder was drawn into the view after it, a
        // hump of sleeve. Its shoulder then under the hand, below the view, the arm coming up from the bottom)
        V3 offAt = Mul(Transposed(V), place[a2.hand] - eye), offRoot = {sx, sy, sz};
        if (chr->asGame && look) {
            st.gripNow = 0;
            fromElbow(a2, roots[1], look->rig + 4);
        } else if (twoHand && chr->plainOff) {
            V3 o = Mul(Transposed(V), place[a2.hand] - eye);
            offRoot = offRoot + (V3{o.x + look->rig[4] * k, o.y + look->rig[5] * k, o.z + look->rig[6] * k} - offRoot) * (1 - st.emptyNow);
            st.gripNow = 0;
            ArmIk(parent, n, a2, roots[1], eye + Mul(V, offRoot), Mul(V, V3{0.7f, -1, -0.1f}), st.emptyNow, rot, pos, R.data(), place.data());
        } else if (twoHand) {
            float on = 1 - offStraight;
            V3 handAt = place[a2.hand];
            float grip = 0;
            if (st.aimRot.size() == (size_t)bones * 4) {
                M3 hR0, oR0;
                V3 hP0, oP0;
                HandOn(parent, roots[0], a.hand, st.rootQ[0], st.rootAt[0], st.aimRot.data(), st.aimPos.data(), &hR0, &hP0);
                HandOn(parent, roots[1], a2.hand, st.rootQ[1], st.rootAt[1], st.aimRot.data(), st.aimPos.data(), &oR0, &oP0);
                V3 relHold = Mul(Transposed(hR0), oP0 - hP0), relNow = Mul(Transposed(R[a.hand]), place[a2.hand] - place[a.hand]);
                grip = 1 - (Len(relNow - relHold) / k - 0.06f) / 0.14f;   // (off its hold - a reload, the draw: the game's hand)
                grip = (grip < 0 ? 0 : (grip > 1 ? 1 : grip)) * on * g_gripOn;
                M3 Gw = Mul(R[a.hand], st.gunInHandNow);   // (the weapon's axes: left, up, along it)
                V3 at0 = Mul(Transposed(st.gunInHandNow), relHold);
                V3 gripAt = place[a.hand] + Mul(Gw, at0 + V3{look->grip[0], look->grip[1], look->grip[2]} * k);
                M3 across = {{-1, 0, 0, 0, 1, 0, 0, 0, -1}};   // (the hand's axes: its fingers to the right, its back up, its thumb's side back to the eye)
                M3 gripR = Mul(Mul(Gw, across), Mul(Mul(About({0, 1, 0}, look->grip[3]), About({0, 0, 1}, look->grip[4])), About({1, 0, 0}, look->grip[5])));
                if (g_weaponLog > 0) Log(L"Diag: grip %.2f - the off hand at its hold, in the weapon's axes (left up along): %.3f %.3f %.3f", grip, at0.x / k, at0.y / k, at0.z / k);
                float q0[4], q1[4];
                ToQuat(R[a2.hand], q0);
                ToQuat(gripR, q1);
                Toward(q0, q1, grip, q0);
                handAt = handAt + (gripAt - handAt) * grip;
                ToQuat(Mul(Transposed(R[parent[a2.hand]]), FromQuat(q0)), rot + (a2.hand + 1) * 4);
                Posed(parent, n, rot, pos, R.data(), place.data());
            }
            st.gripNow = grip;
            // (the elbow: to the left of the hand, about as far off, a little above the view's bottom edge - so the upper arm goes on down out of the view at its
            // own length, its sleeve's cuff all that shows of it: stretched up to an elbow in the middle of the view the sleeve smeared. The forearm's bones as much
            // farther along as it takes to get from there to the hand - the game's own way with this arm, 1.7 times its length to reach the handle)
            V3 hv = Mul(Transposed(V), handAt - eye);
            float upper = Len(place[a2.elbow] - place[a2.shoulder]), lower = Len(place[a2.hand] - place[a2.elbow]);
            V3 uu = Unit(V3{g_offUpper[0], g_offUpper[1], g_offUpper[2]});
            float tanV = av.tans[1], ez = hv.z + look->rig[6] * k, ey = -tanV * ez + look->rig[5] * k;
            if (ey > hv.y - 0.05f * k) ey = hv.y - 0.05f * k;
            V3 elbowV = {hv.x + look->rig[4] * k, ey, ez};
            float f = lower > 1e-4f ? Len(hv - elbowV) / lower : 1;
            f = 1 + (f - 1) * on;
            V3 shoulderV = elbowV + uu * upper;
            for (int c = a2.hand; c != a2.elbow && c >= 0 && c < n && parent[c] != 0xff; c = parent[c])
                for (int i = 0; i < 3; ++i) pos[(c + 1) * 3 + i] *= f;
            Posed(parent, n, rot, pos, R.data(), place.data());
            offRoot = offRoot + (shoulderV - offRoot) * on;
            V3 pole = V3{0.7f, -1, -0.1f} + (elbowV - shoulderV - V3{0.7f, -1, -0.1f}) * on;
            ArmIk(parent, n, a2, roots[1], eye + Mul(V, offRoot), Mul(V, pole), offStraight, rot, pos, R.data(), place.data(), &handAt);
        } else {
            V3 under = V3{offAt.x + 0.25f * k, -0.75f * k, offAt.z - 0.35f * k};
            offRoot = offRoot + (under - offRoot) * (1 - offStraight);
            ArmIk(parent, n, a2, roots[1], eye + Mul(V, offRoot), Mul(V, V3{0.7f, -1, -0.1f}), offStraight, rot, pos, R.data(), place.data());
        }
        // (All at once, not eased from the gun's hold: between the two the arms crossed the view - a forearm stood up in the middle of it for a frame or two
        // as the gun went. The fists come up from under the view instead; and the gun, drawn again, comes up from there as it did)
        float fists = g_fists && chr->fists && !view.gunOut ? 1.0f : 0.0f;
        st.fistUp = fists > 0 ? st.fistUp + (1 - st.fistUp) * (stepDt > 0 ? 1 - expf(-stepDt / 0.06f) : 1.0f) : 0;
        // (the game's hands from the eye, in the body's own axes - to its left, up, ahead: strafing, the body's turned a quarter round from the way they
        // look, and taken in the view's axes its hands were out in front and across - an arm held out at its length all the way along)
        V3 fistNow[2] = {rawHand[0] - eye, rawHand[1] - eye};
        if (g_fists && chr->fists) {
            // (What each hand's move is taken from: where the game carries it - hanging, standing; on a run the gun hand's swung about a place well ahead of
            // that, and taken from where it hangs the fist was held out at arm's length all the run. So its place averaged, over about a stride - but not
            // through a punch, which would drag it along: the gun hand up above the chest, out at its reach or across the body is one, and a little after)
            if (!st.haveFistRest) {
                st.fistRest[0] = V3{-0.78f, -0.91f, -0.35f} * k;
                st.fistRest[1] = V3{0.55f, -0.90f, 0.24f} * k;
                st.haveFistRest = true;
            }
            bool punch = fistNow[0].y > -0.45f * k || fistNow[0].z > 0.9f * k || fistNow[0].x > -0.25f * k;
            st.fistPunch = punch ? 0.25f : (st.fistPunch > stepDt ? st.fistPunch - stepDt : 0);
            if (fists <= 0) {   // (the gun out: the off hand's on it, or hanging another way - from where they hang standing, when the gun goes)
                st.fistRest[0] = V3{-0.78f, -0.91f, -0.35f} * k;
                st.fistRest[1] = V3{0.55f, -0.90f, 0.24f} * k;
            } else if (st.fistPunch <= 0 && stepDt > 0) {
                float t = 1 - expf(-stepDt / 0.4f);
                for (int i = 0; i < 2; ++i) st.fistRest[i] = st.fistRest[i] + (fistNow[i] - st.fistRest[i]) * t;
            }
            if (g_weaponLog > 0) Log(L"Diag: fists %.2f gunOut %d still %d rest %.2f slow %d - gun hand %.3f %.3f %.3f (hangs %.3f %.3f %.3f) off hand %.3f %.3f %.3f (hangs %.3f %.3f %.3f), eye %.3f %.3f %.3f", fists, view.gunOut ? 1 : 0, view.still ? 1 : 0, view.rest, st.fistPunch > 0 ? 1 : 0,
                fistNow[0].x / k, fistNow[0].y / k, fistNow[0].z / k, st.fistRest[0].x / k, st.fistRest[0].y / k, st.fistRest[0].z / k, fistNow[1].x / k, fistNow[1].y / k, fistNow[1].z / k, st.fistRest[1].x / k, st.fistRest[1].y / k, st.fistRest[1].z / k, eye.x / k, eye.y / k, eye.z / k);
            float ease = stepDt > 0 && g_fist[15] > 0.001f ? 1 - expf(-stepDt / g_fist[15]) : 1.0f;
            for (int i = 0; i < 2; ++i) st.fistD[i] = fists > 0.001f ? st.fistD[i] + (fistNow[i] - st.fistRest[i] - st.fistD[i]) * ease : V3{0, 0, 0};
        }
        if (fists > 0.001f) {
            float z0 = g_fist[2] * k;
            for (int i = 0; i < 2; ++i) {
                const Arm& arm = i ? a2 : a;
                float out = i ? 1.0f : -1.0f;   // (the view's x is to the left: the off hand's side)
                V3 d = st.fistD[i];
                const float* gain = g_fist + (i ? 12 : 3);
                V3 tv = V3{out * g_fist[0] * av.tans[0] * z0, -g_fist[1] * av.tans[1] * z0, z0} + V3{gain[0] * d.x, gain[1] * d.y, gain[2] * d.z} + st.offset;
                V3 sv = {out * g_fist[6] * k, -g_fist[7] * k, g_fist[8] * k};
                tv.y -= (1 - st.fistUp) * 0.6f * k, sv.y -= (1 - st.fistUp) * 0.6f * k;
                // (The arm as it comes here is what the gun's hold made of it - hanging out of the view, mostly: the other way from where it's wanted, and
                // an arm solved from that is turned any which way about itself, anew each pose. So: its elbow and wrist bent as the game has them this pose,
                // the whole arm turned at the shoulder till the fist's as it's wanted - its fingers' way along the forearm that's to be, its thumb up and a
                // little in - and solved from there: what's left to turn is the difference of two bends)
                // (and its bones their own length: a two-handed weapon's off forearm is drawn out to its handle)
                { int at2 = 0; for (int c = arm.hand; c >= 0 && c < n && c != arm.shoulder && parent[c] != 0xff && at2 < rawBones[i]; c = parent[c], ++at2) {
                    Toward(rot + (c + 1) * 4, rawRot[i][at2], fists, rot + (c + 1) * 4);
                    for (int x = 0; x < 3; ++x) pos[(c + 1) * 3 + x] += (rawPos[i][at2][x] - pos[(c + 1) * 3 + x]) * fists;
                } }
                Posed(parent, n, rot, pos, R.data(), place.data());
                V3 handAt = place[arm.hand] + (eye + Mul(V, tv) - place[arm.hand]) * fists;
                V3 shoulderAt = place[arm.shoulder] + (eye + Mul(V, sv) - place[arm.shoulder]) * fists;
                V3 pole = Mul(V, V3{out * 0.7f, -1, -0.1f}), S = shoulderAt, E;
                float la = Len(place[arm.elbow] - place[arm.shoulder]), lb = Len(place[arm.hand] - place[arm.elbow]);
                if (la > 1e-4f && lb > 1e-4f && ArmElbow(la, lb, &S, handAt, pole, &E)) {
                    float side = i ? 1.0f : -1.0f;   // (the hand's own axes: the left's fingers +x, thumb +z; the right's a mirror)
                    V3 fw1 = Unit(handAt - E), th1 = Mul(V, Unit(V3{-out * g_fist[9], 1, 0}));
                    float q0[4], q1[4];
                    ToQuat(R[arm.hand], q0);
                    ToQuat(Mul(FrameOf(fw1, th1), Transposed(FrameOf(V3{side, 0, 0}, V3{0, 0, side}))), q1);
                    Toward(q0, q1, fists, q0);
                    M3 turned = Mul(Mul(FromQuat(q0), Transposed(R[arm.hand])), R[arm.shoulder]);
                    ToQuat(Mul(Transposed(R[parent[arm.shoulder]]), turned), rot + (arm.shoulder + 1) * 4);
                    Posed(parent, n, rot, pos, R.data(), place.data());
                }
                ArmIk(parent, n, arm, roots[i], shoulderAt, pole, 0, rot, pos, R.data(), place.data(), &handAt);
                if (g_weaponLog > 0) {
                    V3 hv = Mul(Transposed(V), place[arm.hand] - eye), ev = Mul(Transposed(V), place[arm.elbow] - eye), shv = Mul(Transposed(V), place[arm.shoulder] - eye);
                    Log(L"Diag: fist %d to %.3f %.3f %.3f - hand %.3f %.3f %.3f elbow %.3f %.3f %.3f shoulder %.3f %.3f %.3f, arm %.3f + %.3f, offset %.3f %.3f %.3f, tans %.3f %.3f", i, tv.x / k, tv.y / k, tv.z / k,
                        hv.x / k, hv.y / k, hv.z / k, ev.x / k, ev.y / k, ev.z / k, shv.x / k, shv.y / k, shv.z / k, Len(place[arm.elbow] - place[arm.shoulder]) / k, Len(place[arm.hand] - place[arm.elbow]) / k,
                        st.offset.x / k, st.offset.y / k, st.offset.z / k, av.tans[0], av.tans[1]);
                }
            }
        }
        const V3 thumbRest[2] = {{0.6f, -0.2f, 0.75f}, {0.85f, -0.35f, 0.4f}};
        float swinging = st.emptyNow * (1 - st.emptyDown);
        // (the grip's the Nail Gun's: another weapon's fingers as the game has them on it)
        if (chr->fingers && g_fingers[2] >= 0 && !look) HandShape(parent, n, a.hand, -1, g_fingers[2], g_fingers[3], g_fingers[5] * 0.25f, g_gripThumb, 1 - st.emptyNow, rot, pos, R.data(), place.data());
        if (chr->fingers) HandShape(parent, n, a2.hand, 1, g_fingers[0], g_fingers[1], g_fingers[5], thumbRest, offStraight * (1 - swinging), rot, pos, R.data(), place.data());
        if (fists > 0.001f) {
            const V3 thumbFist[2] = {{0.55f, -0.45f, 0.7f}, {0.75f, -0.6f, 0.25f}};
            HandShape(parent, n, a.hand, -1, g_fist[10], g_fist[11], 0, thumbFist, fists, rot, pos, R.data(), place.data());
            HandShape(parent, n, a2.hand, 1, g_fist[10], g_fist[11], 0, thumbFist, fists, rot, pos, R.data(), place.data());
        }
        if (twoHand && st.gripNow > 0.001f) HandShape(parent, n, a2.hand, 1, look->grip[6], look->grip[7], 0, nullptr, st.gripNow, rot, pos, R.data(), place.data());   // (round the handle)
    }
    if (posed && g_reloadLog > 0 && st.reloadNow > 0.02f) {
        --g_reloadLog;
        V3 l = Mul(Transposed(V), place[a2.hand] - eye), r = Mul(Transposed(V), place[a.hand] - eye), e2 = Mul(Transposed(V), place[a2.elbow] - eye);
        Log(L"Diag: reload clip %d now %.2f with %.2f free %.2f - off hand view %.3f %.3f %.3f screen %.2f %.2f; off elbow screen %.2f %.2f; gun hand screen %.2f %.2f", view.clip, st.reloadNow, st.withNow, st.leftFree, l.x, l.y, l.z,
            l.z > 0.01f ? l.x / (av.tans[0] * l.z) : 99.f, l.z > 0.01f ? l.y / (av.tans[1] * l.z) : 99.f, e2.z > 0.01f ? e2.x / (av.tans[0] * e2.z) : 99.f, e2.z > 0.01f ? e2.y / (av.tans[1] * e2.z) : 99.f,
            r.z > 0.01f ? r.x / (av.tans[0] * r.z) : 99.f, r.z > 0.01f ? r.y / (av.tans[1] * r.z) : 99.f);
    }
    if (g_offBones) {   // (research: "fpv offbones" - the off hand's bones in its own axes, and how it lies in the view)
        g_offBones = false;
        int h2 = a2.hand;
        M3 Rt = Transposed(R[h2]);
        Log(L"Motion: offbones - hand %d elbow %d shoulder %d clav %d, k %.3f", h2, a2.elbow, a2.shoulder, a2.clav, k);
        for (const Arm* q : {&a, &a2}) {
            wchar_t chain[128] = L"";
            for (int c = q->hand; c >= 0 && c != chest; c = parent[c] == 0xff ? -1 : parent[c]) {
                wchar_t one[24];
                swprintf_s(one, L" %d(%.3f)", c, Len(V3{pos[(c + 1) * 3], pos[(c + 1) * 3 + 1], pos[(c + 1) * 3 + 2]}));
                wcscat_s(chain, one);
            }
            M3 Vg = Mul(Transposed(V), R[q->hand]);
            V3 hv = Mul(Transposed(V), place[q->hand] - eye), sv = Mul(Transposed(V), place[q->shoulder] - eye), ev = Mul(Transposed(V), place[q->elbow] - eye);
            Log(L"Motion: arm - chain%s; upper %.3f lower %.3f; in the view hand %.2f %.2f %.2f elbow %.2f %.2f %.2f shoulder %.2f %.2f %.2f", chain, Len(place[q->elbow] - place[q->shoulder]),
                Len(place[q->hand] - place[q->elbow]), hv.x, hv.y, hv.z, ev.x, ev.y, ev.z, sv.x, sv.y, sv.z);
            Log(L"Motion: arm - hand axes in the view (left, up, ahead): x %.3f %.3f %.3f, y %.3f %.3f %.3f, z %.3f %.3f %.3f", Vg.m[0], Vg.m[3], Vg.m[6], Vg.m[1], Vg.m[4], Vg.m[7], Vg.m[2], Vg.m[5], Vg.m[8]);
        }
        for (int hh : {h2, a.hand}) {
            M3 Rh = Transposed(R[hh]);
            for (int b = 0; b < n; ++b) {
                bool on = false;
                for (int c = b; c >= 0 && c < n; c = parent[c] == 0xff ? -1 : parent[c]) if (c == hh) { on = true; break; }
                if (!on) continue;
                V3 l = Mul(Rh, place[b] - place[hh]) * (1 / k);
                Log(L"Motion: offbones - hand %d: bone %d (parent %d) in the hand's axes %.3f %.3f %.3f", hh, b, parent[b], l.x, l.y, l.z);
            }
        }
        V3 fa = Mul(Rt, place[a2.elbow] - place[h2]) * (1 / k);
        Log(L"Motion: offbones - the elbow in the hand's axes %.3f %.3f %.3f", fa.x, fa.y, fa.z);
        M3 Vh = Mul(Transposed(V), R[h2]);
        Log(L"Motion: offbones - the hand's axes in the view (left, up, ahead): x %.2f %.2f %.2f, y %.2f %.2f %.2f, z %.2f %.2f %.2f",
            Vh.m[0], Vh.m[3], Vh.m[6], Vh.m[1], Vh.m[4], Vh.m[7], Vh.m[2], Vh.m[5], Vh.m[8]);
    }
    V3 hc = place[a.hand];
    bool viewmodelNow = w >= 0.999f && view.alone && g_armsHideBody;
    float armS = 1;
    V3 armAhead = {0, 0, 0};
    if (viewmodelNow && g_armScale != 1) {
        // Drawn as a shooter's: both arms (and the gun on them) shrunk toward the eye - the same on the screen, but
        // within reach of the eye: the gun, a model of its own drawn where it is, came out past a wall stood up against
        // (behind a door - not drawn) at an arm's length.
        // (no nearer than kArmNearest: a bone that near the camera, the game stops drawing the character - Stewie's
        // hand, his eye low aiming. Shrunk toward the eye, more or less is the same on the screen.)
        float nearest = 1e9f;
        for (int b = 0; b < n; ++b)
            if (armBone[b] || b == roots[0] || b == roots[1]) nearest = fminf(nearest, Len(place[b] - eye));
        armS = g_armScale * armFlat;
        if (nearest * armS < kArmNearest) armS = nearest > kArmNearest ? kArmNearest / nearest : 1;
        if (posed) armS = chr->rig[4], armAhead = Mul(V, V3{0, 0, chr->rig[5]});
        if (g_armsWhyLog && fabsf(armS - st.lastArmS) > 0.02f) {
            int nb = -1;
            for (int b = 0; b < n; ++b)
                if ((armBone[b] || b == roots[0] || b == roots[1]) && fabsf(Len(place[b] - eye) - nearest) < 1e-5f) nb = b;
            Log(L"Motion: player %d's arms shrunk %.2f (bone %d %.3f from the eye; size %.2f)", slot, armS, nb, nearest, k);
            st.lastArmS = armS;
        }
        for (int i = 0; i < 2; ++i) {
            int r = roots[i], p = parent[r];
            V3 l = Mul(Transposed(R[p]), eye + armAhead + (place[r] - eye) * armS - place[p]);
            pos[(r + 1) * 3] = l.x;
            pos[(r + 1) * 3 + 1] = l.y;
            pos[(r + 1) * 3 + 2] = l.z;
            for (int c = 0; c < 3; ++c) scale[(r + 1) * 3 + c] *= armS;
        }
        hc = eye + armAhead + (hc - eye) * armS;
        Posed(parent, n, rot, pos, R.data(), place.data());   // (the roots' places: the arms' bones beyond, unscaled)
    }
    {   // (their gun hand, in the world - firstperson.cpp finds the gun's draw by it; hiding the body leaves it there)
        V3 hw = org + ax[0] * hc.x + ax[1] * hc.y + ax[2] * hc.z;
        gunHand[0] = hw.x;
        gunHand[1] = hw.y;
        gunHand[2] = hw.z;
        gunHand[3] = armS;
        for (int c = 0; c < 3; ++c) gunHand[4 + c] = view.eye[c], gunHand[7 + c] = view.forward[c];
        // (and in the character's own space: the frame's draw says where and which way the character's put - firstperson.cpp FrameArms)
        gunHand[26] = eye.x, gunHand[27] = eye.y, gunHand[28] = eye.z, gunHand[29] = F.x, gunHand[30] = F.y, gunHand[31] = F.z, gunHand[32] = 0;
        gunHand[10] = st.offset.x * armS, gunHand[11] = st.offset.y * armS, gunHand[12] = st.offset.z * armS;
        gunHand[13] = armLook / armFlat, gunHand[14] = side * spotRight, gunHand[15] = -spotDown;   // (the zoom, about that spot)
        gunHand[16] = Len(armAhead);   // (how far ahead of the eye they're kept: the draw's eye moved up as far)
        M3 Vg = Mul(Transposed(V), R[a.hand]);
        for (int c = 0; c < 9; ++c) gunHand[17 + c] = Vg.m[c];   // (the gun hand's axes in the view)
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
        // (kept out of sight: at an arm's length ahead, the dot of outline it still has peeked out by the gun hand; at the
        // eye, the game stopped drawing Stewie altogether - the camera inside him. Third person's arms: below the screen,
        // ahead - behind the eye their shoulders, stretched to it, fanned out black across the bottom corner)
        // (Drawn zoomed about the gun hand's spot: placed so that it comes out under the middle of the screen, or what's
        // stretched from each shoulder to it crosses the bottom corners - black.)
        M3 base = FromQuat(rot);
        float zm = armLook / armFlat, hideZ = g_hideAhead * k;
        float hideX = (zm - 1) * (side * spotRight) / zm, hideY = ((posed ? chr->hideDown : -3) - (1 - zm) * -spotDown) / zm;
        V3 hideView = {-hideX * av.tans[0] * hideZ, hideY * av.tans[1] * hideZ, hideZ};
        V3 hideAt = eye + Mul(V, g_armMoves == 3 ? hideView : V3{0, -0.3f * k, -0.8f * k});
        st.poseHide = org + ax[0] * hideAt.x + ax[1] * hideAt.y + ax[2] * hideAt.z;
        st.poseHideM = hideAt;
        st.haveHide = true;
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

// (research: "fpv posediag <n>" - the next n poses of player 0: when, on which thread, how far the game's pose came from its last one and from what we made of its last one)
struct PoseDiag { double t; DWORD thread; float inMove, inVsOut, posMove, posVsOut; int inBone, posBone, clip; float hand[3], hw[3], eye[3], fw[3], org[3]; };
std::vector<PoseDiag> g_poseDiag;
int g_poseDiagLeft = 0, g_poseSeq = 0;
std::vector<float> g_diagPrevRot, g_diagPrevPos, g_diagOutRot, g_diagOutPos;
float QuatsApart(const float* a, const float* b, int n, int* worst) {
    float most = 0;
    for (int i = 0; i < n; ++i) {
        float d = fabsf(a[i * 4] * b[i * 4] + a[i * 4 + 1] * b[i * 4 + 1] + a[i * 4 + 2] * b[i * 4 + 2] + a[i * 4 + 3] * b[i * 4 + 3]);
        float gap = 1 - (d > 1 ? 1 : d);
        if (gap > most) most = gap, *worst = i;
    }
    return most;
}
float PlacesApart(const float* a, const float* b, int n, int* worst) {
    float most = 0;
    for (int i = 0; i < n; ++i) {
        float x = a[i * 3] - b[i * 3], y = a[i * 3 + 1] - b[i * 3 + 1], z = a[i * 3 + 2] - b[i * 3 + 2], d = sqrtf(x * x + y * y + z * z);
        if (d > most) most = d, *worst = i;
    }
    return most;
}
// Which pose a frame draws. Characters are skinned on the CPU (0x4b02d0, once for each piece, from a palette kept for that frame): the palette is what's drawn.
// Their hidden body's bones are in it, shrunk to nothing, at where they were put for that pose - by the eye and the way it looks.
const uintptr_t kSkinPiece = 0x4b02d0;
const unsigned char kSkinStart[6] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x28};
typedef void(__thiscall* SkinPiece_t)(void* piece, void* instance);
SkinPiece_t g_skinOriginal = nullptr;
bool g_armMatch = true;   // (research: "fpv armmatch 0")
int g_skinLog = 0, g_skinSeen = 0, g_skinMatched = 0, g_skinNoMatch = 0;
void NoteSkinned(const float* pal, int bones) {
    float t[6][3];
    int nt = 0;
    for (int b = 0; b < bones && nt < 6; ++b) {
        const float* m = pal + b * 12;
        float big = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
        if (big > 0 && big < 0.02f * 0.02f) t[nt][0] = m[3], t[nt][1] = m[7], t[nt][2] = m[11], ++nt;
    }
    if (nt == 0) return;
    ++g_skinSeen;
    EnterCriticalSection(&g_lock);
    auto apart = [](const float* a, const float* b) { float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z); };
    bool any = false;
    for (int s = 0; s < 4 && !any; ++s) {
        Track& tr = g_tracks[s];
        if (!tr.viewmodel || !tr.ent || tr.ringN == 0) continue;
        float dist[Track::kArmRing], bestD = 1e9f;
        bool world = false;
        for (int i = 0; i < tr.ringN; ++i) {
            int k = (tr.ringAt - 1 - i + 2 * Track::kArmRing) % Track::kArmRing;
            dist[i] = 1e9f;
            if (!tr.ringHaveHide[k]) continue;
            for (int j = 0; j < nt; ++j) {
                float w = apart(t[j], tr.ringHide[k]), m = apart(t[j], tr.ringHideM[k]), e = w < m ? w : m;
                if (e < dist[i]) dist[i] = e;
                if (e < bestD) bestD = e, world = w < m;
            }
        }
        if (g_skinLog > 0) {
            --g_skinLog;
            int k0 = (tr.ringAt - 1 + Track::kArmRing) % Track::kArmRing;
            Log(L"Diag: skin th %lu bones %d tiny %d (%.3f %.3f %.3f) - newest hide world %.3f %.3f %.3f model %.3f %.3f %.3f; best %.4f (%s) slot %d", GetCurrentThreadId(), bones, nt, t[0][0], t[0][1], t[0][2],
                tr.ringHide[k0][0], tr.ringHide[k0][1], tr.ringHide[k0][2], tr.ringHideM[k0][0], tr.ringHideM[k0][1], tr.ringHideM[k0][2], bestD, world ? L"world" : L"model", s);
        }
        if (bestD < 0.03f) {
            int n = 0;
            for (int i = 0; i < tr.ringN; ++i) {
                if (dist[i] > bestD + 0.002f) continue;
                int k = (tr.ringAt - 1 - i + 2 * Track::kArmRing) % Track::kArmRing;
                memcpy(tr.skinned[n], tr.ring[k], sizeof(tr.skinned[n]));
                memcpy(tr.skinnedOrg[n], tr.ringOrg[k], sizeof(tr.skinnedOrg[n]));
                tr.skinnedSeq[n] = tr.ringSeq[k];
                ++n;
            }
            tr.skinnedN = n;
            ++g_skinMatched;
            any = true;
        }
    }
    if (!any) ++g_skinNoMatch;
    LeaveCriticalSection(&g_lock);
}
void __fastcall Hook_SkinPiece(void* piece, void*, unsigned char* instance) {
    if (g_armMatch && FirstPersonAny() && instance && !IsBadReadPtr(instance + 0xb0, 8)) {
        int bones = *reinterpret_cast<int*>(instance + 0xb0);
        const float* pal = *reinterpret_cast<const float**>(instance + 0xb4);
        if (bones >= 20 && bones <= 255 && pal && !IsBadReadPtr(pal, bones * 48)) NoteSkinned(pal, bones);
    }
    g_skinOriginal(piece, instance);
}

int g_poseStack = 0;   // (research: "fpv posestack <n>" - who asks for player 1's pose)
void __cdecl Hook_PoseBuild(unsigned char* ent, float* rot, float* pos, float* scale, int bones) {
    if (g_poseStack > 0 && ent && ent == g_tracks[0].ent) {
        --g_poseStack;
        // (the stack by hand: the game's code has no frame pointers to walk - every word that's an address in its code right after a call)
        wchar_t line[900] = L"";
        int at = 0;
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
        for (int i = 0; i < 1200 && at < 860; ++i) {
            if (IsBadReadPtr(sp + i, 4)) break;
            uintptr_t v = sp[i];
            if (v < 0x401000 || v > 0x9c5000) continue;
            const unsigned char* c = reinterpret_cast<const unsigned char*>(v);
            bool call = c[-5] == 0xe8 || (c[-2] == 0xff && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xff && (c[-2] & 0x38) == 0x10) || (c[-6] == 0xff && (c[-5] & 0x38) == 0x10);
            if (call) at += swprintf_s(line + at, 900 - at, L" %x", (unsigned)v);
        }
        Log(L"Diag: pose asked from%s", line);
    }
    FirstPersonPoseStart(ent);
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
        bool diag = g_poseDiagLeft > 0 && &tr == g_tracks && bones == 59;
        PoseDiag pd{};
        if (&tr == g_tracks) ++g_poseSeq;
        if (diag) {
            pd.t = Now(), pd.thread = GetCurrentThreadId(), pd.inBone = pd.posBone = -1;
            if (g_diagPrevRot.size() == (size_t)bones * 4) {
                int w = -1;
                pd.inMove = QuatsApart(rot, g_diagPrevRot.data(), bones, &pd.inBone);
                pd.inVsOut = QuatsApart(rot, g_diagOutRot.data(), bones, &w);
                pd.posMove = PlacesApart(pos, g_diagPrevPos.data(), bones, &pd.posBone);
                pd.posVsOut = PlacesApart(pos, g_diagOutPos.data(), bones, &w);
            }
            g_diagPrevRot.assign(rot, rot + bones * 4);
            g_diagPrevPos.assign(pos, pos + bones * 3);
        }
        tr.viewmodel = false;
        ArmsInView(static_cast<int>(&tr - g_tracks), ent, rot, pos, scale, bones, &tr.viewmodel, tr.gunHandNew);
        if (g_orderTrace > 0 && &tr == g_tracks) {
            --g_orderTrace;
            Log(L"Order: %.5f pose (thread %lu) - eye %.4f %.4f %.4f", Now(), GetCurrentThreadId(), tr.gunHandNew[4],
                tr.gunHandNew[5], tr.gunHandNew[6]);
        }
        tr.armsInView = HeadAway(static_cast<int>(&tr - g_tracks), ent, scale, bones);
        tr.last.resize(bones * kFloats);
        Gather(rot, pos, scale, bones, tr.last.data());
        tr.lastBones = bones;
        tr.haveHead = FirstPersonAny() && HeadPlace(ent, rot, pos, scale, bones, tr.head);
        if (diag) {
            g_diagOutRot.assign(rot, rot + bones * 4);
            g_diagOutPos.assign(pos, pos + bones * 3);
            const float* g = tr.gunHandNew;
            float d[3] = {g[0] - g[4], g[1] - g[5], g[2] - g[6]}, r[3] = {-g[9], 0, g[7]}, l = sqrtf(r[0] * r[0] + r[2] * r[2]);
            if (l > 1e-6f) r[0] /= l, r[2] /= l;
            float u[3] = {r[1] * g[9] - r[2] * g[8], r[2] * g[7] - r[0] * g[9], r[0] * g[8] - r[1] * g[7]};
            pd.hand[0] = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
            pd.hand[1] = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
            pd.hand[2] = d[0] * g[7] + d[1] * g[8] + d[2] * g[9];
            memcpy(pd.hw, g, 12), memcpy(pd.eye, g + 4, 12), memcpy(pd.fw, g + 7, 12);
            { const V3& o = g_armState[0].poseOrg; pd.org[0] = o.x, pd.org[1] = o.y, pd.org[2] = o.z; }
            pd.clip = g_poseSeq;
            g_poseDiag.push_back(pd);
            if (--g_poseDiagLeft == 0) {
                for (const PoseDiag& q : g_poseDiag)
                    Log(L"Diag: pose %.5f th %lu seq %d rot+ %.5f b%d rotVsOut %.5f pos+ %.4f b%d posVsOut %.4f hand %.3f %.3f %.3f hw %.4f %.4f %.4f eye %.4f %.4f %.4f fw %.5f %.5f %.5f org %.4f %.4f %.4f", q.t, q.thread, q.clip, q.inMove, q.inBone, q.inVsOut, q.posMove, q.posBone, q.posVsOut, q.hand[0], q.hand[1], q.hand[2], q.hw[0], q.hw[1], q.hw[2], q.eye[0], q.eye[1], q.eye[2], q.fw[0], q.fw[1], q.fw[2], q.org[0], q.org[1], q.org[2]);
                g_poseDiag.clear();
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    g_poseOriginal(ent, rot, pos, scale, bones);
    // (the eye a pose was made for goes out with its bones: given out before them, a frame drawn in between had the new eye on the old bones - the arms are
    // drawn a hand's breadth from the eye, so the few cm of a strafe between two poses threw them across the view)
    EnterCriticalSection(&g_lock);
    for (Track& tr : g_tracks)
        if (ent && tr.ent == ent) {
            memcpy(tr.gunHand, tr.gunHandNew, sizeof(tr.gunHand));
            memcpy(tr.ring[tr.ringAt], tr.gunHandNew, sizeof(tr.gunHandNew));
            tr.ringSeq[tr.ringAt] = g_poseSeq;
            { const ArmState& as = g_armState[&tr - g_tracks]; tr.ringOrg[tr.ringAt][0] = as.poseOrg.x, tr.ringOrg[tr.ringAt][1] = as.poseOrg.y, tr.ringOrg[tr.ringAt][2] = as.poseOrg.z;
              tr.ringHide[tr.ringAt][0] = as.poseHide.x, tr.ringHide[tr.ringAt][1] = as.poseHide.y, tr.ringHide[tr.ringAt][2] = as.poseHide.z; tr.ringHaveHide[tr.ringAt] = as.haveHide;
              tr.ringHideM[tr.ringAt][0] = as.poseHideM.x, tr.ringHideM[tr.ringAt][1] = as.poseHideM.y, tr.ringHideM[tr.ringAt][2] = as.poseHideM.z; }
            tr.ringAt = (tr.ringAt + 1) % Track::kArmRing;
            if (tr.ringN < Track::kArmRing) ++tr.ringN;
        }
    LeaveCriticalSection(&g_lock);
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
void MotionArmCalm(float calm) { g_armCalm = calm; }
void MotionOffBones() { g_offBones = true; }
void MotionHideDown(float y, float ahead) { g_rigEdit->hideDown = y, g_hideAhead = ahead; }
void MotionKick(const float* values, int count) { for (int i = 0; i < count && i < 4; ++i) g_kick[i] = values[i]; }
void MotionShotLog(bool on) { g_shotLog = on; }
void MotionGripThumb(const float* v) { g_gripThumb[0] = {v[0], v[1], v[2]}, g_gripThumb[1] = {v[3], v[4], v[5]}; }
void MotionLess(float reload, float walk) { g_reloadLess = reload, g_walkLess = walk; }
void MotionFingers(const float* values, int count) { for (int i = 0; i < count && i < 6; ++i) g_fingers[i] = values[i]; }
void MotionRig(const float* values, int count) { for (int i = 0; i < count && i < 6; ++i) g_rigEdit->rig[i] = values[i]; }
void MotionPoseDiag(int n) { g_poseDiagLeft = n; }
void MotionJumpLog(int n) { g_jumpLog = n; }
void MotionWeaponLog(int n) { g_weaponLog = n; }
void MotionPoseStack(int n) { g_poseStack = n; }
void MotionHoldsLearn(bool on) { g_holdsLearn = on; }
void MotionDrawLess(float v) { g_drawLess = v; }
void MotionGripOn(float v) { g_gripOn = v; }
void MotionFists(bool on) { g_fists = on; }
void MotionAsGame(const float* v, int n) { memcpy(g_asGame, v, sizeof(float) * (n < 4 ? n : 4)); }
void MotionFist(const float* v, int n) { memcpy(g_fist, v, sizeof(float) * (n < 16 ? n : 16)); }
// (which: one of Peter's weapons, or 9 - the first weapon's look of the character being played)
WeaponLook* LookToChange(int which) { return which == 9 ? &g_rigEdit->first : (which >= 0 && which < (int)(sizeof(g_weaponLooks) / sizeof(g_weaponLooks[0])) ? &g_weaponLooks[which] : nullptr); }
void MotionWeaponGrip(int which, const float* v, int n) {
    if (WeaponLook* l = LookToChange(which)) memcpy(l->grip, v, sizeof(float) * (n < 8 ? n : 8));
}
void MotionOffUpper(const float* v) { memcpy(g_offUpper, v, sizeof(g_offUpper)); }
void MotionWeaponRig(int which, const float* v, int n) {
    if (WeaponLook* l = LookToChange(which)) memcpy(l->rig, v, sizeof(float) * (n < 7 ? n : 7));
}
void MotionWeaponPose(int which, int zoomed, const float* v, int n) {
    WeaponLook* l = LookToChange(which);
    if (!l) return;
    memcpy(zoomed ? &l->zoomed.right : &l->hold.right, v, sizeof(float) * (n < kArmPoseFloats ? n : kArmPoseFloats));
}
void MotionAimCarry(bool on) { g_aimCarry = on; }
void MotionWeaponHolds(bool on) { g_weaponHolds = on; }
void MotionOffLive(int i, float v) { if (i >= 0 && i < 6) g_offLive[i] = v; }
void MotionClip10Full(bool on) { g_clip10Full = on; }
void MotionReloadLog(int n) { g_reloadLog = n; }
void MotionLandHold(float s, int clip10) { if (s >= 0) g_landHold = s; if (clip10 >= 0) g_clip10Held = clip10 != 0; }
int MotionPoseSeq() { return g_poseSeq; }
void MotionPunchAim(const float* values, int count) { for (int i = 0; i < count && i < 4; ++i) g_punchAim[i] = values[i]; }
void MotionArmPose(int stance, const float* values, int count) {
    if (stance >= 0 && stance < 6) memcpy(&g_rigEdit->pose[stance].right, values, sizeof(float) * (count < kArmPoseFloats ? count : kArmPoseFloats));
}
void MotionArmSize(float size) {
    g_armSize = size;
    Log(L"Motion: first person's arms at %.0f%% of their size", size * 100);
}
void MotionArmFlat(float flat) {
    g_armFlat = flat;
    Log(L"Motion: first person's arms' perspective flat to %.0f%%", flat * 100);
}
void MotionArmMoves(int mode, float swing, bool log) {
    g_armMoves = mode;
    if (swing > 0) g_armSwing = swing;
    g_swingLog = log;
    Log(L"Motion: first person's arms on the move - %s (swing %.2f)", mode == 3 ? L"third person's, all the time" : mode == 2 ? L"the game's" :
        mode == 1 ? L"the hold and the game's swing" : L"the hold",
        g_armSwing);
}
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

// The pose a frame is really drawing: the game draws from the last pose it finished for that frame, on some frames a pose older than the newest and on others
// not (posing and drawing run apart: about 1 frame in 6 at 50-60 fps). Taken for the newest, the arms - drawn a hand's breadth from the eye - jumped across the view
// by what the eye had moved and turned between the two. The one last skinned with (Hook_SkinPiece) is the one being drawn. *seq: which pose.
void MotionArmMatch(bool on) { g_armMatch = on; }
bool MotionViewmodelOf(int slot, const float* at, float* hand, int* seq) {
    if (slot < 0 || slot >= 4 || !g_poseOriginal) return false;
    EnterCriticalSection(&g_lock);
    Track& tr = g_tracks[slot];
    bool on = tr.viewmodel && tr.ent;
    if (on) {
        memcpy(hand, tr.gunHand, sizeof(tr.gunHand));
        if (seq) *seq = g_poseSeq;
        hand[32] = 0;
        if (g_armMatch && tr.skinnedN > 0) {
            int use = 0;   // (the newest of those that fit - or the one the character was standing where this draw says)
            if (at) {
                float bestD = 1e9f;
                for (int i = 0; i < tr.skinnedN; ++i) {
                    const float* o = tr.skinnedOrg[i];
                    float x = at[0] - o[0], y = at[1] - o[1], z = at[2] - o[2], d = x * x + y * y + z * z;
                    if (d < bestD - 1e-6f) bestD = d, use = i;
                }
            }
            memcpy(hand, tr.skinned[use], sizeof(tr.skinned[use]));
            if (seq) *seq = tr.skinnedSeq[use];
            hand[32] = (float)tr.skinnedN;   // (research: how many poses fitted alike)
        }
    }
    LeaveCriticalSection(&g_lock);
    return on;
}
void MotionSkinDiag(int n) { g_skinLog = n; }
void MotionSkinCounts(int* seen, int* matched, int* none) { *seen = g_skinSeen, *matched = g_skinMatched, *none = g_skinNoMatch; }

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
    {
        auto sk = reinterpret_cast<unsigned char*>(kSkinPiece);
        if (IsBadReadPtr(sk, sizeof(kSkinStart)) || memcmp(sk, kSkinStart, sizeof(kSkinStart)) != 0) {
            Log(L"Motion: skinning code isn't what we expect - first person's arms take the newest pose");
        } else if (auto st = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
            memcpy(st, kSkinStart, sizeof(kSkinStart));
            st[sizeof(kSkinStart)] = 0xE9;
            *reinterpret_cast<int*>(st + sizeof(kSkinStart) + 1) = (int)((kSkinPiece + sizeof(kSkinStart)) - (reinterpret_cast<uintptr_t>(st) + sizeof(kSkinStart) + 5));
            g_skinOriginal = reinterpret_cast<SkinPiece_t>(st);
            DWORD o2;
            VirtualProtect(sk, 6, PAGE_EXECUTE_READWRITE, &o2);
            sk[0] = 0xE9;
            *reinterpret_cast<int*>(sk + 1) = (int)(reinterpret_cast<uintptr_t>(&Hook_SkinPiece) - (kSkinPiece + 5));
            sk[5] = 0x90;
            VirtualProtect(sk, 6, o2, &o2);
        }
    }
    LoadHolds();
}
