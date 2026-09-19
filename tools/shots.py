#!/usr/bin/env python3
"""Regenerate the site's screenshots from hddscan's own output.

Every picture on the site is hddscan drawing into a pseudo-terminal, replayed
through a small terminal model and written out as SVG -- nothing is mocked up
by hand.  Two kinds of input feed it:

  * sparse image files, for the form and for scans that really run (a drive
    with injected corruption gives a real summary, a real report, a real map);
  * a demo run store, for the shelf-sized pictures: a 24-drive write test and a
    30-drive low level format.  The records are hand-written the way the suite
    writes them, and their pid is a process this script starts, so hddscan
    sees two live runs and draws them as it would draw real ones.

HDDSCAN_NO_ENUMERATE keeps the machine's own drives out of the picker, so the
pictures are the same wherever they are regenerated.  Needs no root.

    tools/shots.py            # writes docs/img/*.svg
"""
import html
import random
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import fcntl
import termios

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDDSCAN = os.path.join(ROOT, "hddscan")
OUT = os.path.join(ROOT, "docs", "img")

# the look of the existing screenshots: a dark window with a title bar
BG, FG, TITLE = "#1b1f27", "#d0d4de", "#9aa3b2"
COLORS = {31: "#ff7b72", 32: "#8fd66b", 33: "#e5c07b"}
DIM = "#7d8595"
BG256 = {236: "#303640", 254: "#e4e4e4"}
CW, LH, FS = 8.43, 18, 14
MONO = "DejaVu Sans Mono,Menlo,Consolas,Liberation Mono,monospace"
SANS = "-apple-system,Segoe UI,Helvetica,Arial,sans-serif"

