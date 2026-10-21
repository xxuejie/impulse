# libc++ version comparison: 18 → 22 (+ bitset partition opt-out)

Date: 2026-10-10. Question: can we upgrade libcxx (ckb-libcxx-builder) from
LLVM 18.1.8 to newer stable releases, and does it matter for guest size and
performance?

## Setup

- Guest compiler: system clang 22.1.8 in all cases — only the libc++ /
  libc++abi / libunwind headers + archives differ.
- Same musl sysroot, same compiler-rt builtins, same flags (`-Os`,
  rv32im_zfinx), same host runner on OpenVM.
- Every version builds cleanly. Two patch classes in the per-version patch dirs (now `deps/llvm-patches/<v>/`, latest 2 kept):
  - libunwind `Registers.hpp`: all of 19–22 read the `vlenb` CSR
    unconditionally in `Registers_riscv::getRegister`, which traps on OpenVM
    (no Zicsr) — one-hunk fix per tag.
  - `__algorithm/sort.h` (20+): adds the `_LIBCPP_SORT_NO_BITSET_PARTITION`
    opt-out (see below).
- Workload: `demo/bench.cpp` (std::vector+sort, std::string,
  std::unordered_map, std::function, std::atomic, ostringstream, f32 loop),
  run via `make bench-openvm LIBCXX_VERSION=<v>`.

## The bitset partition story

LLVM 20's `std::sort` selects `__bitset_partition` for arithmetic types via
the `__use_branchless_sort` trait (contiguous iterator + cheap-to-copy +
arithmetic + `less`/`greater` comparator). There is **no upstream macro or
CMake switch** to disable it. It trades branches for ~3x more instructions
on the partition path — good for speculative superscalar CPUs, a pure cycle
regression on the in-order 1-IPC cores of zkVMs.

Our overlay patches make the trait `false` when
`_LIBCPP_SORT_NO_BITSET_PARTITION` is defined. The macro must be defined in
**both** places: when building libc++ (plain `std::sort` on arithmetic
pointers resolves to extern-template `__sort<...>` instantiations baked into
`libc++.a`) and when compiling guests (header-inline sorts). The Makefile
wires it into both (`SORT_DEFINES`).

Escape hatch without patching: pass a custom comparator (e.g. a lambda) —
it does not desugar to `std::less`, so the trait is false and the classic
partition is used.

## Results (zfinx, `demo/bench.cpp`)

| Version | text | cycles (stock) | cycles (macro) |
|---|---|---|---|
| 18.1.8 | 376,044 B | 252,092 | 252,092 (macro unused) |
| 19.1.7 | 377,208 B | 252,127 | 252,127 (macro unused) |
| 20.1.8 | 377,976 B | 324,650 | 262,635 |
| 21.1.8 | 385,320 B | 326,618 | 261,681 |
| 22.1.8 | 400,016 B | 324,553 | **259,616** |

All builds produce bit-identical guest checksums. The cycle cliff at LLVM 20
disappears with the macro; the residual ~+3% vs 18.1.8 is other library
changes (visible in text: +6% on 22, mostly iostream/locale-adjacent code —
cycle-cheap).

## Conclusion

- **Default: 22.1.8 with `_LIBCPP_SORT_NO_BITSET_PARTITION`** — newest
  stable (bug fixes land upstream, libc++ is not as stable as musl) at ~3%
  cycle cost and +6% text vs 18.1.8, ~20% fewer cycles than stock 22.
- 18.1.8 / 19.1.8 remain available via `LIBCXX_VERSION` if absolute minimum
  cycles are needed (19.1.7 is the cycle floor with newest-classic-sort).
- 20.1.8/21.1.8 installs in this tree are rebuilt with the macro baked in.
- The Makefile guards the builder's `llvm_src` cache with a version stamp —
  switching `LIBCXX_VERSION` clears stale extracted sources automatically
  (the tarball/`llvm_src`/`build` cache is regenerable; the `release-*`
  installs are the actual toolchains).
