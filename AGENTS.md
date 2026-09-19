# AGENTS.md

Guidance for AI agents working on `hddscan`. `README.md` is the short
introduction and `DESIGN.md` is why it works the way it does — read those
first. This file covers how to change it without breaking it.

## What this is

A single-file C program (`hddscan.c`, ~10000 lines) that scans rotating disks
for sectors that return data too slowly or not at all, and keeps a record of
every run so the work survives the terminal that started it. Build with `make`.

**No dependencies beyond libc.** This is deliberate: the binary has to work on
a rescue system with no packages. The TUI is raw ANSI plus termios rather than
ncurses for that reason. `smartctl`, `hdparm` and sg3-utils are shelled out to
at runtime when present and degraded gracefully when absent — never linked.

Keep it that way. No glibc-only APIs: no `asprintf`, `getline`, `qsort_r`,
`alloca`. Include the header a function actually belongs to rather than relying
on glibc leaking it — `strcasecmp` needs `<strings.h>` even though `<string.h>`
happens to pull it in under `_GNU_SOURCE`.

Note that `_GNU_SOURCE` (required for `O_DIRECT` and `statx`) also enables
glibc's C23 `strtol`/`sscanf` redirects, so the **dynamic** build carries a
hard glibc >= 2.38 requirement no matter what the source does. `make static`
is the answer for anything portable; verify with
`nm -D --undefined-only hddscan | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1`.

```sh
make            # -O2 -Wall -Wextra -Wshadow -Wformat=2, must stay warning-free
make clean
```

## The invariants

These are load-bearing. Breaking any of them makes the tool quietly lie about
drive health, which is worse than it crashing. Do not change them without
understanding the argument.

**1. The chunk budget must never exceed the sector budget.** The tool's whole
claim rests on "a chunk is never faster than its slowest sector", so a chunk
that meets its budget proves every sector inside it did too. If the chunk
budget were looser than the sector budget, a slow sector could sit inside a
chunk that never gets drilled into. `scan_device()` clamps this and warns; the
clamp is not optional.

**1b. The budget follows the platter, and must never follow the media.** An
outer track reads about twice as fast as an inner one, so a flat budget is
loose outside and tight inside; `thr_at()` interpolates between the eight
calibration anchors to keep the same margin everywhere. It is deliberately
*not* learned from the chunks being scanned. A budget that adapted to what it
was reading would raise itself over a bad region and normalise away the fault
the scan exists to find. The anchors are measured once, before the scan, and
the gradient between them is smooth because it is geometry. An explicit
`--chunk-slow-ms` or `--sector-slow-ms` disables it entirely: a threshold given
by hand means that number, not a number this tool then bends. The invariant 1
clamp is applied pointwise inside `chunk_thr_at()`, not once globally.

**2. Every scan order must visit every chunk exactly once.** `order_index()`
returns a full-cycle permutation; the random orders rely on `coprime_stride()`
producing a stride coprime with the segment length. A stride that shares a
factor with the length silently skips sectors while still reporting a clean
scan. Any change here must be re-proved by the coverage test below, not by
inspection.

**3. Neither `EINVAL` nor `EILSEQ` from a read is a media error.** `EINVAL`
means the kernel rejected a misaligned direct read — a geometry bug in this
tool, not a dying drive. `EILSEQ` is the block layer's `BLK_STS_PROTECTION`:
the drive read the block and its own T10 guard tag did not match, which is
what *every* block never written since a PI format looks like. They are
counted separately as `align_errors` and `prot_errors` and must never
contribute to `blocks_bad` or the verdict. Misreporting either condemns
healthy hardware, which is exactly what happened both times before it was
fixed — `EILSEQ` was found doing it to a PI-formatted drive whose platters are
fine.

The corollary is invariant 9's: blocks skipped this way were **not tested**,
so the report carries a `Not covered` line and the verdict box says outright
that it covers nothing about them. Silently skipping is as much a lie as
silently condemning.

**4. `--mode verify` must restore the original bytes.** It reads a chunk, saves
it, writes a pattern, verifies, then writes the original back. The signal
handler also restores a chunk that was in flight. Any change to the write path
must be followed by the md5 round-trip test below.

