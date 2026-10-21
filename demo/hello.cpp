// impulse M0 smoke guest: validates the full toolchain (musl + libc++ +
// compiler-rt + openvm guest API) before Jolt itself lands in M1.
//
// It exercises the output protocol that every impulse guest will use:
//   1. Write a deterministic state blob into the carved output region.
//   2. SHA-256 it via the OpenVM sha256 custom instruction.
//   3. Reveal (ptr, size, hash) as public values.
//   4. Exit; the host runner re-hashes the blob and must match.
//
// The VM-specific parts are guarded by OPENVM (defined in the cross build
// only) so the same file builds as a plain native smoke test.

#include <stdio.h>
#include <string.h>

#ifdef OPENVM
#include <openvm/openvm_vm.h>
#include <openvm/openvm_sha256.h>
#endif

static unsigned char g_blob[64];

#ifdef OPENVM
static void reveal_hash(const unsigned char inHash[32])
{
	for (int i = 0; i < 32; i += 4)
	{
		uint32_t word = (uint32_t)inHash[i]
			| ((uint32_t)inHash[i + 1] << 8)
			| ((uint32_t)inHash[i + 2] << 16)
			| ((uint32_t)inHash[i + 3] << 24);
		openvm_reveal_u32(8 + i, word);
	}
}
#endif

int main()
{
	printf("impulse: M0 smoke guest\n");

	for (size_t i = 0; i < sizeof(g_blob); ++i)
		g_blob[i] = (unsigned char)(i * 7 + 1);

#ifdef OPENVM
	unsigned char hash[32];
	openvm_sha256(g_blob, sizeof(g_blob), hash);

	unsigned char* out = (unsigned char*)OPENVM_CARVE_START;
	memcpy(out, g_blob, sizeof(g_blob));
	openvm_reveal_u32(0, OPENVM_CARVE_START);
	openvm_reveal_u32(4, (uint32_t)sizeof(g_blob));
	reveal_hash(hash);

	printf("impulse: revealed blob hash, terminating\n");
#else
	printf("impulse: native build (no reveal; VM-only)\n");
#endif
	return 0;
}
