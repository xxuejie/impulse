/*
 * impulse: OpenVM input-stream reading for C/C++ guests.
 *
 * Mirrors extensions/rv32im/guest/src/io.rs from the pinned openvm fork
 * (Rust guests use openvm::io::read_vec; same custom-instruction family
 * as reveal/sha256 in openvm_vm.h — SYSTEM_OPCODE 0x0b):
 *
 *   .insn i 0x0b, 0b011, x0, x0, 0   hint_input: pop the next chunk
 *                                     from the host input stream into
 *                                     the hint stream (prepends a
 *                                     4-byte LE length)
 *   .insn i 0x0b, 0b001, rd, x0, 0    hint_store_u32: store the next
 *                                     4 bytes of the hint stream to
 *                                     memory at rd
 *   .insn i 0x0b, 0b001, rd, rs1, 1   hint_buffer_u32: store the next
 *                                     rs1 words (max 1023) to memory
 *                                     at rd
 *
 * Host side: runner passes Streams::new(vec![chunk]) where chunk is the
 * input file bytes, one field element per byte.
 */

#ifndef OPENVM_IO_H
#define OPENVM_IO_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef OPENVM

#define OPENVM_HINT_MAX_WORDS 1023u

/* Pop the next input-stream chunk into the hint stream. */
static inline void openvm_hint_input(void)
{
	__asm__ volatile(".insn i 0x0b, 0b011, x0, x0, 0");
}

/* Store the next 4 bytes of the hint stream to *addr. */
static inline void openvm_hint_store_u32(void* addr)
{
	register void* p asm("t0") = addr;
	__asm__ volatile(".insn i 0x0b, 0b001, t0, x0, 0"
		:
		: "r"(p)
		: "memory");
}

/* Store the next inWords (<= 1023) words of the hint stream to addr. */
static inline void openvm_hint_buffer_u32(void* addr, uint32_t inWords)
{
	register void* p asm("t0") = addr;
	register uint32_t n asm("t1") = inWords;
	__asm__ volatile(".insn i 0x0b, 0b001, t0, t1, 1"
		:
		: "r"(p), "r"(n)
		: "memory");
}

/*
 * Read one length-prefixed blob from the input stream (the host chunk).
 * Returns a malloc'd buffer of *outLen bytes (caller frees), or NULL on
 * stream failure.
 */
static inline unsigned char* openvm_read_input(uint32_t* outLen)
{
	openvm_hint_input();

	uint32_t len;
	openvm_hint_store_u32(&len);
	if (len == 0u || len > (256u * 1024u * 1024u))
		return NULL;

	unsigned char* buf = (unsigned char*)malloc(len);
	if (!buf)
		return NULL;

	/* whole words, in <=1023-word chunks */
	uint32_t words = len / 4u;
	uint32_t done = 0;
	while (words > 0)
	{
		const uint32_t n = words > OPENVM_HINT_MAX_WORDS ? OPENVM_HINT_MAX_WORDS : words;
		openvm_hint_buffer_u32(buf + done, n);
		done += 4u * n;
		words -= n;
	}

	/* tail: hint stream is 4-byte aligned — read one padded word */
	const uint32_t rem = len % 4u;
	if (rem > 0)
	{
		uint32_t tail = 0;
		openvm_hint_store_u32(&tail);
		memcpy(buf + done, &tail, rem);
	}

	*outLen = len;
	return buf;
}

#endif /* OPENVM */
#endif /* OPENVM_IO_H */
