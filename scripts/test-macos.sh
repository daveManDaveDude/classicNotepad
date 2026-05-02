#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-build-macos}"
configuration="${CONFIGURATION:-Debug}"

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
        -h|--help)
            cat <<'EOF'
Usage: scripts/test-macos.sh [options]

Options:
  --build-dir DIR              Build directory. Default: build-macos
  --configuration NAME         CMake configuration. Default: Debug
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
    echo "scripts/test-macos.sh must be run on macOS." >&2
    exit 1
fi

command -v ctest >/dev/null 2>&1 || {
    echo "CTest is required. Install CMake and make sure ctest is on PATH." >&2
    exit 1
}

echo "Repository: $repo_root"
echo "macOS build dir: $build_dir"
echo "Build type: $configuration"

ctest --test-dir "$repo_root/$build_dir" -C "$configuration" --output-on-failure
