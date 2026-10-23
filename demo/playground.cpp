// impulse: interactive playground guest — the flagship demo.
//
// One command fires up an X11 window (demo/viewer.h, native build only)
// rendering a Jolt pyramid scene in real time. Every interaction (fire
// ball / drop box / shockwave / gravity toggle) is recorded as a
// quantized command stamped with the sim step it was applied at, next to
// a full dump of the initial world. On exit the session — initial world
// dump + command log + final state hash — is written to build/session.bin.
//
// The same source, cross-compiled for OpenVM with the session baked into
// the ELF (make demo-replay), rebuilds the world from the session header,
// verifies its initial world hash, replays every command at the exact
// same step, and reveals the final state hash via the standard impulse
// protocol: the zkVM replay must land on a bit-identical state hash (and
// a byte-identical state blob — the Makefile cmp's them).
//
// Determinism rules for shared sim code (docs/ops.md):
//   - command parameters are integers only (aim slots, mass classes)
//   - direction vectors come from integer compass tables + Vec3
//     Normalized() (sqrt + IEEE division; JPH_CROSS_PLATFORM_DETERMINISTIC
//     keeps FMA out of the library)
//   - no libc trig/transcendentals in sim code (glibc vs musl ULPs)
//   - wall-clock/rendering never touches anything recorded
//
// `make demo-auto` runs the identical native logic headless with a
// scripted command sequence (no window needed), and `make demo-replay`
// proves the round trip: native session hash == zkVM replay hash.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

JPH_SUPPRESS_WARNINGS

// PerformanceTest headers assume `using namespace JPH` is already active.
using namespace JPH;

#include "PerformanceTest/Layers.h"

#include "reveal.h"

#ifdef OPENVM
#include <openvm/openvm_vm.h>
#include <openvm/openvm_sha256.h>
#include "../runtime/openvm_io.h"
#else
#include "viewer.h"
#endif

// ---------------------------------------------------------------------------
// Session file format (little-endian, packed; written by the native run,
// baked into and parsed by the VM replay build)
// ---------------------------------------------------------------------------

#define IMP_SESSION_MAGIC "IMPSESS1"
#define IMP_SESSION_VERSION 5

// flags bits
#define IMP_FLAG_ALLOW_SLEEP 0x1u
#define IMP_FLAG_NO_SPLITTER 0x2u

// command opcodes (parameters stay quantized integers)
#define IMP_OP_FIRE_BALL 1
#define IMP_OP_DROP_BOX 2
#define IMP_OP_SHOCKWAVE 3
#define IMP_OP_GRAVITY 4

#define IMP_CMD_MAX 8192
#define IMP_BODY_MAX 4096

#pragma pack(push, 4)
struct SessionCmd
{
	uint32_t				step;			// applied before this step's Update (1-based)
	uint8_t					op;
	uint8_t					pad0;
	uint16_t				pad1;
	int32_t					a;
	int32_t					b;
};

struct SessionHeader
{
	char					magic[8];
	uint32_t				version;
	uint32_t				height;			// pyramid height
	uint32_t				flags;			// IMP_FLAG_*
	uint32_t				solver_v;		// 0 = Jolt default (8)
	uint32_t				solver_p;		// 0 = Jolt default (2)
	uint32_t				total_steps;
	uint32_t				num_cmds;
	uint32_t				num_bodies0;	// bodies in the initial world dump
	uint32_t				reserved;
	unsigned char			initial_hash[32];
	unsigned char			final_hash[32];
};

// Same layout as the pyramid demo: raw BodyID + position + rotation.
struct BodyEntry
{
	uint32_t				mBodyID;
	float					mPosition[3];
	float					mRotation[4];
};
#pragma pack(pop)

static_assert(sizeof(SessionCmd) == 16, "SessionCmd layout");
static_assert(sizeof(SessionHeader) == 108, "SessionHeader layout");
static_assert(sizeof(BodyEntry) == 32, "BodyEntry layout");

// Quantized command parameter tables. All exact float literals: the same
// values on x86-64 and rv32im_zfinx, applied by identical code.
static const float IMP_DT = 1.0f / 60.0f;
static const int IMP_AIM_SLOTS = 32;
// 32 compass directions (~11.25 deg apart) as small integer vectors;
// normalized at init via sqrt + IEEE division (deterministic).
static const int IMP_AIM_DX[32] = {
	1, 4, 2, 4, 1, 3, 1, 1, 0, -1, -1, -3, -1, -4, -2, -4,
	-1, -4, -2, -4, -1, -3, -1, -1, 0, 1, 1, 3, 1, 4, 2, 4 };
static const int IMP_AIM_DZ[32] = {
	0, 1, 1, 3, 1, 4, 2, 4, 1, 4, 2, 4, 1, 3, 1, 1,
	0, -1, -1, -3, -1, -4, -2, -4, -1, -4, -2, -4, -1, -3, -1, -1 };
static const float IMP_BALL_RADIUS[3] = { 0.6f, 0.95f, 1.3f };
static const float IMP_BALL_SPEED = 45.0f;
static const float IMP_BALL_SPAWN_DIST = 30.0f;
static const float IMP_BALL_SPAWN_Y = 8.0f;
// launch elevation: 69 uniform-angle slots, -85 deg..+85 deg in exact
// 2.5 deg steps, stored as exact tan() float literals (baked at build
// time — no runtime trig, byte-identical literals on both legs).
// One Up/Down press = 2.5 deg. Normalized with the horizontal aim via
// sqrt + IEEE division (deterministic).
static const int IMP_ELEV_SLOTS = 69;
#define IMP_ELEV_DEFAULT 34 /* level */
static const float IMP_ELEV_SLOPE[IMP_ELEV_SLOTS] = {
	-11.430052757263184f, -7.595754146575928f, -5.671281814575195f, -4.510708332061768f, -3.732050895690918f, -3.1715948581695557f,
	-2.7474775314331055f, -2.4142136573791504f, -2.1445069313049316f, -1.9209821224212646f, -1.7320507764816284f, -1.5696855783462524f,
	-1.4281480312347412f, -1.3032253980636597f, -1.191753625869751f, -1.0913084745407104f, -1.0f, -0.9163311719894409f,
	-0.839099645614624f, -0.7673270106315613f, -0.700207531452179f, -0.6370702385902405f, -0.5773502588272095f, -0.5205670595169067f,
	-0.466307669878006f, -0.4142135679721832f, -0.36397022008895874f, -0.31529879570007324f, -0.2679491937160492f, -0.22169466316699982f,
	-0.17632697522640228f, -0.13165250420570374f, -0.08748866617679596f, -0.04366094246506691f, 0.0f, 0.04366094246506691f,
	0.08748866617679596f, 0.13165250420570374f, 0.17632697522640228f, 0.22169466316699982f, 0.2679491937160492f, 0.31529879570007324f,
	0.36397022008895874f, 0.4142135679721832f, 0.466307669878006f, 0.5205670595169067f, 0.5773502588272095f, 0.6370702385902405f,
	0.700207531452179f, 0.7673270106315613f, 0.839099645614624f, 0.9163311719894409f, 1.0f, 1.0913084745407104f,
	1.191753625869751f, 1.3032253980636597f, 1.4281480312347412f, 1.5696855783462524f, 1.7320507764816284f, 1.9209821224212646f,
	2.1445069313049316f, 2.4142136573791504f, 2.7474775314331055f, 3.1715948581695557f, 3.732050895690918f, 4.510708332061768f,
	5.671281814575195f, 7.595754146575928f, 11.430052757263184f
};
static const float IMP_DROP_GRID = 4.0f;
static const float IMP_DROP_CLEAR = 4.0f;	// above the pyramid top
// shockwave: uniform velocity kick — the impulse is scaled by each
// body's mass so an 8 t crate and a 1 t ball get the same (visible) dv
static const float IMP_SHOCK_KICK[3] = { 2.0f, 6.0f, 14.0f };

