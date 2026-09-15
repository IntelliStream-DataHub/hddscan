#!/bin/bash
# Command-line and reporting tests for hddscan.
#
# Everything here runs against regular files used as disk images, so the whole
# suite needs no root, no spinning disk, and never touches a real device.  Each
# test names the behaviour it guards; the ones marked REGRESSION guard a bug
# that actually shipped.
set -u

HDDSCAN=${HDDSCAN:-./hddscan}
PASS=0; FAIL=0; SKIP=0
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Every scan is recorded as a run, so the suite gets a store of its own: the
# tests must not see the machine's real runs, and must not leave any of their
# own behind in the user's state directory.
export HDDSCAN_STATE_DIR="$TMP/state"

ok()   { PASS=$((PASS+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n     %s\n' "$1" "${2:-}"; }
skip() { SKIP=$((SKIP+1)); printf '  \033[33mskip\033[0m %s (%s)\n' "$1" "$2"; }

# assert_has NAME HAYSTACK NEEDLE
assert_has() {
	case "$2" in *"$3"*) ok "$1";; *) bad "$1" "expected to find: $3";; esac
}
assert_hasnt() {
	case "$2" in *"$3"*) bad "$1" "should not contain: $3";; *) ok "$1";; esac
}
assert_eq() {
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "want [$3] got [$2]"; fi
}

# image SIZE_MB NAME -> path to a fresh random image
image() {
	dd if=/dev/urandom of="$TMP/$2" bs=1M count="$1" status=none
	echo "$TMP/$2"
}
# a written image carries a valid hddscan pattern
patterned() {
	local f; f=$(image "$1" "$2")
	$HDDSCAN --no-color --mode write --confirm "$f" "$f" >/dev/null 2>&1
	echo "$f"
}
# Most tests below are read-oriented, and the shipped default profile is
# predeploy (which writes).  Ask for the non-destructive profile explicitly so
# each test says what it is testing; the default itself is asserted separately.
run() { $HDDSCAN --no-color --outdir "$TMP" --profile inservice "$@" 2>&1; }
runp() { $HDDSCAN --no-color --outdir "$TMP" "$@" 2>&1; }

echo "== profiles decide what a run does =="

f=$(image 8 prof.bin)
before=$(md5sum < "$f")
# The default profile writes, so a bare invocation must stop and ask rather
# than doing anything.  An error is an acceptable default; data loss is not.
out=$(runp "$f" 2>&1)
assert_has "the default profile is predeploy" "$out" "needs --confirm"
assert_eq  "the default profile touches nothing without --confirm" \
	"$(md5sum < "$f")" "$before"
assert_has "an unknown profile lists the ones that exist" \
	"$(runp --profile nope "$f" 2>&1)" "predeploy"
assert_has "inservice never writes" \
	"$(runp --profile inservice "$f")" "Mode             read-only"
assert_eq  "inservice really left the data alone" "$(md5sum < "$f")" "$before"
assert_has "survey samples rather than reading everything" \
	"$(runp --profile survey "$f")" "SAMPLED"
assert_has "decay re-checks a previously written pattern" \
	"$(runp --profile decay "$f" 2>&1)" "re-check"
assert_has "the report names the profile that produced it" \
	"$(runp --profile inservice "$f")" "Profile          inservice"
# an explicit flag beats the profile whichever order they appear in
assert_has "an explicit --mode overrides the profile" \
	"$(runp --mode read --profile predeploy --confirm "$f" "$f")" \
	"Mode             read-only"
assert_has "order does not matter for that override" \
	"$(runp --profile predeploy --mode read --confirm "$f" "$f")" \
	"Mode             read-only"

echo "== correctness of the scan itself =="

f=$(image 16 clean.bin)
out=$(run "$f"); rc=$?
assert_has "a clean image reads healthy" "$out" "VERDICT: HEALTHY"
assert_eq  "healthy exits 0" "$rc" "0"

