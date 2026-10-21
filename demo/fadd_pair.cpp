// impulse: fadd single-pair probe — one exact fadd of bit patterns
// PA_BITS + PB_BITS, revealed. For bracketing the FaddCoreAir failure
// (h=2 pyramid repro reduced to ediff-boundary pairs).

#include <stdio.h>
#include <string.h>

#ifdef OPENVM
#include "reveal.h"
#else
#include "sha256.h"
#endif

#ifndef PA_BITS
#define PA_BITS 0x3f000000u
#endif
#ifndef PB_BITS
#define PB_BITS 0xb2800001u
#endif

static unsigned bitsf(float f) { unsigned u; memcpy(&u, &f, 4); return u; }

int main()
{
	float a, b;
	unsigned ua = PA_BITS, ub = PB_BITS;
	memcpy(&a, &ua, 4);
	memcpy(&b, &ub, 4);
	volatile float va = a, vb = b;
	float c = va + vb;
	unsigned uc = bitsf(c);

	unsigned blob[4] = { uc, ua, ub, 0x0BADC0DEu };
	unsigned char hash[32];
	impulse_reveal(blob, sizeof(blob), hash);
	char hex[65];
	impulse_hash_hex(hash, hex);
	printf("pair %08x + %08x -> %08x hash=%s\n", ua, ub, uc, hex);
	return 0;
}
