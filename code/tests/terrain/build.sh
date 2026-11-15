#!/bin/sh
# Builds harness.dll (i686, no imports, base 0x20000000) for terrain_harness.py.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-$(command -v i686-w64-mingw32-g++-posix || echo i686-w64-mingw32-g++)}
$CXX -O2 -msse2 -mfpmath=sse -mstackrealign -ffp-contract=off -fno-strict-aliasing -fno-exceptions -fno-rtti \
  -fno-asynchronous-unwind-tables -std=c++17 -Wall -Wextra \
  -shared -nostdlib -Wl,--image-base,0x20000000 -Wl,--entry,_HEntry@12 -Wl,--disable-dynamicbase \
  -o harness.dll harness_dll.cpp ../../src/terrain_fast.cpp -lgcc
i686-w64-mingw32-objdump -p harness.dll | grep -i "DLL Name" && { echo "unexpected imports"; exit 1; } || true
