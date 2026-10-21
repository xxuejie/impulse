#!/usr/bin/env bash
# impulse: three-leg determinism verification matrix.
#
# For each config: build+run native single-threaded (BLUE), native
# JobSystemThreadPool(8), and VM single-threaded zfinx; all three state
# hashes must agree. Run before committing changes to flags or guests.
#
# Usage:
#   scripts/verify-matrix.sh                # quick set (h=4, two configs)
#   scripts/verify-matrix.sh full           # adds h=8 and h=15
#   scripts/verify-matrix.sh custom "PYRAMID_HEIGHT=6 PYRAMID_STEPS=100"

set -u
cd "$(dirname "$0")/.."

VM_RUNNER=./runner/target/release/impulse-runner
MT_THREADS=8
FAIL=0

hash_of() { grep -oE '[0-9a-f]{64}' | head -1; }

run_config() {
	local label="$1"; shift
	local knobs="$*"

	rm -rf build/jolt_native build/jolt_native_mt build/pyramid_native*.o build/pyramid_native_mt
	rm -f build/pyramid.o build/pyramid

	make -j12 build-native-pyramid $knobs > /dev/null 2>&1 \
		|| { echo "FAIL $label: native build"; FAIL=1; return; }
	local h1
	h1=$(./build/pyramid_native | hash_of)

	make -j12 build-native-pyramid $knobs THREADS=$MT_THREADS > /dev/null 2>&1 \
		|| { echo "FAIL $label: native-mt build"; FAIL=1; return; }
	local h2
	h2=$(./build/pyramid_native_mt | hash_of)

	make -j12 build-openvm-pyramid $knobs > /dev/null 2>&1 \
		|| { echo "FAIL $label: vm build"; FAIL=1; return; }
	local h3
	h3=$($VM_RUNNER build/pyramid 2>/dev/null | hash_of)

	if [ -n "$h1" ] && [ "$h1" = "$h2" ] && [ "$h2" = "$h3" ]; then
		echo "PASS $label: $h1"
	else
		echo "FAIL $label:"
		echo "      native-single: $h1"
		echo "      native-mt($MT_THREADS): $h2"
		echo "      vm:            $h3"
		FAIL=1
	fi
}

# Playground golden session: native replay + VM replay must both reproduce
# the recording (hash + blob). Reuses the demo-regress target.
run_playground() {
	local out
	out=$(make demo-regress 2>&1)
	if echo "$out" | grep -q 'replay reproduces the recorded session' \
		&& echo "$out" | grep -q 'golden hash MATCH' \
		&& echo "$out" | grep -q 'golden state blob byte-identical'; then
		echo "PASS playground golden (native + vm reproduce the recording)"
	else
		echo "FAIL playground golden:"
		echo "$out" | grep -E 'MISMATCH|FAIL|hash' | sed 's/^/      /'
		FAIL=1
	fi
}

echo "== impulse verification matrix =="
run_config "h=4 stock"                "PYRAMID_HEIGHT=4"
run_config "h=4 sleep+nosplit"        "PYRAMID_HEIGHT=4 PYRAMID_ALLOW_SLEEP=1 PYRAMID_NO_SPLITTER=1"
run_config "h=4 v4/p1"                "PYRAMID_HEIGHT=4 SOLVER_VSTEPS=4 SOLVER_PSTEPS=1"
run_playground

if [ "${1:-}" = "full" ]; then
	run_config "h=8 stock"            "PYRAMID_HEIGHT=8"
	run_config "h=8 sleep+nosplit"    "PYRAMID_HEIGHT=8 PYRAMID_ALLOW_SLEEP=1 PYRAMID_NO_SPLITTER=1"
	run_config "h=15 stock"           "PYRAMID_HEIGHT=15"
	run_config "h=15 sleep+nosplit"   "PYRAMID_HEIGHT=15 PYRAMID_ALLOW_SLEEP=1 PYRAMID_NO_SPLITTER=1"
elif [ "${1:-}" = "custom" ] && [ -n "${2:-}" ]; then
	run_config "custom ($2)" $2
fi

if [ "$FAIL" -eq 0 ]; then
	echo "== all configs PASS =="
else
	echo "== FAILURES above =="
fi
exit $FAIL
