// impulse: fadd register-bank probe — does the zfinx adapter mishandle
// fadd.s when rd/rs1/rs2 sit in high registers (x16-x31)? Jolt -O3 code
// uses high regs heavily; -Os probes stay low. Selected by FADD_REG_CASE.

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
	float a = 1.5f, b = -0.25f;
	float c = 0.0f;
	unsigned ua = bitsf(a), ub = bitsf(b), uc;

#if FADD_REG_CASE == 0
	// all low registers
	__asm__ volatile(
		"lw t0, 0(%[pa]); lw t1, 0(%[pb]); fadd.s t2, t0, t1; sw t2, 0(%[pc])"
		: : [pa]"r"(&a), [pb]"r"(&b), [pc]"r"(&c) : "t0", "t1", "t2", "memory");
#elif FADD_REG_CASE == 1
	// rs1/rs2 low, rd high
	__asm__ volatile(
		"lw t0, 0(%[pa]); lw t1, 0(%[pb]); fadd.s a6, t0, t1; sw a6, 0(%[pc])"
		: : [pa]"r"(&a), [pb]"r"(&b), [pc]"r"(&c) : "t0", "t1", "a6", "memory");
#elif FADD_REG_CASE == 2
	// rs1/rs2 high, rd low
	__asm__ volatile(
		"lw a6, 0(%[pa]); lw a7, 0(%[pb]); fadd.s t0, a6, a7; sw t0, 0(%[pc])"
		: : [pa]"r"(&a), [pb]"r"(&b), [pc]"r"(&c) : "a6", "a7", "t0", "memory");
#elif FADD_REG_CASE == 3
	// all high registers
	__asm__ volatile(
		"lw a6, 0(%[pa]); lw a7, 0(%[pb]); fadd.s s8, a6, a7; sw s8, 0(%[pc])"
		: : [pa]"r"(&a), [pb]"r"(&b), [pc]"r"(&c) : "a6", "a7", "s8", "memory");
#elif FADD_REG_CASE == 4
	// very high: s10/s11 + s9
	__asm__ volatile(
		"lw s10, 0(%[pa]); lw s11, 0(%[pb]); fadd.s s9, s10, s11; sw s9, 0(%[pc])"
		: : [pa]"r"(&a), [pb]"r"(&b), [pc]"r"(&c) : "s9", "s10", "s11", "memory");
#else
	// mixed mid: a2/a3 -> a4
	__asm__ volatile(
		"lw a2, 0(%[pa]); lw a3, 0(%[pb]); fadd.s a4, a2, a3; sw a4, 0(%[pc])"
		: : [pa]"r"(&a), [pb]"r"(&b), [pc]"r"(&c) : "a2", "a3", "a4", "memory");
#endif

	uc = bitsf(c);
	unsigned blob[5] = { uc, ua, ub, FADD_REG_CASE, 0x1234ABCDu };
	unsigned char hash[32];
	impulse_reveal(blob, sizeof(blob), hash);
	char hex[65];
	impulse_hash_hex(hash, hex);
	printf("fadd reg case %d: %08x + %08x -> %08x hash=%s\n", FADD_REG_CASE, ua, ub, uc, hex);
	return 0;
}
