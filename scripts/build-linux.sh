#!/usr/bin/env sh
# Builds CrossTerm on Linux (Release) and runs the tests.
#
# Dependencies (Debian/Ubuntu 24.04+):
#   sudo apt install build-essential cmake qt6-base-dev libgl1-mesa-dev openssh-client
# Fedora:
#   sudo dnf install gcc-c++ cmake qt6-qtbase-devel openssh-clients
# Optional at runtime: xfreerdp (embedded RDP), libsecret / a Secret Service provider (password vault).
#
# Usage: scripts/build-linux.sh [build-dir]    (Qt elsewhere: export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64)
set -eu
cd "$(dirname "$0")/.."
BUILD="${1:-build}"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure || echo "Some tests failed (see above)."
echo "Built: $BUILD/CrossTerm"
