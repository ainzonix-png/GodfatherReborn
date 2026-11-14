// gf_ragdoll_jolt.cpp -- v1056A: rigid-body ragdoll core on Jolt Physics (replaces gf_ragdoll.c's particle solver).
//
// gf_ragdoll.c still does everything around the physics (hooks, learned rig, hit analysis, corpse hand-off, pose
// writing, recorder). This file owns one Jolt PhysicsSystem per body: 11 rigid parts (pelvis, chest, head, upper /
// lower arms incl. hands, thighs, shins incl. feet) joined by anatomical joints -- swing-twist at spine, neck,
// shoulders and hips (cones anchored on the torso, not on the death pose), hinges at elbows and knees -- on a ground
// box at the feet. Muscle tone: per-part torques towards the game's live animation pose (relative to the parent part),
// equal and opposite on the parent, strength from gf_ragdoll.c's tone schedule. All in the body's start frame
// ("death frame", metres, +y up), the same frame gf_ragdoll.c's particles live in.
//
// C API (floats: v3 = 3 floats; m3 = 9 floats row-major, column-vector convention, like gf_ragdoll.c's m3):
//   jr_create(p0, pv, dt, ground, kn_sign, el_sign)   p0 / pv: 21 joint particles now / one frame before (velocity)
//   jr_tone(h, anim21 or NULL, tone_spine, tone_arms, tone_legs, shot_leg)
//   jr_pull(h, part, point, target, k)               soft pull of a point of a part towards a target (hand on wound)
//   jr_step(h, dt)
//   jr_get(h, Q11, X11, p21) -> 1 when all parts sleep
//   jr_destroy(h)
// v1069H live hit reactions (NPC still alive): jr_hr_create(p0, ground) builds the same 11-part body in MODEL space with
//   the pelvis KINEMATIC (it follows the game's animation), no gravity, no self-collision; every other part is pulled
//   towards the animation's pose relative to its parent by exact-inertia PD muscles (jr_hr_target: anim pose + tone per
//   part + a sway rotation about the feet), jr_impulse() kicks a part at the bullet's hit point, jr_hr_get() returns per
//   part the rotation C that takes the animation's part onto the simulated one (+ both joint positions) so the caller
//   corrects the game's own pose (anim + physics offset). jr_impulse() also works on a dying (death) ragdoll.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>   // v1057C level collision
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace JPH;

namespace {
enum { PEL, LUM, THO, NEC, HED, SHL, ELL, WRL, SHR, ELR, WRR, HIL, KNL, ANL, HIR, KNR, ANR, TOL, TOR, FIL, FIR, NP };
enum { D_LT, D_UT, D_HEAD, D_UAL, D_LAL, D_UAR, D_LAR, D_ULL, D_LLL, D_ULR, D_LLR, ND };
// particle -> part that carries it
const int k_owner[NP] = { D_LT, D_LT, D_UT, D_UT, D_HEAD, D_UT, D_LAL, D_LAL, D_UT, D_LAR, D_LAR, D_LT, D_LLL, D_LLL, D_LT, D_LLR, D_LLR,
                          D_LLL, D_LLR, D_LAL, D_LAR };
const int k_parent[ND] = { -1, D_LT, D_UT, D_UT, D_UAL, D_UT, D_UAR, D_LT, D_ULL, D_LT, D_ULR };
const int k_joint[ND] = { PEL, THO, NEC, SHL, ELL, SHR, ELR, HIL, KNL, HIR, KNR };   // the particle at the part's joint (gf_ragdoll.c k_dj)
const float k_mass[ND] = { 11.f, 20.f, 5.f, 2.5f, 2.f, 2.5f, 2.f, 9.f, 5.f, 9.f, 5.f };   // ~73 kg
const int k_group[ND] = { 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2 };   // tone group: spine/head, arms, legs

namespace Layers { const ObjectLayer STATIC = 0, MOVING = 1, NUM = 2; }
namespace BPL { const BroadPhaseLayer STATIC(0), MOVING(1); const uint NUM = 2; }
class BPLayerIF final : public BroadPhaseLayerInterface {
public:
    uint GetNumBroadPhaseLayers() const override { return BPL::NUM; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l) const override { return l == Layers::STATIC ? BPL::STATIC : BPL::MOVING; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer l) const override { return l == BPL::STATIC ? "STATIC" : "MOVING"; }
#endif
};
class OVBPFilter final : public ObjectVsBroadPhaseLayerFilter {
public: bool ShouldCollide(ObjectLayer a, BroadPhaseLayer b) const override { return a == Layers::MOVING || b == BPL::MOVING; }
};
class OLPFilter final : public ObjectLayerPairFilter {
public: bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override { return a == Layers::MOVING || b == Layers::MOVING; }
};

bool g_inited = false;
TempAllocatorImpl* g_temp = nullptr;
JobSystemSingleThreaded* g_jobs = nullptr;
BPLayerIF g_bpl; OVBPFilter g_ovbp; OLPFilter g_olp;

inline Vec3 V3(const float* f) { return Vec3(f[0], f[1], f[2]); }
inline Vec3 nrm(Vec3 v) { const float l = v.Length(); return l > 1e-6f ? v / l : Vec3::sAxisY(); }
// orthonormal frame: x along a, y = b made perpendicular, z = x cross y  (matches gf_ragdoll.c frame(): columns e1 e2 e3)
Mat44 frame(Vec3 a, Vec3 b)
{
    Vec3 e1 = nrm(a), e2 = b - e1 * e1.Dot(b);
    if (e2.Length() < 1e-5f) e2 = e1.Cross(std::fabs(e1.GetY()) < 0.9f ? Vec3::sAxisY() : Vec3::sAxisX());
    e2 = e2.Normalized(); const Vec3 e3 = e1.Cross(e2);
    return Mat44(Vec4(e1, 0), Vec4(e2, 0), Vec4(e3, 0), Vec4(0, 0, 0, 1));
}
// rotation of a part from particles (its long axis + a reference direction)
Mat44 part_frame(int d, const Vec3* p)
{
    switch (d) {
    case D_LT: return frame(p[LUM] - p[PEL], p[HIR] - p[HIL]);
    case D_UT: return frame(p[NEC] - p[THO], p[SHR] - p[SHL]);
    case D_HEAD: return frame(p[HED] - p[NEC], p[SHR] - p[SHL]);
    case D_UAL: return frame(p[ELL] - p[SHL], p[WRL] - p[ELL]);
    case D_LAL: return frame(p[WRL] - p[ELL], p[FIL] - p[WRL] + (p[ELL] - p[SHL]) * 0.3f);
    case D_UAR: return frame(p[ELR] - p[SHR], p[WRR] - p[ELR]);
    case D_LAR: return frame(p[WRR] - p[ELR], p[FIR] - p[WRR] + (p[ELR] - p[SHR]) * 0.3f);
    case D_ULL: return frame(p[KNL] - p[HIL], p[ANL] - p[KNL] + (p[HIR] - p[HIL]) * 0.2f);
    case D_LLL: return frame(p[ANL] - p[KNL], p[TOL] - p[ANL]);
    case D_ULR: return frame(p[KNR] - p[HIR], p[ANR] - p[KNR] + (p[HIL] - p[HIR]) * 0.2f);
    default:    return frame(p[ANR] - p[KNR], p[TOR] - p[ANR]);
    }
}
Quat q_of(const Mat44& m) { return m.GetQuaternion().Normalized(); }

struct Rag {
    PhysicsSystem sys;
    BodyID body[ND];
    Constraint* con[ND] = {};
    Quat R0[ND];           // part rotation at the start (world)
    Vec3 c0[ND];           // part body position at the start
    Vec3 p0[NP];           // particles at the start
    Quat F0rel[ND];        // part frame -> body rotation offset (part_frame * F0rel = body rotation)
    float inertia[ND];
    float tone[3] = { 0, 0, 0 }; int shot_leg = -1;
    Vec3 anim[NP]; bool have_anim = false;
    Ref<GroupFilterTable> gft;   // v1091F: kept to enable the shoulder / neck pairs once the body lies clear
    BodyID ground;         // v1057C: the flat floor box (lowered to a safety net once the level mesh is in)
    // v1069H hit reaction
    bool hr = false;
    float tone11[ND] = {};
    Quat qa_prev[ND]; bool have_prev = false; Vec3 wa[ND];
    float fmul[ND] = {};   // motor frequency correction: sqrt(inertia of the part + all it carries / the part alone), about its joint
    bool hinge[ND] = {}; Vec3 hax1[ND]; float hth0[ND] = {};   // hinge: axis in body 1's space, angle at creation
    RVec3 kin_pos; Quat kin_rot = Quat::sIdentity(); bool have_kin = false;
    // v1069J additive: the body simulates only the bullet's push on the pose frozen at the hit; the output adds each
    // joint's deviation from that pose on top of the live animation (no lag, never fights the game's pain animation)
    Vec3 sway = Vec3::sZero(), pivot = Vec3::sZero();
    Quat out_q[ND]; Vec3 out_x[ND];   // last displayed part rotations / joint positions (to map new bullets into the body)
    Vec3 extra[ND]; Vec3 root_off = Vec3::sZero();
    Vec3 base[NP]; bool have_base = false;
    // v1069C car: a kinematic box pair moved to the real car every step (death ragdolls hit by the player's car)
    BodyID car; bool have_car = false; RVec3 car_pos; Quat car_rot = Quat::sIdentity(); bool car_tgt = false;   // v1069T: pose the output is built from (default = the live anim)   // v1069S stumble program: world axis*angle per part (about its joint) + root shift
};

void init_once()
{
    if (g_inited) return;
    RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();
    g_temp = new TempAllocatorImpl(4 * 1024 * 1024);
    g_jobs = new JobSystemSingleThreaded(cMaxPhysicsJobs);
    g_inited = true;
}

// capsule between two points (in world space) as a shape positioned relative to the body centre c with body rotation q
RefConst<Shape> capsule_between(Vec3 a, Vec3 b, float r, Vec3 c, Quat q)
{
    const Vec3 ab = b - a; const float L = ab.Length();
    const float hh = std::max(0.01f, L * 0.5f - r * 0.5f);
    RefConst<Shape> cap = new CapsuleShape(hh, r);
    const Quat rot = Quat::sFromTo(Vec3::sAxisY(), L > 1e-5f ? ab / L : Vec3::sAxisY());
    const Quat qi = q.Conjugated();
    return RotatedTranslatedShapeSettings(qi * (a + ab * 0.5f - c), qi * rot, cap).Create().Get();
}
RefConst<Shape> box_at(Vec3 centre, Vec3 half, Quat rot, Vec3 c, Quat q)
{
    RefConst<Shape> bx = new BoxShape(Vec3::sMax(half, Vec3::sReplicate(0.03f)), 0.02f);
    const Quat qi = q.Conjugated();
    return RotatedTranslatedShapeSettings(qi * (centre - c), qi * rot, bx).Create().Get();
}
} // namespace

