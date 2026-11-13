#!/usr/bin/env sh
# Builds CrossTerm.app on macOS (Release), bundles Qt into it and runs the tests.
#
# Dependencies:
#   xcode-select --install
#   brew install cmake qt          (or the Qt online installer: ~/Qt/6.x/macos)
# Optional at runtime: XQuartz (X11 forwarding), Microsoft Remote Desktop / Windows App (RDP).
#
# Usage: scripts/build-macos.sh [build-dir]    (Qt elsewhere: export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/macos)
set -eu
cd "$(dirname "$0")/.."
BUILD="${1:-build}"

# Qt 6: CMAKE_PREFIX_PATH, then Homebrew (only if really installed), then the Qt online installer.
has_qt() { [ -n "$1" ] && [ -f "$1/lib/cmake/Qt6/Qt6Config.cmake" ]; }
QT_PREFIX="${CMAKE_PREFIX_PATH:-}"
if ! has_qt "$QT_PREFIX" && command -v brew >/dev/null 2>&1 && brew list qt >/dev/null 2>&1; then
    QT_PREFIX="$(brew --prefix qt)"
fi
if ! has_qt "$QT_PREFIX"; then
    for candidate in "$HOME"/Qt/6.*/macos; do
        has_qt "$candidate" && QT_PREFIX="$candidate"
    done
fi
if ! has_qt "$QT_PREFIX"; then
    echo "Qt 6 not found. Install it with:  brew install qt" >&2
    echo "or, for another installation:     export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/macos" >&2
    exit 1
fi
echo "Using Qt: $QT_PREFIX"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX"
cmake --build "$BUILD" --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure || echo "Some tests failed (see above)."
"$QT_PREFIX/bin/macdeployqt" "$BUILD/CrossTerm.app"
echo "Built: $BUILD/CrossTerm.app"
