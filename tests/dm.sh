#!/bin/bash
# The device-mapper maps ('hddscan dm', also installed as dm-badblocks).  Everything runs against image files: no root, no
# device-mapper, nothing that can reach a real drive.  The kernel's half is
# stood in for by dm_read/dm_write below, which apply a table line by line
# with dd -- the same arithmetic dm-linear does, and enough to prove that data
# written through one table is still there through the next.
set -u
cd "$(dirname "$0")/.."
BIN=${BIN:-./hddscan dm}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0 FAIL=0

ok()  { PASS=$((PASS+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n     %s\n' "$1" "${2:-}"; }
assert_has()   { case "$2" in *"$3"*) ok "$1";; *) bad "$1" "expected: $3 in: $2";; esac; }
assert_hasnt() { case "$2" in *"$3"*) bad "$1" "should not contain: $3";; *) ok "$1";; esac; }
assert_eq()    { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "want [$3] got [$2]"; fi; }

MiB=1048576

# dm_read TABLE IMG OUT: assemble the virtual device into a file
dm_read() {
	: > "$3"
	while read -r ls len _ _ ps; do
		dd if="$2" of="$3" bs=512 skip="$ps" seek="$ls" count="$len" \
			conv=notrunc status=none
	done < "$1"
}
# dm_write TABLE IMG IN: write a virtual device image through the table
dm_write() {
	while read -r ls len _ _ ps; do
		dd if="$3" of="$2" bs=512 skip="$ls" seek="$ps" count="$len" \
			conv=notrunc status=none
	done < "$1"
}

# check_table TABLE NLOGICAL_EXTENTS BADLIST...: the table's own invariants
check_table() {
	local t=$1 n=$2; shift 2
	awk -v n="$n" -v bad="$*" -v spe="$SPARE_EVERY" -v next_="$NEXT" '
	BEGIN { split(bad, b, " "); for (i in b) isbad[b[i]] = 1; want = 0 }
	{
		if ($1 != want) { print "gap or overlap at " $1; err = 1 }
		want = $1 + $2
		for (e = $5 / 2048; e < ($5 + $2) / 2048; e++) {
			if (e in used) { print "extent " e " used twice"; err = 1 }
			used[e] = 1
			if (e in isbad) { print "bad extent " e " in the table"; err = 1 }
			if (e == 0 || e == next_ - 1) { print "metadata extent " e " in the table"; err = 1 }
		}
	}
	END {
		if (want != n * 2048) { print "covers " want " sectors, want " n * 2048; err = 1 }
		exit err
	}' "$t"
}

echo "== creating a map =="

img=$TMP/d.img
truncate -s 256M "$img"
NEXT=256
SPARE_EVERY=20                  # --reserve 5 on a small image
# bad blocks (1 KiB units): inside extents 7, 8 and 101, and extent 40, a spare
printf '%s\n' 7200 8500 8501 103424 40960 > "$TMP/bad.txt"

out=$($BIN create "$img" --bad "$TMP/bad.txt" --reserve 5 2>&1)
assert_has "create refuses without --confirm" "$out" "--confirm"
assert_eq  "and writes nothing" "$(od -An -c -N8 "$img" | tr -d ' ')" '\0\0\0\0\0\0\0\0'

out=$($BIN create "$img" --bad "$TMP/bad.txt" --reserve 5 --confirm "$img" 2>&1); rc=$?
assert_eq  "create succeeds" "$rc" "0"
assert_has "create names the device it will make" "$out" "bb-d.img"
assert_has "and says how to activate it" "$out" "hddscan dm activate"
assert_has "create counts the skipped extents" "$out" "3 bad extents"

# 256 extents - 2 metadata - 12 spare slots (20..240) - 3 skipped = 239
st=$($BIN status "$img")
assert_has "the virtual device is what is left" "$st" "(239 extents)"
assert_has "a bad spare slot is retired, not skipped" "$st" "12 total: 0 in use, 1 retired, 11 free"

$BIN table "$img" > "$TMP/t1"
if err=$(check_table "$TMP/t1" 239 7 8 101 40); then
	ok "the table is contiguous, never reuses an extent, and avoids damage"
else
	bad "the table is contiguous, never reuses an extent, and avoids damage" "$err"
fi
assert_eq "no spare is in the table before anything is remapped" \
	"$(awk '{for (e=$5/2048; e<($5+$2)/2048; e++) if (e%20==0) print e}' "$TMP/t1")" ""

out=$($BIN create "$img" --bad "$TMP/bad.txt" --confirm "$img" 2>&1)
assert_has "create will not replace an existing map" "$out" "already carries"

echo "== the map survives damage to one copy =="

cp "$img" "$TMP/a.img"
dd if=/dev/zero of="$TMP/a.img" bs=4096 count=1 conv=notrunc status=none
out=$($BIN status "$TMP/a.img" 2>&1)
assert_has "copy B is found when copy A is gone" "$out" "(239 extents)"
assert_has "and the damage is reported" "$out" "copy A is damaged"
cp "$img" "$TMP/b.img"
printf 'X' | dd of="$TMP/b.img" bs=1 seek=$((255 * MiB + 200)) conv=notrunc status=none
out=$($BIN status "$TMP/b.img" 2>&1)
assert_has "a flipped byte in copy B is caught by its checksum" "$out" "copy B is damaged"
dd if=/dev/zero of="$TMP/b.img" bs=4096 count=1 conv=notrunc status=none
out=$($BIN status "$TMP/b.img" 2>&1); rc=$?
assert_has "with both copies gone there is no map, rather than a wrong one" "$out" "no dm-badblocks map"
rm -f "$TMP/a.img" "$TMP/b.img"

