# hddscan

Finds sectors on a rotating disk that return data **too slowly**, not just ones
that fail outright.

That is the failure SMART misses. A drive can read every sector successfully
and still be dying, because the firmware is spending tens of milliseconds of
internal recovery on each one. `SMART Health Status: OK` keeps saying OK right
up until it doesn't — one drive tested with this tool reported OK with a single
reallocated sector in its life, while its own error log showed 42.9 million
reads that needed the slow recovery path, and its own long self-test had
already failed.

Single-file C, no dependencies beyond libc, so the binary works on a rescue
system with nothing installed. `smartctl`, `sdparm`, `hdparm` and sg3-utils are
used when present and done without when absent.

```sh
make
sudo ./hddscan            # opens the form
```

## Quick start

Run it on a terminal with no arguments and it opens a picker: choose drives,
choose what you are doing, watch a live dashboard. When the scan finishes a
summary holds the verdicts on screen — `n` starts another test, `q` quits. If
something is already running on the machine, it shows you that first.
`--check-deps` says which
optional tools are missing and prints the `dnf`/`apt` line that installs them.

The first question is the **profile**, because what a drive needs tested
depends entirely on what you are about to do with it:

| profile | mode | for |
|---|---|---|
| **predeploy** *(default)* | write | No data on it yet. Writes every sector and verifies it reads back. |
| **inservice** | read | It holds data you want to keep. Never writes anything. |
| **survey** | read, sampled | Quick triage of a shelf. |
| **decay** | check | Weeks after a predeploy run: has the pattern rotted? |
| **repair** | write | predeploy, plus forcing a reallocation of anything unreadable. |

The default writes, because a surface test is normally run before a drive goes
into service and a read-only pass cannot tell you whether a sector will *accept*
a write. So a bare `hddscan /dev/sdb` stops and asks for `--confirm` rather than
doing anything — an error, never data loss.

## Runs keep going without you

A scan or a format is a **run**, and it belongs to a supervisor process of its
own rather than to the terminal that started it. Quit, close the window, lose
the ssh session: the work carries on, and every drive keeps a small text record
of where it has got to.

```sh
sudo hddscan --profile predeploy --confirm sdb --detach /dev/sd{b..z}
sudo hddscan --status                 # every run on this machine
sudo hddscan --status sdq             # the run a given drive is in
sudo hddscan --attach 20260912-110000 # open the dashboard on one
sudo hddscan --stop 20260912-110000   # stop it; partial reports are still written
```

That is what makes a shelf manageable: thirty drives in a low level format and
twenty-four more being scanned are two runs, and the dashboard swaps between
them with one keystroke. Start hddscan again on a machine with work in progress
and it shows you that work before it offers to configure any more.

```
  RUN               KIND    WHAT                   DRIVES PROGRESS  STATE        ELAPSED
  20260912-110000   scan    write (predeploy)          24    46.5%  running      2h 10m
  20260912-100000   format  format 4096B PI type 2     30    47.0%  running      3h 41m
```

Records live in `/var/lib/hddscan` as root, `~/.local/state/hddscan` otherwise,
and are plain key-and-value text — `--status-json` if a script wants them, grep
if you are in a hurry. A drive already in a live run is refused by any other
run, since two of them on one spindle would time each other's seeks.

## Testing

```sh
# a drive with data on it: read-only, never writes
sudo hddscan --profile inservice /dev/sdb

# a drive going into service: writes every sector, verifies, re-reads
sudo hddscan --confirm sdb --state /var/tmp/sdb.state --resume /dev/sdb

# quick triage of a shelf, one keystroke per controller in the form
sudo hddscan --profile survey --all

# weeks later, on a drive an earlier predeploy run wrote
sudo hddscan --profile decay /dev/sdb

# give a drive its best chance: write, rewrite what is still slow,
# force a reallocation of anything unreadable, then re-read the lot
sudo hddscan --repair --confirm sdb --chunk 4M --recovery-time 300 /dev/sdb
```

A full pass over a 14 TB drive takes about a day, and `--repair` roughly twice
that. Use `--state` with `--resume` on anything that long.

Two flags matter more than they look on a drive that is already sick:

```sh
--recovery-time 300     # stop the drive grinding a second per bad sector
--retries 3             # stop the tool grinding twenty attempts per sector
```

Without them a drive with a defect band can spend days re-reading a few
thousand sectors and cover almost none of the surface. The scan now notices
that and says so, but the flags are the fix.

## Low level formatting

`--format` runs a SCSI `FORMAT UNIT` on every selected drive, in parallel. It
erases the drive, runs for hours, and prints the exact `sg_format` command
before it starts. `--dry-run` prints that command and stops. A format is a run
like any other: `--detach` it, come back to it with `--status`, and watch all
thirty on one dashboard fed by what each drive reports about its own progress.

```sh
# see what it would do
sudo hddscan --format --format-pi none --confirm sdb --dry-run /dev/sdb

# strip protection information, keep the sector size
sudo hddscan --format --format-pi none --confirm sdb /dev/sdb

# change the sector size too, and ask for a fast format
sudo hddscan --format --format-blocksize 4096 --format-pi none \
             --format-fast --confirm sdb /dev/sdb
```

`--format-pi` defaults to `keep`, because `sg_format` itself defaults to
*stripping* protection information — so a bare sector size change would remove
it as a side effect nobody asked for.

Worth knowing before you reach for this:

- **Removing protection information can recover real capacity.** Type 2 PI
  stores eight bytes beside every *512-byte* block. On a 512e Seagate Exos this
  took 13,715,978,059,776 bytes to 14,000,519,643,136 — the full nominal 14 TB,
  284 GB back.
- **A PI-formatted drive refuses to read blocks that have never been written**
  since the format, with `EILSEQ`. That is the format talking, not the media,
  and hddscan counts it separately and says the blocks were not tested.
- **Not every drive offers 4096.** One tested here rejects it at the MODE
  SELECT, which costs nothing: the format never starts and the drive is
  untouched.
- **A format cannot be called back.** `FORMAT UNIT` runs inside the drive's own
  firmware, so `--stop` on a format run stops *watching* it. The drive carries
  on, and the record says so instead of implying it went back to idle.

## More

- `man 8 hddscan` — every option, exit codes, the guarantee, protection
  information.
- [DESIGN.md](DESIGN.md) — why it works this way, what was measured on real
  drives, and which paths are still unverified.
- [AGENTS.md](AGENTS.md) — the invariants, and how to change this without
  making it quietly lie about drive health.
- `make test` — 151 tests, no root and no real device required.

## Licence

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 Olav Gjerde.
