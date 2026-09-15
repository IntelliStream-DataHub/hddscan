# hddscan — design notes

Why the tool works the way it does. `README.md` is the short version and
`man 8 hddscan` is the reference; this is the reasoning, including the things
that turned out to be wrong on real hardware.

Per-sector latency and error surface scanner for rotating disks.

The question it answers: **does every sector on this drive return its data
within a reasonable time?** Not "can it be read at all" — a dying drive will
usually still return your data, after the firmware has spent two seconds
re-reading the track and grinding through ECC recovery. That latency is the
early warning, and it appears long before SMART admits anything is wrong.

## How it works

Reading a 20 TB drive 4 KiB at a time would take weeks, so the scan is
two-tier:

1. **Bulk pass.** Read the device in 128 KiB chunks and time each one. A chunk
   can never come back faster than its slowest sector, so a chunk that meets
   its budget *proves* all 32 sectors inside it were served in time.
2. **Drill-down.** A chunk that misses its budget (or errors) is re-read one
   4 KiB block at a time to find which sector was responsible.
3. **Retry.** Every suspicious sector is re-read up to 20 times, with a seek to
   the far side of the platter between attempts so the drive cannot serve it
   from its own DRAM cache. This separates a one-off hiccup from a sector that
   is genuinely going, and it is where drives often heal themselves — a sector
   that reads slowly once and cleanly twenty times afterwards is reported as
   `recovered`, not as a fault.

The chunk budget is clamped to be no looser than the sector budget. If it were
looser, a slow sector could hide inside a chunk that never gets drilled into,
and the whole guarantee would be void.

Thresholds are calibrated per drive, **using the same access pattern the scan
will use**. This matters more than it sounds: timing scattered reads and then
scanning sequentially produces a budget inflated by the cost of a seek, which
on a real drive is an order of magnitude, and far too loose to catch a sector
that needs 30 ms of internal recovery.

## Running it

```sh
make
sudo ./hddscan                 # pick drives and settings on a form, then watch
sudo ./hddscan --list          # what is attached, and what is safe to test
sudo ./hddscan --all           # read-only scan of every free rotational disk
sudo ./hddscan /dev/sdb        # one specific drive
```

### Profiles

What a drive needs tested depends on what you are about to do with it, and that
one decision drives half a dozen options. `--profile` is that decision, named,
so the form asks a question you have an answer to instead of six you would have
to derive it from.

| profile | mode | for |
|---|---|---|
| **predeploy** *(default)* | write | No data on it yet. Writes every sector and verifies it reads back — the only way to learn whether a sector will *accept* a write. |
| **inservice** | read | It holds data you want to keep. Never writes anything. |
| **survey** | read, sampled | Quick triage of a shelf. |
| **decay** | check | Weeks after a predeploy run: has the pattern rotted? |
| **repair** | write | predeploy, plus forcing a reallocation of anything unreadable. |

The default writes, because a surface test is nearly always run before a drive
goes into service and a read-only pass cannot tell you whether a sector will
accept a write. That means a bare invocation stops and asks for `--confirm`
rather than doing anything — an error, never data loss. Reach for `inservice`
on a drive that holds something you care about.

Any explicit flag overrides what the profile set, whichever order the two
appear in, and the report names the profile that produced it.

**Run on a terminal with nothing to scan named, and the form opens.** That is
the default way in; `--tui` (or `-i`) asks for it even when devices are named,
which arrive pre-selected, and `--no-tui` refuses it. Naming a device, `--all`
or `--list` is an explicit instruction and is carried out as given, so nothing
that already works in a script changes.

The drives and the settings are one list: up and down walk from the last drive
straight into the first setting, so there is nothing to switch into before it
can be reached. Left and right change a value, `s` starts.

Selecting drives one at a time stops being a workflow somewhere around the
tenth, so the drive list also takes `a` for every free HDD, **`g` for every
free HDD on the same controller as the one under the cursor**, and `n` to clear.
The picker shows each drive's controller, so a shelf hanging off one HBA is one
keystroke.

The settings are in two groups, because they answer different questions:

```
 -- How to test --
   Profile                < predeploy    >
   Mode                   < write        >
   Chunk size             < 128K         >
   ...
 -- Drive settings (sdparm/hdparm, restored on exit) --
   Drive look-ahead       < off          >
   Read cache             < off          >
   ...
   Save to drive          < this run     >
```

```
 -- Low level format --
   Sector size            < keep         >
   Protection info        < keep         >
   Fast format            < no           >
```

That third section **only appears when Mode is `format`**, and choosing that
mode moves the cursor straight onto it. Nineteen settings on a 24-row terminal
means about six visible at a time, and options that only matter for one mode
were sitting three screens below the fold.

Everything in the first group decides how this run behaves. Everything in the
second changes the drive itself, through `sdparm` or `hdparm`, and is put back
when the scan ends unless **Save to drive** says otherwise. The third is only
read when **Mode** is `format`, and both of its fields default to `keep` —
`sg_format` strips protection information unless told otherwise, so a bare
sector size change would remove it as a side effect nobody asked for.

Not every drive offers 4096. A Seagate `ST14000NM0168` rejects it at the MODE
SELECT, which costs nothing: the format never starts and the drive is
untouched. Dropping protection information at its existing 512 recovered
284 GB on that drive — 13,715,978,059,776 bytes to 14,000,519,643,136, the
full nominal 14 TB — because type 2 protection stores eight bytes beside every
512-byte block.

The list scrolls, so the form works on a short terminal. A mode that
writes to the drive needs `s` and then `y`. It switches to a live dashboard — aggregate throughput, per-drive
progress, bad and weak sector counts, ETA — and prints the full reports when it
exits. It is raw ANSI and termios, no ncurses, so the binary still has no
dependencies beyond libc (see Portability).

There is a manual page: `man 8 hddscan` after `make install`, or
`man -l hddscan.8` from the source directory. `make mancheck` verifies it has
not drifted from `--help`.

