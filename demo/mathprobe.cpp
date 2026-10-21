// impulse: FP bit-exactness probe (native x86 vs OpenVM rv32im_zfinx).
//
// Prints the exact bit patterns of a battery of f32/f64 operations,
// including the patterns GJK-style collision code exercises (dot, cross,
// normalize, sqrt, div, min/max, denormals). Run natively and on the VM and
// diff the outputs: any differing line is a transpiled-instruction
// semantics bug. Floats are printed as hex bits (printf %f is unavailable
// in guests — musl formats via binary128 panic stubs).

#include <stdio.h>
#include <string.h>
#include <math.h>

static unsigned bitsf(float f)
{
	unsigned u;
	memcpy(&u, &f, 4);
	return u;
}

static unsigned long long bitsd(double d)
{
	unsigned long long u;
	memcpy(&u, &d, 8);
	return u;
}

static float g_a = 1.0f;
static float g_b = 3.0f;

int main()
{
	printf("== f32 basics ==\n");
	float vals[] = { 0.1f, 0.3f, 1.0f/3.0f, 2.0f/7.0f, 1e-30f, 1e30f, 16777217.0f };
	for (unsigned i = 0; i < sizeof(vals)/sizeof(vals[0]); i++)
		for (unsigned j = 0; j < sizeof(vals)/sizeof(vals[0]); j++)
		{
			float a = vals[i], b = vals[j];
			printf("a=%08x b=%08x add=%08x sub=%08x mul=%08x div=%08x\n",
				bitsf(a), bitsf(b), bitsf(a+b), bitsf(a-b), bitsf(a*b), bitsf(a/b));
		}

	printf("== f32 sqrt ==\n");
	for (unsigned i = 1; i < 40; i++)
	{
		float x = (float)i * 0.371f + 0.013f;
		printf("x=%08x sqrt=%08x\n", bitsf(x), bitsf(sqrt(x)));
	}

	printf("== f32 dot/cross/normalize (GJT-style) ==\n");
	float v1[3] = { 0.317f, -1.414f, 2.718f };
	float v2[3] = { -0.577f, 1.618f, -0.091f };
	float dot = v1[0]*v2[0] + v1[1]*v2[1] + v1[2]*v2[2];
	printf("dot=%08x\n", bitsf(dot));
	float cross[3] = {
		v1[1]*v2[2] - v1[2]*v2[1],
		v1[2]*v2[0] - v1[0]*v2[2],
		v1[0]*v2[1] - v1[1]*v2[0]
	};
	printf("cross=%08x %08x %08x\n", bitsf(cross[0]), bitsf(cross[1]), bitsf(cross[2]));
	float len = sqrt(dot*dot + cross[0]*cross[0] + 1.0f);
	printf("len=%08x inv=%08x\n", bitsf(len), bitsf(1.0f/len));

	printf("== f32 min/max/cmp ==\n");
	float s = -0.0f, t = 0.0f;
	printf("min=%08x max=%08x lt=%d le=%d eq=%d\n",
		bitsf(s < t ? s : t), bitsf(s > t ? s : t),
		(int)(s < t), (int)(s <= t), (int)(s == t));

	printf("== f32 denormals ==\n");
	float dn = 1e-38f, dn2 = dn * 0.0039f; // subnormal result
	printf("dn=%08x dn2=%08x back=%08x\n", bitsf(dn), bitsf(dn2), bitsf(dn2 * 256.0f));

	printf("== f64 (soft) ==\n");
	double dvals[] = { 0.1, 1.0/3.0, 1e-300, 1e300 };
	for (unsigned i = 0; i < 4; i++)
		for (unsigned j = 0; j < 4; j++)
		{
			double a = dvals[i], b = dvals[j];
			printf("a=%016llx b=%016llx add=%016llx mul=%016llx div=%016llx sqrt=%016llx\n",
				bitsd(a), bitsd(b), bitsd(a+b), bitsd(a*b), bitsd(a/b), bitsd(sqrt(a)));
		}

	printf("== f32 <-> f64 converts ==\n");
	for (unsigned i = 0; i < sizeof(vals)/sizeof(vals[0]); i++)
	{
		float f = vals[i];
		double d = (double)f;
		float back = (float)d;
		printf("f=%08x d=%016llx back=%08x\n", bitsf(f), bitsd(d), bitsf(back));
	}
	(void)g_a; (void)g_b;
	printf("probe done\n");
	return 0;
}
