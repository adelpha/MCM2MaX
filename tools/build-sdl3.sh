#!/bin/sh
# Builds a static SDL3 for 32-bit Windows with MinGW, with only the parts
# MCM2MaX uses for controllers: joystick/gamepad, haptic (rumble on
# DirectInput devices) and HIDAPI (PlayStation / Switch pads). Audio, video,
# rendering, GPU, camera, sensors, power, dialogs and tray are left out.
#
#   tools/build-sdl3.sh <work dir>
#
# The result is installed in <work dir>/install; pass that to CMake as
# -DMCM2MaX_SDL3=<work dir>/install. SDL is zlib-licensed. The MinGW build
# imports nothing Windows XP lacks (tools/check-xp-imports.py).
set -eu
WORK=$(mkdir -p "$1" && cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=release-3.4.8
if [ ! -d "$WORK/SDL" ]; then
  git clone -q --depth 1 --branch "$VERSION" https://github.com/libsdl-org/SDL.git "$WORK/SDL"
fi
rm -rf "$WORK/build"
cmake -S "$WORK/SDL" -B "$WORK/build" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/code/toolchain-mingw32.cmake" \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$WORK/install" \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_AUDIO=OFF -DSDL_VIDEO=OFF -DSDL_GPU=OFF -DSDL_RENDER=OFF -DSDL_CAMERA=OFF \
  -DSDL_POWER=OFF -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF \
  -DSDL_JOYSTICK=ON -DSDL_HAPTIC=ON -DSDL_HIDAPI=ON
cmake --build "$WORK/build" -j"$(nproc)"
cmake --install "$WORK/build"