// live sessions auto-end after 60 s of sim time (60 Hz fixed step);
// --max-steps overrides
#define IMP_LIVE_MAX_STEPS_DEFAULT (60u * 60u)

// ---------------------------------------------------------------------------
// Sim state (single PhysicsSystem, single-threaded — identical config on
// both legs)
// ---------------------------------------------------------------------------

static PhysicsSystem* g_physics;
static BodyInterface* g_body_interface;
static TempAllocatorImpl* g_temp_allocator;
static JobSystemSingleThreaded* g_job_system;
static Vec3 g_aim[IMP_AIM_SLOTS];
static RefConst<Shape> g_box_shape;
static float g_drop_y;	// pyramid-relative drop height (set in sim_create)
static Vec3 g_gravity[4];	// earth / moon / zero / up (set in sim_create)
#ifndef OPENVM
static uint32_t g_scene_body_count;	// bodies created by the scene (viewer coloring)
#endif

// Command log: commands are appended with the step they apply at, and
// consumed in order while stepping. g_cmd_count only ever contains
// applied-or-pending commands; on save, the unconsumed tail is dropped.
static SessionCmd g_cmds[IMP_CMD_MAX];
static uint32_t g_cmd_count;
static uint32_t g_cmd_cursor; // consumed up to (exclusive) during stepping

static void sim_create(const SessionHeader& inHeader)
{
	RegisterDefaultAllocator();

	g_temp_allocator = new TempAllocatorImpl(32 * 1024 * 1024);
	g_job_system = new JobSystemSingleThreaded(cMaxPhysicsJobs);

	const uint cMaxBodies = IMP_BODY_MAX;
	const uint cNumBodyMutexes = 0;
	const uint cMaxBodyPairs = 65536;
	const uint cMaxContactConstraints = 10240;

	Factory::sInstance = new Factory();
	RegisterTypes();

	// These must outlive the PhysicsSystem: Init() stores pointers to
	// them (pyramid.cpp keeps them alive in main; here they live for the
	// whole process).
	static BPLayerInterfaceImpl broad_phase_layer_interface;
	static ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
	static ObjectLayerPairFilterImpl object_vs_object_layer_filter;

	g_physics = new PhysicsSystem();
	g_physics->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
		broad_phase_layer_interface, object_vs_broadphase_layer_filter, object_vs_object_layer_filter);
	g_body_interface = &g_physics->GetBodyInterface();

	// Solver knobs from the session (0 = Jolt defaults 8/2). Native live
	// fills these from argv; the VM replay reads the same values back.
	{
		PhysicsSettings settings;
		if (inHeader.solver_v != 0)
			settings.mNumVelocitySteps = inHeader.solver_v;
		if (inHeader.solver_p != 0)
			settings.mNumPositionSteps = inHeader.solver_p;
		settings.mUseLargeIslandSplitter = (inHeader.flags & IMP_FLAG_NO_SPLITTER) == 0;
		g_physics->SetPhysicsSettings(settings);
	}

	// Integer compass table -> unit vectors via Normalized() (sqrt + IEEE
	// division only — deterministic across legs).
	for (int i = 0; i < IMP_AIM_SLOTS; ++i)
		g_aim[i] = Vec3((float)IMP_AIM_DX[i], 0.0f, (float)IMP_AIM_DZ[i]).Normalized();

	// Scene: floor + pyramid, verbatim from PerformanceTest/PyramidScene.h
	// with the height parameterized (see demo/pyramid.cpp) and sleeping
	// honored at runtime from the session flags.
	{
		BodyInterface& bi = *g_body_interface;

		bi.CreateAndAddBody(BodyCreationSettings(new BoxShape(Vec3(50.0f, 1.0f, 50.0f), 0.0f),
			RVec3(Vec3(0.0f, -1.0f, 0.0f)), Quat::sIdentity(), EMotionType::Static, Layers::NON_MOVING),
			EActivation::DontActivate);

		// Low rim around the floor edge: keeps balls/debris in play (and in
		// the simulation budget — strays otherwise cost for the whole
		// session). Static bodies: no solver cost, deterministic creation
		// order (N, E, S, W before the pyramid).
		{
			const float wall_half_h = 1.5f;
			const float wall_half_t = 0.75f;
			const float rim = 50.0f;
			const float len = rim + wall_half_t; // join the corners
			const RVec3 wall_positions[4] = {
				RVec3(Vec3(0.0f, wall_half_h, -rim)),
				RVec3(Vec3(rim, wall_half_h, 0.0f)),
				RVec3(Vec3(0.0f, wall_half_h, rim)),
				RVec3(Vec3(-rim, wall_half_h, 0.0f)),
			};
			const Vec3 wall_extents[4] = {
				Vec3(len, wall_half_h, wall_half_t),
				Vec3(wall_half_t, wall_half_h, len),
				Vec3(len, wall_half_h, wall_half_t),
				Vec3(wall_half_t, wall_half_h, len),
			};
			for (int w = 0; w < 4; ++w)
				bi.CreateAndAddBody(BodyCreationSettings(new BoxShape(wall_extents[w], 0.0f),
					wall_positions[w], Quat::sIdentity(), EMotionType::Static, Layers::NON_MOVING),
				EActivation::DontActivate);
		}

		const float cBoxSize = 2.0f;
		const float cBoxSeparation = 0.5f;
		const float cHalfBoxSize = 0.5f * cBoxSize;
		const int height = (int)inHeader.height;

		g_box_shape = new BoxShape(Vec3::sReplicate(cHalfBoxSize), 0.0f); // no convex radius, matches pyramid.cpp

		const bool allow_sleep = (inHeader.flags & IMP_FLAG_ALLOW_SLEEP) != 0;
		for (int i = 0; i < height; ++i)
			for (int j = i / 2; j < height - (i + 1) / 2; ++j)
				for (int k = i / 2; k < height - (i + 1) / 2; ++k)
				{
					RVec3 position(Real(-height + cBoxSize * j + (i & 1 ? cHalfBoxSize : 0.0f)),
						Real(1.0f + (cBoxSize + cBoxSeparation) * i),
						Real(-height + cBoxSize * k + (i & 1 ? cHalfBoxSize : 0.0f)));
					BodyCreationSettings settings(g_box_shape, position, Quat::sIdentity(),
						EMotionType::Dynamic, Layers::MOVING);
					settings.mAllowSleeping = allow_sleep;
					bi.CreateAndAddBody(settings, EActivation::Activate);
				}
	}

	g_physics->OptimizeBroadPhase();

	// boxes drop just above the pyramid top, not from the stratosphere
	// (deterministic: derived from the session header's height)
	g_drop_y = IMP_DROP_CLEAR + 2.5f * (float)inHeader.height;
	g_gravity[0] = Vec3(0.0f, -9.81f, 0.0f);
	g_gravity[1] = Vec3(0.0f, -1.62f, 0.0f);
	g_gravity[2] = Vec3::sZero();
	g_gravity[3] = Vec3(0.0f, 9.81f, 0.0f);
