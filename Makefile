# Toolchain: CLANG may be overridden (e.g. CLANG=clang-22 or a versioned
# clang on Ubuntu). LLVM tools resolve as <tool> or <tool>-<major> taken
# from `$(CLANG) --version` (Ubuntu ships only versioned names).
CLANG ?= clang
CLANG_VERSION := $(shell $(CLANG) --version | sed -n '1s/.*version \([0-9][0-9]*\).*/\1/p')
define llvm_tool
$(shell for t in $(1) $(1)-$(CLANG_VERSION); do command -v $$t >/dev/null 2>&1 && { echo $$t; break; }; done)
endef
CLANGXX := $(subst clang,clang++,$(CLANG))
LD := $(call llvm_tool,ld.lld)
OBJCOPY := $(call llvm_tool,llvm-objcopy)
OBJDUMP := $(call llvm_tool,llvm-objdump)
# llvm-ar everywhere it exists; plain `ar` as the last resort (stock
# macOS has no llvm tools in PATH — fine for the NATIVE-only builds,
# which is all macOS/Windows targets need)
AR := $(or $(call llvm_tool,llvm-ar),ar)

JOLT := $(abspath extern/JoltPhysics)
MUSL := $(abspath deps/musl)
MUSL_TARGET := $(MUSL)/release/include/openvm/openvm_vm.h
BUILTINS := $(abspath deps/builtins)
BUILTINS_TARGET := $(BUILTINS)/build/libcompiler-rt.a
LIBCXX := $(abspath deps/libcxx)

DEBUG := false
# softfloat, zfinx, f
VM := zfinx

# libc++ variant: 22.1.8 (default), 21.1.8. Patches for the two most
# recent versions live under deps/llvm-patches/<version>/ (older versions
# were dropped; their prebuilt release-<v> installs in deps/libcxx still
# work but cannot be rebuilt without re-adding a patch dir).
# LLVM 20+ std::sort uses __bitset_partition (fewer branches, ~3x more
# instructions on that path: good for speculative CPUs, bad for in-order
# 1-IPC zkVM cores). The patches add an opt-out macro,
# _LIBCPP_SORT_NO_BITSET_PARTITION, which we define for guests and during
# the libc++ build (extern-template sort instantiations live in the
# archive). With it, all versions are within ~4% of each other; the default
# is the newest stable for maintenance. See docs/libcxx-comparison.md.
LIBCXX_VERSION := 22.1.8
LIBCXX_DIR := $(LIBCXX)/release-$(LIBCXX_VERSION)
LIBCXX_PATCH := $(abspath deps/llvm-patches/$(LIBCXX_VERSION))
ifeq ($(LIBCXX_PATCH),)
	$(error No llvm patch dir for LIBCXX_VERSION=$(LIBCXX_VERSION))
endif
LIBCXX_TARGET := $(LIBCXX_DIR)/lib/libc++.a

BASE_CFLAGS := --target=riscv32 \
	-Os -g -DOPENVM -mcmodel=medany \
	-fdata-sections -ffunction-sections -fvisibility=hidden

ifeq ($(VM),softfloat)
	BASE_CFLAGS += -march=rv32im -mabi=ilp32
	RUNNER_FEATURES := --no-default-features --features softfloat
else ifeq ($(VM),zfinx)
	BASE_CFLAGS += -march=rv32im_zfinx -mabi=ilp32
	RUNNER_FEATURES := --no-default-features --features zfinx
else ifeq ($(VM),f)
	BASE_CFLAGS += -march=rv32imf -mabi=ilp32f
	RUNNER_FEATURES := --no-default-features --features f
else
	$(error "Invalid VM configuration!")
endif

LDFLAGS := --gc-sections --static \
  --nostdlib --sysroot $(MUSL)/release \
  -L$(MUSL)/release/lib -L$(BUILTINS)/build \
  -lc -lgcc -lcompiler-rt \
  -L$(LIBCXX_DIR)/lib \
  -lc++ -lc++abi -lunwind

# Show all linker errors
LDFLAGS += --error-limit=0

