#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
#
# End-to-end check of the polled modes (rds, dmr, p25, m17, dvbt, video) on OnAir's own test signals: _build/onair-gen makes
# a known station, call or picture (RDS: PI 2AB1, PS "ZETA FM ", RT "HELLO FROM ZETASDR", 6 s of OnAir's FM generator; DMR,
# P25, M17: one call sent twice, with the node's check values; DVB-T: 6 s of an 8 MHz 8K multiplex with one service, ZETA
# TEST; video: 3 s of a PAL test card on FM), onair-rx decodes it. The signals are synthesised; no capture is used.
# Also: a rate below the receiver's minimum and --pcap-port are refused before any input is read; the programme service name
# and the radiotext are absent until complete, also on input that ends before the name is; a name and a text beyond ASCII are
# written in UTF-8, one valid line each (and the UTF-8 writer on its own); the input through a pipe in uneven chunks gives the
# same lines as from a file; each pass of a call gives one first line and one last line, the same with the speech codecs
# hidden (and the call keying on its own); a status mode (dvbt, video) writes its first line within 2 s of input and then one
# at least every 2 s, also on noise that never locks (dvbt: while trying every channel width), a DVB-T service name beyond
# ASCII in UTF-8; the user CPU of each status run is printed; onair-gen refuses an option its mode does not take.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${ONAIR_RX:-$here/onair-rx}
gen=${ONAIR_GEN:-$here/_build/onair-gen}
fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$bin" ] || fail "no binary at $bin (run ./build.sh)"
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT

# --- onair-rx has each mode: on empty input it ends with exit 0 and no line
for mr in rds:500000 dmr:48000 p25:250000 m17:250000 dvbt:10000000 video:16000000; do
  m=${mr%:*} rate=${mr#*:}
  "$bin" --mode "$m" --rate "$rate" --format cf32 </dev/null >"$d/$m.empty" 2>"$d/$m.empty.err" ||
    fail "$m: $(cat "$d/$m.empty.err")"
  [ ! -s "$d/$m.empty" ] || fail "$m: a line on empty input"
done
echo "ok: onair-rx has rds, dmr, p25, m17, dvbt and video"

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
for mr in dmr:48000 p25:250000 m17:250000; do
  m=${mr%:*} rate=${mr#*:}
  refuses "onair-rx: $((rate / 2)) Hz is too low for $m (at least $rate Hz)" --mode "$m" --rate $((rate / 2)) || fail "$m at half its rate"
  refuses "onair-rx: $m has no packets (no --pcap-port)" --mode "$m" --rate "$rate" --pcap-port 5000 || fail "$m with --pcap-port"
done
# dvbt: the rate must take the channel width asked for (7.9 MHz for 8 MHz, OnAir's receiver's floor), or without one at least
# the narrowest it tries (5 MHz); video: OnAir's analog TV receiver's floor
refuses "onair-rx: 7000000 Hz is too low for dvbt (at least 7900000 Hz)" --mode dvbt --rate 7000000 --bandwidth 8 || fail "dvbt 8 MHz at 7 MHz"
refuses "onair-rx: 4000000 Hz is too low for dvbt (at least 4937500 Hz)" --mode dvbt --rate 4000000 || fail "dvbt at 4 MHz"
refuses "onair-rx: dvbt has no packets (no --pcap-port)" --mode dvbt --rate 10000000 --pcap-port 5000 || fail "dvbt with --pcap-port"
refuses "onair-rx: bad --bandwidth 9 (8, 7, 6 or 5)" --mode dvbt --rate 10000000 --bandwidth 9 || fail "dvbt at 9 MHz"
refuses "onair-rx: video has no --bandwidth (dvbt only)" --mode video --rate 16000000 --bandwidth 8 || fail "video with --bandwidth"
refuses "onair-rx: 4000000 Hz is too low for video (at least 8000000 Hz)" --mode video --rate 4000000 || fail "video at 4 MHz"
refuses "onair-rx: video has no packets (no --pcap-port)" --mode video --rate 16000000 --pcap-port 5000 || fail "video with --pcap-port"

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

# --- the voice modes: the call keying on its own, then one call of each mode sent twice (OnAir's generator, the node's check
# values), decoded with the speech codecs as the system has them and again with them hidden
"$gen" --selftest calls || fail "the call keying"

# calls <tag> <rate> <the first line's members, JSON>: $d/<tag>.jsonl is exactly two calls, each a first line whose members
# (beyond event and t_sample) are exactly the given ones with eot false, then a last line with eot true and nothing between;
# each first line comes from inside its pass of the call, as the generator said ("message from A s to B s and again from C s
# to D s"): the block it was written after overlaps the call; t_sample never decreases, no member is null
calls() {
  python3 -I - "$d/$1.jsonl" "$d/$1.gen" "$2" "$3" <<'EOF'
import json, re, sys
path, genlog, rate, want = sys.argv[1], open(sys.argv[2]).read(), int(sys.argv[3]), json.loads(sys.argv[4])
m = re.search(r"message from ([0-9.]+) s to ([0-9.]+) s and again from ([0-9.]+) s to ([0-9.]+) s", genlog)
assert m, f"the generator did not say where the calls are: {genlog!r}"
passes = [(float(m[1]), float(m[2])), (float(m[3]), float(m[4]))]
raw = open(path, "rb").read()
assert raw.endswith(b"\n"), "no line, or the last line is not ended"
lines = [json.loads(l) for l in raw.splitlines()]
desc = "\n  ".join(json.dumps(o) for o in lines)
assert len(lines) == 4, f"{len(lines)} lines, want 4 (two calls, a first and a last line each):\n  {desc}"
last = -1
for n, o in enumerate(lines):
    assert o.get("event") == "voice.call", f"line {n}: event {o.get('event')!r}"
    t = o.get("t_sample")
    assert type(t) is int and t >= last, f"line {n}: t_sample {t!r} (after {last})"
    last = t
    assert all(v is not None for v in o.values()), f"line {n}: a null member"
members = lambda o: {k: v for k, v in o.items() if k not in ("event", "t_sample")}
for k, (a, b) in enumerate(passes):
    first, end = lines[2 * k], lines[2 * k + 1]
    assert members(first) == dict(want, eot=False), f"call {k + 1}: first line {json.dumps(first)}, want {json.dumps(dict(want, eot=False))}"
    # t_sample is the first sample of the block of 16384 after which the poll ran: the call is in hand within that block
    t = first["t_sample"]
    assert a * rate < t + 16384 and t <= b * rate, f"call {k + 1}: first line's block from sample {t} outside {a} s to {b} s"
    assert members(end) == dict(want, eot=True), f"call {k + 1}: last line {json.dumps(end)}, want the first line's members with eot true"
print(f"ok: two calls, a first and a last line each, the first lines at {lines[0]['t_sample'] / rate:.3f} s and "
      f"{lines[2]['t_sample'] / rate:.3f} s: {json.dumps(want)}")
EOF
}

# voice <mode> <rate> <the first line's members>: the calls with the codecs as the system has them, then with them hidden
# from the receivers (DECT2_MBELIB_PATH and DECT2_CODEC2_PATH name a library that is not there; OnAir then looks for no
# other copy): the same lines, byte for byte
voice() {
  local m=$1 rate=$2 st=0
  gen "$m" --mode "$m" --rate "$rate"
  env -u DECT2_MBELIB_PATH -u DECT2_CODEC2_PATH "$bin" --format cf32 --mode "$m" --rate "$rate" <"$d/$m.iq" >"$d/$m.jsonl" \
    2>"$d/$m.err" || st=$?
  [ "$st" = 0 ] || fail "$m: onair-rx exited $st: $(tail -n 3 "$d/$m.err")"
  calls "$m" "$rate" "$3" || fail "$m"
  DECT2_MBELIB_PATH=/nonexistent DECT2_CODEC2_PATH=/nonexistent "$bin" --format cf32 --mode "$m" --rate "$rate" \
    <"$d/$m.iq" >"$d/$m-hidden.jsonl" 2>"$d/$m-hidden.err" || st=$?
  [ "$st" = 0 ] || fail "$m with the codecs hidden: onair-rx exited $st: $(tail -n 3 "$d/$m-hidden.err")"
  cmp -s "$d/$m.jsonl" "$d/$m-hidden.jsonl" ||
    fail "$m: other lines with the codecs hidden: $(diff "$d/$m.jsonl" "$d/$m-hidden.jsonl" | head -n 6)"
  echo "ok: $m: the same lines with the speech codecs hidden"
}
voice dmr 48000 '{"type":"call","src":9990001,"dst":9990,"call":"group","color_code":5,"slot":1,"encrypted":false,"emergency":false}'
voice p25 250000 '{"type":"call","src":9990001,"dst":65000,"call":"group","nac":291,"encrypted":false,"emergency":false,"algid":128,"kid":0}'
voice m17 250000 '{"src":"VK4ZET","dst":"ALL","can":0,"type":"voice"}'

# channel <tag> <rate> <the channel line's members, JSON>: $d/<tag>.jsonl (a channel on the air, sent twice) starts with one
# channel line with exactly those members, and has no other (the same channel again is not written again); every other line
# is a call of the same colour code, and each call on a slot gives a first line, any lines of what changed, and one last line
channel() {
  python3 -I - "$d/$1.jsonl" "$3" <<'EOF'
import json, sys
lines, want = [json.loads(l) for l in open(sys.argv[1], "rb")], json.loads(sys.argv[2])
assert lines, "no line"
members = lambda o: {k: v for k, v in o.items() if k not in ("event", "t_sample")}
assert members(lines[0]) == want, f"first line {json.dumps(lines[0])}, want {json.dumps(want)}"
chans = [o for o in lines if o.get("type") == "channel"]
assert len(chans) == 1, f"{len(chans)} channel lines, want 1: {chans}"
open_, calls = {}, 0
for o in lines[1:]:
    assert o["type"] == "call" and o.get("color_code") == want.get("color_code"), f"not a call of the channel: {json.dumps(o)}"
    s = o.get("slot")
    if s not in open_: calls += 1
    if o["eot"]: open_.pop(s, None)
    else: open_[s] = o
assert not open_, f"calls without a last line: {list(open_.values())}"
print(f"ok: one channel line, {json.dumps(want)}, then {calls} calls with a first and a last line each")
EOF
}
gen dmr-channel --mode dmr --rate 48000 --channel
rx dmr-channel --mode dmr --rate 48000
channel dmr-channel 48000 '{"type":"channel","color_code":5,"control":false,"network":"channel, colour code 5"}' ||
  fail "a DMR repeater's channel"
gen p25-channel --mode p25 --rate 250000 --channel
rx p25-channel --mode p25 --rate 250000
channel p25-channel 250000 '{"type":"channel","nac":291,"control":true,"network":"control channel, NAC 291"}' ||
  fail "a P25 control channel"

# --- the status modes (dvbt, video): a line on lock, on a change of what the signal says, and at least every 2 s

# timed <tag> <onair-rx args...>: as rx, from $d/<tag>.iq or, when there is none, stdin; prints onair-rx's user CPU over the
# seconds of input it read
timed() {
  local tag=$1 st=0 in=$d/$1.iq secs
  shift
  [ -f "$in" ] || in=/dev/stdin
  /usr/bin/time -f %U -o "$d/$tag.cpu" "$bin" "$@" <"$in" >"$d/$tag.jsonl" 2>"$d/$tag.err" || st=$?
  [ "$st" = 0 ] || fail "$tag: onair-rx exited $st: $(tail -n 3 "$d/$tag.err")"
  secs=$(sed -n 's/^onair-rx: [0-9]* samples (\([0-9.]*\) s)$/\1/p' "$d/$tag.err")
  echo "ok: $tag: user CPU $(tail -n 1 "$d/$tag.cpu") s for $secs s of input"
}

# status <tag> <mode> <rate> <input samples> <what is wanted, JSON>: every line of $d/<tag>.jsonl is one <mode>.status
# object of the row's members only, each of its type and each enumerated one spelled as the row's own decoder spells it
# (dvbt: dvbt-rx; video: the node's composite decoder), no null, t_sample never decreasing, locked and lock on every line and
# saying the same, dvbt's signalling only while locked; the output is UTF-8 with no control byte inside a line. The first line
# waits for first lock or 2 s of input: a locked one comes from a poll whose block starts before 2 s, an unlocked one from the
# poll of the block that reaches 2 s and no earlier. Successive lines are never more than 2 s of input apart, and the last is
# less than 2 s from the end. Wanted: "first", the members the first line has; "locked", the members some locked line has
# (and every locked line with signalling has them); "service", an object some line's services holds; "never", no line is
# locked; "lines", at least that many lines; "snr_above", every snr_db present is above it (a made-up 0 is not written)
status() {
  python3 -I - "$d/$1.jsonl" "$4" "$3" "$2" "$5" <<'EOF'
import json, sys
path, nsamp, rate, mode, want = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], json.loads(sys.argv[5])
block = 16384   # runPolled's: t_sample is the first sample of the block after which the poll ran
raw = open(path, "rb").read()
assert raw.endswith(b"\n"), "no line, or the last line is not ended"
raw.decode("utf-8")   # strict: text is UTF-8, not a byte per character
bad = [b for b in set(raw) if b < 0x20 and b != 0x0A]
assert not bad, f"control bytes {sorted(bad)} in the output"
lines = [json.loads(l) for l in raw.splitlines()]
if mode == "dvbt":
    rates = {"1/2", "2/3", "3/4", "5/6", "7/8"}
    types = {"locked": bool, "lock": str, "bandwidth_hz": int, "mode": str, "guard": str, "constellation": str, "code_rate": str,
             "hierarchy": str, "lp_code_rate": str, "cell_id": int, "snr_db": (int, float), "services": list}
    enums = {"lock": {"locked", "searching"}, "bandwidth_hz": {8000000, 7000000, 6000000, 5000000}, "mode": {"2K", "8K", "4K"},
             "guard": {"1/32", "1/16", "1/8", "1/4"}, "constellation": {"QPSK", "16-QAM", "64-QAM"}, "code_rate": rates,
             "hierarchy": {"a=1", "a=2", "a=4"}, "lp_code_rate": rates}
    signalling = {"bandwidth_hz", "mode", "guard", "constellation", "code_rate"}
else:
    types = {"system": str, "standard": str, "locked": bool, "lock": str, "line_hz": int, "snr_db": int}
    enums = {"lock": {"locked", "no lock"}, "standard": {"pal", "ntsc", "secam"}, "system": {"PAL 625/50", "NTSC 525/60", "SECAM 625/50"}}
    signalling = {"system", "standard"}
assert lines, "no line"
last = None
for n, o in enumerate(lines):
    assert isinstance(o, dict), f"line {n}: not an object"
    assert o.get("event") == f"{mode}.status", f"line {n}: event {o.get('event')!r}"
    t = o.get("t_sample")
    assert type(t) is int and 0 <= t < nsamp, f"line {n}: t_sample {t!r} (of {nsamp} samples)"
    if last is None:
        assert t < 2 * rate, f"the first line after the block from {t / rate:.3f} s, not within 2 s of input"
        assert o.get("locked") or t + block >= 2 * rate, f"an unlocked first line after {(t + block) / rate:.3f} s of input, before 2 s"
    else:
        assert t >= last, f"line {n}: t_sample {t} before {last}"
        assert t - last <= 2 * rate, f"line {n}: {(t - last) / rate:.3f} s of input after the line before it"
    last = t
    for k, v in o.items():
        if k in ("event", "t_sample"): continue
        assert k in types, f"line {n}: a member {k!r} the row does not have"
        assert v is not None, f"line {n}: {k} is null"
        ok = types[k] if isinstance(types[k], tuple) else (types[k],)
        assert type(v) in ok, f"line {n}: {k} {v!r} is not {' or '.join(t.__name__ for t in ok)}"
        if k in enums: assert v in enums[k], f"line {n}: {k} {v!r}, not one of {sorted(enums[k], key=str)}"
    assert "locked" in o and "lock" in o, f"line {n}: no locked or no lock: {o}"
    assert (o["lock"] == "locked") == o["locked"], f"line {n}: lock {o['lock']!r} with locked {o['locked']}"
    if mode == "dvbt" and not o["locked"]:
        assert set(o) == {"event", "t_sample", "locked", "lock"}, f"line {n}: signalling while not locked: {o}"
    for s in o.get("services", []):
        assert type(s.get("id")) is int and set(s) <= {"id", "name", "kind"}, f"line {n}: service {s}"
        assert s.get("kind", "tv") in ("tv", "radio", "data"), f"line {n}: service kind {s.get('kind')!r}"
assert nsamp - last <= 2 * rate, f"the last line {(nsamp - last) / rate:.3f} s of input before the end"
assert len(lines) >= want.get("lines", 1), f"{len(lines)} lines, want at least {want.get('lines', 1)}"
locked = [o for o in lines if o["locked"]]
everything = "the lines:\n  " + "\n  ".join(map(json.dumps, lines))
if "snr_above" in want:
    low = [o for o in lines if "snr_db" in o and not o["snr_db"] > want["snr_above"]]
    assert not low, f"an snr_db not above {want['snr_above']}: {json.dumps(low[0])}"
if "first" in want:
    assert all(lines[0].get(k) == v for k, v in want["first"].items()), f"the first line is not {want['first']}; {everything}"
if want.get("never"):
    assert not locked, f"a locked line on a signal that has none: {json.dumps(locked[0])}"
if "locked" in want:
    w = want["locked"]
    assert any(all(o.get(k) == v for k, v in w.items()) for o in locked), f"no locked line with {w}; {everything}"
    for o in locked:
        if signalling & set(o):
            assert all(o.get(k) == v for k, v in w.items()), f"a locked line that is not {w}: {json.dumps(o)}"
if "service" in want:
    assert any(want["service"] in o.get("services", []) for o in lines), f"no line's services hold {want['service']}; {everything}"
first = next((o["t_sample"] for o in locked), None)
gap = max([b["t_sample"] - a["t_sample"] for a, b in zip(lines, lines[1:])] or [0])
print(f"ok: {len(lines)} {mode}.status lines, the first at {lines[0]['t_sample'] / rate:.3f} s, "
      + (f"locked from {first / rate:.3f} s" if first is not None else "never locked")
      + f", at most {gap / rate:.3f} s apart: {json.dumps(locked[-1] if locked else lines[-1], ensure_ascii=False)}")
EOF
}
samples() { echo $(($(stat -c %s "$d/$1.iq") / 8)); }

# DVB-T: OnAir's generator's 8 MHz multiplex (8K, guard 1/8, 64-QAM, rate 3/4) with one television service, ZETA TEST, at
# 10 MHz; the width is found (8 MHz is tried first) and the service list read from the transport stream
dvbt_locked='{"locked":true,"lock":"locked","bandwidth_hz":8000000,"mode":"8K","guard":"1/8","constellation":"64-QAM","code_rate":"3/4"}'
gen dvbt --mode dvbt --rate 10000000
timed dvbt --format cf32 --mode dvbt --rate 10000000
status dvbt dvbt 10000000 "$(samples dvbt)" \
  "{\"snr_above\":20,\"first\":$dvbt_locked,\"locked\":$dvbt_locked,\"service\":{\"id\":4660,\"name\":\"ZETA TEST\",\"kind\":\"tv\"}}" || fail "dvbt"
rm -f "$d/dvbt.iq"
# the same multiplex after 1.855 s of noise: OnAir's receiver decodes its TPS a few blocks before the 8 MHz width's 2 s end
# (found by trying leads; it is the same on every run), so the first line, at 2 s, is locked at 8 MHz, and the width is
# held from the lock the line reports: no line says 7 MHz, nor another width's signalling; the lock's first line has no
# snr_db yet (no channel estimate), rather than a 0
gen dvbt-late --mode dvbt --rate 10000000 --lead 1.855
timed dvbt-late --format cf32 --mode dvbt --rate 10000000
status dvbt-late dvbt 10000000 "$(samples dvbt-late)" "{\"snr_above\":20,\"first\":$dvbt_locked,\"locked\":$dvbt_locked}" ||
  fail "dvbt, a multiplex that locks at the end of the 8 MHz window"
rm -f "$d/dvbt-late.iq"
# a 7 MHz multiplex: 8 MHz gives no lock in its 2 s, then 7 does
gen dvbt7 --mode dvbt --rate 10000000 --bandwidth 7
timed dvbt7 --format cf32 --mode dvbt --rate 10000000
status dvbt7 dvbt 10000000 "$(samples dvbt7)" "{\"locked\":${dvbt_locked/8000000/7000000}}" || fail "dvbt, a 7 MHz multiplex"
rm -f "$d/dvbt7.iq"
# a service name beyond ASCII, in ISO 8859-5's Cyrillic (EN 300 468's first byte 0x01), with a quote and a backslash: one
# valid line each, the name in UTF-8
gen dvbt-cyr --mode dvbt --rate 10000000 --service $'\x01ZETA "\xc2\xd5\xe1\xe2" \\'
timed dvbt-cyr --format cf32 --mode dvbt --rate 10000000 --bandwidth 8
status dvbt-cyr dvbt 10000000 "$(samples dvbt-cyr)" '{"service":{"id":4660,"name":"ZETA \"Тест\" \\","kind":"tv"}}' ||
  fail "dvbt, a service name beyond ASCII"
rm -f "$d/dvbt-cyr.iq"
# noise only, 8 s of random bytes read as cs16: it never locks, every width is tried in turn, a line at least every 2 s
head -c $((8 * 10000000 * 4)) /dev/urandom | timed dvbt-noise --format cs16 --mode dvbt --rate 10000000
status dvbt-noise dvbt 10000000 80000000 '{"never":true,"lines":3}' || fail "dvbt on noise"

# analog video: OnAir's FPV generator's PAL test card on FM with the swing of the node's check, at 16 MHz and at the
# receiver's floor, 8 MHz (the node's check capture's rate)
for rate in 16000000 8000000; do
  gen "video-$rate" --mode video --rate "$rate"
  timed "video-$rate" --format cf32 --mode video --rate "$rate"
  status "video-$rate" video "$rate" "$(samples "video-$rate")" \
    '{"first":{"locked":true,"standard":"pal"},"locked":{"locked":true,"lock":"locked","standard":"pal","system":"PAL 625/50"}}' ||
    fail "video at $rate Hz"
  rm -f "$d/video-$rate.iq"
done
# noise only, 4 s of random bytes read as cs16 at the floor: never locked, a line at least every 2 s
head -c $((4 * 8000000 * 4)) /dev/urandom | timed video-noise --format cs16 --mode video --rate 8000000
status video-noise video 8000000 32000000 '{"never":true,"lines":2}' || fail "video on noise"

# --- onair-gen's rds takes --ps and --rt only, a voice mode none of the options, dvbt --bandwidth (8, 7, 6 or 5), --service
# and --lead only, video --deviation only; another mode does not take them
for a in "--mode rds --rate 500000 --freq 100000000" "--mode rds --rate 500000 --text X" "--mode rds --rate 500000 --info X" \
  "--mode pocsag --rate 1000000 --ps X" "--mode pocsag --rate 1000000 --rt X" "--mode dmr --rate 48000 --freq 100000000" \
  "--mode p25 --rate 250000 --text X" "--mode m17 --rate 250000 --ps X" "--mode m17 --rate 250000 --channel" \
  "--mode rds --rate 500000 --channel" "--mode dvbt --rate 10000000 --ps X" "--mode dvbt --rate 10000000 --deviation 1000000" \
  "--mode video --rate 16000000 --service X" "--mode video --rate 16000000 --bandwidth 7" "--mode rds --rate 500000 --bandwidth 8" \
  "--mode dvbt --rate 10000000 --bandwidth 9" "--mode pocsag --rate 1000000 --deviation 1000000" \
  "--mode video --rate 8000000 --lead 1"; do
  st=0
  # shellcheck disable=SC2086
  "$gen" $a >/dev/null 2>"$d/gen-refuse.err" || st=$?
  [ "$st" = 2 ] || fail "onair-gen $a exited $st, not 2"
  grep -q '^usage: onair-gen' "$d/gen-refuse.err" || fail "onair-gen $a gave no usage"
done
echo "ok: onair-gen refuses an option its mode does not take"
echo "ok: poll.sh"