#ifndef OPENVM
	g_scene_body_count = g_physics->GetNumBodies();
#endif
}

static void sim_destroy()
{
	if (!g_physics)
		return; // verifier may exit before the sim is created
	UnregisterTypes();
	delete Factory::sInstance;
	Factory::sInstance = nullptr;
	delete g_physics;
	delete g_job_system;
	delete g_temp_allocator;
}

static void apply_command(const SessionCmd& inCmd)
{
	switch (inCmd.op)
	{
	case IMP_OP_FIRE_BALL:
		{
			const uint32_t aim = ((uint32_t)inCmd.a) & 31u;
			// b = mass_class | (elevation << 2):  mass 0..2, elevation 0..68
			const uint32_t mass_class = ((uint32_t)inCmd.b) & 3u;
			uint32_t elev = ((uint32_t)inCmd.b) >> 2;
			if (elev >= (uint32_t)IMP_ELEV_SLOTS)
				elev = IMP_ELEV_DEFAULT; // out-of-range: fire flat
			const Vec3 aim_dir = g_aim[aim];
			// launch velocity: horizontal aim + elevation slope, normalized
			const Vec3 vel_dir = Vec3(aim_dir.GetX(), IMP_ELEV_SLOPE[elev],
				aim_dir.GetZ()).Normalized();
			const Vec3 pos(-aim_dir.GetX() * IMP_BALL_SPAWN_DIST,
				IMP_BALL_SPAWN_Y, -aim_dir.GetZ() * IMP_BALL_SPAWN_DIST);
			BodyCreationSettings settings(new SphereShape(IMP_BALL_RADIUS[mass_class]), pos,
				Quat::sIdentity(), EMotionType::Dynamic, Layers::MOVING);
			settings.mLinearVelocity = vel_dir * IMP_BALL_SPEED;
			const BodyID id = g_body_interface->CreateAndAddBody(settings, EActivation::Activate);
			if (id.IsInvalid())
				printf("impulse: warning: body limit reached, fire skipped at step %u\n",
					(unsigned)inCmd.step);
			break;
		}

	case IMP_OP_DROP_BOX:
		{
			const Vec3 pos((float)inCmd.a * IMP_DROP_GRID, g_drop_y, (float)inCmd.b * IMP_DROP_GRID);
			BodyCreationSettings settings(g_box_shape, pos, Quat::sIdentity(),
				EMotionType::Dynamic, Layers::MOVING);
			const BodyID id = g_body_interface->CreateAndAddBody(settings, EActivation::Activate);
			if (id.IsInvalid())
				printf("impulse: warning: body limit reached, drop skipped at step %u\n",
					(unsigned)inCmd.step);
			break;
		}

	case IMP_OP_SHOCKWAVE:
		{
			const uint32_t k = ((uint32_t)inCmd.a) % 3u;
			BodyIDVector ids;
			g_physics->GetBodies(ids);
			for (size_t i = 0; i < ids.size(); ++i)
			{
				BodyLockRead lock(g_physics->GetBodyLockInterface(), ids[i]);
				if (!lock.Succeeded())
					continue;
				const Body& body = lock.GetBody();
				if (!body.IsDynamic())
					continue;
				const Vec3 d = Vec3(body.GetPosition()) - Vec3(0.0f, 2.0f, 0.0f);
				if (d.LengthSq() < 1.0e-4f)
					continue;
				// kick scales with mass: same dv for every body (AddImpulse
				// wakes sleeping bodies)
				const float mass = 1.0f / body.GetMotionProperties()->GetInverseMass();
				g_body_interface->AddImpulse(ids[i], d.Normalized() * (mass * IMP_SHOCK_KICK[k]));
			}
			break;
		}

	case IMP_OP_GRAVITY:
		{
			// a = 0..3: earth / moon / zero / up. Sleeping bodies ignore
			// gravity changes, so wake everything to make the toggle bite.
			const uint32_t mode = ((uint32_t)inCmd.a) & 3u;
			g_physics->SetGravity(g_gravity[mode]);
			BodyIDVector ids;
			g_physics->GetBodies(ids);
			for (size_t i = 0; i < ids.size(); ++i)
				g_body_interface->ActivateBody(ids[i]);
			break;
		}

	default:
		printf("impulse: warning: unknown opcode %u at step %u\n",
			(unsigned)inCmd.op, (unsigned)inCmd.step);
		break;
	}
}

// One sim tick: apply this step's queued commands, then Update.
static void sim_step(uint32_t inStep)
{
	while (g_cmd_cursor < g_cmd_count && g_cmds[g_cmd_cursor].step == inStep)
	{
		apply_command(g_cmds[g_cmd_cursor]);
		++g_cmd_cursor;
	}
	g_physics->Update(IMP_DT, 1, g_temp_allocator, g_job_system);
}

