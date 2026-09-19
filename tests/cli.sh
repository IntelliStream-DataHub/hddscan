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

# predeploy leaves a drive configured for service.  An image has no mode
# pages, so what is asserted is the plan --dry-run states, not sdparm's work.
out=$(runp --dry-run --confirm "$f" "$f")
assert_has "predeploy scans in 1M chunks" "$out" "1024 KiB"
assert_has "predeploy saves the recommended configuration" "$out" \
	"would save the recommended configuration"
assert_has "predeploy enables the background scan" "$out" \
	"would enable the background scan, every 168 hours"
assert_has "predeploy turns the write cache off for the run" "$out" \
	"would turn the write cache off for the run"
assert_hasnt "--no-fix-config opts out of the saved configuration" \
	"$(runp --dry-run --no-fix-config --confirm "$f" "$f")" \
	"would save the recommended"
assert_hasnt "--bms keep opts out of the background scan" \
	"$(runp --dry-run --bms keep --confirm "$f" "$f")" \
	"would enable the background scan"
# A read pass under the default profile may be of a drive in service; a
# default must never rewrite its configuration.
assert_hasnt "predeploy's saved settings do not follow an overridden mode" \
	"$(runp --dry-run --mode read "$f")" "would save"
assert_hasnt "inservice carries no drive settings" \
	"$(runp --dry-run --profile inservice "$f")" "drive settings not applied"
assert_has "--bms-interval without --bms on is refused" \
	"$(runp --profile inservice --bms-interval 24 "$f" 2>&1)" \
	"--bms-interval needs --bms on"

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

# A scan that takes three days is over long before anyone knows the block size
# and the partition offset the filesystem will want, so the list has to be
# buildable again afterwards out of what the scan wrote down.
run --mode check --csv "$TMP/bb.csv" --state "$TMP/bb.ckpt" "$f" >/dev/null
run --badblocks-from "$TMP/bb.ckpt" --badblocks-list "$TMP/bb3.txt" >/dev/null
assert_eq  "--badblocks-from rebuilds the list from a checkpoint" \
	"$(tr '\n' ' ' < "$TMP/bb3.txt")" "8192 8193 8194 8195 "
run --badblocks-from "$TMP/bb.csv" --badblocks-list "$TMP/bb4.txt" \
	--badblocks-offset 1M >/dev/null
assert_eq  "--badblocks-from reads a --csv too, and shifts it" \
	"$(tr '\n' ' ' < "$TMP/bb4.txt")" "7168 7169 7170 7171 "
run --badblocks-from "$TMP/bb.ckpt" --badblocks-list "$TMP/bb5.txt" \
	--badblocks-blocksize 4096 >/dev/null
assert_eq  "--badblocks-from in the filesystem's own block size" \
	"$(tr '\n' ' ' < "$TMP/bb5.txt")" "2048 "
assert_has "--badblocks-from with nowhere to write it is refused" \
	"$(run --badblocks-from "$TMP/bb.ckpt")" "needs --badblocks-list"
assert_has "the report says how to use the drive anyway" \
	"$out" "Using this drive anyway"
assert_has "the report gives a mkfs line for it" "$out" "mkfs.ext4 -b 4096"
assert_has "the report warns the drive off arrays and pools" "$out" \
	"mdraid array or a"

echo "== hiding the damage behind device-mapper =="

# The map is written for real -- it is this binary's own code -- and read back
# with 'hddscan dm'.  Only activation needs device-mapper, and an image stops
# short of it and says so.
hide() { $HDDSCAN --no-color --outdir "$TMP" "$@" 2>&1; }

f=$(patterned 16 hide.bin)
printf 'BADBADBAD' | dd of="$f" bs=1 seek=8388608 conv=notrunc status=none
assert_has "--hide-bad needs --confirm like a write" \
	"$(hide --hide-bad "$f")" "needs --confirm"