# INVARIANT 4: verify must put the original bytes back.
f=$(image 16 verify.bin)
before=$(md5sum < "$f")
run --mode verify --confirm "$f" "$f" >/dev/null
assert_eq "verify mode is non-destructive" "$(md5sum < "$f")" "$before"

# INVARIANT 2: every order visits every chunk exactly once.  A skipped chunk
# shows up as a hole in the pattern.  7M is deliberately not a power of two and
# does not divide the image evenly.
f=$(image 16 cover.bin)
covfail=0
for seg in 1M 4M 7M 16M 0; do
	run --mode write --order random --segment "$seg" --confirm "$f" "$f" >/dev/null
	n=$(run --mode check "$f" | awk '/wrong data/{print $5}')
	[ "$n" = "0" ] || { covfail=1; echo "     segment $seg left $n bad blocks"; }
done
[ "$covfail" = 0 ] && ok "random order covers every chunk (5 segment sizes)" \
                    || bad "random order covers every chunk" "see above"

# reverse order too
run --mode write --order reverse --confirm "$f" "$f" >/dev/null
assert_eq "reverse order covers every chunk" \
	"$(run --mode check "$f" | awk '/wrong data/{print $5}')" "0"

echo "== finding damage, and pointing at the right place =="

f=$(patterned 16 bb.bin)
printf 'BADBADBAD' | dd of="$f" bs=1 seek=8388608 conv=notrunc status=none
out=$(run --mode check --badblocks-list "$TMP/bb.txt" "$f"); rc=$?
assert_has "injected corruption is found" "$out" "VERDICT: FAILING"
assert_eq  "failing exits 2" "$rc" "2"
assert_eq  "badblocks list names the exact blocks" \
	"$(tr '\n' ' ' < "$TMP/bb.txt")" "8192 8193 8194 8195 "
run --mode check --badblocks-list "$TMP/bb2.txt" --badblocks-offset 1M "$f" >/dev/null
assert_eq  "badblocks offset shifts by exactly the partition start" \
	"$(tr '\n' ' ' < "$TMP/bb2.txt")" "7168 7169 7170 7171 "

echo "== the report must not contradict itself =="

# REGRESSION: percentiles came from bucket interpolation and were reported as
# if exact, producing p50 24.7 and p99.9 30.0 next to an exact max of 22.4.
f=$(image 24 pct.bin)
line=$(run "$f" | grep -m1 'chunk reads')
read -r p50 p95 p99 p999 mx <<<"$(awk '{for(i=1;i<=NF;i++){
	if($i=="p50")a=$(i+1); if($i=="p95")b=$(i+1); if($i=="p99")c=$(i+1);
	if($i=="p99.9")d=$(i+1); if($i=="max")e=$(i+1)}} END{print a,b,c,d,e}' <<<"$line")"
badpct=$(awk -v a="$p50" -v b="$p95" -v c="$p99" -v d="$p999" -v m="$mx" \
	'BEGIN{print (a<=m && b<=m && c<=m && d<=m) ? "" : "p" }')
[ -z "$badpct" ] && ok "no percentile exceeds the observed maximum" \
                 || bad "no percentile exceeds the observed maximum" "$line"

# REGRESSION: the "reads exceeding N ms" table counted whole histogram buckets,
# so it claimed 284 reads over 25 ms while the exact counter said 0.
f=$(image 24 tier.bin)
out=$(run --chunk-slow-ms 25 "$f")
tier=$(awk '/> +25 ms/{print $5}' <<<"$out")
budg=$(awk '/chunks over budget/{print $4}' <<<"$out")
assert_eq "tier table agrees with the exact over-budget counter" "$tier" "$budg"

echo "== budgets and calibration =="

# REGRESSION: the calibration line hardcoded "128K" whatever --chunk said.
f=$(image 96 chunk.bin)
assert_has "calibration reports the chunk size actually used" \
	"$(run --chunk 1M "$f")" "median 1024 KiB read"