// Record a command to be applied at inStep (interactive/script input;
// native build only — the VM leg reads commands from the session).
#ifndef OPENVM
static void record_command(uint32_t inStep, uint8_t inOp, int32_t inA, int32_t inB)
{
	if (g_cmd_count >= IMP_CMD_MAX)
	{
		printf("impulse: warning: command log full, command dropped\n");
		return;
	}
	SessionCmd& c = g_cmds[g_cmd_count++];
	c.step = inStep;
	c.op = inOp;
	c.pad0 = 0;
	c.pad1 = 0;
	c.a = inA;
	c.b = inB;
}
#endif // !OPENVM

// Serialize every body (body-index order = creation order, deterministic).
static uint32_t serialize_bodies(BodyEntry* outEntries, uint32_t inCap)
{
	BodyIDVector ids;
	g_physics->GetBodies(ids);
	uint32_t n = 0;
	for (size_t i = 0; i < ids.size() && n < inCap; ++i)
	{
		BodyLockRead lock(g_physics->GetBodyLockInterface(), ids[i]);
		if (!lock.Succeeded())
			continue;
		const Body& body = lock.GetBody();
		outEntries[n].mBodyID = ids[i].GetIndexAndSequenceNumber();
		const RVec3 pos = body.GetPosition();
		const Quat rot = body.GetRotation();
		outEntries[n].mPosition[0] = pos.GetX();
		outEntries[n].mPosition[1] = pos.GetY();
		outEntries[n].mPosition[2] = pos.GetZ();
		outEntries[n].mRotation[0] = rot.GetX();
		outEntries[n].mRotation[1] = rot.GetY();
		outEntries[n].mRotation[2] = rot.GetZ();
		outEntries[n].mRotation[3] = rot.GetW();
		++n;
	}
	return n;
}

static void session_sha256(const void* inData, uint32_t inSize, unsigned char outHash[32])
{
#ifdef OPENVM
	openvm_sha256((const uint8_t*)inData, inSize, outHash);
#else
	impulse_sha256((const unsigned char*)inData, inSize, outHash);
#endif
}

// Final blob: what both legs hash (and what the VM reveals + the runner
// writes to state_openvm.bin; the native side writes the same bytes to
// build/session_final.bin so the Makefile can cmp them byte-for-byte).
//
// layout: { u32 magic 'IMPG'; u32 num_bodies; u32 total_steps;
//           u32 num_cmds; u32 init_hash_ok; u8 cmd_hash[32]; u8 pad[4];
//           BodyEntry entries[] }
#define IMP_FINAL_MAGIC 0x47504d49u // "IMPG"
#define IMP_FINAL_HDR_SIZE 56

static unsigned char* build_final_blob(const SessionHeader& inHeader, uint32_t inInitOk,
	const unsigned char inCmdHash[32], const BodyEntry* inEntries, uint32_t inNumBodies,
	uint32_t* outSize)
{
	const uint32_t size = IMP_FINAL_HDR_SIZE + sizeof(BodyEntry) * inNumBodies;
	unsigned char* blob = (unsigned char*)malloc(size);
	memset(blob, 0, size);
	uint32_t* w = (uint32_t*)blob;
	w[0] = IMP_FINAL_MAGIC;
	w[1] = inNumBodies;
	w[2] = inHeader.total_steps;
	w[3] = inHeader.num_cmds;
	w[4] = inInitOk;
	memcpy(blob + 20, inCmdHash, 32);
	memcpy(blob + IMP_FINAL_HDR_SIZE, inEntries, sizeof(BodyEntry) * inNumBodies);
	*outSize = size;
	return blob;
}

// ---------------------------------------------------------------------------
// Native: interactive window or headless scripted auto-run
// ---------------------------------------------------------------------------
#ifndef OPENVM

// Fill viewer body list (rendering only — never touches sim state).
static uint32_t fill_viewer_bodies(PGViewBody* outBodies, uint32_t inCap, uint32_t inHeight)
{
	BodyIDVector ids;
	g_physics->GetBodies(ids);
	uint32_t n = 0;
	for (size_t i = 0; i < ids.size() && n < inCap; ++i)
	{
		BodyLockRead lock(g_physics->GetBodyLockInterface(), ids[i]);
		if (!lock.Succeeded())
			continue;
		const Body& body = lock.GetBody();
		PGViewBody& vb = outBodies[n];
		const RVec3 pos = body.GetPosition();
		const Quat rot = body.GetRotation();
		vb.px = pos.GetX(); vb.py = pos.GetY(); vb.pz = pos.GetZ();
		vb.rx = rot.GetX(); vb.ry = rot.GetY(); vb.rz = rot.GetZ(); vb.rw = rot.GetW();
		vb.kind = 0; vb.ex = 1.0f; vb.ey = 1.0f; vb.ez = 1.0f; vb.r = 1.0f;

		const Shape* shape = body.GetShape();
		if (shape->GetSubType() == EShapeSubType::Sphere)
		{
			vb.kind = 1;
			vb.r = static_cast<const SphereShape*>(shape)->GetRadius();
			// color by mass class (display only)
			if (vb.r < 0.8f) { vb.cr = 0.25f; vb.cg = 0.85f; vb.cb = 0.95f; }
			else if (vb.r < 1.1f) { vb.cr = 0.95f; vb.cg = 0.6f; vb.cb = 0.15f; }
			else { vb.cr = 0.95f; vb.cg = 0.2f; vb.cb = 0.2f; }
		}
		else
		{
			const Vec3 he = static_cast<const BoxShape*>(shape)->GetHalfExtent();
			vb.ex = he.GetX(); vb.ey = he.GetY(); vb.ez = he.GetZ();
			if (body.GetMotionType() == EMotionType::Static)
			{ vb.cr = 0.30f; vb.cg = 0.32f; vb.cb = 0.35f; }
			else if (i >= g_scene_body_count)
			{ vb.cr = 0.25f; vb.cg = 0.85f; vb.cb = 0.35f; } // dropped mid-session (creation order)
			else
			{ vb.cr = 0.85f; vb.cg = 0.75f; vb.cb = 0.55f; } // pyramid
		}
		++n;
	}
	(void)inHeight;
	return n;
}

static int parse_op_char(char c)
{
	switch (c)
	{
	case 'F': case 'f': return IMP_OP_FIRE_BALL;
	case 'B': case 'b': return IMP_OP_DROP_BOX;
	case 'X': case 'x': return IMP_OP_SHOCKWAVE;
	case 'G': case 'g': return IMP_OP_GRAVITY;
	}
	return 0;
}

