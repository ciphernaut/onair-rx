#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# End-to-end check of the polled modes (rds) on OnAir's own test signals: _build/onair-gen makes a known station (RDS: PI
# 2AB1, PS "ZETA FM ", RT "HELLO FROM ZETASDR", 6 s of OnAir's FM generator), onair-rx decodes it. The signals are
# synthesised; no capture is used. Also: a rate below the receiver's minimum and --pcap-port are refused before any input is
# read; the programme service name and the radiotext are absent until complete.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${ONAIR_RX:-$here/onair-rx}
gen=${ONAIR_GEN:-$here/_build/onair-gen}
fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$bin" ] || fail "no binary at $bin (run ./build.sh)"
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

# --- onair-rx has each mode: on empty input it ends with exit 0 and no line
for m in rds; do
  rate=500000
  "$bin" --mode "$m" --rate "$rate" --format cf32 </dev/null >"$d/$m.empty" 2>"$d/$m.empty.err" ||
    fail "$m: $(cat "$d/$m.empty.err")"
  [ ! -s "$d/$m.empty" ] || fail "$m: a line on empty input"
done
echo "ok: onair-rx has rds"

# --- refuses <stderr> <onair-rx args...>: exit 2 with exactly that one line on stderr, before reading any input (stdin stays
# open and is never written), and nothing on stdout
refuses() {
  python3 -I - "$bin" "$@" <<'EOF'
import subprocess, sys
want, args = sys.argv[2], [sys.argv[1]] + sys.argv[3:]
p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill()
    sys.exit(f"{' '.join(args[1:])} waited for input")
out, err = p.stdout.read(), p.stderr.read()
assert p.returncode == 2, f"exit {p.returncode}, not 2"
assert err == (want + "\n").encode(), f"stderr {err!r}"
assert out == b"", "it wrote to stdout"
print(f"ok: {' '.join(args[1:])} exits 2 before reading input: {want}")
EOF
}
refuses "onair-rx: 100000 Hz is too low for rds (at least 500000 Hz)" --mode rds --rate 100000 || fail "rds at 100 kHz"
refuses "onair-rx: rds has no packets (no --pcap-port)" --mode rds --rate 500000 --pcap-port 5000 || fail "rds with --pcap-port"

[ -x "$gen" ] || fail "no test signal generator at $gen (run ./build.sh)"

# gen <tag> <onair-gen args...>: the IQ in $d/<tag>.iq, what the generator said in $d/<tag>.gen
gen() {
  local tag=$1
  shift
  "$gen" "$@" >"$d/$tag.iq" 2>"$d/$tag.gen" || fail "onair-gen $*: $(cat "$d/$tag.gen")"
}
# rx <tag> <onair-rx args...>: $d/<tag>.iq through onair-rx into $d/<tag>.jsonl
rx() {
  local tag=$1 st=0
  shift
  "$bin" --format cf32 "$@" <"$d/$tag.iq" >"$d/$tag.jsonl" 2>"$d/$tag.err" || st=$?
  [ "$st" = 0 ] || fail "$tag: onair-rx exited $st: $(tail -n 3 "$d/$tag.err")"
}

# --- RDS: one line per group, each with the group's own PI, type, PTY and TP; PS and RT from OnAir's snapshot, once complete
gen rds --mode rds --rate 500000
rx rds --mode rds --rate 500000
python3 -I - "$d/rds.jsonl" "$(($(stat -c %s "$d/rds.iq") / 8))" <<'EOF' || fail rds
import json, re, sys
path, nsamp = sys.argv[1], int(sys.argv[2])
raw = open(path, "rb").read()
assert raw.endswith(b"\n") or not raw, "the last line is not ended"
lines = raw.splitlines()
assert len(lines) >= 20, f"{len(lines)} lines, want at least 20"
last, ps, rt = -1, 0, 0
for n, l in enumerate(lines):
    o = json.loads(l)
    assert isinstance(o, dict), f"line {n}: not an object"
    assert o.get("event") == "rds.group", f"line {n}: event {o.get('event')!r}"
    t = o.get("t_sample")
    assert type(t) is int and 0 <= t < nsamp, f"line {n}: t_sample {t!r} (of {nsamp} samples)"
    assert t >= last, f"line {n}: t_sample {t} before {last}"
    last = t
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
    assert o.get("pi") == "2AB1", f"line {n}: pi {o.get('pi')!r}"
    assert isinstance(o.get("group"), str) and re.fullmatch(r"[0-9]{1,2}[AB]", o["group"]), f"line {n}: group {o.get('group')!r}"
    assert type(o.get("pty")) is int and 0 <= o["pty"] <= 31, f"line {n}: pty {o.get('pty')!r}"
    assert type(o.get("tp")) is bool, f"line {n}: tp {o.get('tp')!r}"
    if "ps" in o:
        assert isinstance(o["ps"], str) and len(o["ps"]) >= 8, f"line {n}: ps {o['ps']!r} is cut"
        assert o["ps"] == "ZETA FM ", f"line {n}: ps {o['ps']!r}, not the station's"   # the generator sends one name only
        ps += 1
    if "rt" in o:
        assert o["rt"] == "HELLO FROM ZETASDR", f"line {n}: rt {o['rt']!r} is cut or not the station's"
        rt += 1
assert ps, "no line has ps 'ZETA FM '"
assert rt, "no line has rt 'HELLO FROM ZETASDR'"
print(f"ok: {len(lines)} rds.group lines of PI 2AB1, {ps} with the PS and {rt} with the RT")
EOF
echo "ok: poll.sh"