echo "== damage found later is remapped, and the data comes with it =="

head -c $((239 * MiB)) /dev/urandom > "$TMP/data"
dm_write "$TMP/t1" "$img" "$TMP/data"
dm_read "$TMP/t1" "$img" "$TMP/r1"
assert_eq "data written through the table reads back" \
	"$(md5sum < "$TMP/r1")" "$(md5sum < "$TMP/data")"

# physical extent 50 is logical 45: 0 is the map, 7 and 8 were bad, 20 and 40
# are spare slots
assert_has "map translates virtual to physical" \
	"$($BIN map "$img" $((45 * MiB + 123)))" "physical $((50 * MiB + 123))"

echo 51200 > "$TMP/late.txt"           # 1 KiB block 51200 = extent 50
out=$($BIN remap "$img" --bad "$TMP/late.txt" 2>&1); rc=$?
assert_eq  "remap succeeds" "$rc" "0"
assert_has "it goes to the nearest spare" "$out" "from 50 to spare 60"
$BIN table "$img" > "$TMP/t2"
# the old home can now rot without anyone noticing
dd if=/dev/urandom of="$img" bs=$MiB seek=50 count=1 conv=notrunc status=none
dm_read "$TMP/t2" "$img" "$TMP/r2"
assert_eq "the remapped extent kept its data" \
	"$(md5sum < "$TMP/r2")" "$(md5sum < "$TMP/data")"
assert_eq "the table is still exactly the virtual device's size" \
	"$(awk '{s+=$2} END {print s}' "$TMP/t2")" "$((239 * 2048))"
assert_has "status counts the spare in use" "$($BIN status "$img")" "1 in use"

out=$($BIN remap "$img" --bad "$TMP/late.txt" 2>&1)
assert_has "remapping the same damage twice changes nothing" "$out" "nothing to do"

# the spare itself going bad moves the data again
echo $((60 * 1024)) > "$TMP/spare.txt"
out=$($BIN remap "$img" --bad "$TMP/spare.txt" 2>&1)
assert_has "a failing spare in use is moved to the next nearest" "$out" "from 60 to spare 80"
$BIN table "$img" > "$TMP/t3"
dd if=/dev/urandom of="$img" bs=$MiB seek=60 count=1 conv=notrunc status=none
dm_read "$TMP/t3" "$img" "$TMP/r3"
assert_eq "and the data survives the second move" \
	"$(md5sum < "$TMP/r3")" "$(md5sum < "$TMP/data")"

# damage reported in virtual-device terms, as the kernel log would
echo $((200 * 1024)) > "$TMP/logical.txt"
p=$($BIN map "$img" $((200 * MiB)) | awk '{print $NF}')
out=$($BIN remap "$img" --logical --bad "$TMP/logical.txt" 2>&1)
assert_has "--logical remaps by virtual offset" "$out" "remapped logical extent 200"
$BIN table "$img" > "$TMP/t4"
dd if=/dev/urandom of="$img" bs=512 seek=$((p / 512)) count=2048 conv=notrunc status=none
dm_read "$TMP/t4" "$img" "$TMP/r4"
assert_eq "and keeps its data" "$(md5sum < "$TMP/r4")" "$(md5sum < "$TMP/data")"
if err=$(check_table "$TMP/t4" 239 7 8 101 40 50 60 $((p / MiB))); then
	ok "after three remaps the table still avoids every bad extent"
else
	bad "after three remaps the table still avoids every bad extent" "$err"
fi

echo "== running out of spares =="

img2=$TMP/small.img
truncate -s 64M "$img2"
$BIN create "$img2" --reserve 5 --confirm "$img2" >/dev/null 2>&1   # 3 spares
seq 1 5 | awk '{print ($1 * 3 + 1) * 1024}' > "$TMP/many.txt"        # extents 4,7,10,13,16
out=$($BIN remap "$img2" --bad "$TMP/many.txt" 2>&1); rc=$?
assert_has "remap says when the spares are gone" "$out" "no free spare left"
assert_eq  "and exits non-zero" "$rc" "1"
assert_has "the remaps that did fit were kept" "$($BIN status "$img2")" "3 in use"

echo "== refusals =="

truncate -s 64M "$TMP/m.img"
echo 10 > "$TMP/head.txt"
out=$($BIN create "$TMP/m.img" --bad "$TMP/head.txt" --confirm "$TMP/m.img" 2>&1)
assert_has "damage where the map would live is refused" "$out" "first or last"
out=$($BIN activate "$img" 2>&1)
assert_has "activate on an image says to use a loop device" "$out" "losetup"
out=$($BIN create "$TMP/m.img" --name 'x;rm' --confirm "$TMP/m.img" 2>&1)
assert_has "a name that would reach the shell is refused" "$out" "--name may only"

echo "== one binary =="

# REGRESSION: this linked "$PWD/hddscan" whatever $BIN named, which exists in
# a tree that has run make and nowhere else -- the release job tests the
# binary unpacked from its tarball, found the link dangling, and cancelled
# the release.  Link the binary under test.
ln -s "$(realpath "${BIN%% *}")" "$TMP/dm-badblocks"
assert_has "a link named dm-badblocks runs the maps" \
	"$("$TMP/dm-badblocks" status "$img" 2>&1)" "(239 extents)"
assert_has "and names itself that way in its advice" \
	"$("$TMP/dm-badblocks" --help 2>&1)" "dm-badblocks activate-all"

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
