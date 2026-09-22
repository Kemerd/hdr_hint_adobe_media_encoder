#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh - configure, build and test HDR Hint on macOS (build.ps1's twin).
#
#   scripts/build.sh                    Release build + tests
#   scripts/build.sh --debug            Debug build in build-debug/
#   scripts/build.sh --universal        arm64 + x86_64 binaries
#   scripts/build.sh --target HdrHint --no-test
#   scripts/build.sh --clean            wipe the build folder first
#
# Uses Ninja when it is installed (brew install ninja), Unix Makefiles
# otherwise. Needs Xcode or the Command Line Tools and CMake 3.25+.
# ---------------------------------------------------------------------------
set -euo pipefail

config="Release"
target=""
run_tests=1
clean=0
universal="OFF"

usage() {
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
}

# ---- arguments -----------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) config="Debug" ;;
        --config) config="${2:?--config needs a value}"; shift ;;
        --target) target="${2:?--target needs a value}"; shift ;;
        --no-test) run_tests=0 ;;
        --clean) clean=1 ;;
        --universal) universal="ON" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build"
[[ "$config" == "Debug" ]] && build="$root/build-debug"

# ---- tools ------------------------------------------------------------------------
if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake not found. Install it with: brew install cmake" >&2
    exit 1
fi
# (bash 3.2, the macOS default, treats "${empty[@]}" as unbound under set -u:
# the ${a[@]+...} spelling below keeps empty arrays harmless.)
generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi

if [[ $clean -eq 1 && -d "$build" ]]; then
    rm -rf "$build"
fi

# ---- configure once; CMake reuses the cache afterwards ----------------------------
if [[ ! -f "$build/CMakeCache.txt" ]]; then
    cmake -S "$root" -B "$build" ${generator[@]+"${generator[@]}"} -DCMAKE_BUILD_TYPE="$config" -DHH_UNIVERSAL="$universal"
fi

# ---- build ------------------------------------------------------------------------------
target_args=()
[[ -n "$target" ]] && target_args=(--target "$target")
cmake --build "$build" --config "$config" --parallel ${target_args[@]+"${target_args[@]}"}

# ---- test -------------------------------------------------------------------------------
if [[ $run_tests -eq 1 && ( -z "$target" || "$target" == "hdrhint_tests" ) ]]; then
    ctest --test-dir "$build" -C "$config" --output-on-failure
fi

if [[ -d "$build/HdrHint.app" ]]; then
    echo
    echo "Built $build/HdrHint.app"
fi