# Jolt platform overlay: PlatformBlue.h (the JPH_PLATFORM_BLUE hook) is
# supplied out-of-tree so extern/JoltPhysics stays pristine.
JOLT_DEFINES := -DJPH_PLATFORM_BLUE
SORT_DEFINES := -D_LIBCPP_SORT_NO_BITSET_PARTITION

CXXFLAGS := \
  -Wall -Werror \
  -std=c++17 \
  -fno-exceptions \
  -D_GNU_SOURCE \
  -nostdinc -nostdinc++ \
  -isystem $(LIBCXX_DIR)/include/c++/v1 \
  -isystem $(MUSL)/release/include \
  -I overlay \
  $(BASE_CFLAGS) $(JOLT_DEFINES) $(SORT_DEFINES)
ifneq (true,$(DEBUG))
	CXXFLAGS += -DNO_DEBUG_INFO
endif

all: run-openvm

# --- M0: smoke guest -------------------------------------------------------

# Single-threaded __atomic_* stubs (RV32IM has no A extension).
# Single-threaded __atomic_* stubs (RV32IM has no A extension), the guest
# syscall layer (mmap-from-arena override of musl's weak hijack — without
# it malloc(> 112 KiB) fails on the VM, see runtime/guest_syscalls.c), and
# the guest CRT entry (stack at 400 MiB — crt1's 0x00200400 leaves < 1 MiB
# before the text segment, which Jolt's per-island solver stack arrays
# overflow).
RUNTIME_OBJS := build/guest_crt.o build/atomic_stubs.o build/guest_syscalls.o

build/guest_crt.o: runtime/guest_crt.c
	@mkdir -p build
	$(CLANG) $(BASE_CFLAGS) -nostdinc -isystem $(MUSL)/release/include -c $< -o $@

build/atomic_stubs.o: runtime/atomic_stubs.c
	@mkdir -p build
	$(CLANG) $(BASE_CFLAGS) -c $< -o $@

build/guest_syscalls.o: runtime/guest_syscalls.c
	@mkdir -p build
	$(CLANG) $(BASE_CFLAGS) -nostdinc -isystem $(MUSL)/release/include -c $< -o $@

build-openvm: $(MUSL_TARGET) $(BUILTINS_TARGET) $(LIBCXX_TARGET) $(RUNTIME_OBJS)
	$(CLANGXX) $(CXXFLAGS) demo/hello.cpp -c -o build/hello.o
	$(LD) $(LDFLAGS) build/hello.o $(RUNTIME_OBJS) -o build/hello
	$(OBJDUMP) -S build/hello --demangle > build/hello_dump.txt 2> /dev/null

# --- Bench guest: libc++ micro benchmark -----------------------------------
# BENCH_DEFS := -DBENCH_NO_OSTREAM  # measure without iostream/locale weight

bench-openvm: $(MUSL_TARGET) $(BUILTINS_TARGET) $(LIBCXX_TARGET) $(RUNTIME_OBJS)
	$(CLANGXX) $(CXXFLAGS) $(BENCH_DEFS) demo/bench.cpp -c -o build/bench.o
	$(LD) $(LDFLAGS) build/bench.o $(RUNTIME_OBJS) -o build/bench
	$(OBJDUMP) -S build/bench --demangle > build/bench_dump.txt 2> /dev/null

# Native reference build (host clang++) for cross-checking hashes.
NATIVE_CXXFLAGS := -std=c++17 -O2 -ffp-contract=off

build-native:
	@mkdir -p build
	$(CLANGXX) $(NATIVE_CXXFLAGS) demo/hello.cpp -o build/hello_native

run-native: build-native
	./build/hello_native

# Runs the guest from the project root so ./build/hello and state_openvm.bin
# resolve consistently. RUST_LOG enables the per-execution instruction count.
run-openvm: build-openvm
	cd runner && cargo build --release $(RUNNER_FEATURES)
	RUST_LOG=openvm_circuit::arch::interpreter=info ./runner/target/release/impulse-runner