# REGRESSION: a fixed 25 ms floor became a ceiling at large chunks, because a
# 1 MiB read honestly costs ~22 ms; every healthy chunk then drilled.
# the budget line has two shapes now (flat, or a range following the platter),
# so take the first number on it whichever it is
budget_of() { awk '/Latency budget/{for(i=1;i<=NF;i++) if ($i+0>0){print $i; exit}}'; }
b128=$(run --chunk 128K "$f" | budget_of)
b4m=$(run --chunk 4M "$f"   | budget_of)
awk -v a="$b128" -v b="$b4m" 'BEGIN{exit !(b>a)}' \
	&& ok "the budget floor grows with the chunk size" \
	|| bad "the budget floor grows with the chunk size" "128K=$b128 4M=$b4m"

assert_has "raising the chunk states what sensitivity it costs" \
	"$(run --chunk 1M "$f")" "to be noticed"

# REGRESSION: a statement appended to an unbraced "if (median)" re-bound the
# else, so a successful calibration printed both "calibrated:" and "fallback,
# not calibrated" at once.
f=$(image 96 calib.bin)
out=$(run "$f")
assert_hasnt "a successful calibration does not also claim it failed" \
	"$out" "fallback, not calibrated"
assert_has "a successful calibration says what it measured" "$out" "calibrated: median"

# The budget follows the platter's own gradient: outer tracks sweep further per
# revolution and read faster, so one flat number is loose outside and tight in.
assert_has "the budget follows the drive's gradient" \
	"$out" "following the drive's own gradient"
assert_has "the scan says what the gradient it measured looks like" \
	"$out" "the budget follows the platter"
# ...but a threshold given by hand means that number, not a bent one
assert_has "an explicit --chunk-slow-ms stays flat" \
	"$(run --chunk-slow-ms 25 "$f")" "chunk 25.0 ms, sector 25.0 ms"
assert_hasnt "an explicit --sector-slow-ms stays flat" \
	"$(run --sector-slow-ms 30 "$f")" "following the drive's own gradient"

echo "== arguments that must be refused =="

for bad_arg in "--recovery-time abc" "--recovery-time 70000" \
               "--drive-read-retries abc" "--drive-read-retries 999" \
               "--drive-write-retries -3" "--format-blocksize 1024" \
               "--format-pi 9" "--bms maybe" "--write-cache maybe" \
               "--read-cache maybe" "--awre maybe" "--arre maybe" \
               "--drive-lookahead maybe"; do
	# shellcheck disable=SC2086
	if run $bad_arg "$f" >/dev/null 2>&1; then
		bad "rejects '$bad_arg'" "it was accepted"
	else
		ok "rejects '$bad_arg'"
	fi
done

# REGRESSION: atoll() read "abc" as 0, and 0 means "unlimited"/"never retry" --
# a typo silently reconfigured the drive instead of failing.
assert_has "a non-numeric value is named in the error" \
	"$(run --recovery-time abc "$f" 2>&1)" "takes a number from"

echo "== advice the tool prints must actually work =="

# REGRESSION: the report advised repeated --set/--clear, of which sdparm
# honours only the last, so it silently applied half of what it claimed.
src=$(grep -c -- '--clear=RCD --clear=DRA' hddscan.c 2>/dev/null || true)
src=${src:-0}
assert_eq "no repeated --clear/--set in printed sdparm advice" "$src" "0"

echo "== modes, bundles and gates =="

f=$(image 16 rep.bin)
before=$(md5sum < "$f")
out=$(run --repair --confirm "$f" --chunk-slow-ms 0.3 --retries 2 "$f")
assert_has "--repair runs a destructive write pass" "$out" "destructive write + verify"
assert_has "--repair re-reads the device afterwards" "$out" "second pass"
assert_has "--repair reports what it healed" "$out" "healed by rewrite"
[ "$(md5sum < "$f")" != "$before" ] && ok "--repair really wrote to the device" \
	|| bad "--repair really wrote to the device" "contents unchanged"