Nothing is linked; `smartctl`, `sdparm`, `hdparm` and sg3-utils are shelled out
to when present and done without when absent. Every run says on stderr what is
missing and what that costs, and `--check-deps` prints the whole table with the
line that installs the gaps:

```
  smartctl   found    SMART health, temperature, and the drive's own error counters
  sdparm     found    every drive setting on SAS/SCSI: look-ahead, caches, ...
  sg_modes   found    SCSI mode pages, sector reassignment, and reformatting
  hdparm     MISSING  look-ahead and write cache on ATA/SATA drives

Install the missing ones with either of:
    sudo dnf install -y hdparm
    sudo apt install -y hdparm
```

While a scan runs, both the plain progress line and the dashboard show latency
over a **rolling 30-second window** alongside the cumulative figures:

```
sdb   3.1%  12.24 MiB/s  bad:0 slow:0 weak:0  last 30s med 7.5 avg 8.4 min 6.2 max 58.9 ms
```

The report describes the whole scan; this describes what the drive is doing
*now*. A head that has just wandered into a bad patch shows up here long before
it moves the lifetime average. Median is what a typical read costs; the gap
between median and average is what the outliers are doing. The window states
the span it actually covers, so a scan two seconds old says `last 2s` rather
than claiming thirty.

A write test times its writes in a window of their own, shown as `WMED` on the
dashboard and `write med` on the progress line. The read columns cannot show a
drive whose writes crawl: it reads back at full speed, and the only symptom
used to be a rate that made no sense beside the latency next to it. A write
lands on the same track as the read before it and waits for the same
revolution, so it costs what a read there does — 8.79 ms against 8.44 ms on a
healthy SAS drive, write cache on — and a write over the read budget at that
position is counted as over budget.

Exit status is `0` healthy, `1` suspect, `2` failing, `130` interrupted, so it
drops straight into a cron job or a CI gate. Every run prints an estimated
runtime after calibration, before committing to what may be a multi-day scan.

### Parallelism

Multiple drives are scanned **simultaneously by default**, one worker process
per drive, with a live aggregated progress display. Separate spindles are
genuinely independent, so 100 drives take about as long as the slowest single
drive. Past 20 drives the display switches from per-drive lines to a fleet
summary plus the drives needing attention.

Work is deliberately **never** parallelised *within* a drive. A second
outstanding request on one spindle makes the head seek between the two, and the
measurement would be of queueing delay rather than of the media.

`--max-parallel N` caps concurrency, which matters when the HBA, SAS expander
or PSU cannot feed every drive at full rate — a saturated bus inflates every
drive's latency and produces false "slow sector" reports.

## Read-ahead, and why scan order matters

There are two completely separate prefetchers between you and the platter, and
they need different handling.

**Kernel page-cache readahead** is already dealt with: `O_DIRECT` bypasses the
page cache entirely, and readahead is a page-cache mechanism. The knob people
reach for here — `/sys/block/<dev>/queue/read_ahead_kb`, equivalently
`blockdev --setra` or `hdparm -a` — is per-device sysfs, not a sysctl, and it
is irrelevant while we are using `O_DIRECT`. It becomes relevant again under
`--no-direct`, which is why that mode warns about its own results.

**The drive's firmware look-ahead** is the real problem. The HDD speculatively
reads ahead on the media into its onboard DRAM whenever it detects a sequential
access pattern. That happens below the kernel entirely, so no sysfs or sysctl
setting reaches it. During a sequential scan the drive can prefetch a marginal
sector during idle time and then hand it over at DRAM speed, and we never see
the stall. A *severely* bad sector cannot hide this way — the drive cannot
prefetch faster than it can read — but one costing 30–80 ms can.

Two ways to defeat it:

**`--drive-lookahead off` is the default.** There is no one way to ask for it:
on ATA it is a `SET FEATURES` command, sent with `hdparm -A0`; on SAS/SCSI it
is the `DRA` bit of the caching mode page, cleared with `sdparm --set=DRA=1`.
Both are shelled out to when installed, and both are restored on exit, on
`SIGINT`, and on error — always to the current values, never with `--save`, so
nothing is left behind on the drive. This removes the masking mechanism at its
source while keeping the scan sequential and fast. Some drives ignore the
request, so the tool reads the setting back and checks it took effect.
`--drive-lookahead keep` opts out.

If look-ahead could not be turned off — no root, no `hdparm` or `sdparm` for
the transport in hand, or a drive that ignored the command — a sequential scan
**says so on stderr and in the report**, and points you at `--order random`. A
clean result the tool cannot stand behind is worse than no result. The setting
is volatile, so if the process is killed outright it is restored by the next
power cycle, or manually with `hdparm -A1 /dev/sdX` on ATA and
`sdparm --clear=DRA /dev/sdX` on SAS.

**`--order random`** shuffles the visit order so the drive's sequential
detector never fires. The shuffle is *segment-local*: the device is divided
into segments (`--segment`, default 1 GiB), segments are walked in order, and
chunks are shuffled within each segment. That defeats the detector just as well
as a whole-device shuffle while keeping every seek short instead of paying a
full-stroke seek per chunk. `--segment 0` shuffles the whole device.

The segment must be much larger than the drive's DRAM cache, or the drive will
simply hold the entire segment and you will be timing its cache again. The
1 GiB default clears a 256 MB cache comfortably.

### The cost, and why sequential is still the default

For a 20 TB drive at 128 KiB chunks (≈153 million chunks), rough figures for a
7200 rpm drive:

| Order | Per chunk | Whole drive |
|---|---|---|
| Sequential | ~0.7 ms (transfer only) | ~28 hours |
| Random, 1 GiB segments | ~5.8 ms (short seek + rotation + transfer) | ~10 days |
| Random, whole device | ~13 ms (full-stroke seek + rotation + transfer) | ~24 days |

Segmenting the shuffle roughly halves the cost of random order — your intuition
about bounding the seek is correct and worth real time. But it cannot approach
sequential speed, because **average rotational latency is an irreducible floor**:
once you break sequentiality you wait half a revolution (~4.2 ms) for every
chunk no matter how short the seek. That alone is ~7.4 days for a 20 TB drive.