# Prove + verify. Proving is RAM-heavy (~2GB per million guest cycles).
prove-openvm: build-openvm
	cd runner && cargo build --release $(RUNNER_FEATURES)
	RUST_LOG=openvm_circuit::arch::interpreter=info ./runner/target/release/impulse-runner prove

# --- M1: libJolt.a ----------------------------------------------------------
# Headless Jolt sources: everything under Jolt/ except ObjectStream,
# Renderer and the GPU Compute backends (keep CPU), plus one ObjectStream
# exception: SerializableObject.cpp defines the base-class RTTI
# (GetRTTIOfType(SerializableObject*)) that every JPH_IMPLEMENT_RTTI_* in
# the library references. List is generated mechanically — do not
# hand-maintain.
JOLT_SRC := $(shell find $(JOLT)/Jolt -name '*.cpp' \
	! -path '$(JOLT)/Jolt/ObjectStream/*' \
	! -path '$(JOLT)/Jolt/Renderer/*' \
	! -path '$(JOLT)/Jolt/Compute/VK/*' \
	! -path '$(JOLT)/Jolt/Compute/DX12/*' \
	! -path '$(JOLT)/Jolt/Compute/MTL/*') \
	$(JOLT)/Jolt/ObjectStream/SerializableObject.cpp
# Determinism: normalize FP behavior so rv32im_zfinx and x86-64 agree
# bit-for-bit (also set for the native reference build below). NDEBUG keeps
# JPH_DEBUG/JPH_ENABLE_ASSERTS off (JPH_ASSERT pulls std::this_thread state
# our threadless libc++ doesn't have).
JOLT_DETERMINISM := -DNDEBUG -DJPH_CROSS_PLATFORM_DETERMINISTIC -ffp-contract=off -fwrapv -fno-strict-aliasing

# JobSystemThreadPool/JobSystemWithBarrier are the only TUs needing a real
# std::thread/chrono runtime (unused by guests: JobSystemSingleThreaded);
# excluded from the cross build, kept in the native validation build.
JOLT_SRC_CROSS := $(filter-out \
	$(JOLT)/Jolt/Core/JobSystemThreadPool.cpp \
	$(JOLT)/Jolt/Core/JobSystemWithBarrier.cpp,$(JOLT_SRC))
JOLT_OBJ := $(patsubst $(JOLT)/Jolt/%.cpp,build/jolt/%.o,$(JOLT_SRC_CROSS))

# -O3 (not the global -Os) for the Jolt library: fewer dynamic instructions
# in the solver inner loops, which dominate guest cycles (measured -30% on
# the pyramid vs -Os; -O2 and LTO were within 1% of each other, -O3 best).
JOLT_CXXFLAGS := $(filter-out -Os,$(CXXFLAGS)) -O3 -I $(JOLT) -fno-rtti $(JOLT_DETERMINISM)
# Debug aid: JOLT_ASSERTS=true builds the library and guest with
# JPH_ENABLE_ASSERTS (the guest prints failures via AssertFailed).
ifeq (true,$(JOLT_ASSERTS))
	JOLT_CXXFLAGS := $(filter-out -DNDEBUG,$(JOLT_CXXFLAGS)) -DJPH_ENABLE_ASSERTS
endif

# --- Flag stamps ------------------------------------------------------------
# Make cannot see flag changes: stale objects with mismatched defines cause
# ODR/ABI breakage (bit us once with an assert-enabled lib vs assert-off
# guest — silently aborting native runs). Each stamp captures the flags an
# object group was built with; on change the group is wiped. Stamps only
# touch their file on change, so downstream mtimes stay stable.
# The $(eval) calls live next to the flag variables they capture.
.PHONY: FORCE
FORCE:

define FLAG_STAMP
$(1): FORCE
	@mkdir -p $(dir $(1))
	@echo '$(2)' | cmp -s - $(1) && exit 0; \
	echo "impulse: flags changed — wiping $(3)"; \
	rm -rf $(3); echo '$(2)' > $(1)
endef

STAMP_JOLT_CROSS := build/jolt.flag-stamp
$(eval $(call FLAG_STAMP,$(STAMP_JOLT_CROSS),$(JOLT_CXXFLAGS),build/jolt build/libJolt.a))