# --rewrite-weak in read mode is a read-modify-write and must keep the data.
f=$(image 16 rw.bin)
before=$(md5sum < "$f")
run --rewrite-weak --chunk-slow-ms 0.3 --retries 2 --confirm "$f" "$f" >/dev/null
assert_eq "--rewrite-weak preserves data in read mode" "$(md5sum < "$f")" "$before"

# --dry-run must touch nothing at all, even in a writing mode.
f=$(image 8 dry.bin)
before=$(md5sum < "$f")
run --mode write --confirm "$f" --dry-run "$f" >/dev/null
assert_eq "--dry-run writes nothing" "$(md5sum < "$f")" "$before"

f=$(image 8 fmt.bin)
assert_has "--format refuses a regular file" \
	"$(run --format --confirm "$f" "$f" 2>&1)" "is a file"
# Command construction can only be checked against a real SCSI drive, and even
# then only under --dry-run.  Opt in deliberately; the default suite must not
# be able to reach a device at all.
if [ -n "${HDDSCAN_TEST_DEVICE:-}" ]; then
	d=$HDDSCAN_TEST_DEVICE; n=$(basename "$d")
	# --dry-run as well as the missing --confirm, so a regression in one
	# gate still cannot reach the device
	assert_has "--format refuses without --confirm" \
		"$(run --format --dry-run "$d" 2>&1)" "needs --confirm"
	out=$(run --format --confirm "$n" --dry-run "$d" 2>&1)
	assert_has "format keeps the drive's protection type by default" \
		"$out" "--fmtpinfo="
	assert_has "format prints the command and stops under --dry-run" \
		"$out" "dry run, would execute"
	out=$(run --format --format-pi none --confirm "$n" --dry-run "$d" 2>&1)
	assert_has "--format-pi none asks for no protection information" \
		"$out" "--fmtpinfo=0"
	out=$(run --format --format-blocksize 4096 --confirm "$n" --dry-run "$d" 2>&1)
	assert_has "--format-blocksize sets the size sg_format is given" \
		"$out" "--size=4096"
	assert_hasnt "a format is not fast unless asked" "$out" "--ffmt"
	assert_has "--format-fast asks the drive for a fast format" \
		"$(run --format --format-fast --confirm "$n" --dry-run "$d" 2>&1)" \
		"--ffmt=1"
	assert_hasnt "a block size change does not silently strip protection" \
		"$(run --format --format-blocksize 4096 --confirm "$n" --dry-run "$d" 2>&1)" \
		"--fmtpinfo=0"
else
	skip "format command construction" "set HDDSCAN_TEST_DEVICE=/dev/sdX"
fi
assert_has "--apply-settings with nothing to apply is an error" \
	"$(run --apply-settings "$f" 2>&1)" "without any setting"

echo "== coverage must be stated honestly =="

f=$(image 32 cov.bin)
assert_has "a sampled scan says most sectors were never read" \
	"$(run --sample 8 "$f")" "SAMPLED"
assert_has "a range-limited scan says what it did not test" \
	"$(run --end 8M "$f")" "restricted range"
assert_has "a whole-device scan says so" "$(run "$f")" "every sector of the device was read"

echo "== machine readable output =="

f=$(patterned 16 json.bin)
run --json "$TMP/r.json" --csv "$TMP/r.csv" "$f" >/dev/null
if python3 -c "
import json,sys
d=json.load(open('$TMP/r.json'))
need=['device','counters','bytes_tested','protection_type','protection_refused_bytes']
missing=[k for k in need if k not in d]
assert not missing, missing
assert 'protection_errors' in d['counters']
" 2>"$TMP/jerr"; then ok "JSON is valid and carries the expected keys"
else bad "JSON is valid and carries the expected keys" "$(cat "$TMP/jerr")"; fi
[ -s "$TMP/r.csv" ] && ok "CSV is written" || bad "CSV is written" "empty"

