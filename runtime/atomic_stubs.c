/*
 * impulse: __atomic_* libatomic stubs for RV32IM (no A extension) guests.
 *
 * OpenVM executes a single thread, so lock-free plain accesses are correct.
 * These satisfy the libatomic calls clang emits for std::atomic on targets
 * without the A extension.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ATOMIC_N(N, T)                                                                    \
	T __atomic_load_##N(const volatile void* p)                                            \
	{                                                                                      \
		return *(const volatile T*)p;                                                      \
	}                                                                                      \
	void __atomic_store_##N(volatile void* p, T v)                                         \
	{                                                                                      \
		*(volatile T*)p = v;                                                               \
	}                                                                                      \
	T __atomic_exchange_##N(volatile void* p, T v)                                         \
	{                                                                                      \
		T old = *(volatile T*)p;                                                           \
		*(volatile T*)p = v;                                                               \
		return old;                                                                        \
	}                                                                                      \
	bool __atomic_compare_exchange_##N(volatile void* p, void* e, T d, bool weak, int s, int f) \
	{                                                                                      \
		(void)weak; (void)s; (void)f;                                                      \
		T cur = *(volatile T*)p;                                                           \
		if (cur == *(T*)e)                                                                 \
		{                                                                                  \
			*(volatile T*)p = d;                                                           \
			return true;                                                                   \
		}                                                                                  \
		*(T*)e = cur;                                                                      \
		return false;                                                                      \
	}                                                                                      \
	T __atomic_fetch_add_##N(volatile void* p, T v) { T o = *(volatile T*)p; *(volatile T*)p = o + v; return o; } \
	T __atomic_fetch_sub_##N(volatile void* p, T v) { T o = *(volatile T*)p; *(volatile T*)p = o - v; return o; } \
	T __atomic_fetch_and_##N(volatile void* p, T v) { T o = *(volatile T*)p; *(volatile T*)p = o & v; return o; } \
	T __atomic_fetch_or_##N(volatile void* p, T v)  { T o = *(volatile T*)p; *(volatile T*)p = o | v; return o; } \
	T __atomic_fetch_xor_##N(volatile void* p, T v) { T o = *(volatile T*)p; *(volatile T*)p = o ^ v; return o; } \
	T __atomic_fetch_nand_##N(volatile void* p, T v){ T o = *(volatile T*)p; *(volatile T*)p = (T)~(o & v); return o; }

ATOMIC_N(1, uint8_t)
ATOMIC_N(2, uint16_t)
ATOMIC_N(4, uint32_t)
ATOMIC_N(8, uint64_t)

/* Note: __atomic_test_and_set / __atomic_clear / __atomic_*_fence are clang
 * builtins that expand inline; they cannot be redefined here. The sized
 * __atomic_* entry points above are the ones emitted as library calls. */