out=$(hide --hide-bad --dry-run --confirm "$f" "$f")
assert_has "--dry-run says what it would do" "$out" "would write a map"
assert_has "and writes nothing" "$($HDDSCAN dm status "$f" 2>&1)" \
	"no dm-badblocks map"
# the write pass that patterned it saw a clean drive, and that is the
# newest whole scan until the check below
run --mode check "$f" >/dev/null
out=$(hide --hide-bad --confirm "$f" "$f"); rc=$?
assert_eq  "--hide-bad writes the map and exits 0" "$rc" "0"
st=$($HDDSCAN dm status "$f" 2>&1)
assert_has "the map skips exactly the extent the scan found bad" "$st" \
	"1 extent bad when the map was made"
assert_has "and is named for the drive" "$st" "bb-hide.bin"
assert_has "the extent it skips is the damaged one" \
	"$($HDDSCAN dm map "$f" 8388608 --physical)" "not part of the virtual device"
assert_has "an image stops short of activating, and says why" "$out" \
	"needs a loop device"
assert_has "running it twice will not replace the map" \
	"$(hide --hide-bad --confirm "$f" "$f")" "already carries"

f=$(image 16 hide2.bin)
run --sample 64 "$f" >/dev/null
out=$(hide --hide-bad --confirm "$f" "$f"); rc=$?
assert_has "a drive whose only scan was sampled is refused" "$out" \
	"no finished scan of the whole drive"
assert_has "and no map is written" "$($HDDSCAN dm status "$f" 2>&1)" \
	"no dm-badblocks map"
assert_eq  "and it exits 2" "$rc" "2"
f=$(patterned 16 hide4.bin)
printf 'BAD' | dd of="$f" bs=1 seek=4096 conv=notrunc status=none
assert_has "the report offers it for filesystems without a bad-block list" \
	"$(run --mode check "$f")" "--hide-bad --confirm"

# REGRESSION: the pattern was seeded at each chunk's start, so what was on
# the platter depended on the chunk size it was written with.  Predeploy
# moving to 1M chunks while decay stayed at 128K would have made every decay
# run call a healthy drive FAILING.  12 KiB divides neither the unit nor 1M.
f=$(image 16 chunkpat.bin)
run --mode write --chunk 1M --confirm "$f" "$f" >/dev/null
cp=0
for c in 128K 4M 12K; do
	n=$(run --mode check --chunk "$c" "$f" | awk '/wrong data/{print $5}')
	[ "$n" = "0" ] || { cp=1; echo "     checked in $c: $n bad blocks"; }
done
[ "$cp" = 0 ] && ok "a pattern reads back whatever chunk size wrote it" \
              || bad "a pattern reads back whatever chunk size wrote it" \
                     "see above"

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

echo "== over budget now and then is not a verdict =="

# A chunk over budget whose sectors all drill clean is the drive having a
# moment, and every drive has those.  One in a thousand is the line.  The
# counts that decide it are loaded from a checkpoint written by hand, resumed
# at the last chunk, so the test does not depend on how fast this machine is.
# That last chunk is still read for real, cold, from a file dd has only just
# written, and on a slow runner that can go over an auto-calibrated budget --
# a 6 where the test wants 5.  A budget of a minute leaves the counts to the
# checkpoint alone.
f=$(image 32 over.bin)
ckpt() {
	printf 'hddscan-state 2\npos %s\nstep 255\nbytes 13090422784\n' \
		$((255 * 131072)) > "$TMP/over.ckpt"
	printf 'counters 100000 %s 0 0 0 0 0 0 0\nwrites %s\n' \
		"$1" "$2" >> "$TMP/over.ckpt"
	run --chunk-slow-ms 60000 --sector-slow-ms 60000 \
		--state "$TMP/over.ckpt" --resume "$f"
}
out=$(ckpt 5 "0 0")
assert_has "a few chunks over budget in 100000 leave a drive HEALTHY" \
	"$out" "VERDICT: HEALTHY"