echo "== checkpoint and resume =="

f=$(image 32 st.bin)
run --state "$TMP/st" --max-time 1 "$f" >/dev/null
if [ -f "$TMP/st" ]; then
	ok "a checkpoint file is written"
	assert_has "a resumed scan says where it started" \
		"$(run --state "$TMP/st" --resume "$f")" "resuming at"
else
	skip "checkpoint and resume" "scan finished before the first checkpoint"
fi

echo "== runs outlive the process that started them =="

# A run is a directory of small text records, and everything that reports on
# it -- the dashboard, --status, a second copy of hddscan started later --
# reads those.  Nothing here needs the run to still be going, which is the
# whole point.

# a store of its own, since by now this suite has runs of its own in the
# shared one
assert_has "--status with an empty store says so, and where it looked" \
	"$(run --state-dir "$TMP/empty" --status)" "nothing has been run"

# Sparse images: a scan of one is real work that takes long enough to be
# caught in the act, without writing terabytes.
truncate -s 200G "$TMP/sp1.bin" "$TMP/sp2.bin" "$TMP/sp3.bin"
out=$(runp --profile inservice --detach --max-parallel 2 \
	"$TMP/sp1.bin" "$TMP/sp2.bin" "$TMP/sp3.bin" 2>&1)
assert_has "--detach prints the run id and returns" "$out" "started in the background"
rid=$(printf '%s' "$out" | sed -n 's/.*run \([0-9-]*\) started.*/\1/p')
if [ -z "$rid" ]; then
	bad "a detached run gets an id" "$out"