extern "C" {

static Rag* build(const float* p0f, const float* pvf, float dt, float ground, float kn_sign, float el_sign, bool hr)
{
    init_once();
    Rag* R = new Rag(); R->hr = hr;
    Vec3 p[NP], pv[NP];
    for (int i = 0; i < NP; i++) { p[i] = V3(p0f + 3 * i); pv[i] = V3(pvf + 3 * i); R->p0[i] = p[i]; }
    R->sys.Init(64, 0, 256, 256, g_bpl, g_ovbp, g_olp);
    R->sys.SetGravity(Vec3(0, -9.81f, 0));
    { PhysicsSettings ps = R->sys.GetPhysicsSettings(); ps.mNumVelocitySteps = 12; ps.mNumPositionSteps = 4; ps.mTimeBeforeSleep = 0.6f; R->sys.SetPhysicsSettings(ps); }
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    // ground: a big static box whose top is at y = ground (not for a hit reaction: the feet follow the animation)
    if (!hr) { BodyCreationSettings g(new BoxShape(Vec3(60, 1, 60)), RVec3(p[PEL].GetX(), ground - 1.0f, p[PEL].GetZ()), Quat::sIdentity(), EMotionType::Static, Layers::STATIC);
      g.mFriction = 0.9f; g.mRestitution = 0.0f; R->ground = bi.CreateAndAddBody(g, EActivation::DontActivate); }

    // body frame, centre and shape per part
    Ref<GroupFilterTable> gft = new GroupFilterTable(ND);
    // v1056B self-collision: every pair of parts collides except the ones joined directly (and the two torso halves
    // with the upper arms / head, which share the shoulder / neck region) -- legs no longer cross, arms stay out of the body
    for (int d = 1; d < ND; d++) gft->DisableCollision(d, k_parent[d]);
    gft->DisableCollision(D_LT, D_UAL); gft->DisableCollision(D_LT, D_UAR); gft->DisableCollision(D_HEAD, D_UAL); gft->DisableCollision(D_HEAD, D_UAR);
    gft->DisableCollision(D_LT, D_HEAD);
    if (hr) for (int a = 0; a < ND; a++) for (int b = a + 1; b < ND; b++) gft->DisableCollision(a, b);   // v1069H: the anim's own pose may overlap
    R->gft = gft;
    const Vec3 up = nrm(p[LUM] - p[PEL]), lat = nrm(p[HIR] - p[HIL]);
    for (int d = 0; d < ND; d++) {
        const Mat44 F = part_frame(d, p);
        const Quat q = q_of(F);
        Vec3 c; RefConst<Shape> sh;
        switch (d) {
        case D_LT: {   // pelvis: box over the hips
            c = (p[PEL] + p[LUM] + p[HIL] + p[HIR]) * 0.25f;
            const float w = (p[HIR] - p[HIL]).Length() * 0.5f + 0.05f, h = std::max(0.08f, (p[LUM] - p[PEL]).Length() * 0.5f + 0.06f);
            sh = box_at(c, Vec3(h, w, 0.10f), q, c, q); break; }
        case D_UT: {   // chest: box from the lumbar-thoracic joint to the neck
            c = (p[THO] * 2.0f + p[NEC] + (p[SHL] + p[SHR]) * 0.5f) * 0.25f;
            const float w = std::max(0.10f, (p[SHR] - p[SHL]).Length() * 0.5f - 0.03f), h = std::max(0.12f, (p[NEC] - p[THO]).Length() * 0.5f + 0.08f);
            sh = box_at(c, Vec3(h, w, 0.11f), q, c, q); break; }   // slimmer than the shoulders: hanging arms start outside it
        case D_HEAD: { c = p[NEC] * 0.5f + p[HED] * 0.5f; sh = capsule_between(p[NEC] + (p[HED] - p[NEC]) * 0.2f, p[HED], 0.1f, c, q); break; }
        case D_UAL: c = (p[SHL] + p[ELL]) * 0.5f; sh = capsule_between(p[SHL], p[ELL], hr ? 0.055f : 0.062f, c, q); break;   // v1091F: closer to the drawn sleeve (limbs sank into the body)
        case D_UAR: c = (p[SHR] + p[ELR]) * 0.5f; sh = capsule_between(p[SHR], p[ELR], hr ? 0.055f : 0.062f, c, q); break;
        case D_LAL: c = (p[ELL] + p[WRL]) * 0.5f; sh = capsule_between(p[ELL], p[FIL], hr ? 0.04f : 0.05f, c, q); break;   // forearm + hand
        case D_LAR: c = (p[ELR] + p[WRR]) * 0.5f; sh = capsule_between(p[ELR], p[FIR], hr ? 0.04f : 0.05f, c, q); break;
        case D_ULL: c = (p[HIL] + p[KNL]) * 0.5f; sh = capsule_between(p[HIL], p[KNL], 0.07f, c, q); break;
        case D_ULR: c = (p[HIR] + p[KNR]) * 0.5f; sh = capsule_between(p[HIR], p[KNR], 0.07f, c, q); break;
        case D_LLL: case D_LLR: {   // shin + foot (compound)
            const int kn = d == D_LLL ? KNL : KNR, an = d == D_LLL ? ANL : ANR, to = d == D_LLL ? TOL : TOR;
            c = (p[kn] + p[an]) * 0.5f;
            StaticCompoundShapeSettings cs;
            const Quat qi = q.Conjugated();
            { const Vec3 ab = p[an] - p[kn]; const float L = ab.Length(), r = 0.055f;
              cs.AddShape(qi * (p[kn] + ab * 0.5f - c), qi * Quat::sFromTo(Vec3::sAxisY(), L > 1e-5f ? ab / L : Vec3::sAxisY()), new CapsuleShape(std::max(0.01f, L * 0.5f - r * 0.5f), r)); }
            { Vec3 ft = p[to] - p[an]; if (ft.Length() < 0.06f) ft = nrm(p[an] - p[kn]).Cross(lat) * 0.15f;
              const Vec3 fc = p[an] + ft * 0.5f; const Quat fr = Quat::sFromTo(Vec3::sAxisX(), nrm(ft));
              cs.AddShape(qi * (fc - c), qi * fr, new BoxShape(Vec3(std::max(0.05f, ft.Length() * 0.5f), 0.035f, 0.045f), 0.01f)); }
            sh = cs.Create().Get(); break; }
        }
        BodyCreationSettings bs(sh, RVec3(c), q, (hr && d == D_LT) ? EMotionType::Kinematic : EMotionType::Dynamic, Layers::MOVING);
        bs.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
        bs.mMassPropertiesOverride.mMass = k_mass[d];
        bs.mFriction = 0.85f; bs.mRestitution = 0.0f;
        bs.mLinearDamping = 0.05f; bs.mAngularDamping = 0.25f;
        bs.mMotionQuality = EMotionQuality::LinearCast;
        bs.mCollisionGroup = CollisionGroup(gft, 0, (CollisionGroup::SubGroupID)d);
        bs.mAllowSleeping = !hr;
        if (hr) { bs.mGravityFactor = 0.0f; bs.mAngularDamping = 0.4f; bs.mLinearDamping = 0.2f; bs.mMotionQuality = EMotionQuality::Discrete; }
        // start motion: least-squares fit of the particles this part carries (velocity = (p - pv) / dt)
        Vec3 vsum = Vec3::sZero(), csum = Vec3::sZero(); int n = 0;
        for (int i = 0; i < NP; i++) if (k_owner[i] == d || k_joint[d] == i) { vsum += (p[i] - pv[i]) / dt; csum += p[i]; n++; }
        if (n) {
            const Vec3 vm = vsum / (float)n, cm = csum / (float)n; Vec3 w = Vec3::sZero(); float den = 0;
            for (int i = 0; i < NP; i++) if (k_owner[i] == d || k_joint[d] == i) { const Vec3 r = p[i] - cm; w += r.Cross((p[i] - pv[i]) / dt - vm); den += r.LengthSq(); }
            bs.mLinearVelocity = vm;
            bs.mAngularVelocity = den > 1e-4f ? w / den : Vec3::sZero();
            if (bs.mAngularVelocity.Length() > 12.0f) bs.mAngularVelocity = bs.mAngularVelocity.Normalized() * 12.0f;
            if (bs.mLinearVelocity.Length() > 22.0f) bs.mLinearVelocity = bs.mLinearVelocity.Normalized() * 22.0f;   // v1069C: car hits (was 8)
        }
        R->body[d] = bi.CreateAndAddBody(bs, EActivation::Activate);
        R->R0[d] = q; R->c0[d] = c;
        R->inertia[d] = k_mass[d] * 0.02f;
    }
    // joints
    const Mat44 Flt = part_frame(D_LT, p), Fut = part_frame(D_UT, p);
    const Vec3 ut_up = nrm(p[NEC] - p[THO]), ut_lat = nrm(p[SHR] - p[SHL]);
    // v1069H FIX: the anterior (front) is up x (left hip -> right hip) -- anatomical, whatever the pose. gf_ragdoll.c's
    // pose-based kn_sign measured along a body axis that points BACKWARDS, so every death so far had its knee / elbow
    // hinges and hip cones mirrored: the 11 recorded deaths started 33..142 deg outside the knee limits and Jolt wrenched
    // forearms / shins 38-54 deg (upper arms / thighs 14-20) in the first 50 ms. kn_sign is ignored now; the sign is the
    // one that puts the start pose's knees and elbows inside their limits (anatomical on ties).
    (void)kn_sign;
    auto hinge_ok = [&](float sgn) {
        Vec3 a2 = up.Cross(lat) * sgn; a2 = nrm(a2 - up * up.Dot(a2));
        Vec3 au = nrm(p[SHR] - p[SHL]).Cross(nrm(p[NEC] - p[THO])); if (au.Dot(a2) < 0) au = -au; au = nrm(au);
        int n = 0;
        for (int s = 0; s < 2; s++) {
            const int hi = s ? HIR : HIL, kn = s ? KNR : KNL, an = s ? ANR : ANL, sh = s ? SHR : SHL, el = s ? ELR : ELL, wr = s ? WRR : WRL;
            const Vec3 th = nrm(p[kn] - p[hi]), sn = nrm(p[an] - p[kn]); const Vec3 kx = nrm(th.Cross(-a2));
            const Vec3 ua = nrm(p[el] - p[sh]), fa = nrm(p[wr] - p[el]); const Vec3 ex = nrm(ua.Cross(au));
            const float ka = std::atan2(th.Cross(sn).Dot(kx), th.Dot(sn)), ea = std::atan2(ua.Cross(fa).Dot(ex), ua.Dot(fa));
            n += (ka > -0.10f) + (ea > -0.12f);
        }
        return n;
    };
    const float asign = hinge_ok(-1.0f) > hinge_ok(1.0f) ? -1.0f : 1.0f;
    { static int nlog = 0; if (nlog < 6) { nlog++; printf("[rag] v1069H joints: front = %s (knees/elbows inside limits: %d anatomical, %d mirrored)%s\n",
        asign > 0 ? "anatomical" : "MIRRORED", hinge_ok(1.0f), hinge_ok(-1.0f), hr ? " [hit reaction]" : ""); fflush(stdout); } }
    Vec3 ant = up.Cross(lat) * asign; ant = nrm(ant - up * up.Dot(ant));   // anterior (front) of the pelvis
    Vec3 ant_u = ut_lat.Cross(ut_up); if (ant_u.Dot(ant) < 0) ant_u = -ant_u; ant_u = nrm(ant_u);
    (void)Flt; (void)Fut; (void)el_sign;
    BodyLockInterfaceNoLock& bl = (BodyLockInterfaceNoLock&)R->sys.GetBodyLockInterfaceNoLock();
    auto make_st = [&](int d, Vec3 pos, Vec3 tw1, Vec3 pl1, Vec3 tw2, Vec3 pl2, float twist, float normal_cone, float plane_cone) {
        if (hr) {   // v1069H: a living body's animation is always a valid pose -- limits centred on it and wide, the
                    // motors (jr_hr_step) hold the shape
            const bool core = (d == D_UT || d == D_HEAD), mid = (d == D_LAL || d == D_LAR || d == D_LLL || d == D_LLR);
            tw1 = tw2; pl1 = pl2; twist = core ? 1.2f : mid ? 1.0f : 2.6f; normal_cone = plane_cone = (core || mid) ? 1.2f : 2.6f; }
        SwingTwistConstraintSettings s;
        s.mSpace = EConstraintSpace::WorldSpace;
        s.mPosition1 = s.mPosition2 = RVec3(pos);
        s.mTwistAxis1 = nrm(tw1); s.mPlaneAxis1 = nrm(pl1 - s.mTwistAxis1 * s.mTwistAxis1.Dot(pl1));
        s.mTwistAxis2 = nrm(tw2); s.mPlaneAxis2 = nrm(pl2 - s.mTwistAxis2 * s.mTwistAxis2.Dot(pl2));
        s.mTwistMinAngle = -twist; s.mTwistMaxAngle = twist;
        s.mNormalHalfConeAngle = normal_cone; s.mPlaneHalfConeAngle = plane_cone;
        s.mMaxFrictionTorque = 2.0f;
        Body* b1 = bl.TryGetBody(R->body[k_parent[d]]); Body* b2 = bl.TryGetBody(R->body[d]);
        R->con[d] = s.Create(*b1, *b2); R->sys.AddConstraint(R->con[d]);
    };
    auto make_hinge = [&](int d, Vec3 pos, Vec3 axis, Vec3 n1, Vec3 n2, float mn, float mx) {
        if (hr) { make_st(d, pos, n2, axis, n2, axis, 0, 0, 0); return; }   // v1069H: elbows / knees follow the anim in 3D
        HingeConstraintSettings s;
        s.mSpace = EConstraintSpace::WorldSpace;
        s.mPoint1 = s.mPoint2 = RVec3(pos);
        s.mHingeAxis1 = s.mHingeAxis2 = nrm(axis);
        s.mNormalAxis1 = nrm(n1 - s.mHingeAxis1 * s.mHingeAxis1.Dot(n1));
        s.mNormalAxis2 = nrm(n2 - s.mHingeAxis2 * s.mHingeAxis2.Dot(n2));
        s.mLimitsMin = mn; s.mLimitsMax = mx; s.mMaxFrictionTorque = 1.0f;
        Body* b1 = bl.TryGetBody(R->body[k_parent[d]]); Body* b2 = bl.TryGetBody(R->body[d]);
        R->con[d] = s.Create(*b1, *b2); R->sys.AddConstraint(R->con[d]);
        R->hinge[d] = true; R->hax1[d] = b1->GetRotation().Conjugated() * s.mHingeAxis1; R->hth0[d] = ((HingeConstraint*)R->con[d])->GetCurrentAngle();
    };
    const float deg = JPH_PI / 180.0f;
    // spine (pelvis -> chest) at the lumbar/thoracic joint: bend 40 front/back, 25 sideways, twist 30
    make_st(D_UT, (p[LUM] + p[THO]) * 0.5f, up, lat, ut_up, ut_lat, 30 * deg, 40 * deg, 25 * deg);
    // neck (chest -> head)
    make_st(D_HEAD, p[NEC], ut_up, ut_lat, p[HED] - p[NEC], ut_lat, 50 * deg, 40 * deg, 30 * deg);   // v1056C: tighter (head dropped back too far)
    // shoulders: cone centred between hanging and sideways, slightly forward
    for (int s = 0; s < 2; s++) {
        const int sh = s ? SHR : SHL, el = s ? ELR : ELL; const Vec3 out = s ? ut_lat : -ut_lat;
        make_st(s ? D_UAR : D_UAL, p[sh], -ut_up * 0.55f + out * 0.75f + ant_u * 0.35f, ant_u, p[el] - p[sh], ant_u, 70 * deg, 85 * deg, 80 * deg);
    }
    // hips: cone centred 45 deg forward of straight down -> -30 .. +120 flexion, 40 abduction
    for (int s = 0; s < 2; s++) {
        const int hi = s ? HIR : HIL, kn = s ? KNR : KNL; const Vec3 out = s ? lat : -lat;
        make_st(s ? D_ULR : D_ULL, p[hi], -up * 0.707f + ant * 0.707f, out, p[kn] - p[hi], out, 40 * deg, 75 * deg, 40 * deg);
    }
    // elbows / knees: hinges, angle 0 = straight; elbows bend to the front, knees to the back
    for (int s = 0; s < 2; s++) {
        const int sh = s ? SHR : SHL, el = s ? ELR : ELL, wr = s ? WRR : WRL;
        const Vec3 ua = nrm(p[el] - p[sh]), fa = nrm(p[wr] - p[el]);
        Vec3 ax = ua.Cross(ant_u); if (ax.Length() < 0.2f) ax = s ? ut_lat : -ut_lat;
        make_hinge(s ? D_LAR : D_LAL, p[el], ax, ua, fa, -5 * deg, 150 * deg);
        const int hi = s ? HIR : HIL, kn = s ? KNR : KNL, an = s ? ANR : ANL;
        const Vec3 th = nrm(p[kn] - p[hi]), sn = nrm(p[an] - p[kn]);
        Vec3 kx = th.Cross(-ant); if (kx.Length() < 0.2f) kx = s ? lat : -lat;
        make_hinge(s ? D_LLR : D_LLL, p[kn], kx, th, sn, -3 * deg, 150 * deg);
    }
    if (hr) for (int d = 1; d < ND; d++) {   // v1069H: Jolt sizes a motor's spring for the two bodies it joins; a shoulder
        // also swings the forearm -> scale the frequency so the whole chain moves at the frequency asked for
        const Vec3 j = p[k_joint[d]]; float Is = 0.0f, Iself = 0.0f;
        for (int k = 0; k < ND; k++) {
            int a = k; while (a >= 0 && a != d) a = k_parent[a];
            if (a != d) continue;
            const int tip = k == D_HEAD ? HED : k == D_UAL ? ELL : k == D_UAR ? ELR : k == D_LAL ? FIL : k == D_LAR ? FIR : k == D_ULL ? KNL : k == D_ULR ? KNR :
                            k == D_LLL ? TOL : k == D_LLR ? TOR : NEC;
            const float L = (p[tip] - p[k_joint[k]]).Length();
            const float I = k_mass[k] * ((R->c0[k] - j).LengthSq() + L * L / 12.0f);
            Is += I; if (k == d) Iself = I;
        }
        R->fmul[d] = std::max(1.0f, std::min(4.0f, std::sqrt(Is / std::max(1e-4f, Iself))));
    }
    if (hr) for (int d = 1; d < ND; d++) {   // v1069H: position motors, start target = the start pose
        Constraint* c = R->con[d]; if (!c) continue;
        if (R->hinge[d]) { HingeConstraint* h = (HingeConstraint*)c; h->GetMotorSettings() = MotorSettings(8.0f, 1.0f, FLT_MAX, 500.0f);
            h->SetMotorState(EMotorState::Position); h->SetTargetAngle(R->hth0[d]); }
        else { SwingTwistConstraint* st = (SwingTwistConstraint*)c; st->GetSwingMotorSettings() = MotorSettings(8.0f, 1.0f, FLT_MAX, 500.0f);
            st->GetTwistMotorSettings() = st->GetSwingMotorSettings();
            st->SetSwingMotorState(EMotorState::Position); st->SetTwistMotorState(EMotorState::Position);
            st->SetTargetOrientationBS(R->R0[k_parent[d]].Conjugated() * R->R0[d]); }
    }
    R->sys.OptimizeBroadPhase();
    return R;
}

void* jr_create(const float* p0f, const float* pvf, float dt, float ground, float kn_sign, float el_sign)
{
    return build(p0f, pvf, dt, ground, kn_sign, el_sign, false);
}

void jr_tone(void* h, const float* anim, float t_spine, float t_arms, float t_legs, int shot_leg)
{
    Rag* R = (Rag*)h; if (!R) return;
    R->tone[0] = t_spine; R->tone[1] = t_arms; R->tone[2] = t_legs; R->shot_leg = shot_leg;
    R->have_anim = anim != nullptr;
    if (anim) for (int i = 0; i < NP; i++) R->anim[i] = V3(anim + 3 * i);
}

// muscles: torque on each part towards the animation's orientation relative to its parent (and the reverse on the
// parent), a stiff-ish PD with gains from the part's inertia, scaled by the tone of its group
static void apply_muscles(Rag* R)
{
    if (!R->have_anim) return;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    Quat qa[ND];
    for (int d = 0; d < ND; d++) qa[d] = q_of(part_frame(d, R->anim));
    for (int d = 1; d < ND; d++) {
        float k = R->tone[k_group[d]];
        if (k_group[d] == 2 && R->shot_leg >= 0 && ((R->shot_leg == 0) == (d == D_ULL || d == D_LLL))) k *= 0.05f;
        if (k <= 0.002f) continue;
        const int pd = k_parent[d];
        // current rotations of parent / part (bodies carry the part frame times a fixed offset: use the start offset)
        const Quat qp = bi.GetRotation(R->body[pd]) * R->R0[pd].Conjugated();   // parent's rotation since the start
        const Quat qc = bi.GetRotation(R->body[d]) * R->R0[d].Conjugated();
        // desired: the part's start-relative orientation as the animation has it, relative to the parent
        const Quat q0p = R->R0[pd], q0c = R->R0[d];
        const Quat want = qp * q0p * (qa[pd].Conjugated() * qa[d]) * q0c.Conjugated();   // rotation since start the part should have
        Quat err = want * qc.Conjugated(); if (err.GetW() < 0) err = -err;
        Vec3 axis; float ang; err.GetAxisAngle(axis, ang);
        const Vec3 wc = bi.GetAngularVelocity(R->body[d]), wp = bi.GetAngularVelocity(R->body[pd]);
        const float I = R->inertia[d] * 4.0f, w0 = 2.0f * JPH_PI * 4.0f;   // ~4 Hz at full tone
        Vec3 tq = (axis * ang * (I * w0 * w0) - (wc - wp) * (I * 2.0f * w0 * 0.7f)) * k;
        const float tmax = 400.0f * k; if (tq.Length() > tmax) tq = tq.Normalized() * tmax;
        bi.AddTorque(R->body[d], tq); bi.AddTorque(R->body[pd], -tq);
    }
}

void jr_pull(void* h, int part, const float* point_now, const float* target, float k)
{
    Rag* R = (Rag*)h; if (!R || part < 0 || part >= ND) return;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    const Vec3 pt = V3(point_now), tg = V3(target);
    const Vec3 f = (tg - pt) * (k * 400.0f) - bi.GetLinearVelocity(R->body[part]) * (k * 40.0f);
    bi.AddForce(R->body[part], f, RVec3(pt));
    const int rp = k_parent[part] >= 0 ? (part == D_LAL || part == D_LAR ? D_UT : k_parent[part]) : D_LT;
    bi.AddForce(R->body[rp], -f);   // reaction on the torso
    // v1080B: no longer wakes a sleeping body (v1075R did: settled corpses got nudged -- user "moved around when dead")
}

// v1075R (Claude) BALANCE ASSIST: an EXTERNAL force (no reaction on the body) on a point of a part: PD towards a target
// (kp N/m, kd N s/m) plus a feed-forward force ff (N, world). This is the "virtual support" Euphoria-style balance uses to
// keep a dying man on his feet for a moment (gf_ragdoll_bhv.c); muscles alone only bend joints. kp = kd = 0 -> ff only.
void jr_assist(void* h, int part, const float* point_now, const float* target, float kp, float kd, const float* ff)
{
    Rag* R = (Rag*)h; if (!R || part < 0 || part >= ND) return;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    const Vec3 pt = V3(point_now), tg = V3(target);
    Vec3 f = (tg - pt) * kp - bi.GetPointVelocity(R->body[part], RVec3(pt)) * kd;
    if (ff) f += V3(ff);
    const float fm = f.Length(); if (fm > 2500.0f) f *= 2500.0f / fm;   // never a launch
    bi.AddForce(R->body[part], f, RVec3(pt));
    if (!bi.IsActive(R->body[part])) bi.ActivateBody(R->body[part]);
}
// v1075R: damp the whole body's spin about the vertical (k 0..1 per call, scaled by dt) -- a dying man does not pirouette
void jr_yaw_damp(void* h, float k)
{
    Rag* R = (Rag*)h; if (!R) return;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    for (int d = 0; d < ND; d++) {
        const Vec3 w = bi.GetAngularVelocity(R->body[d]);
        bi.SetAngularVelocity(R->body[d], Vec3(w.GetX(), w.GetY() * (1.0f - k), w.GetZ()));
    }
}
// v1075R: external torque on a part (heading hold of gf_ragdoll_bhv.c: pure torque, no net force)
void jr_torque(void* h, int part, const float* tq)
{
    Rag* R = (Rag*)h; if (!R || part < 0 || part >= ND) return;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    Vec3 t = V3(tq); const float m = t.Length(); if (m > 600.0f) t *= 600.0f / m;
    bi.AddTorque(R->body[part], t);
}
// v1075R: spin rate of a part about +y (rad/s)
float jr_yaw_rate(void* h, int part)
{
    Rag* R = (Rag*)h; if (!R || part < 0 || part >= ND) return 0.0f;
    return R->sys.GetBodyInterfaceNoLock().GetAngularVelocity(R->body[part]).GetY();
}
// v1075R: velocity of a point of a part (balance feedback)
void jr_part_vel(void* h, int part, const float* point, float* vout)
{
    Rag* R = (Rag*)h; vout[0] = vout[1] = vout[2] = 0.0f; if (!R || part < 0 || part >= ND) return;
    const Vec3 v = R->sys.GetBodyInterfaceNoLock().GetPointVelocity(R->body[part], RVec3(V3(point)));
    vout[0] = v.GetX(); vout[1] = v.GetY(); vout[2] = v.GetZ();
}

void jr_step(void* h, float dt)
{
    Rag* R = (Rag*)h; if (!R) return;
    if (dt > 0.05f) dt = 0.05f;
    if (R->have_car && R->car_tgt) { R->sys.GetBodyInterfaceNoLock().MoveKinematic(R->car, R->car_pos, R->car_rot, dt); R->car_tgt = false; }   // v1069C
    apply_muscles(R);
    const int steps = dt > 1.0f / 50.0f ? 2 : 1;
    R->sys.Update(dt, steps, g_temp, g_jobs);
}

int jr_get(void* h, float* Q, float* X, float* pout)
{
    Rag* R = (Rag*)h; if (!R) return 0;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    int asleep = 1;
    Mat44 M[ND];
    for (int d = 0; d < ND; d++) {
        const Quat q = bi.GetRotation(R->body[d]); const Vec3 c = Vec3(bi.GetCenterOfMassPosition(R->body[d]) - (q * Vec3::sZero()));
        (void)c;
        M[d] = Mat44::sRotationTranslation(q, Vec3(bi.GetPosition(R->body[d])));
        if (bi.IsActive(R->body[d])) asleep = 0;
        const Mat44 rel = Mat44::sRotation(q * R->R0[d].Conjugated());   // rotation since the start
        for (int r = 0; r < 3; r++) for (int cc = 0; cc < 3; cc++) Q[d * 9 + r * 3 + cc] = rel(r, cc);
    }
    // a part maps its start-pose points: x -> M (M0^-1 x)
    auto map = [&](int d, Vec3 x) { const Mat44 M0 = Mat44::sRotationTranslation(R->R0[d], R->c0[d]); return M[d] * (M0.InversedRotationTranslation() * x); };
    for (int d = 0; d < ND; d++) { const Vec3 x = map(d, R->p0[k_joint[d]]); X[d * 3] = x.GetX(); X[d * 3 + 1] = x.GetY(); X[d * 3 + 2] = x.GetZ(); }
    if (pout) for (int i = 0; i < NP; i++) { const Vec3 x = map(k_owner[i], R->p0[i]); pout[i * 3] = x.GetX(); pout[i * 3 + 1] = x.GetY(); pout[i * 3 + 2] = x.GetZ(); }
    return asleep;
}

void jr_destroy(void* h) { delete (Rag*)h; }

// v1057C: the level's own collision around the body (gf_ragdoll.c, from assets/gf_collision.bin, already in this
// body's simulation frame) as one static mesh; drop_floor > 0 lowers the flat floor box by that much (the mesh is the
// real ground now -- kerbs, steps, slopes -- and the box only catches a fall through a gap).
void jr_add_mesh(void* h, const float* tri, int n, float drop_floor)
{
    Rag* R = (Rag*)h;
    if (!R || !tri || n <= 0) return;
    TriangleList tl; tl.reserve((size_t)n);
    // v1113W (user: "I kept shooting this dude and his body went into the building"): Jolt ignores BACK faces in the simulation
    // (CollideShapeSettings::mBackFaceMode = IgnoreBackFaces), and the game's Havok collision often models a building only from
    // the inside (store interiors: the wall at x -154.26 on Canal St. faces +x into the shop, the street facade has no collision).
    // A body lying against such a wall outside was behind a one-sided triangle, and every shot pushed it further in. Steep
    // triangles (|normal.y| < 0.7 = walls, kerb faces, props) now go in with both windings; floors / ceilings stay one-sided.
    // GF_RAG_WALL2=0 = old.
    static int s_wall2 = -1; if (s_wall2 < 0) { const char* e = getenv("GF_RAG_WALL2"); s_wall2 = (e && *e == '0') ? 0 : 1; }
    tl.reserve((size_t)n * 2);
    for (int i = 0; i < n; i++) {
        const float* t = tri + 9 * i;
        tl.push_back(Triangle(Float3(t[0], t[1], t[2]), Float3(t[3], t[4], t[5]), Float3(t[6], t[7], t[8])));
        if (s_wall2) {
            const float ax = t[3] - t[0], ay = t[4] - t[1], az = t[5] - t[2], bx = t[6] - t[0], by = t[7] - t[1], bz = t[8] - t[2];
            const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            const float L = sqrtf(nx * nx + ny * ny + nz * nz);
            if (L > 1e-6f && fabsf(ny) < 0.7f * L)
                tl.push_back(Triangle(Float3(t[0], t[1], t[2]), Float3(t[6], t[7], t[8]), Float3(t[3], t[4], t[5])));
        }
    }
    MeshShapeSettings ms(tl);
    ShapeSettings::ShapeResult res = ms.Create();
    if (res.HasError()) return;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    BodyCreationSettings b(res.Get(), RVec3::sZero(), Quat::sIdentity(), EMotionType::Static, Layers::STATIC);
    b.mFriction = 0.8f; b.mRestitution = 0.0f;
    bi.CreateAndAddBody(b, EActivation::DontActivate);
    if (drop_floor > 0.0f && !R->ground.IsInvalid()) {
        const RVec3 gp = bi.GetPosition(R->ground);
        bi.SetPosition(R->ground, gp - RVec3(0, drop_floor, 0), EActivation::DontActivate);
    }
    R->sys.OptimizeBroadPhase();
}

// v1091F: switch a part pair's self-collision (gf_ragdoll.c turns the shoulder / neck pairs on once the body lies clear)
void jr_coll_pair(void* h, int a, int b, int on)
{
    Rag* R = (Rag*)h; if (!R || !R->gft || a < 0 || b < 0 || a >= ND || b >= ND || a == b) return;
    if (on) R->gft->EnableCollision((CollisionGroup::SubGroupID)a, (CollisionGroup::SubGroupID)b);
    else R->gft->DisableCollision((CollisionGroup::SubGroupID)a, (CollisionGroup::SubGroupID)b);
}

} // extern "C"

