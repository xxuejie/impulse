# impulse — future work

Parked projects, in rough order of leverage. Nothing here is started.

## 1. Jolt upstream: constraint-path work — capped at ~3% total (measured)

The original hope: ~22% of guest cycles go to re-creating contact
constraints from the manifold cache every step, so a persistent-
constraint redesign could reclaim a large slice. We instrumented the
whole path; the answer is no — best case is ~3%, the 22% headline is
mostly *necessary* per-step math. Closed direction, kept for the record.

### What was measured

**Memoized world-space inverse inertia** (implemented on a temporary
fork, kept as the ready-made upstream PR candidate: `xxuejie/impulse-jolt`,
branch `impulse`, commit `37815e3`, +25/−1 on top of upstream `71de1ae`;
NOT carried here — upstream JoltPhysics stays pinned unmodified):

- hash-identical on every leg (native single / native 8 threads / VM)
- pyramid h=4 −1.7%, h=8 −1.5% guest instructions
- the function's slow path drops 13.8M → 5.0M instructions (h=4)
- invalidation: SetInverseInertia, ScaleToMass, SetMassProperties
- upstream notes: the lazy cache fill is a benign data race when
  threaded constraint setup hits the same body concurrently — the clean
  fix is precomputing once per active body in a single-writer job (e.g.
  the gravity pass) and treating it as read-only afterwards; audit
  SetMassProperties / ScaleToMass / allowed-DOF changes; sibling win
  for jointed scenes: `MultiplyWorldSpaceInverseInertiaByVector`
  rebuilds the same product per solver iteration.

**Decomposition of the cache path** (h=4: 26,329 pair-visits, 26,329
manifolds, 94,927 contact points over 199 steps):

| component | instr | share of total | class |
|---|---|---|---|
| per-point Jacobian/property math (inlined) | ~52M | ~15.5% | **necessary** — positions change every step |
| cache double-buffer copy (Create + memcpy) | ~5.3M | 1.6% | removable by redesign |
| pair hash lookups (wrapper) | 8.2M | 2.4% | partially removable |
| constraint alloc + island link | ~2M | 0.6% | removable-ish |
| inverse inertia (memoized) | 4.9M | 1.5% | already optimized |
| combine callbacks | 0.3M | 0.1% | negligible |

### Verdict

A persistent-constraint redesign would buy **~3% best case, not 22%**
(removable overhead ~1.6+2.4+0.6% discounted for what a redesign can
actually reach; the Jacobian recomputation dominates and is inherent to
warm-started impulse solvers). The inertia memo alone is worth its
+25/−1 — send that upstream; skip everything else. Settled piles are
better served by sleeping (−56–69%, already available).

## 2. openvm circuit: packed / fused f32 instruction

The only ISA augmentation that attacks the dominant cost: the velocity
solver is ~42% of pyramid cycles and iteration-bound in pure f32 zfinx
code (`fmul.s`/`fadd.s`/`fsub.s` + `lw/sw`, no calls — nothing left to
compile away). A custom instruction in the openvm-floating pattern
(like the sha256 op) that fuses:

- a 2×f32 packed lane op (halves FP instruction count in Vec3-heavy
  code), or
- a dot3 / fmadd-style accumulate (collapses whole GJK/solver inner
  idioms)

would be the zfinx-tier win (zfinx itself was 5.6× over soft-float).
Requires circuit executors + a compiler story (intrinsics pass or
auto-vectorization for the custom encoding) — a real project, justified
only when bigger scenes must be proven cheaply.
## Other parked items

- **GPU proving / bigger host**: proving costs ~3.1 GB fixed +
  ~15 GB per 1M guest instructions on the CPU prover; beyond ~2M
  instruction guests (e.g. pyramid h=3+, HelloWorld at 6.4M) need the
  GPU path from the openvm benchmarks or another machine.
- **openvm upstream**: openvm's `air_test` hardcodes a degree-4
  constraint budget; openvm-floating's fsub fix needs 5 (we work around
  it with `air_test_deg5` in the runner). Worth an upstream nudge.
- **Godot guests** (G0–G5 staged notes in the ops manual): deterministic
  headless Godot on the same runtime. GDScript `float` is f64 = soft
  float on the VM — when this starts, **zdinx** (f64 in GPR pairs, the
  openvm-floating pattern) is the highest-leverage circuit augmentation,
  and the bitmanip (B) evaluation should be re-run on that integer-heavy
  profile.

