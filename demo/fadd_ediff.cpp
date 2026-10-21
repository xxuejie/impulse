// impulse: fadd ediff probe — focused grid over exponent-difference buckets
// (the alignment right-shift + sticky network boundaries at 23-27, plus
// cancellation paths) that the corner-case grid missed.

#include <stdio.h>
#include <string.h>

#ifdef OPENVM
#include "reveal.h"
#else
#include "sha256.h"
#endif

#ifndef EDIFF_START
#define EDIFF_START 0
#endif
#ifndef EDIFF_COUNT
#define EDIFF_COUNT 256
#endif

static unsigned bitsf(float f) { unsigned u; memcpy(&u, &f, 4); return u; }

// case index -> (a, b) bit patterns. Layout:
//   i % 4    -> sign pair
//   (i/4) % 4 -> mantissa pair selector
//   (i/16) % 3 -> a_exp in {126,127,128}
//   (i/48)    -> d in EDIFF_D (b_exp = a_exp - d, clamped at 0)
static const int EDIFF_D[] = { 0, 1, 2, 3, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 31, 47, 48, 49, 60, 100, 126, 127, 128, 150, 200, 254 };
static const unsigned MANTS[] = { 0, 1, 0x7fffff, 0x2aaaaa };

static void gen_case(unsigned i, unsigned* oa, unsigned* ob)
{
	unsigned selpair = i % 4;
	unsigned mantpair = (i / 4) % 4;
	unsigned ai = (i / 16) % 3;
	unsigned di = (i / 48) % 26;

	int a_exp = 126 + (int)ai;
	int b_exp = a_exp - EDIFF_D[di];
	if (b_exp < 0) b_exp = 0;

	unsigned sa = selpair & 1, sb = (selpair >> 1) & 1;
	unsigned ma = MANTS[mantpair & 3], mb = MANTS[(mantpair + 1) & 3];

	*oa = (sa << 31) | ((unsigned)a_exp << 23) | ma;
	*ob = (sb << 31) | ((unsigned)b_exp << 23) | mb;
}

int main()
{
	unsigned acc = 0;
	unsigned fa = 0, fb = 0, fr = 0;
	for (unsigned p = 0; p < EDIFF_COUNT; ++p)
	{
		unsigned ua, ub;
		gen_case(EDIFF_START + p, &ua, &ub);
		float a, b;
		memcpy(&a, &ua, 4);
		memcpy(&b, &ub, 4);
		volatile float va = a, vb = b;
		float c = va + vb;
		unsigned uc = bitsf(c);
		acc ^= uc ^ (ua * 3) ^ (ub * 5);
		if (p == 0) { fa = ua; fb = ub; fr = uc; }
	}

	unsigned blob[5] = { acc, fa, fb, fr, 0xED1FF000u };
	unsigned char hash[32];
	impulse_reveal(blob, sizeof(blob), hash);
	char hex[65];
	impulse_hash_hex(hash, hex);
	printf("ediff grid [%u,%u): acc=%08x first=(%08x+%08x->%08x) hash=%s\n",
		(unsigned)EDIFF_START, (unsigned)(EDIFF_START + EDIFF_COUNT), acc, fa, fb, fr, hex);
	return 0;
}