Random order also *costs* you sensitivity. Sequential reads have a ~0.7 ms
baseline with almost no spread, so a 25 ms budget is a 35x signal and catches
even mild recovery events. Random reads have a ~5.8 ms baseline with a 0–8.3 ms
rotational spread, so the budget has to sit above the noise — around 30–40 ms —
and sectors needing 20 ms of recovery slip through. Random order trades one
blind spot for another.

So the defaults are **sequential order with the drive's look-ahead disabled**,
which gives the tight baseline *and* no prefetch masking, at ~28 hours. Reach
for `--order random` when the drive will not honour the request, when neither
`hdparm` nor `sdparm` is installed, or as a belt-and-braces second opinion — and pair it with a
larger `--chunk` (say 1 MiB) to amortise the rotational cost, which brings a
20 TB drive down to roughly 2 days.

Raising `--chunk` in random mode is safe: the "chunk is never faster than its
slowest sector" guarantee holds at any chunk size, and prefetch masking happens
*between* requests, never inside one — the drive must genuinely read every
sector of a single large request.

### Why a scan is slow, and what to do about it

A sequential scan of a 7200 rpm drive at the default 128 KiB chunk runs at
about 14 MB/s, not the 200 MB/s the drive can stream. That is not a fault, it
is the cost of the guarantee: with look-ahead off and one request outstanding,
each read finishes just *after* the start of the next chunk has passed under
the head, so the drive waits a full revolution — about 8.3 ms — to come back
round to it. Against 0.6 ms of actual transfer for 128 KiB, almost all of the
time is spent waiting.

The fix is a bigger chunk, because the lost revolution is paid once per
*request*, not per byte. 1 MiB chunks pay the same 8.3 ms for eight times the
data. So after calibrating, hddscan measures this directly — it times a few
reads eight times the chunk size — and tells you what it found:

```
  estimated runtime 1d 4h at 8.87 ms per 128 KiB chunk
  most of that is waiting for the platter, not reading it: 1024 KiB
  took 13.20 ms, so '--chunk 1024K' is 5.4x the throughput and would
  bring this scan to about 5h 12m.
```

### The budget follows the platter

A disk spins at a fixed rate, so an outer track sweeps further per revolution
and — with zone bit recording putting more sectors there — reads roughly twice
as fast as one at the inner edge. A single budget for the whole device is
therefore loose on the outside and tight on the inside: the same 26 ms means
four times the local median at LBA 0 and barely twice it at the end.

So the budget follows the gradient. Calibration already samples eight anchors
spread across the device; the budget is interpolated between them, keeping the
same margin relative to what a read *at that position* is expected to cost:

```
  calibrated: median 128 KiB read 8.81 ms -> budget 26.4 ms chunk / 26.4 ms sector
  the budget follows the platter: 6.20 ms per chunk at the outer edge,
  11.40 ms at the inner, so the same margin applies everywhere
```

```
  Latency budget   25.0 ms where the platter is quickest to 34.2 ms where it is
                   slowest, following the drive's own gradient (auto-calibrated)
```

Two things it deliberately does **not** do. It never learns from the chunks it
is scanning — a budget that adapted to what it was reading would quietly raise
itself over a bad region and normalise away the exact thing the scan exists to
find. The gradient is physics, measured once, and smooth. And it never bends a
threshold you gave by hand: `--chunk-slow-ms 25` means 25 ms everywhere.

The multiple is three by default (`--auto-factor`). A chunk that takes three
times what a read at that position should cost has something inside it worth
looking at, and on a drive whose worst reads sit at four or five times its
median a looser multiple reported nothing at all. Going over the chunk budget
only costs a drill-down; a sector is still counted slow only if it is over the
sector budget itself. `--floor-ms` still sits underneath, so a very fast
drive's budget never drops into the rotational noise.

Invariant 1 still holds pointwise — the chunk budget is clamped to the sector
budget at each position, not just on average.

The guarantee is unchanged at any chunk size — a chunk is still never faster
than its slowest sector, and drill-down still resolves to 4 KiB. What does
change is **what the scan can see**. The budget is calibrated as a multiple of
the median chunk, so a bigger chunk raises the bar a sector has to clear before
anyone looks at it: on a 7200 rpm drive 128 KiB calibrates to about 26 ms,
1 MiB to about 66 ms, 4 MiB to around 90 ms. A sector taking 80 ms is found
by the first and hides inside the last. The scan says so up front rather than
leaving you to work it out:

```
  at 1024 KiB chunks a sector has to exceed 43.4 ms to be noticed;
  the 128 KiB default is a tighter bar. --sector-slow-ms pins it back,
  at the cost of drilling into more chunks.
```

That is why the default stays at 128 KiB, on the command line and on the form
alike — fast and blind is not a sensible thing to ship as a default, and a
setting that differed depending on how you launched the tool would make two
scans of the same drive incomparable. **Chunk size** is a field on the form so
the trade is one keystroke away when you want it, and `--sector-slow-ms` buys
back the sensitivity at the cost of more drill-downs.

Write modes pay this twice, once for the write and once for the read-back that
verifies it, which is why `--mode write` feels roughly half as fast as a read
scan. The same fix applies and matters twice as much.

### Shingled (SMR) drives

`/sys/block/*/queue/zoned` is checked before anything is written. A
**host-managed** SMR drive can only have its zones written sequentially from
the start, so a write test would fail on nearly every request and look like
total media failure — those are refused for write modes outright. **Host-aware**
drives accept the writes but rewrite whole bands behind your back, so the write
latency means nothing; you get a warning. `--order random` is refused with any
write mode on a zoned drive for the same reason.

**Drive-managed** SMR is the nasty one: it reports itself as conventional and
there is nothing in sysfs to find. The only tell is behavioural, so the scanner
compares early write cost against late — when the persistent cache fills and
the firmware starts rewriting bands, throughput collapses. An eightfold
collapse mid-scan raises a warning that the latency results are not
trustworthy, rather than reporting thousands of phantom slow sectors.

### Test modes

