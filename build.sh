#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds onair-rx: OnAir headless (no GUI, no tests, no whisper) in _onair-build, then src/*.cpp linked against its libraries
# with onair-server's own flags and libraries. Needs the FFmpeg dev packages, libhackrf, libusb-1.0, SoapySDR.
#
# OnAir is built from _onair-src: a copy of the checkout $ONAIR_SRC with every patches/*.patch applied (git apply, in name
# order). The checkout itself is never changed. The copy is remade when the checkout's commit or a patch's content changes
# (its stamp, _onair-src/.onair-rx-stamp, holds both hashes and is written once OnAir has built from the copy); only files
# whose content changed get a new time, so ninja rebuilds only what they touch. OnAir's version (ONAIR_VERSION) is the checkout's short commit followed by +patched.
#
# The OnAir build (about 20 minutes) runs when _onair-build has no libdect2core.a yet, when the copy was remade or
# reconfigured, or with ONAIR_REBUILD=1; otherwise only the wrapper's sources are compiled and linked. ONAIR_SRC defaults
# to the checkout _onair-src was made from.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
s=$here/_onair-src
b=$here/_onair-build
o=$here/_build
if [ -z "${ONAIR_SRC:-}" ] && [ -f "$s/.onair-rx-origin" ]; then
  ONAIR_SRC=$(cat "$s/.onair-rx-origin")
fi
if [ -z "${ONAIR_SRC:-}" ] && [ -f "$b/CMakeCache.txt" ]; then   # an _onair-build configured from the checkout itself
  ONAIR_SRC=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$b/CMakeCache.txt")
fi
src=$(cd "${ONAIR_SRC:?set ONAIR_SRC to an OnAir checkout}" && pwd)
[ "$src" != "$s" ] || { echo "build.sh: ONAIR_SRC is _onair-src itself; set it to the OnAir checkout" >&2; exit 1; }
commit=$(git -C "$src" rev-parse HEAD) || { echo "build.sh: $src is not a git checkout" >&2; exit 1; }
version="$(git -C "$src" rev-parse --short HEAD)+patched"
shopt -s nullglob
patches=("$here"/patches/*.patch)
shopt -u nullglob
# the patches' names and contents, in one hash (test/patch.sh computes the same)
phash=$(cd "$here" && { [ "${#patches[@]}" = 0 ] || sha256sum "${patches[@]#"$here"/}"; } | sha256sum | cut -d' ' -f1)
stamp="onair $commit
patches $phash"

# --- the patched copy
fresh=0
if [ ! -f "$s/.onair-rx-stamp" ] || [ "$(cat "$s/.onair-rx-stamp")" != "$stamp" ]; then
  t=$(mktemp -d)
  trap 'rm -rf "$t"' EXIT
  rsync -a --exclude=.git "$src/" "$t/"
  for p in "${patches[@]}"; do
    (cd "$t" && git apply "$p") || { echo "build.sh: $(basename "$p") does not apply to OnAir $commit" >&2; exit 1; }
  done
  mkdir -p "$s"
  rm -f "$s/.onair-rx-stamp"   # until OnAir has built from the new copy
  # by content (-c) and without the checkout's times (no -t): a file that changed gets the time now, the others keep theirs
  rsync -rlc --delete --exclude=/.onair-rx-origin "$t/" "$s/"
  printf '%s\n' "$src" >"$s/.onair-rx-origin"
  rm -rf "$t"
  trap - EXIT
  fresh=1   # the stamp is written once OnAir has built from the copy, so a failed build is resumed by the next run
  echo "made _onair-src: OnAir ${commit:0:7} with ${#patches[@]} patch(es)"
fi

# --- configure from _onair-src with the version; a build directory configured from another source is configured afresh
home= have=
if [ -f "$b/CMakeCache.txt" ]; then
  home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$b/CMakeCache.txt")
  have=$(sed -n 's/^ONAIR_VERSION:STRING=//p' "$b/CMakeCache.txt")
fi
if [ -n "$home" ] && [ "$home" != "$s" ]; then
  rm -rf "$b/CMakeCache.txt" "$b/CMakeFiles"
fi
if [ ! -f "$b/build.ninja" ] || [ "$home" != "$s" ] || [ "$have" != "$version" ]; then
  cmake -S "$s" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=OFF -DDECT2_BUILD_TESTS=OFF \
    -DDECT2_WITH_WHISPER=OFF -DDECT2_BUILD_PLUGIN_EXAMPLES=OFF -DDECT2_HAVE_HAMLIB=OFF -DDECT2_WITH_RFNM=OFF \
    -DONAIR_VERSION="$version"
  rm -f "$s/.onair-rx-stamp"
  fresh=1
fi
if [ ! -f "$b/libdect2core.a" ] || [ "$fresh" = 1 ] || [ "${ONAIR_REBUILD:-0}" = 1 ]; then
  nice -n 19 ninja -C "$b" -j"${JOBS:-8}"
fi
[ -f "$s/.onair-rx-stamp" ] || printf '%s\n' "$stamp" >"$s/.onair-rx-stamp"

# --- the wrapper, with onair-server's flags and libraries (kept in _build for test/patch.sh)
flags=$(awk '/^build CMakeFiles\/onair-server.dir\/server\/main.cpp.o:/{f=1} f&&/^  DEFINES/{d=$0} f&&/^  FLAGS/{fl=$0} f&&/^  INCLUDES/{print d; print fl; print $0; exit}' "$b/build.ninja" | sed 's/^  [A-Z]* = //; s/\\"/"/g' | tr '\n' ' ')
libs=$(awk '/^build onair-server:/{f=1} f&&/LINK_LIBRARIES/{sub(/.*= /,""); print; exit}' "$b/build.ninja" | sed 's#libonair_web.a##')
mkdir -p "$o"
printf '%s\n' "$flags" >"$o/onair.flags"
printf '%s\n' "$libs" >"$o/onair.libs"
objs=()
for f in "$here"/src/*.cpp; do
  obj=$o/$(basename "${f%.cpp}").o
  # shellcheck disable=SC2086
  g++ $flags -Wall -Wextra -I"$here/src" -I"$s/core/include" -c "$f" -o "$obj"
  objs+=("$obj")
done
# the tests' signal generator (test/bus.sh): OnAir's own test signals, one known message per mode
# shellcheck disable=SC2086
g++ $flags -Wall -Wextra -I"$s/core/include" -c "$here/test/gen.cpp" -o "$o/gen.o"
cd "$b"   # the library list has paths relative to the OnAir build
# shellcheck disable=SC2086
g++ -o "$here/onair-rx" "${objs[@]}" -Wl,--start-group $libs -Wl,--end-group -lpthread
# shellcheck disable=SC2086
g++ -o "$o/onair-gen" "$o/gen.o" -Wl,--start-group $libs -Wl,--end-group -lpthread
echo "built $here/onair-rx and $o/onair-gen"