assert_has "a HEALTHY verdict still says what went over budget" \
	"$out" "5 chunk reads went over it"
assert_has "more than one chunk in a thousand over budget is SUSPECT" \
	"$(ckpt 500 "0 0")" "500 of 100001 chunk reads were over the latency budget"
assert_has "more than one write in a thousand over budget is SUSPECT" \
	"$(ckpt 0 "100000 500")" "500 of 100000 chunk writes took longer"
# REGRESSION: any single chunk over budget used to make a drive SUSPECT, which
# lowering the budget to 3x the median would have turned into noise.  Every
# chunk over budget is still a habit, not a moment.
assert_has "every chunk over budget is SUSPECT even with clean sectors" \
	"$(run --chunk-slow-ms 0.001 --sector-slow-ms 1000 --floor-ms 0 "$f")" \
	"256 of 256 chunk reads"
out=$(runp --profile predeploy --confirm "$f" --json "$TMP/w.json" "$f")
assert_has "a write scan reports its writes over budget" "$out" \
	"writes over budget"
# REGRESSION: this asserted 0, which is a claim about how fast a runner
# writes, not about the JSON -- and a write on aarch64 CI went over budget.
assert_eq "the JSON counts writes over budget" \
	"$(grep -cE '"writes_over_budget": [0-9]+,' "$TMP/w.json")" "1"

echo "== a stretch of the surface far slower than expected =="

# A band reading far slower than the gradient calibration measured is a head
# or a scratch, even with every sector inside its budget.  An image file has
# no slow stretch to find, so the band's figures come from a checkpoint --
# which is also where a resumed scan gets its surface map back from.
f=$(image 32 band.bin)
bandck() {
	printf 'hddscan-state 2\npos %s\nstep 255\nbytes 13090422784\n' \
		$((255 * 131072)) > "$TMP/band.ckpt"
	printf 'counters 100000 0 0 0 0 0 0 0 0\n' >> "$TMP/band.ckpt"
	printf 'band 200 64 0 0 0 %s 0 8388608\n' "$1" >> "$TMP/band.ckpt"
	run --state "$TMP/band.ckpt" --resume --json "$TMP/band.json" "$f"
}
# REGRESSION: the map's shading is relative, and the band had 100 ms a chunk
# against the one chunk the resume reads for real.  A cold O_DIRECT read of a
# file dd has only just written can take longer than that on a slow runner,
# and on aarch64 CI it did: the fresh chunk became the '@' and the band from
# the checkpoint a '.'.  Ten seconds a chunk is beyond any read that returns.
out=$(bandck 640000000)
assert_has "a band far slower than calibration expects is SUSPECT" \
	"$out" "VERDICT: SUSPECT - 1 band of the surface"
assert_has "the report says where the slow band is" "$out" "worst"
assert_has "the JSON counts slow bands" "$(cat "$TMP/band.json")" \
	'"slow_bands": 1'
assert_has "a resumed scan's surface map keeps the bands scanned before" \
	"$out" "_@_"
# microseconds over an image's own calibration are noise, not a factor of two
assert_has "a band only microseconds over expectations is not held against it" \
	"$(bandck 3200)" "VERDICT: HEALTHY"

echo "== what the kernel logged against the drive =="

# A drive that stops answering is aborted, reset and retried by the kernel,
# and when a retry works the scan sees one slow read and nothing else.  The
# fixture is the log of a real drive doing that mid-scan; HDDSCAN_KMSG reads it
# in place of /dev/kmsg, and HDDSCAN_KMSG_HCTL gives the image the address the
# log's lines name the drive by.
f=$(image 8 kmsg.bin)
fx=$PWD/tests/fixtures/kmsg-sas-resets.txt
out=$(HDDSCAN_KMSG=$fx HDDSCAN_KMSG_HCTL=6:0:2:0 run --json "$TMP/k.json" "$f")
assert_has "kernel resets and timeouts against the drive are counted" \
	"$out" "4 resets, 5 command timeouts"
