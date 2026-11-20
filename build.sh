#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds onair-rx: OnAir headless (no GUI, no tests, no whisper) in _onair-build, then src/*.cpp linked against its libraries
# with onair-server's own flags and libraries. Needs the FFmpeg dev packages, libhackrf, libusb-1.0, SoapySDR.
#
# The OnAir build (about 20 minutes) runs only when _onair-build has no libdect2core.a yet, or with ONAIR_REBUILD=1;
# otherwise only the wrapper's sources are compiled and linked. ONAIR_SRC defaults to the checkout _onair-build was made from.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
b=$here/_onair-build
o=$here/_build
if [ -z "${ONAIR_SRC:-}" ] && [ -f "$b/CMakeCache.txt" ]; then
  ONAIR_SRC=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$b/CMakeCache.txt")
fi
src=${ONAIR_SRC:?set ONAIR_SRC to an OnAir checkout}
if [ ! -f "$b/build.ninja" ]; then
  cmake -S "$src" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=OFF -DDECT2_BUILD_TESTS=OFF \
    -DDECT2_WITH_WHISPER=OFF -DDECT2_BUILD_PLUGIN_EXAMPLES=OFF -DDECT2_HAVE_HAMLIB=OFF -DDECT2_WITH_RFNM=OFF
fi
if [ ! -f "$b/libdect2core.a" ] || [ "${ONAIR_REBUILD:-0}" = 1 ]; then
  nice -n 19 ninja -C "$b" -j"${JOBS:-8}"
fi
flags=$(awk '/^build CMakeFiles\/onair-server.dir\/server\/main.cpp.o:/{f=1} f&&/^  DEFINES/{d=$0} f&&/^  FLAGS/{fl=$0} f&&/^  INCLUDES/{print d; print fl; print $0; exit}' "$b/build.ninja" | sed 's/^  [A-Z]* = //; s/\\"/"/g' | tr '\n' ' ')
libs=$(awk '/^build onair-server:/{f=1} f&&/LINK_LIBRARIES/{sub(/.*= /,""); print; exit}' "$b/build.ninja" | sed 's#libonair_web.a##')
mkdir -p "$o"
objs=()
for f in "$here"/src/*.cpp; do
  obj=$o/$(basename "${f%.cpp}").o
  # shellcheck disable=SC2086
  g++ $flags -Wall -Wextra -I"$here/src" -I"$src/core/include" -c "$f" -o "$obj"
  objs+=("$obj")
done
cd "$b"   # the library list has paths relative to the OnAir build
# shellcheck disable=SC2086
g++ -o "$here/onair-rx" "${objs[@]}" -Wl,--start-group $libs -Wl,--end-group -lpthread
echo "built $here/onair-rx"
