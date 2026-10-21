// impulse: Pyramid scene guest (PerformanceTest) — many-body determinism
// test and M2 demo.
//
// Scene setup copied verbatim from
// extern/JoltPhysics/PerformanceTest/PyramidScene.h (MIT) with one change:
// the pyramid height is a compile-time knob (PYRAMID_HEIGHT, stock 15) so
// configs can be sized for proving. Height h builds h(h+1)(2h+1)/6 boxes
// (15 -> 1240) in one large solver island (mAllowSleeping = false) on a
// static floor. Layers/filters come from PerformanceTest/Layers.h
// unchanged. Fixed step count (PYRAMID_STEPS), then serializes every body
// (ID + position + rotation, body-index order) and reveals the state hash
// via demo/reveal.h — comparable bit-exactly across:
//   - native x86-64 single-threaded (BLUE reference, matches the VM build)
//   - native x86-64 JobSystemThreadPool(N)  (JOLT_HELLO_THREADS=N)
//   - OpenVM rv32im_zfinx single-threaded
//
// Prints (no floats — see docs/ops.md gotcha 11) every PRINT_EVERY steps.

#include <stdio.h>
#include <string.h>

#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#if defined(JOLT_HELLO_THREADS) && JOLT_HELLO_THREADS > 0
#include <Jolt/Core/JobSystemThreadPool.h>
#endif
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

JPH_SUPPRESS_WARNINGS

// PerformanceTest headers assume `using namespace JPH` is already active.
using namespace JPH;

#include "PerformanceTest/Layers.h"

#include "reveal.h"

#ifndef PYRAMID_HEIGHT
#define PYRAMID_HEIGHT 15
#endif

#ifndef PYRAMID_STEPS
#define PYRAMID_STEPS 200
#endif

#ifndef PRINT_EVERY
#define PRINT_EVERY 50
#endif

// Verbatim from PerformanceTest/PyramidScene.h::StartTest, with the height
// parameterized.
static void CreatePyramidScene(PhysicsSystem &inPhysicsSystem, int inHeight)
{
	BodyInterface &bi = inPhysicsSystem.GetBodyInterface();

	// Floor
	bi.CreateAndAddBody(BodyCreationSettings(new BoxShape(Vec3(50.0f, 1.0f, 50.0f), 0.0f), RVec3(Vec3(0.0f, -1.0f, 0.0f)), Quat::sIdentity(), EMotionType::Static, Layers::NON_MOVING), EActivation::DontActivate);

	const float cBoxSize = 2.0f;
	const float cBoxSeparation = 0.5f;
	const float cHalfBoxSize = 0.5f * cBoxSize;

	RefConst<Shape> box_shape = new BoxShape(Vec3::sReplicate(cHalfBoxSize), 0.0f); // No convex radius to force more collisions

	// Pyramid
	for (int i = 0; i < inHeight; ++i)
		for (int j = i / 2; j < inHeight - (i + 1) / 2; ++j)
			for (int k = i / 2; k < inHeight - (i + 1) / 2; ++k)
			{
				RVec3 position(Real(-inHeight + cBoxSize * j + (i & 1? cHalfBoxSize : 0.0f)), Real(1.0f + (cBoxSize + cBoxSeparation) * i), Real(-inHeight + cBoxSize * k + (i & 1? cHalfBoxSize : 0.0f)));
			BodyCreationSettings settings(box_shape, position, Quat::sIdentity(), EMotionType::Dynamic, Layers::MOVING);
#ifndef PYRAMID_ALLOW_SLEEP
			// Stock PerformanceTest behavior: keep the large island awake.
			// PYRAMID_ALLOW_SLEEP enables Jolt's sleeping (deterministic;
			// settled bodies stop simulating — standard game behavior and
			// a large cycle win on long runs).
			settings.mAllowSleeping = false;
#endif
			bi.CreateAndAddBody(settings, EActivation::Activate);
			}
}

// Per body: raw BodyID + position (3f) + rotation quat (4f). All entries
// 4-byte aligned; the blob is memset first so padding cannot leak state.
struct BodyEntry
{
	uint32_t					mBodyID;
	float						mPosition[3];
	float						mRotation[4];
};