**5. Never parallelise within one drive.** One worker process per drive, one
outstanding request per worker. A second concurrent request on the same spindle
makes the head seek between them and turns a latency measurement into a
queueing measurement. Parallelism across drives is fine and is the default.

**6. Calibration must use the access pattern the scan will use.** Timing
scattered reads and then scanning sequentially yields a budget inflated by a
seek — roughly 10x on real hardware — which is loose enough to miss real
faults. See `calibrate()`.

**7. Restore anything you change on the system.** The kernel per-device
timeout (`timeout_restore`), the drive's read look-ahead
(`lookahead_restore`) and its write cache (`wc_restore`) are all registered
with `atexit` *and* handled on the hard-stop signal path. A new knob must do both, and must register its `atexit`
handler only once — `atexit` slots are a small fixed resource and a sequential
scan of many drives would otherwise exhaust them.

**7b. Settings only outlive the process when asked, and every such change
goes through `persist_ok()`.** Three things deliberately survive the scan:
`--bms on`, because a background scan that stopped when the scan did would be
pointless; `--fix-config`, because a configuration fix that reverted would not
be a fix; and `--persist`, which saves the settings a run changed so a drive
can be configured once and left. All of them announce what they did on stderr
and print the exact command that undoes it. Do not "fix" them by adding a
restore handler.

The first two are defaults of the **predeploy** and **repair** profiles (with
`--bms-interval 168`), and that is the only place a saved setting is not typed
by hand. A drive being prepared for service is the moment someone is
deliberately configuring it, and a run there has already been confirmed as a
destructive one. The boundaries are load-bearing: the profile's drive settings
apply only while its own mode stands (`--mode read` under predeploy may be a
drive in service, and saves nothing), never under `--apply-settings` or
`--format`, never under `--dry-run`, and never through `--persist`, which saves
only what was named. `--persist` stays opt-in and is never a profile default.

All of it is applied by `apply_settings()` in the process that owns the run —
the supervisor `run_begin()` forks — before the scan starts, and never inside
each worker as it reaches its drive: a setting should land when the user asks
for it, `--apply-settings` needs to work with no scan at all (and so still
applies in `main()`, since it has no run), and the run-scoped restore list then
lives in the one process that catches `SIGINT` for the whole run, so an
interrupt puts every drive back rather than only the ones whose workers got the
signal. It must not live in a viewer: see invariant 12. That list handles both backends (`RS_SDPARM`,
`RS_HDPARM_WC`) through one `atexit` handler; do not go back to a pair of
globals per knob, which silently only remembered the last drive.

Three rules keep that from becoming a trap, and all three are load-bearing:

- **Saving is a SCSI mode page or it does not happen.** `hdparm` has no
  equivalent for ATA. `persist_ok()` refuses, says so, and falls back to
  run-scoped rather than letting anyone believe a setting stuck when it dies
  at the next power cycle.
- **Only settings the user named are saved.** `--drive-lookahead off` is this
  tool's *default*, so `--persist` alone must never save it — that would leave
  every drive it ever touched permanently slower at sequential reads. Hence
  `o.lookahead_set`, and hence the TUI comparing against `la_init` rather than
  trusting the field's current value.
- **The form asks twice.** `--persist` arms the same `s`-then-`y` confirmation
  that write modes get, because the interactive form is the default entry
  point and a permanent change to someone's hardware should not be one
  keystroke away.

**8. Never write to a zoned drive without checking `queue/zoned`.** Host-managed
SMR zones can only be written sequentially from their start. A write pass would
fail on nearly every request and report a healthy drive as totally dead. Write
modes are refused on host-managed drives, and `--order random` is refused with
any write mode on any zoned drive.

**9. A scan whose guarantee does not hold must say so.** If the drive's
look-ahead could not be disabled and the order is sequential, prefetch may have
masked a marginal sector, so both stderr and the report state that plainly.
Never let a caveated scan render as an unqualified HEALTHY.

**10. The verify journal needs exactly one fsync per chunk, before the pattern
write.** Replay is idempotent — rewriting the original bytes over an
already-restored chunk is a no-op — which is why finishing cleanly needs no
second fsync. Do not "optimise" the fsync away; it is the entire guarantee.

