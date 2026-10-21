/*
 * impulse: guest CRT entry — replaces musl's ckb/crt1.o for our guests.
 *
 * Why: crt1 hardcodes the initial stack pointer at 0x00200400, which with
 * the text segment starting at ~0x10000 leaves under a megabyte of stack
 * before growing into the program image. Jolt's constraint solver puts
 * large per-island arrays on the stack; the 1240-body pyramid island
 * overflows it and execution jumps to garbage (observed: pc 0).
 *
 * This entry mirrors ckb/crt1.c exactly (gp setup, TLS init, libc init,
 * main, terminate-success loop) but places the stack at GUEST_STACK_TOP
 * (default 400 MiB): below the mmap arena (416-480 MiB, see
 * runtime/guest_syscalls.c) and far above anything the brk heap will
 * reach, leaving ~400 MiB of downward growth.
 *
 * A direct object defining _start preempts libc.a's crt1.o member, which
 * is then never pulled (nothing else references its symbols).
 */

#define GUEST_STACK_TOP_DEFAULT 0x19000000ul /* 400 MiB */

#ifndef GUEST_STACK_TOP
#define GUEST_STACK_TOP GUEST_STACK_TOP_DEFAULT
#endif

#define GUEST_STR(x) #x
#define GUEST_XSTR(x) GUEST_STR(x)

__asm__(
".section .sdata,\"aw\"\n"
".text\n"
".global _start\n"
".type _start,%function\n"
"_start:\n"
".weak __global_pointer$\n"
".hidden __global_pointer$\n"
".option push\n"
".option norelax\n"
"lla gp, __global_pointer$\n"
".option pop\n\t"
"li sp, " GUEST_XSTR(GUEST_STACK_TOP) "\n\t"
"andi sp, sp, -16\n\t"
"tail _start_c"
);

#include <stdint.h>
#include <stddef.h>

extern int main(int, char **, char **);
extern void __init_tls(size_t *);
extern void __libc_start_init(void);

__attribute__((noreturn)) static inline void guest_terminate_success(void)
{
	__asm__ volatile(".insn i 0x0b, 0b000, x0, x0, 0");
	__builtin_unreachable();
}

/* argc/argv are not available in this environment; pass 0s like ckb's
 * crt1 does. */
void _start_c(long *p)
{
	(void)p;
	__init_tls(0);
	__libc_start_init();

	main(0, 0, 0);
	for (;;)
		guest_terminate_success();
}
