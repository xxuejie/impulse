// impulse: fadd count probe — execute exactly FADD_N fadd.s instructions
// in a tight loop (result XOR-folded) to find the row count at which
// FaddCoreAir constraints start failing during proving (h=2 pyramid failed
// at row 58706; small guests pass — suspected positional/row-space issue).

#include <stdio.h>
#include <string.h>

#ifdef OPENVM
#include "reveal.h"
#else
#include "sha256.h"
#endif

#ifndef FADD_N
#define FADD_N 1000
#endif

static unsigned bitsf(float f) { unsigned u; memcpy(&u, &f, 4); return u; }

int main()
{
	float a = 1.0000010f, b = -0.9999985f;
	volatile float va = a, vb = b;
	float c = va + vb;
	unsigned acc = bitsf(c);

	for (unsigned i = 0; i < FADD_N - 1; ++i)
	{
		va = c * 1.0000001f - 0.0000001f * (float)(i & 7);
		c = va + vb;
		acc ^= bitsf(c);
	}

	unsigned blob[4] = { acc, bitsf(c), FADD_N, 0xFEEDBEEFu };
	unsigned char hash[32];
	impulse_reveal(blob, sizeof(blob), hash);
	char hex[65];
	impulse_hash_hex(hash, hex);
	printf("fadd count %d: acc=%08x hash=%s\n", FADD_N, acc, hex);
	return 0;
}