// Parse "--script F:30:0:1,B:90:2:2" (op:step:a:b, op:step:a, or op:step).
static void parse_script(const char* inScript)
{
	char buf[4096];
	snprintf(buf, sizeof(buf), "%s", inScript);
	char* saveptr = nullptr;
	for (char* tok = strtok_r(buf, ",", &saveptr); tok; tok = strtok_r(nullptr, ",", &saveptr))
	{
		char op = 0;
		int step = 0, a = 0, b = 0;
		const int nfields = sscanf(tok, " %c:%d:%d:%d", &op, &step, &a, &b);
		if (nfields < 2)
		{
			printf("impulse: bad script token '%s' skipped\n", tok);
			continue;
		}
		if (nfields < 3) a = 0;
		if (nfields < 4) b = 0;
		const int opc = parse_op_char(op);
		if (!opc || step < 1)
		{
			printf("impulse: bad script token '%s' skipped\n", tok);
			continue;
		}
		record_command((uint32_t)step, (uint8_t)opc, a, b);
	}
	// stable sort by step (insertion sort; equal steps keep input order)
	for (uint32_t i = 1; i < g_cmd_count; ++i)
	{
		const SessionCmd key = g_cmds[i];
		uint32_t j = i;
		while (j > 0 && g_cmds[j - 1].step > key.step)
		{
			g_cmds[j] = g_cmds[j - 1];
			--j;
		}
		g_cmds[j] = key;
	}
}

// Fill sidecar paths from the session path (both native build only).
// Sidecar names strip a trailing ".bin": session.bin -> session.cmds.txt,
// session.world0.txt, session_final.bin.
static void write_sidecars(const char* inOutPath, const SessionHeader& inHeader, const BodyEntry* inWorld0)
{
	char stem[768];
	const size_t n = strlen(inOutPath);
	snprintf(stem, sizeof(stem), "%.*s", (int)(n > 4 && !strcmp(inOutPath + n - 4, ".bin") ? n - 4 : n), inOutPath);
	char path[896];

	// <stem>.cmds.txt — human-readable command log (the recorded keys)
	snprintf(path, sizeof(path), "%s.cmds.txt", stem);
	FILE* f = fopen(path, "w");
	if (f)
	{
		fprintf(f, "# impulse session commands: op step a b  (F: a=aim 0..31, b=mass+4*elev (elev 0..68, 2.5 deg steps); B: a,b=lattice; X: a=strength; G: a=1 moon)\n");
		for (uint32_t i = 0; i < inHeader.num_cmds; ++i)
			fprintf(f, "%c %u %d %d\n", "?FBXG"[g_cmds[i].op], (unsigned)g_cmds[i].step,
				g_cmds[i].a, g_cmds[i].b);
		fclose(f);
	}

	// <stem>.world0.txt — initial world dump (auditable)
	snprintf(path, sizeof(path), "%s.world0.txt", stem);
	f = fopen(path, "w");
	if (f)
	{
		fprintf(f, "# impulse initial world dump: id px py pz rx ry rz rw\n");
		for (uint32_t i = 0; i < inHeader.num_bodies0; ++i)
		{
			const BodyEntry& e = inWorld0[i];
			// %a is host libc (native build only — no floats on the VM leg)
			fprintf(f, "%u %a %a %a %a %a %a %a\n", e.mBodyID,
				e.mPosition[0], e.mPosition[1], e.mPosition[2],
				e.mRotation[0], e.mRotation[1], e.mRotation[2], e.mRotation[3]);
		}
		fclose(f);
	}
}

// Replay a recorded session: commands auto-fire at their exact steps.
// Windowed (demo-replay) shows it live; headless (--replay X --auto) is
// the same replay without a window (regression check). Both recompute
// both hashes and must match the recording.
static bool replay_finalize(const SessionHeader& header, uint32_t init_ok, uint32_t steps_done)
{
	BodyEntry* final_entries = (BodyEntry*)malloc(sizeof(BodyEntry) * IMP_BODY_MAX);
	const uint32_t n_final = serialize_bodies(final_entries, IMP_BODY_MAX);
	unsigned char cmd_hash[32];
	session_sha256(g_cmds, sizeof(SessionCmd) * header.num_cmds, cmd_hash);
	uint32_t blob_size = 0;
	unsigned char* blob = build_final_blob(header, init_ok, cmd_hash, final_entries, n_final, &blob_size);
	unsigned char final_hash[32];
	impulse_reveal(blob, blob_size, final_hash);
	const bool final_ok = memcmp(final_hash, header.final_hash, 32) == 0;
	char hex[65], recorded_hex[65];
	impulse_hash_hex(final_hash, hex);
	impulse_hash_hex(header.final_hash, recorded_hex);
	printf("impulse: ================================================================\n");
	printf("impulse: replay done: %u/%u steps, %u commands, %u bodies\n",
		(unsigned)steps_done, (unsigned)header.total_steps, (unsigned)header.num_cmds, (unsigned)n_final);
	printf("impulse: replayed final hash: %s\n", hex);
	printf("impulse: recorded final hash: %s\n", recorded_hex);
	printf("impulse: %s\n", final_ok
		? "MATCH — replay reproduces the recorded session"
		: "*** MISMATCH ***");
	printf("impulse: ================================================================\n");
	free(blob);
	free(final_entries);
	return final_ok;
}