**The default profile writes.** `predeploy` is the shipped default because a
surface test is normally run before a drive enters service, and a read-only
pass cannot tell you whether a sector will accept a write. The consequence is
that a bare invocation dies asking for `--confirm` — that is the intended
behaviour, not a regression. Do not "fix" it by defaulting to `inservice`, and
do not give the CLI and the form different defaults: a report has to be
comparable with another report, which is why every report names its profile.

## The test suite

`make test` runs all three. Everything runs against regular files used as
images: no root, no spinning disk, and nothing in it can reach `/dev/sd*`.

- `tests/cli.sh` — scan correctness, coverage honesty, exit codes, argument
  validation, output formats, checkpoint/resume, the run store, and the
  printed advice.
- `tests/dm.sh` — the device-mapper maps (`hddscan dm`): the table's own
  invariants, the map surviving damage to one copy, and data written through
  one table reading back through the next after remaps. The kernel's half is
  stood in for by `dd` applying each table line, the same arithmetic dm-linear
  does.
- `tests/tui.py` — the form, the dashboard and the list of runs, driven
  through a pty.

Both suites export `HDDSCAN_STATE_DIR` into a temporary directory of their own.
Every scan is recorded as a run now, so a suite that did not would both see the
machine's real runs and leave dozens of its own in the user's state directory.
Anything new that starts a scan has to keep that property.

Runs are testable without hardware in two ways, and both are used:

- **Sparse images.** `truncate -s 300G` gives a scan that takes minutes of real
  work without writing anything, which is the only way to catch a run in the
  act — to assert that a drive is queued, that quitting leaves the run going,
  or that a stopped drive still wrote its report.
- **Records written by hand.** A run directory plus a job record or two is
  enough to exercise every reader, including the states that need a crash to
  produce: a record that says running with a pid that does not exist is how the
  orphan path is tested.

**Tests named REGRESSION guard a bug that actually shipped.** Do not delete one
because it looks redundant; each is there because the tool was once wrong in
exactly that way. Add to them rather than replacing them.

Three things the suite cannot reach by default. Two are the ones the rest of
this file calls out: anything needing `EIO` from real media, and anything
needing `EILSEQ` from a drive's protection information. A file cannot be made
to produce either.

The third is `--format`. On a file target it is refused at the `is a file`
gate, so the banner, the constructed `sg_format` command, `format_child()` and
everything it feeds the format dashboard from — the percentage parser in
`fmt_pct()`, the carriage-return line splitting, the note the record carries —
are never reached; the only way to exercise them is to select a real drive with
format mode. **Do not write a default test that does that.** A suite
that erases a drive when a gate regresses has the risk exactly backwards.
Those checks live behind `HDDSCAN_TEST_DEVICE=/dev/sdX`, run only under
`--dry-run`, and even the missing-`--confirm` check passes `--dry-run` as well
so a regression in either gate still cannot reach the platter.

**Check the form at 24x80, not only at the suite's default size.** The suite
runs at 24x112 and the format section tests at 42 rows, and at those sizes a
section cut off by the row budget, or a line that wraps at 80 columns, is
invisible. That is how the low level format section came to show only its
heading and first field on an ordinary terminal and was reported as missing.
Two traps when writing such a test: `Session.set_choice()` and `value_of()`
read a value back by walking the cursor to it, which scrolls the list and can
undo exactly the scrolling being tested -- step with raw keys and watch a line
that does not move, such as the selection line's warning. And every line the
form prints must fit the terminal width: a wrapped row is a row the budget did
not count, and the terminal answers by scrolling the top of the form away.
The form's height also depends on the machine: its drive list includes the
machine's own drives, and a missing optional tool adds a line. Read what the
drive keys selected from the `N drives selected` line, not from the ticks that
happen to be in view, and walk to a drive with `goto()` before looking at its
row. `HDDSCAN_TOOLS=none` makes every optional tool look absent, which is
the only way to put the longest missing-tools line on the form on purpose;
`HDDSCAN_TOOLS=all` keeps the line off the screenshots.