build/jolt/%.o: $(JOLT)/Jolt/%.cpp $(STAMP_JOLT_CROSS)
	@mkdir -p $(dir $@)
	$(CLANGXX) $(JOLT_CXXFLAGS) -c $< -o $@

build/libJolt.a: $(JOLT_OBJ) $(STAMP_JOLT_CROSS)
	$(AR) rcs $@ $(JOLT_OBJ)

build-openvm-jolt: $(MUSL_TARGET) $(BUILTINS_TARGET) $(LIBCXX_TARGET) $(RUNTIME_OBJS) build/libJolt.a
	$(CLANGXX) $(JOLT_CXXFLAGS) demo/jolt_hello.cpp -c -o build/jolt_hello.o
	$(LD) $(LDFLAGS) build/jolt_hello.o build/libJolt.a $(RUNTIME_OBJS) -o build/jolt_hello
	$(OBJDUMP) -S build/jolt_hello --demangle > build/jolt_hello_dump.txt 2> /dev/null

run-openvm-jolt: build-openvm-jolt
	cd runner && cargo build --release $(RUNNER_FEATURES)
	RUST_LOG=openvm_circuit::arch::interpreter=info ./runner/target/release/impulse-runner build/jolt_hello

# Native x86-64 reference build of the same source list + guest: validates
# the list before cross-building and produces the reference state hash.
# THREADS=N (N>0) builds a multithreaded flavor: JobSystemThreadPool(N),
# WITHOUT JPH_PLATFORM_BLUE (real threads need real mutexes/semaphores —
# the BLUE overlay no-ops them) and its own library build to match. Used
# to test whether Jolt's multithreaded solver converges to the
# single-threaded result (the VM is always single-threaded).
NATIVE_MT := $(if $(filter-out 0,$(THREADS)),yes,)
NATIVE_TAG := $(if $(NATIVE_MT),_mt,)
NATIVE_THREAD_LIBS := $(if $(NATIVE_MT),-pthread,)

JOLT_NATIVE_OBJ := $(patsubst $(JOLT)/Jolt/%.cpp,build/jolt_native$(NATIVE_TAG)/%.o,$(JOLT_SRC))
JOLT_NATIVE_DEFINES := $(if $(NATIVE_MT),,$(JOLT_DEFINES))

JOLT_NATIVE_CXXFLAGS := -std=c++17 -O2 -fno-exceptions -fno-rtti -fvisibility=hidden \
	-I $(JOLT) -I overlay $(JOLT_NATIVE_DEFINES) $(JOLT_DETERMINISM)

ifeq (true,$(JOLT_ASSERTS))
	JOLT_NATIVE_CXXFLAGS := $(filter-out -DNDEBUG,$(JOLT_NATIVE_CXXFLAGS)) -DJPH_ENABLE_ASSERTS
endif

ifneq (,$(NATIVE_MT))
	JOLT_NATIVE_CXXFLAGS += -DJOLT_HELLO_THREADS=$(THREADS)
endif

STAMP_JOLT_NATIVE := build/jolt_native$(NATIVE_TAG).flag-stamp
$(eval $(call FLAG_STAMP,$(STAMP_JOLT_NATIVE),$(JOLT_NATIVE_CXXFLAGS),build/jolt_native$(NATIVE_TAG) build/libJolt_native$(NATIVE_TAG).a build/jolt_hello_native$(NATIVE_TAG).o build/jolt_hello_native$(NATIVE_TAG)))

build/jolt_native$(NATIVE_TAG)/%.o: $(JOLT)/Jolt/%.cpp $(STAMP_JOLT_NATIVE)
	@mkdir -p $(dir $@)
	$(CLANGXX) $(JOLT_NATIVE_CXXFLAGS) -c $< -o $@

build/libJolt_native$(NATIVE_TAG).a: $(JOLT_NATIVE_OBJ) $(STAMP_JOLT_NATIVE)
	$(AR) rcs $@ $(JOLT_NATIVE_OBJ)

