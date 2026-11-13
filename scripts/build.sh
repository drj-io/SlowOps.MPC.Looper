#!/usr/bin/env bash
# Build the MPC plugin folder (native + arm32 + arm64 + skins + validation) and the test tools.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/third_party/mpc-plugin-builder-sdk"
PY="${PYTHON:-python3}"
cd "$ROOT"
[ -f "$SDK/tools/build.py" ] || git submodule update --init third_party/mpc-plugin-builder-sdk
"$PY" "$SDK/tools/build.py" plugin "$@"
mkdir -p build
c++ -std=c++17 -O2 -Wall -Wextra -I core/include tests/core_tests.cpp -o build/core_tests
c++ -std=c++17 -O2 -Wall -I "$SDK/framework" tests/scenario_host.cpp -o build/scenario_host
echo
echo "MPC folder: $ROOT/plugin/build/Slow Ops - VST - Looper"
