#!/usr/bin/env bash
# Spike build: OnAir headless (no GUI, no tests, no whisper), then spike/main.cpp linked against dect2core
# with onair-server's own flags and libraries. Needs the FFmpeg dev packages, libhackrf, libusb-1.0, SoapySDR.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=${ONAIR_SRC:?set ONAIR_SRC to an OnAir checkout}
b=$here/_onair-build
cmake -S "$src" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=OFF -DDECT2_BUILD_TESTS=OFF \
  -DDECT2_WITH_WHISPER=OFF -DDECT2_BUILD_PLUGIN_EXAMPLES=OFF -DDECT2_HAVE_HAMLIB=OFF -DDECT2_WITH_RFNM=OFF
nice -n 19 ninja -C "$b" -j"${JOBS:-8}"
cd "$b"
flags=$(awk '/^build CMakeFiles\/onair-server.dir\/server\/main.cpp.o:/{f=1} f&&/^  DEFINES/{d=$0} f&&/^  FLAGS/{fl=$0} f&&/^  INCLUDES/{print d; print fl; print $0; exit}' build.ninja | sed 's/^  [A-Z]* = //' | tr '\n' ' ')
libs=$(awk '/^build onair-server:/{f=1} f&&/LINK_LIBRARIES/{sub(/.*= /,""); print; exit}' build.ninja | sed 's#libonair_web.a##')
g++ $flags -I"$src/core/include" -c "$here/spike/main.cpp" -o "$here/spike/main.o"
g++ -o "$here/spike/onair-rx" "$here/spike/main.o" -Wl,--start-group $libs -Wl,--end-group -lpthread
echo "built $here/spike/onair-rx"
