#!/usr/bin/env sh
# Builds CrossTerm on Linux (Release) and runs the tests.
#
# Dependencies (Debian/Ubuntu 24.04+):
#   sudo apt install build-essential cmake qt6-base-dev libgl1-mesa-dev openssh-client
# Fedora:
#   sudo dnf install gcc-c++ cmake qt6-qtbase-devel openssh-clients
# Arch:
#   sudo pacman -S base-devel cmake qt6-base openssh
# Optional at runtime: xfreerdp (embedded RDP), a Secret Service provider such as gnome-keyring (password vault).
#
# Usage: scripts/build-linux.sh [build-dir]    (Qt elsewhere: export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64)
set -eu
cd "$(dirname "$0")/.."
BUILD="${1:-build}"

# Folder containing Qt6Config.cmake: CMAKE_PREFIX_PATH, qmake6 from the distribution, the Qt online installer
# (~/Qt/6.x/gcc_64) or the usual system locations.
QT6_DIR=""
for candidate in ${CMAKE_PREFIX_PATH:+"$CMAKE_PREFIX_PATH/lib/cmake/Qt6"} \
    "$(command -v qmake6 >/dev/null 2>&1 && qmake6 -query QT_INSTALL_LIBS)/cmake/Qt6" \
    "$HOME"/Qt/6.*/gcc_64/lib/cmake/Qt6 /usr/lib/*-linux-gnu/cmake/Qt6 /usr/lib64/cmake/Qt6 /usr/lib/cmake/Qt6; do
    if [ -z "$QT6_DIR" ] && [ -f "$candidate/Qt6Config.cmake" ]; then
        QT6_DIR="$candidate"
    fi
done
if [ -z "$QT6_DIR" ]; then
    echo "Qt 6 not found. Install the development package, e.g.:" >&2
    echo "  Debian/Ubuntu: sudo apt install qt6-base-dev     Fedora: sudo dnf install qt6-qtbase-devel" >&2
    echo "or point to another installation: export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64" >&2
    exit 1
fi
echo "Using Qt: $QT6_DIR"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DQt6_DIR="$QT6_DIR"
cmake --build "$BUILD" --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure || echo "Some tests failed (see above)."
echo "Built: $BUILD/CrossTerm"
