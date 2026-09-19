#!/usr/bin/env python3
"""Interactive-form and dashboard tests for hddscan, driven through a pty.

The form prints top to bottom and could be checked by stripping escapes, but
the dashboard draws every row by absolute position, so the order bytes arrive
in is not the order they appear on screen.  Both are therefore replayed into a
screen model and asserted on as a grid -- which is the only way to see a row
the dashboard failed to erase.

Navigation is by field label rather than by counting keypresses, because the
drive list depends on whatever is attached to the machine running the tests.
Real devices are never selected: only image files passed on the command line.
"""
import os, pty, re, select, struct, subprocess, sys, termios, fcntl, time
import tempfile, shutil

HDDSCAN = os.environ.get("HDDSCAN", "./hddscan")
# A store of this suite's own: the tests must not see the machine's real runs
# and must not leave any of their own in the user's state directory.
STATE = tempfile.mkdtemp(prefix="hddscan-tests-")
os.environ["HDDSCAN_STATE_DIR"] = STATE
ROWS, COLS = 24, 112
DOWN, UP, RIGHT, LEFT, TAB = b"\x1b[B", b"\x1b[A", b"\x1b[C", b"\x1b[D", b"\t"
CSI = re.compile(rb"\x1b\[([0-9;?]*)([a-zA-Z])")
# an operating system command -- hddscan asks the terminal for its background
# colour with one -- runs to a BEL or a string terminator and draws nothing
OSC = re.compile(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")

passed = failed = 0


def ok(name):
    global passed
    passed += 1
    print("  \033[32mok\033[0m   %s" % name)


def bad(name, detail=""):
    global failed
    failed += 1
    print("  \033[31mFAIL\033[0m %s\n     %s" % (name, detail))


def check(name, cond, detail=""):
    ok(name) if cond else bad(name, detail)


class Screen:
    """Replay a byte stream the way a terminal would."""

    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.hl = [[False] * cols for _ in range(rows)]
        self.r = self.c = 0
        self.rev = False

    def feed(self, data):
        i = 0
        while i < len(data):
            m = OSC.match(data, i)
            if m:
                i = m.end()
                continue
            m = CSI.match(data, i)
            if m:
                args, cmd = m.group(1), m.group(2)
                nums = [int(x) for x in args.split(b";") if x.isdigit()]
                if cmd == b"H":
                    self.r = (nums[0] - 1) if nums else 0
                    self.c = (nums[1] - 1) if len(nums) > 1 else 0
                elif cmd == b"J" and nums and nums[0] == 2:
                    self.grid = [[" "] * self.cols for _ in range(self.rows)]
                    self.hl = [[False] * self.cols for _ in range(self.rows)]
                elif cmd == b"K":
                    if 0 <= self.r < self.rows:
                        for x in range(self.c, self.cols):
                            self.grid[self.r][x] = " "
                            self.hl[self.r][x] = False
                elif cmd == b"m":
                    if 7 in nums:
                        self.rev = True
                    elif not nums or 0 in nums:
                        self.rev = False
                i = m.end()
                continue
            b = data[i : i + 1]
            if b == b"\n":
                self.r, self.c = self.r + 1, 0
            elif b == b"\r":
                self.c = 0
            elif b == b"\x1b":
                pass
            elif b" " <= b <= b"~" or b >= b"\x80":
                if 0 <= self.r < self.rows and 0 <= self.c < self.cols:
                    self.grid[self.r][self.c] = b.decode("utf-8", "replace")
                    self.hl[self.r][self.c] = self.rev
                self.c += 1
                if self.c >= self.cols:
                    self.r, self.c = self.r + 1, 0
            if self.r >= self.rows:
                self.grid.pop(0); self.grid.append([" "] * self.cols)
                self.hl.pop(0);   self.hl.append([False] * self.cols)
                self.r = self.rows - 1
            i += 1

    def lines(self):
        return ["".join(row).rstrip() for row in self.grid]

    def highlighted(self):
        return [
            "".join(self.grid[r][c] for c in range(self.cols) if self.hl[r][c]).strip()
            for r in range(self.rows)
            if any(self.hl[r])
        ]


class Session:
    def __init__(self, args, rows=ROWS, cols=COLS):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(HDDSCAN, [HDDSCAN] + args)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        self.screen = Screen(rows, cols)
        self.raw = bytearray()
        self.alive = True
        self.pump(0.6)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                d = os.read(self.fd, 65536)
            except OSError:
                self.alive = False
                return
            if not d:
                self.alive = False
                return
            self.raw.extend(d)
            self.screen.feed(d)

    def send(self, keys, settle=0.3):
        os.write(self.fd, keys)
        self.pump(settle)

    def close(self):
        try: os.close(self.fd)
        except OSError: pass
        try: os.waitpid(self.pid, 0)
        except ChildProcessError: pass

    def text(self):
        return CSI.sub(b"", bytes(self.raw)).decode("utf-8", "replace")

    def goto(self, label, limit=40):
        """Move the cursor to the row carrying `label`.

        Down never wraps, so anything above the cursor is unreachable by
        walking down -- rewind to the top first.  Getting this wrong made
        tests fail depending on which check ran before them."""
        if any(label in h for h in self.screen.highlighted()):
            return True
        os.write(self.fd, UP * limit)
        self.pump(0.6)
        for _ in range(limit):
            if any(label in h for h in self.screen.highlighted()):
                return True
            self.send(DOWN, 0.12)
        return any(label in h for h in self.screen.highlighted())

    def wait(self, pred, timeout=4.0):
        """Poll the screen until pred holds.  Fixed sleeps race the redraw --
        the form repaints on its own 200 ms tick, so a key pressed just after
        one repaint is not visible until the next."""
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            self.pump(0.12)
        return pred()

    def set_choice(self, label, want, limit=12):
        """Step a choice field to a value by name.  Counting keypresses from
        an assumed starting value breaks the moment a default changes, which
        is exactly what adding profiles did."""
        if not self.goto(label):
            return False
        for _ in range(limit):
            if self.value_of(label) == want:
                return True
            self.send(RIGHT, 0.14)
            self.wait(lambda: self.value_of(label) is not None, 1.0)
        return self.value_of(label) == want

    def on_screen(self, text):
        return any(text in l for l in self.screen.lines())

    def ensure_selected(self, name):
        """Devices named on the command line arrive already ticked."""
        if not self.goto(name, 14):
            return False
        row = [h for h in self.screen.highlighted() if name in h]
        if row and row[0].lstrip().startswith("[ ]"):
            self.send(b" ", 0.25)
        row = [h for h in self.screen.highlighted() if name in h]
        return bool(row) and row[0].lstrip().startswith("[x]")

    def value_of(self, label, scroll=True):
        for line in self.screen.lines():
            if label in line and "<" in line:
                return line.split("<", 1)[1].split(">", 1)[0].strip()
        # the settings list scrolls on a short terminal, so a field can be
        # off screen; move to it and look again
        if scroll and self.goto(label):
            return self.value_of(label, scroll=False)
        return None


def drawn_widths(raw):
    """How wide each row was as the program wrote it, from one cursor move to
    the next.  A Screen wraps at its own width and the next absolute move then
    writes over the spill, so a row wider than the terminal cannot be seen in
    the grid at all -- it can only be measured in the stream."""
    widths = []
    for seg in re.split(rb"\x1b\[\d+;\d+H", bytes(raw))[1:]:
        seg = CSI.sub(b"", OSC.sub(b"", seg))
        seg = re.split(rb"[\r\n]", seg)[0]
        widths.append(len(seg.decode("utf-8", "replace")))
    return widths


def img(d, name, mb):
    # synced, as in cli.sh: an O_DIRECT read of pages still waiting for
    # writeback waits with them, and a scan would time the wait
    p = os.path.join(d, name)
    with open(p, "wb") as f:
        f.write(os.urandom(mb << 20))
        f.flush()
        os.fsync(f.fileno())
    return p


def main():
    tmp = tempfile.mkdtemp()
    try:
        a = img(tmp, "a.bin", 12)
        b = img(tmp, "b.bin", 12)

        print("== the form opens and can be navigated ==")

        s = Session([])
        check("a bare invocation on a terminal opens the picker",
              s.wait(lambda: s.on_screen("configure a run")),
              "\n".join(s.screen.lines()[:4]))
        check("the drive list is shown",
              s.wait(lambda: s.on_screen("Drives")))
        s.send(b"q"); s.close()

        # REGRESSION: drives and settings used to be separate sections that
        # Tab switched between; down had to cross the boundary by itself.
        s = Session(["-i", a])
        crossed = s.goto("Mode")
        check("down arrow crosses from the drive list into the settings",
              crossed, "never reached the Mode field with the down arrow")
        s.send(b"q"); s.close()

        s = Session(["-i", a])
        s.send(TAB, 0.3)
        check("tab still jumps to the settings for anyone with the habit",
              any("Mode" in h or "Settings" in h for h in s.screen.highlighted()))
        s.send(b"q"); s.close()

        print("== the form opens on a profile, and the profile sets the rest ==")

        s = Session(["-i", "--no-color", "--outdir", tmp, a])
        check("the form defaults to the predeploy profile",
              s.wait(lambda: s.value_of("Profile") == "predeploy"),
              "got %r" % s.value_of("Profile"))
        check("predeploy means the mode writes",
              s.wait(lambda: s.value_of("Mode") == "write"),
              "got %r" % s.value_of("Mode"))
        check("the profile explains itself in words",
              s.wait(lambda: s.on_screen("no data on it yet")),
              "\n".join(s.screen.lines()))
        # predeploy leaves the drive configured for service, and the form
        # has to show the same defaults the command line uses
        for field, want in (("Chunk size", "1M"), ("Write cache", "off"),
                            ("Recommended config", "yes"),
                            ("Background scan", "enable"),
                            ("Scan interval (hours)", "168"),
                            ("Save to drive", "this run")):
            check("predeploy sets %s to %s" % (field, want),
                  s.wait(lambda: s.value_of(field) == want),
                  "got %r" % s.value_of(field))
        check("a read pass under predeploy saves nothing",
              s.set_choice("Mode", "read")
              and s.wait(lambda: s.value_of("Recommended config") == "no")
              and s.value_of("Background scan") == "keep"
              and s.value_of("Write cache") == "keep",
              "config %r, bms %r, wc %r" % (
                  s.value_of("Recommended config"),
                  s.value_of("Background scan"), s.value_of("Write cache")))
        check("going back to write restores them",
              s.set_choice("Mode", "write")
              and s.wait(lambda: s.value_of("Recommended config") == "yes"),
              "config %r" % s.value_of("Recommended config"))
        check("switching to inservice makes the mode read-only",
              s.set_choice("Profile", "inservice")
              and s.wait(lambda: s.value_of("Mode") == "read"),
              "mode %r" % s.value_of("Mode"))
        check("switching to survey turns sampling on",
              s.set_choice("Profile", "survey")
              and s.wait(lambda: s.value_of("Sample 1 chunk in N") == "64"))
        check("leaving survey puts the chunk size back",
              s.set_choice("Profile", "decay")
              and s.wait(lambda: s.value_of("Chunk size") == "128K"),
              "chunk %r" % s.value_of("Chunk size"))
        s.send(b"q"); s.close()

        print("== the form is grouped by what a setting changes ==")

        # Tall enough for every setting at once on any machine: the drive
        # list above them holds the machine's own drives too, up to eight
        # rows, and at forty rows a machine with four of them scrolled
        # "Save to drive" out of sight.
        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, a], rows=48, cols=110)
        s.wait(lambda: s.on_screen("Protection info"))
        lines = s.screen.lines()
        idx = lambda t: next((i for i, l in enumerate(lines) if t in l), -1)
        check("the form has a section for how the scan runs",
              idx("-- How to test --") >= 0)
        check("the form has a separate section for drive settings",
              idx("Drive settings (sdparm/hdparm") >= 0)
        check("drive settings say they are put back afterwards",
              s.on_screen("restored on exit"))
        check("test settings come before drive settings",
              -1 < idx("-- How to test --") < idx("Drive settings (sdparm"))
        # REGRESSION: the format options were reported as missing twice --
        # once when they sat three screens down, and again when they were
        # hidden until Mode was set to format, because nobody looking for
        # them knows they are behind a value of Mode.  They are a section of
        # their own now, on the form from the start, and they do not scroll.
        check("the form has a separate section for low level format",
              idx("Low level format") >= 0 and idx("Sector size") >= 0,
              "\n".join(lines))
        check("the format section comes after the drive settings",
              -1 < idx("Drive settings (sdparm") < idx("Low level format"),
              "\n".join(lines))
        for t, want_test in (("Profile", True), ("Mode", True),
                             ("Chunk size", True), ("Scan order", True),
                             ("Retries per sector", True),
                             ("Sample 1 chunk in N", True),
                             ("Drive look-ahead", False), ("Read cache", False),
                             ("Write cache", False), ("Auto-reallocate", False),
                             ("Background scan", False),
                             ("Save to drive", False)):
            where = idx(t)
            in_test = -1 < where < idx("Drive settings (sdparm")
            check("%s is in the %s section" % (t, "test" if want_test else "drive"),
                  where >= 0 and in_test == want_test,
                  "row %d, boundary %d" % (where, idx("Drive settings (sdparm")))
        s.send(b"q"); s.close()

        print("== format options on the form ==")

        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, a], rows=42, cols=112)
        check("the format section is there before format is chosen",
              s.wait(lambda: s.on_screen("Low level format")
                     and s.on_screen("Protection info")),
              "\n".join(s.screen.lines()))
        check("choosing format says the section is now in use",
              s.set_choice("Mode", "format")
              and s.wait(lambda: s.on_screen("Mode is format")),
              "\n".join(s.screen.lines()))
        # choosing format used to move the cursor into the section, so the
        # next right-arrow meant for Mode changed the sector size instead
        check("choosing format leaves the cursor on Mode",
              any("Mode" in h for h in s.screen.highlighted()),
              s.screen.highlighted())
        check("format options come after the drive settings",
              next((i for i, l in enumerate(s.screen.lines())
                    if "Drive settings (sdparm" in l), -1)
              < next((i for i, l in enumerate(s.screen.lines())
                      if "Low level format" in l), -1))
        check("sector size offers the drive's two real choices",
              s.set_choice("Sector size", "512")
              and s.set_choice("Sector size", "4096"),
              "got %r" % s.value_of("Sector size"))
        check("sector size can be left alone",
              s.set_choice("Sector size", "keep"))
        # sg_format defaults --fmtpinfo to 0, so a bare block size change would
        # strip protection as a side effect; the form must default to keep
        check("protection defaults to keeping what the drive has",
              s.value_of("Protection info") == "keep",
              "got %r" % s.value_of("Protection info"))
        check("protection can be dropped or set to a type",
              s.set_choice("Protection info", "none")
              and s.set_choice("Protection info", "type 2"),
              "got %r" % s.value_of("Protection info"))
        check("a fast format is off unless asked for",
              s.value_of("Fast format") == "no",
              "got %r" % s.value_of("Fast format"))
        check("a fast format can be requested",
              s.set_choice("Fast format", "yes"))
        check("leaving format keeps the section on the form",
              s.set_choice("Mode", "read")
              and s.wait(lambda: s.on_screen("used when Mode is set to format")
                         and s.on_screen("Sector size")),
              "\n".join(s.screen.lines()))
        s.send(b"q"); s.close()

        # REGRESSION: the check above runs on 42 rows, which is why this
        # shipped.  On an ordinary 24x80 terminal choosing format scrolled
        # only as far as the section's first field, so the heading and
        # "Sector size" sat on the last line and the protection setting --
        # the one sg_format silently strips if nobody looks -- was below
        # "more below".  The section was reported as missing.
        #
        # Mode is stepped with raw keys, watching the selection line's
        # warning: set_choice() reads a value back by walking the cursor to
        # it, which scrolls the list straight back up and hides exactly what
        # this is checking for.
        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, a], rows=24, cols=80)
        section = ("Low level format", "Sector size", "Protection info",
                   "Fast format")
        # the report that started this: a 24x80 form, nothing chosen yet
        check("the whole format section is on a 24x80 form from the start",
              s.wait(lambda: all(s.on_screen(t) for t in section)),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        s.goto("Mode")
        for _ in range(8):
            if s.on_screen("ERASES the drive completely"):
                break
            s.send(RIGHT, 0.3)
        check("choosing format on a 24x80 terminal shows the whole section",
              s.wait(lambda: all(s.on_screen(t) for t in section)),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        # a row that wraps is a row the form's budget did not count, and
        # the terminal answers by scrolling the top of the form away
        check("the form fits a 24x80 terminal with format chosen",
              s.on_screen("configure a run") and s.on_screen("q quit"),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        s.send(b"q"); s.close()

        # the default profile writes AND saves settings, and its warnings
        # side by side once ran past eighty columns
        s = Session(["-i", "--no-color", "--dry-run", "--outdir", tmp, a],
                    rows=24, cols=80)
        check("predeploy's footer fits a 24x80 terminal",
              s.wait(lambda: s.on_screen("saves drive settings"))
              and s.on_screen("configure a run") and s.on_screen("q quit"),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        s.send(b"s", 0.2)
        check("predeploy's confirmation fits a 24x80 terminal",
              s.wait(lambda: s.on_screen("and save settings, other keys"))
              and s.on_screen("configure a run"),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        s.send(b"n", 0.2)
        s.send(b"q"); s.close()

        print("== bulk selection, for a shelf rather than a drive ==")

        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, a, b])
        # the form draws top to bottom, so the heading is on screen before
        # the rows under it: wait for the rows themselves
        picked = lambda: [l for l in s.screen.lines() if "[" in l and "]" in l]
        check("the picker shows which controller each drive is on",
              s.wait(lambda: any(re.search(r"\[.\]\s+\S+\s+(host\d+|file)",
                                           l) for l in picked())),
              "\n".join(picked()))
        # The drive list scrolls once the machine's own drives and these two
        # do not fit, so what a key selected is read from the count under
        # the settings, which is always on the form, rather than from
        # whichever ticks happen to be in view.
        def nsel():
            m = [re.search(r"(\d+) drives? selected", l)
                 for l in s.screen.lines()]
            m = [x for x in m if x]
            return int(m[0].group(1)) if m else -1

        s.send(b"n", 0.3)
        check("n clears the selection", s.wait(lambda: nsel() == 0),
              "%d selected" % nsel())
        s.send(b"a", 0.3)
        check("a selects every free HDD", s.wait(lambda: nsel() >= 2),
              "%d selected" % nsel())
        s.send(b"n", 0.3)
        s.goto(os.path.basename(a), 14)
        s.send(b"g", 0.3)
        s.wait(lambda: nsel() == 2)
        picked = [l for l in s.screen.lines() if "[x]" in l]
        check("g selects every free drive on that controller",
              nsel() == 2 and picked and all("file" in l for l in picked),
              "%d selected\n%s" % (nsel(), "\n".join(picked)))
        check("g does not select drives that are in use",
              not any("IN USE" in l for l in picked), "\n".join(picked))
        s.send(b"q"); s.close()

        print("== the form fits a small terminal ==")

        s = Session(["-i", "--no-color", "--outdir", tmp, a], rows=24, cols=112)
        check("the profile description is visible on a 24 row terminal",
              s.wait(lambda: s.on_screen("no data on it yet")),
              "\n".join(s.screen.lines()))
        check("the start/quit hint is visible on a 24 row terminal",
              s.wait(lambda: s.on_screen("q quit")),
              "\n".join(s.screen.lines()[-4:]))
        check("the settings say when more are scrolled off",
              s.wait(lambda: s.on_screen("more below")))
        s.send(b"q"); s.close()

        print("== the mode a form shows is the mode it runs ==")

        # REGRESSION, and the worst one: the choice list was
        # {read,check,verify,write} but the enum is {READ,VERIFY,WRITE,CHECK},
        # and the index was cast straight to the enum.  Picking "check" ran a
        # verify pass, and picking "verify" ran a destructive write.
        os.system("%s --no-color --mode write --confirm %s %s >/dev/null 2>&1"
                  % (HDDSCAN, a, a))
        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, a])
        s.goto("Mode")
        check("the form can be set to 'check'",
              s.set_choice("Mode", "check"),
              "got %r" % s.value_of("Mode"))
        s.send(b"s", 0.5)
        s.pump(8)
        s.close()
        t = s.text()
        check("choosing 'check' runs a read-only re-check",
              "read-only re-check" in t,
              [l for l in t.splitlines() if l.strip().startswith("Mode ")][:1])
        check("choosing 'check' does not run a verify pass",
              "non-destructive read/write verify" not in t)

        print("== destructive choices take two keys ==")

        for mode, presses, word, warn in (
                ("write", 3, "write", "writes to the drive"),
                ("repair", 4, "repair", "overwrites everything"),
                ("format", 5, "format", "ERASES the drive completely"),
                ("hide bad", 7, "hide bad", "hides the last scan's damage")):
            s = Session(["-i", "--no-color", "--dry-run", "--outdir", tmp, a])
            s.set_choice("Mode", word)
            shown = s.value_of("Mode")
            # the warning is the only thing standing between a mistyped
            # keystroke and a wiped drive, so assert the exact wording
            check("choosing %s warns what it will do" % mode,
                  s.wait(lambda: s.on_screen(warn)),
                  "no line contained %r; mode shown %r" % (warn, shown))
            s.send(b"s", 0.2)
            armed = s.wait(lambda: s.on_screen("press y"))
            check("%s arms a confirmation instead of starting" % mode,
                  shown == word and armed,
                  "mode shown %r, armed %s" % (shown, armed))
            check("the %s confirmation names the mode it will run" % mode,
                  s.on_screen("press y to start a %s" % word),
                  [l for l in s.screen.lines() if "press y" in l])
            s.send(b"n", 0.2)
            check("a key other than y cancels the %s" % mode,
                  s.wait(lambda: s.on_screen("q quit")))
            s.send(b"q"); s.close()

        # REGRESSION: --format had no way in from the form at all.
        s = Session(["-i", "--no-color", "--dry-run", "--outdir", tmp, a])
        s.set_choice("Mode", "format")
        s.send(b"s", 0.45); s.send(b"y", 0.8)
        s.pump(3)
        s.close()
        t = s.text()
        check("format chosen on the form reaches the format path",
              "--format is a SCSI FORMAT UNIT" in t, t[-300:])
        # and on a real device it prints the command first; check the gate
        # ordering here instead, which is what protects the device
        # "Low level format" is also the form's section heading now, so match
        # the banner's own wording rather than the words it shares
        check("format refuses before touching anything",
              "Low level format of" not in t, t[-300:])

        print("== settings on the form reach the scan ==")

        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, a])
        if s.goto("Chunk size"):
            for _ in range(3):
                s.send(RIGHT, 0.16)          # 128K -> 1M
            s.wait(lambda: s.value_of("Chunk size") == "1M")
            picked = s.value_of("Chunk size")
            s.send(b"s", 0.5); s.pump(14); s.close()
            rep = ""
            for fn in os.listdir(tmp):
                if fn.startswith("hddscan-") and fn.endswith(".txt"):
                    rep += open(os.path.join(tmp, fn)).read()
            body = s.text() + rep
            check("the chunk size chosen on the form is the one scanned",
                  picked == "1M" and "1024 KiB / 4 KiB" in body,
                  "picked %r; report had %r" % (
                      picked,
                      [l for l in body.splitlines() if "Chunk / block" in l][:1]))
        else:
            bad("the chunk size chosen on the form is the one scanned",
                "no Chunk size field")

        # REGRESSION: a numeric field whose floor is the "keep" sentinel used
        # to display -1 and step to -1+step instead of to a round number.
        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, a])
        if s.goto("Recovery limit"):
            check("a numeric field at its floor reads 'keep', not -1",
                  s.wait(lambda: s.value_of("Recovery limit") == "keep"),
                  "got %r" % s.value_of("Recovery limit"))
            s.send(RIGHT, 0.15)
            check("stepping off 'keep' lands on 0, not on -1 plus the step",
                  s.wait(lambda: s.value_of("Recovery limit") == "0"),
                  "got %r" % s.value_of("Recovery limit"))
            s.send(LEFT, 0.15)
            check("stepping back returns to 'keep'",
                  s.wait(lambda: s.value_of("Recovery limit") == "keep"))
        else:
            bad("recovery limit sentinel", "no Recovery limit field")
        s.send(b"q"); s.close()

        print("== the dashboard ==")

        # REGRESSION: msg() wrote straight to the terminal during the
        # dashboard, scrolling it, and the dashboard never erased its own
        # blank separator rows -- so fragments of old frames stayed forever.
        big_a, big_b = img(tmp, "p.bin", 64), img(tmp, "q.bin", 64)
        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, "--chunk-slow-ms", "0.05",
                     "--retries", "8", big_a, big_b])
        sel = [s.ensure_selected(os.path.basename(big_a)),
               s.ensure_selected(os.path.basename(big_b))]
        check("both image drives can be selected on the form", all(sel), sel)
        s.send(b"s", 0.6)

        # The dashboard no longer lingers once the last drive finishes -- the
        # summary screen replaces it -- and an image scan finishes inside one
        # poll tick, so the dashboard cannot be caught live.  Its final frame
        # is still in the byte stream: replay everything up to the cursor-home
        # that starts the summary's first frame.  Asserting on live values
        # would be a race anyway; the values themselves are covered by the
        # progress-line test below.  What is pinned here is that a finished
        # drive's second row says what the drive is rather than holding the
        # latency it had while it was still running.  Which drives the last
        # frame caught finished is a race too -- this check once passed on a
        # frame whose only drive was still scanning, because a single read
        # was not yet enough to show -- so it looks only at rows that say
        # they are done, and the section on drilling drives below pins the
        # same thing with a record written by hand.
        s.wait(lambda: s.on_screen("finished"), 12.0)
        raw = bytes(s.raw)
        fin = raw.find(b"finished")
        cut = raw.rfind(b"\x1b[1;1H", 0, fin) if fin >= 0 else -1
        dash = Screen(ROWS, COLS)
        if cut > 0:
            dash.feed(raw[:cut])
        lines = ["".join(row).rstrip() for row in dash.grid]

        def rows_now():
            h = next((k for k, l in enumerate(lines)
                      if "DRIVE" in l and "STATE" in l), -1)
            return lines[h + 1:h + 8] if h >= 0 else []

        done_rows = rows_now()
        check("a drive the last frame shows finished has no latency under it",
              bool([l for l in done_rows if l.strip()]) and
              not any(re.search(r"HEALTHY|SUSPECT|FAILING|STOPPED", l) and
                      " med " in done_rows[k + 1]
                      for k, l in enumerate(done_rows[:-1])),
              "\n".join(done_rows or lines[:14]))
        hdr = [i for i, l in enumerate(lines) if "DRIVE" in l and "STATE" in l]
        if not hdr:
            bad("the dashboard renders a drive table", "\n".join(lines[:14]))
        else:
            i = hdr[0]
            ok("the dashboard renders a drive table")
            check("a blank row separates the table from the summary",
                  "" in (lines[i - 1], lines[i - 2]),
                  "rows above header: %r" % (lines[max(0, i - 3):i],))
            check("no drive row appears above the table header",
                  not any(re.match(r"\s+\S+\s+\d+\.\d%\s+\S+/s", l) for l in lines[:i]),
                  "\n".join(lines[:i]))
            check("only one aggregate line is on screen",
                  sum("aggregate" in l for l in lines) == 1,
                  "\n".join(l for l in lines if "aggregate" in l))
            # REGRESSION: the weak/slow count was only ever shown as a
            # total, so with two drives you could not tell which one was
            # producing it.
            check("the table has a per-drive WEAK column",
                  "WEAK" in lines[i], lines[i])
            # WEAK and SLOW were two columns splitting one idea, and the
            # split was never actionable: one column now, and the report
            # carries the tries and errors behind each sector.
            check("the table does not split weak sectors into two columns",
                  "SLOW" not in lines[i], lines[i])
            # the header carries the first row of a record; the second is
            # labelled in place, and is checked against a running drive
            # further down
            for col in ("PCT", "RATE", "ETA", "STATE"):
                check("the table has a %s column" % col,
                      col in lines[i], lines[i])
            check("the table says what window and unit its numbers use",
                  any("over the last 30 seconds" in l for l in lines))
            check("the table has a RATE column",
                  "RATE" in lines[i], lines[i])

        # A scan that ran for days must not scroll its result away the moment
        # it finishes: the summary holds the screen until a key, and 'n' goes
        # back to the form for another test instead of exiting.
        check("the summary screen holds when every drive finishes",
              s.on_screen("finished") and s.on_screen("n new test") and
              s.on_screen("q quit"),
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        check("the summary lists a verdict per drive",
              sum(("HEALTHY" in l) or ("SUSPECT" in l) or ("FAILING" in l)
                  for l in s.screen.lines()) >= 2,  # one row per image
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        s.send(b"n", 0.6)
        check("'n' on the summary returns to the form for another test",
              s.wait(lambda: s.on_screen("Profile"), 6.0),
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        s.send(b"q", 0.5)
        s.pump(1.5)
        check("q on the re-entered form exits", not s.alive)
        s.close()

        print("== hiding a drive's damage from the summary ==")

        # A scan that found damage offers 'h' on its summary; y hands that
        # drive to --hide-bad.  An image gets its map written and stops short
        # of activating, which needs a loop device -- so the map is read back
        # with 'hddscan dm' to prove the key did the real thing.
        def patterned(name):
            p = img(tmp, name, 16)
            subprocess.run([HDDSCAN, "--no-color", "--outdir", tmp, "--mode",
                            "write", "--confirm", p, p],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return p

        hurt = patterned("hurt.bin")
        with open(hurt, "r+b") as f:
            f.seek(8 << 20)
            f.write(b"BADBADBAD")
        s = Session(["-i", "--no-color", "--profile", "inservice", "--mode",
                     "check", "--outdir", tmp, hurt])
        s.send(b"s", 0.6)
        s.wait(lambda: s.on_screen("finished"), 12.0)
        check("a summary with damage on it offers h",
              s.wait(lambda: s.on_screen("h hide bad blocks")),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        check("and points at the drive it would act on",
              any(l.startswith(">") and "hurt.bin" in l
                  for l in s.screen.lines()),
              "\n".join(l for l in s.screen.lines() if "hurt" in l))
        s.send(b"h", 0.3)
        check("h asks before doing anything",
              s.wait(lambda: s.on_screen("press y to hide the damage")))
        s.send(b"n", 0.3)
        check("any other key backs out",
              s.wait(lambda: s.on_screen("h hide bad blocks")))
        before = subprocess.run([HDDSCAN, "dm", "status", hurt],
                                capture_output=True, text=True)
        check("and has written nothing",
              "no dm-badblocks map" in before.stderr, before.stderr)
        s.send(b"h", 0.3)
        s.wait(lambda: s.on_screen("press y to hide the damage"))
        s.send(b"y", 0.3)
        check("y hides the damage and says how it went",
              s.wait(lambda: s.on_screen("press any key to return"), 8.0)
              and s.on_screen("needs a loop device") and s.on_screen("done"),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        after = subprocess.run([HDDSCAN, "dm", "status", hurt],
                               capture_output=True, text=True)
        check("the map on the drive skips the extent the scan found bad",
              "1 extent bad when the map was made" in after.stdout,
              after.stdout + after.stderr)
        s.send(b" ", 0.3)
        check("a key returns to the summary, which no longer offers h",
              s.wait(lambda: s.on_screen("n new test"))
              and not s.on_screen("hide bad blocks"),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        s.send(b"q", 0.5)
        s.close()

        clean = patterned("clean.bin")
        s = Session(["-i", "--no-color", "--profile", "inservice", "--mode",
                     "check", "--outdir", tmp, clean])
        s.send(b"s", 0.6)
        s.wait(lambda: s.on_screen("n new test"), 12.0)
        check("a clean drive's summary offers nothing to hide",
              not s.on_screen("hide bad blocks")
              and not any(l.startswith(">") for l in s.screen.lines()),
              "\n".join(l for l in s.screen.lines() if l.strip()))
        s.send(b"q", 0.5)
        s.close()

        print("== a run's dispatch flags do not leak into the next ==")

        # The form comes back after every run, so an accept must not inherit
        # the previous accept's repair/format/apply-settings flag.  A leftover
        # repair flag would silently turn a freshly chosen read pass into a
        # write pass -- the exact failure is a read that modifies the image.
        import hashlib
        lk = img(tmp, "leak.bin", 8)
        s = Session(["-i", "--no-color", "--outdir", tmp, lk])
        s.wait(lambda: s.on_screen("Profile"), 6.0)
        s.set_choice("Mode", "repair")
        s.send(b"s", 0.5)
        s.wait(lambda: s.on_screen("press y"), 3.0)
        s.send(b"y", 0.5)
        check("the repair run reaches its summary",
              s.wait(lambda: s.on_screen("finished"), 40.0))
        h1 = hashlib.md5(open(lk, "rb").read()).hexdigest()
        s.send(b"n", 0.6)
        s.wait(lambda: s.on_screen("Profile"), 6.0)
        s.set_choice("Mode", "read")
        s.send(b"s", 0.5)
        check("a read pass after a repair run needs no write confirmation",
              not s.wait(lambda: s.on_screen("press y"), 1.5))
        s.wait(lambda: s.on_screen("finished"), 20.0)
        h2 = hashlib.md5(open(lk, "rb").read()).hexdigest()
        check("a read pass after a repair run does not write to the drive",
              h1 == h2)
        s.send(b"q", 0.5)
        s.pump(1.0)
        s.close()

        print("== format against a real drive (opt in) ==")

        real = os.environ.get("HDDSCAN_TEST_DEVICE")
        if not real:
            print("  \033[33mskip\033[0m format command construction "
                  "(set HDDSCAN_TEST_DEVICE=/dev/sdX, root, --dry-run only)")
        else:
            name = os.path.basename(real)
            s = Session(["-i", "--no-color", "--dry-run", "--outdir", tmp,
                         real])
            s.ensure_selected(name)
            check("format is selected for %s" % real,
                  s.set_choice("Mode", "format"))
            s.send(b"s", 0.5); s.send(b"y", 1.0)
            s.pump(4); s.close()
            t = s.text()
            check("the format banner warns before anything runs",
                  "Low level format" in t and "erases everything" in t,
                  t[-300:])
            check("the exact sg_format command is printed",
                  "sg_format --format --quick --size=" in t, t[-300:])
            check("--dry-run stops before executing it",
                  "dry run, would execute" in t, t[-300:])

            # the format options chosen on the form must reach the command
            s = Session(["-i", "--no-color", "--dry-run", "--outdir", tmp,
                         real], rows=42, cols=112)
            s.ensure_selected(name)
            s.set_choice("Mode", "format")
            s.wait(lambda: s.on_screen("Sector size"))
            s.set_choice("Sector size", "4096")
            s.set_choice("Protection info", "none")
            s.send(b"s", 0.5); s.send(b"y", 1.0)
            s.pump(4); s.close()
            t = s.text()
            check("the sector size chosen on the form reaches sg_format",
                  "--size=4096" in t, t[-300:])
            check("the protection chosen on the form reaches sg_format",
                  "--fmtpinfo=0" in t, t[-300:])
            check("a form-chosen format is not fast unless asked",
                  "--ffmt" not in t, t[-300:])

        print("== the plain progress line carries the same window ==")

        s = Session(["--no-tui", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, a])
        s.pump(4)
        txt = s.text().replace("\r", "\n")
        prog = [l for l in txt.splitlines() if "elapsed" in l and "%" in l]
        check("the progress line reports a rolling latency window",
              any(re.search(r"last \d+s med \d+\.\d avg \d+\.\d "
                            r"min \d+\.\d max \d+\.\d ms", l)
                  for l in prog),
              "\n".join(prog[-2:]) if prog else "no progress line seen")
        # a short scan must say how long the window really is, not claim 30s
        check("the window states the span it actually covers",
              any(re.search(r"last \d+s med", l) for l in prog),
              "\n".join(prog[-2:]) if prog else "none")
        s.close()

        print("== a run outlives the program that started it ==")

        # Sparse images make a scan that takes minutes without writing
        # terabytes, so the run can be caught in the act -- which is the only
        # way to test that leaving it alone really does leave it running.
        big = [os.path.join(tmp, "sparse%d.bin" % k) for k in (1, 2, 3)]
        for path in big:
            with open(path, "wb") as f:
                f.truncate(300 << 30)

        def status(*args):
            return subprocess.run([HDDSCAN, "--no-color", "--status"] +
                                  list(args), capture_output=True,
                                  text=True).stdout

        s = Session(["-i", "--no-color", "--profile", "inservice",
                     "--outdir", tmp, "--max-parallel", "2"] + big,
                    rows=26, cols=104)
        for path in big:
            s.ensure_selected(os.path.basename(path))
        s.send(b"s", 0.8)
        check("the dashboard says the run does not need this terminal",
              s.wait(lambda: s.on_screen("the run keeps going either way")),
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        check("a drive waiting its turn is shown as queued, not at 0%",
              s.wait(lambda: any("queued" in l for l in s.screen.lines())),
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        # q on the dashboard is 'I am done looking', not 'stop the drives'.
        # Anything else and closing a window would throw away a day's scan.
        s.send(b"q", 0.8)
        # poll rather than settle: how long the program takes to put the
        # terminal back depends on what the machine is doing
        check("q leaves the dashboard", s.wait(lambda: not s.alive, 6.0))
        s.close()
        out = status()
        rid = re.search(r"^\s+(\d{8}-\d{6}\S*)", out, re.M)
        rid = rid.group(1) if rid else ""
        check("the run is still going after the program exited",
              "running" in out and bool(rid), out[:400])

        # Starting again must show the work in progress before offering to
        # configure more of it: a form on an empty screen implies an idle
        # machine, and this one is not idle.
        # the images are named again so the picker has them to show; a drive
        # in a live run has to appear there as unavailable rather than simply
        # not appear, or the reason it cannot be chosen is invisible
        s = Session(["-i", "--no-color", "--outdir", tmp] + big,
                    rows=26, cols=104)
        check("a fresh start lists the runs already going",
              s.wait(lambda: s.on_screen("runs on this machine")),
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        check("the run this machine is busy with is on the list",
              s.on_screen(rid), "\n".join(s.screen.lines())[:400])
        s.send(b"\r", 1.0)
        check("opening a run shows its drives mid-scan",
              s.wait(lambda: s.on_screen("scanning")),
              "\n".join(l for l in s.screen.lines() if l.strip())[:600])
        check("watching a run offers the way back to the list",
              s.on_screen("r runs"),
              "\n".join(l for l in s.screen.lines() if l.strip())[-200:])
        # 'n' from the dashboard goes on to configure another test while this
        # run carries on, which is how a second batch of drives gets started.
        s.send(b"n", 1.0)
        check("a run can be left for the form to start another",
              s.wait(lambda: s.on_screen("configure a run")),
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        # REGRESSION: both of these were asserted the moment the form's
        # heading appeared, before the rows under it had been drawn, and
        # failed in CI whenever the frame arrived in two reads
        check("the form says work is already in progress",
              s.wait(lambda: s.on_screen("already in progress")),
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        # the picker's name column is 8 wide, so match what it shows; and
        # it scrolls when the machine has drives of its own, so walk to it
        s.wait(lambda: s.on_screen("q quit"))
        s.goto("sparse1")
        check("a drive already in a run is not offered as free",
              s.wait(lambda: any("sparse1" in l and "IN USE" in l
                                 for l in s.screen.lines())),
              "\n".join(l for l in s.screen.lines() if "sparse" in l))
        s.send(b"r", 0.8)
        check("'r' on the form goes back to the list of runs",
              s.wait(lambda: s.on_screen("runs on this machine")),
              "\n".join(l for l in s.screen.lines() if l.strip())[:400])
        # x asks first: stopping thirty drives by mistyping a key is not a
        # thing this should allow.
        s.send(b"x", 0.6)
        check("stopping a run from the list asks first",
              s.wait(lambda: s.on_screen("press y to stop")),
              "\n".join(l for l in s.screen.lines() if l.strip())[-300:])
        s.send(b"y", 1.5)
        check("the run stops when told to",
              s.wait(lambda: "running" not in status(), 12.0),
              status()[:400])
        s.send(b"q", 0.5)
        s.close()
        # REGRESSION: every drive that was mid-scan still owes a report for
        # what it did cover; a stop is not an abandonment.  A stop that
        # reached a worker twice hit its hard-exit path and wrote nothing.
        rep = os.path.join(tmp, "hddscan-sparse1.bin.txt")
        check("a stopped drive still wrote its report",
              os.path.exists(rep) and os.path.getsize(rep) > 0,
              "missing or empty: %s" % rep)

        print("== a shelf too big for a table ==")

        # Thirty drives need fifty rows as a table, and a shelf is exactly
        # what this screen is for -- so when the table will not fit, every
        # drive still has to appear.  Records written by hand: no hardware
        # can be asked to format thirty drives for a test.
        run = os.path.join(STATE, "runs", "20200202-030405")
        os.makedirs(run, exist_ok=True)
        with open(os.path.join(run, "run"), "w") as f:
            f.write("run 1\nid 20200202-030405\nkind format\n"
                    "what format 4096B PI type 2\n"
                    "detail low level format, 4096 byte sectors\n"
                    "outdir %s\ncreated %d\nended 0\nsup %d\nsupstart %d\n"
                    "njobs 30\n"
                    % (tmp, int(time.time()) - 600, os.getpid(),
                       int(open("/proc/self/stat").read().rsplit(")", 1)[1]
                           .split()[19])))
        for k in range(1, 31):
            with open(os.path.join(run, "sdz%02d.job" % k), "w") as f:
                f.write("job 1\ndevice sdz%02d\npath /dev/sdz%02d\n"
                        "model ST14000NM0168\nsize 14000519643136\n"
                        "state 1\nverdict -1\npid %d\npidstart %d\n"
                        "started %d\nupdated %d\npct %d.0\n"
                        "note Format in progress, %d.00%% done\n"
                        % (k, k, os.getpid(),
                           int(open("/proc/self/stat").read().rsplit(")", 1)[1]
                               .split()[19]),
                           int(time.time()) - 600, int(time.time()),
                           k * 3, k * 3))

        s = Session(["--attach", "20200202-030405", "--no-color"],
                    rows=24, cols=100)
        s.wait(lambda: s.on_screen("drives"), 5.0)
        shown = [l for l in s.screen.lines() if "sdz" in l]
        seen = sum(l.count("sdz") for l in shown)
        check("every drive of a thirty-drive run is on a 24-row screen",
              seen == 30, "%d of 30 shown:\n%s" % (seen, "\n".join(shown)))
        check("the compact view says where the full list is",
              s.on_screen("--status 20200202-030405"),
              "\n".join(l for l in s.screen.lines() if "drives" in l))
        # the detailed table is what a handful of drives still gets
        check("a run small enough keeps the per-drive table",
              "LAST MESSAGE" not in "\n".join(s.screen.lines()),
              "the thirty-drive run should not be drawing the wide table")
        s.send(b"q", 0.4)
        s.close()

        print("== a hundred drives, eighty columns ==")

        # A shelf is the case this screen exists for, and eighty columns is
        # the terminal everyone actually has.  Neither can be met by one row
        # per drive, so a drive is two rows and a rule, and the table pages.
        # Records written by hand: a hundred drives cannot be scanned for a
        # test.
        run = os.path.join(STATE, "runs", "20200404-050607")
        os.makedirs(run, exist_ok=True)
        pst = int(open("/proc/self/stat").read().rsplit(")", 1)[1].split()[19])
        with open(os.path.join(run, "run"), "w") as f:
            f.write("run 1\nid 20200404-050607\nkind scan\n"
                    "what destructive write + verify\n"
                    "detail destructive write + verify\n"
                    "outdir %s\ncreated %d\nended 0\nsup %d\nsupstart %d\n"
                    "njobs 100\n"
                    % (tmp, int(time.time()) - 600, os.getpid(), pst))
        for k in range(100):
            with open(os.path.join(run, "sdk%03d.job" % k), "w") as f:
                f.write("job 1\ndevice sdk%03d\npath /dev/sdk%03d\n"
                        "model ST14000NM0168\nsize 14000519643136\n"
                        "state 1\nverdict -1\npid %d\npidstart %d\n"
                        "started %d\nupdated %d\npct %.1f\nrate 6320000\n"
                        "eta 1915980\nbad 0\nweak %d\nbytes 22520000000000\n"
                        "lat 47.70 76.30 36.30 264.70\nwlat 41.30\n"
                        % (k, k, os.getpid(), pst, int(time.time()) - 600,
                           int(time.time()), 13.5, 119 + k))

        s = Session(["--attach", "20200404-050607", "--no-color"],
                    rows=24, cols=80)
        s.wait(lambda: s.on_screen("to a line") or s.on_screen("DRIVE"), 5.0)
        # a hundred drives is too many for the table, so it opens on the
        # overview, which pages too
        check("a shelf opens on the overview",
              s.on_screen("to a line"), "\n".join(s.screen.lines()[:12]))
        check("the overview pages rather than dropping drives",
              s.on_screen("page 1/"),
              "\n".join(l for l in s.screen.lines() if "page" in l))
        s.send(b"g", 0.6)
        lines = s.screen.lines()
        hdr = next((i for i, l in enumerate(lines)
                    if "DRIVE" in l and "STATE" in l), -1)
        check("g swaps the overview for the drive table", hdr >= 0,
              "\n".join(lines[:12]))
        if hdr >= 0:
            first, second = lines[hdr + 1], lines[hdr + 2]
            # if either row were wider than the terminal it would wrap and
            # the pair would no longer line up like this
            check("a drive is a row of its own, then a row of latency",
                  first.lstrip().startswith("sdk") and
                  " med " in second and " wmed " in second,
                  "%r\n%r" % (first, second))
            check("a rule separates one drive from the next",
                  set(lines[hdr + 3].strip()) == {"-"} and
                  lines[hdr + 4].lstrip().startswith("sdk"),
                  "\n".join(lines[hdr + 1:hdr + 6]))
            check("no row is wider than the terminal",
                  all(len(l) <= 80 for l in lines) and
                  not any(l.startswith("med") or l.startswith("h 1")
                          for l in lines),
                  "\n".join(lines))
        page1 = [l for l in s.screen.lines() if l.lstrip().startswith("sdk")]
        s.send(b">", 0.6)
        page2 = [l for l in s.screen.lines() if l.lstrip().startswith("sdk")]
        check("the table pages forward", bool(page1) and page1 != page2,
              "page 1: %r\npage 2: %r" % (page1[:2], page2[:2]))
        check("the footer says which page this is",
              s.on_screen("page 2/"),
              "\n".join(l for l in s.screen.lines() if "page" in l))
        s.send(b"<", 0.6)
        check("the table pages back",
              [l for l in s.screen.lines() if l.lstrip().startswith("sdk")]
              == page1, "did not return to the first page")
        s.send(b"q", 0.4)
        s.close()

        # With colour the shaded background separates one drive from the
        # next, so the rule is not drawn and does not cost a line: the same
        # screen holds more drives.  Without colour there is nothing to see,
        # which is why the rule is still there in the session above.
        s = Session(["--attach", "20200404-050607"], rows=24, cols=80)
        s.wait(lambda: s.on_screen("DRIVE") or s.on_screen("to a line"), 5.0)
        s.send(b"g", 0.6)
        lines = s.screen.lines()
        rows = [l for l in lines if l.lstrip().startswith("sdk")]
        check("colour separates drives without spending a line on a rule",
              not any(l.strip() and set(l.strip()) == {"-"} for l in lines)
              and len(rows) > len(page1),
              "%d striped rows against %d ruled ones" % (len(rows), len(page1)))
        raw = bytes(s.raw)
        check("the terminal is asked which way its background goes",
              b"\x1b]11;?" in raw, "no OSC 11 query in the stream")
        check("every other drive is drawn on a background of its own",
              raw.count(b"\x1b[48;5;") >= 2,
              "%d background runs in the frame" % raw.count(b"\x1b[48;5;"))
        s.send(b"q", 0.4)
        s.close()

        # REGRESSION: the grid sized a cell as 27 columns when it is 29, so
        # a 112-column terminal got four to a line and the last bar was cut
        # off at the edge.  A complete cell has its percentage and the
        # closing bracket of its bar on the same line.
        s = Session(["--attach", "20200202-030405", "--no-color"],
                    rows=24, cols=112)
        s.wait(lambda: s.on_screen("to a line"), 5.0)
        grid = [l for l in s.screen.lines() if "sdz" in l]
        cut = [l for l in grid if l.count("%") != l.count("]")]
        check("no grid cell is cut off at the edge of a 112-column terminal",
              grid and not cut, "\n".join(cut or grid or s.screen.lines()))
        s.send(b"q", 0.4)
        s.close()

        print("== a drive drilling past the window keeps its latency row ==")

        # REGRESSION: a drive drilling into one chunk for longer than the
        # thirty-second window finished no chunk inside it, so its record
        # carried no latency and the dashboard drew the row a drive that is
        # not running gets -- model and size -- for the one drive on the
        # screen in the worst trouble.  Records written by hand, since only
        # a failing disk drills for that long.
        run = os.path.join(STATE, "runs", "20200606-070809")
        os.makedirs(run, exist_ok=True)
        pst = int(open("/proc/self/stat").read().rsplit(")", 1)[1].split()[19])
        now = int(time.time())
        with open(os.path.join(run, "run"), "w") as f:
            f.write("run 1\nid 20200606-070809\nkind scan\n"
                    "what destructive write + verify\n"
                    "detail destructive write + verify\n"
                    "outdir %s\ncreated %d\nended 0\nsup %d\nsupstart %d\n"
                    "njobs 4\n" % (tmp, now - 164, os.getpid(), pst))
        for dev, bad_n, weak_n, lat in (
                ("sdc", 0, 98, "lat 0.00 0.00 0.00 0.00\n"
                               "blat 12.40 850.20 4.10 4800.20\n"),
                ("sdd", 3, 7, "lat 0.00 0.00 0.00 0.00\nlatidle 1\n"),
                ("sdf", 0, 0, "lat 30.20 28.00 18.40 72.00\nwlat 2.50\n"),
                ("sdh", 0, 0, "lat 10.10 10.20 8.80 36.90\nwlat 7.20\n"),
                # what a worker leaves behind: the latency it had while it
                # ran, and the supervisor's verdict patched in on top
                ("sdx", 0, 0, "lat 10.10 10.20 8.80 36.90\nwlat 7.20\n"
                              "state 2\nverdict 0\n")):
            with open(os.path.join(run, dev + ".job"), "w") as f:
                # sdc's ETA is 42781h, ten characters: the width that once
                # pushed its state word a column right of the header
                f.write("job 1\ndevice %s\npath /dev/%s\n"
                        "model ST14000NM0168\nsize 14000519643136\n"
                        "state 1\nverdict -1\npid %d\npidstart %d\n"
                        "started %d\nupdated %d\npct 0.0\nrate 90000\n"
                        "eta %d\nbad %d\nweak %d\nbytes 2000000000\n%s"
                        % (dev, dev, os.getpid(), pst, now - 164, now,
                           154012000 if dev == "sdc" else 1000000,
                           bad_n, weak_n, lat))

        s = Session(["--attach", "20200606-070809", "--no-color"],
                    rows=30, cols=80)
        s.wait(lambda: s.on_screen("DRIVE"), 5.0)
        lines = s.screen.lines()

        def under(dev):
            i = next((k for k, l in enumerate(lines)
                      if l.lstrip().startswith(dev + " ")), -1)
            return lines[i + 1] if 0 <= i < len(lines) - 1 else ""

        check("a drive drilling past the window shows its sector reads",
              re.search(r"sector med\s+12\.4 .*max 4800\.2", under("sdc"))
              and "ST14000" not in under("sdc"),
              "%r\n%s" % (under("sdc"), "\n".join(lines)))
        check("a drive that finished no read in the window says so",
              "no read finished in the last 30 seconds" in under("sdd"),
              "%r" % under("sdd"))
        check("a drive with chunks in the window is not labelled sector",
              re.search(r"^\s+med\s+30\.2 ", under("sdf")) is not None,
              "%r" % under("sdf"))
        w80 = drawn_widths(s.raw)
        check("the latency rows fit an 80-column terminal",
              w80 and max(w80) <= 80, "widest row drawn: %d" % max(w80 or [0]))
        check("a finished drive shows no stale latency numbers",
              "ST14000" in under("sdx") and " med " not in under("sdx"),
              "%r" % under("sdx"))

        def state_cols(ls):
            i = next((k for k, l in enumerate(ls)
                      if "DRIVE" in l and "STATE" in l), -1)
            if i < 0:
                return -1, []
            words = [m.start() for l in ls[i + 1:]
                     for m in re.finditer(r"\b(scanning|HEALTHY)\b", l)]
            return ls[i].find("STATE"), words

        # REGRESSION: an ETA wider than its column pushed that drive's
        # state a column right of the header, and every other one with it
        h, w = state_cols(lines)
        check("every state word starts under STATE",
              h > 0 and len(w) == 5 and set(w) == {h},
              "STATE at %d, state words at %r\n%s" % (h, w, "\n".join(lines)))
        s.send(b"q", 0.4)
        s.close()

        # A terminal with room for both halves side by side gets them on one
        # row -- measured, never cut: one column short of that, it is two.
        def wide(cols):
            s = Session(["--attach", "20200606-070809", "--no-color"],
                        rows=24, cols=cols)
            s.wait(lambda: s.on_screen("DRIVE"), 5.0)
            s.pump(0.3)
            ls, raw = s.screen.lines(), bytes(s.raw)
            s.send(b"q", 0.4)
            s.close()
            return ls, raw

        lines, raw = wide(134)
        h, w = state_cols(lines)
        check("a wide terminal puts each drive on one row",
              any(l.lstrip().startswith("sdc ") and "4800.2" in l
                  for l in lines) and
              any(l.lstrip().startswith("sdx ") and "ST14000" in l
                  for l in lines), "\n".join(lines))
        check("one row per drive still lines its state up under STATE",
              h > 0 and len(w) == 5 and set(w) == {h},
              "STATE at %d, state words at %r" % (h, w))
        check("one row per drive never runs past the terminal",
              drawn_widths(raw) and max(drawn_widths(raw)) <= 134,
              "widest row drawn: %d" % max(drawn_widths(raw) or [0]))
        # and a terminal wider still is not left half empty: the model and
        # size go beside the name, then a bar of each drive's progress
        lines, raw = wide(214)
        h, w = state_cols(lines)
        row = lambda d: next((l for l in lines
                              if l.lstrip().startswith(d + " ")), "")
        check("a wide terminal gives every drive its model and a bar",
              "ST14000" in row("sdc") and "[---" in row("sdc") and
              "4800.2" in row("sdc") and "[====" in row("sdx"),
              "\n".join(lines))
        check("the widest layout still lines its state up under STATE",
              h > 0 and len(w) == 5 and set(w) == {h},
              "STATE at %d, state words at %r" % (h, w))
        check("the widest layout fills the terminal and no more",
              max(drawn_widths(raw) or [0]) == 214,
              "widest row drawn: %d" % max(drawn_widths(raw) or [0]))
        lines, raw = wide(133)
        check("a terminal one column short keeps two rows",
              not any(l.lstrip().startswith("sdc ") and "med" in l
                      for l in lines) and
              any(re.search(r"sector med\s+12\.4", l) for l in lines),
              "\n".join(lines))

        # The same screen in colour: a figure over 50 ms is yellow, over
        # 100 ms red, and a bad or weak count is red or yellow once it is
        # not zero.  Colour is separate bytes, so the padding before a
        # number sits inside the colour and the width is unchanged.
        s = Session(["--attach", "20200606-070809"], rows=24, cols=80)
        s.wait(lambda: s.on_screen("DRIVE"), 5.0)
        s.pump(0.4)
        raw = bytes(s.raw)
        red, yel = b"\x1b[31m", b"\x1b[33m"
        check("a latency over 100 ms is red",
              red + b"4800.2" in raw and red + b" 850.2" in raw,
              "no red 4800.2 / 850.2 in the frame")
        check("a latency over 50 ms is yellow",
              yel + b"  72.0" in raw, "no yellow 72.0 in the frame")
        check("a latency under 50 ms is not coloured",
              b"med   10.1" in raw and b"med   30.2" in raw,
              "10.1 or 30.2 carries a colour")
        check("a weak count is yellow, a bad count red",
              yel + b"      98" in raw and red + b"      3" in raw,
              "weak 98 / bad 3 not coloured")
        check("a zero count is not coloured",
              red + b"      0" not in raw and yel + b"       0" not in raw,
              "a zero count carries a colour")
        check("the summary colours the totals",
              b"bad sectors " + red + b"3" in raw and
              b"weak " + yel + b"105" in raw,
              "summary totals not coloured")
        lines = s.screen.lines()
        check("colour costs no columns: the stripes still line up",
              max(drawn_widths(s.raw) or [0]) <= 80 and
              any(re.search(r"sdc\s+0\.0%.*\s98\s", l) for l in lines),
              "widest row drawn: %d\n%s"
              % (max(drawn_widths(s.raw) or [0]), "\n".join(lines)))
        s.send(b"q", 0.4)
        s.close()

        print("== the form's banner survives a short terminal ==")

        # REGRESSION: with a run in progress and a tool missing, the form
        # printed two banner lines and budgeted for one, and the terminal
        # scrolled "configure a run" off the top.  Only at heights where
        # both settings groups are in view does the budget have no slack to
        # hide that in -- 29 to 37 rows when this was found -- so every
        # height from 24 up is tried rather than one that happens to pass.
        # The machine's own drives are left out so those heights do not
        # move with it, and every tool is made to look absent: that is a
        # rescue system, and its list of missing tools is too long for
        # eighty columns, a wrapped row the budget cannot see.  The runs
        # written by hand above are still live -- their supervisor is this
        # process -- so the program opens on them and 'n' goes to the form.
        os.environ["HDDSCAN_TOOLS"] = "none"
        os.environ["HDDSCAN_NO_ENUMERATE"] = "1"
        lost, wrapped = [], []
        try:
            for h in range(24, 41):
                s = Session(["-i", "--no-color", "--outdir", tmp, a],
                            rows=h, cols=80)
                s.wait(lambda: s.on_screen("runs on this machine"), 5.0)
                s.send(b"n", 0.3)
                s.wait(lambda: s.on_screen("q quit"), 5.0)
                s.pump(0.4)
                lines = s.screen.lines()
                if not (lines[0].strip().endswith("configure a run") and
                        "already in progress" in lines[1]):
                    lost.append(h)
                miss = next((k for k, l in enumerate(lines)
                             if "missing:" in l), -1)
                if not (0 < miss and len(lines[miss]) < 80 and
                        "hdparm" in lines[miss] and
                        not lines[miss + 1].strip()):
                    wrapped.append(h)
                s.send(b"q", 0.2)
                s.close()
        finally:
            del os.environ["HDDSCAN_TOOLS"]
            del os.environ["HDDSCAN_NO_ENUMERATE"]
        check("the banner stays on the form with a run going and no tools",
              not lost, "banner scrolled away at %r rows" % lost)
        check("a long missing-tools line is cut to the width, not wrapped",
              not wrapped, "missing-tools line wrong at %r rows" % wrapped)

        print("== --no-tui is honoured on a terminal ==")

        s = Session(["--no-tui"])
        s.pump(0.8)
        check("--no-tui does not open the picker even on a terminal",
              not any("configure a run" in l for l in s.screen.lines()))
        s.close()

    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(STATE, ignore_errors=True)

    print("\n  %d passed, %d failed\n" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