**Never assert with a fixed sleep.** The form repaints on its own 200 ms tick,
so a key pressed just after a repaint is invisible until the next one, and
fixed settles make the warning and confirmation tests flaky. Use
`Session.wait(pred)`, which polls.

Writing new tests: `--confirm` is matched against the device name *as given*,
so for an image at a path it is the whole path, not the basename. A device
named on the command line arrives already ticked in the picker, so pressing
space on it deselects it. Both of those cost an hour the first time.

## Testing without a failing drive

There is usually no spinning disk and no root available in the dev environment.
`hddscan` accepts a **regular file** as a device, which is how nearly everything
gets tested:

```sh
dd if=/dev/urandom of=t.bin bs=1M count=64 status=none

# force the drill-down and retry paths with an unreachable budget
./hddscan --no-color --chunk-slow-ms 0.3 --retries 5 t.bin

# invariant 4: verify must be non-destructive
md5sum t.bin > t.md5
./hddscan --no-color --mode verify --confirm t.bin t.bin >/dev/null 2>&1
md5sum -c t.md5

# invariant 2: coverage. Write the pattern shuffled, verify it sequentially.
# Any chunk the permutation skipped shows up as corrupt.
for seg in 1M 4M 7M 64M 0; do
  ./hddscan --no-color --mode write --order random --segment $seg \
      --confirm t.bin t.bin >/dev/null 2>&1
  ./hddscan --no-color --mode check t.bin 2>/dev/null | grep "wrong data"
done

# corruption detection
printf 'ZZZZ' | dd of=t.bin bs=1 seek=1048576 conv=notrunc status=none
./hddscan --no-color --mode check t.bin 2>/dev/null | grep -E "wrong data|VERDICT"
```

**Deterministic tests beat timing-dependent ones.** Which sectors get flagged
"slow" depends on machine load, so do not build assertions on that. Injected
corruption plus `--mode check` gives exact, repeatable findings at known
offsets — that is how the badblocks offset arithmetic is verified:

```sh
./hddscan --mode write --confirm t.bin t.bin
printf 'BADBADBAD' | dd of=t.bin bs=1 seek=8388608 conv=notrunc status=none
./hddscan --mode check --badblocks-list bb.txt t.bin       # -> 8192..8195
./hddscan --mode check --badblocks-list bb.txt --badblocks-offset 1M t.bin
                                                            # -> 7168..7171
```

**Invariant 10 (the journal) is testable by building the crash state by hand**:
write a valid journal header (`jrn_hdr_t`, 104 bytes, little-endian, hash from
`jrn_sum`) plus the original payload, clobber that region of the file, then run
with `--journal` and confirm the md5 comes back. Killing a live run with
`SIGKILL` almost always lands between chunks and proves nothing.

**The TUI is testable headlessly** by driving it through a pty — fork with
`pty.fork()`, set the window size with `TIOCSWINSZ`, and write keys. Pass an
image file as an argument and it appears in the drive picker, so the whole
config-to-dashboard-to-report path can be exercised without any real hardware.

The configure form can be checked by stripping the escapes, since it prints
top to bottom. **The dashboard cannot.** It draws every row by absolute
position (`\033[r;cH`), so the order bytes arrive in is not the order they
appear on screen, and stripping escapes collapses the whole frame onto one
line. Testing it needs a small screen model: replay the stream into a grid,
honouring cursor moves, `\033[K` and `\033[2J`, then assert on the grid. That
is the only way to catch what the naive approach hid for a long time — rows
the dashboard never erased holding fragments of earlier frames.

Include a segment size that is **not** a power of two (7M above) and one that
does not divide the device evenly. Those are where permutation bugs live.

The `EIO` path cannot be tested this way. It needs root and a `dm-error`
target; the recipe is in `DESIGN.md`. That path remains verified by inspection
only — say so rather than implying otherwise.

## CI and releases

`.github/workflows/ci.yml` runs on every push and pull request, on x86_64 and
aarch64: the build with `-Werror`, `make test`, `mancheck`, `sitecheck`, then
the static build and the CLI and dm suites again against it. A warning another
compiler version finds is a failure, the same as on a developer's machine.