int main(int argc, char** argv)
{
	(void)argc; (void)argv;

	RegisterDefaultAllocator();

	Factory::sInstance = new Factory();
	RegisterTypes();

	TempAllocatorImpl temp_allocator(32 * 1024 * 1024);

#if defined(JOLT_HELLO_THREADS) && JOLT_HELLO_THREADS > 0
	JobSystemThreadPool job_system(cMaxPhysicsJobs, cMaxPhysicsBarriers, JOLT_HELLO_THREADS);
	printf("impulse: pyramid, %d worker threads\n", JOLT_HELLO_THREADS);
#else
	JobSystemSingleThreaded job_system(cMaxPhysicsJobs);
	printf("impulse: pyramid, single threaded\n");
#endif

	const uint cMaxBodies = 4096;
	const uint cNumBodyMutexes = 0;
	const uint cMaxBodyPairs = 65536;
	const uint cMaxContactConstraints = 10240;

	BPLayerInterfaceImpl broad_phase_layer_interface;
	ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
	ObjectLayerPairFilterImpl object_vs_object_layer_filter;

	PhysicsSystem physics_system;
	physics_system.Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, broad_phase_layer_interface, object_vs_broadphase_layer_filter, object_vs_object_layer_filter);

	// Solver iteration knobs (Jolt defaults: 8 velocity / 2 position).
	// Fewer iterations converge less but cost proportionally less — the
	// velocity solve is ~42% of guest cycles. Deterministic either way;
	// hashes differ from the default config. Native and VM builds must use
	// the same values (both get SOLVER_* via the Makefile).
#ifdef SOLVER_VSTEPS
	{
		PhysicsSettings settings;
		settings.mNumVelocitySteps = SOLVER_VSTEPS;
	#ifdef SOLVER_PSTEPS
		settings.mNumPositionSteps = SOLVER_PSTEPS;
	#endif
		physics_system.SetPhysicsSettings(settings);
		printf("impulse: solver iterations v=%u p=%u\n",
			(unsigned)settings.mNumVelocitySteps, (unsigned)settings.mNumPositionSteps);
	}
#endif

	// PYRAMID_NO_SPLITTER: disable the large-island splitter. It exists to
	// partition big islands for PARALLEL solving — on the single-threaded
	// VM it is pure bookkeeping. Same solver iteration counts, same
	// fidelity; the constraint grouping/order differs so hashes change.
#ifdef PYRAMID_NO_SPLITTER
	{
		PhysicsSettings settings = physics_system.GetPhysicsSettings();
		settings.mUseLargeIslandSplitter = false;
		physics_system.SetPhysicsSettings(settings);
		printf("impulse: large island splitter disabled\n");
	}
#endif

	CreatePyramidScene(physics_system, PYRAMID_HEIGHT);

	physics_system.OptimizeBroadPhase();

	printf("impulse: bodies=%u active=%u, stepping %d x 1/60s\n",
		(uint)physics_system.GetNumBodies(), (uint)physics_system.GetNumActiveBodies(EBodyType::RigidBody), PYRAMID_STEPS);

	const float cDeltaTime = 1.0f / 60.0f;
	const int cCollisionSteps = 1;

	for (int step = 1; step <= PYRAMID_STEPS; ++step)
	{
		physics_system.Update(cDeltaTime, cCollisionSteps, &temp_allocator, &job_system);

		if ((step % PRINT_EVERY) == 0)
			printf("  step %d/%d done\n", step, PYRAMID_STEPS);
	}

	// Serialize every body in body-index order (GetBodies returns index
	// order = creation order, deterministic).
	BodyIDVector body_ids;
	physics_system.GetBodies(body_ids);

	unsigned char* blob = (unsigned char*)malloc(sizeof(BodyEntry) * body_ids.size() + 8);
	BodyEntry* entries = (BodyEntry*)(blob + 8);
	memset(blob, 0, sizeof(BodyEntry) * body_ids.size() + 8);
	((uint32_t*)blob)[0] = (uint32_t)body_ids.size();
	((uint32_t*)blob)[1] = (uint32_t)PYRAMID_STEPS;

	for (size_t i = 0; i < body_ids.size(); i++)
	{
		BodyLockRead lock(physics_system.GetBodyLockInterface(), body_ids[i]);
		if (!lock.Succeeded())
			continue;
		const Body& body = lock.GetBody();
		entries[i].mBodyID = body_ids[i].GetIndexAndSequenceNumber();
		RVec3 pos = body.GetPosition();
		Quat rot = body.GetRotation();
		entries[i].mPosition[0] = pos.GetX();
		entries[i].mPosition[1] = pos.GetY();
		entries[i].mPosition[2] = pos.GetZ();
		entries[i].mRotation[0] = rot.GetX();
		entries[i].mRotation[1] = rot.GetY();
		entries[i].mRotation[2] = rot.GetZ();
		entries[i].mRotation[3] = rot.GetW();
	}

	unsigned char hash[32];
	impulse_reveal(blob, (uint32_t)(sizeof(BodyEntry) * body_ids.size() + 8), hash);

	char hex[65];
	impulse_hash_hex(hash, hex);
	printf("impulse: pyramid done, bodies=%u steps=%d hash = %s\n",
		(unsigned)body_ids.size(), PYRAMID_STEPS, hex);

	free(blob);
	UnregisterTypes();
	delete Factory::sInstance;
	Factory::sInstance = nullptr;
	return 0;
}