## 3. Real-game proving economics (study, 2026-10)

Measured anchors (this repo, all bit-exact native == VM):

| anchor | value |
|---|---|
| guest fixed cost (startup + scene + world dump) | ~1.6M instr |
| light-contact body-step (flight / rolling / few contacts) | ~20-25K instr |
| stacked-contact body-step (deep crate pile, 8v/2p solver) | ~95K instr |
| human "chaos" minute (236 bodies, 160 cmds, sleep defeated) | 178M instr / sim-s |
| scripted "calm" minute (sleep effective) | ~28M instr / sim-s |
| guest cost split (folded profile, chaos session) | velocity solver ~26%, narrowphase ~12%, contact setup ~7%, broadphase ~6%, integration/islands ~49% |

Execution / replay speeds (60 s chaos session, 10.7B guest instr):

| engine | speed | vs real time |
|---|---|---|
| native x86-64 replay | 0.93 s | ~65x |
| OpenVM interpreter (zfinx) | 55.5 s | ~1.1x |
| CPU prover (~90 s / 1M instr, ~15 GB / 1M) | 267 days | never (160 TB) |
| GPU prover (openvm benchmarks) | 2-7M instr/s | 25-90 min |

Game archetypes -> sustained sim-second cost, derived from the anchors
(ragdoll = ~12 bodies + 11 joints ~= a small crate pile in the solver;
projectiles/characters as kinematic casts, not rigid bodies):

| archetype | profile | instr / game-s | GPUs for real-time proving* |
|---|---|---|---|
| on-chain lite (20-30 Hz, <20 light bodies) | OHOL-class | 5-20M | 1-5 |
| FPS/BR props (60 Hz, ~30 avg active, bursts 150) | Fortnite-class | 30-80M avg, ~400M peaks | 8-20 |
| ragdoll party (8 ragdolls, 60 Hz) | Gang Beasts-class | 300-700M | 75-175 |
| destruction sandbox | Teardown-class rigid bodies | 1-5B | 250-1250+ |

*at 2-7M instr/s per GPU; nominal 4M.

20-min match, batch proving (wall time = total / (4M x GPUs) + minutes
of aggregation; 1-s segments parallelize perfectly and are sized to
GPU RAM):

| archetype | total instr | 8 GPUs | 64 GPUs | 256 GPUs |
|---|---|---|---|---|
| on-chain lite | ~14B | ~6 min | ~47 s | ~12 s |
| FPS/BR props | ~66B | ~34 min | ~4.3 min | ~65 s |
| ragdoll party | ~576B | 7.5 h | ~37 min | ~9.4 min |

Conclusions: prop-physics and lighter titles are provable in
minutes-class wall time on a single-digit-to-64-GPU cluster (faster
than the match itself); ragdoll-heavy titles need ~64-256 GPUs for the
same; CPU proving is dead beyond ~2M-instr sessions. Verification
stays milliseconds regardless — generation is the only hard cost.

Levers, ranked by measured impact:
1. **30 Hz authority + client interpolation** (common in real titles): -50% across the board.
2. **Sleep discipline + deterministic reap of out-of-bounds bodies**: measured 6.4x between calm and chaos minutes; stray balls kept costing for the whole session.
3. **Circuit work** (packed / fused f32 below): the 26% velocity solver + scalar FP in narrowphase is the target; plausibly -20-35% total.
4. **GPU prover maturation**: the 2-7M instr/s path is an openvm benchmark today, not a product.

## 4. Parked: OpenVM AOT for fast execution / quick verification

To be clear about priorities: **proving is the bottleneck to optimize**
(GPU prover path above). AOT does not help proving — it helps
*execution*, which has its own everyday use: quickly checking a
result without paying for a proof. The interpreter replays the chaos
session at ~1.1x real time (55 s for a 60 s game — tolerable); OpenVM
AOT (guest compiled to native) should land replay near the native
~65x mark, i.e. **a 60 s session verified in ~1 s instead of ~55 s**.
That makes cheap: trustless-but-fast result checks for spectators
and opponents, debug bisecting of sessions (per-step hash hunting),
and witness generation wherever a prover wants a second opinion
before committing GPU time. Natural next step once quick-verify at
scale matters.