else
	ok "a detached run gets an id"
	st=$(run --status)
	assert_has "--status lists the run" "$st" "$rid"
	assert_has "--status says it is running" "$st" "running"
	# --max-parallel 2 against three drives: one must be waiting, and the
	# record has to say so rather than showing it at 0% "running"
	det=$(run --status "$rid")
	assert_has "the drives of a run are listed" "$det" "sp1.bin"
	assert_has "a drive waiting its turn says queued" "$det" "queued"
	assert_has "--status finds a run by the name of a drive in it" \
		"$(run --status sp2.bin)" "$rid"
	assert_has "a prefix of a run id is enough" \
		"$(run --status "${rid%??}")" "$rid"
	# REGRESSION: two runs on one spindle time each other's seeks and both
	# results are then worthless.  The kernel's O_EXCL only catches the
	# drive a worker already has open, not one still queued behind others.
	busy=$(runp --profile inservice "$TMP/sp1.bin" 2>&1)
	assert_has "a drive already in a run is refused" "$busy" "already in run"
	assert_has "the refusal names the run holding it" "$busy" "$rid"

	if python3 -c 'import json,sys; json.load(sys.stdin)' \
			<<<"$($HDDSCAN --state-dir "$HDDSCAN_STATE_DIR" --status-json)" \
			2>/dev/null; then
		ok "--status-json is valid JSON"
	else
		bad "--status-json is valid JSON" "$($HDDSCAN --status-json | head -5)"
	fi

	assert_has "--stop names the run it stopped" \
		"$(run --stop "$rid")" "stopping run $rid"
	# the stop has to reach every worker, so give them a moment to write
	# their last record
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		case "$(run --status)" in *running*) sleep 0.5;; *) break;; esac
	done
	st=$(run --status)
	assert_hasnt "a stopped run is no longer running" "$st" "running"
	assert_has "a stopped run says how many drives were stopped" "$st" "stopped"
	# REGRESSION: a drive mid-scan still owes a report for what it covered,
	# and stopping used to destroy exactly that.  The stop reached each
	# worker twice -- once directly, once forwarded by its supervisor --
	# and the second SIGINT is the hard-exit path that gives up on writing
	# the report.
	if [ -s "$TMP/hddscan-sp1.bin.txt" ]; then
		ok "a stopped drive still wrote its report"
		assert_has "the report of a stopped scan says what it covered" \
			"$(cat "$TMP/hddscan-sp1.bin.txt")" "Coverage"
	else
		bad "a stopped drive still wrote its report" \
			"no $TMP/hddscan-sp1.bin.txt"
	fi
	# Each drive gets its own checkpoint inside the run.  One --state path
	# shared by a whole parallel run had every worker overwriting the
	# others, so nothing could be resumed.
	n=$(ls "$HDDSCAN_STATE_DIR"/runs/"$rid"/*.ckpt 2>/dev/null | wc -l)
	assert_eq "every drive in a run checkpoints separately" "$n" "2"

	assert_has "--forget drops a finished run" \
		"$(run --forget "$rid")" "forgot run $rid"
	assert_hasnt "a forgotten run is gone from --status" \
		"$(run --status)" "$rid"
fi

# A record claiming to be running whose process is gone is a crash, a kill -9
# or the power going.  Saying "running" forever would be a confident wrong
# answer; the reader has to notice the pid is not there.
mkdir -p "$HDDSCAN_STATE_DIR/runs/20200101-000000"
d=$HDDSCAN_STATE_DIR/runs/20200101-000000
cat > "$d/run" <<EOF
run 1
id 20200101-000000
kind scan
what write (predeploy)
detail destructive write + verify
outdir $TMP
created $(( $(date +%s) - 60 ))
ended 0
sup 999999
supstart 12345
njobs 1
EOF
cat > "$d/sdz.job" <<EOF
job 1
device sdz
path /dev/sdz
model MADE UP
size 1000000000000
state 1
verdict -1
pid 999999
pidstart 12345
started $(( $(date +%s) - 60 ))
updated $(( $(date +%s) - 60 ))
pct 42.0
EOF
st=$(run --status)
assert_has "a run whose processes are gone is not called running" "$st" "interrupted"
assert_has "--status counts the drives that never finished" "$st" "never finished"
assert_has "such a drive is reported as orphaned" \
	"$(run --status 20200101-000000)" "orphaned"
assert_has "--forget refuses nothing here and tidies it away" \
	"$(run --forget 20200101-000000)" "forgot run"

assert_has "--stop with no such run is an error, not a silent success" \
	"$(run --stop nosuchrun 2>&1)" "no run matches"
assert_eq "--stop with no such run exits 2" \
	"$(run --stop nosuchrun >/dev/null 2>&1; echo $?)" "2"

echo "== a drive too slow to test is failing =="

# A drive can return every sector inside its budget and still take a year to
# get through the surface.  The floor only judges once a scan has run long
# enough for its rate to mean something, so a short scan against an
# unreachable floor must still come back as it would have without one.
f=$(image 16 rate.bin)
out=$(run --min-rate 1000000 "$f")
assert_hasnt "the throughput floor does not judge a scan before it settles" \
	"$out" "Too slow"
assert_has "--min-rate below zero is refused" \
	"$(run --min-rate -1 "$f" 2>&1)" "--min-rate must be"

# While running, the record carries the flag and every reader shows it.  The
# worker is stood in for by a sleep, so the record's process is really alive.
sleep 120 & sp=$!
spstart=$(sed 's/.*) //' /proc/$sp/stat | awk '{print $20}')
d=$HDDSCAN_STATE_DIR/runs/20200303-000000
mkdir -p "$d"
cat > "$d/run" <<EOF
run 1
id 20200303-000000
kind scan
what write (predeploy)
detail destructive write + verify
outdir $TMP
created $(( $(date +%s) - 600 ))
ended 0
sup $sp
supstart $spstart
njobs 1
EOF
cat > "$d/sdy.job" <<EOF
job 1
device sdy
path /dev/sdy
model MADE UP
size 1000000000000
state 1
verdict -1
pid $sp
pidstart $spstart
started $(( $(date +%s) - 600 ))
updated $(date +%s)
pct 0.1
rate 449545
tooslow 1
EOF
assert_has "a running drive under the floor says too slow" \
	"$(run --status 20200303-000000)" "too slow"
kill $sp 2>/dev/null; wait $sp 2>/dev/null
run --forget 20200303-000000 >/dev/null

# The verdict itself needs a scan that outlasts the settle time.  A 1 TiB
# sparse image drilled chunk by chunk takes that long, and an unreachable
# floor stands in for a drive that is genuinely too slow.  Drilling needs the
# default chunk: the budget floor's allowance for a bigger one keeps a sparse
# read under any budget.
truncate -s 1T "$TMP/rate.big"
out=$(run --chunk-slow-ms 0.001 --floor-ms 0 --retries 0 --max-time 130 \
	--min-rate 1000000 --json "$TMP/rate.json" "$TMP/rate.big"); rc=$?
assert_has "a drive under the floor is FAILING" "$out" "VERDICT: FAILING"
assert_has "the verdict says it was too slow, and by how much" "$out" \
	"below the"
assert_has "the report carries a Too slow line" "$out" "Too slow"
assert_eq "a drive under the floor exits 2" "$rc" "2"
assert_has "the JSON says too_slow" "$(cat "$TMP/rate.json")" '"too_slow": true'
rm -f "$TMP/rate.big"

echo "== the SAS logs the report is built from =="

# REGRESSION: smart_read() ran "smartctl -H -A -i", and on SAS the error
# counter and self-test logs are not in -A.  The parser was correct and simply
# never saw the data, so every real SAS drive reported "no sector counters were
# read from this drive" -- including one that had failed its own self-test.
#
# The C path needs a real device, so what is guarded here is the format the
# parser assumes, against output captured from such a drive.
fx=tests/fixtures/smartctl-sas-failing.txt
if [ -f "$fx" ] && grep -q -- "-l error -l selftest" hddscan.c; then
	ok "SCSI drives are asked for the error counter and self-test logs"
else
	bad "SCSI drives are asked for the error counter and self-test logs" \
		"smart_read() is not requesting the SAS log pages"
fi
if python3 - "$fx" <<'PY' 2>"$TMP/perr"
import sys
out = open(sys.argv[1]).read()
row = st = None; failed = False
for line in out.splitlines():
    if line.startswith("read:"):
        f = line[5:].split()
        if len(f) >= 7: row = f
    if line.startswith("#") and st is None:
        st = line[1:].lstrip("0123456789 ").strip()
        failed = "Failed" in line
assert row, "the read: row of the error counter log no longer parses"
assert row[1] == "42899520", "delayed corrections column moved: %r" % (row,)
assert row[5] == "535295.725", "gigabytes column moved: %r" % (row,)
assert row[6] == "0", "uncorrected column moved: %r" % (row,)
assert st and st.startswith("Background long"), "self-test line: %r" % st
assert failed, "a failed self-test is no longer recognised"
PY
then ok "the captured SAS logs still parse the way the report expects"
else bad "the captured SAS logs still parse the way the report expects" \
	"$(cat "$TMP/perr")"; fi

echo "== dependencies and non-interactive behaviour =="

out=$(run --check-deps); rc=$?
assert_eq "--check-deps exits 0" "$rc" "0"
for t in smartctl sdparm sg_modes hdparm; do
	assert_has "--check-deps mentions $t" "$out" "$t"
done

# REGRESSION: with the form as the default entry point, a piped invocation must
# still fail with advice rather than trying to open a UI.
out=$(run 2>&1 </dev/null); rc=$?
assert_has "no device and no terminal is an error, not a hang" "$out" "no device given"
assert_eq  "usage error exits 2" "$rc" "2"
assert_has "--no-tui with no device errors the same way" \
	"$(run --no-tui 2>&1 </dev/null)" "no device given"

printf '\n  %d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
