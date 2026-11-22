#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# End-to-end check of the polled modes (rds) on OnAir's own test signals: _build/onair-gen makes a known station (RDS: PI
# 2AB1, PS "ZETA FM ", RT "HELLO FROM ZETASDR", 6 s of OnAir's FM generator), onair-rx decodes it. The signals are
# synthesised; no capture is used. Also: a rate below the receiver's minimum and --pcap-port are refused before any input is
# read; the programme service name and the radiotext are absent until complete, also on input that ends before the name is;
# a name and a text beyond ASCII are written in UTF-8, one valid line each (and the UTF-8 writer on its own); the input through
# a pipe in uneven chunks gives the same lines as from a file; onair-gen refuses an option its mode does not take.
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

# station <tag> <ps> <rt> <min lines> <text|none>: every line of $d/<tag>.jsonl is one rds.group object of PI 2AB1 with the
# group's own type, PTY and TP, t_sample never decreasing; the output is UTF-8 with no control byte inside a line; a ps or rt
# present is exactly the station's (so never cut), decoded from the RDS character set. text: some line has each, and the
# first four (before the name's four segments can have come) have neither; none: no line has either
station() {
  python3 -I - "$d/$1.jsonl" "$(($(stat -c %s "$d/$1.iq") / 8))" "$2" "$3" "$4" "$5" <<'EOF'
import json, re, sys
path, nsamp, want_ps, want_rt, least, text = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], int(sys.argv[5]), sys.argv[6] == "text"
raw = open(path, "rb").read()
assert raw.endswith(b"\n") or not raw, "the last line is not ended"
raw.decode("utf-8")   # strict: the text is UTF-8, not a byte per character
bad = [b for b in set(raw) if b < 0x20 and b != 0x0A]
assert not bad, f"control bytes {sorted(bad)} in the output"
lines = raw.splitlines()
assert len(lines) >= least, f"{len(lines)} lines, want at least {least}"
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
        assert o["ps"] == want_ps, f"line {n}: ps {o['ps']!r}, not {want_ps!r}"   # the generator sends one name only
        ps += 1
    if "rt" in o:
        assert o["rt"] == want_rt, f"line {n}: rt {o['rt']!r}, not {want_rt!r} (cut, or spelled otherwise)"
        rt += 1
    if text and n < 4:
        assert "ps" not in o and "rt" not in o, f"line {n}: a name or text before it can be complete: {l!r}"
if text:
    assert ps, f"no line has ps {want_ps!r}"
    assert rt, f"no line has rt {want_rt!r}"
else:
    assert not ps and not rt, f"{ps} line(s) with ps and {rt} with rt, want none"
print(f"ok: {len(lines)} rds.group lines of PI 2AB1, {ps} with the PS {want_ps!r} and {rt} with the RT {want_rt!r}")
EOF
}

# --- RDS: one line per group, each with the group's own PI, type, PTY and TP; PS and RT from OnAir's snapshot, once complete
gen rds --mode rds --rate 500000
rx rds --mode rds --rate 500000
station rds "ZETA FM " "HELLO FROM ZETASDR" 20 text || fail rds

# the input ends at the block whose poll first had the name: nothing before it has a name or a text, and none comes at the end
cut=$(python3 -I -c '
import json, sys
print(next(o["t_sample"] for o in map(json.loads, open(sys.argv[1], "rb")) if "ps" in o))' "$d/rds.jsonl")
head -c $((cut * 8)) "$d/rds.iq" >"$d/rds-cut.iq"
rx rds-cut --mode rds --rate 500000
station rds-cut "ZETA FM " "HELLO FROM ZETASDR" 1 none || fail "rds cut before its name is complete"

# a name and a text in the RDS character set beyond ASCII (EBU Latin: 0x80 a and 0x82 e with an acute, 0xa9 the euro sign),
# with a quote and a backslash: one valid line each, the text in UTF-8 as the station spells it
gen rds-latin --mode rds --rate 500000 --ps $'Z\x82TA FM ' --rt $'R\x80DIO "Z" \\ \xa9'
rx rds-latin --mode rds --rate 500000
station rds-latin "ZéTA FM " 'RáDIO "Z" \ €' 20 text || fail "rds beyond ASCII"
# the writer of that text on its own (onair-gen links onair-rx's): text that is not valid UTF-8 is escaped byte by byte
"$gen" --selftest json-utf8 || fail "the JSON writer's UTF-8 text"

# --- a replay gives the same lines as a live run: the RDS input through a pipe written in small uneven chunks, so that each
# read gets what happens to be there, writes byte for byte what the file gave
python3 -I - "$bin" "$d/rds.iq" "$d/rds.jsonl" <<'EOF' || fail "rds through a pipe"
import random, subprocess, sys, threading, time
binary, iq, want = sys.argv[1], open(sys.argv[2], "rb").read(), open(sys.argv[3], "rb").read()
p = subprocess.Popen([binary, "--mode", "rds", "--rate", "500000", "--format", "cf32"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
got = []
reader = threading.Thread(target=lambda: got.append(p.stdout.read()))
reader.start()
rng = random.Random(5)
at = writes = 0
while at < len(iq):
    k = rng.randint(1, 12000)   # not a whole number of samples, and less than a block
    p.stdin.write(iq[at:at + k])
    p.stdin.flush()
    at += k
    writes += 1
    if writes % 64 == 0: time.sleep(0.001)
p.stdin.close()
reader.join()
assert p.wait() == 0, f"exit {p.returncode}"
n_got, n_want = len(got[0].splitlines()), len(want.splitlines())
assert got[0] == want, f"{n_got} lines through the pipe differ from the file's {n_want}"
print(f"ok: the same {n_want} lines through a pipe in {writes} uneven writes")
EOF

# --- onair-gen's rds takes --ps and --rt only; another mode does not take them
for a in "--mode rds --rate 500000 --freq 100000000" "--mode rds --rate 500000 --text X" "--mode rds --rate 500000 --info X" \
  "--mode pocsag --rate 1000000 --ps X" "--mode pocsag --rate 1000000 --rt X"; do
  st=0
  # shellcheck disable=SC2086
  "$gen" $a >/dev/null 2>"$d/gen-refuse.err" || st=$?
  [ "$st" = 2 ] || fail "onair-gen $a exited $st, not 2"
  grep -q '^usage: onair-gen' "$d/gen-refuse.err" || fail "onair-gen $a gave no usage"
done
echo "ok: onair-gen refuses an option its mode does not take"
echo "ok: poll.sh"