// ------------------------------------------------------------------ v1069H live hit reactions
extern "C" {

void* jr_hr_create(const float* p0f, float ground)
{
    // knee bend side from the pose: same rule as gf_ragdoll.c (knee forward of the hip-ankle line = +1)
    Vec3 p[NP]; for (int i = 0; i < NP; i++) p[i] = V3(p0f + 3 * i);
    const Vec3 f = nrm((p[HIR] - p[HIL]).Cross(p[THO] - p[PEL]));
    const float k0 = (p[KNL] - (p[HIL] + p[ANL]) * 0.5f).Dot(f);
    Rag* R = build(p0f, p0f, 1.0f / 60.0f, ground, k0 >= 0.0f ? 1.0f : -1.0f, -1.0f, true);   // (kn_sign ignored since v1069H)
    for (int d = 0; d < ND; d++) { R->tone11[d] = 1.0f; R->out_q[d] = R->R0[d]; R->out_x[d] = p[k_joint[d]]; R->extra[d] = Vec3::sZero(); }
    for (int i = 0; i < NP; i++) R->anim[i] = p[i];
    return R;
}

// the animation's pose this frame (21 particles, model space), muscle tone per part (0..1), sway = axis * angle
// (radians) of the whole body about pivot (between the feet): the pelvis is moved there kinematically
void jr_hr_target(void* h, const float* anim, const float* tone11, const float* sway, const float* pivot)
{
    Rag* R = (Rag*)h; if (!R || !R->hr || !anim) return;
    for (int i = 0; i < NP; i++) R->anim[i] = V3(anim + 3 * i);
    R->have_anim = true;
    if (tone11) for (int d = 0; d < ND; d++) R->tone11[d] = std::max(0.0f, std::min(1.0f, tone11[d]));
    R->sway = sway ? V3(sway) : Vec3::sZero(); R->pivot = pivot ? V3(pivot) : Vec3::sZero();
}

// motors: each joint is driven (implicitly, by the constraint solver) to the animation's rotation of the part relative
// to its parent, led by the spring's own lag (1 / (pi f) at critical damping) using the animation's angular velocity;
// tone sets the spring frequency (1.5 Hz limp .. 8 Hz) and the torque limit
static void hr_motors(Rag* R)
{
    for (int d = 1; d < ND; d++) {
        Constraint* c = R->con[d]; if (!c || R->hinge[d]) continue;
        const float tone = R->tone11[d];
        const float f = (1.5f + 8.5f * std::sqrt(tone)) * R->fmul[d], T = 20.0f + 480.0f * tone;
        SwingTwistConstraint* st = (SwingTwistConstraint*)c;
        st->GetSwingMotorSettings() = MotorSettings(f, 1.0f, FLT_MAX, T); st->GetTwistMotorSettings() = st->GetSwingMotorSettings();
        st->SetTargetOrientationBS(R->R0[k_parent[d]].Conjugated() * R->R0[d]);   // the frozen pose
    }
}

void jr_hr_step(void* h, float dt)
{
    Rag* R = (Rag*)h; if (!R || !R->hr) return;
    if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
    if (dt > 0.05f) dt = 0.05f;
    const int n = std::max(1, std::min(6, (int)std::ceil(dt * 120.0f - 1e-3f)));
    const float hs = dt / (float)n;
    hr_motors(R);
    for (int i = 0; i < n; i++) R->sys.Update(hs, 1, g_temp, g_jobs);
}

// per part: C (3x3 row-major, column-vector convention) = simulated rotation * animation rotation^T, the simulated and
// the animated position of the part's joint, all 21 simulated particles; *maxdeg = largest correction angle
int jr_hr_get(void* h, float* C, float* Xp, float* Xa, float* pout, float* maxdeg)
{
    Rag* R = (Rag*)h; if (!R || !R->hr || !R->have_anim) return 0;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    Quat P[ND], A[ND], O[ND]; Vec3 X[ND]; float mx = 0.0f;
    const Vec3* b = R->have_base ? R->base : R->anim;   // v1069T: stumble -> legs follow the stumble, not the game's pain anim
    Quat B[ND];
    for (int d = 0; d < ND; d++) { P[d] = bi.GetRotation(R->body[d]); A[d] = q_of(part_frame(d, R->anim)); B[d] = R->have_base ? q_of(part_frame(d, b)) : A[d]; }
    // root: the animation's pelvis swayed about the feet
    const float sa = R->sway.Length();
    const Quat S = sa > 1e-6f ? Quat::sRotation(R->sway / sa, sa) : Quat::sIdentity();
    O[0] = (S * B[0]).Normalized(); X[0] = R->pivot + S * (b[k_joint[0]] - R->pivot) + R->root_off;
    auto apply_extra = [&](int d) { const Vec3 e = R->extra[d]; const float a = e.Length();
        if (a > 1e-5f && a < 3.0f) { O[d] = (Quat::sRotation(e / a, a) * O[d]).Normalized(); if (a > mx) mx = a; } };
    apply_extra(0);
    mx = sa;
    for (int d = 1; d < ND; d++) {   // parents come first in the part order
        const int pd = k_parent[d];
        Quat dev = (P[pd].Conjugated() * P[d]) * (R->R0[pd].Conjugated() * R->R0[d]).Conjugated();   // deviation, parent space
        dev = dev.Normalized(); if (dev.GetW() < 0) dev = -dev;
        const float ang = 2.0f * std::acos(std::min(1.0f, dev.GetW())); if (ang > mx) mx = ang;
        O[d] = (O[pd] * dev * (B[pd].Conjugated() * B[d])).Normalized();
        const Quat Cp = O[pd] * B[pd].Conjugated();
        X[d] = X[pd] + Cp * (b[k_joint[d]] - b[k_joint[pd]]);
        apply_extra(d);
    }
    for (int d = 0; d < ND; d++) {
        R->out_q[d] = O[d]; R->out_x[d] = X[d];
        const Mat44 Cm = Mat44::sRotation((O[d] * A[d].Conjugated()).Normalized());
        if (C) for (int r = 0; r < 3; r++) for (int cc = 0; cc < 3; cc++) C[d * 9 + r * 3 + cc] = Cm(r, cc);
        const Vec3 xa = R->anim[k_joint[d]];
        if (Xp) { Xp[d * 3] = X[d].GetX(); Xp[d * 3 + 1] = X[d].GetY(); Xp[d * 3 + 2] = X[d].GetZ(); }
        if (Xa) { Xa[d * 3] = xa.GetX(); Xa[d * 3 + 1] = xa.GetY(); Xa[d * 3 + 2] = xa.GetZ(); }
    }
    if (pout) for (int i = 0; i < NP; i++) { const int d = k_owner[i]; const Vec3 x = X[d] + (O[d] * A[d].Conjugated()) * (R->anim[i] - R->anim[k_joint[d]]);
        pout[i * 3] = x.GetX(); pout[i * 3 + 1] = x.GetY(); pout[i * 3 + 2] = x.GetZ(); }
    if (maxdeg) *maxdeg = mx * 57.29578f;
    return 1;
}

// a bullet: impulse J (N s) at point on a part (hit reaction or death ragdoll); 0 = that part cannot be pushed
int jr_impulse(void* h, int part, const float* point, const float* J)
{
    Rag* R = (Rag*)h; if (!R || part < 0 || part >= ND) return 0;
    BodyLockInterfaceNoLock& bl = (BodyLockInterfaceNoLock&)R->sys.GetBodyLockInterfaceNoLock();
    Body* b = bl.TryGetBody(R->body[part]); if (!b || !b->IsDynamic()) return 0;
    Vec3 j = V3(J); if (!(j.GetX() == j.GetX())) return 0;
    if (j.Length() > 120.0f) j = j.Normalized() * 120.0f;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    Vec3 pt = V3(point);
    if (R->hr) {   // v1069J: the bullet hit the displayed (animated) part -> same spot on the simulated one
        const Quat M = b->GetRotation() * R->out_q[part].Conjugated();
        const Mat44 Mb = Mat44::sRotationTranslation(b->GetRotation(), Vec3(b->GetPosition()));
        const Mat44 M0 = Mat44::sRotationTranslation(R->R0[part], R->c0[part]);
        const Vec3 jp = Mb * (M0.InversedRotationTranslation() * R->p0[k_joint[part]]);   // the part's joint, simulated
        pt = jp + M * (pt - R->out_x[part]); j = M * j;
    }
    bi.AddImpulse(R->body[part], j, RVec3(pt));
    bi.ActivateBody(R->body[part]);
    return 1;
}

} // extern "C"

