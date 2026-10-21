# impulse

**Deterministic game physics, provable on-chain.**

impulse runs [JoltPhysics](https://github.com/jrouwe/JoltPhysics) on
[OpenVM](https://github.com/openvm-org/openvm) — a RISC-V zkVM — so a
physics simulation can be executed, hashed, and *proven*: the same guest
binary produces a bit-identical state hash on x86 and inside the
zero-knowledge proof.

The name: in physics, an impulse is a jolt.

## Results

All state hashes below agree across native x86-64 (1 thread and
JobSystemThreadPool 1–12), and the VM (rv32im_zfinx, single-threaded):

| Guest | instructions (VM) | hash |
|---|---|---|
| HelloWorld (48 steps) | 6,376,380 | `62003de6...aad0` |
| Pyramid h=15 (1240 boxes, 200 steps) | 23,468,076,988 | `ca1d02e2...9e9ec` |
| Pyramid h=4 (30 boxes) | 339,700,252 | `fcc59ef0...df97` |
| Live demo (60 s human session, h=6, 160 cmds) | 10,708,033,015 | `1e23f3bd...57dc0` |

Proving (CPU, this machine): hello = 10 s / 3.1 GB; pyramid h=2
(1.59M instr) = 143 s / 28.3 GB; live-demo session (1.98M instr) =
~148 s / ~28 GB. Economics: ~3.1 GB fixed + ~15 GB per 1M guest
instructions — this box proves ≤ ~2M instruction guests; larger
scenes execute but need a GPU path or another host to prove.

Proofs are persisted: `impulse-runner prove` writes a ~2 MB artifact
(verifying key + continuation proof, bincode) next to the guest;
`impulse-runner verify <artifact> [expected-hash]` verifies it
anywhere in ~20 ms — no ELF, no execution — and MATCHes the revealed
state hash against a recorded session.

## Layout

```
extern/JoltPhysics/   pinned submodule (unmodified — overlay/ shadows headers)
deps/                 musl (OpenVM-patched), libcxx, compiler-rt builtins,
                      llvm-patches/ (libunwind/libcxx overlays, latest 2 versions)
overlay/              Jolt platform layer + shadowed Jolt headers
demo/                 guests: hello, jolt_hello, pyramid, playground
                      (live demo; viewer.h = X11/GL viewer), + probes
runner/               Rust host runner: execute / prove / verify
runtime/              guest runtime: CRT entry, syscall layer, atomics
scripts/              verify-matrix.sh (three-leg determinism check)
docs/                 libcxx-comparison.md, future.md (proving economics study)
```

## Usage

Requires clang/LLVM and a recent Rust toolchain. Then:

```sh
git submodule update --init --recursive

make run-openvm                    # smoke guest on OpenVM (VM=zfinx)
make run-native-jolt               # x86-64 reference, single-threaded
make run-native-jolt THREADS=8     # or JobSystemThreadPool(8)
make run-openvm-jolt               # Jolt HelloWorld on the VM

make run-openvm-pyramid            # 1240-box pyramid (~2 min, ~23.5B instr)
make run-openvm-pyramid PYRAMID_HEIGHT=4 PYRAMID_ALLOW_SLEEP=1 PYRAMID_NO_SPLITTER=1
make prove-openvm-pyramid PYRAMID_HEIGHT=2 PYRAMID_STEPS=1 \
     PYRAMID_ALLOW_SLEEP=1 PYRAMID_NO_SPLITTER=1   # ~28 GB — check free -g

scripts/verify-matrix.sh           # determinism: native/MT/VM hashes agree
```

### Live demo: play it, prove it, replay it

```sh
make demo-play     # X11 window (F11 toggles fullscreen): f fire ball
                    # (hold arrows / , . to sweep the 32-slot aim, hold
                    # Up/Down to pitch from steeply down to a high lob,
                    # class 1-3), b drop box, x
                    # shockwave, g moon gravity, p pause — the session
                    # auto-ends at 60 s and the window auto-closes
                    # (--max-steps overrides). Records build/session.bin: the
                    # initial world dump, every key as a quantized command
                    # stamped with the step it applied at, and the final
                    # state hash.
make demo-verify   # runs the recording in OpenVM: rebuilds the world,
                    # checks the initial hash, replays every command at the
                    # exact step, reveals the final state hash and prints
                    # whether it matches the recording (and the revealed
                    # blob is byte-compared against the native one)
make demo-replay   # watch the recording again in the window: the same
                    # actions auto-fire, the same pile results
make demo-e2e      # headless round trip (no display): demo-auto + demo-verify
make demo-prove    # tiny session + full zk proof (~2M instr, ~30 GB — free -g)
```

`DEMO_HEIGHT` (default 6) sizes the pyramid; `DEMO_STEPS`/`DEMO_SCRIPT`
size headless sessions, e.g.
`make demo-auto DEMO_HEIGHT=8 DEMO_STEPS=600 DEMO_SCRIPT="F:60:0:2,X:200:2"`.
The viewer needs only libX11 + libGL; camera controls (mouse) are
never recorded — only the quantized physics commands are.

Future work (proving economics, circuit ideas): `docs/future.md`.

## Credits

The runtime pattern (musl + libc++ + compiler-rt for OpenVM guests), the
host runner, and the profiler are adapted from
[ray-tracer-on-openvm](https://github.com/xxuejie/ray-tracer-on-openvm)
by Xuejie Xiao (MIT). Physics by JoltPhysics (MIT). The float pipeline
comes from [openvm-floating](https://github.com/xxuejie/openvm-floating).
