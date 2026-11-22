#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# Checks the patches build.sh applies to OnAir:
# - each patches/*.patch, in order, applies to a clean copy of $ONAIR_SRC (git apply --check, then applied for the next);
# - when _onair-src exists, its stamp names the checkout's commit and the patches' content (else build.sh is due);
# - when OnAir has been built, the patch's own test tests/wirecap/test_wire_sink.cpp, compiled from _onair-src against
#   the built libraries with build.sh's flags, passes (Hub::setSink).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
s=$here/_onair-src
fail() { echo "FAIL: $*" >&2; exit 1; }
if [ -z "${ONAIR_SRC:-}" ] && [ -f "$s/.onair-rx-origin" ]; then ONAIR_SRC=$(cat "$s/.onair-rx-origin"); fi
src=${ONAIR_SRC:?set ONAIR_SRC to an OnAir checkout}
[ -d "$here/patches" ] || fail "no patches/ directory"
shopt -s nullglob
patches=("$here"/patches/*.patch)
[ "${#patches[@]}" -gt 0 ] || fail "no patches/*.patch"
commit=$(git -C "$src" rev-parse HEAD) || fail "$src is not a git checkout"

d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

# --- every patch applies, in order, to a clean copy
rsync -a --exclude=.git "$src/" "$d/src/"
for p in "${patches[@]}"; do
  (cd "$d/src" && git apply --check "$p") || fail "$(basename "$p") does not apply to OnAir $commit"
  (cd "$d/src" && git apply "$p")
done
echo "ok: ${#patches[@]} patch(es) apply to OnAir ${commit:0:7}"

# --- the copy build.sh made is from this commit and these patches
if [ -d "$s" ]; then
  want="onair $commit
patches $(cd "$here" && sha256sum "${patches[@]#"$here"/}" | sha256sum | cut -d' ' -f1)"   # as build.sh hashes them
  [ -f "$s/.onair-rx-stamp" ] || fail "_onair-src has no stamp (run ./build.sh)"
  [ "$(cat "$s/.onair-rx-stamp")" = "$want" ] || fail "_onair-src's stamp does not match the checkout and patches (run ./build.sh)"
  echo "ok: _onair-src matches OnAir ${commit:0:7} and the patches"
else
  echo "skip: no _onair-src yet (run ./build.sh)"
fi

# --- the sink, through the patch's own OnAir test
b=$here/_onair-build
if [ -d "$s" ] && [ -f "$b/libdect2core.a" ] && [ -f "$here/_build/onair.flags" ] && [ -f "$here/_build/onair.libs" ]; then
  flags=$(cat "$here/_build/onair.flags")
  libs=$(cat "$here/_build/onair.libs")
  # shellcheck disable=SC2086
  g++ $flags -I"$s/core/include" -c "$s/tests/wirecap/test_wire_sink.cpp" -o "$d/test_wire_sink.o"
  # shellcheck disable=SC2086
  (cd "$b" && g++ -o "$d/test_wire_sink" "$d/test_wire_sink.o" -Wl,--start-group $libs -Wl,--end-group -lpthread)
  "$d/test_wire_sink" >"$d/sink.out" 2>&1 || fail "test_wire_sink: $(tail -n 5 "$d/sink.out")"
  tail -n 1 "$d/sink.out" | grep -qx ok || fail "test_wire_sink: $(tail -n 5 "$d/sink.out")"
  echo "ok: test_wire_sink ($(grep -c '^-- ' "$d/sink.out") parts)"
else
  echo "skip: OnAir is not built from _onair-src yet (run ./build.sh): the sink test needs it"
fi
echo "PASS"