// v1069S stumble program (gf_ragdoll.c): per part a world-space rotation (axis * angle, radians) about the part's joint,
// applied on top of anim + physics (children follow), and a shift of the whole body; NULL clears
// v1069T: base pose (21 particles, model space) the output is built from instead of the live anim; NULL = live anim
extern "C" void jr_hr_base(void* h, const float* base21)
{
    Rag* R = (Rag*)h; if (!R || !R->hr) return;
    R->have_base = base21 != nullptr;
    if (base21) for (int i = 0; i < NP; i++) R->base[i] = V3(base21 + 3 * i);
}
extern "C" void jr_hr_extra(void* h, const float* ex11, const float* root_off)
{
    Rag* R = (Rag*)h; if (!R || !R->hr) return;
    for (int d = 0; d < ND; d++) R->extra[d] = ex11 ? V3(ex11 + 3 * d) : Vec3::sZero();
    R->root_off = root_off ? V3(root_off) : Vec3::sZero();
}

// v1069C the car that hit this body: rot9 = car-local -> sim frame (3x3 row-major, column-vector), pos = car origin, oy = the
// car's ground clearance base relative to its origin along its up axis (bottom of the body). Boxes: lower body 1.84 x 0.85 x 4.7 m,
// cabin 1.6 x 0.56 x 2.3 m set 0.3 m back. Then jr_car_move every frame (target for the next step); jr_car_remove.
static Quat q_from9(const float* m)
{
    const Mat44 M(Vec4(m[0], m[3], m[6], 0), Vec4(m[1], m[4], m[7], 0), Vec4(m[2], m[5], m[8], 0), Vec4(0, 0, 0, 1));
    return M.GetQuaternion().Normalized();
}
extern "C" void jr_car_add(void* h, const float* rot9, const float* pos, float oy)
{
    Rag* R = (Rag*)h; if (!R || R->have_car || R->hr) return;
    StaticCompoundShapeSettings cs;
    // v1108X (user: driving into a body is "not physical"): the lower box started 0.28 m up -> a body lying on the road (~0.2 m)
    // passed UNDER the car untouched. Now: lower body from 0.20 m (the bumper shoves), cabin as before, and four WHEELS down to the
    // ground (oy) that roll onto / push the limbs.
    // v1108Y: wheel boxes removed again (they shoved every body aside, so the car never went OVER one -- no bumps); the lower body
    // starts at 0.24 m: a lying torso is caught and dragged, limbs pass under; the wheels' effect is gf_ragdoll.c car_react().
    cs.AddShape(Vec3(0, oy + 0.24f + 0.445f, 0), Quat::sIdentity(), new BoxShape(Vec3(0.92f, 0.445f, 2.35f), 0.08f));
    cs.AddShape(Vec3(0, oy + 0.24f + 0.89f + 0.28f, -0.30f), Quat::sIdentity(), new BoxShape(Vec3(0.80f, 0.28f, 1.15f), 0.08f));
    ShapeSettings::ShapeResult r = cs.Create(); if (r.HasError()) return;
    const Quat q = q_from9(rot9);
    BodyCreationSettings b(r.Get(), RVec3(V3(pos)), q, EMotionType::Kinematic, Layers::MOVING);
    b.mFriction = 0.5f; b.mRestitution = 0.05f; b.mAllowSleeping = false;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock();
    R->car = bi.CreateAndAddBody(b, EActivation::Activate);
    R->have_car = !R->car.IsInvalid(); R->car_pos = RVec3(V3(pos)); R->car_rot = q;
    R->sys.OptimizeBroadPhase();
}
extern "C" void jr_car_move(void* h, const float* rot9, const float* pos)
{
    Rag* R = (Rag*)h; if (!R || !R->have_car) return;
    R->car_pos = RVec3(V3(pos)); R->car_rot = q_from9(rot9); R->car_tgt = true;
}
extern "C" void jr_car_remove(void* h)
{
    Rag* R = (Rag*)h; if (!R || !R->have_car) return;
    BodyInterface& bi = R->sys.GetBodyInterfaceNoLock(); bi.RemoveBody(R->car); bi.DestroyBody(R->car); R->have_car = false;
}
