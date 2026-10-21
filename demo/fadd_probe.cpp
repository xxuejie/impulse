// impulse: fadd circuit-edge-case probe.
//
// The zfinx FaddCoreAir constraint set fails on some f32 input produced by
// physics guests (first seen proving the h=2 pyramid: "constraints had
// nonzero value on air VmAirWrapper<Rv32FpAdapterAir, FaddCoreAir>"). This
// guest performs ONE edge-case addition, selected by FADD_CASE, so
// build+prove per case bisects the culprit. Results fold into a revealed
// checksum so nothing is optimized away.

#include <stdio.h>
#include <string.h>

#ifdef OPENVM
#include "reveal.h"
#else
#include "sha256.h"
#endif

static unsigned bitsf(float f) { unsigned u; memcpy(&u, &f, 4); return u; }

int main()
{
	float a, b;

#if FADD_CASE == 0
	a = 1.0f; b = 0.5f;                    // plain
#elif FADD_CASE == 1
	a = 1.0f; b = -1.0f;                   // exact cancellation -> +0
#elif FADD_CASE == 2
	a = -1.0f; b = 1.0f;                   // cancellation, operand order
#elif FADD_CASE == 3
	a = 0.0f; b = -0.0f;                   // +0 + -0 -> +0
#elif FADD_CASE == 4
	a = -0.0f; b = -0.0f;                  // -0 + -0 -> -0
#elif FADD_CASE == 5
	a = 1e-45f; b = 1e-45f;                // subnormal + subnormal
#elif FADD_CASE == 6
	a = 1e-38f; b = -0.9e-38f;             // underflow toward subnormal
#elif FADD_CASE == 7
	a = 3.4e38f; b = 3.4e38f;              // overflow -> +inf
#elif FADD_CASE == 8
	a = -3.4e38f; b = -3.4e38f;            // overflow -> -inf
#elif FADD_CASE == 9
	a = 16777216.0f; b = 1.0f;             // tie, round-to-even
#elif FADD_CASE == 10
	a = 1.0f; b = 1e-30f;                  // large exponent alignment
#elif FADD_CASE == 11
	a = -5.0f; b = 5.0f;                   // cancellation from distinct values
#elif FADD_CASE == 12
	a = 0.1f; b = 0.3000001f;              // generic inexact
#elif FADD_CASE == 13
	a = 1.0f; b = 0.0f;                    // identity
#elif FADD_CASE == 14
	a = -0.0f; b = 0.0f;                   // -0 + +0 -> +0
#elif FADD_CASE == 15
	a = 1e-44f; b = -1e-44f;               // subnormal cancellation -> +0?
#else
	a = 2.5f; b = -1.25f;                  // plain negative
#endif

	volatile float va = a, vb = b;
	float c = va + vb;

	unsigned blob[2] = { bitsf(c), 0xFADDCA5Eu };
	unsigned char hash[32];
	impulse_reveal(blob, sizeof(blob), hash);

	char hex[65];
	impulse_hash_hex(hash, hex);
	printf("fadd case %d: result=%08x hash=%s\n", FADD_CASE, blob[0], hex);
	return 0;
}
