#!/usr/bin/env sh
# Builds CrossTerm.app on macOS (Release), bundles Qt into it and runs the tests.
# Missing dependencies are installed automatically: Xcode Command Line Tools, then cmake and qt with Homebrew.
# Optional at runtime: XQuartz (X11 forwarding), Microsoft Remote Desktop / Windows App (RDP).
#
# Usage: scripts/build-macos.sh [build-dir]    (Qt elsewhere: export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/macos)
set -eu
cd "$(dirname "$0")/.."
BUILD="${1:-build}"

# --- Dependencies
if ! xcode-select -p >/dev/null 2>&1; then
    echo "Installing the Xcode Command Line Tools: complete the dialog, then run this script again."
    xcode-select --install || true
    exit 1
fi
has_qt() { [ -n "$1" ] && [ -f "$1/lib/cmake/Qt6/Qt6Config.cmake" ]; }
QT_PREFIX="${CMAKE_PREFIX_PATH:-}"
if ! has_qt "$QT_PREFIX"; then
    for candidate in "$HOME"/Qt/6.*/macos; do
        has_qt "$candidate" && QT_PREFIX="$candidate"
    done
fi
missing=""
command -v cmake >/dev/null 2>&1 || missing="$missing cmake"
if ! has_qt "$QT_PREFIX"; then
    if command -v brew >/dev/null 2>&1 && brew list qt >/dev/null 2>&1; then
        QT_PREFIX="$(brew --prefix qt)"
    else
        missing="$missing qt"
    fi
fi
if [ -n "$missing" ]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is needed to install:$missing" >&2
        echo "Install it from https://brew.sh, then run this script again." >&2
        exit 1
    fi
    echo "Installing with Homebrew:$missing"
    # shellcheck disable=SC2086
    brew install $missing
    has_qt "$QT_PREFIX" || QT_PREFIX="$(brew --prefix qt)"
fi
if ! has_qt "$QT_PREFIX"; then
    echo "Qt 6 not found after installation; set CMAKE_PREFIX_PATH=/path/to/Qt/6.x/macos" >&2
    exit 1
fi
echo "Using Qt: $QT_PREFIX"

# --- Build, tests, bundle
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX"
cmake --build "$BUILD" --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure || echo "Some tests failed (see above)."
"$QT_PREFIX/bin/macdeployqt" "$BUILD/CrossTerm.app"
echo "Built: $BUILD/CrossTerm.app"