| Mode | Writes? | What it proves |
|---|---|---|
| `--mode read` | no | Every sector returns data in time. The default. |
| `--mode verify` | yes, restores | Same, plus writes work. Saves each chunk, writes a pattern, reads it back, compares, restores the original. |
| `--mode write` | destroys everything | Full surface write + read-back verify. |
| `--mode check` | no | Re-verifies a pattern left by an earlier `--mode write`. Run it weeks later to catch sectors that lost their charge. |

Anything that writes requires `--confirm <device-name-or-serial>`. `verify`
mode carries a real risk: if power is lost between writing the pattern and
restoring the original, that chunk's data is gone.

The write pattern is derived from the byte offset, so a block written to the
wrong place fails verification even though it holds valid-looking data. That
catches misdirected writes, which are otherwise invisible.

### Making a drive heal itself

`--rewrite-weak` rewrites sectors that read back slowly but successfully. The
write either refreshes the magnetic domains or trips the firmware into
reallocating the sector to a spare. Original data is preserved — only sectors
that could be read are rewritten. The tool re-reads afterwards to confirm the
fix and reports `healed by rewrite`.

`--force-remap` overwrites sectors that are **unreadable** with zeros, forcing
reallocation. That sector's data is gone, but a pending sector becomes a
reallocated one and the drive stops stalling on it.

### Repairing a drive in one pass

`--repair` is the whole sequence, in order, on every sector:

```sh
sudo hddscan --repair --confirm sdc --chunk 4M \
     --recovery-time 300 --retries 3 \
     --state /var/tmp/sdc.state --resume /dev/sdc
```

1. **Overwrite every sector.** This is the part that actually repairs: a slow
   sector is usually a weakly written one, and rewriting it restores the signal
   margin. No spare is consumed and nothing is retired.
2. **Rewrite anything still slow afterwards.** If a sector was slow *after* it
   was just written, the problem is the media rather than the data on it.
3. **Force a reallocation on anything that will not read at all.** On SAS this
   is an explicit `REASSIGN BLOCKS`; on ATA it overwrites and lets the firmware
   decide.
4. **Re-read the entire device** and re-verify the pattern, to prove the repair
   held rather than assuming it.

It also turns the drive's read cache off, because a repair verified out of the
drive's own DRAM is not a repair you verified.

It destroys all data and needs `--confirm`, and it is three full passes over
the device — expect roughly twice the wall clock of a plain `--mode write`.
`--state` with `--resume` is strongly advised.

The report says what it achieved:

```
    sectors healed by rewrite          78  (read cleanly afterwards)
    sectors reassigned                  3  (the drive retired them to spares on request)
    sectors unreadable                  0  (every attempt failed)
```

Auto-reallocation has to be on for step 3 to be possible at all; `--fix-config`
sets it. And bound the drive's own recovery time, or a defect band will dominate
the run: at the factory `5000 ms` a single bad sector costs 5 seconds per
attempt.

### Repairing sectors, and what the firmware actually does

A read never makes a drive repair anything. A failed read just increments
`Current_Pending_Sector`; the drive keeps the data, because it cannot relocate
what it cannot read. **A write is what forces the decision.** On write the drive
rewrites in place and verifies — if it holds now, the fault was transient and
pending clears with no reallocation. If the sector is genuinely bad, the drive
pulls one from its spare pool and increments `Reallocated_Sector_Ct`.

So `--rewrite-weak` and `--force-remap` are not tricks; they are the only lever
that exists. The report now confirms what the firmware did rather than
inferring it from latency, by diffing the SMART counters across the scan: sectors
healed by rewrite, and separately the count the drive actually reallocated.

If pending sectors survive being rewritten and nothing was reallocated, the
spare pool is exhausted — the drive has no way left to repair itself. That is
reported explicitly and is a hard `FAILING`.

On SAS there is also an explicit `REASSIGN BLOCKS` command, which `--force-remap`
uses via `sg_reassign` when sg3-utils is present. ATA has no equivalent.

### SAS drives that write fast but read slowly

This is a specific, common fault, and SAS drives carry the evidence in a place
ATA drives do not: the SCSI error counter log. The report shows, before and
after the scan, how many reads the drive corrected *with possible delays*, how
many needed *rereads/rewrites*, and how many were uncorrected. Delays and
rereads climbing during a scan is the firmware fighting the media — which is
precisely what a drive that writes at full speed but reads like treacle is
doing, since writing never has to read anything back.

Before concluding the media is bad, the report also dumps the mode pages that
change what those numbers mean:

- **RCD** (read cache disabled) with **WCE** (write cache enabled) produces fast
  writes and slow reads on completely healthy hardware. Ex-array drives often
  arrive configured this way.
- **DRA** (read look-ahead disabled) makes sequential reads crawl.
- **ARRE/AWRE** off means the drive will never automatically reallocate a
  sector it struggles with, so marginal sectors stay marginal and get worse.
- A **recovery time limit** of 0 means unlimited: one bad sector can stall for
  a very long time instead of failing fast.

`--read-cache on|off` is the read side of the same page. Off stops the drive
answering *any* read out of its own DRAM, which is stricter than
`--drive-lookahead off` — that only stops it reading ahead. Left off on a drive
in service it makes reads crawl, which is the fault half of this section.

`--awre` and `--arre` control whether the firmware may retire a sector it
struggles with, on write and on read. **This is the setting that decides
whether anything can be repaired at all**: with both off, `--rewrite-weak` and
`--force-remap` rewrite a failing sector straight back onto itself and the
drive dutifully keeps using it. Ex-array drives routinely arrive this way.

If you run ZFS, note that it does not cover for AWRE being off. ZFS repairs
*data* — a checksum mismatch on a redundant vdev is rewritten from the good
copy — but that rewrite lands on the same LBA, and whether the drive retires
that sector to a spare is AWRE's decision alone. With AWRE off you get a clean
`zpool status` over media that is quietly rotting.

