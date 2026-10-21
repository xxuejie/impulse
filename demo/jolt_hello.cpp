// impulse M1 guest: JoltPhysics HelloWorld on OpenVM.
//
// Adapted from extern/JoltPhysics/HelloWorld/HelloWorld.cpp (CC0-1.0):
//   - printf instead of std::cout/iostream (musl; iostream is cycle-heavy
//     on the VM)
//   - JobSystemSingleThreaded instead of JobSystemThreadPool (no OS, no
//     threads on the VM)
//   - the sim loop runs a fixed number of steps and terminates when the
//     sphere goes to sleep, like upstream
//   - final sphere state (step count + position + velocity) is hashed and
//     revealed via the impulse output protocol (demo/reveal.h), so the VM
//     run can be compared bit-exactly against the native reference build
//
// Builds both cross (OPENVM, rv32im_zfinx) and native (x86-64 reference)
// from the same source.

#include <stdio.h>
#include <cstdarg>

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
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include "reveal.h"

JPH_SUPPRESS_WARNINGS

using namespace JPH;
using namespace JPH::literals;

static void TraceImpl(const char *inFMT, ...)
{
	va_list list;
	va_start(list, inFMT);
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), inFMT, list);
	va_end(list);
	printf("%s\n", buffer);
}

namespace Layers
{
	static constexpr ObjectLayer NON_MOVING = 0;
	static constexpr ObjectLayer MOVING = 1;
	static constexpr ObjectLayer NUM_LAYERS = 2;
};

class ObjectLayerPairFilterImpl : public ObjectLayerPairFilter
{
public:
	virtual bool					ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override
	{
		switch (inObject1)
		{
		case Layers::NON_MOVING:
			return inObject2 == Layers::MOVING;
		case Layers::MOVING:
			return true;
		default:
			JPH_ASSERT(false);
			return false;
		}
	}
};