`.github/workflows/release.yml` runs on a tag `v<VERSION>` and refuses a tag
that does not match `VERSION` in `hddscan.c`. Each architecture builds on its
own runner with `make dist`, unpacks the tarball and runs all three suites
against the binary inside, the one that ships rather than a sibling, then
the tarballs go to a **draft** release. Publishing is a person's decision.
`make dist` is the whole recipe, so a release can be reproduced by hand.
Releases are static glibc, which is proven by those suites. musl would be
tidier and has never been tried; do not switch to it without the suites
behind it.

## Environment notes

- Some paths only real hardware reaches, and each needs a particular kind of
  drive. A **SAS** drive reaches the error counter log, the mode pages,
  `sg_reassign` and the `sdparm` look-ahead backend (`sdparm` and `sg3_utils`
  installed). A **4Kn** drive, with a logical block size of 4096, is where the
  alignment arithmetic behind invariant 3 is actually exercised. A drive
  **formatted with T10 protection information and barely written since** is
  the one place the `prot_errors` path of invariant 3 runs for real: every
  block not written since the format refuses to be read, with `Sense Key:
  Aborted Command / Logical block guard check failed`, which reaches us as
  `EILSEQ`. That is not a fault, it is what a freshly PI-formatted drive does,
  so never read a FAILING verdict from such a drive as a dying one without
  checking `prot_errors` first. A drive with `protection_type` 0 is the clean
  control beside it.
- Never test on the drive the system runs from. The safety checks refuse a
  mounted drive, but check which disk holds `/` before handing out a command.
- Protection type is readable without root or sg3_utils, from
  `/sys/block/<dev>/device/scsi_disk/*/protection_type`. The `integrity/`
  directory under `/sys/block/<dev>/` is **not** the same thing: it describes
  host-side DIX, and reads `format: none` on a type 2 drive.
- Reading a block device needs root or the `disk` group, which an agent
  session normally has neither of. Hardware runs are handed to the person at
  the machine to execute, with the exact command; image files remain the way
  to test everything else.
- Untested for lack of hardware, and honestly labelled as such in `DESIGN.md`:
  the `EIO` path, the drive look-ahead success path, SMR gating, every
  SAS-specific code path (error counter log, mode pages, `sg_reassign`), and
  the format dashboard's feed — `format_child()` only ever runs against a real
  SCSI drive. A drive vanishing is tested only as an image whose capacity
  collapses; the `/sys/block`, device-state and `ENODEV` checks in
  `dev_gone()` need a drive actually pulled. So does a destructive pass writing
  over a chunk whose read failed, and `blocks_unrepaired` counting what that
  write did not fix: an image cannot return `EIO`, so the suite reaches the
  verdict only through a checkpoint's counts. Do not quietly upgrade these to "verified" without real hardware
  behind them.
- Test artifacts (`*.bin`, `hddscan-*.txt`, `*.md5`, state files) must be
  cleaned up; the repo tracks only `hddscan.c`, `Makefile`, `hddscan.8`,
  `LICENSE`, `README.md`, `DESIGN.md`, `tests/`, `docs/` (the site, and the
  README's screenshots), `tools/` (what generates them), `contrib/` (the boot
  unit for the maps) and this file.
- `hddscan.8` and `usage()` document the same options and must not drift.
  `make mancheck` diffs the two option lists and prints nothing when they
  agree; run it after adding or renaming any flag.
- `docs/` is the GitHub Pages site: `index.html` (what it is for),
  `guide.html` (the form, the dashboard and the CLI) and `style.css`, plain
  files with no build step and no script. The option reference inside
  `guide.html` is generated from `--help` by `tools/options.py`, between its
  markers, and `make sitecheck` prints nothing when it is current -- run it
  with `mancheck`. The screenshots in `docs/img/` are drawn from hddscan's own
  output by `tools/shots.py` (`make site` does both); regenerate them after
  changing anything a screen shows, never edit an SVG by hand. The runs in them
  come from a demo store whose records point at a `sleep` the script starts,
  so they draw as live. `HDDSCAN_NO_ENUMERATE` keeps the machine's own drives,
  serials included, out of the form, so the pictures are the same wherever they
  are made.

