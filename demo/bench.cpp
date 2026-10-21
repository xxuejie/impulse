// impulse bench guest: libc++-heavy micro benchmark for toolchain comparisons.
//
// Exercises std::vector/sort, std::string, std::unordered_map, std::function,
// std::atomic and ostringstream — the libc++ surface JoltPhysics touches —
// plus an f32 loop (zfinx path). The final checksum is written through the
// standard impulse reveal protocol so the runner verifies execution.

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef OPENVM
#include <openvm/openvm_vm.h>
#include <openvm/openvm_sha256.h>
#endif

static uint32_t lcg(uint32_t& ioSeed)
{
	ioSeed = ioSeed * 1664525u + 1013904223u;
	return ioSeed;
}

int main()
{
	uint32_t seed = 42;
	uint32_t acc = 0;

	// vector + sort
	{
		std::vector<uint32_t> v;
		v.reserve(512);
		for (int i = 0; i < 512; ++i)
			v.push_back(lcg(seed));
		std::sort(v.begin(), v.end());
		for (uint32_t x : v)
			acc += x;
	}

	// string build / find
	{
		std::string s = "impulse";
		for (int i = 0; i < 64; ++i)
		{
			s += "x";
			s.insert(0, 1, char('a' + (i % 26)));
			acc += (uint32_t)s.size();
		}
		acc += s.find("impulse") != std::string::npos ? 7u : 13u;
	}

	// unordered_map insert / lookup
	{
		std::unordered_map<uint32_t, uint32_t> m;
		m.reserve(256);
		for (int i = 0; i < 256; ++i)
			m[lcg(seed) & 0xffu] = (uint32_t)i;
		for (int i = 0; i < 256; ++i)
		{
			auto it = m.find((uint32_t)i);
			acc += it != m.end() ? it->second : 1u;
		}
	}

	// std::function
	{
		std::function<uint32_t(uint32_t)> f = [](uint32_t x) { return x * 3u + 1u; };
		for (int i = 0; i < 256; ++i)
			acc += f((uint32_t)i);
	}

	// atomics (libatomic/compiler-rt path, no A extension)
	{
		std::atomic<uint32_t> a{0};
		for (int i = 0; i < 256; ++i)
			acc += a.fetch_add(1, std::memory_order_relaxed);
	}

	// ostringstream (locale/ios machinery — the known cycle hog).
	// Build with -DBENCH_NO_OSTREAM to measure without it.
#ifndef BENCH_NO_OSTREAM
	{
		std::ostringstream os;
		os << "n=" << acc << " seed=" << seed;
		acc += (uint32_t)os.str().size();
	}
#endif

	// f32 loop (zfinx instructions, libc++ independent)
	{
		float fa = 1.5f;
		for (int i = 0; i < 1024; ++i)
			fa = fa * 1.0009765625f + 0.25f;
		acc += (uint32_t)(fa * 256.0f);
	}

	printf("impulse bench checksum: %u\n", acc);

#ifdef OPENVM
	uint8_t blob[4] = {
		(uint8_t)acc, (uint8_t)(acc >> 8), (uint8_t)(acc >> 16), (uint8_t)(acc >> 24) };
	uint8_t hash[32];
	openvm_sha256(blob, sizeof(blob), hash);
	uint8_t* out = (uint8_t*)OPENVM_CARVE_START;
	memcpy(out, blob, sizeof(blob));
	openvm_reveal_u32(0, OPENVM_CARVE_START);
	openvm_reveal_u32(4, (uint32_t)sizeof(blob));
	for (int i = 0; i < 32; i += 4)
		openvm_reveal_u32(8 + i, (uint32_t)hash[i] | ((uint32_t)hash[i + 1] << 8)
			| ((uint32_t)hash[i + 2] << 16) | ((uint32_t)hash[i + 3] << 24));
#endif
	return 0;
}