namespace BroadPhaseLayers
{
	static constexpr BroadPhaseLayer NON_MOVING(0);
	static constexpr BroadPhaseLayer MOVING(1);
	static constexpr uint NUM_LAYERS(2);
};

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface
{
public:
									BPLayerInterfaceImpl()
	{
		mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
		mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
	}

	virtual uint					GetNumBroadPhaseLayers() const override
	{
		return BroadPhaseLayers::NUM_LAYERS;
	}

	virtual BroadPhaseLayer			GetBroadPhaseLayer(ObjectLayer inLayer) const override
	{
		JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
		return mObjectToBroadPhase[inLayer];
	}

private:
	BroadPhaseLayer					mObjectToBroadPhase[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl : public ObjectVsBroadPhaseLayerFilter
{
public:
	virtual bool				ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override
	{
		switch (inLayer1)
		{
		case Layers::NON_MOVING:
			return inLayer2 == BroadPhaseLayers::MOVING;
		case Layers::MOVING:
			return true;
		default:
			JPH_ASSERT(false);
			return false;
		}
	}
};

// An example contact listener (debug: verify contacts happen on the VM)
class MyContactListener : public ContactListener
{
public:
	virtual ValidateResult	OnContactValidate(const Body &inBody1, const Body &inBody2, RVec3Arg inBaseOffset, const CollideShapeResult &inCollisionResult) override
	{
		printf("Contact validate callback\n");
		return ValidateResult::AcceptAllContactsForThisBodyPair;
	}

	virtual void			OnContactAdded(const Body &inBody1, const Body &inBody2, const ContactManifold &inManifold, ContactSettings &ioSettings) override
	{
		printf("A contact was added (depth*1000 = %d, normal_y*1000 = %d, points = %u)\n",
			(int)(inManifold.mPenetrationDepth * 1000.0f),
			(int)(inManifold.mWorldSpaceNormal.GetY() * 1000.0f),
			(unsigned)inManifold.mRelativeContactPointsOn1.size());
	}

	virtual void			OnContactPersisted(const Body &inBody1, const Body &inBody2, const ContactManifold &inManifold, ContactSettings &ioSettings) override
	{
		printf("A contact was persisted\n");
	}

	virtual void			OnContactRemoved(const SubShapeIDPair &inSubShapePair) override
	{
		printf("A contact was removed\n");
	}
};

// Final-state blob hashed by the output protocol. Fixed layout, memset to
// zero first so any padding/uninitialized bytes cannot leak nondeterminism.
struct StateBlob
{
	uint32_t						mStep;
	uint32_t						mPadding;
	float							mPosition[3];
	float							mVelocity[3];
};

int main(int argc, char** argv)
{
	(void)argc; (void)argv;

	RegisterDefaultAllocator();

	Trace = TraceImpl;
	JPH_IF_ENABLE_ASSERTS(AssertFailed = [](const char* inExpression, const char* inMessage, const char* inFile, uint inLine)
		{
			printf("ASSERT FAILED: %s:%u: (%s) %s\n", inFile, (unsigned)inLine, inExpression, inMessage ? inMessage : "");
			return false; // don't breakpoint: keep running to gather more
		};)

	Factory::sInstance = new Factory();
	RegisterTypes();

	TempAllocatorImpl temp_allocator(10 * 1024 * 1024);

#if defined(JOLT_HELLO_THREADS) && JOLT_HELLO_THREADS > 0
	// Experiment: multithreaded job system (native builds only — the VM is
	// single-threaded). Tests whether Jolt's threading model converges to
	// the same state regardless of worker count.
	JobSystemThreadPool job_system(cMaxPhysicsJobs, cMaxPhysicsBarriers, JOLT_HELLO_THREADS);
#else
	JobSystemSingleThreaded job_system(cMaxPhysicsJobs);
#endif

	// Debug: contact listener to see whether the VM ever finds contacts.
	MyContactListener contact_listener;

	// This is the max amount of rigid bodies that you can add to the physics system. If you try to add more you'll get an error.
	const uint cMaxBodies = 1024;
	const uint cNumBodyMutexes = 0;
	const uint cMaxBodyPairs = 1024;
	const uint cMaxContactConstraints = 1024;

	BPLayerInterfaceImpl broad_phase_layer_interface;
	ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
	ObjectLayerPairFilterImpl object_vs_object_layer_filter;

	PhysicsSystem physics_system;
	physics_system.Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, broad_phase_layer_interface, object_vs_broadphase_layer_filter, object_vs_object_layer_filter);
	physics_system.SetContactListener(&contact_listener);

	BodyInterface &body_interface = physics_system.GetBodyInterface();

	BoxShapeSettings floor_shape_settings(Vec3(100.0f, 1.0f, 100.0f));
	floor_shape_settings.SetEmbedded();

	ShapeSettings::ShapeResult floor_shape_result = floor_shape_settings.Create();
	ShapeRefC floor_shape = floor_shape_result.Get();

	BodyCreationSettings floor_settings(floor_shape, RVec3(0.0_r, -1.0_r, 0.0_r), Quat::sIdentity(), EMotionType::Static, Layers::NON_MOVING);

	Body *floor = body_interface.CreateBody(floor_settings);

	body_interface.AddBody(floor->GetID(), EActivation::DontActivate);

	SphereShape sphere_shape(0.5f);
	sphere_shape.SetEmbedded(); // stack object: refcount must not free it
	BodyCreationSettings sphere_settings(&sphere_shape, RVec3(0.0_r, 2.0_r, 0.0_r), Quat::sIdentity(), EMotionType::Dynamic, Layers::MOVING);
	BodyID sphere_id = body_interface.CreateAndAddBody(sphere_settings, EActivation::Activate);

	body_interface.SetLinearVelocity(sphere_id, Vec3(0.0f, -5.0f, 0.0f));

	const float cDeltaTime = 1.0f / 60.0f;

	physics_system.OptimizeBroadPhase();

	printf("impulse: simulating Jolt HelloWorld\n");

	StateBlob blob;
	memset(&blob, 0, sizeof(blob));

	uint step = 0;
	// Safety cap: a runaway sim must terminate on the VM (would otherwise
	// loop forever when deactivation never triggers).
	const uint cMaxSteps = 100;
	while (body_interface.IsActive(sphere_id) && step < cMaxSteps)
	{
		++step;

		RVec3 position = body_interface.GetCenterOfMassPosition(sphere_id);
		Vec3 velocity = body_interface.GetLinearVelocity(sphere_id);

		blob.mStep = step;
		blob.mPosition[0] = position.GetX();
		blob.mPosition[1] = position.GetY();
		blob.mPosition[2] = position.GetZ();
		blob.mVelocity[0] = velocity.GetX();
		blob.mVelocity[1] = velocity.GetY();
		blob.mVelocity[2] = velocity.GetZ();

		// NOTE: musl's printf formats every float via long double, which is
		// binary128 on riscv — only panic stubs exist for those on this
		// platform (see deps/builtins/tf_stubs.c). Guests must never printf
		// %f/%e/%g: print scaled integers instead. Exact bits travel in the
		// state hash.
		printf("Step %u: Position = (%d, %d, %d) mm, Velocity = (%d, %d, %d) mm/s\n",
			step,
			(int)(blob.mPosition[0] * 1000.0f),
			(int)(blob.mPosition[1] * 1000.0f),
			(int)(blob.mPosition[2] * 1000.0f),
			(int)(blob.mVelocity[0] * 1000.0f),
			(int)(blob.mVelocity[1] * 1000.0f),
			(int)(blob.mVelocity[2] * 1000.0f));

		const int cCollisionSteps = 1;

		physics_system.Update(cDeltaTime, cCollisionSteps, &temp_allocator, &job_system);
	}

	body_interface.RemoveBody(sphere_id);
	body_interface.DestroyBody(sphere_id);
	body_interface.RemoveBody(floor->GetID());
	body_interface.DestroyBody(floor->GetID());

	UnregisterTypes();
	delete Factory::sInstance;
	Factory::sInstance = nullptr;

	// Cross-check the custom instruction against the reference SHA-256 on
	// VM builds, then reveal (ptr, size, hash). Native builds just compute
	// and print the hash for comparison against the VM run.
	unsigned char hash[32];
	impulse_reveal(&blob, sizeof(blob), hash);

	char hex[65];
	impulse_hash_hex(hash, hex);
	printf("impulse: steps=%u state hash = %s\n", blob.mStep, hex);

#ifdef OPENVM
	unsigned char ref[32];
	impulse_sha256((const unsigned char*)&blob, sizeof(blob), ref);
	if (memcmp(ref, hash, 32) != 0)
		printf("impulse: ERROR custom sha256 != reference sha256\n");
#endif

	printf("impulse: done\n");
	return 0;
}