`--recovery-time MS` bounds how long the drive fights one bad sector before
reporting it (the SCSI `RTL` field; the equivalent of ATA's SCT ERC or TLER).
The common factory value is 0, meaning unlimited, and that is what makes a
dying disk stall an array for minutes at a time until the member gets faulted
— a drive dropped for being slow, not for being broken. Bounding it is
standard practice for ZFS and RAID members. On a lone data drive you may
genuinely want the drive to keep fighting, so this one is `keep` by default and
run-scoped unless you ask for `--persist`.

`--fix-config` applies all of that at once — auto-reallocate on for reads and
writes, read cache on — changing only the parts actually wrong on each drive
and printing each change. It saves what it sets, for the same reason `--bms on`
does: a configuration fix that reverted when the scan ended would not be a fix.

`--write-cache on|off` flips WCE for the duration of a scan and puts it back
afterwards, which is how you tell those apart without committing to anything:
measure, flip, measure again. It matters for write modes on its own account
too — with the cache on, a write returns as soon as the drive has the data in
DRAM, so you are timing the cache rather than the platter. `off` makes a write
mode honest and a great deal slower. Like the look-ahead and unlike `--bms`,
it is restored on exit, on `SIGINT` and on error; nothing is saved to the
drive.

Fix the rest and re-measure. Note this takes **two** commands: sdparm honours
only the last `--set` or `--clear` on a command line, and a comma separated
list may only name fields from one mode page. RCD and DRA are in the caching
page, AWRE and ARRE in read-write error recovery.

```sh
sdparm --clear=RCD,DRA --save /dev/sdX
sdparm --set=AWRE=1,ARRE=1 --save /dev/sdX
``` If reads are still slow with the cache on
and auto-reallocation enabled, it is the media, and a full `--mode write` pass
is what refreshes every sector and forces the drive to remap what it cannot
rescue. For ex-array SAS drives a low-level `sg_format --format --size=512`
also strips leftover protection information and rebuilds the defect list,
though it takes hours.

### Configuring a drive and making it stick

By default nothing this tool changes survives it: the kernel timeout, the
drive's look-ahead and its write cache are all put back on exit, on `SIGINT`
and on error. That is right for a measurement, and wrong when what you actually
want is to *configure* a drive — a freshly pulled ex-array disk usually needs
its caching sorted out before it is worth anything, and doing that by hand for
every drive in a shelf is a chore.

`--persist` saves what the run changed to the drive, so it survives a power
cycle:

```sh
sudo hddscan --persist --write-cache on --bms on /dev/sdb /dev/sdc
```

On the form it is the **Save to drive** field, and choosing it makes starting
the scan a two-key confirmation the same way a write mode does.

**Settings are applied before anything else happens**, to every selected drive,
not when a scan worker eventually reaches its drive. A setting you asked for
takes effect when you asked for it. And if configuring is all you wanted,
`--apply-settings` — or **Mode: settings** on the form — applies them and stops
without scanning at all:

```sh
sudo hddscan --apply-settings --fix-config --recovery-time 7000 /dev/sdb /dev/sdc
```

Applying up front also puts the run-scoped restore list in the parent process,
so a `Ctrl-C` during a parallel run puts every drive back rather than only the
ones whose workers happened to get the signal.

Three things about it are deliberate:

- **It needs a SAS/SCSI drive and `sdparm`.** Saving means writing a saved mode
  page; `hdparm` has no equivalent on ATA. Rather than pretend, the tool says
  the setting cannot be saved on that drive and applies it for the run only.
- **Only the settings you named are saved.** `--drive-lookahead off` is the
  scan default, so a bare `--persist` will not make it permanent — otherwise
  every drive you ever scanned would be left slower at sequential reads for the
  sake of a measurement that ended hours ago. Name it explicitly and it is
  saved like anything else.
- **It tells you how to undo each change**, per drive, as it makes it.

```
hddscan: sdb: write cache turned on and saved to the drive; it stays that way
         until you run 'sdparm --set=WCE=0 /dev/sdb'
```

### Letting the drive scan itself

SCSI drives can run a **background medium scan**: the firmware sweeps the
platters whenever the drive has been idle for a moment, logs what it finds, and
reallocates the sectors it can still recover. Most drives ship with it off.
Turning it on is one of the better things you can do to a drive you intend to
keep, and `--bms on` does it for every drive selected:

```sh
sudo hddscan --bms on /dev/sdb          # or the 'Background scan' field on the form
```

Two things about it are deliberately unlike everything else here.

**It is not undone afterwards.** Every other setting the tool touches — the
kernel timeout, the drive's look-ahead — is restored on exit, on `SIGINT` and
on error, because a scan has no business leaving your system altered. This one
writes a *saved* mode page and stays on, because a background scan that stopped
when the scan did would be pointless: sweeping a 14 TB drive takes days of idle
time. So it is opt-in, it says so on stderr when it happens, and it prints the
command to reverse it (`sdparm --clear=EN_BMS --save /dev/sdX`).

**It is applied once, before the first read, never during.** A drive seeking on
its own behalf in the middle of a latency measurement is exactly the queueing
measurement the one-request-at-a-time rule exists to avoid.

Worth being honest about what it repairs. Where a sector is still *recoverable*,
the drive can rewrite or reallocate it on its own — a real fix, and the reason
to enable it. Where it isn't, the drive has no idea what the data should have
been, so all it can do is record a pending defect. Turning those into repairs
still takes a write to that LBA, which is `--mode write`, `--rewrite-weak` or
`--force-remap`. Background scanning is proactive detection plus opportunistic
repair, not a substitute for a write pass.

The report shows the state either way, so a drive that isn't checking itself
says so:

```
  Background scan  off: the drive never checks its own media ('--bms on' turns it on for good)
```

### Reformatting

`--format` runs a SCSI FORMAT UNIT on every selected drive, in parallel,
streaming each drive's progress tagged with its name. It is the only way to
change the logical block size, and the only way to add or strip protection
information — which is the fix for a drive whose PI was left behind by a
previous owner.

```sh
sudo hddscan --format --format-pi none --confirm sdb /dev/sdb
sudo hddscan --format --format-blocksize 4096 --confirm sdb /dev/sdb
```

This erases the drive, runs for hours, and cannot be undone; losing power
partway through can leave a drive needing vendor tooling to revive. So it needs
the same `--confirm <name-or-serial>` a write mode does, it refuses a drive
that is mounted or claimed, and **it prints the exact `sg_format` command for
every drive before anything starts**. `--dry-run` prints those commands and
stops, which is the sane way to check what you are about to do:

```
  dry run, would execute:
    sg_format --format --quick --size=4096 --fmtpinfo=3 /dev/sdb
```

`--format-fast` asks the drive not to visit every block, which can turn hours
into minutes. It only makes sense when the *physical* layout is not changing: a
512e drive already has 4096-byte physical sectors, so moving the logical size to
4096 is a remapping rather than a re-lay-out. Blocks never written read without
error afterwards. A drive may refuse it, which costs nothing.

`--format-pi` defaults to `keep`, meaning the protection type the drive already
reports. That default exists because `sg_format` itself defaults `--fmtpinfo`
to 0: a plain "just change the block size" would otherwise strip protection
information as a side effect nobody asked for. The setting is always passed
explicitly for that reason.

### Protection information, and a drive that refuses to be read

Enterprise SAS drives are often formatted with **T10 protection information**:
eight extra bytes beside every logical block holding a CRC guard and tags. The
kernel then reads with `RDPROTECT` set, so the drive checks that guard against
the data on every single read.

A block that has **never been written since the format has no valid guard**.
Reading it fails immediately — `Aborted Command / Logical block guard check
failed`, which the block layer turns into `EILSEQ` — even though the platter
under it is in perfect condition. On a freshly formatted drive that is most of
the device, and a read-only scan of it will hit the wall at the exact offset
the last write reached.

This is easy to mistake for catastrophic media failure, and mistaking it costs
you a working drive. The tells are all there once you know to look: the
failures are *instantaneous* (a struggling sector takes tens to hundreds of
milliseconds, a rejected command takes none), they start on a boundary and
continue forever rather than clustering, and the drive's own SMART reports
nothing wrong.

So `hddscan` reads the format up front — from
`/sys/block/sdX/device/scsi_disk/*/protection_type`, no root and no sg3-utils
needed — and shows it in `--list` and in the report header. `EILSEQ` is
counted as `blocked by protection`, never as a bad sector and never in the
verdict, exactly as `EINVAL` is. And because a skipped block is an *untested*
block, the report carries a `Not covered` line with the byte count and the
verdict box says plainly that it covers nothing about that part of the device.
A clean bill of health over the 2% of a drive that happened to be written is
not a clean bill of health.

To actually test those blocks you have to write to them, which is what makes
the tags valid: `--mode write` covers the whole device and destroys everything
on it. Reformatting without protection (`sg_format --fmtpinfo=0`) does the same
job and the same damage. Neither belongs anywhere near a drive whose contents
you still want.

### Useful flags

```sh
--sample 64                          # fast surface survey, 1 chunk in 64
--order random --segment 4G          # segment-local shuffle
--drive-lookahead keep               # leave the drive's prefetch alone
--bms on                             # SAS: enable the drive's own background media scan (permanent)
--write-cache off                    # time the platter, not the drive's DRAM (restored after)
--persist                            # save the settings this run changed to the drive
--fix-config                         # auto-reallocate on, read cache on, saved
--recovery-time 7000                 # stop the drive stalling an array on one sector
--check-deps                         # what optional tools are missing, and how to install them
--apply-settings                     # configure the drives and stop, no scan
--chunk 1M                           # stop paying a revolution per 128 KiB
--format --format-pi none            # low level format, stripping protection information
--no-tui                             # never open the form, whatever the terminal is
--state /var/tmp/sdb.st --resume     # checkpoint every 15s, survive a reboot
--io-timeout 7                       # bad sectors fail fast instead of hanging
--retry-max-seconds 60               # bound retries on a badly failing drive
--max-temp 55                        # abort if the drive cooks itself
--json r.json --csv bad.csv          # machine-readable results
--badblocks-list bb.txt              # feed straight to 'mke2fs -l bb.txt'
--prometheus /var/lib/node_exporter/textfile_collector/hddscan.prom
--journal /var/tmp/sdb.jrn           # crash-safe verify mode
```

`--badblocks-list` writes bad, corrupt and unstable sectors in badblocks(8)
format so a new filesystem steps around them. Use `--badblocks-blocksize` to
match the filesystem block size and `--badblocks-offset` to subtract the
partition's start, since `mke2fs` runs on the partition rather than the disk.

`--journal` closes the one real danger in verify mode. The original bytes are
written and fsynced to the journal before the test pattern goes down, so a
power cut in that window is undone by the next run, which replays the record
before scanning. Replay is idempotent, so only one fsync per chunk is needed.

## The report

Latency percentiles (p50/p95/p99/p99.9/max) for chunks and for individual
sectors, a bucketed histogram, counts of reads exceeding 25 ms through 5 s, and
a surface map of the platter so you can see whether damage is localised to a
band — which usually means a head or a scratch rather than random wear. Then
SMART attributes from before and after the scan, with anything that *changed
during the run* highlighted: reallocated sectors growing while you watch is the
single most damning signal a drive can give.

Sectors are classified as `recovered` (slow once, clean afterwards), `slow`
(consistently over budget), `unstable` (intermittently slow or failing), `bad`
(unreadable on every attempt) or `corrupt` (returned data that did not match
what was written).

A drive can pass every sector and still fail. One that covers its surface at a
few hundred KiB a second is spending the time somewhere — writes that crawl, or
a chunk over budget nearly every time, each drilled into sector by sector — and
a scan of it will not finish this year. So speed is part of the verdict: a hard
drive that tests below 10 MiB/s at 4 MiB chunks (`--min-rate`) is FAILING. A
working one manages several times that even writing and reading every chunk
back, so this is not a slow drive but a broken one:

```
  sdc   0.0%  439.01 KiB/s  ...  too slow
  sdf   0.1%   29.00 MiB/s  ...  scanning
```

The floor scales *down* with the chunk and never up. A small chunk pays the same
lost revolution per request for less data, so 128 KiB honestly runs far slower,
and a flat floor would condemn healthy drives at the default; 128 KiB gets a
thirty-second of it. Verify mode writes the original back as well, so it gets
three quarters. The rate is the whole scan's, judged only once it has run for
two minutes, and the dashboard says `too slow` from that point on.

What is judged is deliberately narrow. Only a rotational drive on a local bus: a
slow iSCSI link or an SD card is not a failing platter. A USB drive is SUSPECT
instead of FAILING, because a USB 2 bridge alone can hold a write-and-verify
pass near the floor.

Going over budget is not by itself a verdict. A chunk whose sectors all read
cleanly when drilled into was the drive having a moment — a neighbour's
vibration, a thermal recalibration, its own background media scan — and every
drive has those. More than one chunk read in a thousand, or one write in a
thousand, is a habit, and makes the drive SUSPECT; fewer leave it HEALTHY, and
the verdict says how many there were rather than dropping them. At least three
are needed either way, so a short range is not judged on one. A sector that
*stays* slow, needs retries or cannot be read is judged as before, however
rare.

Coverage is stated explicitly in every report — "every sector of the device was
read" versus "SAMPLED" or "PARTIAL" — so a clean verdict is never mistaken for
more than it is.

## Runs, and why they do not live in the terminal

A shelf is not dealt with in one sitting. Thirty drives go into a low level
format, twenty-four more are being scanned, the terminal is wanted for
something else, the ssh session drops, the laptop closes. The work takes hours
to days; nobody sits in front of it.

So a run does not belong to the process that started it:

```
$ hddscan --status
  RUN               KIND    WHAT                   DRIVES PROGRESS  STATE        ELAPSED
  20260912-110000   scan    write (predeploy)          24    46.5%  running      2h 10m
  20260912-100000   format  format 4096B PI type 2     30    47.0%  running      3h 41m
```

**The run store.** Each scan or format gets a directory under
`/var/lib/hddscan/runs/<id>` (or `~/.local/state/hddscan` when not root, or
`--state-dir`). In it: a `run` record saying what the run is and which
supervisor owns it, one `<drive>.job` record per drive, that drive's
checkpoint, and a `log` for anything the run had to say. All of it is plain
key-and-value text, rewritten whole through a temporary file and `rename(2)`,
so a reader never sees half a record and `grep` is a perfectly good client.

**One writer per record.** The worker that holds a drive is the only thing that
writes that drive's record; the supervisor patches in only how the worker
ended, by reading the record and writing it back. Progress is therefore never
somebody's second-hand summary of what a drive is doing — it comes from the
process doing it.

**A supervisor per run.** `run_begin()` forks a process that calls `setsid()`,
so the run leaves the terminal's session entirely. Nothing that happens to the
terminal reaches it: quit the program, close the window, lose the connection,
and thirty formats carry on. The supervisor schedules workers up to
`--max-parallel`, waits, records each verdict, and puts the drive settings back
when the *run* ends — which is the important half. The settings a run changes
(look-ahead, read cache, recovery limits) must not be restored when somebody's
*view* of it exits, or the remaining drives would quietly be measured under
different conditions than the ones already done.

**A shelf does not fit a table.** Thirty drives at one row each need fifty
terminal lines, and a shelf is precisely what this screen is for, so the
dashboard drops to a grid of small cells — name, percentage, bar, several to a
line — whenever the table would be cut off. Less about each drive, every drive:
the right trade when the question is "how is the batch going". `--status` and
`--status-json` always list them in full.

**Every screen is a viewer.** The dashboard reads job records, not pipes.
That is what makes a run this terminal started and a run some other terminal
started the same thing to look at, and it is why `--attach` needs no
cooperation from the process that started the work. The TUI grew a list of
runs — one row each, `enter` to open one, `r` to come back — so swapping
between the thirty being formatted and the twenty-four being scanned is two
keystrokes. Starting hddscan on a machine with work in progress shows that
list before it shows the form, because a form on an empty screen implies an
idle machine.

**Liveness is a pid and its start time.** Pids wrap. A week-old record whose
number has since been handed to something else would otherwise report itself
as still scanning, so every record stores `/proc/<pid>/stat` field 22 beside
the pid and a reader compares both. A run whose supervisor is gone but whose
workers are not is still live — killing the supervisor leaves each worker
reading its own drive to the end. A record that says running whose process is
not there is reported as `orphaned` and the run as `interrupted`, never as
progress that will never move again.

**Stopping is exactly one signal per worker.** The second `SIGINT` a worker
gets is its "stop arguing and get out" path, which abandons the report. So
`--stop` signals the supervisor while there is one and lets it forward once;
the supervisor marks each worker as told rather than re-sending while it waits.
Getting this wrong destroys the thing stopping is meant to preserve: the report
for the part of the drive that *was* covered, with its `Coverage` line saying
how much that was.

A format is the exception that has to be said out loud: `FORMAT UNIT` runs
inside the drive's own firmware. Stopping a format run stops watching it. The
drive keeps going, and the record says so rather than implying the drive went
back to idle.

**What is deliberately not here.** A run interrupted by a reboot is not resumed
automatically. Every drive keeps its own checkpoint inside the run directory,
so the information to do it exists, but restarting work on a shelf of drives is
a decision, not a default.

## Safety

A drive is only scanned if it passes all of:

- not mounted (checked by device number, so `/dev/mapper` and bind mounts are
  caught too, not just by name)
- not an active swap device
- no holders in sysfs — not part of an md array, LVM group, dm target or bcache
- the kernel grants an `O_EXCL` open, which is the authoritative test and also
  reserves the drive for the duration of the scan
- it is not already in a live run. `O_EXCL` only covers the drive a worker
  currently holds open, not one sitting twenty-ninth in a queue, and two runs
  on one spindle time each other's seeks — both answers would be wrong
- rotational (`--include-ssd` to override)
- locally attached (`--include-remote` to override)

That last one matters. An iSCSI LUN, a virtio disk or a Hyper-V disk all report
`rotational=1`. Scanning one measures the network and someone else's page
cache, and a clean result would say nothing about any physical drive.

The drive holding `/` is refused even with `--force`.

## Portability

The source uses no glibc-only APIs — no `asprintf`, `getline`, `qsort_r`,
`alloca` or friends. Everything is POSIX apart from genuinely Linux-specific
things (`O_DIRECT`, `statx`, sysfs, the `BLK*` ioctls), which are kernel
dependencies rather than libc ones. It should build against musl with no
source changes, though that has not been tested here for want of a toolchain.

**The dynamically linked binary is a different matter: it requires glibc 2.38
or newer**, which is Ubuntu 24.04 / Debian 13 era. That is not something the
code asks for. `_GNU_SOURCE` — needed for `O_DIRECT` and `statx` — also switches
on glibc's C23 behaviour, which redirects `strtol`, `strtoll`, `strtoull` and
`sscanf` to `__isoc23_*` symbols that simply do not exist in older glibc. So a
binary built on a current distro will not run on an older one, which matters
precisely when you are trying to test disks from a rescue image.

```sh
make static      # -> hddscan-static, ~1 MB, no libc version floor at all
```

Use that for anything you intend to carry between machines. Build it on the
oldest system you need to support if you would rather stay dynamic.

## Limitations

**Latency is only meaningful on an otherwise idle drive.** Any other I/O to the
same spindle shows up as slow sectors. The `O_EXCL` open prevents mounting but
not a competing raw reader.

**SMART data comes from `smartctl`** and is skipped if smartmontools is absent.
Some USB-SATA bridges do not pass SMART through at all.

**The ATA half of the look-ahead control is untested on real hardware.** It
needs root and an ATA drive. The SAS half — `sdparm` clearing `DRA`, reading it
back, and restoring it on exit — has now been exercised on a real SAS drive and
works. The failure paths on both (tool not installed, wrong transport, drive
refuses, cannot read the setting back) are handled and reported.

**The SCSI error counter and self-test logs are read on SAS.** They live in
log pages `smartctl -A` does not print, so the tool asks for them explicitly.
This is the single most decisive thing a SAS drive will tell you about itself.
The `delayed` column of the read row is the one to look at: it counts data the
drive got back only after extra work, which is precisely what this scan
measures as latency. One real drive, reporting `SMART Health Status: OK` with
a single reallocated sector in its life:

```
read:  0  42899520  0  42899520  481501625  535295.725  0
# 1  Background long   Failed in segment -->  3  24188  - [0x1 0x5d 0xfd]
```

Zero uncorrected errors, so no data was ever lost — and 42.9 million delayed
corrections over 535 TB, about 80,000 per TB read. The drive gets your data
back every time, by grinding for it. ASC `0x5D` on that self-test line is
*failure prediction threshold exceeded*: the drive predicted its own failure
years ago and has reported `OK` ever since, because `OK` only means no
threshold is **currently** exceeded. A failed self-test is now a FAILING
verdict on its own.

**Several paths could not be exercised for lack of hardware**, and are listed
here rather than glossed over: SMR gating (no shingled drive), the SAS mode
pages and `sg_reassign`, and the drive look-ahead success path on ATA.
Their failure and fallback branches are handled and reported; their success
branches have not been watched working.

**The `EIO` path has not been exercised on real failing hardware** — there was
no failing drive and no root access to build one during development. The code
handles read errors, short reads and per-sector retry, but if you want to
verify that before trusting it, build a synthetic bad sector:

```sh
truncate -s 100M /tmp/back.img
LOOP=$(sudo losetup --show -f /tmp/back.img)
printf '0 16384 linear %s 0\n16384 8 error\n16392 188392 linear %s 16392\n' \
    "$LOOP" "$LOOP" | sudo dmsetup create badsect
sudo ./hddscan --no-color /dev/mapper/badsect     # expect 1 unreadable sector
sudo dmsetup remove badsect && sudo losetup -d "$LOOP"
```

## Licence

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 Olav Gjerde.

## What was verified during development

Device and transport detection, mount/swap/holder/`O_EXCL` safety refusal,
chunk timing, drill-down, retry classification, the surface map, all output
formats, checkpoint and resume, `SIGINT` producing a partial report, per-sector
retry capping, parallel execution across 25 simultaneous targets, and runs
surviving the exit of the process that started them.

Three properties were tested end-to-end rather than by inspection, because they
are the ones that would matter most if they were wrong:

- **`--mode verify` really is non-destructive** — md5 before and after a full
  verify pass are identical.
- **Every scan order covers every sector exactly once** — the pattern is
  written in shuffled order and verified in sequential order, at segment sizes
  of 1M, 4M, 7M (deliberately not a power of two), 64M and whole-device. Any
  chunk a permutation skipped would show up as corrupt. None did.
- **Silent corruption is caught at the right offset** — byte-level damage
  injected at three offsets was reported at exactly the right 4 KiB blocks.

The run store was tested the same way, against sparse images — a `truncate -s
300G` file is minutes of genuine work without writing anything, which is the
only way to catch a run in the act. End to end: a run keeps going after the
program that started it exits, a second copy of the program finds it and shows
it mid-scan, a drive already in a live run is refused by any other, `--stop`
leaves every drive that was scanning with the report for the part it covered,
and a record whose process is gone is reported as orphaned rather than as
progress. That last one needs a crash to produce, so it is tested by writing a
record with a pid that does not exist — the same trick that makes the reader
testable without ever killing anything.

What is *not* tested: the format dashboard's feed. `format_child()` parses
`sg_format`'s output, and `--format` refuses a file at the `is a file` gate, so
that path only ever runs against a real SCSI drive. It is verified by
inspection, like everything else behind `--format`.

On real hardware, on a SAS drive: the `sdparm` look-ahead path, and protection
information being detected and reported as a coverage gap rather than as media
failure. The interactive form is driven through a pty in testing — window size
set with `TIOCSWINSZ`, keys written in, escapes stripped, assertions on the
resulting frame — which is how the merged cursor, the two-key confirmation on
writing modes, and the list of runs are checked.