assert_has "a drive the kernel had to reset repeatedly is FAILING" \
	"$out" "VERDICT: FAILING - the kernel had to reset the drive 4 times"
assert_has "the JSON carries the kernel log counts" "$(cat "$TMP/k.json")" \
	'"resets": 4, "timeouts": 5'
out=$(HDDSCAN_KMSG=$fx HDDSCAN_KMSG_HCTL=6:0:3:0 run "$f")
assert_has "another drive's trouble in the same log is not held against it" \
	"$out" "0 resets, 0 command timeouts"
assert_has "a drive with nothing logged against it stays HEALTHY" \
	"$out" "VERDICT: HEALTHY"
# ATA names a drive by port, and ata51 is not ata5
printf '%s\n' "ata51: hard resetting link" "ata51: hard resetting link" \
	"ata51: hard resetting link" \
	"ata5.00: exception Emask 0x0 SAct 0x0 SErr 0x0 action 0x6 frozen" \
	"ata5.00: error: { UNC }" > "$TMP/ata.log"
out=$(HDDSCAN_KMSG=$TMP/ata.log HDDSCAN_KMSG_HCTL=ata5 run "$f")
assert_has "ATA lines are matched by port, and not by a longer port number" \
	"$out" "0 resets, 1 command timeouts, 1 medium errors"
assert_has "one timeout is SUSPECT, not FAILING" "$out" "VERDICT: SUSPECT"
assert_hasnt "an image with no log to read says nothing about one" \
	"$(run "$f")" "Kernel log"

echo "== SMART counters that move during the scan =="

# A scan reads SMART before and after.  HDDSCAN_SMARTCTL runs a stand-in that
# serves one captured report per call, so the verdict can be tested against
# counters that move -- which needs a drive going wrong, and no image does.
f=$(image 8 smart.bin)
cat > "$TMP/smartctl" <<'EOF'
#!/bin/sh
d=$(dirname "$0"); n=$(cat "$d/smart.n" 2>/dev/null || echo 0)
n=$((n + 1)); echo $n > "$d/smart.n"
if [ -f "$d/smart.$n" ]; then cat "$d/smart.$n"; else cat "$d/smart.2"; fi
EOF
chmod +x "$TMP/smartctl"
# ata REALLOC SPIN_RETRY END_TO_END REPORTED_UNCORRECT COMMAND_TIMEOUT
ata() {
	printf 'SMART overall-health self-assessment test result: PASSED\n'
	printf '%3d %s 0x0033 100 100 010 Pre-fail Always - %s\n' \
		5 Reallocated_Sector_Ct "$1" 10 Spin_Retry_Count "$2" \
		184 End-to-End_Error "$3" 187 Reported_Uncorrect "$4" \
		188 Command_Timeout "$5"
}
# sas GROWN_DEFECTS READ_DELAYED READ_GB READ_UNCORRECTED
sas() {
	printf 'SMART Health Status: OK\n\nElements in grown defect list: %s\n\n' "$1"
	printf 'Error counter log:\n'
	printf 'read:          0 %s         0  1007   1007     %s           %s\n' \
		"$2" "$3" "$4"
	printf 'write:         0        0         0         0     103400     233851.148           0\n'
}
smart() {
	rm -f "$TMP/smart.n"
	HDDSCAN_SMARTCTL=$TMP/smartctl run "$f"
}
ata 0 0 0 0 0 > "$TMP/smart.1"; ata 0 0 0 0 0 > "$TMP/smart.2"
assert_has "SMART counters that do not move leave a drive HEALTHY" \
	"$(smart)" "VERDICT: HEALTHY"
ata 0 0 0 0 0 > "$TMP/smart.1"; ata 0 0 0 0 2 > "$TMP/smart.2"
assert_has "command timeouts (188) growing during the scan are SUSPECT" \
	"$(smart)" "VERDICT: SUSPECT - the drive counted 2 command timeouts"