**12. A run outlives whatever started it, and every screen is only a viewer.**
A scan or a format belongs to a supervisor process that `setsid()`s away from
the terminal; the dashboard, `--status` and `--attach` all read the run's job
records and none of them own anything. Two consequences are load-bearing.

- **The process holding a drive is the only writer of that drive's record.**
  The worker rewrites its own record whole (tmp file plus `rename`); the
  supervisor only ever reads one back, patches how the worker ended, and
  writes it again. Do not add a second writer, and do not make a viewer write
  to a run it does not own — a dashboard that "corrects" a record is a
  dashboard that invents progress.
- **The run-scoped drive settings are restored when the *run* ends, never when
  a viewer exits.** That is why `apply_settings()` now runs inside
  `run_begin()`'s child and why that child ends with `exit()` rather than
  `_exit()` (invariant 7b's restore list is an `atexit` handler). Putting the
  look-ahead back because somebody closed a window would leave the drives still
  being scanned measured under different conditions than the ones already
  finished.

**13. Liveness is a pid *and* its start time, never a pid alone.** Pids wrap.
Every record carries `/proc/<pid>/stat` field 22 beside the pid and `pid_live()`
compares both, or a week-old run whose number has been reused reports itself as
still scanning. A record that says running whose process is gone is reported as
`orphaned` and its run as `interrupted` — never as a percentage that will never
move again, which is the same class of confident wrong answer as invariant 3.
A run whose supervisor is gone but whose workers are not is still live: killing
the supervisor leaves each worker reading its own drive to the end.

**14. A stop is exactly one signal per worker.** The second `SIGINT` a worker
gets is `on_signal()`'s hard path, which abandons the report. `store_stop()`
therefore signals the supervisor while there is one and lets it forward, and
`forward_stop()` marks each worker as told rather than re-sending on every
tick — the old parallel loop re-sent every 250 ms, so stopping a run threw away
the partial report of every drive it was stopping. That report, with its
`Coverage` line, is the entire point of stopping gracefully; there is a
REGRESSION test on it.

Two more things that follow from the store, both easy to get wrong:

- **A drive in a live run is not available to another one.** `safety_check()`
  asks the store, because `O_EXCL` only covers the drive a worker currently
  holds open — not one sitting twenty-ninth in a queue — and two runs on one
  spindle time each other's seeks.
- **Composing store paths needs `STORE_MAX`, not `PATH_MAX`.** A run directory
  is root + id + slug; three `PATH_MAX` parts cannot live in a `PATH_MAX`
  buffer, and `-Wformat-truncation` (part of `-Wformat=2`, which must stay
  silent) will say so. Refuse a path that does not fit rather than truncating
  one into a path that points somewhere else.

**15. A device-mapper map puts data in the wrong place if it is wrong, so it
is never guessed at.** `hddscan dm` (also installed as the `dm-badblocks`
link, and what `--hide-bad` calls in-process) keeps a map of a drive's bad
extents on the drive and loads it as a dm-linear table. It was a separate
project once; it lives here so a rescue system needs one binary and so the map
has one implementation. Its code is prefixed `dmbb_` and keeps its own `die()`
and `msg()`. Five rules:

- **The skip list never changes after `create`.** It defines the layout:
  logical extent L is the Lth physical extent that is not metadata, a spare
  slot or skipped. Adding to it later would move every byte after the new
  entry. Damage found later is a remap, always.
- **The map is written A, sync, B, sync, and read as the newest copy that
  checks out.** Neither copy is written while the other is the only good one.
  Both bad means "no map", never a best guess. Damage in either copy's extent
  refuses `create`.
- **A live remap is suspend, copy, verify, write map, reload, resume.** If the
  reload fails the previous map is written back as a newer generation, or the
  next activation would read copies that stopped receiving writes the moment
  the old table resumed.
- **Whether a device is live is asked of sysfs, not `dmsetup`,** which needs
  root to answer at all. A wrong "not live" would copy extents out from under
  a mounted filesystem. `activate-all` likewise says it could not read a drive
  rather than reporting that no drive has a map.
- **`--hide-bad` only takes its list from a finished whole-drive scan,**
  judged by the checkpoint's own start, end and byte count. A sampled or
  interrupted scan's list hides only what it looked at, and still looks like
  a fix. Every scan, single-drive ones included, keeps a checkpoint in its run
  directory for this reason.

The kernel side (`activate`, a live `remap`, the boot unit) needs root and
has not been run. Say so rather than implying otherwise.

**11. `--format` is not a scan mode and must not become one.** FORMAT UNIT
erases the drive, runs for hours and cannot be undone, so it dispatches before
the scan path entirely rather than joining `scan_mode_t`. It keeps three
guards: the same `--confirm <name-or-serial>` a write mode needs, a refusal on
any drive `safety_check()` marks unsafe, and printing the exact `sg_format`
command per drive before anything runs. `--format-pi` must always be passed
explicitly — `sg_format` defaults `--fmtpinfo` to 0, so a bare block size
change would silently strip protection information, and the default here is to
preserve whatever `prot_type` the drive reports.

## Style

Kernel-ish C: tabs, 80 columns, declarations at the top of a block, `static`
on everything with file scope, no typedef'd pointers. Comments explain *why* a
constraint exists — the seek/rotation argument, the cache-busting seek, the
`O_EXCL` reasoning — not what the next line does. Match what is already there.

Error paths report and continue where a drive fault is expected; `die()` is for
usage and setup errors only. A scan must never abort because one sector is bad,
since finding bad sectors is the entire point.

**Any sdparm command line this tool prints or runs must name one field per
`--set`/`--clear`/`--get`, or a comma separated list from a single mode page.**
sdparm silently honours only the last such option on a command line. The advice
the report used to print, `--clear=RCD --clear=DRA --set=AWRE=1 --set=ARRE=1`,
therefore applied only DRA and ARRE and quietly skipped the other two — and
those four do not fit in one invocation anyway, since RCD and DRA are in the
caching page and AWRE/ARRE are in read-write error recovery. Every setter in
the code passes a single field for this reason; keep it that way.

**`smartctl -A` does not print the SAS log pages.** The error counter log and
the self-test log need `-l error -l selftest`, and `smart_read()` adds them for
`TR_SCSI` only — on ATA the extra sections give the attribute parser more lines
to trip over for no gain. This was wrong for a long time and the failure was
silent: the parser was correct and simply never saw the data, so every real SAS
drive reported that no counters could be read. `tests/fixtures/` holds output
captured from a failing drive; the suite guards the column positions the parser
depends on, since the C path itself needs hardware.

The kernel log is the same story. `HDDSCAN_KMSG` makes a scan read a file of
log lines instead of `/dev/kmsg`, and `HDDSCAN_KMSG_HCTL` gives an image the
SCSI address (or `ataN` port) those lines name a drive by; they exist for the
suite and nothing else. `tests/fixtures/kmsg-sas-resets.txt` is a real drive
being aborted and reset mid-scan. What they cannot reach is the `/dev/kmsg`
read itself -- one record per `read()`, `EPIPE` when the ring overwrote one --
and the address taken from sysfs, both of which need root and a real drive.

`HDDSCAN_SMARTCTL` does the same for SMART: it runs a command in place of
`smartctl`, for any device including an image, and the suite's stand-in serves
a before and an after report so counters can be seen to move. It proves the
parser and the verdict, not smartmontools' real layout on real drives -- that
is still what the captured fixtures are for.

## When reporting results

**The report must never contradict itself.** Two numbers describing the same
thing have to be derived the same way. The "reads exceeding N ms" table was
once computed from the latency histogram, which counts a whole bucket as over
the line the moment the line falls inside it; on a drive whose chunks all
landed in the 20-30 ms bucket it claimed 284 reads over 25 ms while the exact
`chunks over budget` counter three sections below said 0 and the slowest read
of the entire scan was 22.4 ms. Both cannot be true, and a reader has no way
to tell which to believe. Tier counts are now exact (`lat_t.over[]`), and
`lat_pct()` clamps its bucket estimate to the observed min and max, because a
p50 above the maximum is not an approximation, it is a wrong number.

State coverage honestly. If a scan was sampled, interrupted or range-limited,
the report says so and so should you. A verdict of HEALTHY over 3% of a drive
is not a healthy drive, and the report has a `Coverage` line specifically so
that distinction cannot be lost.
