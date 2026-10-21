// impulse: guest output protocol helpers.
//
// Every impulse guest finishes the same way (see demo/hello.cpp):
//   1. Write a deterministic state blob into the carved output region.
//   2. SHA-256 it via the OpenVM sha256 custom instruction.
//   3. Reveal (ptr, size, hash) as public values: ptr@0, size@4, hash@8..40.
//
// VM-specific parts are OPENVM-guarded so the same demo sources build as
// native reference binaries.

#pragma once

#include <stdint.h>
#include <string.h>

#include "sha256.h"

#ifdef OPENVM
#include <openvm/openvm_vm.h>
#include <openvm/openvm_sha256.h>
#endif

#ifdef OPENVM

// Copies the state blob into the carve region, hashes it with the sha256
// custom instruction and reveals (ptr, size, hash). Returns the hash via
// outHash so callers can cross-check it against demo/sha256.h.
static void impulse_reveal(const void* inData, uint32_t inSize, unsigned char outHash[32])
{
	openvm_sha256((const uint8_t*)inData, inSize, outHash);

	unsigned char* out = (unsigned char*)OPENVM_CARVE_START;
	memcpy(out, inData, inSize);
	openvm_reveal_u32(0, OPENVM_CARVE_START);
	openvm_reveal_u32(4, inSize);
	for (int i = 0; i < 32; i += 4)
	{
		uint32_t word = (uint32_t)outHash[i]
			| ((uint32_t)outHash[i + 1] << 8)
			| ((uint32_t)outHash[i + 2] << 16)
			| ((uint32_t)outHash[i + 3] << 24);
		openvm_reveal_u32(8 + i, word);
	}
}

#else

// Native reference build: no reveal, just compute the hash so it can be
// compared against the VM run.
static void impulse_reveal(const void* inData, uint32_t inSize, unsigned char outHash[32])
{
	impulse_sha256((const unsigned char*)inData, inSize, outHash);
}

#endif

static inline void impulse_hash_hex(const unsigned char inHash[32], char outHex[65])
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++)
	{
		outHex[i * 2] = hex[inHash[i] >> 4];
		outHex[i * 2 + 1] = hex[inHash[i] & 0xf];
	}
	outHex[64] = 0;
}
