#!/usr/bin/env bash
# Full verification: build, core unit tests, SDK host simulator (stopped + playing), scenario host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/third_party/mpc-plugin-builder-sdk"
LIB="$ROOT/plugin/build/native/slow-ops-looper.dylib"
[ "$(uname)" = "Darwin" ] || LIB="${LIB%.dylib}.so"
cd "$ROOT"
"$ROOT/scripts/build.sh"
echo; echo "== core engine tests"; ./build/core_tests
echo; echo "== SDK host simulator (transport stopped)"
"$SDK/.sdk-tools/host_sim" "$LIB" test --seconds 8 --wav plugin/build/hostsim.wav
echo; echo "== SDK host simulator (transport playing)"
"$SDK/.sdk-tools/host_sim" "$LIB" test --seconds 8 --playing --wav plugin/build/hostsim-playing.wav
echo; echo "== scenario host (scripted transport + button presses, real plugin binary)"
./build/scenario_host "$LIB" plugin/build
echo; echo "ALL CHECKS PASSED"
