#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-build-macos}"
configuration="${CONFIGURATION:-Debug}"
generator="${GENERATOR:-Auto}"
parallel="${PARALLEL:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
architecture="${ARCHITECTURE:-arm64}"
fresh="${FRESH:-0}"
universal="${UNIVERSAL:-0}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        --configuration)
            configuration="$2"
            shift 2
            ;;
        --generator)
            generator="$2"
            shift 2
            ;;
        --parallel)
            parallel="$2"
            shift 2
            ;;
        --architecture|--architectures)
            architecture="$2"
            shift 2
            ;;
        --universal)
            architecture="arm64;x86_64"
            universal="1"
            shift
            ;;
        --fresh)
            fresh="1"
            shift
            ;;
        -h|--help)
            cat <<'EOF'
Usage: scripts/build-macos.sh [options]

Options:
  --build-dir DIR              Build directory. Default: build-macos
  --configuration NAME         CMake build type. Default: Debug
  --generator NAME             Auto, Ninja, or Unix Makefiles. Default: Auto
  --parallel N                 Parallel build jobs. Default: CPU count
  --architecture ARCHS         CMAKE_OSX_ARCHITECTURES value. Default: arm64
  --universal                  Build arm64;x86_64
  --fresh                      Reconfigure from a clean CMake cache
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "scripts/build-macos.sh must be run on macOS." >&2
    exit 1
fi

command -v xcrun >/dev/null 2>&1 || {
    echo "Xcode Command Line Tools are required. Install them with: xcode-select --install" >&2
    exit 1
}

command -v cmake >/dev/null 2>&1 || {
    echo "CMake is required. Install it from cmake.org or with Homebrew." >&2
    exit 1
}

selected_generator="$generator"
if [[ "$selected_generator" == "Auto" ]]; then
    if command -v ninja >/dev/null 2>&1; then
        selected_generator="Ninja"
    else
        selected_generator="Unix Makefiles"
    fi
fi

generator_available() {
    case "$1" in
        Ninja)
            command -v ninja >/dev/null 2>&1
            ;;
        "Unix Makefiles")
            command -v make >/dev/null 2>&1
            ;;
        *)
            return 0
            ;;
    esac
}

cache_file="$repo_root/$build_dir/CMakeCache.txt"
if [[ -f "$cache_file" ]]; then
    existing_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache_file" | head -n 1)"
    if [[ -n "$existing_generator" && "$existing_generator" != "$selected_generator" ]]; then
        if [[ "$fresh" == "1" ]]; then
            echo "Fresh reconfigure requested: switching $build_dir from '$existing_generator' to '$selected_generator'."
        elif ! generator_available "$existing_generator"; then
            echo "Build directory '$build_dir' uses unavailable generator '$existing_generator'; reconfiguring with '$selected_generator'."
            fresh="1"
        else
            echo "Build directory '$build_dir' already uses '$existing_generator'; pass --fresh to switch generators." >&2
            selected_generator="$existing_generator"
        fi
    fi
fi

fresh_args=()
if [[ "$fresh" == "1" ]]; then
    if cmake --help | grep -q -- '--fresh'; then
        fresh_args+=(--fresh)
    else
        cmake -E rm -rf "$repo_root/$build_dir/CMakeCache.txt" "$repo_root/$build_dir/CMakeFiles"
    fi
fi

echo "Repository: $repo_root"
echo "macOS build dir: $build_dir"
echo "Build type: $configuration"
echo "CMake generator: $selected_generator"
echo "Architectures: $architecture"

if [[ ${#fresh_args[@]} -gt 0 ]]; then
    cmake "${fresh_args[@]}" -S "$repo_root" -B "$repo_root/$build_dir" -G "$selected_generator" \
        -DCMAKE_BUILD_TYPE="$configuration" \
        -DCMAKE_OSX_ARCHITECTURES="$architecture"
else
    cmake -S "$repo_root" -B "$repo_root/$build_dir" -G "$selected_generator" \
        -DCMAKE_BUILD_TYPE="$configuration" \
        -DCMAKE_OSX_ARCHITECTURES="$architecture"
fi

cmake --build "$repo_root/$build_dir" --config "$configuration" --parallel "$parallel"