CSI = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")
OSC = re.compile(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")


class Term:
    """Enough of a terminal for what hddscan sends: CUP, CUU, EL, ED, SGR."""

    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.blank = (" ", None, None, False, False, False)
        self.grid = [[self.blank] * cols for _ in range(rows)]
        self.r = self.c = 0
        self.fg = self.bg = None
        self.bold = self.dim = self.rev = False

    def feed(self, text):
        i = 0
        while i < len(text):
            m = OSC.match(text, i)
            if m:
                i = m.end()
                continue
            m = CSI.match(text, i)
            if m:
                self.csi(m.group(1), m.group(2))
                i = m.end()
                continue
            ch = text[i]
            i += 1
            if ch == "\n":
                self.r += 1
                self.c = 0
            elif ch == "\r":
                self.c = 0
            elif ch == "\x1b" or ord(ch) < 32:
                continue
            else:
                if self.c >= self.cols:
                    self.r += 1
                    self.c = 0
                self.scroll()
                self.grid[self.r][self.c] = (ch, self.fg, self.bg, self.bold,
                                             self.dim, self.rev)
                self.c += 1
            self.scroll()

    def scroll(self):
        while self.r >= self.rows:
            self.grid.pop(0)
            self.grid.append([self.blank] * self.cols)
            self.r -= 1

    def csi(self, args, cmd):
        priv = args.startswith("?")
        nums = [int(x) if x else 0 for x in args.lstrip("?").split(";")] \
            if args.lstrip("?") else []
        if cmd == "H":
            self.r = max(0, (nums[0] if nums else 1) - 1)
            self.c = max(0, (nums[1] if len(nums) > 1 else 1) - 1)
            self.r = min(self.r, self.rows - 1)
        elif cmd == "A":
            self.r = max(0, self.r - (nums[0] if nums and nums[0] else 1))
        elif cmd == "K":
            for x in range(self.c, self.cols):
                self.grid[self.r][x] = self.blank
        elif cmd == "J":
            if nums and nums[0] == 2:
                self.grid = [[self.blank] * self.cols for _ in range(self.rows)]
        elif cmd in "hl" and priv and 1049 in nums and cmd == "h":
            self.grid = [[self.blank] * self.cols for _ in range(self.rows)]
            self.r = self.c = 0
        elif cmd == "m":
            self.sgr(nums or [0])

    def sgr(self, nums):
        k = 0
        while k < len(nums):
            n = nums[k]
            if n == 0:
                self.fg = self.bg = None
                self.bold = self.dim = self.rev = False
            elif n == 1:
                self.bold = True
            elif n == 2:
                self.dim = True
            elif n == 22:
                self.bold = self.dim = False
            elif n == 7:
                self.rev = True
            elif n == 27:
                self.rev = False
            elif n in COLORS:
                self.fg = n
            elif n == 39:
                self.fg = None
            elif n == 49:
                self.bg = None
            elif n == 48 and k + 2 < len(nums) and nums[k + 1] == 5:
                self.bg = nums[k + 2]
                k += 2
            k += 1

    def lines(self):
        return ["".join(c[0] for c in row).rstrip() for row in self.grid]

    def text(self):
        return "\n".join(self.lines())


def svg(rows, title, path):
    """rows: lists of cells, as Term.grid holds them"""
    cols = max((len(r) for r in rows), default=80)
    # trailing blank cells carry nothing, so the window is as wide as its text
    used = 0
    for row in rows:
        for x in range(len(row) - 1, -1, -1):
            ch, fg, bg, bold, dim, rev = row[x]
            if ch != " " or bg is not None or rev:
                used = max(used, x + 1)
                break
    cols = max(used, 60)
    w = round(36 + cols * CW)
    h = 61 + (len(rows) - 1) * LH + 16
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
           'viewBox="0 0 %d %d" role="img" aria-label="%s">'
           % (w, h, w, h, html.escape(title)),
           '<rect width="%d" height="%d" rx="9" fill="%s"/>' % (w, h, BG),
           '<rect width="%d" height="30" rx="9" fill="#2a2f3a"/>' % w,
           '<rect y="21" width="%d" height="9" fill="#2a2f3a"/>' % w,
           '<circle cx="20" cy="15" r="6" fill="#ff5f57"/>',
           '<circle cx="40" cy="15" r="6" fill="#febc2e"/>',
           '<circle cx="60" cy="15" r="6" fill="#28c840"/>',
           '<text x="%d" y="20" fill="%s" font-family="%s" font-size="13" '
           'text-anchor="middle">%s</text>'
           % (w // 2, TITLE, SANS, html.escape(title))]
    for i, row in enumerate(rows):
        y = 61 + i * LH
        spans, rects = [], []
        x = 0
        row = row[:cols]
        while x < len(row):
            st = row[x][1:]
            e = x
            while e < len(row) and row[e][1:] == st:
                e += 1
            fg, bg, bold, dim, rev = st
            color = DIM if dim else COLORS.get(fg, FG)
            back = BG256.get(bg) if bg is not None else None
            if rev:
                back, color = color, BG
            if back:
                rects.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%d" '
                             'fill="%s"/>' % (18 + x * CW, y - 13.5,
                                              (e - x) * CW, LH, back))
            txt = "".join(c[0] for c in row[x:e])
            if txt.strip() or back:
                spans.append('<tspan x="%.1f" fill="%s"%s>%s</tspan>'
                             % (18 + x * CW, color,
                                ' font-weight="bold"' if bold else "",
                                html.escape(txt)))
            x = e
        out.extend(rects)
        if spans:
            out.append('<text y="%d" font-family="%s" font-size="%d" '
                       'xml:space="preserve">%s</text>'
                       % (y, MONO, FS, "".join(spans)))
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("  wrote", os.path.relpath(path, ROOT))


class Pty:
    def __init__(self, args, rows, cols, env, cwd):
        self.rows, self.cols = rows, cols
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(cwd)
            os.execve(HDDSCAN, [HDDSCAN] + args, env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        self.raw = bytearray()
        self.pump(0.5)

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                d = os.read(self.fd, 65536)
            except OSError:
                return
            if not d:
                return
            self.raw.extend(d)

    def screen(self):
        t = Term(self.rows, self.cols)
        t.feed(self.raw.decode("utf-8", "replace"))
        return t

    def send(self, keys, settle=0.4):
        os.write(self.fd, keys)
        self.pump(settle)

    def wait(self, text, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.2)
            if text in self.screen().text():
                self.pump(0.5)     # let the frame finish painting
                return True
        raise SystemExit("timed out waiting for %r on screen:\n%s"
                         % (text, self.screen().text()))

    def close(self):
        try:
            os.kill(self.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


def transcript(cmds, env, cwd, cols=120, keep=None, rows=400, subst=()):
    """A terminal session of shell commands and their output, as cells.
    keep, if given, filters each command's output lines to a slice; rows is
    the height each command believes its terminal has, which is what decides
    how much of a long list hddscan prints; subst rewrites the temporary
    paths this script works in to the ones a real machine would show."""
    t = Term(400, cols)
    for i, (shown, args) in enumerate(cmds):
        t.feed("\x1b[32m$ %s\x1b[0m\r\n" % shown)
        p = Pty(args, rows, cols, env, cwd)
        p.pump(0.2)
        while True:
            try:
                pid, _ = os.waitpid(p.pid, os.WNOHANG)
            except ChildProcessError:
                break
            p.pump(0.2)
            if pid:
                p.pump(0.3)
                break
        text = p.raw.decode("utf-8", "replace")
        for a, b in subst:
            text = text.replace(a, b)
        if keep and keep[i]:
            lines = text.replace("\r\n", "\n").split("\n")
            text = "\r\n".join(keep[i](lines)) + "\r\n"
        t.feed(text)
        p.close()
    rows = [r for r in t.grid]
    while rows and all(c[0] == " " for c in rows[-1]):
        rows.pop()
    return rows


# ---------------------------------------------------------------- demo store

def pid_start(pid):
    with open("/proc/%d/stat" % pid) as f:
        s = f.read()
    return int(s[s.rindex(")") + 2:].split()[19])


def demo_store(state, live_pid):
    now = int(time.time())
    start = pid_start(live_pid)
    TiB = 1 << 40

    def run(rid, kind, what, detail, profile, created, jobs):
        d = os.path.join(state, "runs", rid)
        os.makedirs(d)
        with open(os.path.join(d, "run"), "w") as f:
            f.write("run 1\nid %s\nkind %s\nwhat %s\ndetail %s\nprofile %s\n"
                    "outdir /var/log/hddscan\ncreated %d\nended 0\nsup %d\n"
                    "supstart %d\nnjobs %d\n"
                    % (rid, kind, what, detail, profile, created, live_pid,
                       start, len(jobs)))
        for j in jobs:
            with open(os.path.join(d, j["dev"] + ".job"), "w") as f:
                f.write("job 1\ndevice %s\npath /dev/%s\nmodel %s\n"
                        "serial %s\nsize %d\nstate %d\nverdict %d\n"
                        "pid %d\npidstart %d\nstarted %d\nupdated %d\n"
                        "ended %d\npct %.3f\nrate %.0f\neta %.1f\nbad %d\n"
                        "weak %d\nbytes %d\nlat %.2f %.2f %.2f %.2f\n"
                        % (j["dev"], j["dev"], j["model"], j["serial"],
                           j["size"], j["state"], j["verdict"],
                           live_pid if j["state"] == 1 else 0,
                           start if j["state"] == 1 else 0,
                           created, now, j.get("ended", 0), j["pct"],
                           j.get("rate", 0), j.get("eta", 0), j.get("bad", 0),
                           j.get("weak", 0), j.get("bytes", 0),
                           *j.get("lat", (0, 0, 0, 0))))
                if j.get("wlat"):
                    f.write("wlat %.2f\n" % j["wlat"])
                if j.get("note"):
                    f.write("note %s\n" % j["note"])
                if j["state"] == 2:
                    f.write("report /var/log/hddscan/hddscan-%s.txt\n" % j["dev"])

    # a 24-drive predeploy write test, 83 hours in
    names = ["sd" + a for a in "bcdefghijklmnopqrstuvwxy"]
    scan = []
    for i, n in enumerate(names):
        big = i % 3 == 0
        size = int(12.73 * TiB) if big else int(9.10 * TiB)
        model = "ST14000NM0168" if big else "HUH721010AL4200"
        if i < 5:
            pct = 71.0 + 4.1 * i
            scan.append(dict(dev=n, model=model, serial="ZL2%05d" % i,
                             size=size, state=1, verdict=-1, pct=pct,
                             rate=(169.75 + 5.7 * i) * (1 << 20),
                             eta=(5 * 3600 + 50 * 60) + i * 1600,
                             weak=40 + 23 * i, bytes=int(size * pct / 50),
                             lat=(11.0 + 0.4 * i, 12.8 + 0.6 * i, 7.5,
                                  26.7 + 9 * i), wlat=21.4))
        else:
            weak = 612 if n == "sdg" else (3 if i % 7 == 0 else 0)
            bad = 3 if n == "sdg" else 0
            scan.append(dict(dev=n, model=model, serial="ZL2%05d" % i,
                             size=size, state=2, verdict=1 if bad or weak else 0,
                             pct=100, ended=now - 3600 * (i % 5), bad=bad,
                             weak=weak, bytes=size * 2,
                             lat=(10.9, 12.1, 7.4, 31.0)))
    run("20260912-080512", "scan", "write (predeploy)",
        "destructive write + verify", "predeploy", now - 83 * 3600, scan)

    # a 30-drive low level format beside it
    fmt = []
    spread = random.Random(7)
    for i in range(30):
        n = "sda" + chr(ord("a") + i) if i < 26 else "sdb" + chr(ord("a") + i - 26)
        big = i % 5 == 4
        pct = round(spread.uniform(44.0, 79.0), 2)
        fmt.append(dict(dev=n, model="ST14000NM0168" if big else "HUH721010AL4200",
                        serial="8DG%05d" % i,
                        size=int((12.73 if big else 9.10) * TiB),
                        state=1 if i < 28 else 2, verdict=-1 if i < 28 else 0,
                        pct=pct if i < 28 else 100,
                        note="Format in progress, %.2f%% done" % pct
                        if i < 28 else "format complete"))
    run("20260912-093044", "format", "format 4096B PI kept",
        "low level format, 4096 byte sectors, protection information kept",
        "predeploy", now - (4 * 3600 + 8 * 60), fmt)


    # the same shelf's previous batch, finished: what the summary shows
    done = []
    verdicts = {"sdg": (2, 214, 37), "sdm": (1, 0, 1042), "sdr": (1, 0, 12)}
    for i, n in enumerate(names):
        big = i % 3 == 0
        size = int(12.73 * TiB) if big else int(9.10 * TiB)
        v, bad, weak = verdicts.get(n, (0, 0, 0))
        done.append(dict(dev=n, model="ST14000NM0168" if big
                         else "HUH721010AL4200", serial="ZL3%05d" % i,
                         size=size, state=2, verdict=v, pct=100,
                         ended=now - 2 * 86400, bad=bad, weak=weak,
                         bytes=size * 2))
    rid = "20260915-201544"
    run(rid, "scan", "write (predeploy)", "destructive write + verify",
        "predeploy", now - 2 * 86400 - 81 * 3600, done)
    d = os.path.join(state, "runs", rid)
    with open(os.path.join(d, "run")) as f:
        rec = f.read()
    with open(os.path.join(d, "run"), "w") as f:
        f.write(rec.replace("ended 0\n", "ended %d\n" % (now - 2 * 86400))
                .replace("sup %d\n" % live_pid, "sup 0\n"))
    # a whole-drive checkpoint beside each drive that found something,
    # which is what the summary asks before it offers 'h'
    for j in done:
        if j["bad"] or j["weak"]:
            with open(os.path.join(d, j["dev"] + ".ckpt"), "w") as f:
                f.write("hddscan-state 2\ndevice %s\nsize %d\nstart 0\n"
                        "end %d\nbytes %d\n" % (j["dev"], j["size"],
                                                 j["size"], j["size"]))


def patterned(path, mb, env, cwd):
    with open(path, "wb") as f:
        f.write(os.urandom(mb << 20))
    subprocess.run([HDDSCAN, "--no-color", "--mode", "write", "--confirm",
                    path, path], env=env, cwd=cwd, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def main():
    if not os.access(HDDSCAN, os.X_OK):
        raise SystemExit("build hddscan first (make)")
    os.makedirs(OUT, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="hddscan-shots-")
    state = os.path.join(tmp, "state")
    work = os.path.join(tmp, "work")
    os.makedirs(state)
    os.makedirs(work)
    env = dict(os.environ, HDDSCAN_STATE_DIR=state, HDDSCAN_NO_ENUMERATE="1",
               HDDSCAN_THEME="dark", TERM="xterm-256color", LANG="C.UTF-8")
    env.pop("NO_COLOR", None)
    sleeper = subprocess.Popen(["sleep", "3600"])
    try:
        shoot(tmp, state, work, env, sleeper.pid)
    finally:
        sleeper.kill()
        shutil.rmtree(tmp, ignore_errors=True)


def shoot(tmp, state, work, env, live):
    img = lambda n: os.path.join(OUT, n)
    demo = state + "-demo"
    # what this script's temporary paths would be on a real machine
    real = [(demo, "/var/lib/hddscan"), (state, "/var/lib/hddscan")]

    # ---- the form, over a shelf of sparse images
    for n in ("sdb.img", "sdc.img", "sdd.img", "sde.img"):
        with open(os.path.join(work, n), "wb") as f:
            f.truncate(int(12.73 * (1 << 40)) if n != "sde.img"
                       else int(9.1 * (1 << 40)))
    p = Pty(["-i", "sdb.img", "sdc.img", "sdd.img", "sde.img"], 44, 100,
            env, work)
    p.wait("Protection info")
    svg(p.screen().grid, "sudo hddscan", img("tui-form.svg"))
    p.close()

    # ---- the shelf: a demo store with two live runs and a finished one
    demo_store(demo, live)
    denv = dict(env, HDDSCAN_STATE_DIR=demo)
    # more drives than the table holds, so it opens on the overview
    p = Pty(["--attach", "20260912-080512"], 30, 100, denv, work)
    p.wait("aggregate")
    svg(p.screen().grid, "sudo hddscan --attach 20260912-080512",
        img("tui-overview.svg"))
    p.send(b"g", 0.8)
    p.wait("DRIVE")
    svg(p.screen().grid, "sudo hddscan --attach 20260912-080512  (g)",
        img("tui-dashboard.svg"))
    p.close()

    p = Pty(["--attach"], 14, 100, denv, work)
    p.wait("enter open")
    svg(p.screen().grid, "sudo hddscan --attach", img("tui-runs.svg"))
    p.close()

    p = Pty(["--attach", "20260915-201544"], 32, 100, denv, work)
    p.wait("h hide bad blocks")
    svg(p.screen().grid, "sudo hddscan --attach 20260915-201544",
        img("tui-summary.svg"))
    p.send(b"h", 0.5)
    p.wait("press y to hide")
    svg(p.screen().grid, "sudo hddscan --attach 20260915-201544  (h)",
        img("tui-hide-confirm.svg"))
    p.send(b"n", 0.3)
    p.close()

    rows = transcript([("sudo hddscan --status", ["--status"]),
                       ("sudo hddscan --status 20260912-093044 | head -16",
                        ["--status", "20260912-093044"])],
                      denv, work, cols=140, rows=24, subst=real,
                      keep=[None, lambda ls: ls[:16]])
    svg(rows, "sudo hddscan --status", img("cli-status.svg"))

    # ---- a real scan that finds damage, and its report
    patterned(os.path.join(work, "sdh.img"), 64, env, work)
    with open(os.path.join(work, "sdh.img"), "r+b") as f:
        f.seek(24 << 20)
        f.write(b"BADBADBAD")

    def report_tail(lines):
        k = next((i for i, l in enumerate(lines) if "--- Counters ---" in l), 0)
        e = next((i for i, l in enumerate(lines) if "Or, for any filesystem" in l),
                 len(lines) - 4)
        keep = [l for l in lines[k:e + 4] if "ms at byte" not in l]
        return keep
    rows = transcript([("sudo hddscan --profile inservice --mode check sdh.img",
                        ["--profile", "inservice", "--mode", "check",
                         "sdh.img"])],
                      env, work, cols=110, keep=[report_tail], subst=real)
    svg(rows, "sudo hddscan --profile inservice --mode check sdh.img",
        img("cli-report.svg"))

    # ---- the map, on an image the size of a real drive
    with open(os.path.join(work, "sdg.img"), "wb") as f:
        f.truncate(int(12.73 * (1 << 40)))
    with open(os.path.join(work, "sdg.bb"), "w") as f:
        # three patches of damage, in 4 KiB blocks
        for b in list(range(781250, 781262)) + [1953125] + \
                list(range(2441406, 2441700)):
            f.write("%d\n" % b)
    with open(os.path.join(work, "late.bb"), "w") as f:
        f.write("%d\n" % 1220703125)
    rows = transcript([
        ("sudo hddscan dm create sdg.img --bad sdg.bb --block-size 4096 "
         "--confirm sdg.img",
         ["dm", "create", "sdg.img", "--bad", "sdg.bb", "--block-size",
          "4096", "--confirm", "sdg.img"]),
        ("sudo hddscan dm remap sdg.img --bad late.bb --block-size 4096 "
         "--offline",
         ["dm", "remap", "sdg.img", "--bad", "late.bb", "--block-size",
          "4096", "--offline"]),
        ("sudo hddscan dm status sdg.img", ["dm", "status", "sdg.img"]),
        ("sudo hddscan dm table sdg.img | head -4",
         ["dm", "table", "sdg.img"]),
    ], env, work, cols=100, subst=real,
        keep=[None, None, None, lambda ls: ls[:4]])
    svg(rows, "hddscan dm", img("cli-dm.svg"))


if __name__ == "__main__":
    main()