build-native-jolt: build/libJolt_native$(NATIVE_TAG).a
	$(CLANGXX) $(JOLT_NATIVE_CXXFLAGS) -c demo/jolt_hello.cpp -o build/jolt_hello_native$(NATIVE_TAG).o
	$(CLANGXX) build/jolt_hello_native$(NATIVE_TAG).o build/libJolt_native$(NATIVE_TAG).a $(NATIVE_THREAD_LIBS) -o build/jolt_hello_native$(NATIVE_TAG)

run-native-jolt: build-native-jolt
	./build/jolt_hello_native$(NATIVE_TAG)

# --- M2: Pyramid demo -------------------------------------------------------
# demo/pyramid.cpp reuses PerformanceTest/{PyramidScene.h,Layers.h}.
# PYRAMID_HEIGHT (stock 15, h(h+1)(2h+1)/6 boxes) and PYRAMID_STEPS
# (default 200) size the config — the proving-relevant knobs for M3.
# THREADS=N builds the native multithreaded flavor (see the native-jolt
# section above for the BLUE/no-BLUE split).

PYRAMID_HEIGHT ?= 15
PYRAMID_STEPS ?= 200
PYRAMID_DEFINES := -DPYRAMID_HEIGHT=$(PYRAMID_HEIGHT) -DPYRAMID_STEPS=$(PYRAMID_STEPS)
# Optional solver iteration knobs (Jolt defaults 8 velocity / 2 position):
# e.g. make run-openvm-pyramid PYRAMID_HEIGHT=4 SOLVER_VSTEPS=4 SOLVER_PSTEPS=1
ifneq (,$(SOLVER_VSTEPS))
	PYRAMID_DEFINES += -DSOLVER_VSTEPS=$(SOLVER_VSTEPS)
endif
ifneq (,$(SOLVER_PSTEPS))
	PYRAMID_DEFINES += -DSOLVER_PSTEPS=$(SOLVER_PSTEPS)
endif
# PYRAMID_ALLOW_SLEEP=1: let settled bodies sleep (stock PerformanceTest
# keeps the island awake; sleeping is deterministic and much cheaper on
# long runs). PYRAMID_NO_SPLITTER=1: skip the large-island splitter
# (parallelism bookkeeping, useless on the single-threaded VM).
ifneq (,$(PYRAMID_ALLOW_SLEEP))
	PYRAMID_DEFINES += -DPYRAMID_ALLOW_SLEEP=1
endif
ifneq (,$(PYRAMID_NO_SPLITTER))
	PYRAMID_DEFINES += -DPYRAMID_NO_SPLITTER=1
endif

JOLT_PYRAMID_CXXFLAGS := $(JOLT_NATIVE_CXXFLAGS) -I $(JOLT)/PerformanceTest $(PYRAMID_DEFINES)

STAMP_PYRAMID_VM := build/pyramid_vm.flag-stamp
STAMP_PYRAMID_NATIVE := build/pyramid_native$(NATIVE_TAG).flag-stamp
$(eval $(call FLAG_STAMP,$(STAMP_PYRAMID_VM),$(JOLT_CXXFLAGS) -I $(JOLT)/PerformanceTest $(PYRAMID_DEFINES),build/pyramid.o build/pyramid))
$(eval $(call FLAG_STAMP,$(STAMP_PYRAMID_NATIVE),$(JOLT_PYRAMID_CXXFLAGS),build/pyramid_native$(NATIVE_TAG).o build/pyramid_native$(NATIVE_TAG)))

build-native-pyramid: build/libJolt_native$(NATIVE_TAG).a $(STAMP_PYRAMID_NATIVE)
	$(CLANGXX) $(JOLT_PYRAMID_CXXFLAGS) -c demo/pyramid.cpp -o build/pyramid_native$(NATIVE_TAG).o
	$(CLANGXX) build/pyramid_native$(NATIVE_TAG).o build/libJolt_native$(NATIVE_TAG).a $(NATIVE_THREAD_LIBS) -o build/pyramid_native$(NATIVE_TAG)

run-native-pyramid: build-native-jolt build-native-pyramid
	./build/pyramid_native$(NATIVE_TAG)

