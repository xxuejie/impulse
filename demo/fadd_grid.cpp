// impulse: fadd grid probe — systematic search for the f32 add input that
// violates the zfinx FaddCoreAir constraints during proving.
//
// The h=2 pyramid guest fails proving with "constraints had nonzero value
// on air VmAirWrapper<Rv32FpAdapterAir, FaddCoreAir>"; simple edge cases
// (cancellation, signed zeros, subnormals, overflow) all prove fine. This
// guest performs FADD_GRID_COUNT additions starting at pair index
// FADD_GRID_START, drawn from a corner-case grid of 32 bit patterns
// (exponents 0/1/60/120/126/127/128/200/254 x mantissa edges x signs).
// Results are XOR-folded into the revealed hash so nothing folds away.

#include <stdio.h>
#include <string.h>

#ifdef OPENVM
#include "reveal.h"
#else
#include "sha256.h"
#endif

#ifndef FADD_GRID_START
#define FADD_GRID_START 0
#endif
#ifndef FADD_GRID_COUNT
#define FADD_GRID_COUNT 64
#endif

static unsigned bitsf(float f) { unsigned u; memcpy(&u, &f, 4); return u; }

static unsigned grid_pattern(unsigned i)
{
	static const unsigned exps[] = { 0, 1, 60, 120, 126, 127, 128, 200, 254 };
	unsigned e = exps[(i / 6) % 9];
	unsigned m;
	switch (i % 6)
	{
	case 0: m = 0; break;
	case 1: m = 1; break;
	case 2: m = 0x400000; break;
	case 3: m = 0x7fffff; break;
	case 4: m = 0x3fffff; break;
	default: m = 0x000001; break;
	}
	unsigned s = (i / 54) & 1; // spread signs across the pattern index space
	return (s << 31) | (e << 23) | m;
}

int main()
{
	unsigned acc = 0;
	unsigned first_a = 0, first_b = 0, first_r = 0;
	unsigned blob[5];

	for (unsigned p = 0; p < FADD_GRID_COUNT; ++p)
	{
		unsigned idx = FADD_GRID_START + p;
		unsigned ua = grid_pattern(idx % 32);
		unsigned ub = grid_pattern((idx / 32) % 32);
		float a, b;
		memcpy(&a, &ua, 4);
		memcpy(&b, &ub, 4);
		volatile float va = a, vb = b;
		float c = va + vb;
		unsigned uc = bitsf(c);
		acc ^= uc ^ (ua * 3) ^ (ub * 5);
		if (p == 0) { first_a = ua; first_b = ub; first_r = uc; }
	}

	blob[0] = acc; blob[1] = first_a; blob[2] = first_b; blob[3] = first_r; blob[4] = 0x600D5EEDu;

	unsigned char hash[32];
	impulse_reveal(blob, sizeof(blob), hash);
	char hex[65];
	impulse_hash_hex(hash, hex);
	printf("fadd grid [%u,%u): acc=%08x first=(%08x+%08x->%08x) hash=%s\n",
		(unsigned)FADD_GRID_START, (unsigned)(FADD_GRID_START + FADD_GRID_COUNT),
		acc, first_a, first_b, first_r, hex);
	return 0;
}