static int run_replay(const char* inPath, bool inWindowed)
{
	FILE* f = fopen(inPath, "rb");
	if (!f)
	{
		printf("impulse: cannot open %s\n", inPath);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	const long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* data = (unsigned char*)malloc((size_t)len);
	if (!data || fread(data, 1, (size_t)len, f) != (size_t)len)
	{
		printf("impulse: cannot read %s\n", inPath);
		return 1;
	}
	fclose(f);

	if ((uint32_t)len < sizeof(SessionHeader))
	{
		printf("impulse: session file too small\n");
		return 1;
	}
	SessionHeader header;
	memcpy(&header, data, sizeof(header));
	if (memcmp(header.magic, IMP_SESSION_MAGIC, 8) != 0
		|| header.version != IMP_SESSION_VERSION
		|| header.num_cmds > IMP_CMD_MAX || header.num_bodies0 > IMP_BODY_MAX
		|| (uint32_t)len != sizeof(SessionHeader) + sizeof(SessionCmd) * header.num_cmds
			+ sizeof(BodyEntry) * header.num_bodies0)
	{
		printf("impulse: session file malformed\n");
		return 1;
	}

	printf("impulse: replaying session: height=%u steps=%u cmds=%u flags=%#x solver=%u/%u\n",
		(unsigned)header.height, (unsigned)header.total_steps, (unsigned)header.num_cmds,
		(unsigned)header.flags, (unsigned)header.solver_v, (unsigned)header.solver_p);

	sim_create(header);

	// verify the initial world hash against the recording
	BodyEntry* world0 = (BodyEntry*)malloc(sizeof(BodyEntry) * IMP_BODY_MAX);
	const uint32_t n0 = serialize_bodies(world0, IMP_BODY_MAX);
	unsigned char initial_hash[32];
	session_sha256(world0, sizeof(BodyEntry) * n0, initial_hash);
	const uint32_t init_ok = n0 == header.num_bodies0 && memcmp(initial_hash, header.initial_hash, 32) == 0;
	char hex[65], recorded_hex[65];
	impulse_hash_hex(initial_hash, hex);
	impulse_hash_hex(header.initial_hash, recorded_hex);
	printf("impulse: initial world: %u/%u bodies, hash = %s (recorded %s) %s\n",
		(unsigned)n0, (unsigned)header.num_bodies0, hex, recorded_hex,
		init_ok ? "MATCH" : "*** MISMATCH ***");
	free(world0);

	memcpy(g_cmds, data + sizeof(SessionHeader), sizeof(SessionCmd) * header.num_cmds);
	g_cmd_count = header.num_cmds;
	g_cmd_cursor = 0;

	if (!inWindowed)
	{
		for (uint32_t s = 1; s <= header.total_steps; ++s)
		{
			sim_step(s);
			if (s % 100 == 0)
				printf("  step %u/%u (cmds applied %u)\n", (unsigned)s,
					(unsigned)header.total_steps, (unsigned)g_cmd_cursor);
		}
		const bool ok = replay_finalize(header, init_ok, header.total_steps);
		free(data);
		sim_destroy();
		return ok ? 0 : 1;
	}

	PGViewer* viewer = PGViewer::create(1024, 640, header.height);
	if (!viewer)
	{
		printf("impulse: could not open window (no display?)\n");
		return 1;
	}
	viewer->replay_mode = true;

	PGViewBody bodies[IMP_BODY_MAX];
	PGViewCmd discarded[16];
	uint32_t steps_done = 0;
	bool finalized = false;
	int final_ok = 0;
	double acc = 0.0, last = viewer->now();
	for (;;)
	{
		if (!viewer->pump())
			break;
		viewer->poll_cmds(discarded, 16); // replay ignores live commands

		const double now = viewer->now();
		if (!viewer->paused())
		{
			acc += now - last;
			const double dt = (double)IMP_DT;
			while (acc >= dt && steps_done < header.total_steps)
			{
				sim_step(steps_done + 1);
				++steps_done;
				acc -= dt;
			}
		}
		else
		{
			acc = 0.0;
		}
		if (acc > 1.0)
			acc = 1.0; // long stalls must not fast-forward the replay
		last = now;

		if (!finalized && steps_done >= header.total_steps)
		{
			final_ok = replay_finalize(header, init_ok, steps_done) ? 1 : 0;
			snprintf(viewer->hud_note, sizeof(viewer->hud_note), "REPLAY COMPLETE — hash %s (q to quit)",
				final_ok ? "MATCH" : "MISMATCH");
			finalized = true;
		}
		else if (!finalized)
		{
			snprintf(viewer->hud_note, sizeof(viewer->hud_note), "replaying %u/%u steps, %u/%u cmds applied",
				(unsigned)steps_done, (unsigned)header.total_steps,
				(unsigned)g_cmd_cursor, (unsigned)header.num_cmds);
		}

		const uint32_t nb = fill_viewer_bodies(bodies, IMP_BODY_MAX, header.height);
		viewer->render(bodies, (int)nb, steps_done, header.total_steps, g_cmd_cursor,
			g_physics->GetNumBodies());
	}
	delete viewer;
	free(data);
	sim_destroy();
	return final_ok ? 0 : 1;
}

// Finalize the live session: trim unapplied commands, serialize + hash,
// write session.bin / sidecars / final blob, print the banner. Called
// either when the user quits or when the step cap (60 s) is reached —
// in the latter case the window stays open (HUD note) until q.
static void finalize_live_session(SessionHeader& ioHeader, const BodyEntry* inWorld0,
	const char* inOutPath, uint32_t inStepsDone, PGViewer* inViewer)
{
	g_cmd_count = g_cmd_cursor;
	ioHeader.total_steps = inStepsDone;
	ioHeader.num_cmds = g_cmd_count;

	BodyEntry* final_entries = (BodyEntry*)malloc(sizeof(BodyEntry) * IMP_BODY_MAX);
	const uint32_t n_final = serialize_bodies(final_entries, IMP_BODY_MAX);

	unsigned char cmd_hash[32];
	session_sha256(g_cmds, sizeof(SessionCmd) * g_cmd_count, cmd_hash);

	uint32_t blob_size = 0;
	unsigned char* blob = build_final_blob(ioHeader, 1, cmd_hash, final_entries, n_final, &blob_size);

	unsigned char final_hash[32];
	impulse_reveal(blob, blob_size, final_hash); // native: portable sha256
	memcpy(ioHeader.final_hash, final_hash, 32);
	char hex[65];
	impulse_hash_hex(final_hash, hex);

	FILE* f = fopen(inOutPath, "wb");
	if (!f)
	{
		printf("impulse: cannot write %s\n", inOutPath);
		free(blob);
		free(final_entries);
		return;
	}
	fwrite(&ioHeader, sizeof(ioHeader), 1, f);
	fwrite(g_cmds, sizeof(SessionCmd), g_cmd_count, f);
	fwrite(inWorld0, sizeof(BodyEntry), ioHeader.num_bodies0, f);
	fclose(f);

	char stem[768];
	const size_t nlen = strlen(inOutPath);
	snprintf(stem, sizeof(stem), "%.*s",
		(int)(nlen > 4 && !strcmp(inOutPath + nlen - 4, ".bin") ? nlen - 4 : nlen), inOutPath);
	char final_path[896];
	snprintf(final_path, sizeof(final_path), "%s_final.bin", stem);
	f = fopen(final_path, "wb");
	if (f)
	{
		fwrite(blob, 1, blob_size, f);
		fclose(f);
	}

	write_sidecars(stem, ioHeader, inWorld0);

	printf("impulse: ================================================================\n");
	printf("impulse: session: %u steps (%.1f s), %u commands, %u bodies\n",
		(unsigned)inStepsDone, (double)inStepsDone / 60.0, (unsigned)g_cmd_count, (unsigned)n_final);
	impulse_hash_hex(ioHeader.initial_hash, hex);
	printf("impulse: initial world hash: %s\n", hex);
	impulse_hash_hex(ioHeader.final_hash, hex);
	printf("impulse: FINAL STATE HASH:   %s\n", hex);
	printf("impulse: ================================================================\n");
	printf("impulse: session written: %s (+ .cmds.txt / .world0.txt / _final.bin sidecars)\n", inOutPath);
	printf("impulse: now prove it:  make demo-verify   (visual: make demo-replay)\n");

	if (inViewer)
		snprintf(inViewer->hud_note, sizeof(inViewer->hud_note),
			"SESSION COMPLETE — %.0fs, %u cmds, hash %c%c%c%c... (closing)",
			(double)inStepsDone / 60.0, (unsigned)g_cmd_count,
			hex[0], hex[1], hex[2], hex[3]); // hex still holds the final hash

	free(blob);
	free(final_entries);
}

int main(int argc, char** argv)
{
	uint32_t height = 6;
	uint32_t auto_steps = 0;
	const char* script = "";
	const char* out_path = "build/session.bin";
	const char* replay_path = nullptr;
	bool replay_headless = false;
	uint32_t max_steps = IMP_LIVE_MAX_STEPS_DEFAULT;
	uint32_t flags = IMP_FLAG_ALLOW_SLEEP;
	uint32_t solver_v = 0, solver_p = 0;

	for (int i = 1; i < argc; ++i)
	{
		if (!strcmp(argv[i], "--height") && i + 1 < argc) height = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--auto") && i + 1 < argc) auto_steps = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
		else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_path = argv[++i];
		else if (!strcmp(argv[i], "--headless")) replay_headless = true;
		else if (!strcmp(argv[i], "--max-steps") && i + 1 < argc) max_steps = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--nosleep")) flags &= ~IMP_FLAG_ALLOW_SLEEP;
		else if (!strcmp(argv[i], "--nosplitter")) flags |= IMP_FLAG_NO_SPLITTER;
		else if (!strcmp(argv[i], "--vsteps") && i + 1 < argc) solver_v = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--psteps") && i + 1 < argc) solver_p = (uint32_t)atoi(argv[++i]);
		else { printf("usage: %s [--height N] [--auto N] [--script 'F:30:4:1,...']\n"
			              "  script: F:step:aim:mass+4*elev  B:step:x:z  X:step:strength  G:step:mode\n"
			              "          [--out FILE] [--replay FILE [--headless]] [--nosleep] [--nosplitter]\n"
			              "          [--vsteps N] [--psteps N]\n",
			argv[0]); return 1; }
	}
	if (replay_path)
		return run_replay(replay_path, !replay_headless);
	if (height > 16)
	{
		printf("impulse: height capped at 16\n");
		height = 16;
	}


	SessionHeader header;
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, IMP_SESSION_MAGIC, 8);
	header.version = IMP_SESSION_VERSION;
	header.height = height;
	header.flags = flags;
	header.solver_v = solver_v;
	header.solver_p = solver_p;

	printf("impulse: playground, height=%u %ssleeping splitter=%s\n",
		(unsigned)height, (flags & IMP_FLAG_ALLOW_SLEEP) ? "" : "no-",
		(flags & IMP_FLAG_NO_SPLITTER) ? "off" : "on");

	sim_create(header);
	printf("impulse: bodies=%u active=%u\n",
		(unsigned)g_physics->GetNumBodies(),
		(unsigned)g_physics->GetNumActiveBodies(EBodyType::RigidBody));

	// initial world dump + hash
	BodyEntry* world0 = (BodyEntry*)malloc(sizeof(BodyEntry) * IMP_BODY_MAX);
	header.num_bodies0 = serialize_bodies(world0, IMP_BODY_MAX);
	session_sha256(world0, sizeof(BodyEntry) * header.num_bodies0, header.initial_hash);

	char hex[65];
	impulse_hash_hex(header.initial_hash, hex);
	printf("impulse: initial world: %u bodies, hash = %s\n",
		(unsigned)header.num_bodies0, hex);

	// parse the script BEFORE any auto/viewer stepping so its commands
	// carry their own step stamps
	if (script[0])
		parse_script(script);

	uint32_t steps_done = 0;
	if (auto_steps > 0)
	{
		for (uint32_t s = 1; s <= auto_steps; ++s)
		{
			sim_step(s);
			++steps_done;
			if (s % 100 == 0)
				printf("  step %u/%u (cmds applied %u)\n", (unsigned)s, (unsigned)auto_steps,
					(unsigned)g_cmd_cursor);
		}
	}
	else
	{
		PGViewer* viewer = PGViewer::create(1024, 640, height);
		if (!viewer)
		{
			printf("impulse: could not open window (no display?) — use --auto N instead\n");
			return 1;
		}
		PGViewBody bodies[IMP_BODY_MAX];
		double acc = 0.0, last = viewer->now();
		bool finalized = false;
		double finalized_at = 0.0;
		printf("impulse: playing: %u s cap (%u steps), q to end early; F11 fullscreen\n",
			(unsigned)(max_steps / 60), (unsigned)max_steps);
		for (;;)
		{
			if (!viewer->pump())
				break;
			PGViewCmd vc[16];
			const int n = viewer->poll_cmds(vc, 16);
			if (!finalized)
				for (int i = 0; i < n; ++i)
					record_command(steps_done + 1, vc[i].op, vc[i].a, vc[i].b);

			const double now = viewer->now();
			if (!viewer->paused())
			{
				acc += now - last;
				const double dt = (double)IMP_DT;
				while (acc >= dt && steps_done < max_steps)
				{
					sim_step(steps_done + 1);
					++steps_done;
					acc -= dt;
				}
			}
			else
			{
				acc = 0.0;
			}
			if (acc > 1.0)
				acc = 1.0;
			last = now;

			if (!finalized && steps_done >= max_steps)
			{
				// 60 s (or --max-steps) reached: record the session now and
				// auto-close the window after a short beat to see the result
				finalized = true;
				finalized_at = viewer->now();
				finalize_live_session(header, world0, out_path, steps_done, viewer);
			}
			if (finalized && viewer->now() - finalized_at >= 2.0)
				break; // auto-terminate

			const uint32_t nb = fill_viewer_bodies(bodies, IMP_BODY_MAX, height);
			viewer->render(bodies, (int)nb, steps_done, max_steps, g_cmd_cursor,
				g_physics->GetNumBodies());
		}
		delete viewer;
		if (!finalized)
			finalize_live_session(header, world0, out_path, steps_done, nullptr);
		free(world0);
		sim_destroy();
		return 0;
	}

	// auto mode (or early live exit) finalization
	finalize_live_session(header, world0, out_path, steps_done, nullptr);

	free(world0);
	sim_destroy();
	return 0;
}