build-openvm-pyramid: $(MUSL_TARGET) $(BUILTINS_TARGET) $(LIBCXX_TARGET) $(RUNTIME_OBJS) build/libJolt.a $(STAMP_PYRAMID_VM)
	$(CLANGXX) $(JOLT_CXXFLAGS) -I $(JOLT)/PerformanceTest $(PYRAMID_DEFINES) demo/pyramid.cpp -c -o build/pyramid.o
	$(LD) $(LDFLAGS) build/pyramid.o build/libJolt.a $(RUNTIME_OBJS) -o build/pyramid

run-openvm-pyramid: build-openvm-pyramid
	cd runner && cargo build --release $(RUNNER_FEATURES)
	RUST_LOG=openvm_circuit::arch::interpreter=info ./runner/target/release/impulse-runner build/pyramid

prove-openvm-pyramid: build-openvm-pyramid
	cd runner && cargo build --release $(RUNNER_FEATURES)
	./runner/target/release/impulse-runner prove build/pyramid

# --- M5: Live demo — play, verify, replay -----------------------------------
# demo/playground.cpp is a dual-mode guest:
#   native: interactive GLFW window (demo/viewer.h). Play a minute, then
#           quit: the session — initial world dump + every command (the
#           keys, resolved to quantized ops) + final state hash — is
#           recorded in build/session.bin (+ .cmds.txt/.world0.txt
#           sidecars, + _final.bin state blob).
#   openvm: ONE GENERIC VERIFIER BINARY — the session (initial world
#           dump + commands + claimed hashes) arrives on the OpenVM input
#           stream (--input); the guest re-simulates it and reveals a
#           single verdict value: 0 = simulated final hash matches the
#           claim, non-zero = rejection code.
#
#   make demo-play    fire up the window and play (keys below); q ends
#   make demo-verify  run the recording in OpenVM; prints hash MATCH/MISMATCH
#   make demo-replay  watch the recording auto-fire in the window again
#   make demo-auto    headless scripted session (no display; same recorder)
#   make demo-e2e     demo-auto + demo-verify (determinism round trip)
#   make demo-prove   tiny session + full zk proof (~30 GB RAM class)
#
# Keys: f=fire ball  b=drop box  x=shockwave  s=shock strength  g=moon/earth
#       gravity  ,/.=aim  1/2/3=ball class  p=pause  q=end session.  Mouse
#       orbits/zooms (camera only — never recorded).

DEMO_HEIGHT ?= 6
DEMO_OUT := build/session.bin

# GLFW viewer dependency (recorder/replayer window). pkg-config first
# (apt libglfw3-dev / brew glfw / vcpkg glfw3 all ship glfw3.pc), then a
# per-OS fallback.
GLFW_CFLAGS := $(shell pkg-config --cflags glfw3 2>/dev/null)
GLFW_PC_LIBS := $(shell pkg-config --libs glfw3 2>/dev/null)
ifeq ($(shell uname),Darwin)
	# glfw3.pc keeps the frameworks in Requires.private — link them ourselves
	# (duplicate -framework flags are harmless)
	GLFW_LIBS := $(if $(GLFW_PC_LIBS),$(GLFW_PC_LIBS),-lglfw) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
	# glfw3.pc keeps libGL in Requires.private — link it ourselves
	GLFW_LIBS := $(if $(GLFW_PC_LIBS),$(GLFW_PC_LIBS),-lglfw) -lGL
endif
JOLT_PG_NATIVE_CXXFLAGS := $(JOLT_NATIVE_CXXFLAGS) -I $(JOLT)/PerformanceTest

build-native-playground: build/libJolt_native.a
	@echo "impulse: building the native playground (Jolt + GLFW viewer) — one-time, ~2 min"
	$(CLANGXX) $(JOLT_PG_NATIVE_CXXFLAGS) $(GLFW_CFLAGS) demo/playground.cpp build/libJolt_native.a -o build/playground_native $(GLFW_LIBS)

demo-play: build-native-playground
	./build/playground_native --height $(DEMO_HEIGHT) --out $(DEMO_OUT)

demo-replay: build-native-playground $(DEMO_OUT)
	./build/playground_native --replay $(DEMO_OUT)

