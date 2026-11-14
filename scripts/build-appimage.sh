#!/usr/bin/env sh
# Builds CrossTerm on Linux with build-linux.sh, then packs it with Qt into a single-file AppImage (x86_64)
# using linuxdeploy and linuxdeploy-plugin-qt (downloaded once into <build-dir>/appimage-tools, needs curl).
# The AppImage runs on distributions whose glibc is at least as new as the build machine's: for the widest reach
# build on an older one (e.g. Ubuntu 22.04) with Qt 6.5+ from the Qt online installer or aqtinstall, passed as
# CMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64.
# Not bundled, expected on the target (present on desktop installs): libOpenGL/libEGL (libopengl0, libegl1), X11 or
# XWayland (only the xcb platform plugin is bundled) and an ssh client.
#
# Usage: scripts/build-appimage.sh [build-dir]    (QMAKE=/path/to/qmake when it is not found automatically)
set -eu
cd "$(dirname "$0")/.."
BUILD="${1:-build}"
sh scripts/build-linux.sh "$BUILD"

if [ -z "${QMAKE:-}" ]; then
    if [ -n "${CMAKE_PREFIX_PATH:-}" ] && [ -x "$CMAKE_PREFIX_PATH/bin/qmake" ]; then
        QMAKE="$CMAKE_PREFIX_PATH/bin/qmake"
    else
        QMAKE="$(command -v qmake6 || command -v qmake)"
    fi
fi
export QMAKE

TOOLS="$BUILD/appimage-tools"
mkdir -p "$TOOLS"
for tool in linuxdeploy/linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt/linuxdeploy-plugin-qt-x86_64.AppImage; do
    file="$TOOLS/${tool#*/}"
    if [ ! -f "$file" ]; then
        curl -fL -o "$file" "https://github.com/linuxdeploy/${tool%%/*}/releases/download/continuous/${tool#*/}"
        chmod +x "$file"
    fi
done

# No FUSE needed (containers, CI); Qt SVG for the icon engine; version in the file name.
export APPIMAGE_EXTRACT_AND_RUN=1 EXTRA_QT_MODULES=svg
LINUXDEPLOY_OUTPUT_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt")"
export LINUXDEPLOY_OUTPUT_VERSION
rm -rf "$BUILD/AppDir"
# linuxdeploy accepts icons up to 512x512, named like the desktop file's Icon= entry.
cp assets/crossterm-512.png "$TOOLS/crossterm.png"
ROOT="$(pwd)"
cd "$BUILD"
"./appimage-tools/linuxdeploy-x86_64.AppImage" --appdir AppDir --executable CrossTerm \
    --desktop-file "$ROOT/assets/crossterm.desktop" --icon-file appimage-tools/crossterm.png \
    --plugin qt --output appimage
echo "AppImage: $BUILD/CrossTerm-$LINUXDEPLOY_OUTPUT_VERSION-x86_64.AppImage"
