#!/usr/bin/env sh
# Builds CrossTerm on Linux (Release) and runs the tests.
# Missing dependencies (compiler, cmake, Qt 6 development files, OpenGL headers, ssh client) are installed
# automatically with apt, dnf, pacman or zypper (sudo is asked for once).
# Optional at runtime: xfreerdp (embedded RDP), a Secret Service provider such as gnome-keyring (password vault).
#
# Usage: scripts/build-linux.sh [build-dir]    (Qt elsewhere: export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64)
set -eu
cd "$(dirname "$0")/.."
BUILD="${1:-build}"

# Folder containing Qt6Config.cmake: CMAKE_PREFIX_PATH, qmake6 from the distribution, the Qt online installer
# (~/Qt/6.x/gcc_64) or the usual system locations.
find_qt() {
    QT6_DIR=""
    for candidate in ${CMAKE_PREFIX_PATH:+"$CMAKE_PREFIX_PATH/lib/cmake/Qt6"} \
        "$(command -v qmake6 >/dev/null 2>&1 && qmake6 -query QT_INSTALL_LIBS)/cmake/Qt6" \
        "$HOME"/Qt/6.*/gcc_64/lib/cmake/Qt6 /usr/lib/*-linux-gnu/cmake/Qt6 /usr/lib64/cmake/Qt6 /usr/lib/cmake/Qt6; do
        if [ -z "$QT6_DIR" ] && [ -f "$candidate/Qt6Config.cmake" ]; then
            QT6_DIR="$candidate"
        fi
    done
}

# --- Dependencies: installed only if something is missing.
find_qt
if [ -z "$QT6_DIR" ] || ! command -v cmake >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1 ||
    ! command -v ssh >/dev/null 2>&1; then
    SUDO=""
    [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    if command -v apt-get >/dev/null 2>&1; then
        $SUDO apt-get update
        $SUDO apt-get install -y build-essential cmake qt6-base-dev libgl1-mesa-dev openssh-client
    elif command -v dnf >/dev/null 2>&1; then
        $SUDO dnf install -y gcc-c++ make cmake qt6-qtbase-devel mesa-libGL-devel openssh-clients
    elif command -v pacman >/dev/null 2>&1; then
        $SUDO pacman -S --needed --noconfirm base-devel cmake qt6-base openssh
    elif command -v zypper >/dev/null 2>&1; then
        $SUDO zypper install -y gcc-c++ make cmake qt6-base-devel Mesa-libGL-devel openssh-clients
    else
        echo "Unknown package manager: install a C++17 compiler, cmake, Qt 6 (base, development files) and an ssh client." >&2
        exit 1
    fi
    find_qt
fi
if [ -z "$QT6_DIR" ]; then
    echo "Qt 6 not found after installation; set CMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64" >&2
    exit 1
fi
echo "Using Qt: $QT6_DIR"

# --- Build and tests
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DQt6_DIR="$QT6_DIR"
cmake --build "$BUILD" --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure || echo "Some tests failed (see above)."
echo "Built: $BUILD/CrossTerm"