DEMO_STEPS ?= 360
demo-auto: build-native-playground
	./build/playground_native --height $(DEMO_HEIGHT) --auto $(DEMO_STEPS) --out $(DEMO_OUT) $(if $(DEMO_SCRIPT),--script "$(DEMO_SCRIPT)")

$(DEMO_OUT):
	@echo "impulse: $(DEMO_OUT) missing — record one first: make demo-play (or demo-auto)"; exit 1

# The VM guest is GENERIC: the session arrives on the OpenVM input
# stream (--input), no session data is baked into the binary.
build-openvm-playground: $(MUSL_TARGET) $(BUILTINS_TARGET) $(LIBCXX_TARGET) $(RUNTIME_OBJS) build/libJolt.a
	$(CLANGXX) $(JOLT_CXXFLAGS) -I $(JOLT)/PerformanceTest demo/playground.cpp -c -o build/playground.o
	$(LD) $(LDFLAGS) build/playground.o build/libJolt.a $(RUNTIME_OBJS) -o build/playground

# Runs the recorded session through the generic verifier guest on the VM
# (session fed via the OpenVM input stream; guest verdict must be 0).
# PROVE=1 runs execute+prove+verify in the runner (RAM-heavy; see README).
demo-verify: build-openvm-playground
	cd runner && cargo build --release $(RUNNER_FEATURES)
ifeq ($(PROVE),1)
	./runner/target/release/impulse-runner prove --input $(DEMO_OUT) build/playground
else
	RUST_LOG=openvm_circuit::arch::interpreter=info ./runner/target/release/impulse-runner --input $(DEMO_OUT) build/playground
endif

# Golden-session regression: replay the committed session natively and on
# the VM; both hashes and the full state blob must match the committed
# golden files (catches silent determinism drift — libc++ bumps, flag
# changes, Jolt updates). Session: h=4, 180 steps, all four command types.
demo-regress: build-native-playground
	cp tests/golden-session.bin build/session.bin
	./build/playground_native --replay build/session.bin --headless | tail -n 2
	@exp=$$(cat tests/golden-hash.txt); \
		if ./build/playground_native --replay build/session.bin --headless | grep -q "replayed final hash: $$exp"; then \
			echo "impulse: native golden hash MATCH ($$exp)"; \
		else echo "impulse: native golden hash MISMATCH"; exit 1; fi
	$(MAKE) build-openvm-playground
	cd runner && cargo build --release $(RUNNER_FEATURES)
	./runner/target/release/impulse-runner --input tests/golden-session.bin build/playground > build/regress-vm.log 2>&1 \
		|| { cat build/regress-vm.log; exit 1; }
	grep -q 'verdict = 0' build/regress-vm.log \
		&& echo "impulse: VM verifier verdict 0 (VALID) on the golden session" \
		|| { cat build/regress-vm.log; exit 1; }
	python3 scripts/tamper-session.py tests/golden-session.bin build/session-tampered.bin final
	if ./runner/target/release/impulse-runner --input build/session-tampered.bin build/playground > build/regress-tamper.log 2>&1; then \
		echo "impulse: TAMPER CHECK FAILED (verdict 0 on a corrupted session)"; exit 1; fi
	grep -q 'verdict = 4' build/regress-tamper.log \
		&& echo "impulse: tampered session correctly rejected (verdict 4 = BAD FINAL HASH)" \
		|| { cat build/regress-tamper.log; exit 1; }

demo-e2e:
	$(MAKE) demo-auto
	$(MAKE) demo-verify

# Session stats + proving-cost projection for the recorded session
# (parses $(DEMO_OUT), runs it once on the VM for the instruction count).
demo-stats: build-openvm-playground
	cd runner && cargo build --release $(RUNNER_FEATURES)
	@RUST_LOG=openvm_circuit::arch::interpreter=info \
		./runner/target/release/impulse-runner --input $(DEMO_OUT) build/playground 2>/dev/null \
		| grep instructions_executed | sed 's/.*instructions_executed=//' > build/session.instr
	python3 scripts/session-stats.py $(DEMO_OUT) $$(cat build/session.instr)