ata 0 0 0 0 0 > "$TMP/smart.1"; ata 0 0 0 1 0 > "$TMP/smart.2"
# REGRESSION: uncorrectable errors growing used to be FAILING outright, but
# every drive grows some and a couple of hundred can be a drive with years left
assert_has "reported uncorrectable (187) growing during the scan is SUSPECT" \
	"$(smart)" "VERDICT: SUSPECT - the drive's own counters recorded 1 uncorrectable"
ata 0 0 0 0 0 > "$TMP/smart.1"; ata 0 0 4 0 0 > "$TMP/smart.2"
assert_has "end-to-end errors (184) growing during the scan are FAILING" \
	"$(smart)" "VERDICT: FAILING - the drive recorded 4 end-to-end errors"
ata 0 3 0 0 0 > "$TMP/smart.1"; ata 0 3 0 0 0 > "$TMP/smart.2"
out=$(smart)
assert_has "a drive that has needed spin-up retries (10) is SUSPECT" \
	"$out" "retries to spin up"
assert_has "the report lists spin retries" "$out" "spin retries (10)"
sas 1 1007 1000.000 0 > "$TMP/smart.1"; sas 1 1007 1010.000 0 > "$TMP/smart.2"
assert_has "a SAS drive whose counters stay put is HEALTHY" \
	"$(smart)" "VERDICT: HEALTHY"
sas 1 1007 1000.000 0 > "$TMP/smart.1"; sas 3 1007 1010.000 0 > "$TMP/smart.2"
out=$(smart)
assert_has "a SAS grown defect list growing during the scan is SUSPECT" \
	"$out" "the drive reallocated during the scan"
assert_has "the report shows the grown defect list" "$out" "grown defect list"
sas 1 1007 1000.000 0 > "$TMP/smart.1"; sas 1 1107 1010.000 0 > "$TMP/smart.2"
assert_has "SAS delayed corrections at one or more per GB read are SUSPECT" \
	"$(smart)" "delayed corrections or rereads 100 times during the scan"
sas 1 1007 1000.000 0 > "$TMP/smart.1"; sas 1 1012 1010.000 0 > "$TMP/smart.2"
assert_has "a few SAS delayed corrections are not held against a drive" \
	"$(smart)" "VERDICT: HEALTHY"
sas 1 1007 1000.000 0 > "$TMP/smart.1"; sas 1 1007 1010.000 2 > "$TMP/smart.2"
assert_has "SAS uncorrected read errors growing during the scan are SUSPECT" \
	"$(smart)" "VERDICT: SUSPECT - the drive's own counters recorded 2 uncorrectable"

echo "== unreadable is not unrepairable =="

# Every drive grows uncorrectable sectors, and writing them is what makes the
# firmware remap them.  A drive is failing when the write did not help.  An
# image cannot return EIO, so the counts come from a checkpoint.
f=$(image 32 unrep.bin)
unrep() {
	printf 'hddscan-state 2\npos %s\nstep 255\nbytes 13090422784\n' \
		$((255 * 131072)) > "$TMP/unrep.ckpt"
	printf 'counters 100000 0 0 0 0 %s 0 0 0\nunrepaired %s\n' \
		"$1" "$2" >> "$TMP/unrep.ckpt"
	run --state "$TMP/unrep.ckpt" --resume --json "$TMP/unrep.json" "$f"
}
out=$(unrep 200 0)
# REGRESSION: a single unreadable sector made a drive FAILING, even when the
# write that followed let the drive remap it.
assert_has "200 unreadable sectors nobody has rewritten are SUSPECT" \
	"$out" "VERDICT: SUSPECT - 200 unreadable sectors"
assert_has "a read-only scan says a write pass will settle it" \
	"$out" "Run a write pass"
out=$(unrep 200 1)
assert_has "a sector still unreadable after being rewritten is FAILING" \
	"$out" "VERDICT: FAILING - 1 sector stayed unreadable after being rewritten"
