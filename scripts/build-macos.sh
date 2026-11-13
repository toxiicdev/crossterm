#!/usr/bin/env sh
# Builds CrossTerm.app on macOS (Release), bundles Qt into it and runs the tests.
#
# Dependencies:
#   xcode-select --install
#   brew install cmake qt
# Optional at runtime: XQuartz (X11 forwarding), Microsoft Remote Desktop / Windows App (RDP).
#
# Usage: scripts/build-macos.sh [build-dir]    (Qt elsewhere: export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/macos)
set -eu
cd "$(dirname "$0")/.."
BUILD="${1:-build}"
QT_PREFIX="${CMAKE_PREFIX_PATH:-$(brew --prefix qt 2>/dev/null || true)}"
if [ -z "$QT_PREFIX" ]; then
    echo "Qt 6 not found: brew install qt, or export CMAKE_PREFIX_PATH." >&2
    exit 1
fi
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX"
cmake --build "$BUILD" --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure || echo "Some tests failed (see above)."
"$QT_PREFIX/bin/macdeployqt" "$BUILD/CrossTerm.app"
echo "Built: $BUILD/CrossTerm.app"