#endif // !OPENVM

// ---------------------------------------------------------------------------
// OpenVM: generic session verifier.
//
// One binary, no session baked in. The session (initial game state +
// recorded player inputs + claimed final state hash) arrives on the
// OpenVM input stream; the guest re-simulates it and reveals a single
// output value: 0 = validation success (simulated final hash matches
// the claim), non-zero = the failure code below.
// ---------------------------------------------------------------------------
#ifdef OPENVM

// verdict codes (the program's only public output)
#define IMP_VLD_OK 0u
#define IMP_VLD_NO_INPUT 1u
#define IMP_VLD_BAD_FORMAT 2u
#define IMP_VLD_BAD_INITIAL 3u
#define IMP_VLD_BAD_FINAL 4u

static const char* verdict_name(uint32_t v)
{
	switch (v)
	{
	case IMP_VLD_OK: return "VALID";
	case IMP_VLD_NO_INPUT: return "NO INPUT";
	case IMP_VLD_BAD_FORMAT: return "BAD FORMAT";
	case IMP_VLD_BAD_INITIAL: return "BAD INITIAL STATE";
	case IMP_VLD_BAD_FINAL: return "BAD FINAL HASH";
	}
	return "?";
}

int main(int argc, char** argv)
{
	(void)argc; (void)argv;

	printf("impulse: verifier: generic replay guest, session via input stream\n");

	uint32_t status = IMP_VLD_OK;
	uint32_t len = 0;
	unsigned char* blob = openvm_read_input(&len);
	const SessionHeader* header = NULL;
	const unsigned char* world0_in = NULL;
	BodyEntry* scratch = NULL;

	do
	{
		if (!blob)
		{
			printf("impulse: no/invalid input stream (run the runner with --input <session.bin>)\n");
			status = IMP_VLD_NO_INPUT;
			break;
		}
		if (len < sizeof(SessionHeader))
		{
			printf("impulse: input smaller than a session header (%u bytes)\n", (unsigned)len);
			status = IMP_VLD_BAD_FORMAT;
			break;
		}
		header = (const SessionHeader*)blob;
		if (memcmp(header->magic, IMP_SESSION_MAGIC, 8) != 0
			|| header->version != IMP_SESSION_VERSION
			|| header->num_cmds > IMP_CMD_MAX || header->num_bodies0 > IMP_BODY_MAX
			|| header->height > 16
			|| len != sizeof(SessionHeader) + sizeof(SessionCmd) * header->num_cmds
				+ sizeof(BodyEntry) * header->num_bodies0)
		{
			printf("impulse: input is not a well-formed session\n");
			status = IMP_VLD_BAD_FORMAT;
			break;
		}

		printf("impulse: session: height=%u steps=%u cmds=%u flags=%#x solver=%u/%u\n",
			(unsigned)header->height, (unsigned)header->total_steps, (unsigned)header->num_cmds,
			(unsigned)header->flags, (unsigned)header->solver_v, (unsigned)header->solver_p);

		world0_in = blob + sizeof(SessionHeader) + sizeof(SessionCmd) * header->num_cmds;

		// 1. rebuild the scene and check the INITIAL state three ways:
		//    simulated == provided dump, dump hashes to the recorded
		//    initial hash
		sim_create(*header);
		scratch = (BodyEntry*)malloc(sizeof(BodyEntry) * IMP_BODY_MAX);
		const uint32_t n0 = serialize_bodies(scratch, IMP_BODY_MAX);
		unsigned char hash[32];
		session_sha256(world0_in, sizeof(BodyEntry) * header->num_bodies0, hash);

		char hex[65], hex2[65];
		impulse_hash_hex(hash, hex);
		impulse_hash_hex(header->initial_hash, hex2);
		printf("impulse: initial world: %u/%u bodies, dump hash %s (recorded %s)\n",
			(unsigned)n0, (unsigned)header->num_bodies0, hex, hex2);

		if (n0 != header->num_bodies0
			|| memcmp(hash, header->initial_hash, 32) != 0
			|| memcmp(scratch, world0_in, sizeof(BodyEntry) * n0) != 0)
		{
			printf("impulse: initial state does not match the recording\n");
			status = IMP_VLD_BAD_INITIAL;
			break;
		}

		// 2. replay the recorded inputs at their exact steps
		memcpy(g_cmds, blob + sizeof(SessionHeader), sizeof(SessionCmd) * header->num_cmds);
		g_cmd_count = header->num_cmds;
		g_cmd_cursor = 0;
		for (uint32_t st = 1; st <= header->total_steps; ++st)
		{
			sim_step(st);
			if (st % 100 == 0)
				printf("  step %u/%u (cmds applied %u)\n", (unsigned)st,
					(unsigned)header->total_steps, (unsigned)g_cmd_cursor);
		}
		if (g_cmd_cursor != header->num_cmds)
			printf("impulse: warning: %u commands were never applied\n",
				(unsigned)(header->num_cmds - g_cmd_cursor));

		// 3. hash the simulated final state exactly like the recorder
		const uint32_t n_final = serialize_bodies(scratch, IMP_BODY_MAX);
		unsigned char cmd_hash[32];
		session_sha256(g_cmds, sizeof(SessionCmd) * header->num_cmds, cmd_hash);
		uint32_t blob_size = 0;
		unsigned char* final_blob = build_final_blob(*header, 1, cmd_hash, scratch, n_final, &blob_size);
		session_sha256(final_blob, blob_size, hash);
		free(final_blob);

		impulse_hash_hex(hash, hex);
		impulse_hash_hex(header->final_hash, hex2);
		printf("impulse: final state hash: %s (claimed %s) %s\n", hex, hex2,
			memcmp(hash, header->final_hash, 32) == 0 ? "MATCH" : "*** MISMATCH ***");

		if (memcmp(hash, header->final_hash, 32) != 0)
		{
			status = IMP_VLD_BAD_FINAL;
			break;
		}
	} while (0);

	// the single output value: 0 = validation success
	openvm_reveal_u32(0, status);
	printf("impulse: verdict = %u (%s)\n", (unsigned)status, verdict_name(status));

	free(scratch);
	free(blob);
	sim_destroy();
	return 0;
}

#endif // OPENVM
