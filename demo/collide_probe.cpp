// impulse: narrowphase unit probe — CollideShape(sphere, box) at a sweep of
// vertical offsets, natively and on the VM. Isolates the GJK/narrowphase
// math from broadphase/job machinery: any line that differs between the
// native and VM runs is the divergence.

#include <stdio.h>
#include <string.h>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

JPH_SUPPRESS_WARNINGS

using namespace JPH;

static unsigned bitsf(float f)
{
	unsigned u;
	memcpy(&u, &f, 4);
	return u;
}

class ProbeCollector : public CollideShapeCollector
{
public:
	virtual void	AddHit(const CollideShapeResult &inResult) override
	{
		mHadHit = true;
		mDepth = inResult.mPenetrationDepth;
		mAxisY = inResult.mPenetrationAxis.GetY();
		mPoint2Y = inResult.mContactPointOn2.GetY();
	}

	bool			mHadHit = false;
	float			mDepth = 0.0f;
	float			mAxisY = 0.0f;
	float			mPoint2Y = 0.0f;
};

int main()
{
	RegisterDefaultAllocator();
	Factory::sInstance = new Factory();
	RegisterTypes();

	BoxShape box(Vec3(100.0f, 1.0f, 100.0f), 0.05f);
	SphereShape sphere(0.5f);

	CollideShapeSettings settings;
	settings.mActiveEdgeMode = EActiveEdgeMode::CollideOnlyWithActive;
	settings.mBackFaceMode = EBackFaceMode::CollideWithBackFaces;

	SubShapeIDCreator s1, s2;

	printf("== sweep: sphere center y, sphere-vs-box collide ==\n");
	for (int i = 0; i <= 32; i++)
	{
		float y = 3.0f - 0.25f * (float)i; // 3.0 down to -1.0
		Vec3 pos(0.2f, y, -0.3f);

		ProbeCollector collector;
		CollisionDispatch::sCollideShapeVsShape(
			&sphere, &box,
			Vec3::sOne(), Vec3::sOne(),
			Mat44::sTranslation(pos), Mat44::sTranslation(Vec3::sZero()),
			s1, s2, settings, collector);

		printf("y=%08x(%d) hit=%d depth*1e6=%d axisYbits=%08x point2Y*1e6=%d\n",
			bitsf(y), (int)(y * 100.0f),
			(int)collector.mHadHit,
			(int)(collector.mDepth * 1000000.0f),
			bitsf(collector.mAxisY),
			(int)(collector.mPoint2Y * 1000000.0f));
	}

	UnregisterTypes();
	delete Factory::sInstance;
	Factory::sInstance = nullptr;
	printf("collide probe done\n");
	return 0;
}
