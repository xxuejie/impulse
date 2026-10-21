#!/usr/bin/env python3
"""impulse: session stats + proving-cost projection.

Usage: session-stats.py [session.bin] [instruction_count]

Parses the recorded live-demo session (config, command mix, sim time)
and, when the guest instruction count is given (from `make demo-stats`,
which runs the session once on the VM), projects proving cost:

  CPU prover (calibrated on this repo's machine): ~90 s / 1M guest
  instructions, ~3.1 GB fixed + ~15 GB / 1M (pyramid h=2: 1.59M instr =
  143 s / 28.3 GB, README).
  GPU path (openvm benchmarks, docs/future.md): ~2-7M guest
  instructions / s of proving.
"""
import struct
import sys

OPS = {1: "fire", 2: "drop", 3: "shock", 4: "gravity"}

# calibrated on this machine (README: 1.59M instr -> 143 s, 28.3 GB)
CPU_SECS_PER_M = 90.0
CPU_GB_FIXED = 3.1
CPU_GB_PER_M = 15.0
GPU_M_PER_SEC = (2.0, 7.0)


def main() -> int:
    # --hash <session>: print the recorded final hash only (for proof checks)
    if len(sys.argv) >= 3 and sys.argv[1] == "--hash":
        data = open(sys.argv[2], "rb").read()
        print(data[76:108].hex())
        return 0

    path = sys.argv[1] if len(sys.argv) > 1 else "build/session.bin"
    instr = int(sys.argv[2]) if len(sys.argv) > 2 else None

    data = open(path, "rb").read()
    magic, version, height, flags, sv, sp, steps, ncmds, nbodies0, _ = (
        struct.unpack_from("<8s9I", data, 0))
    if magic != b"IMPSESS1":
        print(f"not an impulse session: {path}")
        return 1
    final_hash = data[76:108]

    print(f"session:    {path}")
    print(f"scene:      pyramid h={height} ({nbodies0} bodies incl. floor), "
          f"solver {sv or 8}/{sp or 2}, flags {flags:#04x}")
    print(f"length:     {steps} steps = {steps / 60.0:.1f} s of sim time")
    mix = {}
    for i in range(ncmds):
        op = struct.unpack_from("<I", data, 108 + 16 * i + 4)[0] & 0xFF
        mix[OPS.get(op, f"op{op}")] = mix.get(OPS.get(op, f"op{op}"), 0) + 1
    print(f"commands:   {ncmds} ({', '.join(f'{k}x{v}' for k, v in sorted(mix.items())) or 'none'})")
    print(f"final hash: {final_hash.hex()}")

    if instr is None:
        print("\n(run with an instruction count for cost projections, or: make demo-stats)")
        return 0

    m = instr / 1e6
    print(f"\ninstructions: {instr:,} ({m:.2f}M; "
          f"{instr / max(steps, 1):,.0f}/step, {instr * 60 / max(steps, 1) / 1e6:.0f}M/sim-second)")

    cpu_s = CPU_SECS_PER_M * m
    cpu_gb = CPU_GB_FIXED + CPU_GB_PER_M * m
    print(f"prove (CPU):  ~{cpu_s:,.0f} s ({cpu_s / 60:.1f} min), ~{cpu_gb:,.0f} GB peak")
    print(f"prove (GPU):  ~{m / GPU_M_PER_SEC[1]:,.0f}-{m / GPU_M_PER_SEC[0]:,.0f} s "
          f"(openvm GPU path, {GPU_M_PER_SEC[0]:.0f}-{GPU_M_PER_SEC[1]:.0f}M instr/s)")
    print(f"verify:       milliseconds anywhere (proof verification is cheap; "
          f"generation is the cost)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
