#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-build-macos-universal}"
configuration="${CONFIGURATION:-Release}"
architecture="${ARCHITECTURE:-arm64;x86_64}"
artifact_dir="${ARTIFACT_DIR:-artifacts/macos}"
fresh="${FRESH:-0}"
skip_tests="${SKIP_TESTS:-0}"
skip_automation="${SKIP_AUTOMATION:-0}"

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
        --architecture|--architectures)
            architecture="$2"
            shift 2
            ;;
        --artifact-dir)
            artifact_dir="$2"
            shift 2
            ;;
        --fresh)
            fresh="1"
            shift
            ;;
        --skip-tests)
            skip_tests="1"
            shift
            ;;
        --skip-automation)
            skip_automation="1"
            shift
            ;;
        -h|--help)
            cat <<'EOF'
Usage: scripts/package-macos.sh [options]

Builds, ad-hoc signs, verifies, and zips ClassicNotepadMac.app.

Options:
  --build-dir DIR              Build directory. Default: build-macos-universal
  --configuration NAME         CMake build type. Default: Release
  --architecture ARCHS         CMAKE_OSX_ARCHITECTURES value. Default: arm64;x86_64
  --artifact-dir DIR           Output directory. Default: artifacts/macos
  --fresh                      Reconfigure from a clean CMake cache
  --skip-tests                 Skip CTest
  --skip-automation            Skip shared JSON-lines automation suite
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
    echo "scripts/package-macos.sh must be run on macOS." >&2
    exit 1
fi

command -v codesign >/dev/null 2>&1 || {
    echo "codesign is required and should be provided by Xcode Command Line Tools." >&2
    exit 1
}

build_args=(
    --build-dir "$build_dir"
    --configuration "$configuration"
    --architecture "$architecture"
)
if [[ "$fresh" == "1" ]]; then
    build_args+=(--fresh)
fi

"$repo_root/scripts/build-macos.sh" "${build_args[@]}"

if [[ "$skip_tests" != "1" ]]; then
    "$repo_root/scripts/test-macos.sh" --build-dir "$build_dir" --configuration "$configuration"
fi

app_path="$repo_root/$build_dir/ClassicNotepadMac.app"
binary_path="$app_path/Contents/MacOS/ClassicNotepadMac"

if [[ "$architecture" == *";"* || "$architecture" == *"x86_64"* ]]; then
    archs="$(lipo -archs "$binary_path")"
    echo "Binary architectures: $archs"
    for expected in arm64 x86_64; do
        if [[ " $archs " != *" $expected "* ]]; then
            echo "Expected architecture '$expected' was not found in $binary_path." >&2
            exit 1
        fi
    done
fi

codesign --force --deep --sign - "$app_path"
codesign --verify --deep --strict --verbose=2 "$app_path"

if [[ "$skip_automation" != "1" ]]; then
    python3 "$repo_root/tests/automation/run_automation_tests.py" \
        --binary "$binary_path" \
        --platform macos
fi

mkdir -p "$repo_root/$artifact_dir"
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
zip_path="$repo_root/$artifact_dir/ClassicNotepadMac-${version}-${configuration}-universal.zip"
rm -f "$zip_path"
ditto -c -k --keepParent "$app_path" "$zip_path"

echo "Packaged: $zip_path"
