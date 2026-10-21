/*
 * impulse: guest syscall layer for OpenVM (strong override of musl's weak
 * default_hijack_syscall — see deps/musl/ckb/hijack_syscall.c and
 * arch/riscv32/syscall_arch.h for the calling convention).
 *
 * WHY THIS EXISTS
 *
 * musl (oldmalloc) serves allocations larger than MMAP_THRESHOLD (112 KiB
 * on rv32) via __mmap, and the stock hijack FAILS every mmap by design.
 * Consequence: malloc(> 112 KiB) returned NULL on the VM. Jolt's
 * TempAllocatorImpl (10 MiB) and ContactConstraintManager's two ~200 KiB
 * cache stores all landed on NULL — writing over each other in flat low
 * memory, and the cache's first allocation (offset 0, i.e. a NULL
 * KeyValue*) was misread by AddBodyPair as "cache full", silently dropping
 * every contact (EPhysicsUpdateError::BodyPairCacheFull). Small guests
 * were even worse: they link lite_malloc, whose 8 MiB stack guard blocks
 * ALL brk growth when _end sits just below the stack at 0x00200400 —
 * every malloc fails.
 *
 * WHAT WE CHANGE
 *
 *   - mmap2 (anonymous, !MAP_FIXED, fd == -1, addr == 0): served from a
 *     static page-aligned bump arena inside .bss. Chunks are never
 *     reclaimed.
 *   - munmap: no-op success (arena chunks are bump-only; big-chunk free()
 *     must not fall through to a real ecall, which traps on OpenVM).
 *   - brk: stateless, like the stock hijack (both mallocs keep their own
 *     cursors): brk(0) -> page-aligned _end, brk(a) accepted within
 *     [page_align(_end), OPENVM_HEAP_END).
 *   - writev (stdout/stderr) -> openvm_print, ioctl(TIOCGWINSZ),
 *     set_tid_address, madvise: same behavior as the stock hijack.
 *   - anything else: *processed stays 0 -> falls through to the real
 *     ecall (unchanged stock behavior).
 *
 * The arena is a static array, so its addresses are fixed per binary —
 * deterministic, no collision with the brk heap (which starts after
 * _end, i.e. after the arena).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <openvm/openvm_vm.h>

#ifndef GUEST_MMAP_ARENA_SIZE
#define GUEST_MMAP_ARENA_SIZE (64u * 1024 * 1024)
#endif

/* The arena lives at a fixed virtual address at the TOP of the heap
 * region, just below the 32 MiB output carve. It must not be in .bss:
 * bss sits just above the text segment (~1-2 MiB), and a large arena
 * there would overlap the initial stack at 0x00200400 — the very first
 * big allocation would smash the stack (observed: jump to pc 0). */
#define GUEST_MMAP_ARENA_START ((uintptr_t)OPENVM_HEAP_END - GUEST_MMAP_ARENA_SIZE)

/* Must match struct syscall_context in deps/musl/ckb/hijack_syscall.c:
 * the caller (arch/riscv32/syscall_arch.h) builds it with designated
 * initializers and passes a pointer as the 8th argument. Field order is
 * part of the ABI with the pinned musl submodule. */
struct guest_syscall_context {
	long n;
	long a;
	long b;
	long c;
	long d;
	long e;
	long f;
	int *processed;
};

extern char _end[];

static long guest_page_align_up(long x)
{
	return (x + 4095) & ~4095L;
}

static long guest_brk_min(void)
{
	static long cached = 0;
	if (!cached)
		cached = guest_page_align_up((long)_end);
	return cached;
}

/* brk must stop below the arena, or a long-growing heap could collide. */
static long guest_brk_max(void)
{
	return (long)GUEST_MMAP_ARENA_START;
}

static long g_mmap_cursor = 0;