assert_has "the JSON counts unrepaired sectors" "$(cat "$TMP/unrep.json")" \
	'"sectors_unrepaired": 1'

echo "== a drive that disappears mid-scan =="

# A drive that drops off the bus fails every read from then on, and a scan that
# took each failure at face value reported millions of bad sectors for one
# event.  Truncating an image mid-scan is the same thing seen from a file: its
# capacity collapses under the scan.
truncate -s 1T "$TMP/gone.img"
run --chunk-slow-ms 0.001 --floor-ms 0 --retries 0 --max-time 60 \
	--json "$TMP/gone.json" "$TMP/gone.img" > "$TMP/gone.out" 2>&1 &
gp=$!
for _ in $(seq 1 100); do
	grep -q "testing" "$TMP/gone.out" 2>/dev/null && break
	sleep 0.1
done
sleep 1
truncate -s 100M "$TMP/gone.img"
wait $gp; rc=$?
out=$(cat "$TMP/gone.out")
assert_has "a drive that disappears mid-scan is FAILING" "$out" \
	"VERDICT: FAILING - the drive disappeared from the system"
assert_has "the report says how it disappeared" "$out" "its capacity collapsed"
assert_eq "a drive that disappears exits 2" "$rc" "2"
assert_has "its reads after it went are not counted as bad sectors" \
	"$(cat "$TMP/gone.json")" '"sectors_bad": 0'
rm -f "$TMP/gone.img"

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

# The verdict itself needs a scan that outlasts the settle time, and an
# unreachable floor stands in for a drive that is genuinely too slow.
# Drilling needs the default chunk: the budget floor's allowance for a bigger
# one keeps a sparse read under any budget.
#
# REGRESSION: the image was 1 TiB, which drilled chunk by chunk took about
# three minutes on the machine this was written on -- and under two on a
# faster CI runner, which then finished before the floor was allowed to
# judge and reported SUSPECT for the chunks over budget alone.  8 TiB, still
# under ext4's 16, would need 68 GiB/s to finish inside the settle time.
truncate -s 8T "$TMP/rate.big"
out=$(run --chunk-slow-ms 0.001 --floor-ms 0 --retries 0 --max-time 130 \
	--min-rate 1000000 --json "$TMP/rate.json" "$TMP/rate.big"); rc=$?
assert_has "a drive under the floor is FAILING" "$out" "VERDICT: FAILING"
assert_has "the verdict says it was too slow, and by how much" "$out" \
	"below the"
assert_has "the report carries a Too slow line" "$out" "Too slow"
assert_eq "a drive under the floor exits 2" "$rc" "2"
assert_has "the JSON says too_slow" "$(cat "$TMP/rate.json")" '"too_slow": true'
rm -f "$TMP/rate.big"

# A stretch that crawled is judged over a trailing ten-minute window, which no
# test can wait for, so it arrives the way a resumed scan would carry it: in
# the checkpoint.
f=$(image 32 stretch.bin)
printf 'hddscan-state 2\npos %s\nstep 255\nbytes 13090422784\n' \
	$((255 * 131072)) > "$TMP/stretch.ckpt"
printf 'counters 100000 0 0 0 0 0 0 0 0\nslowstretch 840 200000 3000000000\n' \
	>> "$TMP/stretch.ckpt"
out=$(run --state "$TMP/stretch.ckpt" --resume --json "$TMP/stretch.json" "$f")
assert_has "a stretch under the floor is SUSPECT when the whole scan is not" \
	"$out" "VERDICT: SUSPECT - for 14m 00s of the scan"
assert_has "the report says how long and where it was slow" "$out" "Slow stretch"
assert_has "the JSON carries the slow stretch" "$(cat "$TMP/stretch.json")" \
	'"slow_stretch_s": 840'
assert_has "--min-rate 0 turns the slow stretch off too" \
	"$(run --min-rate 0 --state "$TMP/stretch.ckpt" --resume "$f")" \
	"VERDICT: HEALTHY"

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