# Provable session: ~2M guest instructions, ~1 min / ~28 GB peak host
# RAM on the dev box. Continuation proving keeps peak memory bounded by
# segment size (~52 GB here even at 6.2M instr, 3 segments) while time
# scales ~57 s per 1M instructions — check free -g.
demo-prove:
	$(MAKE) demo-auto DEMO_HEIGHT=2 DEMO_STEPS=3 DEMO_SCRIPT="F:1:0:1"
	$(MAKE) demo-verify PROVE=1
	# standalone third-party check of the persisted artifact (no ELF, no
	# execution): the proof's verdict value must be 0
	./runner/target/release/impulse-runner verify build/playground.proof

# --- Dependencies ----------------------------------------------------------

MUSL_CFLAGS := $(BASE_CFLAGS) -DPAGE_SIZE=4096

$(MUSL_TARGET):
	cd $(MUSL) && \
		CLANG=$(CLANG) \
			BASE_CFLAGS="$(MUSL_CFLAGS)" \
			./ckb/build.sh

BUILTINS_CFLAGS := $(BASE_CFLAGS)
BUILTINS_CFLAGS += -nostdinc -I $(MUSL)/release/include -I $(LIBCXX_DIR)/include
BUILTINS_CFLAGS += -fno-builtin -fomit-frame-pointer
BUILTINS_CFLAGS += -I compiler-rt/lib/builtins
BUILTINS_CFLAGS += -DVISIBILITY_HIDDEN -DCOMPILER_RT_HAS_FLOAT16

$(BUILTINS_TARGET): $(MUSL_TARGET) $(LIBCXX_TARGET)
	cd $(BUILTINS) && \
		make CC=$(CLANG) \
			AR=$(AR) \
			CFLAGS="$(BUILTINS_CFLAGS)"

LLVM_CMAKE_OPTIONS := -DCMAKE_BUILD_TYPE=MinSizeRel
LLVM_CMAKE_OPTIONS += -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF
LLVM_CMAKE_OPTIONS += -DLIBCXXABI_SILENT_TERMINATE=ON

$(LIBCXX_TARGET): $(MUSL_TARGET)
	@if [ -d $(LIBCXX)/llvm_src ] && [ "$$(cat $(LIBCXX)/llvm_src/.impulse_version 2>/dev/null)" != "$(LIBCXX_VERSION)" ]; then \
		echo "Clearing stale llvm_src (need LLVM $(LIBCXX_VERSION))"; \
		rm -rf $(LIBCXX)/llvm_src $(LIBCXX)/build; \
	fi
	cd $(LIBCXX) && \
		CLANG=$(CLANG) \
			BASE_CFLAGS="$(BASE_CFLAGS) $(SORT_DEFINES)" \
			MUSL=$(MUSL)/release \
			INSTALL_DIR=$(abspath $(LIBCXX_DIR)) \
			DEBUG=true \
			LLVM_VERSION="$(LIBCXX_VERSION)" \
			LLVM_PATCH="$(LIBCXX_PATCH)" \
			LLVM_CMAKE_OPTIONS="$(LLVM_CMAKE_OPTIONS)" \
		  ./build.sh
	mkdir -p $(LIBCXX)/llvm_src
	echo $(LIBCXX_VERSION) > $(LIBCXX)/llvm_src/.impulse_version
	touch $@

clean:
	rm -rf build/*
	cd $(MUSL) && make clean && rm -rf release
	cd $(BUILTINS) && make clean
	cd $(LIBCXX) && rm -rf release release-$(LIBCXX_VERSION) build llvm_src *.tar.gz

.PHONY: all build-openvm bench-openvm build-native run-native run-openvm prove-openvm \
	build-openvm-jolt run-openvm-jolt build-native-jolt run-native-jolt \
	build-native-pyramid run-native-pyramid build-openvm-pyramid run-openvm-pyramid prove-openvm-pyramid \
	build-native-playground demo-play demo-replay demo-auto demo-verify demo-e2e demo-stats demo-prove demo-regress \
	build-openvm-playground clean