static long guest_mmap(struct guest_syscall_context *ctx)
{
	long len = ctx->b;
	long flags = ctx->d;
	long fd = ctx->e;
	long addr = ctx->a;

	/* Only plain anonymous mappings are supported. */
	if (addr != 0 || fd != -1 || (flags & MAP_FIXED) != 0 || (flags & MAP_ANON) == 0)
		return -1;
	if (len <= 0 || len > (long)GUEST_MMAP_ARENA_SIZE)
		return -1;

	if (g_mmap_cursor == 0)
		g_mmap_cursor = guest_page_align_up((long)GUEST_MMAP_ARENA_START);

	long start = guest_page_align_up(g_mmap_cursor);
	long end = start + guest_page_align_up(len);
	if (end > (long)GUEST_MMAP_ARENA_START + (long)GUEST_MMAP_ARENA_SIZE)
		return -1; /* arena exhausted: behave like the stock hijack */

	/* No memset: anonymous mmap must read as zeros, and it does — guest
	 * RAM starts zeroed (verified with a probe) and arena addresses are
	 * never reused within a run (bump-only allocation, munmap is a no-op
	 * and oldmalloc never recycles freed mmapped chunks). musl's calloc
	 * relies on mmap-zero semantics for big chunks, so this invariant
	 * must hold — re-verify if the allocator changes. Zeroing here cost
	 * ~2.9% of total guest instructions on the M2 pyramid profile. */
	g_mmap_cursor = end;
	return start;
}

static long guest_writev(struct guest_syscall_context *ctx)
{
	int fd = (int)ctx->a;
	if (fd != 1 && fd != 2)
		return -1;

	const struct iovec *iov = (const struct iovec *)ctx->b;
	int iovcnt = (int)ctx->c;
	long total = 0;
	for (int i = 0; i < iovcnt; i++)
	{
		openvm_print(iov[i].iov_base, (uint32_t)iov[i].iov_len);
		total += (long)iov[i].iov_len;
	}
	return total;
}

static long guest_ioctl(struct guest_syscall_context *ctx)
{
	int fd = (int)ctx->a;
	if (fd != 1 && fd != 2)
		return -1;
	if ((int)ctx->b != TIOCGWINSZ)
		return -1;

	struct winsize *wsz = (struct winsize *)ctx->c;
	wsz->ws_row = 80;
	wsz->ws_col = 25;
	wsz->ws_xpixel = 14;
	wsz->ws_ypixel = 14;
	return 0;
}

long __ckb_hijack_syscall(long n, long a, long b, long c, long d, long e, long f, int *processed)
{
	struct guest_syscall_context ctx = { n, a, b, c, d, e, f, processed };

	if (n == SYS_brk)
	{
		*processed = 1;
		if (a == 0)
			return guest_brk_min();
		if (a >= guest_brk_min() && a <= guest_brk_max())
			return a;
		return -1;
	}

	if (n == SYS_mmap2)
	{
		long ret = guest_mmap(&ctx);
		if (ret != -1)
		{
			*processed = 1;
			return ret;
		}
		/* Not representable: report processed failure like the stock
		 * hijack (returning -1 without processing would ecall-trap). */
		*processed = 1;
		return -1;
	}

	if (n == SYS_munmap)
	{
		/* Arena chunks are bump-allocated; frees are no-ops. */
		*processed = 1;
		return 0;
	}

	if (n == SYS_madvise)
	{
		*processed = 1;
		return 0;
	}

	if (n == SYS_writev)
	{
		long ret = guest_writev(&ctx);
		if (ret != -1)
		{
			*processed = 1;
			return ret;
		}
		return -1;
	}

	if (n == SYS_ioctl)
	{
		long ret = guest_ioctl(&ctx);
		if (ret != -1)
		{
			*processed = 1;
			return ret;
		}
		return -1;
	}

	if (n == SYS_set_tid_address)
	{
		*processed = 1;
		*((int *)ctx.a) = 0;
		return 0;
	}

	return -1;
}
