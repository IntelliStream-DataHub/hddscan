/* SPDX-License-Identifier: MIT */
/*
 * hddscan - per-sector latency and error surface scanner for rotating disks
 *
 * Copyright (c) 2026 Olav Gjerde.  See LICENSE for the full text.
 *
 * Strategy: read the device in large chunks (default 128 KiB) and time each
 * one.  A chunk can never be faster than its slowest sector, so a chunk that
 * comes back within budget proves every sector inside it was served in time.
 * Only when a chunk misses its budget (or errors) do we drill into it one
 * logical block at a time, and every suspicious block is then re-read many
 * times to tell a one-off hiccup apart from a sector that is genuinely dying.
 *
 * No warranty of any kind.  This tool can destroy data: --mode write and
 * --format erase a drive outright, and --mode verify rewrites what it reads.
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp; glibc leaks it via string.h, others may not */
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/fs.h>

#define PROG "hddscan"
#define VERSION "1.0"

/*
 * How many chunks may be refused back to back before a scan concludes it is
 * looking at an unwritten PI-formatted device rather than a gap.  Sixty-four
 * in a row is already conclusive, and the cost of being lax here is real: a
 * refused request is not free, and at 1 MiB apiece they cost about 20 ms
 * each, so an earlier limit of ten thousand spent two and a half minutes
 * proving something the first second had already established.
 */
/*
 * Consecutive 512-write windows that must all show the collapse before a drive
 * is called shingled.  One window is a defect; a shingled drive never recovers.
 */
#define SMR_WINDOWS 3

/* calibration anchors, and the gradient the budget follows between them */
#define NANCHOR 8

#define PROT_RUN_MAX 64

#define DEF_CHUNK (128u * 1024u)
#define DEF_BLOCK (4u * 1024u)
#define RATE_REF_CHUNK (4u * 1024u * 1024u) /* the chunk --min-rate is quoted at */
#define RATE_SETTLE_S 120   /* scan time before the throughput floor judges */
#define MAX_RETRIES 1000
#define N_BANDS 512
#define MAX_FINDINGS 200000
#define TOP_SLOW 20

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

static FILE *g_log;            /* optional tee target */
static int g_quiet;
static int g_color = -1;       /* -1 = auto */
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_stop_hard;

static void out(const char *fmt, ...)
{
	va_list ap;

	if (!g_quiet) {
		va_start(ap, fmt);
		vfprintf(stdout, fmt, ap);
		va_end(ap);
	}
	if (g_log) {
		va_start(ap, fmt);
		vfprintf(g_log, fmt, ap);
		va_end(ap);
	}
}

/*
 * While the dashboard owns the screen it draws every row by absolute
 * position, so anything written to the terminal behind its back scrolls the
 * frame out from under it and leaves rows the dashboard does not own holding
 * pieces of an older frame.  Warnings are therefore parked in a ring the
 * dashboard renders itself.  Dropping them instead would be worse: several of
 * them -- look-ahead refused, protection information, an SMR collapse -- are
 * the difference between a result you can trust and one you cannot.
 */
#define MSG_RING 4

static char g_msgring[MSG_RING][200];
static int g_msg_head, g_msg_count;
static int g_dash;
static int g_tui_again;   /* the summary screen asked for another test */
static int g_runs_seen;   /* the list of runs has already been offered once */

static void msg(const char *fmt, ...)
{
	va_list ap;

	if (g_dash) {
		char *p;

		va_start(ap, fmt);
		vsnprintf(g_msgring[g_msg_head], sizeof(g_msgring[0]), fmt, ap);
		va_end(ap);
		for (p = g_msgring[g_msg_head]; *p; p++)
			if (*p == '\n' || *p == '\r')
				*p = ' ';
		g_msg_head = (g_msg_head + 1) % MSG_RING;
		if (g_msg_count < MSG_RING)
			g_msg_count++;
	} else {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
	if (g_log) {
		va_start(ap, fmt);
		vfprintf(g_log, fmt, ap);
		va_end(ap);
	}
}

static void die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, PROG ": ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

static const char *c_red(void)   { return g_color ? "\033[31m" : ""; }
static const char *c_grn(void)   { return g_color ? "\033[32m" : ""; }
static const char *c_yel(void)   { return g_color ? "\033[33m" : ""; }
static const char *c_bold(void)  { return g_color ? "\033[1m"  : ""; }
static const char *c_off(void)   { return g_color ? "\033[0m"  : ""; }

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static const char *human_size(uint64_t b, char *buf, size_t n)
{
	static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
	double v = (double)b;
	int i = 0;

	while (v >= 1024.0 && i < 5) {
		v /= 1024.0;
		i++;
	}
	snprintf(buf, n, i == 0 ? "%.0f %s" : "%.2f %s", v, u[i]);
	return buf;
}

/* avoid pulling in libm just for fmod() */
static double fmod_local(double a, double b)
{
	return a - b * (double)(long long)(a / b);
}

static const char *human_time(double s, char *buf, size_t n)
{
	if (s < 0 || s != s)
		snprintf(buf, n, "unknown");
	else if (s < 90)
		snprintf(buf, n, "%.0fs", s);
	else if (s < 5400)
		/* truncate, not round: 119 s is 1m 59s, never "2m 59s" */
		snprintf(buf, n, "%lldm %02llds", (long long)(s / 60),
			 (long long)fmod_local(s, 60));
	else
		snprintf(buf, n, "%lldh %02lldm", (long long)(s / 3600),
			 (long long)(fmod_local(s, 3600) / 60));
	return buf;
}

static int parse_size(const char *s, uint64_t *out_v)
{
	char *end;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || end == s)
		return -1;
	while (*end == ' ')
		end++;
	switch (tolower((unsigned char)*end)) {
	case 'k': v <<= 10; end++; break;
	case 'm': v <<= 20; end++; break;
	case 'g': v <<= 30; end++; break;
	case 't': v <<= 40; end++; break;
	case 'p': v <<= 50; end++; break;
	case '\0': break;
	default: return -1;
	}
	if (tolower((unsigned char)*end) == 'i')
		end++;
	if (tolower((unsigned char)*end) == 'b')
		end++;
	if (*end)
		return -1;
	*out_v = v;
	return 0;
}

/*
 * A number that must actually be a number.  atoll() reads "abc" as 0, and 0 is
 * a legal value for the retry counts and the recovery limit -- a typo would
 * quietly tell the drive to give up on the first error instead of failing the
 * command line.
 */
static long long numarg(const char *opt, const char *v, long long lo,
			long long hi)
{
	char *end;
	long long n;

	errno = 0;
	n = strtoll(v, &end, 10);
	if (end == v || *end || errno || n < lo || n > hi)
		die("%s takes a number from %lld to %lld, or 'keep'", opt, lo,
		    hi);
	return n;
}

static char *trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static int read_file_line(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "re");

	if (!f)
		return -1;
	if (!fgets(buf, (int)n, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int sysfs_str(char *dst, size_t n, const char *fmt, ...)
{
	char path[PATH_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);
	if (read_file_line(path, dst, n) < 0) {
		dst[0] = 0;
		return -1;
	}
	memmove(dst, trim(dst), strlen(trim(dst)) + 1);
	return 0;
}

static long long sysfs_ll(const char *fmt, ...)
{
	char path[PATH_MAX], buf[64];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);
	if (read_file_line(path, buf, sizeof(buf)) < 0)
		return -1;
	return strtoll(buf, NULL, 10);
}

static int sysfs_write(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t w;

	if (fd < 0)
		return -1;
	w = write(fd, val, strlen(val));
	close(fd);
	return w < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ *
 * the run store
 *
 * A shelf of drives is not dealt with in one sitting.  Thirty drives are
 * being low level formatted, twenty-four more are being scanned, the terminal
 * is wanted for something else, and the machine gets walked away from -- so a
 * run cannot live inside the process that started it.
 *
 * Every run gets a directory, every drive in it gets one small text file, and
 * the process that holds that drive is the only thing that ever writes it.
 * Whatever wants to know what is happening reads those files: this program's
 * own dashboard, a second copy of it started an hour later, '--status' in a
 * script, or grep when this tool is not what is at hand.  Nothing has to
 * still be running for the answer to exist, and no reader has to ask
 * permission of a writer.
 *
 * Each record is rewritten whole through a temporary file and rename(2), so a
 * reader never sees half of one.  Keeping it text costs a few microseconds a
 * second and buys the one property that matters on a machine mid-way through
 * testing a shelf: the state is still legible when the tool that wrote it is
 * gone.
 * ------------------------------------------------------------------ */

#define RUN_VER 1

/*
 * A run directory is <store root>/runs/<run id>, and the files in it add a
 * drive slug on top of that.  Every one of those parts would be PATH_MAX on
 * its own, which cannot all fit in a PATH_MAX buffer, so the run directory
 * gets a bound of its own and anything deeper is refused rather than silently
 * truncated into a path that points somewhere else.
 */
#define STORE_MAX 768

/* copy a path into a fixed field, refusing one that does not fit */
static int pathcpy(char *dst, size_t n, const char *src)
{
	size_t len = strlen(src);

	if (len >= n)
		return -1;
	memcpy(dst, src, len + 1);
	return 0;
}

/*
 * Where a job is.  ORPHANED is never written: it is what a reader concludes
 * about a record that says "running" whose process is not there any more,
 * which is what a crash, a kill -9 or a reboot leaves behind.
 */
#define JS_QUEUED   0
#define JS_RUNNING  1
#define JS_DONE     2
#define JS_STOPPED  3
#define JS_ORPHANED 4

typedef struct {
	char slug[80];          /* the job file's stem, from the device name */
	char file[STORE_MAX + 128];     /* the job file itself */
	char dev[64];           /* device as it was named, sdb or a path */
	char path[PATH_MAX];
	char model[80];
	char serial[80];
	uint64_t size;
	int state;
	int verdict;            /* exit status once done, -1 until then */
	pid_t pid;
	unsigned long long pidstart;
	long long started, updated, ended;
	double pct, rate, eta;
	uint64_t bad, weak, slow, bytes;
	int too_slow;           /* under the throughput floor, see rate_judge() */
	double lat_med, lat_avg, lat_min, lat_max;
	double wlat_med;        /* 0 when nothing has been written */
	char report[PATH_MAX];
	char note[160];         /* the last thing this job had to say */
} jrec_t;

typedef struct {
	char id[48];            /* 20260912-201355-4711 */
	char dir[STORE_MAX];
	char kind[8];           /* scan or format */
	char what[40];          /* the short label the run list has room for */
	char detail[96];        /* the same thing said in full, for a header */
	char profile[24];
	char outdir[PATH_MAX];
	long long created, ended;
	pid_t sup;              /* the supervisor that owns this run */
	unsigned long long supstart;
	int njobs;
	int live;               /* derived, never stored */
} run_t;

static char g_store_dir[PATH_MAX];      /* --state-dir */
static char g_own_run[48];              /* the run this process started */

/*
 * A device named by path -- which is how an image file is given, and how any
 * device outside /dev would be -- would otherwise put slashes into the file
 * name, the open would fail, and the record would be lost with no complaint.
 * Use the last path component, restricted to what a filename can safely hold.
 */
static void report_slug(const char *name, char *out, size_t n)
{
	const char *base = strrchr(name, '/');
	size_t i;

	base = base ? base + 1 : name;
	for (i = 0; i + 1 < n && base[i]; i++)
		out[i] = (isalnum((unsigned char)base[i]) || base[i] == '.' ||
			  base[i] == '-' || base[i] == '_') ? base[i] : '_';
	out[i] = 0;
	if (!out[0])
		snprintf(out, n, "device");
}

static int mkpath(const char *path)
{
	char tmp[PATH_MAX];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/*
 * Where the store lives.  A real drive needs root, so a root run belongs
 * somewhere that survives a logout and that another root shell will find;
 * everything else (image files, --status from a normal user) goes under the
 * user's own state directory.
 */
static const char *store_root(void)
{
	static char root[PATH_MAX];
	const char *e;

	if (root[0])
		return root;
	if (g_store_dir[0])
		snprintf(root, sizeof(root), "%s", g_store_dir);
	else if ((e = getenv("HDDSCAN_STATE_DIR")) && *e)
		snprintf(root, sizeof(root), "%s", e);
	else if (geteuid() == 0)
		snprintf(root, sizeof(root), "/var/lib/" PROG);
	else if ((e = getenv("XDG_STATE_HOME")) && *e)
		snprintf(root, sizeof(root), "%s/" PROG, e);
	else if ((e = getenv("HOME")) && *e)
		snprintf(root, sizeof(root), "%s/.local/state/" PROG, e);
	else
		snprintf(root, sizeof(root), "/tmp/" PROG "-%u",
			 (unsigned)getuid());
	return root;
}

/*
 * Reading looks in more places than writing does.  A scan of real hardware
 * has to be root, and asking "what is running?" should not have to be: a
 * normal user's '--status' still finds root's runs when the permissions let
 * it, rather than reporting an empty machine that is in fact busy.
 */
static int store_roots(const char *v[], int max)
{
	int n = 0;

	if (max > 0)
		v[n++] = store_root();
	if (n < max && strcmp(store_root(), "/var/lib/" PROG) &&
	    !g_store_dir[0] && !getenv("HDDSCAN_STATE_DIR") &&
	    !access("/var/lib/" PROG "/runs", R_OK | X_OK))
		v[n++] = "/var/lib/" PROG;
	return n;
}

static unsigned long long pid_start(pid_t p)
{
	char path[64], buf[1024], *s;
	int fd, i;
	ssize_t n;
	unsigned long long v;

	if (p <= 0)
		return 0;
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)p);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	/*
	 * The command name sits in parentheses and may itself contain spaces
	 * and parentheses, so fields are counted from the last ')': state is
	 * the next one and starttime is the twentieth after it.
	 */
	s = strrchr(buf, ')');
	if (!s)
		return 0;
	s++;
	for (i = 0; i < 19; i++) {
		s += strspn(s, " ");
		s += strcspn(s, " ");
	}
	v = strtoull(s, NULL, 10);
	return v ? v : 1;   /* a live process must never answer zero */
}

/*
 * Liveness is a pid *and* the boot time it started at.  A pid alone is a lie
 * waiting to happen: pids wrap, and a week-old run whose number has since
 * been handed to something else would otherwise report itself as still
 * scanning, which is exactly the sort of confident wrong answer this tool
 * exists to avoid.
 */
static int pid_live(pid_t p, unsigned long long start)
{
	unsigned long long now = pid_start(p);

	if (!now)
		return 0;
	return !start || now == start;
}

/* key and value from one "key rest of line" record line */
static char *kv_split(char *line, char **val)
{
	char *k = line + strspn(line, " \t");
	char *v = k + strcspn(k, " \t\r\n");

	if (*v)
		*v++ = 0;
	*val = trim(v);
	return k;
}

static void jrec_put(const jrec_t *j)
{
	char tmp[sizeof(j->file) + 8];
	FILE *f;

	if (!j->file[0])
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", j->file);
	f = fopen(tmp, "we");
	if (!f)
		return;
	fprintf(f, "job %d\n", RUN_VER);
	fprintf(f, "device %s\n", j->dev);
	fprintf(f, "path %s\n", j->path);
	if (j->model[0])
		fprintf(f, "model %s\n", j->model);
	if (j->serial[0])
		fprintf(f, "serial %s\n", j->serial);
	fprintf(f, "size %" PRIu64 "\n", j->size);
	fprintf(f, "state %d\n", j->state);
	fprintf(f, "verdict %d\n", j->verdict);
	fprintf(f, "pid %d\npidstart %llu\n", (int)j->pid, j->pidstart);
	fprintf(f, "started %lld\nupdated %lld\nended %lld\n",
		j->started, j->updated, j->ended);
	fprintf(f, "pct %.3f\nrate %.0f\neta %.1f\n", j->pct, j->rate, j->eta);
	fprintf(f, "bad %" PRIu64 "\nweak %" PRIu64 "\nslow %" PRIu64
		"\nbytes %" PRIu64 "\n", j->bad, j->weak, j->slow, j->bytes);
	fprintf(f, "lat %.2f %.2f %.2f %.2f\n", j->lat_med, j->lat_avg,
		j->lat_min, j->lat_max);
	if (j->too_slow)
		fprintf(f, "tooslow 1\n");
	if (j->wlat_med > 0)
		fprintf(f, "wlat %.2f\n", j->wlat_med);
	if (j->report[0])
		fprintf(f, "report %s\n", j->report);
	if (j->note[0])
		fprintf(f, "note %s\n", j->note);
	fclose(f);
	rename(tmp, j->file);
}

static int jrec_get(const char *path, jrec_t *j)
{
	FILE *f = fopen(path, "re");
	char line[1024];
	const char *base;

	if (!f)
		return -1;
	memset(j, 0, sizeof(*j));
	j->verdict = -1;
	if (pathcpy(j->file, sizeof(j->file), path) < 0) {
		fclose(f);
		return -1;
	}
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	{
		size_t len = strlen(base);
		char *dot;

		if (len >= sizeof(j->slug))
			len = sizeof(j->slug) - 1;
		memcpy(j->slug, base, len);
		j->slug[len] = 0;
		dot = strrchr(j->slug, '.');
		if (dot && !strcmp(dot, ".job"))
			*dot = 0;
	}
	while (fgets(line, sizeof(line), f)) {
		char *v, *k = kv_split(line, &v);

#define JSTR(key, dst) if (!strcmp(k, key)) { \
		snprintf(dst, sizeof(dst), "%s", v); continue; }
		JSTR("device", j->dev)
		JSTR("path", j->path)
		JSTR("model", j->model)
		JSTR("serial", j->serial)
		JSTR("report", j->report)
		JSTR("note", j->note)
#undef JSTR
		if (!strcmp(k, "size"))
			j->size = strtoull(v, NULL, 10);
		else if (!strcmp(k, "state"))
			j->state = atoi(v);
		else if (!strcmp(k, "verdict"))
			j->verdict = atoi(v);
		else if (!strcmp(k, "tooslow"))
			j->too_slow = atoi(v);
		else if (!strcmp(k, "wlat"))
			j->wlat_med = atof(v);
		else if (!strcmp(k, "pid"))
			j->pid = (pid_t)atoi(v);
		else if (!strcmp(k, "pidstart"))
			j->pidstart = strtoull(v, NULL, 10);
		else if (!strcmp(k, "started"))
			j->started = strtoll(v, NULL, 10);
		else if (!strcmp(k, "updated"))
			j->updated = strtoll(v, NULL, 10);
		else if (!strcmp(k, "ended"))
			j->ended = strtoll(v, NULL, 10);
		else if (!strcmp(k, "pct"))
			j->pct = atof(v);
		else if (!strcmp(k, "rate"))
			j->rate = atof(v);
		else if (!strcmp(k, "eta"))
			j->eta = atof(v);
		else if (!strcmp(k, "bad"))
			j->bad = strtoull(v, NULL, 10);
		else if (!strcmp(k, "weak"))
			j->weak = strtoull(v, NULL, 10);
		else if (!strcmp(k, "slow"))
			j->slow = strtoull(v, NULL, 10);
		else if (!strcmp(k, "bytes"))
			j->bytes = strtoull(v, NULL, 10);
		else if (!strcmp(k, "lat"))
			sscanf(v, "%lf %lf %lf %lf", &j->lat_med, &j->lat_avg,
			       &j->lat_min, &j->lat_max);
	}
	fclose(f);
	/*
	 * A record that claims to be running whose process is gone was killed
	 * outright -- kill -9, a crash, or the power going.  Say that, rather
	 * than showing a percentage that will never move again.
	 */
	if (j->state == JS_RUNNING && !pid_live(j->pid, j->pidstart))
		j->state = JS_ORPHANED;
	return 0;
}

/* what a job's state is called, for one narrow column */
static const char *jstate_name(const jrec_t *j)
{
	switch (j->state) {
	case JS_QUEUED: return "queued";
	case JS_RUNNING: return "running";
	case JS_STOPPED: return "stopped";
	case JS_ORPHANED: return "orphaned";
	}
	return "done";
}

/*
 * What to put in a name column.  The record keeps the device as it was named
 * -- which is what --confirm is matched against and what says whether two
 * runs are fighting over the same drive -- but an image file named by path
 * would otherwise take the whole column and push everything after it out of
 * line.
 */
static const char *jrec_name(const jrec_t *j)
{
	const char *p = strrchr(j->dev, '/');

	return (p && p[1]) ? p + 1 : j->dev;
}

static int jrec_active(const jrec_t *j)
{
	return j->state == JS_QUEUED || j->state == JS_RUNNING;
}

static int store_jobs(const run_t *r, jrec_t **out);

static void run_put(const run_t *r)
{
	char path[sizeof(r->dir) + 16], tmp[sizeof(r->dir) + 16];
	FILE *f;

	snprintf(path, sizeof(path), "%s/run", r->dir);
	snprintf(tmp, sizeof(tmp), "%s/run.tmp", r->dir);
	f = fopen(tmp, "we");
	if (!f)
		return;
	fprintf(f, "run %d\n", RUN_VER);
	fprintf(f, "id %s\n", r->id);
	fprintf(f, "kind %s\n", r->kind);
	fprintf(f, "what %s\n", r->what);
	fprintf(f, "detail %s\n", r->detail);
	fprintf(f, "profile %s\n", r->profile);
	fprintf(f, "outdir %s\n", r->outdir);
	fprintf(f, "created %lld\nended %lld\n", r->created, r->ended);
	fprintf(f, "sup %d\nsupstart %llu\n", (int)r->sup, r->supstart);
	fprintf(f, "njobs %d\n", r->njobs);
	fclose(f);
	rename(tmp, path);
}

static int run_get(const char *dir, run_t *r)
{
	char path[STORE_MAX + 16];
	FILE *f;
	char line[1024];
	const char *base;

	if (strlen(dir) >= sizeof(r->dir))
		return -1;
	snprintf(path, sizeof(path), "%s/run", dir);
	f = fopen(path, "re");
	if (!f)
		return -1;
	memset(r, 0, sizeof(*r));
	pathcpy(r->dir, sizeof(r->dir), dir);
	base = strrchr(dir, '/');
	snprintf(r->id, sizeof(r->id), "%s", base ? base + 1 : dir);
	while (fgets(line, sizeof(line), f)) {
		char *v, *k = kv_split(line, &v);

#define RSTR(key, dst) if (!strcmp(k, key)) { \
		snprintf(dst, sizeof(dst), "%s", v); continue; }
		RSTR("id", r->id)
		RSTR("kind", r->kind)
		RSTR("what", r->what)
		RSTR("detail", r->detail)
		RSTR("profile", r->profile)
		RSTR("outdir", r->outdir)
#undef RSTR
		if (!strcmp(k, "created"))
			r->created = strtoll(v, NULL, 10);
		else if (!strcmp(k, "ended"))
			r->ended = strtoll(v, NULL, 10);
		else if (!strcmp(k, "sup"))
			r->sup = (pid_t)atoi(v);
		else if (!strcmp(k, "supstart"))
			r->supstart = strtoull(v, NULL, 10);
		else if (!strcmp(k, "njobs"))
			r->njobs = atoi(v);
	}
	fclose(f);
	if (!r->kind[0])
		snprintf(r->kind, sizeof(r->kind), "scan");
	if (!r->detail[0])
		snprintf(r->detail, sizeof(r->detail), "%s", r->what);
	/*
	 * A run is live while its supervisor is.  The window between creating
	 * the directory and the supervisor writing its own pid into it is a
	 * few milliseconds wide, so a run with no pid yet counts as starting
	 * -- but only briefly, or a supervisor that died before it could
	 * record itself would stay "running" forever.
	 */
	if (r->ended)
		r->live = 0;
	else if (r->sup <= 0)
		r->live = time(NULL) - r->created < 30;
	else if (pid_live(r->sup, r->supstart))
		r->live = 1;
	else {
		/*
		 * The supervisor is gone but a worker need not be: killing
		 * the supervisor outright leaves each drive's worker reading
		 * its own drive to the end, and a run with a drive still
		 * being read is still a run.  A record that says running has
		 * already been checked against its worker's pid by jrec_get,
		 * so this is the honest question and not a guess.
		 */
		jrec_t *j;
		int n = store_jobs(r, &j), i;

		for (i = 0; i < n; i++)
			if (j[i].state == JS_RUNNING) {
				r->live = 1;
				break;
			}
		free(j);
	}
	return 0;
}

static int run_cmp(const void *a, const void *b)
{
	const run_t *x = a, *y = b;

	if (x->live != y->live)
		return x->live ? -1 : 1;
	if (x->created != y->created)
		return x->created > y->created ? -1 : 1;
	return strcmp(y->id, x->id);
}

/*
 * What order the drives in a run are listed in.  readdir gives whatever order
 * the filesystem feels like, and a dashboard whose rows move between frames
 * is unreadable -- but so is one where the four drives actually being worked
 * on are below the twenty-six still queued, since the list is cut off at the
 * bottom of the terminal.  So: what is happening now, then what went wrong,
 * then what is waiting, then what came back clean, and inside each group by
 * name.  Rows move only when something has actually changed state.
 */
static int jrec_rank(const jrec_t *j)
{
	switch (j->state) {
	case JS_RUNNING: return 0;
	case JS_ORPHANED: return 1;
	case JS_QUEUED: return 3;
	}
	return j->verdict ? 2 : 4;
}

static int jrec_cmp_order(const void *a, const void *b)
{
	const jrec_t *x = a, *y = b;
	int rx = jrec_rank(x), ry = jrec_rank(y);

	if (rx != ry)
		return rx < ry ? -1 : 1;
	return strcmp(x->dev, y->dev);
}

static int store_jobs(const run_t *r, jrec_t **out)
{
	DIR *d = opendir(r->dir);
	struct dirent *e;
	jrec_t *v = NULL;
	int n = 0;

	*out = NULL;
	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char path[sizeof(r->dir) + 300];
		size_t len = strlen(e->d_name);
		jrec_t j;

		if (len < 5 || strcmp(e->d_name + len - 4, ".job"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", r->dir, e->d_name);
		if (jrec_get(path, &j) < 0)
			continue;
		v = realloc(v, (size_t)(n + 1) * sizeof(*v));
		if (!v) {
			closedir(d);
			return 0;
		}
		v[n++] = j;
	}
	closedir(d);
	if (n > 1)
		qsort(v, (size_t)n, sizeof(*v), jrec_cmp_order);
	*out = v;
	return n;
}

static int store_list(run_t **out)
{
	const char *roots[2];
	int nroot = store_roots(roots, 2);
	run_t *v = NULL;
	int n = 0, i;

	*out = NULL;
	for (i = 0; i < nroot; i++) {
		char base[STORE_MAX];
		DIR *d;
		struct dirent *e;

		if (snprintf(base, sizeof(base), "%s/runs", roots[i]) >=
		    (int)sizeof(base))
			continue;   /* too deep to hold a run; see STORE_MAX */
		d = opendir(base);
		if (!d)
			continue;
		while ((e = readdir(d))) {
			char dir[STORE_MAX + 300];
			run_t r;
			int k, dup = 0;

			if (e->d_name[0] == '.')
				continue;
			snprintf(dir, sizeof(dir), "%s/%s", base, e->d_name);
			if (run_get(dir, &r) < 0)
				continue;
			for (k = 0; k < n; k++)
				if (!strcmp(v[k].id, r.id))
					dup = 1;
			if (dup)
				continue;
			v = realloc(v, (size_t)(n + 1) * sizeof(*v));
			if (!v) {
				closedir(d);
				return 0;
			}
			v[n++] = r;
		}
		closedir(d);
	}
	if (n > 1)
		qsort(v, (size_t)n, sizeof(*v), run_cmp);
	*out = v;
	return n;
}

static int store_live_count(void)
{
	run_t *v;
	int n = store_list(&v), i, live = 0;

	for (i = 0; i < n; i++)
		if (v[i].live)
			live++;
	free(v);
	return live;
}

/*
 * Is this drive already being worked on?  Two runs on one spindle would time
 * each other's seeks and both would be wrong, so the picker and the command
 * line both refuse a drive that is already in a live run.  The run this
 * process started is not a conflict with itself.
 */
static int store_busy(const char *dev, char *who, size_t wn)
{
	run_t *v;
	int n = store_list(&v), i, k, busy = 0;

	for (i = 0; i < n && !busy; i++) {
		jrec_t *j;
		int nj;

		if (!v[i].live || !strcmp(v[i].id, g_own_run))
			continue;
		nj = store_jobs(&v[i], &j);
		for (k = 0; k < nj; k++) {
			if (strcmp(j[k].dev, dev) || !jrec_active(&j[k]))
				continue;
			if (who)
				snprintf(who, wn, "%s", v[i].id);
			busy = 1;
			break;
		}
		free(j);
	}
	free(v);
	return busy;
}

static int store_rmrun(const run_t *r)
{
	DIR *d = opendir(r->dir);
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char path[sizeof(r->dir) + 300];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", r->dir, e->d_name);
		unlink(path);
	}
	closedir(d);
	return rmdir(r->dir);
}

/*
 * The store is a record of what the machine has been doing, not an archive.
 * Keep the finished runs for a week and at most sixty-four of them, so a
 * scripted sweep of a shelf every night does not grow without bound, and
 * never touch one that is still live.
 */
#define STORE_KEEP_DAYS 7
#define STORE_KEEP_RUNS 64

static void store_prune(void)
{
	run_t *v;
	int n = store_list(&v), i, kept = 0;
	long long now = (long long)time(NULL);

	for (i = 0; i < n; i++) {
		if (v[i].live || !strcmp(v[i].id, g_own_run))
			continue;
		kept++;
		if (kept > STORE_KEEP_RUNS ||
		    now - v[i].created > (long long)STORE_KEEP_DAYS * 86400)
			store_rmrun(&v[i]);
	}
	free(v);
}

/*
 * Find the run a name refers to: a run id, a prefix of one, or the name of a
 * drive that is in one.  Prefers a live run, because "what is sdb doing" is
 * almost always about now rather than about last Tuesday.
 */
static int store_find(const char *name, run_t *out)
{
	run_t *v;
	int n = store_list(&v), i, k, found = 0;

	for (i = 0; i < n && !found; i++) {
		jrec_t *j;
		int nj;

		if (!strncmp(v[i].id, name, strlen(name))) {
			*out = v[i];
			found = 1;
			break;
		}
		nj = store_jobs(&v[i], &j);
		for (k = 0; k < nj; k++) {
			char slug[80];

			report_slug(name, slug, sizeof(slug));
			if (strcmp(j[k].dev, name) && strcmp(j[k].slug, slug))
				continue;
			*out = v[i];
			found = 1;
			break;
		}
		free(j);
	}
	free(v);
	return found ? 0 : -1;
}

/*
 * Stopping a run means stopping its supervisor, which forwards one signal to
 * each worker and lets it write the report for the part of the drive it did
 * cover.  Two things matter here and both have bitten:
 *
 * Signal the supervisor, not the workers, while it is alive.  Killing the
 * workers directly leaves the supervisor to start the next queued drive,
 * which is the opposite of what was asked.
 *
 * And exactly once per worker.  A second SIGINT is the deliberate "stop
 * arguing and get out" path -- it abandons the report -- so a stop that
 * reached a worker both directly and through its supervisor destroyed the
 * very thing stopping is supposed to preserve.
 */
static int store_stop(const run_t *r)
{
	jrec_t *j;
	int n, i, hit = 0;

	if (r->sup > 0 && pid_live(r->sup, r->supstart))
		return kill(r->sup, SIGINT) ? -1 : 0;
	/* no supervisor left: its workers are on their own, so tell them */
	n = store_jobs(r, &j);
	for (i = 0; i < n; i++)
		if (j[i].state == JS_RUNNING && j[i].pid > 0 &&
		    pid_live(j[i].pid, j[i].pidstart))
			if (!kill(j[i].pid, SIGINT))
				hit = 1;
	free(j);
	return hit ? 0 : -1;
}

/*
 * The writer side of a job record.  A worker owns exactly one of these and
 * rewrites it as it goes; the supervisor reads it back and patches the state
 * in when the worker exits, so neither has to know what the other was about
 * to write.
 */
static jrec_t g_job;
static int g_job_own;           /* this process is the worker for g_job */

static void job_flush(int state)
{
	if (!g_job_own || !g_job.file[0])
		return;
	g_job.state = state;
	g_job.updated = (long long)time(NULL);
	jrec_put(&g_job);
}

/* ------------------------------------------------------------------ *
 * latency histogram
 * ------------------------------------------------------------------ */

#define NBUCKETS 24
#define NTIERS 7
static const uint64_t lat_tiers[NTIERS] = { 25000, 50000, 100000, 250000,
					    500000, 1000000, 5000000 };

static const uint64_t bucket_hi[NBUCKETS] = {
	100, 200, 500, 1000, 2000, 5000, 10000, 15000, 20000, 30000,
	50000, 75000, 100000, 150000, 200000, 300000, 500000, 750000,
	1000000, 2000000, 5000000, 10000000, 30000000, UINT64_MAX
};

typedef struct {
	uint64_t count;
	uint64_t sum_us;
	uint64_t min_us;
	uint64_t max_us;
	uint64_t hist[NBUCKETS];
	/*
	 * The "reads exceeding N ms" table used to be derived from the
	 * histogram, which counts a whole bucket as over the line the moment
	 * the line falls inside it.  On a drive whose chunks all landed in the
	 * 20-30 ms bucket that reported 284 reads over 25 ms when the slowest
	 * read of the whole scan was 22.37 ms, and flatly contradicted the
	 * exact "chunks over budget 0" three sections further down.  Count
	 * them exactly instead; a report that argues with itself is worse than
	 * one that omits the number.
	 */
	uint64_t over[NTIERS];
} lat_t;

static void lat_init(lat_t *l)
{
	memset(l, 0, sizeof(*l));
	l->min_us = UINT64_MAX;
}

static void lat_add(lat_t *l, uint64_t us)
{
	int i;

	l->count++;
	for (i = 0; i < NTIERS; i++)
		if (us > lat_tiers[i])
			l->over[i]++;
	l->sum_us += us;
	if (us < l->min_us)
		l->min_us = us;
	if (us > l->max_us)
		l->max_us = us;
	for (i = 0; i < NBUCKETS; i++) {
		if (us <= bucket_hi[i]) {
			l->hist[i]++;
			return;
		}
	}
	l->hist[NBUCKETS - 1]++;
}

/*
 * Approximate percentile by interpolating inside the bucket the sample falls
 * in.  The buckets are wide, so the estimate is clamped to the range actually
 * observed: without that a run whose reads all sat in the 20-30 ms bucket
 * reported p50 24.7 ms and p99.9 30.0 ms next to an exact max of 22.4 ms,
 * which cannot be true of any set of numbers.
 */
static double lat_pct_raw(const lat_t *l, double p)
{
	uint64_t target, seen = 0;
	int i;

	if (!l->count)
		return 0;
	target = (uint64_t)(p / 100.0 * (double)l->count + 0.5);
	if (target == 0)
		target = 1;
	for (i = 0; i < NBUCKETS; i++) {
		if (seen + l->hist[i] >= target) {
			uint64_t lo = i ? bucket_hi[i - 1] : 0;
			uint64_t hi = bucket_hi[i];
			double frac;

			if (hi == UINT64_MAX)
				return (double)l->max_us;
			if (!l->hist[i])
				return (double)hi;
			frac = (double)(target - seen) / (double)l->hist[i];
			return (double)lo + frac * (double)(hi - lo);
		}
		seen += l->hist[i];
	}
	return (double)l->max_us;
}

static double lat_pct(const lat_t *l, double p)
{
	double v = lat_pct_raw(l, p);

	if (!l->count)
		return 0;
	if (v > (double)l->max_us)
		v = (double)l->max_us;
	if (l->min_us != UINT64_MAX && v < (double)l->min_us)
		v = (double)l->min_us;
	return v;
}

/* ------------------------------------------------------------------ *
 * device model
 * ------------------------------------------------------------------ */

/*
 * How the disk is attached.  This matters because several transports present
 * a device that claims to be rotational but whose latency has nothing to do
 * with a spinning platter: an iSCSI LUN measures the network and the storage
 * server behind it, a virtio disk measures the hypervisor's page cache.
 * Certifying one of those as a healthy HDD would be meaningless, so anything
 * that is not locally attached needs an explicit opt-in.
 */
typedef enum {
	TR_UNKNOWN = 0,
	TR_ATA,
	TR_SCSI,
	TR_USB,
	TR_NVME,
	TR_MMC,
	TR_ISCSI,
	TR_FC,
	TR_VIRTIO,
	TR_VMBUS,
	TR_LOOP
} transport_t;

static const char *zoned_name(int z)
{
	return z == 2 ? "host-managed SMR" : z == 1 ? "host-aware SMR" : "conventional";
}

static const char *transport_name(transport_t t)
{
	switch (t) {
	case TR_ATA: return "SATA/ATA";
	case TR_SCSI: return "SAS/SCSI";
	case TR_USB: return "USB";
	case TR_NVME: return "NVMe";
	case TR_MMC: return "SD/MMC";
	case TR_ISCSI: return "iSCSI";
	case TR_FC: return "FibreChannel";
	case TR_VIRTIO: return "virtio";
	case TR_VMBUS: return "Hyper-V";
	case TR_LOOP: return "loop";
	default: return "unknown";
	}
}

/* is this a physical drive whose latency reflects its own mechanics? */
static int transport_is_local(transport_t t)
{
	return t == TR_ATA || t == TR_SCSI || t == TR_USB ||
	       t == TR_NVME || t == TR_MMC || t == TR_UNKNOWN;
}

#define LATWIN 4096
#define LATWIN_SECS 30
typedef struct {
	struct {
		uint64_t at_us;
		uint32_t us;
	} s[LATWIN];
	int head;
	int n;
} latwin_t;

typedef struct {
	char name[64];          /* sda */
	char path[128];         /* /dev/sda */
	char model[80];
	char vendor[32];
	char serial[80];
	char byid[288];
	transport_t transport;
	char syspath[PATH_MAX];
	uint64_t size;
	int logical_bs;
	int physical_bs;
	int rotational;
	int prot_type;          /* T10 PI: -1 unknown, 0 none, 1..3 type */
	char group[24];         /* HBA/host the drive hangs off, for bulk select */
	int zoned;              /* 0 none, 1 host-aware, 2 host-managed */
	uint64_t zone_sectors;
	int removable;
	int readonly;
	int is_file;            /* a regular file used as an image */
	dev_t rdev;
	dev_t part_rdev[64];
	int nparts;

	/* safety verdict */
	int unsafe;
	char unsafe_why[1024];
	int is_root_disk;
	int no_access;          /* could not even open it: almost always "not root" */
} device_t;

/*
 * T10 protection information.  A SCSI drive formatted with PI stores an
 * 8-byte guard and tag beside every logical block and the kernel then reads
 * with RDPROTECT set, so the drive checks the guard against the data on every
 * read.  A block that has never been written since the format has no valid
 * guard, and the read comes back "Aborted Command / Logical block guard check
 * failed" -- EILSEQ to us -- while the platter under it is perfect.  Knowing
 * the format up front is what lets a scan say that instead of condemning a
 * healthy drive, so this is read for every device, not just on demand.
 */
static void probe_protection(device_t *d);
static void probe_group(device_t *d);

/*
 * What to put in a name column.  A device named by path -- an image file,
 * usually -- otherwise either takes the whole column and pushes everything
 * after it out of line, or gets truncated to a meaningless prefix like
 * "/tmp/tmp".
 */
static const char *disp_name(const device_t *d)
{
	const char *p = strrchr(d->name, '/');

	return (p && p[1]) ? p + 1 : d->name;
}

static int name_ok(const char *n)
{
	size_t i;

	if (!n[0] || strlen(n) > 48)
		return 0;
	for (i = 0; n[i]; i++)
		if (!isalnum((unsigned char)n[i]) && n[i] != '-' && n[i] != '_')
			return 0;
	return 1;
}

static int skip_blockdev(const char *n)
{
	static const char *pfx[] = { "loop", "ram", "zram", "dm-", "md", "sr",
				     "fd", "nbd", "zd", NULL };
	int i;

	for (i = 0; pfx[i]; i++)
		if (!strncmp(n, pfx[i], strlen(pfx[i])))
			return 1;
	return 0;
}

static void lookup_byid(device_t *d)
{
	DIR *dir = opendir("/dev/disk/by-id");
	struct dirent *e;
	char best[288] = "";

	if (!dir)
		return;
	while ((e = readdir(dir))) {
		char link[PATH_MAX], real[PATH_MAX];
		const char *base;

		if (e->d_name[0] == '.')
			continue;
		snprintf(link, sizeof(link), "/dev/disk/by-id/%s", e->d_name);
		if (!realpath(link, real))
			continue;
		base = strrchr(real, '/');
		if (!base || strcmp(base + 1, d->name))
			continue;
		/* prefer a stable ata-/scsi-/nvme- id over wwn- */
		if (!best[0] || (!strncmp(e->d_name, "ata-", 4) ||
				 !strncmp(e->d_name, "scsi-", 5)))
			snprintf(best, sizeof(best), "%s", e->d_name);
	}
	closedir(dir);
	snprintf(d->byid, sizeof(d->byid), "%s", best);

	/* many by-id names end in the serial number */
	if (!d->serial[0] && best[0]) {
		const char *u = strrchr(best, '_');

		if (u && u[1])
			snprintf(d->serial, sizeof(d->serial), "%s", u + 1);
	}
}

static int path_has(const char *hay, const char *needle)
{
	return strstr(hay, needle) != NULL;
}

static void probe_transport(device_t *d)
{
	char link[PATH_MAX], real[PATH_MAX];
	struct stat st;

	d->transport = TR_UNKNOWN;
	snprintf(link, sizeof(link), "/sys/block/%s", d->name);
	if (!realpath(link, real))
		return;
	snprintf(d->syspath, sizeof(d->syspath), "%s", real);

	/*
	 * An iSCSI disk sits under a scsi_transport session node.  The device
	 * path alone ("/session1/") is a strong hint; confirm it against the
	 * iscsi class so a coincidental "session" in some other bus path does
	 * not get misfiled.
	 */
	if (path_has(real, "/session") && !stat("/sys/class/iscsi_session", &st)) {
		d->transport = TR_ISCSI;
		return;
	}
	if (path_has(real, "/rport-") || path_has(real, "fc_host")) {
		d->transport = TR_FC;
		return;
	}
	if (path_has(real, "/virtio")) {
		d->transport = TR_VIRTIO;
		return;
	}
	if (path_has(real, "/vmbus") || path_has(real, "VMBUS")) {
		d->transport = TR_VMBUS;
		return;
	}
	if (path_has(real, "/usb")) {
		d->transport = TR_USB;
		return;
	}
	if (path_has(real, "/nvme")) {
		d->transport = TR_NVME;
		return;
	}
	if (path_has(real, "/mmc")) {
		d->transport = TR_MMC;
		return;
	}
	if (path_has(real, "/ata")) {
		d->transport = TR_ATA;
		return;
	}
	if (path_has(real, "/host") || path_has(real, "/target"))
		d->transport = TR_SCSI;
}

static void probe_partitions(device_t *d)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *e;

	snprintf(path, sizeof(path), "/sys/block/%s", d->name);
	dir = opendir(path);
	if (!dir)
		return;
	while ((e = readdir(dir)) && d->nparts < 64) {
		char p[PATH_MAX], devbuf[64];
		struct stat st;
		unsigned maj, min;

		if (strncmp(e->d_name, d->name, strlen(d->name)))
			continue;
		snprintf(p, sizeof(p), "/sys/block/%s/%s/partition", d->name, e->d_name);
		if (stat(p, &st) < 0)
			continue;
		snprintf(p, sizeof(p), "/sys/block/%s/%s/dev", d->name, e->d_name);
		if (read_file_line(p, devbuf, sizeof(devbuf)) < 0)
			continue;
		if (sscanf(devbuf, "%u:%u", &maj, &min) != 2)
			continue;
		d->part_rdev[d->nparts++] = makedev(maj, min);
	}
	closedir(dir);
}

static int probe_device(const char *name, device_t *d)
{
	char buf[128];
	long long sz;
	unsigned maj, min;

	memset(d, 0, sizeof(*d));
	snprintf(d->name, sizeof(d->name), "%s", name);
	snprintf(d->path, sizeof(d->path), "/dev/%s", name);

	sz = sysfs_ll("/sys/block/%s/size", name);
	if (sz <= 0)
		return -1;
	d->size = (uint64_t)sz * 512ull;

	d->rotational = (int)sysfs_ll("/sys/block/%s/queue/rotational", name);
	d->removable = (int)sysfs_ll("/sys/block/%s/removable", name);
	d->readonly = (int)sysfs_ll("/sys/block/%s/ro", name);
	d->logical_bs = (int)sysfs_ll("/sys/block/%s/queue/logical_block_size", name);
	d->physical_bs = (int)sysfs_ll("/sys/block/%s/queue/physical_block_size", name);
	if (d->logical_bs <= 0)
		d->logical_bs = 512;
	if (d->physical_bs <= 0)
		d->physical_bs = d->logical_bs;

	sysfs_str(d->model, sizeof(d->model), "/sys/block/%s/device/model", name);
	sysfs_str(d->vendor, sizeof(d->vendor), "/sys/block/%s/device/vendor", name);
	sysfs_str(d->serial, sizeof(d->serial), "/sys/block/%s/device/serial", name);
	if (!d->model[0])
		sysfs_str(d->model, sizeof(d->model), "/sys/block/%s/device/name", name);

	if (!sysfs_str(buf, sizeof(buf), "/sys/block/%s/dev", name) &&
	    sscanf(buf, "%u:%u", &maj, &min) == 2)
		d->rdev = makedev(maj, min);

	{
		char z[32];

		sysfs_str(z, sizeof(z), "/sys/block/%s/queue/zoned", name);
		if (!strcmp(z, "host-managed"))
			d->zoned = 2;
		else if (!strcmp(z, "host-aware"))
			d->zoned = 1;
		d->zone_sectors =
			(uint64_t)sysfs_ll("/sys/block/%s/queue/chunk_sectors", name);
	}

	probe_protection(d);
	probe_group(d);
	probe_transport(d);
	probe_partitions(d);
	lookup_byid(d);
	return 0;
}

/*
 * Which controller a drive hangs off, so a shelf of them can be selected as a
 * unit.  Ticking 200 drives one at a time is not a workflow.  The SCSI host
 * number is the useful granularity: one HBA, one expander, one enclosure's
 * worth of drives all share it.
 */
static void probe_group(device_t *d)
{
	char link[PATH_MAX], path[PATH_MAX];
	int hc, c, t, l;
	const char *h;
	ssize_t n;

	snprintf(d->group, sizeof(d->group), "%s",
		 d->is_file ? "file" : "-");
	if (d->is_file || !name_ok(d->name))
		return;
	snprintf(path, sizeof(path), "/sys/block/%s/device", d->name);
	n = readlink(path, link, sizeof(link) - 1);
	if (n <= 0)
		return;
	link[n] = 0;
	/*
	 * The link is relative and short -- ../../../6:0:1:0 -- so there is no
	 * "host" in it to look for.  The last component is the SCSI H:C:T:L
	 * tuple and H is the host.  Anything else (nvme0, and whatever comes
	 * next) is grouped by that last component as-is, which is still the
	 * controller.
	 */
	h = strrchr(link, '/');
	h = h ? h + 1 : link;
	if (sscanf(h, "%d:%d:%d:%d", &hc, &c, &t, &l) == 4)
		snprintf(d->group, sizeof(d->group), "host%d", hc);
	else if (*h)
		snprintf(d->group, sizeof(d->group), "%.*s",
			 (int)sizeof(d->group) - 1, h);
}

static void probe_protection(device_t *d)
{
	char dir[PATH_MAX], buf[32];
	struct dirent *e;
	DIR *dp;

	d->prot_type = -1;
	if (d->is_file || !name_ok(d->name))
		return;
	snprintf(dir, sizeof(dir), "/sys/block/%s/device/scsi_disk", d->name);
	dp = opendir(dir);
	if (!dp)
		return;
	while ((e = readdir(dp))) {
		if (e->d_name[0] == '.')
			continue;
		if (!sysfs_str(buf, sizeof(buf), "%s/%s/protection_type",
			       dir, e->d_name))
			d->prot_type = atoi(buf);
		break;
	}
	closedir(dp);
}

static int enumerate_devices(device_t **out)
{
	DIR *dir = opendir("/sys/block");
	struct dirent *e;
	device_t *v = NULL;
	int n = 0, cap = 0;

	if (!dir)
		die("cannot open /sys/block: %s", strerror(errno));
	while ((e = readdir(dir))) {
		device_t d;

		if (e->d_name[0] == '.' || skip_blockdev(e->d_name))
			continue;
		if (probe_device(e->d_name, &d) < 0)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			v = realloc(v, (size_t)cap * sizeof(*v));
			if (!v)
				die("out of memory");
		}
		v[n++] = d;
	}
	closedir(dir);
	*out = v;
	return n;
}

/* ------------------------------------------------------------------ *
 * safety: is this disk in use?
 * ------------------------------------------------------------------ */

static void add_why(device_t *d, const char *fmt, ...)
{
	size_t len = strlen(d->unsafe_why);
	va_list ap;

	if (len + 4 >= sizeof(d->unsafe_why))
		return;
	if (len)
		len += (size_t)snprintf(d->unsafe_why + len,
					sizeof(d->unsafe_why) - len, "; ");
	va_start(ap, fmt);
	vsnprintf(d->unsafe_why + len, sizeof(d->unsafe_why) - len, fmt, ap);
	va_end(ap);
	d->unsafe = 1;
}

static int rdev_belongs(const device_t *d, dev_t r)
{
	int i;

	if (r == d->rdev)
		return 1;
	for (i = 0; i < d->nparts; i++)
		if (r == d->part_rdev[i])
			return 1;
	return 0;
}

static void check_mounts(device_t *d)
{
	FILE *f = fopen("/proc/mounts", "re");
	char line[4096];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char src[1024], mnt[1024];
		struct stat st;

		if (sscanf(line, "%1023s %1023s", src, mnt) != 2)
			continue;
		if (src[0] != '/')
			continue;
		if (stat(src, &st) < 0 || !S_ISBLK(st.st_mode))
			continue;
		if (!rdev_belongs(d, st.st_rdev))
			continue;
		add_why(d, "%s is mounted at %s", src, mnt);
		if (!strcmp(mnt, "/"))
			d->is_root_disk = 1;
	}
	fclose(f);
}

static void check_swap(device_t *d)
{
	FILE *f = fopen("/proc/swaps", "re");
	char line[2048];
	int first = 1;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char src[1024];
		struct stat st;

		if (first) {
			first = 0;
			continue;
		}
		if (sscanf(line, "%1023s", src) != 1 || src[0] != '/')
			continue;
		if (stat(src, &st) < 0 || !S_ISBLK(st.st_mode))
			continue;
		if (rdev_belongs(d, st.st_rdev))
			add_why(d, "%s is an active swap device", src);
	}
	fclose(f);
}

static void check_holders_dir(device_t *d, const char *dirpath, const char *who)
{
	DIR *dir = opendir(dirpath);
	struct dirent *e;

	if (!dir)
		return;
	while ((e = readdir(dir))) {
		if (e->d_name[0] == '.')
			continue;
		add_why(d, "%s is claimed by %s (md/lvm/dm/bcache)", who, e->d_name);
	}
	closedir(dir);
}

static void check_holders(device_t *d)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *e;

	snprintf(path, sizeof(path), "/sys/block/%s/holders", d->name);
	check_holders_dir(d, path, d->name);

	snprintf(path, sizeof(path), "/sys/block/%s", d->name);
	dir = opendir(path);
	if (!dir)
		return;
	while ((e = readdir(dir))) {
		char p[PATH_MAX];
		struct stat st;

		if (strncmp(e->d_name, d->name, strlen(d->name)))
			continue;
		snprintf(p, sizeof(p), "/sys/block/%s/%s/partition", d->name, e->d_name);
		if (stat(p, &st) < 0)
			continue;
		snprintf(p, sizeof(p), "/sys/block/%s/%s/holders", d->name, e->d_name);
		check_holders_dir(d, p, e->d_name);
	}
	closedir(dir);
}

/*
 * The authoritative test: opening a block device with O_EXCL fails with EBUSY
 * when the device or any of its partitions is mounted or claimed by the
 * kernel.  It also reserves the device for us for the duration of the scan.
 */
static int check_exclusive(device_t *d)
{
	int fd = open(d->path, O_RDONLY | O_EXCL | O_CLOEXEC);

	if (fd >= 0) {
		close(fd);
		return 0;
	}
	if (errno == EBUSY) {
		add_why(d, "kernel refuses exclusive open (device is in use)");
	} else if (errno == EACCES || errno == EPERM) {
		/* not a safety problem, just a privilege one */
		d->no_access = 1;
	} else {
		add_why(d, "cannot open %s: %s", d->path, strerror(errno));
	}
	return -1;
}

static void safety_check(device_t *d)
{
	char who[48];

	d->unsafe = 0;
	d->unsafe_why[0] = 0;
	/*
	 * A drive already in a live run is not available, image file or not.
	 * Two runs on one spindle time each other's seeks and both answers
	 * are then worthless -- and if one of them writes, the other is
	 * reading someone else's pattern.  The kernel's O_EXCL catches this
	 * for a scan that has the drive open, but not for a drive sitting
	 * queued behind twenty-nine others in a format run.
	 */
	if (store_busy(d->name, who, sizeof(who)))
		add_why(d, "already in run %s (hddscan --status %s)", who, who);
	if (d->is_file)
		return;
	check_mounts(d);
	check_swap(d);
	check_holders(d);
	check_exclusive(d);
	if (d->readonly)
		add_why(d, "device is read-only at the kernel level");
}

/* ------------------------------------------------------------------ *
 * SMART (optional, via smartctl)
 * ------------------------------------------------------------------ */

typedef struct {
	int have;
	char health[32];
	char serial[80];
	char model[80];
	char firmware[48];
	long long realloc_ct;
	long long realloc_event;
	long long pending;
	long long offline_uncorr;
	long long crc_err;
	long long reported_uncorr;
	long long seek_err;
	long long power_on_hours;
	long long temp_c;
	long long start_stop;
	long long load_cycle;

	/*
	 * SAS/SCSI drives keep an error counter log instead of ATA attributes.
	 * The "corrected with possible delays" and "rereads/rewrites" columns
	 * are the direct measure of the firmware working hard to get data back
	 * off the media - exactly the fault that shows up as a drive which
	 * writes at full speed but reads like treacle.
	 */
	/*
	 * A drive answers with one or the other, never both, and a SAS drive
	 * still prints a health line -- so "we got some SMART output" is not
	 * "we read the reallocated-sector count".  These two say which table
	 * was actually parsed; without them the report printed the ATA
	 * attribute names with a zero beside each and passed that off as a
	 * clean bill of health for a drive that has no such attributes.
	 */
	int ata;
	int sas;
	long long rd_delayed, rd_reread, rd_corrected, rd_algo, rd_uncorr;
	long long wr_delayed, wr_reread, wr_corrected, wr_algo, wr_uncorr;
	double rd_gb, wr_gb;
	long long non_medium;

	/*
	 * The most recent self-test the drive ran on itself.  A SAS drive that
	 * has failed its own background long test has predicted its own
	 * failure, and that is worth more than any latency this tool can
	 * measure -- but only if something asks for the log page.
	 */
	int selftest_failed;
	char selftest[120];

	/* mode page settings that change what the numbers mean */
	int rcd, dra, wce, awre, arre;    /* -1 unknown */
	long long recovery_time_ms;
} smart_t;

static void smart_modes(const device_t *d, smart_t *s);

/*
 * Everything hddscan shells out to is optional, so "is it there" is asked in
 * a dozen places.  One search, four directories, no PATH: a tool found only
 * through a caller's PATH is a tool we cannot promise is the real one.
 */
static int tool_present(const char *tool)
{
	static const char *dirs[] = { "/usr/bin", "/usr/sbin", "/bin", "/sbin" };
	char path[128];
	size_t i;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", dirs[i], tool);
		if (access(path, X_OK) == 0)
			return 1;
	}
	return 0;
}

/*
 * What each tool buys, and what installs it.  Package names differ between
 * the two big families for sg3_utils and nothing else, which is exactly the
 * sort of detail someone on a rescue system at 3am should not have to guess.
 */
typedef struct {
	const char *tool;
	const char *dnf;
	const char *apt;
	const char *what;
} dep_t;

static const dep_t g_deps[] = {
	{ "smartctl", "smartmontools", "smartmontools",
	  "SMART health, temperature, and the drive's own error counters" },
	{ "sdparm", "sdparm", "sdparm",
	  "every drive setting on SAS/SCSI: look-ahead, caches, "
	  "reallocation, recovery time, background scan" },
	{ "sg_modes", "sg3_utils", "sg3-utils",
	  "SCSI mode pages, sector reassignment, and reformatting" },
	{ "hdparm", "hdparm", "hdparm",
	  "look-ahead and write cache on ATA/SATA drives" },
};
#define NDEPS ((int)(sizeof(g_deps) / sizeof(g_deps[0])))

/* comma list of what is missing, or NULL; for the one-line form in the TUI */
static const char *deps_missing_str(char *buf, size_t n)
{
	size_t used = 0;
	int i;

	buf[0] = 0;
	for (i = 0; i < NDEPS; i++) {
		if (tool_present(g_deps[i].tool))
			continue;
		used += (size_t)snprintf(buf + used, used < n ? n - used : 0,
					 "%s%s", used ? ", " : "", g_deps[i].dnf);
		if (used >= n)
			break;
	}
	return buf[0] ? buf : NULL;
}

static void deps_report(int all)
{
	char dnf[256] = "", apt[256] = "";
	size_t dn = 0, an = 0;
	int i, missing = 0;

	for (i = 0; i < NDEPS; i++)
		if (!tool_present(g_deps[i].tool))
			missing++;
	if (all) {
		printf("Optional tools hddscan uses.  None of them are linked; "
		       "each is\nshelled out to when present and done without "
		       "when absent.\n\n");
		for (i = 0; i < NDEPS; i++)
			printf("  %-10s %-8s %s\n", g_deps[i].tool,
			       tool_present(g_deps[i].tool) ? "found" : "MISSING",
			       g_deps[i].what);
	} else if (missing) {
		msg("\n" PROG ": %d optional tool%s missing.  The scan still "
		    "runs; this is what\n         you lose:\n\n", missing,
		    missing == 1 ? " is" : "s are");
		for (i = 0; i < NDEPS; i++)
			if (!tool_present(g_deps[i].tool))
				msg("    %-10s %s\n", g_deps[i].tool,
				    g_deps[i].what);
	}
	if (!missing) {
		if (all)
			printf("\nEverything hddscan can use is installed.\n");
		return;
	}
	for (i = 0; i < NDEPS; i++) {
		if (tool_present(g_deps[i].tool))
			continue;
		dn += (size_t)snprintf(dnf + dn, dn < sizeof(dnf) ? sizeof(dnf) - dn : 0,
				       " %s", g_deps[i].dnf);
		an += (size_t)snprintf(apt + an, an < sizeof(apt) ? sizeof(apt) - an : 0,
				       " %s", g_deps[i].apt);
	}
	if (all)
		printf("\nInstall the missing ones with either of:\n"
		       "    sudo dnf install -y%s\n"
		       "    sudo apt install -y%s\n", dnf, apt);
	else
		msg("\n         sudo dnf install -y%s\n"
		    "         sudo apt install -y%s\n\n", dnf, apt);
}

static int have_smartctl(void)
{
	return tool_present("smartctl");
}

static int smart_read(const device_t *d, smart_t *s)
{
	char cmd[256], line[1024];
	FILE *p;

	memset(s, 0, sizeof(*s));
	s->temp_c = -1;
	s->rcd = s->dra = s->wce = s->awre = s->arre = -1;
	s->recovery_time_ms = -1;
	if (d->is_file || !have_smartctl() || !name_ok(d->name))
		return -1;

	/*
	 * SAS keeps its error counters and its self-test results in log pages
	 * that -A does not print, which is why the SAS half of this report was
	 * empty on every real drive: the parser was correct and simply never
	 * saw the data.  Ask for them explicitly.  Not on ATA, where -A is
	 * already the attribute table and the extra logs only add lines for
	 * the attribute parser to trip over.
	 */
	snprintf(cmd, sizeof(cmd), "smartctl -H -A -i%s /dev/%s 2>/dev/null",
		 d->transport == TR_SCSI ? " -l error -l selftest" : "",
		 d->name);
	p = popen(cmd, "re");
	if (!p)
		return -1;
	while (fgets(line, sizeof(line), p)) {
		int id;
		char name[64], rest[512];
		char *colon;

		if (strstr(line, "overall-health self-assessment test result")) {
			colon = strrchr(line, ':');
			if (colon)
				snprintf(s->health, sizeof(s->health), "%s", trim(colon + 1));
			s->have = 1;
			continue;
		}
		if (strstr(line, "SMART Health Status")) {
			colon = strrchr(line, ':');
			if (colon)
				snprintf(s->health, sizeof(s->health), "%s", trim(colon + 1));
			s->have = 1;
			continue;
		}
		/* "# 1  Background long   Failed in segment -->  3  24188 ..." */
		if (line[0] == '#' && !s->selftest[0]) {
			char *st = line + 1;

			while (*st == ' ' || (*st >= '0' && *st <= '9'))
				st++;
			snprintf(s->selftest, sizeof(s->selftest), "%.100s",
				 trim(st));
			if (strstr(line, "Failed"))
				s->selftest_failed = 1;
			s->have = 1;
			continue;
		}
		if (!strncmp(line, "Non-medium error count:", 23)) {
			s->non_medium = strtoll(line + 23, NULL, 10);
			continue;
		}
		if (!strncmp(line, "read:", 5) || !strncmp(line, "write:", 6)) {
			int is_rd = line[0] == 'r';
			long long f, d2, rr, tc, ai, un;
			double gb;

			if (sscanf(line + (is_rd ? 5 : 6),
				   " %lld %lld %lld %lld %lld %lf %lld",
				   &f, &d2, &rr, &tc, &ai, &gb, &un) == 7) {
				s->sas = 1;
				s->have = 1;
				if (is_rd) {
					s->rd_delayed = d2;
					s->rd_reread = rr;
					s->rd_corrected = tc;
					s->rd_algo = ai;
					s->rd_gb = gb;
					s->rd_uncorr = un;
				} else {
					s->wr_delayed = d2;
					s->wr_reread = rr;
					s->wr_corrected = tc;
					s->wr_algo = ai;
					s->wr_gb = gb;
					s->wr_uncorr = un;
				}
			}
			continue;
		}
		if (!strncmp(line, "Serial Number:", 14)) {
			snprintf(s->serial, sizeof(s->serial), "%s", trim(line + 14));
			continue;
		}
		if (!strncmp(line, "Device Model:", 13)) {
			snprintf(s->model, sizeof(s->model), "%s", trim(line + 13));
			continue;
		}
		if (!strncmp(line, "Firmware Version:", 17)) {
			snprintf(s->firmware, sizeof(s->firmware), "%s", trim(line + 17));
			continue;
		}
		/* attribute rows: "  5 Reallocated_Sector_Ct 0x0033 100 100 036 Pre-fail Always - 0" */
		if (sscanf(line, " %d %63s %511[^\n]", &id, name, rest) == 3) {
			char *tok, *last = NULL, *save = NULL;
			long long raw;

			for (tok = strtok_r(rest, " \t", &save); tok;
			     tok = strtok_r(NULL, " \t", &save))
				last = tok;
			if (!last || !isdigit((unsigned char)last[0]))
				continue;
			raw = strtoll(last, NULL, 10);
			s->have = 1;
			s->ata = 1;
			switch (id) {
			case 5:   s->realloc_ct = raw; break;
			case 7:   s->seek_err = raw; break;
			case 9:   s->power_on_hours = raw; break;
			case 4:   s->start_stop = raw; break;
			case 187: s->reported_uncorr = raw; break;
			case 190:
			case 194: if (s->temp_c < 0) s->temp_c = raw % 256; break;
			case 193: s->load_cycle = raw; break;
			case 196: s->realloc_event = raw; break;
			case 197: s->pending = raw; break;
			case 198: s->offline_uncorr = raw; break;
			case 199: s->crc_err = raw; break;
			default: break;
			}
		}
	}
	pclose(p);
	if (s->sas)
		smart_modes(d, s);
	return s->have ? 0 : -1;
}

static int have_sg(const char *tool)
{
	return tool_present(tool);
}

/*
 * Pull one mode page as raw bytes.  Returns the number of bytes placed in
 * buf, or 0.  We validate the page code we got back rather than trusting the
 * position in the output, so a parsing slip cannot silently produce wrong
 * advice about a drive's configuration.
 */
static int mode_page_bytes(const device_t *d, int page, unsigned char *buf,
			   int max)
{
	char cmd[192], line[512];
	FILE *f;
	int n = 0;

	if (!have_sg("sg_modes") || !name_ok(d->name))
		return 0;
	snprintf(cmd, sizeof(cmd), "sg_modes -p 0x%02x -H /dev/%s 2>/dev/null",
		 page, d->name);
	f = popen(cmd, "re");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f) && n < max) {
		char *p = strchr(line, ' ');
		unsigned v;

		/* hex dump rows look like "  00     01 0a c0 0b ..." */
		if (!strstr(line, "  ") || strchr(line, '>'))
			continue;
		p = line;
		while (*p == ' ')
			p++;
		if (!isxdigit((unsigned char)*p))
			continue;
		while (*p && *p != ' ')       /* skip the offset column */
			p++;
		while (*p && n < max) {
			while (*p == ' ')
				p++;
			if (sscanf(p, "%2x", &v) != 1)
				break;
			buf[n++] = (unsigned char)v;
			while (*p && *p != ' ')
				p++;
		}
	}
	pclose(f);
	if (n < 3 || (buf[0] & 0x3f) != page)
		return 0;              /* not the page we asked for */
	return n;
}

static void smart_modes(const device_t *d, smart_t *s)
{
	unsigned char b[64];
	int n;

	if (d->is_file)
		return;
	n = mode_page_bytes(d, 0x08, b, sizeof(b));    /* caching */
	if (n >= 13) {
		s->rcd = (b[2] & 0x01) ? 1 : 0;
		s->wce = (b[2] & 0x04) ? 1 : 0;
		s->dra = (b[12] & 0x20) ? 1 : 0;
	}
	n = mode_page_bytes(d, 0x01, b, sizeof(b));    /* rw error recovery */
	if (n >= 12) {
		s->awre = (b[2] & 0x80) ? 1 : 0;
		s->arre = (b[2] & 0x40) ? 1 : 0;
		s->recovery_time_ms = ((long long)b[10] << 8) | b[11];
	}
}

static long long smart_temp(const device_t *d)
{
	smart_t s;

	if (smart_read(d, &s) < 0)
		return -1;
	return s.temp_c;
}

/* ------------------------------------------------------------------ *
 * write patterns
 * ------------------------------------------------------------------ */

static inline uint64_t splitmix64(uint64_t *x)
{
	uint64_t z = (*x += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/*
 * The pattern is derived from the byte offset, so a block written to the wrong
 * place on the platter fails verification even though it holds valid-looking
 * data.
 */
static void pattern_fill(void *buf, size_t len, uint64_t off, uint64_t seed)
{
	uint64_t s = seed ^ (off * 0x9E3779B97F4A7C15ull);
	uint8_t *p = buf;
	size_t i;

	for (i = 0; i + 8 <= len; i += 8) {
		uint64_t v = splitmix64(&s);

		memcpy(p + i, &v, 8);
	}
	for (; i < len; i++)
		p[i] = (uint8_t)i;
}

static int pattern_check(const void *buf, size_t len, uint64_t off,
			 uint64_t seed, uint64_t *bad_at)
{
	uint64_t s = seed ^ (off * 0x9E3779B97F4A7C15ull);
	const uint8_t *p = buf;
	size_t i;

	for (i = 0; i + 8 <= len; i += 8) {
		uint64_t v = splitmix64(&s), got;

		memcpy(&got, p + i, 8);
		if (got != v) {
			if (bad_at)
				*bad_at = off + i;
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * crash journal for the non-destructive verify mode
 *
 * Verify mode's one real danger is the window between overwriting a chunk
 * with the test pattern and writing the original back: lose power there and
 * that chunk is gone.  Recording the original bytes and fsyncing them before
 * the pattern write closes the window - a later run replays the record and
 * puts the data back.
 *
 * Only one chunk is ever in flight, so the journal is a single slot.  Replay
 * is idempotent: rewriting the original bytes over an already-restored chunk
 * is a no-op, which is why finishing cleanly needs no second fsync.
 * ------------------------------------------------------------------ */

#define JRN_MAGIC 0x484444534A524E31ull   /* "HDDSJRN1" */

static void *alloc_aligned(size_t len);

typedef struct {
	uint64_t magic;
	uint64_t offset;
	uint64_t len;
	uint64_t devsize;
	uint64_t sum;           /* of the payload, to reject a torn record */
	char device[64];
} jrn_hdr_t;

static uint64_t jrn_sum(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint64_t h = 1469598103934665603ull;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

static int jrn_open(const char *path)
{
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

	if (fd < 0)
		die("cannot open journal %s: %s", path, strerror(errno));
	return fd;
}

/* record the original bytes and make sure they are on stable storage */
static int jrn_write(int fd, const device_t *d, uint64_t off, const void *buf,
		     size_t len)
{
	jrn_hdr_t h;

	if (fd < 0)
		return 0;
	memset(&h, 0, sizeof(h));
	h.magic = JRN_MAGIC;
	h.offset = off;
	h.len = len;
	h.devsize = d->size;
	h.sum = jrn_sum(buf, len);
	snprintf(h.device, sizeof(h.device), "%s", d->name);
	if (pwrite(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h))
		return -1;
	if (pwrite(fd, buf, len, (off_t)sizeof(h)) != (ssize_t)len)
		return -1;
	return fdatasync(fd);
}

static void jrn_clear(int fd)
{
	jrn_hdr_t h;

	if (fd < 0)
		return;
	memset(&h, 0, sizeof(h));
	if (pwrite(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h))
		return;
	fdatasync(fd);
}

/* put back anything a previous run was interrupted in the middle of */
static int jrn_replay(const char *path, const device_t *d, int devfd)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	jrn_hdr_t h;
	void *buf;
	int rc = 0;

	if (fd < 0)
		return 0;
	if (read(fd, &h, sizeof(h)) != (ssize_t)sizeof(h) || h.magic != JRN_MAGIC) {
		close(fd);
		return 0;
	}
	if (strcmp(h.device, d->name) || h.devsize != d->size) {
		msg(PROG ": journal %s belongs to %s, not %s - ignoring\n",
		    path, h.device, d->name);
		close(fd);
		return 0;
	}
	if (!h.len || h.len > (1u << 24) || h.offset + h.len > d->size) {
		close(fd);
		return 0;
	}
	buf = alloc_aligned((size_t)h.len);
	if (pread(fd, buf, (size_t)h.len, (off_t)sizeof(h)) != (ssize_t)h.len ||
	    jrn_sum(buf, (size_t)h.len) != h.sum) {
		msg(PROG ": journal record is incomplete; the interrupted write "
		    "never got far enough to matter\n");
		free(buf);
		close(fd);
		return 0;
	}
	msg(PROG ": %s: replaying journal, restoring %" PRIu64 " bytes at "
	    "offset %" PRIu64 "\n", d->name, h.len, h.offset);
	if (pwrite(devfd, buf, (size_t)h.len, (off_t)h.offset) != (ssize_t)h.len) {
		msg(PROG ": journal replay failed: %s\n", strerror(errno));
		rc = -1;
	}
	free(buf);
	close(fd);
	return rc;
}

/* ------------------------------------------------------------------ *
 * scan context
 * ------------------------------------------------------------------ */

typedef enum {
	MODE_READ = 0,
	MODE_VERIFY,     /* read original, write pattern, verify, restore */
	MODE_WRITE,      /* destructive: write pattern, verify */
	MODE_CHECK       /* read-only: re-verify a pattern written by an earlier run */
} scan_mode_t;

static const char *mode_name(scan_mode_t m)
{
	switch (m) {
	case MODE_READ: return "read-only";
	case MODE_VERIFY: return "non-destructive read/write verify";
	case MODE_WRITE: return "destructive write + verify";
	case MODE_CHECK: return "read-only re-check of a previously written pattern";
	}
	return "?";
}

static int mode_writes(scan_mode_t m)
{
	return m == MODE_VERIFY || m == MODE_WRITE;
}

typedef enum {
	ST_OK = 0,
	ST_RECOVERED,    /* slow once, clean on every retry */
	ST_SLOW,         /* consistently over budget */
	ST_UNSTABLE,     /* intermittent slow or intermittent error */
	ST_BAD,          /* unreadable on every attempt */
	ST_CORRUPT       /* data came back but did not match what we wrote */
} status_t;

static const char *status_name(status_t s)
{
	switch (s) {
	case ST_OK: return "ok";
	case ST_RECOVERED: return "recovered";
	case ST_SLOW: return "slow";
	case ST_UNSTABLE: return "unstable";
	case ST_BAD: return "bad";
	case ST_CORRUPT: return "corrupt";
	}
	return "?";
}

typedef struct {
	uint64_t offset;
	uint32_t len;
	status_t status;
	uint32_t attempts;
	uint32_t errors;
	uint64_t first_us;
	uint64_t min_us;
	uint64_t med_us;
	uint64_t max_us;
	int errnum;
	int rewritten;
	int fixed_by_rewrite;
} finding_t;

typedef struct {
	uint64_t chunks;
	uint64_t slow_chunks;
	uint64_t bad_blocks;
	uint64_t slow_blocks;
	uint64_t sum_us;
	uint64_t max_us;
	uint64_t bytes;
} band_t;

typedef struct {
	uint64_t us;
	uint64_t off;
} topslow_t;

/*
 * Scan order.  Sequential is the only practical choice for a multi-terabyte
 * drive, but it has a blind spot: the drive prefetches ahead of us, so a
 * merely marginal sector can be read during idle time and served to us out of
 * the drive's own buffer at full speed.  Random order defeats that completely
 * - every read is a fresh seek to an unpredictable place - at the cost of
 * being roughly an order of magnitude slower.
 */
typedef enum {
	ORD_SEQ = 0,
	ORD_REVERSE,
	ORD_RANDOM
} order_t;

static const char *order_name(order_t o)
{
	switch (o) {
	case ORD_REVERSE: return "reverse";
	case ORD_RANDOM: return "random";
	default: return "sequential";
	}
}

static uint64_t gcd_u64(uint64_t a, uint64_t b)
{
	while (b) {
		uint64_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

typedef struct {
	device_t *dev;
	int fd;

	/* configuration */
	scan_mode_t mode;
	uint64_t start, end;
	size_t chunk, block;
	int retries;
	uint64_t retry_cap_us;
	uint64_t chunk_thr_us, block_thr_us;
	/*
	 * The gradient the budget follows across the platter.  Empty when
	 * calibration could not run, or when the user pinned a threshold by
	 * hand -- an explicit --chunk-slow-ms means exactly that number
	 * everywhere, not a number this tool then bends.
	 */
	uint64_t grad_off[NANCHOR];
	uint64_t grad_us[NANCHOR];
	int grad_n;
	uint64_t grad_mid;      /* median of the anchors' sequential runs */
	uint64_t grad_typ;      /* what a chunk costs in the order being scanned */
	int grad_budget;        /* the budget follows it (nothing pinned by hand) */
	double auto_factor;
	uint64_t floor_us;
	int auto_thr;
	int calib_failed;       /* the drive told us nothing; the budget is a guess */
	uint64_t sample;
	order_t order;
	uint64_t nchunks;
	uint64_t step;
	uint64_t seg_chunks;    /* chunks per shuffled segment */
	uint64_t cur_seg;       /* memoised permutation for the segment in hand */
	uint64_t cur_len;
	uint64_t cur_stride;
	int rewrite_weak;
	int force_remap;
	int second_pass;
	uint64_t seed;
	uint64_t max_time;
	uint64_t max_errors;
	long long max_temp;
	int no_direct;
	int map_width;
	const char *state_path;
	int resume;

	/* running state */
	uint64_t pos;
	uint64_t bytes_done;
	uint64_t bytes_at_start; /* bytes_done when this session began: a resumed
				  * scan brings its old total with it */
	uint64_t t_start_us;
	uint64_t t_end_us;
	uint64_t chunks_read;
	uint64_t chunks_slow;
	/*
	 * A write lands on the same track as the read before it and waits for
	 * the same revolution, so it costs what a read there does: 8.79 ms
	 * against 8.44 on a healthy SAS drive even with its write cache on.
	 * One over the read budget is a write the drive struggled to put down,
	 * which a read-back that verifies cannot see.
	 */
	uint64_t writes_done;
	uint64_t writes_slow;
	uint64_t chunks_err;
	uint64_t blocks_drilled;
	uint64_t prot_errors;   /* reads refused by the drive's guard tag check */
	uint64_t prot_bytes;    /* how much of the device that hid from us */
	uint64_t prot_fixed;    /* chunks a write pass gave a valid guard tag */
	uint64_t prot_fixed_bytes;
	uint64_t prot_run;      /* consecutive refusals, to spot a whole-device one */
	uint64_t blocks_slow;
	uint64_t blocks_bad;
	uint64_t blocks_recovered;
	uint64_t blocks_unstable;
	uint64_t blocks_corrupt;
	uint64_t blocks_fixed;          /* read fast again after we rewrote them */
	long long realloc_delta;        /* sectors the firmware moved, per SMART */
	uint64_t sas_reassigned;
	int spares_exhausted;
	uint64_t hard_errors;
	uint64_t align_errors;
	uint64_t retry_ios;
	uint64_t short_reads;

	lat_t chunk_lat;
	lat_t block_lat;      /* per-4K reads, drill-down + retries */
	lat_t write_lat;

	band_t bands[N_BANDS];
	topslow_t top[TOP_SLOW];

	finding_t *find;
	int nfind, findcap;
	int find_overflow;

	smart_t smart_before, smart_after;
	long long temp_min, temp_max;
	uint64_t last_temp_check;
	/*
	 * Drive-managed SMR reports itself as conventional, so the only tell is
	 * behavioural: writes run at cache speed until the persistent cache
	 * fills, then collapse while the firmware rewrites bands.  Comparing
	 * early write cost against late catches it.
	 */
	uint64_t wr_early_sum, wr_early_n;
	uint64_t wr_late_sum, wr_late_n;
	int smr_suspected;
	int smr_windows;        /* consecutive collapsed write windows */
	int stall_warned;       /* said once that retries are eating the run */

	/* the throughput floor: see rate_judge() */
	double rate_floor;      /* bytes/s; 0 when the rule does not apply */
	double rate_seen;       /* what the scan managed, once it has settled */
	int too_slow;
	int too_slow_warned;

	/*
	 * A rolling window of recent chunk latencies.  The cumulative figures
	 * in the report describe the whole scan; while it is running what you
	 * want to know is what the drive is doing *now* -- a head that has
	 * just wandered into a bad patch shows up here long before it moves
	 * the lifetime average.  Bounded by both age and capacity: on a fast
	 * device the window is whatever the last LATWIN samples cover, and
	 * the display says which.
	 */
	latwin_t latwin;
	latwin_t wlatwin;       /* the same for writes, which cost what a read
				 * at the same place does -- see writes_slow */
	int temp_abort;
	const char *profile;    /* the named profile these defaults came from */
	int la_disabled;        /* drive look-ahead was actually turned off */
	int bms_state;          /* background medium scan: -1 unknown, 0 off, 1 on */
	int wc_state;           /* write cache: -1 unknown, 0 off, 1 on */
	int awre_state, arre_state;   /* -1 unknown, 0 off, 1 on */
	long long rtl_state;    /* recovery time limit in ms, -1 unknown */
	long long rrc_state;    /* read retry count, -1 unknown */
	long long rcd_state;    /* read cache disable bit, -1 unknown */
	int pm_bg;              /* power-management precedence field, -1 unknown */
	int sleep_timers;       /* idle/standby condition timers switched on */

	/* restore-on-abort bookkeeping for MODE_VERIFY */
	int jrn_fd;
	void *restore_buf;
	uint64_t restore_off;
	size_t restore_len;
	int restore_pending;

	char abort_reason[256];
} ctx_t;

static ctx_t *g_ctx;    /* for the signal handler / atexit cleanup */

/* ------------------------------------------------------------------ *
 * kernel per-device timeout handling
 * ------------------------------------------------------------------ */

static char g_timeout_path[PATH_MAX];
static char g_timeout_orig[32];
static int g_timeout_changed;
static int g_timeout_registered;

static void timeout_restore(void)
{
	if (g_timeout_changed) {
		sysfs_write(g_timeout_path, g_timeout_orig);
		g_timeout_changed = 0;
	}
}

static void timeout_set(const device_t *d, int secs)
{
	char val[32];

	if (d->is_file || secs <= 0)
		return;
	snprintf(g_timeout_path, sizeof(g_timeout_path),
		 "/sys/block/%s/device/timeout", d->name);
	if (read_file_line(g_timeout_path, g_timeout_orig, sizeof(g_timeout_orig)) < 0)
		return;
	snprintf(val, sizeof(val), "%d", secs);
	if (sysfs_write(g_timeout_path, val) == 0) {
		g_timeout_changed = 1;
		if (!g_timeout_registered) {
			atexit(timeout_restore);
			g_timeout_registered = 1;
		}
		msg(PROG ": %s: kernel I/O timeout %ss -> %ds for this run\n",
		    d->name, g_timeout_orig, secs);
	}
}

/* ------------------------------------------------------------------ *
 * drive read look-ahead
 *
 * This is the drive's own firmware prefetching into its onboard DRAM, a layer
 * below anything the kernel controls.  O_DIRECT and the per-device
 * read_ahead_kb knob in sysfs both act on the page cache; neither touches
 * this.  With it off, a sequential scan can no longer be handed a marginal
 * sector out of the drive's buffer at DRAM speed.
 *
 * There is no one way to ask for it.  On ATA it is a SET FEATURES command,
 * which is hdparm's job; on SAS/SCSI it is the DRA bit of the caching mode
 * page, which is sdparm's.  Both are shelled out to when present, so a
 * missing tool costs the guarantee rather than the scan -- see invariant 9.
 * ------------------------------------------------------------------ */

static int have_sdparm(void)
{
	return tool_present("sdparm");
}

/*
 * Saving a setting to the drive is the one thing here that outlives the
 * process, so it goes through a single gate rather than being open-coded at
 * each knob.  Only SCSI mode pages can genuinely be saved: hdparm has nothing
 * equivalent for ATA, and pretending otherwise would leave someone believing a
 * setting stuck when it dies at the next power cycle.  Refusing loudly and
 * falling back to run-scoped is the honest answer.
 */
static int persist_ok(const device_t *d, int save, const char *what)
{
	if (!save)
		return 0;
	if (d->transport != TR_SCSI) {
		msg(PROG ": %s: the %s cannot be saved on a %s drive -- that "
		    "needs a SCSI mode page. Applying it for this run only.\n",
		    d->name, what, transport_name(d->transport));
		return 0;
	}
	if (!have_sdparm()) {
		msg(PROG ": %s: sdparm is needed to save the %s to the drive. "
		    "Applying it for this run only.\n", d->name, what);
		return 0;
	}
	return 1;
}

static char g_la_dev[64];
static int g_la_orig = -1;	/* as the drive had it: 1 on, 0 already off */
static int g_la_changed;
static int g_la_registered;
static int g_la_scsi;		/* backend that owns g_la_dev/g_la_orig */

static int have_hdparm(void)
{
	return tool_present("hdparm");
}

static int lookahead_get(const char *name)
{
	char cmd[160], line[256];
	FILE *f;
	int val = -1;

	if (!have_hdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd), "hdparm -A /dev/%s 2>/dev/null", name);
	f = popen(cmd, "re");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "look-ahead");
		char *eq;

		if (p && (eq = strchr(p, '=')))
			val = atoi(trim(eq + 1));
	}
	pclose(f);
	return val;
}

static int lookahead_put(const char *name, int on)
{
	char cmd[160];
	int rc;

	if (!have_hdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd), "hdparm -A%d /dev/%s >/dev/null 2>&1",
		 on ? 1 : 0, name);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

/*
 * The SCSI knob is inverted -- DRA=1 *disables* read-ahead -- so these two
 * translate to the hdparm convention above, where 1 means look-ahead is on.
 * The change deliberately goes to the current values only, never --save:
 * invariant 7 says a scan puts back everything it touched, and a saved mode
 * page would outlive the process that promised to undo it.
 */
static int sd_lookahead_get(const char *name)
{
	char cmd[160], line[256];
	FILE *f;
	int val = -1;

	if (!have_sdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd), "sdparm --get=DRA /dev/%s 2>/dev/null",
		 name);
	f = popen(cmd, "re");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "DRA");

		if (!p)
			continue;
		p = trim(p + 3);
		if (*p == ':' || *p == '=')
			p = trim(p + 1);
		if (*p >= '0' && *p <= '9')
			val = (*p == '0');
	}
	pclose(f);
	return val;
}

static int sd_lookahead_put(const char *name, int on, int save)
{
	char cmd[192];

	if (!have_sdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "sdparm --set=DRA=%d %s /dev/%s >/dev/null 2>&1", on ? 0 : 1,
		 save ? "--save" : "", name);
	return system(cmd) == 0 ? 0 : -1;
}

static int la_get(const char *name, int scsi)
{
	return scsi ? sd_lookahead_get(name) : lookahead_get(name);
}

static int la_put(const char *name, int on, int scsi, int save)
{
	return scsi ? sd_lookahead_put(name, on, save)
		    : lookahead_put(name, on);
}

static void lookahead_restore(void)
{
	if (g_la_changed && g_la_orig >= 0) {
		la_put(g_la_dev, g_la_orig, g_la_scsi, 0);
		g_la_changed = 0;
	}
}

static int lookahead_disable(const device_t *d, int save)
{
	const char *tool, *undo;
	int scsi, now;

	if (d->is_file)
		return -1;
	if (d->transport == TR_ATA) {
		scsi = 0;
		tool = "hdparm";
		undo = "hdparm -A1";
	} else if (d->transport == TR_SCSI) {
		scsi = 1;
		tool = "sdparm";
		undo = "sdparm --clear=DRA";
	} else {
		msg(PROG ": %s: no way to reach the read look-ahead of a %s "
		    "drive, leaving it alone\n", d->name,
		    transport_name(d->transport));
		return -1;
	}
	if (scsi ? !have_sdparm() : !have_hdparm()) {
		msg(PROG ": %s: %s not installed, leaving drive look-ahead "
		    "alone\n", d->name, tool);
		return -1;
	}
	g_la_orig = la_get(d->name, scsi);
	if (g_la_orig < 0) {
		msg(PROG ": %s: could not read the drive's look-ahead setting\n",
		    d->name);
		return -1;
	}
	if (g_la_orig == 0) {
		msg(PROG ": %s: drive look-ahead is already off\n", d->name);
		return 0;
	}
	save = persist_ok(d, save, "read look-ahead");
	if (la_put(d->name, 0, scsi, save) < 0) {
		msg(PROG ": %s: failed to disable drive look-ahead "
		    "(need root?)\n", d->name);
		return -1;
	}
	now = la_get(d->name, scsi);
	if (now != 0) {
		msg(PROG ": %s: drive ignored the request to disable "
		    "look-ahead\n", d->name);
		return -1;
	}
	if (save) {
		msg(PROG ": %s: drive read look-ahead disabled and %ssaved to "
		    "the drive%s; it stays off until you run '%s %s'\n",
		    d->name, c_yel(), c_off(), undo, d->path);
		return 0;
	}
	snprintf(g_la_dev, sizeof(g_la_dev), "%s", d->name);
	g_la_scsi = scsi;
	g_la_changed = 1;
	if (!g_la_registered) {
		atexit(lookahead_restore);
		g_la_registered = 1;
	}
	msg(PROG ": %s: drive read look-ahead disabled for this run "
	    "(restored on exit; if this process is killed outright, restore it "
	    "with '%s %s')\n", d->name, undo, d->path);
	return 0;
}

/* ------------------------------------------------------------------ *
 * mode page fields
 *
 * The remaining knobs are all the same shape: one named field of one SCSI
 * mode page, read and written with sdparm.  They differ only in the name and
 * in what the number means, so they share these helpers rather than growing a
 * near-identical pair each.
 *
 * They also share one restore list.  Invariant 7 wants an atexit handler
 * registered exactly once -- slots are a small fixed resource and a sequential
 * scan of a shelf of drives would burn through them -- so a single handler
 * walks a table rather than each field bringing its own.
 * ------------------------------------------------------------------ */

#define MAX_RESTORE 16

enum { RS_SDPARM, RS_HDPARM_WC };

static struct {
	char dev[64];
	char field[16];
	long long orig;
	int kind;
} g_restore[MAX_RESTORE];
static int g_nrestore;
static int g_restore_registered;

static long long sd_field_get(const char *name, const char *field)
{
	char cmd[192], line[256];
	FILE *f;
	long long val = -1;
	size_t flen = strlen(field);

	if (!have_sdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd), "sdparm --get=%s /dev/%s 2>/dev/null",
		 field, name);
	f = popen(cmd, "re");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, field);

		if (!p)
			continue;
		p = trim(p + flen);
		if (*p == ':' || *p == '=')
			p = trim(p + 1);
		if (*p >= '0' && *p <= '9')
			val = strtoll(p, NULL, 10);
	}
	pclose(f);
	return val;
}

static int sd_field_put(const char *name, const char *field, long long val,
			int save)
{
	char cmd[224];

	if (!have_sdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "sdparm --set=%s=%lld %s /dev/%s >/dev/null 2>&1", field, val,
		 save ? "--save" : "", name);
	return system(cmd) == 0 ? 0 : -1;
}

static int hd_wc_put(const char *name, int on);

static void field_restore_all(void)
{
	int i;

	for (i = 0; i < g_nrestore; i++) {
		if (g_restore[i].kind == RS_HDPARM_WC)
			hd_wc_put(g_restore[i].dev, (int)g_restore[i].orig);
		else
			sd_field_put(g_restore[i].dev, g_restore[i].field,
				     g_restore[i].orig, 0);
	}
	g_nrestore = 0;
}

static int field_restore_kind(const char *dev, const char *field,
			      long long orig, int kind)
{
	if (g_nrestore >= MAX_RESTORE)
		return -1;
	snprintf(g_restore[g_nrestore].dev, sizeof(g_restore[0].dev), "%s", dev);
	snprintf(g_restore[g_nrestore].field, sizeof(g_restore[0].field), "%s",
		 field);
	g_restore[g_nrestore].orig = orig;
	g_restore[g_nrestore].kind = kind;
	g_nrestore++;
	if (!g_restore_registered) {
		atexit(field_restore_all);
		g_restore_registered = 1;
	}
	return 0;
}

static int field_restore_add(const char *dev, const char *field, long long orig)
{
	return field_restore_kind(dev, field, orig, RS_SDPARM);
}

/* returns the value the field holds afterwards, or -1 if nothing was done */
static long long sd_field_apply(const device_t *d, const char *field,
				long long want, int save, const char *what)
{
	long long now;

	if (d->is_file)
		return -1;
	if (d->transport != TR_SCSI) {
		msg(PROG ": %s: %s is a SCSI mode page setting and this is a "
		    "%s drive; leaving it alone\n", d->name, what,
		    transport_name(d->transport));
		return -1;
	}
	if (!have_sdparm()) {
		msg(PROG ": %s: sdparm not installed, cannot set %s\n",
		    d->name, what);
		return -1;
	}
	now = sd_field_get(d->name, field);
	if (now < 0) {
		msg(PROG ": %s: could not read %s from the drive\n", d->name,
		    what);
		return -1;
	}
	if (now == want) {
		msg(PROG ": %s: %s is already %lld\n", d->name, what, want);
		return now;
	}
	save = persist_ok(d, save, what);
	if (sd_field_put(d->name, field, want, save) < 0) {
		msg(PROG ": %s: failed to set %s (need root?)\n", d->name,
		    what);
		return -1;
	}
	if (sd_field_get(d->name, field) != want) {
		msg(PROG ": %s: the drive ignored the request to set %s\n",
		    d->name, what);
		return -1;
	}
	if (save) {
		msg(PROG ": %s: %s %lld -> %lld, %ssaved to the drive%s; undo "
		    "with 'sdparm --set=%s=%lld --save %s'\n", d->name, what,
		    now, want, c_yel(), c_off(), field, now, d->path);
		return want;
	}
	if (field_restore_add(d->name, field, now) < 0)
		msg(PROG ": %s: %sno room left to remember the old %s%s; it "
		    "will not be put back\n", d->name, c_yel(), what, c_off());
	else
		msg(PROG ": %s: %s %lld -> %lld for this run (restored on "
		    "exit)\n", d->name, what, now, want);
	return want;
}

/* ------------------------------------------------------------------ *
 * write cache
 *
 * With the write cache on, a write returns as soon as the drive has the data
 * in DRAM, so timing one measures the cache rather than the platter -- the
 * same argument as the read look-ahead above, in the other direction.  Turning
 * it off makes a write mode honest at the cost of a great deal of speed.
 *
 * It is also the other half of the classic ex-array fault: RCD set with WCE
 * set gives a drive that writes at full speed and reads like treacle on
 * completely healthy media.  Being able to flip it and re-measure is what
 * tells those apart.
 *
 * Unlike the background scan this is run-scoped: changed on the way in,
 * restored on the way out, on SIGINT and on error, per invariant 7.  Nothing
 * here is saved to the drive.
 * ------------------------------------------------------------------ */

static int hd_wc_get(const char *name)
{
	char cmd[160], line[256];
	FILE *f;
	int val = -1;

	if (!have_hdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd), "hdparm -W /dev/%s 2>/dev/null", name);
	f = popen(cmd, "re");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "write-caching");
		char *eq;

		if (p && (eq = strchr(p, '=')))
			val = atoi(trim(eq + 1)) ? 1 : 0;
	}
	pclose(f);
	return val;
}

static int hd_wc_put(const char *name, int on)
{
	char cmd[160];

	if (!have_hdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd), "hdparm -W%d /dev/%s >/dev/null 2>&1",
		 on ? 1 : 0, name);
	return system(cmd) == 0 ? 0 : -1;
}

static int sd_wc_get(const char *name)
{
	char cmd[160], line[256];
	FILE *f;
	int val = -1;

	if (!have_sdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd), "sdparm --get=WCE /dev/%s 2>/dev/null",
		 name);
	f = popen(cmd, "re");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "WCE");

		if (!p)
			continue;
		p = trim(p + 3);
		if (*p == ':' || *p == '=')
			p = trim(p + 1);
		if (*p >= '0' && *p <= '9')
			val = (*p != '0');
	}
	pclose(f);
	return val;
}

static int sd_wc_put(const char *name, int on, int save)
{
	char cmd[192];

	if (!have_sdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "sdparm --set=WCE=%d %s /dev/%s >/dev/null 2>&1", on ? 1 : 0,
		 save ? "--save" : "", name);
	return system(cmd) == 0 ? 0 : -1;
}

static int wc_get(const char *name, int scsi)
{
	return scsi ? sd_wc_get(name) : hd_wc_get(name);
}

static int wc_put(const char *name, int on, int scsi, int save)
{
	return scsi ? sd_wc_put(name, on, save) : hd_wc_put(name, on);
}

/* returns the state the drive is in afterwards, or -1 if unknown */
static int wc_set(const device_t *d, int on, int save)
{
	char undo[64];
	const char *tool;
	int scsi, now;
	long long orig;

	if (d->is_file)
		return -1;
	if (d->transport == TR_ATA) {
		scsi = 0;
		tool = "hdparm";
	} else if (d->transport == TR_SCSI) {
		scsi = 1;
		tool = "sdparm";
	} else {
		msg(PROG ": %s: no way to reach the write cache of a %s drive, "
		    "leaving it alone\n", d->name,
		    transport_name(d->transport));
		return -1;
	}
	if (scsi ? !have_sdparm() : !have_hdparm()) {
		msg(PROG ": %s: %s not installed, leaving the write cache "
		    "alone\n", d->name, tool);
		return -1;
	}
	orig = wc_get(d->name, scsi);
	if (orig < 0) {
		msg(PROG ": %s: could not read the drive's write cache "
		    "setting\n", d->name);
		return -1;
	}
	if (orig == on) {
		msg(PROG ": %s: write cache is already %s\n", d->name,
		    on ? "on" : "off");
		return on;
	}
	save = persist_ok(d, save, "write cache setting");
	if (wc_put(d->name, on, scsi, save) < 0) {
		msg(PROG ": %s: failed to turn the write cache %s (need "
		    "root?)\n", d->name, on ? "on" : "off");
		return -1;
	}
	now = wc_get(d->name, scsi);
	if (now != on) {
		msg(PROG ": %s: the drive ignored the request to turn its "
		    "write cache %s\n", d->name, on ? "on" : "off");
		return -1;
	}
	if (scsi)
		snprintf(undo, sizeof(undo), "sdparm --set=WCE=%lld", orig);
	else
		snprintf(undo, sizeof(undo), "hdparm -W%lld", orig);
	if (save) {
		msg(PROG ": %s: write cache turned %s and %ssaved to the "
		    "drive%s; it stays that way until you run '%s %s'\n",
		    d->name, on ? "on" : "off", c_yel(), c_off(), undo,
		    d->path);
		return on;
	}
	if (field_restore_kind(d->name, "WCE", orig,
			       scsi ? RS_SDPARM : RS_HDPARM_WC) < 0)
		msg(PROG ": %s: %sno room left to remember the old write cache "
		    "setting%s; it will not be put back\n", d->name, c_yel(),
		    c_off());
	else
		msg(PROG ": %s: write cache turned %s for this run (restored on "
		    "exit; if this process is killed outright, restore it with "
		    "'%s %s')\n", d->name, on ? "on" : "off", undo, d->path);
	return on;
}

/*
 * Power condition timers.  A drive that parks its heads or spins down partway
 * through a scan turns a latency measurement into a measurement of how long a
 * drive takes to wake up, and burns load-unload cycles doing it.  Worse, it
 * can quietly defeat --bms: a drive asleep is a drive not scanning its own
 * media.  This is read and reported, never changed -- someone's power policy
 * is not a scanner's business to rewrite.
 *
 * The whole page comes back in one call rather than one popen per field; six
 * more sdparm invocations per drive would be noticeable on a shelf of them.
 */
static void power_probe(const device_t *d, int *pm_bg, int *timers)
{
	static const char *tf[] = { "STANDBY_Y", "STANDBY_Z", "IDLE_A",
				    "IDLE_B", "IDLE_C" };
	char cmd[160], line[256];
	FILE *f;
	size_t i;

	*pm_bg = -1;
	*timers = 0;
	if (d->is_file || d->transport != TR_SCSI || !have_sdparm() ||
	    !name_ok(d->name))
		return;
	snprintf(cmd, sizeof(cmd), "sdparm --page=po /dev/%s 2>/dev/null",
		 d->name);
	f = popen(cmd, "re");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char name[32];
		long long v;

		if (sscanf(line, " %31s %lld", name, &v) != 2)
			continue;
		if (!strcmp(name, "PM_BG"))
			*pm_bg = (int)v;
		for (i = 0; i < sizeof(tf) / sizeof(tf[0]); i++)
			if (!strcmp(name, tf[i]) && v)
				(*timers)++;
	}
	pclose(f);
}

/* ------------------------------------------------------------------ *
 * background medium scan
 *
 * A SCSI drive can sweep its own media whenever it is idle, log what it finds
 * and reallocate the sectors it can still recover.  It is off on most drives
 * as shipped.  Turning it on is worth doing once per drive and is deliberately
 * NOT part of a scan: it is a saved mode page, so unlike every other knob here
 * it outlives the process, which is why invariant 7 does not cover it and why
 * it takes an explicit --bms on.  Enabling it mid-scan would also be
 * self-defeating -- the drive would start seeking on its own behalf while we
 * are timing it, which is the queueing measurement invariant 5 exists to
 * prevent.  So it is applied once, before the first read, and announced.
 * ------------------------------------------------------------------ */

static int bms_get(const char *name)
{
	char cmd[160], line[256];
	FILE *f;
	int val = -1;

	if (!have_sdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd), "sdparm --get=EN_BMS /dev/%s 2>/dev/null",
		 name);
	f = popen(cmd, "re");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "EN_BMS");

		if (!p)
			continue;
		p = trim(p + 6);
		if (*p == ':' || *p == '=')
			p = trim(p + 1);
		if (*p >= '0' && *p <= '9')
			val = (*p != '0');
	}
	pclose(f);
	return val;
}

static int bms_put(const char *name, int on, int save)
{
	char cmd[192];

	if (!have_sdparm() || !name_ok(name))
		return -1;
	snprintf(cmd, sizeof(cmd),
		 "sdparm --set=EN_BMS=%d %s /dev/%s >/dev/null 2>&1",
		 on ? 1 : 0, save ? "--save" : "", name);
	return system(cmd) == 0 ? 0 : -1;
}

/* returns 1 if the drive is running a background scan when we are done */
static int bms_enable(const device_t *d)
{
	int now;

	if (d->is_file)
		return -1;
	if (d->transport != TR_SCSI) {
		msg(PROG ": %s: background medium scan is a SCSI feature and "
		    "this is a %s drive; leaving it alone\n", d->name,
		    transport_name(d->transport));
		return -1;
	}
	if (!have_sdparm()) {
		msg(PROG ": %s: sdparm not installed, cannot enable the "
		    "background medium scan\n", d->name);
		return -1;
	}
	now = bms_get(d->name);
	if (now == 1) {
		msg(PROG ": %s: background medium scan is already enabled\n",
		    d->name);
		return 1;
	}
	if (bms_put(d->name, 1, 1) < 0) {
		/*
		 * Not every drive lets a mode page be saved.  A current-values
		 * change still works, but it dies at the next power cycle and
		 * a background scan needs days of idle time, so say plainly
		 * that it will not survive rather than implying it will.
		 */
		if (bms_put(d->name, 1, 0) < 0) {
			msg(PROG ": %s: could not enable the background medium "
			    "scan (need root?)\n", d->name);
			return -1;
		}
		msg(PROG ": %s: %sthe drive would not save the setting%s, so "
		    "the background scan stops at the next power cycle\n",
		    d->name, c_yel(), c_off());
	}
	now = bms_get(d->name);
	if (now != 1) {
		msg(PROG ": %s: the drive ignored the request to enable its "
		    "background medium scan\n", d->name);
		return -1;
	}
	msg(PROG ": %s: background medium scan enabled.  This is a saved "
	    "setting and is %snot%s undone when the scan finishes; turn it "
	    "back off with 'sdparm --clear=EN_BMS --save %s'\n",
	    d->name, c_yel(), c_off(), d->path);
	return 1;
}

/* ------------------------------------------------------------------ *
 * I/O helpers
 * ------------------------------------------------------------------ */

static void *alloc_aligned(size_t len)
{
	void *p = NULL;

	if (posix_memalign(&p, 4096, len))
		die("out of memory allocating %zu bytes", len);
	memset(p, 0, len);
	return p;
}

/* one timed pread; returns bytes read or -1, latency always reported */
static ssize_t timed_pread(int fd, void *buf, size_t len, uint64_t off,
			   uint64_t *us, int *err)
{
	uint64_t t0 = now_us();
	ssize_t r = pread(fd, buf, len, (off_t)off);

	*us = now_us() - t0;
	*err = (r < 0) ? errno : 0;
	return r;
}

static ssize_t timed_pwrite(int fd, const void *buf, size_t len, uint64_t off,
			    uint64_t *us, int *err)
{
	uint64_t t0 = now_us();
	ssize_t r = pwrite(fd, buf, len, (off_t)off);

	*us = now_us() - t0;
	*err = (r < 0) ? errno : 0;
	return r;
}

/*
 * Force the next read of `off` to come off the platter rather than out of the
 * drive's own DRAM cache: touch a block half a device away first.
 */
static void cache_bust(ctx_t *c, void *scratch, uint64_t off)
{
	uint64_t far = off + c->dev->size / 2;
	uint64_t us;
	int err;

	far %= c->dev->size ? c->dev->size : 1;
	far -= far % (uint64_t)c->dev->logical_bs;
	if (far + c->block > c->dev->size)
		far = 0;
	timed_pread(c->fd, scratch, c->block, far, &us, &err);
}

/* ------------------------------------------------------------------ *
 * bookkeeping
 * ------------------------------------------------------------------ */

static int band_of(const ctx_t *c, uint64_t off)
{
	uint64_t span = c->dev->size ? c->dev->size : 1;
	uint64_t b = (off / (span / N_BANDS + 1));

	if (b >= N_BANDS)
		b = N_BANDS - 1;
	return (int)b;
}

static void top_add(ctx_t *c, uint64_t us, uint64_t off)
{
	int i, worst = 0;

	for (i = 1; i < TOP_SLOW; i++)
		if (c->top[i].us < c->top[worst].us)
			worst = i;
	if (us > c->top[worst].us) {
		c->top[worst].us = us;
		c->top[worst].off = off;
	}
}

static finding_t *find_add(ctx_t *c)
{
	if (c->nfind >= MAX_FINDINGS) {
		c->find_overflow = 1;
		return NULL;
	}
	if (c->nfind == c->findcap) {
		int cap = c->findcap ? c->findcap * 2 : 256;
		finding_t *v = realloc(c->find, (size_t)cap * sizeof(*v));

		if (!v)
			return NULL;
		c->find = v;
		c->findcap = cap;
	}
	memset(&c->find[c->nfind], 0, sizeof(finding_t));
	return &c->find[c->nfind++];
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : (x > y);
}

/* ------------------------------------------------------------------ *
 * per-block deep analysis
 * ------------------------------------------------------------------ */

static uint64_t chunk_thr_at(const ctx_t *c, uint64_t off);
static uint64_t block_thr_at(const ctx_t *c, uint64_t off);

static void analyze_block(ctx_t *c, uint64_t off, size_t len, void *buf,
			  void *scratch, uint64_t first_us, int first_err)
{
	uint64_t *lat = calloc((size_t)c->retries + 1, sizeof(uint64_t));
	uint64_t t_begin = now_us();
	uint64_t sorted_med = 0;
	int errors = (first_err != 0);
	int attempts = 1;
	int i, n_lat = 0;
	int last_err = first_err;
	status_t st;
	finding_t *f;
	int readable_copy = 0;
	void *keep = NULL;

	if (!lat)
		die("out of memory");
	if (!first_err) {
		lat[n_lat++] = first_us;
		keep = alloc_aligned(len);
		memcpy(keep, buf, len);
		readable_copy = 1;
	}

	for (i = 0; i < c->retries && !g_stop_hard; i++) {
		uint64_t us;
		int err;
		ssize_t r;

		/*
		 * A sector that is truly gone can take the full SCSI timeout
		 * per attempt.  Thirty of those is half an hour on one sector,
		 * and a dying drive has thousands.  Give up early: by then we
		 * already know the answer.
		 */
		if (c->retry_cap_us && now_us() - t_begin > c->retry_cap_us)
			break;

		cache_bust(c, scratch, off);
		r = timed_pread(c->fd, buf, len, off, &us, &err);
		attempts++;
		c->retry_ios++;
		if (r < 0 || (size_t)r != len) {
			errors++;
			last_err = err ? err : EIO;
		} else {
			lat[n_lat++] = us;
			lat_add(&c->block_lat, us);
			if (!readable_copy) {
				keep = alloc_aligned(len);
				memcpy(keep, buf, len);
				readable_copy = 1;
			}
		}
	}

	if (n_lat) {
		qsort(lat, (size_t)n_lat, sizeof(uint64_t), cmp_u64);
		sorted_med = lat[n_lat / 2];
	}

	if (errors == attempts)
		st = ST_BAD;
	else if (errors > 0)
		st = ST_UNSTABLE;
	else if (n_lat && lat[0] > block_thr_at(c, off))
		st = ST_SLOW;
	else if (n_lat && lat[n_lat - 1] > block_thr_at(c, off))
		st = ST_UNSTABLE;
	else
		st = ST_RECOVERED;

	f = find_add(c);
	if (f) {
		f->offset = off;
		f->len = (uint32_t)len;
		f->status = st;
		f->attempts = (uint32_t)attempts;
		f->errors = (uint32_t)errors;
		f->first_us = first_us;
		f->min_us = n_lat ? lat[0] : 0;
		f->med_us = sorted_med;
		f->max_us = n_lat ? lat[n_lat - 1] : 0;
		f->errnum = last_err;
	}

	/*
	 * Rewriting a weak sector is what usually makes a drive "heal": the
	 * write either refreshes the magnetic domains or trips a reallocation
	 * to a spare sector.  Only ever rewrite what we could read back, so
	 * the data survives - unless the caller explicitly asked to sacrifice
	 * unreadable sectors to force a remap.
	 */
	if (mode_writes(c->mode) || c->rewrite_weak || c->force_remap) {
		int do_rewrite = 0;
		const void *src = NULL;

		if (readable_copy && c->rewrite_weak &&
		    (st == ST_SLOW || st == ST_UNSTABLE || st == ST_RECOVERED)) {
			do_rewrite = 1;
			src = keep;
		} else if (!readable_copy && c->force_remap && st == ST_BAD) {
			/*
			 * SAS has an explicit REASSIGN BLOCKS command, which is
			 * more direct than hoping a write triggers relocation.
			 * ATA has no equivalent, so there we fall back to
			 * overwriting and letting the firmware decide.
			 */
			if (c->dev->transport == TR_SCSI && have_sg("sg_reassign") &&
			    name_ok(c->dev->name)) {
				char cmd[192];
				int rc;

				snprintf(cmd, sizeof(cmd),
					 "sg_reassign --address=%" PRIu64
					 " /dev/%s >/dev/null 2>&1",
					 off / (uint64_t)c->dev->logical_bs,
					 c->dev->name);
				rc = system(cmd);
				if (f)
					f->rewritten = 1;
				if (rc == 0)
					c->sas_reassigned++;
			}
			memset(buf, 0, len);
			do_rewrite = 1;
			src = buf;
		}
		if (do_rewrite && c->fd >= 0) {
			uint64_t us;
			int err;
			ssize_t w = timed_pwrite(c->fd, src, len, off, &us, &err);

			if (f)
				f->rewritten = 1;
			if (w == (ssize_t)len) {
				int ok = 1, k;

				for (k = 0; k < 3; k++) {
					uint64_t rus;
					ssize_t r;

					cache_bust(c, scratch, off);
					r = timed_pread(c->fd, buf, len, off, &rus, &err);
					if (r != (ssize_t)len ||
					    rus > block_thr_at(c, off))
						ok = 0;
				}
				if (ok) {
					if (f)
						f->fixed_by_rewrite = 1;
					c->blocks_fixed++;
				}
			}
		}
	}

	free(keep);
	free(lat);

	switch (st) {
	case ST_BAD: c->blocks_bad++; break;
	case ST_UNSTABLE: c->blocks_unstable++; break;
	case ST_SLOW: c->blocks_slow++; break;
	case ST_RECOVERED: c->blocks_recovered++; break;
	default: break;
	}
	if (st == ST_BAD || st == ST_UNSTABLE)
		c->bands[band_of(c, off)].bad_blocks++;
	else
		c->bands[band_of(c, off)].slow_blocks++;
}

/*
 * The chunk budget at a given offset.  Linear interpolation between the
 * calibration anchors, scaled the same way the flat budget was: whatever
 * chunk_thr_us is relative to the overall median, each point keeps that same
 * relationship to its local expected cost.  So an explicit --auto-factor, the
 * floor, and the invariant-1 clamp all carry over unchanged; only the baseline
 * moves with the platter.
 */
static uint64_t expected_at(const ctx_t *c, uint64_t off)
{
	uint64_t lo_off, hi_off, lo_us, hi_us, exp;
	int i;

	if (c->grad_n < 2 || !c->grad_mid || !c->grad_typ)
		return 0;
	if (off <= c->grad_off[0])
		exp = c->grad_us[0];
	else if (off >= c->grad_off[c->grad_n - 1])
		exp = c->grad_us[c->grad_n - 1];
	else {
		for (i = 1; i < c->grad_n; i++)
			if (off < c->grad_off[i])
				break;
		lo_off = c->grad_off[i - 1];
		hi_off = c->grad_off[i];
		lo_us = c->grad_us[i - 1];
		hi_us = c->grad_us[i];
		if (hi_off <= lo_off)
			exp = lo_us;
		else
			exp = lo_us + (hi_us > lo_us ? (hi_us - lo_us) : 0) *
				      (off - lo_off) / (hi_off - lo_off) -
			      (hi_us < lo_us ? (lo_us - hi_us) *
					       (off - lo_off) / (hi_off - lo_off)
					     : 0);
	}
	/*
	 * The anchors are sequential runs.  A random scan pays a seek on top
	 * of each, so scale to the order being scanned: this point costs what
	 * the typical chunk does, times how this point compares to the
	 * sequential median.  Dividing by the random median instead shrank
	 * every random-order budget to about an eighth, leaving only the floor.
	 */
	return (uint64_t)((double)exp * (double)c->grad_typ / (double)c->grad_mid);
}

static uint64_t thr_at(const ctx_t *c, uint64_t off, uint64_t flat)
{
	uint64_t exp = c->grad_budget ? expected_at(c, off) : 0;

	if (!exp)
		return flat;
	/* scale the flat budget by how this point compares to the typical chunk */
	return (uint64_t)((double)flat * (double)exp / (double)c->grad_typ);
}

static uint64_t chunk_thr_at(const ctx_t *c, uint64_t off)
{
	uint64_t v = thr_at(c, off, c->chunk_thr_us);
	uint64_t b = thr_at(c, off, c->block_thr_us);

	if (v < c->floor_us)
		v = c->floor_us;
	if (b < c->floor_us)
		b = c->floor_us;
	/* invariant 1, pointwise: a chunk budget looser than the sector budget
	 * would let a slow sector hide inside a chunk nobody drills into */
	return v > b ? b : v;
}

static uint64_t block_thr_at(const ctx_t *c, uint64_t off)
{
	uint64_t v = thr_at(c, off, c->block_thr_us);

	return v < c->floor_us ? c->floor_us : v;
}

/* walk a suspicious chunk one block at a time */
static void drill_chunk(ctx_t *c, uint64_t off, size_t len, void *buf,
			void *scratch)
{
	uint64_t o;

	for (o = off; o < off + len && !g_stop_hard; o += c->block) {
		size_t bl = (size_t)((off + len - o) < c->block ? (off + len - o) : c->block);
		uint64_t us;
		int err;
		ssize_t r;

		r = timed_pread(c->fd, buf, bl, o, &us, &err);
		/*
		 * EINVAL is the kernel refusing a misaligned direct read, not
		 * the drive refusing to give us the data.  Counting it as a
		 * bad sector would condemn a perfectly healthy disk.
		 */
		if (r < 0 && err == EINVAL) {
			c->align_errors++;
			continue;
		}
		/*
		 * EILSEQ is the block layer's BLK_STS_PROTECTION: the drive
		 * read the block and its own T10 guard tag did not match.
		 * That is a statement about the format, not the media -- see
		 * probe_protection() -- so it can no more be a bad sector
		 * than EINVAL can.
		 */
		if (r < 0 && err == EILSEQ) {
			c->prot_errors++;
			c->prot_bytes += bl;
			continue;
		}
		c->blocks_drilled++;
		if (r == (ssize_t)bl) {
			lat_add(&c->block_lat, us);
			top_add(c, us, o);
			if (us <= block_thr_at(c, o))
				continue;
		} else if (r >= 0) {
			c->short_reads++;
			err = EIO;
		}
		if (r < 0 || (size_t)r != bl)
			c->hard_errors++;
		analyze_block(c, o, bl, buf, scratch, us, (r == (ssize_t)bl) ? 0 : (err ? err : EIO));
	}
}

/* ------------------------------------------------------------------ *
 * progress
 * ------------------------------------------------------------------ */

/*
 * When we are one of many workers the parent draws the display, so instead of
 * painting a progress line we send it a status record.  One write() per
 * update, comfortably under PIPE_BUF, so records never interleave.
 */

static void latwin_add(latwin_t *w, uint64_t us)
{
	w->s[w->head].at_us = now_us();
	w->s[w->head].us = us > 0xffffffffu ? 0xffffffffu : (uint32_t)us;
	w->head = (w->head + 1) % LATWIN;
	if (w->n < LATWIN)
		w->n++;
}

/*
 * Median, mean, min and max of the window, plus how many seconds it actually
 * spans -- which is not always LATWIN_SECS, and saying so is cheaper than
 * letting someone believe a two second sample is a thirty second one.
 */
static int latwin_stats(const latwin_t *w, double *med, double *avg,
			double *lo, double *hi, double *span)
{
	static uint32_t tmp[LATWIN];
	uint64_t now = now_us(), cut, oldest = 0;
	uint64_t sum = 0;
	int i, n = 0;

	cut = now > (uint64_t)LATWIN_SECS * 1000000ull
	      ? now - (uint64_t)LATWIN_SECS * 1000000ull : 0;
	for (i = 0; i < w->n; i++) {
		int k = (w->head - 1 - i + LATWIN * 2) % LATWIN;

		if (w->s[k].at_us < cut)
			break;
		tmp[n++] = w->s[k].us;
		oldest = w->s[k].at_us;
	}
	if (n < 2)
		return 0;
	qsort(tmp, (size_t)n, sizeof(tmp[0]), cmp_u32);
	for (i = 0; i < n; i++)
		sum += tmp[i];
	*med = (double)tmp[n / 2] / 1000.0;
	*avg = (double)sum / (double)n / 1000.0;
	*lo = (double)tmp[0] / 1000.0;
	*hi = (double)tmp[n - 1] / 1000.0;
	*span = (double)(now - oldest) / 1e6;
	return n;
}

/*
 * A drive can return every sector inside its budget and still be no use.
 * One that covers its surface at a few hundred KiB a second is spending the
 * time somewhere -- writes that crawl, or a chunk over budget almost every
 * time, each drilled into sector by sector -- and a scan of it will not
 * finish this year.  A working hard drive at 4 MiB chunks manages well over
 * 10 MiB/s even writing and reading every chunk back, so below that is not
 * a slow drive, it is a failing one.
 *
 * The floor scales down with the chunk and never up: a small chunk pays the
 * same lost revolution per request for less data, so 128 KiB honestly runs
 * far slower and a flat floor would condemn healthy drives at the default.
 * Verify mode writes the original back on top of what write mode does, so
 * it gets a quarter less.  Only a hard drive on a local bus is judged --
 * a slow network link or an SD card is not a failing platter -- and image
 * files, so the rule can be tested at all.
 */
static double rate_floor_of(const ctx_t *c, double min_mib)
{
	double f;

	if (min_mib <= 0 || !(c->dev->is_file || c->dev->rotational) ||
	    !transport_is_local(c->dev->transport))
		return 0;
	f = min_mib * 1048576.0;
	if (c->chunk < RATE_REF_CHUNK)
		f *= (double)c->chunk / (double)RATE_REF_CHUNK;
	if (c->mode == MODE_VERIFY)
		f *= 0.75;
	return f;
}

/*
 * Judge the rate over the whole of this session, once it has run long enough
 * to mean something.  Not sticky while scanning: the last call, with the end
 * time, is the one the verdict uses.
 */
static void rate_judge(ctx_t *c, uint64_t now)
{
	char b1[32], b2[32];
	double el;

	if (c->rate_floor <= 0 || now <= c->t_start_us)
		return;
	el = (double)(now - c->t_start_us) / 1e6;
	if (el < RATE_SETTLE_S)
		return;
	c->rate_seen = (double)(c->bytes_done - c->bytes_at_start) / el;
	c->too_slow = c->rate_seen < c->rate_floor;
	if (c->too_slow && !c->too_slow_warned) {
		c->too_slow_warned = 1;
		msg(PROG ": %s: %stesting at %s/s, below the %s/s floor for "
		    "%zu KiB chunks%s; unless it picks up it will be reported "
		    "%s\n", c->dev->name, c_red(),
		    human_size((uint64_t)c->rate_seen, b1, sizeof(b1)),
		    human_size((uint64_t)c->rate_floor, b2, sizeof(b2)),
		    c->chunk / 1024, c_off(),
		    c->dev->transport == TR_USB ? "SUSPECT" : "FAILING");
	}
}

static void progress(ctx_t *c, int final)
{
	static uint64_t last;
	uint64_t now = now_us();
	double el, rate, eta, pct;
	char b1[32], b2[32], b3[32], lw[96] = "";
	int tty = isatty(STDERR_FILENO);

	/*
	 * A dashboard reading job records needs them fresh whether or not
	 * this worker has a terminal -- it usually has none at all, having
	 * been detached from the one that started it -- so the record cadence
	 * follows the reader, not stderr.
	 */
	if (!final && now - last < (uint64_t)(g_job_own ? 1000000 :
					      tty ? 500000 : 30000000))
		return;
	last = now;
	el = (double)(now - c->t_start_us) / 1e6;
	rate = el > 0 ? (double)(c->bytes_done - c->bytes_at_start) / el : 0;
	rate_judge(c, now);
	{
		uint64_t left = c->nchunks > c->step ? c->nchunks - c->step : 0;

		if (c->sample > 1)
			left /= c->sample;
		eta = rate > 0 ? (double)left * (double)c->chunk / rate : -1;
	}
	pct = c->nchunks ? 100.0 * (double)c->step / (double)c->nchunks : 100.0;

	/*
	 * A scan that will not finish this decade is not a scan.  It happens
	 * when a drive has enough marginal sectors that drill-down and retries
	 * consume the entire run: the surface is barely being read at all and
	 * almost every second goes into re-reading the same handful of blocks.
	 *
	 * Say so once, early, with the levers that fix it.  Left to itself the
	 * only symptom is a percentage that does not move, and by the time
	 * anyone works out why, days are gone.
	 */
	if (!c->stall_warned && el > 900 && eta > 30.0 * 24 * 3600 &&
	    c->retry_ios > c->chunks_read) {
		c->stall_warned = 1;
		msg(PROG ": %s: %sat this rate this scan needs %s%s, and %"
		    PRIu64 " retry reads\n"
		    "         against %" PRIu64 " chunks says the time is going "
		    "into re-reading marginal\n"
		    "         sectors rather than covering the surface. Stop it "
		    "and re-run with\n"
		    "         '--retries 3 --recovery-time 300 --read-cache off' "
		    "to make the drive\n"
		    "         give up quickly; sectors it cannot deliver in "
		    "300 ms will then be\n"
		    "         reported as unreadable, which is the honest "
		    "answer for them.\n",
		    c->dev->name, c_yel(), human_time(eta, b3, sizeof(b3)),
		    c_off(), c->retry_ios, c->chunks_read);
	}

	/*
	 * Hand the numbers to whoever is watching.  This is the only channel
	 * out of a worker: it has no terminal of its own, and the process
	 * that started the run may well have exited hours ago.
	 */
	if (g_job_own) {
		double med = 0, avg = 0, lo = 0, hi = 0, span = 0;
		double wmed = 0, wavg, wlo, whi, wspan;

		latwin_stats(&c->latwin, &med, &avg, &lo, &hi, &span);
		if (!latwin_stats(&c->wlatwin, &wmed, &wavg, &wlo, &whi, &wspan))
			wmed = 0;
		g_job.wlat_med = wmed;
		g_job.pct = pct;
		g_job.rate = rate;
		g_job.eta = eta;
		g_job.bad = c->blocks_bad + c->blocks_corrupt;
		g_job.weak = c->blocks_recovered;
		g_job.slow = c->blocks_slow + c->blocks_unstable;
		g_job.too_slow = c->too_slow;
		g_job.bytes = c->bytes_done;
		g_job.lat_med = med;
		g_job.lat_avg = avg;
		g_job.lat_min = lo;
		g_job.lat_max = hi;
		job_flush(JS_RUNNING);
	}

	if (g_quiet)
		return;
	{
		double med, avg, lo, hi, span;
		double wmed, wavg, wlo, whi, wspan;

		if (latwin_stats(&c->latwin, &med, &avg, &lo, &hi, &span))
			snprintf(lw, sizeof(lw),
				 "  last %.0fs med %.1f avg %.1f min %.1f "
				 "max %.1f ms", span, med, avg, lo, hi);
		if (lw[0] && latwin_stats(&c->wlatwin, &wmed, &wavg, &wlo,
					  &whi, &wspan))
			snprintf(lw + strlen(lw) - 3, sizeof(lw) - (strlen(lw) - 3),
				 ", write med %.1f ms", wmed);
	}
	fprintf(stderr, "\r%s  %5.1f%%  %s/s%s  bad:%" PRIu64 " slow:%" PRIu64
		" weak:%" PRIu64 "%s  elapsed %s  eta %s   ",
		c->dev->name, pct,
		human_size((uint64_t)rate, b1, sizeof(b1)),
		c->too_slow ? " TOO SLOW" : "",
		c->blocks_bad, c->blocks_slow + c->blocks_unstable,
		c->blocks_recovered, lw,
		human_time(el, b2, sizeof(b2)),
		human_time(eta, b3, sizeof(b3)));
	if (final)
		fputc('\n', stderr);
	fflush(stderr);
}

/* ------------------------------------------------------------------ *
 * checkpoint / resume
 * ------------------------------------------------------------------ */

static void state_save(ctx_t *c)
{
	char tmp[PATH_MAX];
	FILE *f;
	int i;

	if (!c->state_path)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", c->state_path);
	f = fopen(tmp, "we");
	if (!f)
		return;
	fprintf(f, "hddscan-state 1\n");
	fprintf(f, "device %s\n", c->dev->name);
	fprintf(f, "size %" PRIu64 "\n", c->dev->size);
	fprintf(f, "mode %d\n", (int)c->mode);
	fprintf(f, "chunk %zu\nblock %zu\n", c->chunk, c->block);
	fprintf(f, "pos %" PRIu64 "\nstep %" PRIu64 "\norder %d\n",
		c->pos, c->step, (int)c->order);
	fprintf(f, "start %" PRIu64 "\nend %" PRIu64 "\n", c->start, c->end);
	fprintf(f, "bytes %" PRIu64 "\n", c->bytes_done);
	fprintf(f, "counters %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
		" %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
		" %" PRIu64 " %" PRIu64 "\n",
		c->chunks_read, c->chunks_slow, c->chunks_err, c->blocks_drilled,
		c->blocks_slow, c->blocks_bad, c->blocks_recovered,
		c->blocks_unstable, c->blocks_fixed, c->hard_errors, c->retry_ios);
	fprintf(f, "writes %" PRIu64 " %" PRIu64 "\n", c->writes_done,
		c->writes_slow);
	/* the surface map, so a resumed scan still draws what came before */
	for (i = 0; i < N_BANDS; i++) {
		const band_t *b = &c->bands[i];

		if (b->chunks)
			fprintf(f, "band %d %" PRIu64 " %" PRIu64 " %" PRIu64
				" %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
				"\n", i, b->chunks, b->slow_chunks, b->bad_blocks,
				b->slow_blocks, b->sum_us, b->max_us, b->bytes);
	}
	fprintf(f, "chunklat %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
		c->chunk_lat.count, c->chunk_lat.sum_us,
		c->chunk_lat.min_us, c->chunk_lat.max_us);
	for (i = 0; i < NBUCKETS; i++)
		fprintf(f, "%s%" PRIu64, i ? " " : "chunkhist ", c->chunk_lat.hist[i]);
	fputc('\n', f);
	fprintf(f, "blocklat %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
		c->block_lat.count, c->block_lat.sum_us,
		c->block_lat.min_us, c->block_lat.max_us);
	for (i = 0; i < NBUCKETS; i++)
		fprintf(f, "%s%" PRIu64, i ? " " : "blockhist ", c->block_lat.hist[i]);
	fputc('\n', f);
	for (i = 0; i < c->nfind; i++) {
		finding_t *x = &c->find[i];

		fprintf(f, "finding %" PRIu64 " %u %d %u %u %" PRIu64 " %" PRIu64
			" %" PRIu64 " %" PRIu64 " %d %d %d\n",
			x->offset, x->len, (int)x->status, x->attempts, x->errors,
			x->first_us, x->min_us, x->med_us, x->max_us, x->errnum,
			x->rewritten, x->fixed_by_rewrite);
	}
	fclose(f);
	rename(tmp, c->state_path);
}

static int state_load(ctx_t *c)
{
	FILE *f = fopen(c->state_path, "re");
	char line[1024];
	char name[64] = "";
	int i;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "device ", 7)) {
			sscanf(line + 7, "%63s", name);
		} else if (!strncmp(line, "pos ", 4)) {
			c->pos = strtoull(line + 4, NULL, 10);
		} else if (!strncmp(line, "step ", 5)) {
			c->step = strtoull(line + 5, NULL, 10);
		} else if (!strncmp(line, "bytes ", 6)) {
			c->bytes_done = strtoull(line + 6, NULL, 10);
		} else if (!strncmp(line, "counters ", 9)) {
			sscanf(line + 9, "%" SCNu64 " %" SCNu64 " %" SCNu64
			       " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
			       " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
			       &c->chunks_read, &c->chunks_slow, &c->chunks_err,
			       &c->blocks_drilled, &c->blocks_slow, &c->blocks_bad,
			       &c->blocks_recovered, &c->blocks_unstable,
			       &c->blocks_fixed, &c->hard_errors, &c->retry_ios);
		} else if (!strncmp(line, "band ", 5)) {
			band_t b;
			int bi;

			memset(&b, 0, sizeof(b));
			if (sscanf(line + 5, "%d %" SCNu64 " %" SCNu64 " %" SCNu64
				   " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
				   &bi, &b.chunks, &b.slow_chunks, &b.bad_blocks,
				   &b.slow_blocks, &b.sum_us, &b.max_us,
				   &b.bytes) == 8 && bi >= 0 && bi < N_BANDS)
				c->bands[bi] = b;
		} else if (!strncmp(line, "writes ", 7)) {
			sscanf(line + 7, "%" SCNu64 " %" SCNu64,
			       &c->writes_done, &c->writes_slow);
		} else if (!strncmp(line, "chunklat ", 9)) {
			sscanf(line + 9, "%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
			       &c->chunk_lat.count, &c->chunk_lat.sum_us,
			       &c->chunk_lat.min_us, &c->chunk_lat.max_us);
		} else if (!strncmp(line, "blocklat ", 9)) {
			sscanf(line + 9, "%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
			       &c->block_lat.count, &c->block_lat.sum_us,
			       &c->block_lat.min_us, &c->block_lat.max_us);
		} else if (!strncmp(line, "chunkhist ", 10) ||
			   !strncmp(line, "blockhist ", 10)) {
			lat_t *l = line[0] == 'c' ? &c->chunk_lat : &c->block_lat;
			char *save = NULL, *tok = strtok_r(line + 10, " \t\n", &save);

			for (i = 0; i < NBUCKETS && tok; i++) {
				l->hist[i] = strtoull(tok, NULL, 10);
				tok = strtok_r(NULL, " \t\n", &save);
			}
		} else if (!strncmp(line, "finding ", 8)) {
			finding_t *x = find_add(c);
			int st;

			if (!x)
				continue;
			sscanf(line + 8, "%" SCNu64 " %u %d %u %u %" SCNu64
			       " %" SCNu64 " %" SCNu64 " %" SCNu64 " %d %d %d",
			       &x->offset, &x->len, &st, &x->attempts, &x->errors,
			       &x->first_us, &x->min_us, &x->med_us, &x->max_us,
			       &x->errnum, &x->rewritten, &x->fixed_by_rewrite);
			x->status = (status_t)st;
		}
	}
	fclose(f);
	if (name[0] && strcmp(name, c->dev->name)) {
		msg(PROG ": state file belongs to %s, not %s - ignoring\n",
		    name, c->dev->name);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * calibration
 * ------------------------------------------------------------------ */

static uint64_t coprime_stride(uint64_t n);

typedef struct {
	/*
	 * What a chunk costs at each calibration anchor, and where that anchor
	 * was.  A platter is not uniform: it spins at a fixed rate, so an
	 * outer track sweeps further per revolution and holds more sectors,
	 * and reads there are roughly twice as fast as at the inner edge.  A
	 * single budget for the whole device is therefore loose on the outside
	 * and tight on the inside.
	 *
	 * These points let the budget follow that gradient.  It is deliberately
	 * interpolated between eight fixed anchors rather than learned from the
	 * chunks as they are scanned: the gradient is physics and is smooth,
	 * whereas a budget that adapted to what it was reading would quietly
	 * raise itself over a bad region and normalise away the very thing the
	 * scan exists to find.
	 */
	uint64_t anchor_off[NANCHOR];
	uint64_t anchor_us[NANCHOR];
	int nanchor;

	uint64_t typical_us;    /* what a chunk costs in the pattern we will use */
	uint64_t seq_us;        /* what a chunk costs read purely sequentially */
	uint64_t big_us;        /* what a much larger request costs */
	size_t big_len;         /* how much larger; 0 if it was not measured */
	int valid;
} calib_t;

/*
 * Calibrate against the access pattern the scan will actually use.  Timing
 * scattered reads and then scanning sequentially (or the reverse) produces a
 * budget that is wrong by the cost of a seek, which on a real drive is an
 * order of magnitude - far too loose to catch a sector that needs 30 ms of
 * internal recovery.
 */
static void calibrate(ctx_t *c, void *buf, calib_t *cal)
{
	uint64_t samples[160];
	int n = 0, a, i;
	uint64_t span = c->end - c->start;
	uint64_t anchors[NANCHOR];

	memset(cal, 0, sizeof(*cal));
	if (span < (uint64_t)c->chunk * 64)
		return;

	for (a = 0; a < NANCHOR; a++)
		anchors[a] = c->start + (span / (NANCHOR + 1)) * (uint64_t)(a + 1);

	/* sequential cost: a run at each anchor, discarding the read that paid
	 * for the seek to get there */
	for (a = 0; a < NANCHOR && !g_stop; a++) {
		uint64_t one[12];
		int m = 0;

		for (i = 0; i < 12; i++) {
			uint64_t off = anchors[a] + (uint64_t)i * (uint64_t)c->chunk;
			uint64_t us;
			int err;
			ssize_t r;

			off -= off % (uint64_t)c->dev->logical_bs;
			if (off + (uint64_t)c->chunk > c->end)
				break;
			r = timed_pread(c->fd, buf, c->chunk, off, &us, &err);
			if (r == (ssize_t)c->chunk && i > 0) {
				if (n < (int)(sizeof(samples) / sizeof(samples[0])))
					samples[n++] = us;
				one[m++] = us;
			}
		}
		if (m >= 4) {
			qsort(one, (size_t)m, sizeof(one[0]), cmp_u64);
			cal->anchor_off[cal->nanchor] = anchors[a];
			cal->anchor_us[cal->nanchor] = one[m / 2];
			cal->nanchor++;
		}
	}
	if (n < 4)
		return;
	qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);
	cal->seq_us = samples[n / 2];
	cal->typical_us = cal->seq_us;
	cal->valid = 1;

	/*
	 * How much of a chunk's time is data moving, and how much is the
	 * platter coming back round?  With look-ahead off and one request
	 * outstanding, a sequential read finishes just after the start of the
	 * next chunk has passed under the head, so it waits a whole revolution
	 * for it -- roughly 8 ms on a 7200 rpm drive, against well under a
	 * millisecond of actual transfer for a small chunk.
	 *
	 * Time a few reads eight times the size to find out.  If eight times
	 * the data costs far less than eight times the time, the scan is
	 * paying for revolutions rather than for reading, and a larger --chunk
	 * buys most of it back.  Measured rather than worked out from the
	 * rotation rate, because the answer depends on the drive, the HBA and
	 * the request size together.
	 */
	if (span >= (uint64_t)c->chunk * 128) {
		size_t big = (size_t)c->chunk * 8;
		uint64_t bs[6];
		void *bb = NULL;
		int bn = 0;

		if (posix_memalign(&bb, 4096, big) == 0) {
			for (i = 0; i < 6 && !g_stop; i++) {
				uint64_t off = anchors[1] +
					       (uint64_t)i * (uint64_t)big;
				uint64_t us;
				int err;
				ssize_t r;

				off -= off % (uint64_t)c->dev->logical_bs;
				if (off + big > c->end)
					break;
				r = timed_pread(c->fd, bb, big, off, &us, &err);
				if (r == (ssize_t)big && i > 0)
					bs[bn++] = us;
			}
			free(bb);
		}
		if (bn >= 2) {
			qsort(bs, (size_t)bn, sizeof(uint64_t), cmp_u64);
			cal->big_us = bs[bn / 2];
			cal->big_len = big;
		}
	}

	if (c->order != ORD_RANDOM)
		return;

	/* random cost: scattered reads inside one segment, as the scan will do */
	n = 0;
	for (a = 0; a < 8 && !g_stop; a++) {
		uint64_t stride = coprime_stride(c->seg_chunks);

		for (i = 0; i < 12; i++) {
			uint64_t idx = ((uint64_t)i * stride + (uint64_t)a) %
				       (c->seg_chunks ? c->seg_chunks : 1);
			uint64_t off = anchors[a] + idx * (uint64_t)c->chunk;
			uint64_t us;
			int err;
			ssize_t r;

			off -= off % (uint64_t)c->dev->logical_bs;
			if (off + (uint64_t)c->chunk > c->end)
				continue;
			r = timed_pread(c->fd, buf, c->chunk, off, &us, &err);
			if (r == (ssize_t)c->chunk &&
			    n < (int)(sizeof(samples) / sizeof(samples[0])))
				samples[n++] = us;
		}
	}
	if (n >= 4) {
		qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);
		cal->typical_us = samples[n / 2];
	}
}

/*
 * A full-cycle permutation of [0,n): step by roughly the golden ratio of n,
 * nudged up until it is coprime with n so the walk lands on every value
 * exactly once.
 */
static uint64_t coprime_stride(uint64_t n)
{
	uint64_t st;

	if (n < 3)
		return 1;
	st = (uint64_t)((double)n * 0.6180339887);
	if (st < 2)
		st = 2;
	while (gcd_u64(st, n) != 1)
		st++;
	return st;
}

/*
 * Which chunk to visit on a given step.
 *
 * Random order shuffles *within a segment* and walks the segments in order,
 * rather than shuffling the whole device.  Both defeat the drive's sequential
 * detector equally well - that is what stops it prefetching a marginal sector
 * into its buffer and serving it to us at DRAM speed - but a segment-local
 * shuffle keeps every seek short instead of paying a full-stroke seek on each
 * chunk.  The segment still has to be far larger than the drive's cache, or
 * the drive would simply hold the whole segment and we would be timing its
 * DRAM again.
 */
static uint64_t order_index(ctx_t *c, uint64_t step)
{
	uint64_t seg, within, base, len;

	if (!c->nchunks)
		return 0;
	switch (c->order) {
	case ORD_REVERSE:
		return c->nchunks - 1 - step;
	case ORD_RANDOM:
		seg = step / c->seg_chunks;
		within = step - seg * c->seg_chunks;
		base = seg * c->seg_chunks;
		len = c->nchunks - base;
		if (len > c->seg_chunks)
			len = c->seg_chunks;
		if (seg != c->cur_seg || !c->cur_len) {
			c->cur_seg = seg;
			c->cur_len = len;
			c->cur_stride = coprime_stride(len);
		}
		return base + (within * c->cur_stride + seg) % len;
	default:
		return step;
	}
}

static void order_setup(ctx_t *c, uint64_t segment_bytes)
{
	c->nchunks = (c->end - c->start + (uint64_t)c->chunk - 1) / (uint64_t)c->chunk;
	c->cur_seg = UINT64_MAX;
	c->cur_len = 0;

	if (!segment_bytes)          /* 0 means shuffle the whole device */
		c->seg_chunks = c->nchunks;
	else
		c->seg_chunks = segment_bytes / (uint64_t)c->chunk;
	if (c->seg_chunks < 1)
		c->seg_chunks = 1;
	if (c->seg_chunks > c->nchunks)
		c->seg_chunks = c->nchunks;
}

static void check_temperature(ctx_t *c)
{
	long long t;

	if (c->dev->is_file || !have_smartctl() || c->max_temp <= 0)
		return;
	if (now_us() - c->last_temp_check < 60000000ull)
		return;
	c->last_temp_check = now_us();
	t = smart_temp(c->dev);
	if (t < 0)
		return;
	if (c->temp_min < 0 || t < c->temp_min)
		c->temp_min = t;
	if (t > c->temp_max)
		c->temp_max = t;
	if (t >= c->max_temp) {
		snprintf(c->abort_reason, sizeof(c->abort_reason),
			 "drive temperature %lld C reached the limit of %lld C",
			 t, c->max_temp);
		c->temp_abort = 1;
		g_stop = 1;
	}
}

static void scan_loop(ctx_t *c, int verify_only_pass)
{
	void *buf = alloc_aligned(c->chunk);
	void *orig = (c->mode == MODE_VERIFY) ? alloc_aligned(c->chunk) : NULL;
	void *pat = mode_writes(c->mode) ? alloc_aligned(c->chunk) : NULL;
	void *scratch = alloc_aligned(c->block);
	uint64_t last_state = now_us();

	c->restore_buf = orig;

	for (; c->step < c->nchunks && !g_stop; c->step++) {
		size_t len;
		uint64_t us = 0;
		int err = 0;
		ssize_t r;
		int drill = 0, prot_write = 0;
		int band;

		if (c->sample > 1 && (c->step % c->sample))
			continue;

		c->pos = c->start + order_index(c, c->step) * (uint64_t)c->chunk;
		if (c->pos >= c->end)
			continue;
		len = (size_t)((c->end - c->pos) < (uint64_t)c->chunk ?
			       (c->end - c->pos) : (uint64_t)c->chunk);
		band = band_of(c, c->pos);

		if (c->max_time && now_us() - c->t_start_us > c->max_time * 1000000ull) {
			snprintf(c->abort_reason, sizeof(c->abort_reason),
				 "time limit of %" PRIu64 "s reached", c->max_time);
			break;
		}

		/* ---- read the chunk ---- */
		r = timed_pread(c->fd, buf, len, c->pos, &us, &err);
		c->chunks_read++;
		c->bands[band].chunks++;
		c->bands[band].bytes += len;
		if (r == (ssize_t)len) {
			c->prot_run = 0;
			lat_add(&c->chunk_lat, us);
			latwin_add(&c->latwin, us);
			c->bands[band].sum_us += us;
			if (us > c->bands[band].max_us)
				c->bands[band].max_us = us;
			if (us > chunk_thr_at(c, c->pos)) {
				c->chunks_slow++;
				c->bands[band].slow_chunks++;
				drill = 1;
			}
		} else if (r < 0 && err == EINVAL) {
			c->align_errors++;
			continue;
		} else if (r < 0 && err == EILSEQ && c->mode == MODE_WRITE &&
			   !verify_only_pass) {
			/*
			 * A destructive write pass is the cure for this, not a
			 * victim of it: writing the block is what generates a
			 * valid guard tag, so a refusal here is the reason to
			 * write the chunk rather than a reason to skip it.
			 * The pre-read only exists to time the drive and the
			 * data is about to be overwritten anyway.
			 *
			 * Verify mode deliberately does not do this. It has to
			 * save the original bytes before writing a pattern and
			 * it has just failed to read them, so writing would
			 * break invariant 4.
			 */
			prot_write = 1;
			c->prot_run = 0;
		} else if (r < 0 && err == EILSEQ) {
			/* never read, so it must not colour the surface map:
			 * take the band accounting back off and let the cell
			 * render as '_' rather than as an instant, fast read */
			c->prot_errors++;
			c->prot_bytes += len;
			c->chunks_read--;
			c->bands[band].chunks--;
			c->bands[band].bytes -= len;
			/*
			 * A drive formatted with PI and never written refuses
			 * every block, instantly.  Grinding through the whole
			 * device to learn that a second time would take most
			 * of a day and teach nothing, so give up once it is
			 * clearly the format rather than a written region we
			 * happened to fall off the end of.
			 */
			if (++c->prot_run >= PROT_RUN_MAX) {
				snprintf(c->abort_reason, sizeof(c->abort_reason),
					 "%" PRIu64 " chunks in a row refused by "
					 "protection information: nothing here has "
					 "been written since the format",
					 c->prot_run);
				break;
			}
			continue;
		} else {
			if (r >= 0)
				c->short_reads++;
			c->chunks_err++;
			c->hard_errors++;
			drill = 1;
		}

		if (verify_only_pass && r == (ssize_t)len) {
			uint64_t bad_at = 0;

			if (pattern_check(buf, len, c->pos, c->seed, &bad_at) < 0) {
				finding_t *f = find_add(c);

				c->blocks_corrupt++;
				if (f) {
					f->offset = bad_at - (bad_at % c->block);
					f->len = (uint32_t)c->block;
					f->status = ST_CORRUPT;
					f->attempts = 1;
				}
				drill = 1;
			}
		}

		/* ---- write phase ---- */
		if (!verify_only_pass && mode_writes(c->mode) &&
		    (r == (ssize_t)len || prot_write)) {
			uint64_t wus = 0, rus = 0;
			int werr = 0;
			ssize_t w;

			if (c->mode == MODE_VERIFY) {
				memcpy(orig, buf, len);
				c->restore_off = c->pos;
				c->restore_len = len;
				c->restore_pending = 1;
				if (jrn_write(c->jrn_fd, c->dev, c->pos, orig, len) < 0) {
					msg(PROG ": journal write failed: %s; "
					    "stopping rather than risk data\n",
					    strerror(errno));
					snprintf(c->abort_reason,
						 sizeof(c->abort_reason),
						 "journal write failed");
					break;
				}
			}
			pattern_fill(pat, len, c->pos, c->seed);
			w = timed_pwrite(c->fd, pat, len, c->pos, &wus, &werr);
			if (w == (ssize_t)len) {
				lat_add(&c->write_lat, wus);
				latwin_add(&c->wlatwin, wus);
				c->writes_done++;
				if (wus > chunk_thr_at(c, c->pos))
					c->writes_slow++;
				if (c->wr_early_n < 512) {
					c->wr_early_sum += wus;
					c->wr_early_n++;
				} else {
					c->wr_late_sum += wus;
					c->wr_late_n++;
					if (c->wr_late_n >= 512) {
						double e = (double)c->wr_early_sum /
							   (double)c->wr_early_n;
						double l = (double)c->wr_late_sum /
							   (double)c->wr_late_n;

						/*
						 * SMR collapses and stays
						 * collapsed once its CMR cache
						 * is full.  A single bad window
						 * is a defect the head is
						 * fighting, not a shingled
						 * drive -- a cluster of sectors
						 * needing a second of recovery
						 * each used to be enough to
						 * declare a conventional disk
						 * SMR, in a line the report
						 * then repeated as fact.
						 */
						if (e > 0 && l > e * 8)
							c->smr_windows++;
						else
							c->smr_windows = 0;
						if (!c->smr_suspected &&
						    c->smr_windows >= SMR_WINDOWS) {
							c->smr_suspected = 1;
							msg(PROG ": %s: %swrite speed has been "
							    "%.0fx slower than the start of "
							    "the scan for %d windows "
							    "running%s; that is what a "
							    "drive-managed SMR disk does once "
							    "its cache fills, and the latency "
							    "results are not trustworthy\n",
							    c->dev->name, c_yel(), l / e,
							    SMR_WINDOWS, c_off());
						}
						c->wr_late_sum = 0;
						c->wr_late_n = 0;
					}
				}
				if (wus > chunk_thr_at(c, c->pos) * 4)
					drill = 1;
				r = timed_pread(c->fd, buf, len, c->pos, &rus, &err);
				if (r == (ssize_t)len) {
					uint64_t bad_at = 0;

					lat_add(&c->chunk_lat, rus);
					latwin_add(&c->latwin, rus);
					if (prot_write) {
						/* it reads now, so it counts
						 * as covered, and the band
						 * needs the timing the
						 * refused pre-read never gave
						 * it */
						c->prot_fixed++;
						c->prot_fixed_bytes += len;
						c->bands[band].sum_us += rus;
					}
					if (rus > chunk_thr_at(c, c->pos))
						drill = 1;
					if (pattern_check(buf, len, c->pos,
							  c->seed, &bad_at) < 0) {
						finding_t *f = find_add(c);

						c->blocks_corrupt++;
						if (f) {
							f->offset = bad_at - (bad_at % c->block);
							f->len = (uint32_t)c->block;
							f->status = ST_CORRUPT;
							f->attempts = 1;
						}
						drill = 1;
					}
				} else {
					c->hard_errors++;
					drill = 1;
				}
			} else {
				c->hard_errors++;
				drill = 1;
			}

			if (c->mode == MODE_VERIFY) {
				uint64_t rus2;
				int e2;

				timed_pwrite(c->fd, orig, len, c->pos, &rus2, &e2);
				c->restore_pending = 0;
				jrn_clear(c->jrn_fd);
			}
		}

		if (drill)
			drill_chunk(c, c->pos, len, buf, scratch);

		c->bytes_done += len;

		if (c->max_errors && c->hard_errors >= c->max_errors) {
			snprintf(c->abort_reason, sizeof(c->abort_reason),
				 "hard error limit of %" PRIu64 " reached",
				 c->max_errors);
			break;
		}

		check_temperature(c);
		progress(c, 0);
		if (c->state_path && now_us() - last_state > 15000000ull) {
			state_save(c);
			last_state = now_us();
		}
	}
	progress(c, 1);
	state_save(c);

	free(buf);
	free(orig);
	free(pat);
	free(scratch);
	c->restore_buf = NULL;
}

/* ------------------------------------------------------------------ *
 * reporting
 * ------------------------------------------------------------------ */

static int find_cmp_sev(const void *a, const void *b)
{
	const finding_t *x = a, *y = b;
	int sx, sy;
	static const int rank[] = { 0, 1, 2, 3, 5, 4 };

	sx = rank[x->status];
	sy = rank[y->status];
	if (sx != sy)
		return sy - sx;
	if (x->max_us != y->max_us)
		return x->max_us < y->max_us ? 1 : -1;
	return x->offset < y->offset ? -1 : 1;
}

static void print_hist(const lat_t *l, const char *title)
{
	uint64_t peak = 0;
	int i;

	if (!l->count)
		return;
	for (i = 0; i < NBUCKETS; i++)
		if (l->hist[i] > peak)
			peak = l->hist[i];
	out("\n  %s (%" PRIu64 " samples)\n", title, l->count);
	for (i = 0; i < NBUCKETS; i++) {
		char lo[32], hi[32];
		int bar, j;

		if (!l->hist[i])
			continue;
		snprintf(lo, sizeof(lo), "%.3g", i ? (double)bucket_hi[i - 1] / 1000.0 : 0.0);
		if (bucket_hi[i] == UINT64_MAX)
			snprintf(hi, sizeof(hi), "inf");
		else
			snprintf(hi, sizeof(hi), "%.4g", (double)bucket_hi[i] / 1000.0);
		bar = peak ? (int)(40.0 * (double)l->hist[i] / (double)peak + 0.5) : 0;
		out("    %8s - %-8s ms | ", lo, hi);
		for (j = 0; j < bar; j++)
			out("#");
		for (; j < 41; j++)
			out(" ");
		out("%10" PRIu64 "  %5.2f%%\n", l->hist[i],
		    100.0 * (double)l->hist[i] / (double)l->count);
	}
}

static void print_percentiles(const lat_t *l, const char *what)
{
	if (!l->count)
		return;
	out("  %-12s n=%-10" PRIu64 " avg %8.2f ms   p50 %7.2f  p95 %7.2f  "
	    "p99 %7.2f  p99.9 %7.2f  max %8.2f ms\n",
	    what, l->count, (double)l->sum_us / (double)l->count / 1000.0,
	    lat_pct(l, 50) / 1000.0, lat_pct(l, 95) / 1000.0,
	    lat_pct(l, 99) / 1000.0, lat_pct(l, 99.9) / 1000.0,
	    (double)l->max_us / 1000.0);
}

/*
 * A stretch of the surface that reads far slower than calibration says it
 * should, even with every sector inside its budget.  Damage localised to a
 * band is a head struggling over a patch or a scratch, not random wear, and
 * it is a trend that gets worse.  Judged against the gradient measured before
 * the scan, never against the neighbouring bands: a whole platter going slow
 * must not hide by being uniformly bad.
 *
 * Twice the expected cost is well clear of anything a smooth zone gradient
 * does between anchors, and the band has to be over by a couple of
 * milliseconds as well, so microsecond jitter -- an image file, or an SSD --
 * never looks like a factor of two.  A sampled scan seeks between the chunks
 * it reads, which calibration did not time, so it is not judged at all.
 */
#define BAND_SLOW_X 2.0
#define BAND_MIN_EXCESS_US 2000
#define BAND_MIN_CHUNKS 32

typedef struct {
	int n;                  /* slow bands */
	uint64_t bytes;         /* how much of the surface they cover */
	uint64_t worst_off;
	double worst_x;
} slowbands_t;

static uint64_t band_start(const ctx_t *c, int i)
{
	uint64_t span = c->dev->size ? c->dev->size : 1;

	return (uint64_t)i * (span / N_BANDS + 1);
}

static void slow_bands(const ctx_t *c, slowbands_t *sb)
{
	int i;

	memset(sb, 0, sizeof(*sb));
	if (c->sample > 1)
		return;
	for (i = 0; i < N_BANDS; i++) {
		const band_t *b = &c->bands[i];
		uint64_t mid, exp;
		double avg;

		if (b->chunks < BAND_MIN_CHUNKS)
			continue;
		mid = band_start(c, i) + (band_start(c, 1) / 2);
		if (mid < c->start)
			mid = c->start;
		if (mid > c->end)
			mid = c->end;
		exp = expected_at(c, mid);
		if (!exp)
			return;
		avg = (double)b->sum_us / (double)b->chunks;
		if (avg < (double)exp * BAND_SLOW_X ||
		    avg - (double)exp < BAND_MIN_EXCESS_US)
			continue;
		sb->n++;
		sb->bytes += b->bytes;
		if (avg / (double)exp > sb->worst_x) {
			sb->worst_x = avg / (double)exp;
			sb->worst_off = band_start(c, i);
		}
	}
}

static void print_map(ctx_t *c)
{
	int width = c->map_width;
	int cells, i;
	double maxavg = 0, minavg = -1;
	char b[32];
	static const char heat[] = ".:-=+*#@";

	if (width < 16)
		width = 16;
	if (width > N_BANDS)
		width = N_BANDS;
	cells = width;

	for (i = 0; i < N_BANDS; i++)
		if (c->bands[i].chunks) {
			double a = (double)c->bands[i].sum_us / (double)c->bands[i].chunks;

			if (a > maxavg)
				maxavg = a;
			if (minavg < 0 || a < minavg)
				minavg = a;
		}
	if (minavg < 0)
		minavg = 0;

	out("\n  Surface map (%s per cell, left = LBA 0, right = end of disk)\n",
	    human_size(c->dev->size / (uint64_t)cells, b, sizeof(b)));
	out("    ");
	for (i = 0; i < cells; i++) {
		int lo = i * N_BANDS / cells, hi = (i + 1) * N_BANDS / cells;
		uint64_t chunks = 0, bad = 0, slow = 0, sum = 0;
		int j;
		char ch;

		for (j = lo; j < hi && j < N_BANDS; j++) {
			chunks += c->bands[j].chunks;
			bad += c->bands[j].bad_blocks;
			slow += c->bands[j].slow_blocks + c->bands[j].slow_chunks;
			sum += c->bands[j].sum_us;
		}
		if (!chunks)
			ch = '_';
		else if (bad)
			ch = 'X';
		else if (slow)
			ch = '!';
		else if (maxavg > minavg) {
			double a = (double)sum / (double)chunks;
			int lvl = (int)(7.0 * (a - minavg) / (maxavg - minavg) + 0.5);

			if (lvl < 0)
				lvl = 0;
			if (lvl > 7)
				lvl = 7;
			ch = heat[lvl];
		} else {
			ch = '.';
		}
		out("%c", ch);
	}
	out("\n    legend: '.:-=+*#@' fastest to slowest average read, "
	    "'!' weak sectors, 'X' bad sectors, '_' not scanned\n");
	{
		slowbands_t sb;
		char b1[32], b2[32];

		slow_bands(c, &sb);
		if (sb.n)
			out("    %s%d band%s (%s) read at more than %.0fx what "
			    "calibration expects there%s,\n    worst %.1fx at %s: "
			    "damage localised like this is a head or a scratch\n",
			    c_yel(), sb.n, sb.n == 1 ? "" : "s",
			    human_size(sb.bytes, b1, sizeof(b1)), BAND_SLOW_X,
			    c_off(), sb.worst_x,
			    human_size(sb.worst_off, b2, sizeof(b2)));
	}
	out("    a smooth left-to-right gradient is normal: outer tracks hold more\n"
	    "    sectors per revolution, so they read faster than inner ones\n");
}

static const char *errname(int e)
{
	return e ? strerror(e) : "-";
}

/*
 * A chunk over budget whose sectors all read cleanly when drilled into was the
 * drive having a moment -- a neighbour's vibration, a thermal recalibration,
 * its own background scan -- and every drive has those.  Once in a while is
 * not a verdict; as a habit it is.  A few are needed before the rate means
 * anything, so a short range is not judged on one.
 */
#define OVER_ONE_IN 1000
#define OVER_MIN 3

static int over_too_often(uint64_t over, uint64_t of)
{
	return over >= OVER_MIN && over > of / OVER_ONE_IN;
}

static int verdict_of(ctx_t *c, const char **why)
{
	static char buf[200];
	long long d_realloc = 0, d_pending = 0, d_uncorr = 0;

	if (c->smart_before.have && c->smart_after.have) {
		d_realloc = c->smart_after.realloc_ct - c->smart_before.realloc_ct;
		d_pending = c->smart_after.pending - c->smart_before.pending;
		d_uncorr = c->smart_after.offline_uncorr - c->smart_before.offline_uncorr;
	}

	if (c->spares_exhausted) {
		*why = "the drive can no longer reallocate: its spare sectors are gone";
		return 2;
	}
	if (c->blocks_bad || c->blocks_corrupt || d_uncorr > 0) {
		*why = "unreadable or corrupted sectors were found";
		return 2;
	}
	if (c->smart_after.have && c->smart_after.health[0] &&
	    strcasecmp(c->smart_after.health, "PASSED") &&
	    strcasecmp(c->smart_after.health, "OK")) {
		*why = "the drive's own SMART health assessment failed";
		return 2;
	}
	/*
	 * A drive that has failed its own long self-test has said it is going
	 * to fail.  It reports SMART health OK right up until it does, because
	 * "OK" only means no threshold is currently exceeded.
	 */
	if (c->smart_before.selftest_failed) {
		*why = "the drive has failed its own self-test";
		return 2;
	}
	if (c->too_slow) {
		char b1[32], b2[32];
		int usb = c->dev->transport == TR_USB;

		snprintf(buf, sizeof(buf), "the drive tested at %s/s, below the "
			 "%s/s floor for %zu KiB chunks%s",
			 human_size((uint64_t)c->rate_seen, b1, sizeof(b1)),
			 human_size((uint64_t)c->rate_floor, b2, sizeof(b2)),
			 c->chunk / 1024,
			 usb ? "; a USB 2 link alone can be that slow" : "");
		*why = buf;
		return usb ? 1 : 2;
	}
	if (c->blocks_unstable || c->blocks_slow || d_realloc > 0 || d_pending > 0) {
		*why = "sectors needed retries or the drive reallocated during the scan";
		return 1;
	}
	if (c->blocks_recovered) {
		*why = "some reads exceeded the latency budget but recovered on retry";
		return 1;
	}
	if (over_too_often(c->chunks_slow, c->chunks_read)) {
		snprintf(buf, sizeof(buf), "%" PRIu64 " of %" PRIu64 " chunk reads "
			 "were over the latency budget, more than one in %d, "
			 "though no sector stayed slow when drilled into",
			 c->chunks_slow, c->chunks_read, OVER_ONE_IN);
		*why = buf;
		return 1;
	}
	{
		slowbands_t sb;
		char b1[32], b2[32];

		slow_bands(c, &sb);
		if (sb.n) {
			snprintf(buf, sizeof(buf), "%d band%s of the surface (%s) "
				 "read at more than %.0fx what calibration expects "
				 "there, worst %.1fx at %s", sb.n,
				 sb.n == 1 ? "" : "s",
				 human_size(sb.bytes, b1, sizeof(b1)), BAND_SLOW_X,
				 sb.worst_x,
				 human_size(sb.worst_off, b2, sizeof(b2)));
			*why = buf;
			return 1;
		}
	}
	if (over_too_often(c->writes_slow, c->writes_done)) {
		snprintf(buf, sizeof(buf), "%" PRIu64 " of %" PRIu64 " chunk writes "
			 "took longer than the latency budget, more than one in "
			 "%d%s", c->writes_slow, c->writes_done, OVER_ONE_IN,
			 c->smr_suspected ? "; the pattern is drive-managed SMR" : "");
		*why = buf;
		return 1;
	}
	if (c->smart_after.have &&
	    (c->smart_after.pending || c->smart_after.offline_uncorr ||
	     c->smart_after.reported_uncorr)) {
		*why = "SMART reports pending or uncorrectable sectors from before this scan";
		return 1;
	}
	if (c->chunks_slow || c->writes_slow) {
		char wr[48] = "";

		if (c->writes_done)
			snprintf(wr, sizeof(wr), " and %" PRIu64 " writes",
				 c->writes_slow);
		snprintf(buf, sizeof(buf), "every sector returned data within the "
			 "latency budget; %" PRIu64 " chunk reads%s went over it, "
			 "too rarely to hold against the drive", c->chunks_slow, wr);
		*why = buf;
		return 0;
	}
	*why = "every sector returned data within the latency budget";
	return 0;
}

static void print_smart_line(const char *label, long long before, long long after,
			     int have_after)
{
	if (before < 0 && after < 0)
		return;
	if (have_after && before != after)
		out("    %-28s %8lld -> %-8lld %s(changed during scan)%s\n",
		    label, before, after, c_red(), c_off());
	else
		out("    %-28s %8lld\n", label, before);
}

static void report(ctx_t *c)
{
	device_t *d = c->dev;
	char b1[32], b2[32];
	double secs = (double)(c->t_end_us - c->t_start_us) / 1e6;
	const char *why;
	int v;
	int i, shown;

	out("\n");
	out("========================================================================\n");
	out(" %sDrive health report%s - %s\n", c_bold(), c_off(), d->path);
	out("========================================================================\n");
	out("  Model            %s %s\n", d->vendor, d->model);
	if (d->serial[0])
		out("  Serial           %s\n", d->serial);
	if (d->byid[0])
		out("  Stable id        /dev/disk/by-id/%s\n", d->byid);
	out("  Capacity         %s (%" PRIu64 " bytes)\n",
	    human_size(d->size, b1, sizeof(b1)), d->size);
	out("  Sector size      %d logical / %d physical\n",
	    d->logical_bs, d->physical_bs);
	out("  Media            %s, %s%s\n",
	    d->rotational ? "rotational (HDD)" : "non-rotational (SSD/flash)",
	    zoned_name(d->zoned),
	    c->smr_suspected ? " (write behaviour suggests drive-managed SMR)" : "");
	if (!d->is_file) {
		if (d->prot_type > 0)
			out("  Protection       T10 type %d, 8 bytes beside every "
			    "logical block\n", d->prot_type);
		out("  Transport        %s%s\n", transport_name(d->transport),
		    transport_is_local(d->transport) ? "" :
		    "  (NOT a local drive - these numbers describe the link "
		    "and the remote storage)");
	}
	if (c->profile)
		out("  Profile          %s\n", c->profile);
	out("  Mode             %s\n", mode_name(c->mode));
	out("  Range            %s .. %s (%s)\n",
	    human_size(c->start, b1, sizeof(b1)),
	    human_size(c->end, b2, sizeof(b2)),
	    human_size(c->end - c->start, (char[32]){0}, 32));
	out("  Chunk / block    %zu KiB / %zu KiB\n", c->chunk / 1024, c->block / 1024);
	{
		uint64_t tail = d->size > c->end ? d->size - c->end : 0;
		int whole = (c->start == 0 && tail == 0);

		if (c->sample > 1)
			out("  Coverage         %sSAMPLED: 1 chunk in %" PRIu64
			    ", so most sectors were never read%s\n",
			    c_yel(), c->sample, c_off());
		else if (whole && !c->abort_reason[0] && !g_stop && c->step >= c->nchunks)
			out("  Coverage         every sector of the device was read\n");
		else if (whole)
			out("  Coverage         %sPARTIAL: stopped after %s of %s%s\n",
			    c_yel(), human_size(c->bytes_done, b1, sizeof(b1)),
			    human_size(d->size, b2, sizeof(b2)), c_off());
		else
			out("  Coverage         %srestricted range; %s at the end of the "
			    "device was not tested%s\n", c_yel(),
			    human_size(tail, b1, sizeof(b1)), c_off());
		if (c->prot_errors)
			out("  Not covered      %s%s refused by the drive's protection "
			    "information%s\n", c_yel(),
			    human_size(c->prot_bytes, b1, sizeof(b1)), c_off());
	}
	if (c->grad_budget) {
		uint64_t lo = UINT64_MAX, hi = 0;
		int k;

		for (k = 0; k < c->grad_n; k++) {
			uint64_t t = chunk_thr_at(c, c->grad_off[k]);

			if (t < lo)
				lo = t;
			if (t > hi)
				hi = t;
		}
		out("  Latency budget   %.1f ms where the platter is quickest to "
		    "%.1f ms where it is\n                   slowest, following the "
		    "drive's own gradient (auto-calibrated)\n",
		    (double)lo / 1000.0, (double)hi / 1000.0);
	} else
	out("  Latency budget   chunk %.1f ms, sector %.1f ms%s\n",
	    (double)c->chunk_thr_us / 1000.0, (double)c->block_thr_us / 1000.0,
	    c->calib_failed ? "  <- FALLBACK: nothing on this drive could be "
	    "timed, so this is not its budget" :
	    c->auto_thr ? " (auto-calibrated)" : "");
	out("  Retries          %d per suspicious sector, up to %.0fs each\n",
	    c->retries, (double)c->retry_cap_us / 1e6);
	if (c->order == ORD_RANDOM)
		out("  Shuffle segment  %s (%" PRIu64 " chunks); seeks stay inside "
		    "this window\n",
		    human_size(c->seg_chunks * (uint64_t)c->chunk, b1, sizeof(b1)),
		    c->seg_chunks);
	if (!d->is_file) {
		if (c->sleep_timers)
			out("  Power saving     %s%d idle/standby timer%s "
			    "enabled%s%s\n", c_yel(), c->sleep_timers,
			    c->sleep_timers == 1 ? "" : "s", c_off(),
			    c->bms_state > 0 ?
			    "  <- a sleeping drive is not running its "
			    "background scan" :
			    "  <- the drive may spin down mid-scan");
		if (c->sleep_timers && c->bms_state > 0 && c->pm_bg >= 0)
			out("                   power-management precedence "
			    "field is %d (0 vendor specific,\n"
			    "                   1 background functions win, "
			    "2 power management wins)\n", c->pm_bg);
		if (c->awre_state >= 0 || c->arre_state >= 0)
			out("  Auto-reallocate  write %s, read %s%s\n",
			    c->awre_state > 0 ? "on" : "OFF",
			    c->arre_state > 0 ? "on" : "OFF",
			    (c->awre_state > 0 && c->arre_state > 0) ? "" :
			    "  <- the drive will not retire a sector it "
			    "struggles with");
		if (c->rtl_state == 0)
			out("  Recovery limit   %sunlimited%s: one bad sector "
			    "can stall this scan for minutes\n", c_yel(),
			    c_off());
		else if (c->rtl_state > 0)
			out("  Recovery limit   %lld ms per sector\n",
			    c->rtl_state);
		if (c->rrc_state >= 0)
			out("  Read retries     %lld inside the drive before it "
			    "reports the sector\n", c->rrc_state);
		if (c->rcd_state == 0)
			out("  Read cache       on: a re-read of the same sector "
			    "can be answered from DRAM\n");
		else if (c->rcd_state > 0)
			out("  Read cache       off: every read reaches the "
			    "platter\n");
		if (c->wc_state >= 0)
			out("  Write cache      %s\n", c->wc_state ?
			    "on: a write returns once the drive has it in DRAM, "
			    "not on the platter" :
			    "off: a write reaches the platter before it returns");
		if (c->bms_state >= 0)
			out("  Background scan  %s\n", c->bms_state ?
			    "enabled: the drive sweeps its own media when idle" :
			    "off: the drive never checks its own media "
			    "('--bms on' turns it on for good)");
		out("  Drive look-ahead %s\n", c->la_disabled ?
		    "disabled for this scan (no prefetch masking)" :
		    c->order == ORD_RANDOM ?
		    "left on (random order defeats it anyway)" :
		    "LEFT ON - a marginal sector could have been served from the "
		    "drive's cache");
	}
	out("  Scan order       %s%s\n", order_name(c->order),
	    c->order == ORD_SEQ ?
	    "  (drive prefetch may mask marginal sectors; --order random avoids this)"
	    : "");
	out("  Duration         %s, %s read, %s/s average\n",
	    human_time(secs, b1, sizeof(b1)),
	    human_size(c->bytes_done, b2, sizeof(b2)),
	    human_size(secs > 0 ? (uint64_t)((double)(c->bytes_done -
						      c->bytes_at_start) / secs) : 0,
		       (char[32]){0}, 32));
	if (c->too_slow)
		out("  %sToo slow%s         %s/s against a floor of %s/s at %zu KiB "
		    "chunks (--min-rate)\n", c_red(), c_off(),
		    human_size((uint64_t)c->rate_seen, b1, sizeof(b1)),
		    human_size((uint64_t)c->rate_floor, b2, sizeof(b2)),
		    c->chunk / 1024);
	if (c->abort_reason[0])
		out("  %sIncomplete%s       %s\n", c_yel(), c_off(), c->abort_reason);
	if (g_stop && !c->abort_reason[0])
		out("  %sIncomplete%s       interrupted by user\n", c_yel(), c_off());

	out("\n  --- Latency ---\n");
	print_percentiles(&c->chunk_lat, "chunk reads");
	print_percentiles(&c->block_lat, "sector reads");
	print_percentiles(&c->write_lat, "chunk writes");

	out("\n  Reads exceeding a given time (chunk level):\n");
	{
		for (i = 0; i < NTIERS; i++)
			out("    > %6.0f ms : %10" PRIu64 "  %s\n",
			    (double)lat_tiers[i] / 1000.0,
			    c->chunk_lat.over[i],
			    c->chunk_lat.over[i] ? "" : "(none)");
	}

	print_hist(&c->chunk_lat, "Chunk read latency distribution");
	if (c->block_lat.count)
		print_hist(&c->block_lat, "Sector read latency distribution (suspect sectors only)");

	print_map(c);

	out("\n  --- Counters ---\n");
	out("    chunks read              %12" PRIu64 "\n", c->chunks_read);
	out("    chunks over budget       %12" PRIu64 "\n", c->chunks_slow);
	if (mode_writes(c->mode))
		out("    writes over budget       %12" PRIu64 "  of %" PRIu64 "\n",
		    c->writes_slow, c->writes_done);
	out("    chunks with I/O errors   %12" PRIu64 "\n", c->chunks_err);
	out("    sectors drilled down     %12" PRIu64 "\n", c->blocks_drilled);
	out("    retry reads issued       %12" PRIu64 "\n", c->retry_ios);
	out("    short reads              %12" PRIu64 "\n", c->short_reads);
	out("    hard I/O errors          %12" PRIu64 "\n", c->hard_errors);
	if (c->align_errors)
		out("    %smisaligned reads skipped %12" PRIu64 "%s  (kernel rejected "
		    "the request: a tool/geometry issue, not a drive fault)\n",
		    c_yel(), c->align_errors, c_off());
	if (c->retry_ios && c->rcd_state == 0)
		out("\n    %sThe retries above were run against a drive whose read "
		    "cache is on.%s\n"
		    "    Re-reading a sector the drive has just fetched is "
		    "answered from its\n"
		    "    DRAM, so a sector that recovers slowly once and returns "
		    "instantly for\n"
		    "    every retry is indistinguishable from one that is "
		    "genuinely fine. That\n"
		    "    is why such sectors land in 'intermittently bad' rather "
		    "than\n    'persistently slow'. Re-run with --read-cache off "
		    "to make the retries\n    mean something.\n",
		    c_yel(), c_off());
	if (c->prot_errors)
		out("    %sblocked by protection   %12" PRIu64 "%s  (guard tag check "
		    "failed: never written since the format, not a media fault)\n",
		    c_yel(), c->prot_errors, c_off());
	if (c->prot_fixed)
		out("    %sguard tags written      %12" PRIu64 "%s  (chunks that "
		    "refused to be read until this pass wrote them)\n",
		    c_grn(), c->prot_fixed, c_off());
	out("\n");
	out("    %ssectors recovered on retry%s%12" PRIu64 "  (slow once, fine afterwards)\n",
	    c->blocks_recovered ? c_yel() : "", c->blocks_recovered ? c_off() : "",
	    c->blocks_recovered);
	out("    %ssectors intermittently bad%s%12" PRIu64 "  (sometimes slow or failing)\n",
	    c->blocks_unstable ? c_yel() : "", c->blocks_unstable ? c_off() : "",
	    c->blocks_unstable);
	out("    %ssectors persistently slow %s%12" PRIu64 "  (always over budget)\n",
	    c->blocks_slow ? c_yel() : "", c->blocks_slow ? c_off() : "",
	    c->blocks_slow);
	out("    %ssectors unreadable        %s%12" PRIu64 "  (every attempt failed)\n",
	    c->blocks_bad ? c_red() : "", c->blocks_bad ? c_off() : "",
	    c->blocks_bad);
	if (mode_writes(c->mode) || c->mode == MODE_CHECK || c->second_pass)
		out("    %ssectors with wrong data   %s%12" PRIu64 "  (silent corruption)\n",
		    c->blocks_corrupt ? c_red() : "", c->blocks_corrupt ? c_off() : "",
		    c->blocks_corrupt);
	if (c->rewrite_weak || c->force_remap) {
		out("    sectors healed by rewrite%12" PRIu64 "  (read cleanly "
		    "afterwards)\n", c->blocks_fixed);
		if (c->sas_reassigned)
			out("    sectors reassigned       %12" PRIu64 "  (the drive "
			    "retired them to spares on request)\n",
			    c->sas_reassigned);
		if (c->smart_after.have)
			out("    firmware reallocations   %12lld  (sectors the drive "
			    "moved to spares during this scan)\n", c->realloc_delta);
	}
	if (c->spares_exhausted)
		out("\n    %sThe drive kept its pending sectors after we rewrote them "
		    "and reallocated nothing.%s\n"
		    "    That usually means the spare sector pool is exhausted and "
		    "the drive has\n    no way left to repair itself.\n",
		    c_red(), c_off());

	if (c->smart_before.have) {
		out("\n  --- SMART ---\n");
		if (c->smart_before.health[0])
			out("    overall health               %s%s%s\n",
			    strcasecmp(c->smart_before.health, "PASSED") ? c_red() : c_grn(),
			    c->smart_before.health, c_off());
		if (c->smart_before.power_on_hours)
			out("    power on hours           %8lld\n",
			    c->smart_before.power_on_hours);
		if (c->smart_before.ata) {
			out("    (values shown before -> after the scan when they "
			    "changed)\n");
			print_smart_line("reallocated sectors (5)",
					 c->smart_before.realloc_ct,
					 c->smart_after.realloc_ct,
					 c->smart_after.have);
			print_smart_line("reallocation events (196)",
					 c->smart_before.realloc_event,
					 c->smart_after.realloc_event,
					 c->smart_after.have);
			print_smart_line("pending sectors (197)",
					 c->smart_before.pending,
					 c->smart_after.pending,
					 c->smart_after.have);
			print_smart_line("offline uncorrectable (198)",
					 c->smart_before.offline_uncorr,
					 c->smart_after.offline_uncorr,
					 c->smart_after.have);
			print_smart_line("reported uncorrect (187)",
					 c->smart_before.reported_uncorr,
					 c->smart_after.reported_uncorr,
					 c->smart_after.have);
			print_smart_line("interface CRC errors (199)",
					 c->smart_before.crc_err,
					 c->smart_after.crc_err,
					 c->smart_after.have);
		} else if (!c->smart_before.sas) {
			out("    %sno sector counters were read from this drive%s\n"
			    "    neither the ATA attribute table nor the SCSI error "
			    "counter log\n    came back, so nothing here confirms or "
			    "denies reallocations\n", c_yel(), c_off());
		}
		if (c->smart_before.selftest[0])
			out("    last self-test           %s%s%s\n",
			    c->smart_before.selftest_failed ? c_red() : "",
			    c->smart_before.selftest,
			    c->smart_before.selftest_failed ? c_off() : "");
		if (c->smart_before.selftest_failed)
			out("    %sThe drive has failed its own self-test.%s "
			    "That is the drive predicting\n    its own failure, "
			    "and it outranks anything this scan measured.\n",
			    c_red(), c_off());
		if (c->temp_max > 0)
			out("    temperature (C)          %8lld min / %lld max\n",
			    c->temp_min < 0 ? c->temp_max : c->temp_min, c->temp_max);
	}

	if (c->smart_before.have && c->smart_before.sas) {
		const smart_t *a = c->smart_after.have ? &c->smart_after
						      : &c->smart_before;
		const smart_t *b0 = &c->smart_before;

		out("\n  --- SAS error counter log ---\n");
		out("    %-34s %12s %12s\n", "", "before", "after");
		out("    %-34s %12lld %12lld\n", "read: corrected with delays",
		    b0->rd_delayed, a->rd_delayed);
		out("    %-34s %12lld %12lld\n", "read: rereads/rewrites needed",
		    b0->rd_reread, a->rd_reread);
		out("    %-34s %12lld %12lld\n", "read: ECC invocations",
		    b0->rd_algo, a->rd_algo);
		out("    %-34s %12lld %12lld\n", "read: UNCORRECTED errors",
		    b0->rd_uncorr, a->rd_uncorr);
		out("    %-34s %12lld %12lld\n", "write: UNCORRECTED errors",
		    b0->wr_uncorr, a->wr_uncorr);
		out("    %-34s %12lld\n", "non-medium errors", a->non_medium);
		if (b0->rd_delayed > 0 && b0->rd_gb > 0)
			out("\n    %s%lld delayed corrections%s over %.1f TB read "
			    "(%.1f per TB).\n"
			    "    Delayed means the drive got the data back only "
			    "after extra work,\n    which is what this scan "
			    "measures as latency. 'fast' corrections\n    cost "
			    "nothing; these do.\n",
			    b0->rd_delayed > 1000000 ? c_yel() : "",
			    b0->rd_delayed,
			    b0->rd_delayed > 1000000 ? c_off() : "",
			    b0->rd_gb / 1000.0,
			    (double)b0->rd_delayed / b0->rd_gb * 1000.0);

		if (a->rd_delayed > b0->rd_delayed || a->rd_reread > b0->rd_reread)
			out("\n    %sThe drive needed extra time or extra passes to read "
			    "data during this scan.%s\n"
			    "    That is the firmware fighting the media, and it is "
			    "what a drive that\n    writes fast but reads slowly is "
			    "actually doing. Rewriting the affected\n    sectors "
			    "(--rewrite-weak, or a full --mode write pass) refreshes "
			    "them and\n    lets the drive remap whatever cannot be "
			    "refreshed.\n", c_yel(), c_off());

		if (a->rcd < 0 && a->dra < 0 && a->wce < 0 && a->awre < 0 &&
		    a->arre < 0 && a->recovery_time_ms < 0)
			goto no_modes;
		out("\n  --- SAS configuration ---\n");
		if (a->rcd >= 0)
			out("    read cache          %s%s\n",
			    a->rcd ? "DISABLED" : "enabled",
			    a->rcd ? "  <- every read goes to the media; this alone "
			    "makes reads crawl" : "");
		if (a->dra >= 0)
			out("    read look-ahead     %s%s\n",
			    a->dra ? "DISABLED" : "enabled",
			    a->dra ? "  <- no prefetch; sequential reads will be slow"
			    : "");
		if (a->wce >= 0)
			out("    write cache         %s%s\n",
			    a->wce ? "enabled" : "disabled",
			    (a->wce == 1 && a->rcd == 1) ?
			    "  <- writes cached but reads not: that asymmetry is a "
			    "setting, not a fault" : "");
		if (a->arre >= 0)
			out("    auto-reallocate on read (ARRE)  %s%s\n",
			    a->arre ? "on" : "OFF",
			    a->arre ? "" : "  <- the drive will never repair a sector "
			    "it struggles to read");
		if (a->awre >= 0)
			out("    auto-reallocate on write (AWRE) %s%s\n",
			    a->awre ? "on" : "OFF",
			    a->awre ? "" : "  <- rewriting will not trigger a remap");
		if (a->recovery_time_ms >= 0)
			out("    recovery time limit %lld ms%s\n", a->recovery_time_ms,
			    a->recovery_time_ms == 0 ?
			    "  (0 = unlimited: a bad sector can stall for a very long "
			    "time)" : "");
		if (a->rcd == 1 || a->arre == 0 || a->awre == 0)
			/*
			 * Two commands, not one: sdparm honours only the last
			 * --set or --clear on a command line, and a comma
			 * separated list may only name fields from a single
			 * mode page.  RCD and DRA live in the caching page,
			 * AWRE and ARRE in read-write error recovery.
			 */
			out("\n    Fix these with sdparm before concluding the media is "
			    "bad:\n"
			    "      sdparm --clear=RCD,DRA --save %s\n"
			    "      sdparm --set=AWRE=1,ARRE=1 --save %s\n",
			    d->path, d->path);
no_modes:	;
	}

	if (!c->smart_before.have && !d->is_file) {
		out("\n  --- SMART ---\n    not available%s\n",
		    have_smartctl() ? " for this device" :
		    " (install smartmontools for SMART data)");
	}

	/* findings */
	if (c->nfind) {
		out("\n  --- Suspect sectors (worst first) ---\n");
		qsort(c->find, (size_t)c->nfind, sizeof(finding_t), find_cmp_sev);
		out("    %-18s %-10s %6s %6s %10s %10s %10s  %s\n",
		    "byte offset", "status", "tries", "errs", "first ms",
		    "med ms", "worst ms", "note");
		shown = c->nfind < 50 ? c->nfind : 50;
		for (i = 0; i < shown; i++) {
			finding_t *f = &c->find[i];
			const char *col = (f->status == ST_BAD || f->status == ST_CORRUPT)
					  ? c_red() : c_yel();

			out("    %s%-18" PRIu64 "%s %-10s %6u %6u %10.2f %10.2f %10.2f  %s%s\n",
			    col, f->offset, c_off(), status_name(f->status),
			    f->attempts, f->errors,
			    (double)f->first_us / 1000.0,
			    (double)f->med_us / 1000.0,
			    (double)f->max_us / 1000.0,
			    f->errnum ? errname(f->errnum) : "",
			    f->fixed_by_rewrite ? " [healed by rewrite]" :
			    f->rewritten ? " [rewritten, still weak]" : "");
		}
		if (c->nfind > shown)
			out("    ... and %d more (use --csv to get the full list)\n",
			    c->nfind - shown);
		if (c->find_overflow)
			out("    %swarning: finding list truncated at %d entries%s\n",
			    c_yel(), MAX_FINDINGS, c_off());
		out("\n    LBA of a byte offset = offset / %d\n", d->logical_bs);
	}

	if (c->top[0].us) {
		out("\n  --- Slowest individual sector reads ---\n");
		for (i = 0; i < TOP_SLOW; i++) {
			int j, best = -1;

			for (j = 0; j < TOP_SLOW; j++)
				if (c->top[j].us && (best < 0 || c->top[j].us > c->top[best].us))
					best = j;
			if (best < 0)
				break;
			out("    %10.2f ms at byte %" PRIu64 " (LBA %" PRIu64 ")\n",
			    (double)c->top[best].us / 1000.0, c->top[best].off,
			    c->top[best].off / (uint64_t)d->logical_bs);
			c->top[best].us = 0;
		}
	}

	v = verdict_of(c, &why);
	out("\n========================================================================\n");
	out(" VERDICT: %s%s%s - %s\n",
	    v == 0 ? c_grn() : v == 1 ? c_yel() : c_red(),
	    v == 0 ? "HEALTHY" : v == 1 ? "SUSPECT" : "FAILING",
	    c_off(), why);
	if (c->prot_errors) {
		out("\n %sNot covered:%s %s of this device answered \"logical block\n"
		    " guard check failed\" and was skipped.\n", c_yel(), c_off(),
		    human_size(c->prot_bytes, b1, sizeof(b1)));
		out(" That is the drive checking the T10 protection information it keeps\n"
		    " beside every block, not the media failing: a block that has never\n"
		    " been written since the format has no valid guard tag, and this\n"
		    " drive is formatted with protection. The platters under those blocks\n"
		    " were never read, so nothing above says anything about them.\n"
		    " Writing a block makes its tag valid, so a full write pass would\n"
		    " cover the device -- and destroy everything on it. So would\n"
		    " reformatting without protection ('sg_format --fmtpinfo=0').\n");
	}
	if (v == 2)
		out(" Do not trust this drive with data. Copy anything valuable off it now,\n"
		    " then replace it. A full destructive write pass can force the drive to\n"
		    " remap the bad sectors, but reallocation growth means the media is going.\n");
	else if (v == 1)
		out(" The drive works but showed weak spots. Re-run the scan in a few days;\n"
		    " if the same offsets are slow again, or SMART counters keep growing,\n"
		    " treat it as failing. --rewrite-weak can refresh marginal sectors.\n");
	out("========================================================================\n\n");
}

/* ------------------------------------------------------------------ *
 * machine readable output
 * ------------------------------------------------------------------ */

static void json_escape(FILE *f, const char *s)
{
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			fprintf(f, "\\u%04x", *s);
		else
			fputc(*s, f);
	}
}

static void write_json(ctx_t *c, const char *path)
{
	FILE *f = fopen(path, "we");
	const char *why;
	int v, i;

	if (!f) {
		msg(PROG ": cannot write %s: %s\n", path, strerror(errno));
		return;
	}
	v = verdict_of(c, &why);
	fprintf(f, "{\n  \"tool\": \"" PROG "\",\n  \"version\": \"" VERSION "\",\n");
	fprintf(f, "  \"device\": \"");
	json_escape(f, c->dev->path);
	fprintf(f, "\",\n  \"model\": \"");
	json_escape(f, c->dev->model);
	fprintf(f, "\",\n  \"serial\": \"");
	json_escape(f, c->dev->serial);
	fprintf(f, "\",\n  \"by_id\": \"");
	json_escape(f, c->dev->byid);
	fprintf(f, "\",\n");
	fprintf(f, "  \"size_bytes\": %" PRIu64 ",\n", c->dev->size);
	fprintf(f, "  \"logical_block_size\": %d,\n", c->dev->logical_bs);
	fprintf(f, "  \"rotational\": %s,\n", c->dev->rotational ? "true" : "false");
	fprintf(f, "  \"mode\": \"%s\",\n",
		c->mode == MODE_READ ? "read" : c->mode == MODE_VERIFY ? "verify" :
		c->mode == MODE_CHECK ? "check" : "write");
	fprintf(f, "  \"range\": {\"start\": %" PRIu64 ", \"end\": %" PRIu64 "},\n",
		c->start, c->end);
	fprintf(f, "  \"chunk_bytes\": %zu,\n  \"block_bytes\": %zu,\n", c->chunk, c->block);
	fprintf(f, "  \"chunk_threshold_us\": %" PRIu64 ",\n", c->chunk_thr_us);
	fprintf(f, "  \"block_threshold_us\": %" PRIu64 ",\n", c->block_thr_us);
	fprintf(f, "  \"retries\": %d,\n", c->retries);
	fprintf(f, "  \"duration_s\": %.3f,\n",
		(double)(c->t_end_us - c->t_start_us) / 1e6);
	fprintf(f, "  \"bytes_tested\": %" PRIu64 ",\n", c->bytes_done);
	fprintf(f, "  \"rate_bytes_per_s\": %.0f,\n", c->rate_seen);
	fprintf(f, "  \"rate_floor_bytes_per_s\": %.0f,\n", c->rate_floor);
	fprintf(f, "  \"too_slow\": %s,\n", c->too_slow ? "true" : "false");
	fprintf(f, "  \"protection_type\": %d,\n", c->dev->prot_type);
	fprintf(f, "  \"protection_refused_bytes\": %" PRIu64 ",\n", c->prot_bytes);
	fprintf(f, "  \"complete\": %s,\n",
		(!c->abort_reason[0] && !g_stop && c->pos >= c->end) ? "true" : "false");
	if (c->abort_reason[0]) {
		fprintf(f, "  \"abort_reason\": \"");
		json_escape(f, c->abort_reason);
		fprintf(f, "\",\n");
	}
	fprintf(f, "  \"counters\": {\n");
	fprintf(f, "    \"protection_errors\": %" PRIu64 ",\n", c->prot_errors);
	fprintf(f, "    \"chunks_read\": %" PRIu64 ",\n", c->chunks_read);
	fprintf(f, "    \"chunks_over_budget\": %" PRIu64 ",\n", c->chunks_slow);
	fprintf(f, "    \"writes\": %" PRIu64 ",\n", c->writes_done);
	fprintf(f, "    \"writes_over_budget\": %" PRIu64 ",\n", c->writes_slow);
	{
		slowbands_t sb;

		slow_bands(c, &sb);
		fprintf(f, "    \"slow_bands\": %d,\n", sb.n);
	}
	fprintf(f, "    \"sectors_drilled\": %" PRIu64 ",\n", c->blocks_drilled);
	fprintf(f, "    \"sectors_recovered\": %" PRIu64 ",\n", c->blocks_recovered);
	fprintf(f, "    \"sectors_unstable\": %" PRIu64 ",\n", c->blocks_unstable);
	fprintf(f, "    \"sectors_slow\": %" PRIu64 ",\n", c->blocks_slow);
	fprintf(f, "    \"sectors_bad\": %" PRIu64 ",\n", c->blocks_bad);
	fprintf(f, "    \"sectors_corrupt\": %" PRIu64 ",\n", c->blocks_corrupt);
	fprintf(f, "    \"sectors_healed\": %" PRIu64 ",\n", c->blocks_fixed);
	fprintf(f, "    \"sectors_reassigned\": %" PRIu64 ",\n", c->sas_reassigned);
	fprintf(f, "    \"hard_errors\": %" PRIu64 "\n  },\n", c->hard_errors);
	fprintf(f, "  \"latency_ms\": {\n");
	fprintf(f, "    \"chunk\": {\"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, "
		"\"p999\": %.3f, \"max\": %.3f},\n",
		lat_pct(&c->chunk_lat, 50) / 1000.0, lat_pct(&c->chunk_lat, 95) / 1000.0,
		lat_pct(&c->chunk_lat, 99) / 1000.0, lat_pct(&c->chunk_lat, 99.9) / 1000.0,
		(double)c->chunk_lat.max_us / 1000.0);
	fprintf(f, "    \"sector\": {\"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, "
		"\"p999\": %.3f, \"max\": %.3f}\n  },\n",
		lat_pct(&c->block_lat, 50) / 1000.0, lat_pct(&c->block_lat, 95) / 1000.0,
		lat_pct(&c->block_lat, 99) / 1000.0, lat_pct(&c->block_lat, 99.9) / 1000.0,
		(double)c->block_lat.max_us / 1000.0);
	if (c->smart_before.have) {
		fprintf(f, "  \"smart\": {\n");
		fprintf(f, "    \"source\": \"%s\",\n",
			c->smart_before.ata ? "ata_attributes" :
			c->smart_before.sas ? "scsi_error_counter_log" : "none");
		fprintf(f, "    \"health\": \"");
		json_escape(f, c->smart_before.health);
		fprintf(f, "\",\n");
		fprintf(f, "    \"power_on_hours\": %lld,\n", c->smart_before.power_on_hours);
		fprintf(f, "    \"reallocated_before\": %lld, \"reallocated_after\": %lld,\n",
			c->smart_before.realloc_ct, c->smart_after.realloc_ct);
		fprintf(f, "    \"pending_before\": %lld, \"pending_after\": %lld,\n",
			c->smart_before.pending, c->smart_after.pending);
		fprintf(f, "    \"offline_uncorrectable_before\": %lld, "
			"\"offline_uncorrectable_after\": %lld,\n",
			c->smart_before.offline_uncorr, c->smart_after.offline_uncorr);
		fprintf(f, "    \"crc_errors\": %lld,\n", c->smart_after.crc_err);
		fprintf(f, "    \"temperature_max_c\": %lld\n  },\n", c->temp_max);
	}
	fprintf(f, "  \"findings\": [\n");
	for (i = 0; i < c->nfind; i++) {
		finding_t *x = &c->find[i];

		fprintf(f, "    {\"offset\": %" PRIu64 ", \"lba\": %" PRIu64
			", \"len\": %u, \"status\": \"%s\", \"attempts\": %u, "
			"\"errors\": %u, \"first_ms\": %.3f, \"median_ms\": %.3f, "
			"\"worst_ms\": %.3f, \"errno\": %d, \"rewritten\": %s, "
			"\"healed\": %s}%s\n",
			x->offset, x->offset / (uint64_t)c->dev->logical_bs, x->len,
			status_name(x->status), x->attempts, x->errors,
			(double)x->first_us / 1000.0, (double)x->med_us / 1000.0,
			(double)x->max_us / 1000.0, x->errnum,
			x->rewritten ? "true" : "false",
			x->fixed_by_rewrite ? "true" : "false",
			i + 1 < c->nfind ? "," : "");
	}
	fprintf(f, "  ],\n");
	fprintf(f, "  \"verdict\": \"%s\",\n",
		v == 0 ? "healthy" : v == 1 ? "suspect" : "failing");
	fprintf(f, "  \"verdict_reason\": \"");
	json_escape(f, why);
	fprintf(f, "\"\n}\n");
	fclose(f);
	msg(PROG ": wrote %s\n", path);
}

/*
 * badblocks(8) format: one block number per line, in units of blocksize, so
 * the list can be handed straight to 'mke2fs -l' or 'fsck -l' to build a
 * filesystem that steps around the damage.  Offsets here are device-relative;
 * mke2fs runs on a partition, so --badblocks-offset subtracts its start.
 */
static void write_badblocks(ctx_t *c, const char *path, uint64_t bs,
			    uint64_t part_off)
{
	FILE *f;
	int i;
	uint64_t n = 0;

	if (!bs)
		bs = 1024;
	f = fopen(path, "we");
	if (!f) {
		msg(PROG ": cannot write %s: %s\n", path, strerror(errno));
		return;
	}
	for (i = 0; i < c->nfind; i++) {
		finding_t *x = &c->find[i];
		uint64_t o, end;

		/* anything we would not trust a filesystem to live on */
		if (x->status != ST_BAD && x->status != ST_CORRUPT &&
		    x->status != ST_UNSTABLE)
			continue;
		if (x->offset < part_off)
			continue;
		end = x->offset - part_off + (x->len ? x->len : c->block);
		for (o = (x->offset - part_off) / bs * bs; o < end; o += bs) {
			fprintf(f, "%" PRIu64 "\n", o / bs);
			n++;
		}
	}
	fclose(f);
	msg(PROG ": wrote %s (%" PRIu64 " blocks of %" PRIu64 " bytes)\n",
	    path, n, bs);
}

/* ------------------------------------------------------------------ *
 * Prometheus textfile output, for node_exporter's textfile collector
 * ------------------------------------------------------------------ */

static void prom_label(FILE *f, const char *s)
{
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('_', f);
		else if ((unsigned char)*s < 0x20)
			fputc('_', f);
		else
			fputc(*s, f);
	}
}

static void write_prometheus(ctx_t *c, const char *path)
{
	char tmp[PATH_MAX];
	FILE *f;
	const char *why;
	int v = verdict_of(c, &why);
	double secs = (double)(c->t_end_us - c->t_start_us) / 1e6;
	int i;
	struct {
		const char *name, *help, *type;
		double val;
	} m[] = {
	{ "hddscan_verdict", "0 healthy, 1 suspect, 2 failing", "gauge", (double)v },
	{ "hddscan_complete", "1 if the whole requested range was tested", "gauge",
	  (!c->abort_reason[0] && c->step >= c->nchunks) ? 1 : 0 },
	{ "hddscan_bytes_tested", "Bytes read during the scan", "gauge",
	  (double)c->bytes_done },
	{ "hddscan_duration_seconds", "Wall clock duration of the scan", "gauge", secs },
	{ "hddscan_sectors_bad", "Sectors unreadable on every attempt", "gauge",
	  (double)c->blocks_bad },
	{ "hddscan_sectors_unstable", "Sectors intermittently slow or failing",
	  "gauge", (double)c->blocks_unstable },
	{ "hddscan_sectors_slow", "Sectors consistently over the latency budget",
	  "gauge", (double)c->blocks_slow },
	{ "hddscan_writes_slow", "Chunk writes over the latency budget",
	  "gauge", (double)c->writes_slow },
	{ "hddscan_sectors_recovered", "Sectors slow once but clean on retry",
	  "gauge", (double)c->blocks_recovered },
	{ "hddscan_sectors_corrupt", "Sectors returning data that did not match",
	  "gauge", (double)c->blocks_corrupt },
	{ "hddscan_sectors_healed", "Weak sectors that read clean after a rewrite",
	  "gauge", (double)c->blocks_fixed },
	{ "hddscan_firmware_reallocations", "Sectors the drive reallocated during "
	  "this scan, per SMART", "gauge", (double)c->realloc_delta },
	{ "hddscan_spares_exhausted", "1 if pending sectors did not clear after "
	  "being rewritten, suggesting the reserve pool is used up", "gauge",
	  (double)c->spares_exhausted },
	{ "hddscan_hard_errors", "Hard I/O errors seen during the scan", "gauge",
	  (double)c->hard_errors },
	{ "hddscan_latency_p50_ms", "Median chunk read latency", "gauge",
	  lat_pct(&c->chunk_lat, 50) / 1000.0 },
	{ "hddscan_latency_p99_ms", "99th percentile chunk read latency", "gauge",
	  lat_pct(&c->chunk_lat, 99) / 1000.0 },
	{ "hddscan_latency_p999_ms", "99.9th percentile chunk read latency",
	  "gauge", lat_pct(&c->chunk_lat, 99.9) / 1000.0 },
	{ "hddscan_latency_max_ms", "Slowest chunk read seen", "gauge",
	  (double)c->chunk_lat.max_us / 1000.0 },
	{ "hddscan_smart_reallocated", "SMART 5 after the scan", "gauge",
	  (double)c->smart_after.realloc_ct },
	{ "hddscan_smart_pending", "SMART 197 after the scan", "gauge",
	  (double)c->smart_after.pending },
	{ "hddscan_smart_offline_uncorrectable", "SMART 198 after the scan",
	  "gauge", (double)c->smart_after.offline_uncorr },
	{ "hddscan_smart_crc_errors", "SMART 199 after the scan", "gauge",
	  (double)c->smart_after.crc_err },
	{ "hddscan_smart_power_on_hours", "SMART 9", "gauge",
	  (double)c->smart_before.power_on_hours },
	{ "hddscan_temperature_celsius", "Highest drive temperature during the scan",
	  "gauge", (double)c->temp_max },
	{ "hddscan_last_run_timestamp_seconds", "When this scan finished", "gauge",
	  (double)time(NULL) },
	};

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "we");
	if (!f) {
		msg(PROG ": cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}
	for (i = 0; i < (int)(sizeof(m) / sizeof(m[0])); i++) {
		fprintf(f, "# HELP %s %s\n# TYPE %s %s\n",
			m[i].name, m[i].help, m[i].name, m[i].type);
		fprintf(f, "%s{device=\"", m[i].name);
		prom_label(f, c->dev->name);
		fprintf(f, "\",serial=\"");
		prom_label(f, c->dev->serial);
		fprintf(f, "\",model=\"");
		prom_label(f, c->dev->model);
		fprintf(f, "\"} %.6g\n", m[i].val);
	}
	fclose(f);
	/* rename so node_exporter never reads a half-written file */
	if (rename(tmp, path) < 0)
		msg(PROG ": cannot rename %s: %s\n", tmp, strerror(errno));
	else
		msg(PROG ": wrote %s\n", path);
}

static void write_csv(ctx_t *c, const char *path)
{
	FILE *f = fopen(path, "we");
	int i;

	if (!f) {
		msg(PROG ": cannot write %s: %s\n", path, strerror(errno));
		return;
	}
	fprintf(f, "device,offset_bytes,lba,length,status,attempts,errors,"
		"first_ms,min_ms,median_ms,worst_ms,errno,errstr,rewritten,healed\n");
	for (i = 0; i < c->nfind; i++) {
		finding_t *x = &c->find[i];

		fprintf(f, "%s,%" PRIu64 ",%" PRIu64 ",%u,%s,%u,%u,%.3f,%.3f,%.3f,"
			"%.3f,%d,%s,%d,%d\n",
			c->dev->name, x->offset,
			x->offset / (uint64_t)c->dev->logical_bs, x->len,
			status_name(x->status), x->attempts, x->errors,
			(double)x->first_us / 1000.0, (double)x->min_us / 1000.0,
			(double)x->med_us / 1000.0, (double)x->max_us / 1000.0,
			x->errnum, errname(x->errnum), x->rewritten,
			x->fixed_by_rewrite);
	}
	fclose(f);
	msg(PROG ": wrote %s\n", path);
}

/* ------------------------------------------------------------------ *
 * signals
 * ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	if (g_stop) {
		g_stop_hard = 1;
		/* second Ctrl-C: give up on a graceful report */
		if (g_ctx && g_ctx->restore_pending && g_ctx->restore_buf) {
			ssize_t w = pwrite(g_ctx->fd, g_ctx->restore_buf,
					   g_ctx->restore_len,
					   (off_t)g_ctx->restore_off);
			(void)w;
		}
		timeout_restore();
		lookahead_restore();
		field_restore_all();
		_exit(130);
	}
	g_stop = 1;
}

/* ------------------------------------------------------------------ *
 * options
 * ------------------------------------------------------------------ */

typedef struct {
	scan_mode_t mode;
	int list_only;
	int tui;
	int scan_all;
	int include_ssd;
	int include_remote;
	int sequential;
	int max_parallel;
	int force;
	int no_direct;
	int write_cache;        /* -1 keep, 0 off, 1 on */
	int persist;            /* save changed settings to the drive */
	int fix_config;         /* apply the configuration the report advises */
	int apply_only;         /* apply the settings and stop, do not scan */
	int format;             /* low level format instead of a scan */
	int fmt_bs;             /* 0 keep, else the logical block size to set */
	int fmt_pi;             /* -1 keep what the drive has, else 0..3 */
	int fmt_fast;           /* FFMT: do not visit every block */
	int awre;               /* -1 keep, 0 off, 1 on */
	int arre;               /* -1 keep, 0 off, 1 on */
	int read_cache;         /* -1 keep, 0 off, 1 on (RCD is the inverse) */
	long long recovery_ms;  /* -1 keep, else the RTL to set */
	long long rd_retries;   /* -1 keep, else the RRC to set */
	long long wr_retries;   /* -1 keep, else the WRC to set */
	int lookahead_set;      /* --drive-lookahead was named, not defaulted */
	int read_cache_set;     /* --read-cache was named, not defaulted */
	int rewrite_weak;
	int force_remap;
	int second_pass;
	int repair;             /* the whole repair sequence in one flag */
	const char *profile;    /* which profile produced these defaults */
	int retries;
	double retry_cap_s;
	int io_timeout;
	int lookahead;          /* -1 keep, 0 disable for the run */
	int bms;                /* 0 keep, 1 turn the background scan on */
	int map_width;
	int no_smart;
	int dry_run;
	uint64_t chunk, block;
	uint64_t start, end;
	uint64_t sample;
	order_t order;
	uint64_t segment;
	uint64_t max_time;
	uint64_t max_errors;
	double chunk_ms, block_ms;
	double auto_factor;
	double floor_ms;
	double min_rate;        /* MiB/s at RATE_REF_CHUNK chunks */
	long long max_temp;
	uint64_t seed;
	const char *confirm;
	const char *json;
	const char *csv;
	const char *badblocks;
	uint64_t bb_blocksize;
	uint64_t bb_offset;
	const char *prometheus;
	const char *journal;
	const char *state;
	int resume;
	const char *outdir;
	/* runs: starting them detached, and looking at the ones already going */
	int detach;             /* start the run and return to the shell */
	int status;             /* print what is running and stop */
	int status_json;
	int attach;             /* open the dashboard on a run already going */
	const char *runarg;     /* a run id, a prefix of one, or a drive name */
	const char *stop;
	const char *forget;
} opts_t;

/* ------------------------------------------------------------------ *
 * profiles
 *
 * What a drive needs tested depends entirely on what you are about to do with
 * it, and that decision drives half a dozen options at once.  A profile is
 * that decision, named -- so the form asks the question a human actually has
 * an answer to ("is there data on this drive?") instead of six questions they
 * have to derive it from.
 *
 * The default is predeploy, because a surface test is nearly always run before
 * a drive goes into service and a read-only pass cannot tell you whether a
 * sector will accept a write.  That means the default mode writes, so a bare
 * invocation stops and asks for --confirm rather than doing anything: an
 * error, never data loss.  Reach for 'inservice' on a drive that holds
 * anything you want to keep.
 * ------------------------------------------------------------------ */

typedef struct {
	const char *name;
	const char *what;
	scan_mode_t mode;
	int rewrite_weak;
	int force_remap;
	int second_pass;
	uint64_t sample;
	size_t chunk;
} profile_t;

static const profile_t g_profiles[] = {
	{ "predeploy",
	  "no data on it yet: write every sector and verify it reads back",
	  MODE_WRITE, 1, 0, 1, 0, 0 },
	{ "inservice",
	  "it holds data you want to keep: never writes anything",
	  MODE_READ,  0, 0, 0, 0, 0 },
	{ "survey",
	  "quick triage of a shelf: samples the surface, read only",
	  MODE_READ,  0, 0, 0, 64, 1024u * 1024 },
	{ "decay",
	  "weeks after a predeploy run: has the pattern rotted?",
	  MODE_CHECK, 0, 0, 0, 0, 0 },
	{ "repair",
	  "predeploy, plus forcing a reallocation of anything unreadable",
	  MODE_WRITE, 1, 1, 1, 0, 0 },
};
#define NPROFILES ((int)(sizeof(g_profiles) / sizeof(g_profiles[0])))

static const profile_t *profile_by_name(const char *n)
{
	int i;

	for (i = 0; i < NPROFILES; i++)
		if (!strcmp(g_profiles[i].name, n))
			return &g_profiles[i];
	return NULL;
}

static void usage(void)
{
	printf(
"%s %s - per-sector latency and error scanner for rotating disks\n"
"\n"
"Usage: %s [options] [device ...]\n"
"       %s --list\n"
"       %s --status [RUN|DRIVE]\n"
"\n"
"Run on a terminal with no device named, hddscan opens the interactive\n"
"picker; --no-tui turns that off.  Devices may be given as /dev/sdb, sdb, or\n"
"a regular file to use as an image.  With no device and --all, every\n"
"unmounted rotational disk is scanned.\n"
"\n"
"Selection\n"
"  --profile NAME         what you are about to do with the drive, which sets\n"
"                         the mode and the passes that go with it.  Default\n"
"                         'predeploy', so a bare run stops and asks for\n"
"                         --confirm rather than doing anything:\n"
"                           predeploy  no data on it yet: write every sector\n"
"                                      and verify it reads back\n"
"                           inservice  it holds data you want to keep: never\n"
"                                      writes anything\n"
"                           survey     quick triage of a shelf: samples the\n"
"                                      surface, read only\n"
"                           decay      weeks after predeploy: has it rotted?\n"
"                           repair     predeploy, plus forcing a reallocation\n"
"                                      of anything unreadable\n"
"                         Any explicit flag overrides what the profile set,\n"
"                         whichever order they appear in\n"
"  -i, --tui              interactive terminal UI: pick drives and settings on\n"
"                         one form, then watch a live dashboard while it runs.\n"
"                         A summary holds the verdicts when the scan finishes;\n"
"                         from there n starts another test and q quits.\n"
"                         Implied when nothing to scan was named and stdin and\n"
"                         stdout are a terminal; pass it to get the form even\n"
"                         with devices named, which arrive pre-selected\n"
"      --no-tui           never open the UI, whatever the terminal looks like\n"
"  --check-deps           list the optional tools hddscan can use, whether\n"
"                         each is installed, and the dnf/apt line that\n"
"                         installs what is missing, then exit\n"
"  --list                 show detected drives, their health-relevant facts\n"
"                         and whether they are safe to test, then exit\n"
"  --all                  scan every detected rotational, unmounted disk\n"
"  --include-ssd          allow non-rotational devices (thresholds will be off)\n"
"  --include-remote       allow iSCSI/virtio/FC/Hyper-V disks.  These report\n"
"                         themselves as rotational but you would be measuring\n"
"                         a network and someone else's cache, not a platter\n"
"\n"
"Parallelism\n"
"  Several drives are always scanned at the same time, one worker process per\n"
"  drive, because separate spindles are genuinely independent.  Work is never\n"
"  parallelised inside a single drive: a second outstanding request makes the\n"
"  head seek away and you would be timing the queue, not the platter.\n"
"  --max-parallel N       cap concurrent drives.  Useful when the HBA, expander\n"
"                         or PSU cannot feed every drive at full speed, which\n"
"                         would otherwise show up as inflated latency\n"
"  --sequential           one drive at a time, full report straight to stdout\n"
"\n"
"Runs that outlive the terminal\n"
"  A scan or a format is a run: it belongs to a supervisor process of its own,\n"
"  not to the window that started it, and every drive in it keeps a small\n"
"  record of where it has got to.  Quit, close the terminal or lose the ssh\n"
"  session and the work carries on; come back with --status or --attach.\n"
"  Records live under /var/lib/hddscan as root, otherwise under\n"
"  ~/.local/state/hddscan.  On the dashboard, q leaves the run going, d goes\n"
"  back to the list of runs and x stops it.\n"
"  --detach               start the run and return to the shell, printing its\n"
"                         id.  Nothing is watched and nothing is waited for\n"
"  --status [RUN|DRIVE]   what every run on this machine is doing, as text.\n"
"                         With a run id (or a prefix of one, or the name of a\n"
"                         drive in it), the drives in that one run\n"
"  --status-json [RUN|DRIVE]   the same thing as JSON, for a script\n"
"  --attach [RUN|DRIVE]   open the dashboard on a run already going.  With no\n"
"                         argument, the list of runs to choose from\n"
"  --stop RUN|all         ask a run to stop.  Each drive still writes the\n"
"                         report for the part of it that was covered\n"
"  --forget RUN|finished  drop the records of a finished run.  Never touches a\n"
"                         live one, and never touches the reports themselves\n"
"  --state-dir DIR        keep run records here instead of the default\n"
"                         (HDDSCAN_STATE_DIR does the same thing)\n"
"\n"
"Test mode\n"
"  --mode read            read-only surface scan (default, safe)\n"
"  --mode verify          non-destructive write test: save sector, write\n"
"                         pattern, read back, restore.  DATA LOSS ON POWER CUT\n"
"  --mode write           destructive: overwrite everything with a pattern\n"
"                         and verify it reads back correctly\n"
"  --mode check           read-only re-verify of a pattern an earlier\n"
"                         --mode write run left behind.  Run it days or weeks\n"
"                         later to catch sectors that lost their charge\n"
"  --confirm STRING       required for verify/write; must equal the device\n"
"                         name (e.g. sdb) or its serial number\n"
"  --repair               give a drive its best chance, in one flag: overwrite\n"
"                         every sector, which is what refreshes a weak one;\n"
"                         rewrite anything still slow after that; force a\n"
"                         reallocation on anything that will not read at all;\n"
"                         then re-read the whole device to prove it held.\n"
"                         Implies --mode write and so DESTROYS ALL DATA and\n"
"                         needs --confirm.  Also turns the drive's read cache\n"
"                         off, since a repair verified out of the drive's own\n"
"                         DRAM is not a repair you verified\n"
"  --second-pass          after --mode write, re-read the whole device and\n"
"                         re-verify the pattern (catches slow data decay)\n"
"  --rewrite-weak         rewrite readable-but-slow sectors in place to make\n"
"                         the drive refresh or remap them (needs verify/write,\n"
"                         or read mode plus this flag for read-modify-write)\n"
"  --force-remap          overwrite UNREADABLE sectors with zeros to force a\n"
"                         reallocation.  Destroys the contents of those sectors\n"
"\n"
"Range and sizing\n"
"  --start SIZE           first byte to test (default 0)\n"
"  --end SIZE             last byte to test (default end of device)\n"
"  --chunk SIZE           bulk read size, default 128K\n"
"  --block SIZE           drill-down granularity, default 4K\n"
"  --sample N             only test every Nth chunk (fast surface survey)\n"
"  --order ORDER          sequential (default), reverse, or random.  Sequential\n"
"                         reads at full streaming speed, but the drive is\n"
"                         prefetching ahead of you, so a merely marginal sector\n"
"                         can be served from its buffer and look healthy.\n"
"                         random defeats that entirely and is far slower;\n"
"                         combine it with --sample for a thorough spot check\n"
"  --max-time SECONDS     stop after this long and report what was covered\n"
"  --max-errors N         stop after N hard I/O errors (default: never)\n"
"\n"
"Latency budget\n"
"  --chunk-slow-ms MS     chunk budget; over this we drill into sectors\n"
"  --sector-slow-ms MS    sector budget; over this we retry the sector\n"
"  --auto-factor F        auto budget = F x measured median (default 3)\n"
"  --floor-ms MS          auto budget never goes below this (default 25)\n"
"  --min-rate MIB         a hard drive testing slower than this many MiB/s at\n"
"                         4 MiB chunks is failing; scaled down for smaller\n"
"                         chunks.  Judged after 2 minutes; 0 turns it off\n"
"                         (default 10)\n"
"  --retries N            re-reads of a suspicious sector (default 20).  A\n"
"                         sector that is merely weak often reads back fine\n"
"                         after a few tries; that is the drive recovering\n"
"  --retry-max-seconds S  give up retrying one sector after this long\n"
"                         (default 60, 0 = no limit).  Without it a drive\n"
"                         whose every read hits the SCSI timeout would take\n"
"                         minutes per sector and never finish\n"
"\n"
"Safety and environment\n"
"  --force                test even if the drive looks mounted or in use\n"
"  --drive-lookahead off|keep\n"
"                         'off' (the default) tells the drive to stop\n"
"                         prefetching into its own DRAM for the duration of\n"
"                         the scan, so a sequential pass cannot be handed a\n"
"                         marginal sector at cache speed.  Restored on exit.\n"
"                         needs hdparm on ATA/SATA, sdparm on SAS/SCSI;\n"
"                         without the right one the scan says so and runs\n"
"\n"
"Low level format\n"
"  --format               FORMAT UNIT every selected drive, in parallel. This\n"
"                         ERASES THE DRIVE, runs for hours and cannot be\n"
"                         undone; a power cut partway through can leave a\n"
"                         drive needing vendor tooling. Needs --confirm and\n"
"                         sg_format.  The exact command is printed per drive\n"
"                         before anything starts\n"
"  --format-blocksize 512|4096|keep\n"
"                         logical block size to format to (default: keep)\n"
"  --format-fast          ask for a fast format: the drive does not visit\n"
"                         every block, which can turn hours into minutes.\n"
"                         Only meaningful when the physical sector layout is\n"
"                         not changing -- a 512e drive is already 4096 bytes\n"
"                         per physical sector, so moving the logical size to\n"
"                         4096 is a remapping.  Unwritten blocks read without\n"
"                         error afterwards.  Drives may refuse it, at no cost\n"
"  --format-pi none|keep|0..3\n"
"                         protection information to format with.  'keep'\n"
"                         preserves what the drive already reports, and is\n"
"                         the default because sg_format itself defaults to\n"
"                         stripping it -- changing block size would otherwise\n"
"                         silently remove protection as a side effect\n"
"\n"
"Drive settings\n"
"  --apply-settings       apply the settings below to every selected drive and\n"
"                         stop, without scanning anything.  Settings are\n"
"                         applied before the scan either way, so a drive is\n"
"                         configured the moment you ask rather than when a\n"
"                         scan happens to reach it\n"
"  --fix-config           apply the configuration this tool recommends for a\n"
"                         drive going into service: auto-reallocate on for\n"
"                         both reads and writes, read cache on.  Only the\n"
"                         parts actually wrong are touched, each is printed,\n"
"                         and they are SAVED -- a fix that reverted when the\n"
"                         scan ended would not be a fix\n"
"  --awre on|off|keep     let the drive retire a sector it struggles to write\n"
"  --arre on|off|keep     the same on read.  With both off the firmware never\n"
"                         reallocates, so --rewrite-weak and --force-remap\n"
"                         rewrite a bad sector straight back onto itself\n"
"  --read-cache on|off|keep\n"
"                         'off' stops the drive serving any read from its own\n"
"                         DRAM, which is stricter than --drive-lookahead off.\n"
"                         Leaving it off on a drive in service makes reads\n"
"                         crawl; this is the other half of the classic\n"
"                         ex-array 'writes fast, reads like treacle' fault\n"
"  --drive-read-retries N bound how many times the drive re-reads a sector\n"
"                         internally before reporting it (SCSI RRC, 0..255).\n"
"                         The count-based twin of --recovery-time; firmware\n"
"                         differs in which of the two it actually honours, so\n"
"                         setting both is the reliable way to make a drive\n"
"                         fail fast.  Not to be confused with --retries, which\n"
"                         is how many times *this tool* re-reads a sector\n"
"  --drive-write-retries N   the same for writes (SCSI WRC)\n"
"  --recovery-time MS     bound how long the drive fights one bad sector\n"
"                         before reporting it (SCSI RTL; 0 means unlimited,\n"
"                         which is what makes a dying disk stall for minutes\n"
"                         and a ZFS or RAID member get faulted).  SAS only\n"
"  --persist              save the settings this run changes to the drive so\n"
"                         they survive a power cycle, instead of putting them\n"
"                         back when the scan ends.  Needs a SAS/SCSI drive and\n"
"                         sdparm; anything else is applied for the run only and\n"
"                         says so.  Only settings you named are saved -- the\n"
"                         look-ahead default is not, or every scanned drive\n"
"                         would be left permanently slower\n"
"  --write-cache on|off|keep\n"
"                         with the cache on a write returns as soon as the\n"
"                         drive has it in DRAM, so a write mode times the\n"
"                         cache and not the platter; 'off' makes it honest\n"
"                         and much slower.  Also the other half of the\n"
"                         ex-array fault where RCD and WCE together give a\n"
"                         drive that writes fast and reads like treacle.\n"
"                         Restored on exit.  Default 'keep' touches nothing\n"
"  --bms on|keep          'on' tells every selected SAS/SCSI drive to run its\n"
"                         own background medium scan: the firmware sweeps the\n"
"                         platters whenever the drive is idle, logs what it\n"
"                         finds and reallocates the sectors it can still\n"
"                         recover.  Unlike every other setting here this one\n"
"                         is SAVED ON THE DRIVE and is not undone afterwards;\n"
"                         'sdparm --clear=EN_BMS --save /dev/sdX' turns it\n"
"                         back off.  Default 'keep' touches nothing\n"
"  --io-timeout SEC       lower the kernel's per-command timeout during the\n"
"                         scan so a dying sector fails fast (restored after)\n"
"  --max-temp C           abort if the drive gets this hot (default 58, 0=off)\n"
"  --no-smart             do not call smartctl\n"
"  --no-direct            do not use O_DIRECT (results will include cache hits)\n"
"  --dry-run              show what would be done, touch nothing\n"
"\n"
"Output\n"
"  --json FILE            write a machine readable report\n"
"  --badblocks-list FILE  write bad sectors in badblocks(8) format, ready for\n"
"                         'mke2fs -l FILE' so a new filesystem steps around\n"
"                         them.  Includes bad, corrupt and unstable sectors\n"
"  --badblocks-blocksize N   unit for that list (default 1024)\n"
"  --badblocks-offset N   subtract a partition's start offset from the block\n"
"                         numbers, since mke2fs runs on the partition\n"
"  --prometheus FILE      write metrics for node_exporter's textfile collector\n"
"  --journal FILE         crash journal for --mode verify: the original bytes\n"
"                         are fsynced here before the pattern is written, so a\n"
"                         power cut mid-chunk can be undone by the next run\n"
"  --csv FILE             write every suspect sector as CSV\n"
"  --log FILE             tee the report to a file\n"
"  --state FILE           checkpoint progress here every 15s.  With several\n"
"                         drives the name gets the device appended, one file\n"
"                         each; without it every drive in a run checkpoints\n"
"                         inside the run's own directory anyway\n"
"  --resume               continue from the checkpoint in --state\n"
"  --outdir DIR           where per-device reports go with --parallel\n"
"  --map-width N          width of the surface map (default 64)\n"
"  --quiet                no progress output\n"
"  --no-color             plain text\n"
"  -h, --help             this text\n"
"\n"
"Exit status: 0 healthy, 1 suspect, 2 failing or usage error, 130 interrupted.\n",
	PROG, VERSION, PROG, PROG, PROG);
}

/* ------------------------------------------------------------------ *
 * listing
 * ------------------------------------------------------------------ */

static void list_devices(int no_smart)
{
	device_t *v;
	int n, i;
	char b[32];

	n = enumerate_devices(&v);
	printf("%-8s %-10s %-24s %-18s %-9s %-12s %s\n",
	       "DEVICE", "SIZE", "MODEL", "SERIAL", "MEDIA", "TRANSPORT", "STATUS");
	for (i = 0; i < n; i++) {
		device_t *d = &v[i];
		smart_t s;
		int testable;

		safety_check(d);
		if (!no_smart && !smart_read(d, &s) && !d->serial[0] && s.serial[0])
			snprintf(d->serial, sizeof(d->serial), "%s", s.serial);
		testable = d->rotational && transport_is_local(d->transport) && !d->unsafe;
		printf("%-8s %-10s %-24.24s %-18.18s %-9s %-12s %s%s%s\n",
		       d->name, human_size(d->size, b, sizeof(b)),
		       d->model[0] ? d->model : "?",
		       d->serial[0] ? d->serial : "?",
		       d->zoned ? "HDD-SMR" : d->rotational ? "HDD" : "SSD/flash",
		       transport_name(d->transport),
		       d->unsafe ? c_red() : testable ? c_grn() : c_yel(),
		       d->unsafe ? "IN USE" : d->no_access ? "need root" :
		       testable ? "ready" : "skipped",
		       c_off());
		if (d->unsafe)
			printf("         %s\n", d->unsafe_why);
		else if (d->no_access)
			printf("         cannot open %s; re-run as root to see whether "
			       "it is free\n", d->path);
		else if (!d->rotational)
			printf("         not a rotating disk; needs --include-ssd\n");
		else if (!transport_is_local(d->transport))
			printf("         %s disk: latency reflects the link and the "
			       "remote storage, not this drive's mechanics; "
			       "needs --include-remote\n", transport_name(d->transport));
		if (d->prot_type > 0)
			printf("         formatted with type %d protection information: "
			       "a read-only scan can only\n         reach the blocks "
			       "something has already written\n", d->prot_type);
	}
	free(v);
}

/* ------------------------------------------------------------------ *
 * running one device
 * ------------------------------------------------------------------ */

static int open_device(device_t *d, scan_mode_t mode, int writable, int force,
		       int no_direct)
{
	int flags = writable ? O_RDWR : O_RDONLY;
	int fd;

	flags |= O_CLOEXEC;
	if (!force && !d->is_file)
		flags |= O_EXCL;
	if (!no_direct)
		flags |= O_DIRECT;
	if (writable)
		flags |= O_DSYNC;   /* make writes reach the platter, not the cache */

	fd = open(d->path, flags);
	if (fd < 0 && errno == EINVAL && !no_direct) {
		msg(PROG ": %s: O_DIRECT unavailable, falling back "
		    "(latency numbers may include cache hits)\n", d->name);
		flags &= ~O_DIRECT;
		fd = open(d->path, flags);
	}
	return fd;
}

static int scan_device(device_t *d, const opts_t *o, const char *json_path,
		       const char *csv_path, const char *bb_path,
		       const char *prom_path)
{
	ctx_t c;
	char b1[32], b2[32];
	uint64_t median = 0;
	calib_t cal;
	const char *why;
	int v;
	int save_la;

	memset(&c, 0, sizeof(c));
	lat_init(&c.chunk_lat);
	lat_init(&c.block_lat);
	lat_init(&c.write_lat);
	c.dev = d;
	c.fd = -1;
	c.mode = o->mode;
	c.chunk = (size_t)o->chunk;
	c.block = (size_t)o->block;
	c.retries = o->retries;
	c.retry_cap_us = (uint64_t)(o->retry_cap_s * 1000000.0);
	c.auto_factor = o->auto_factor;
	/*
	 * The floor is an absolute tolerance for a stall, quoted for the
	 * default chunk.  A larger chunk legitimately takes longer just to
	 * move the data, so allow for that -- a fixed 25 ms against a 1 MiB
	 * read that honestly costs 22 ms puts every healthy chunk within a
	 * whisker of the budget and lets drill-down take over the scan.
	 *
	 * The allowance is transfer time at a deliberately pessimistic
	 * 50 MB/s, not a multiple of the floor: scaling the floor itself
	 * would also loosen a budget that calibration had measured properly,
	 * which is the opposite of what a floor is for.
	 */
	c.floor_us = (uint64_t)(o->floor_ms * 1000.0);
	if (c.chunk > DEF_CHUNK)
		c.floor_us += ((uint64_t)c.chunk - DEF_CHUNK) / 50;
	c.sample = o->sample;
	c.order = o->order;
	c.rewrite_weak = o->rewrite_weak;
	c.force_remap = o->force_remap;
	c.second_pass = o->second_pass;
	c.profile = o->profile;
	c.seed = o->seed;
	c.max_time = o->max_time;
	c.max_errors = o->max_errors;
	c.max_temp = o->max_temp;
	c.no_direct = o->no_direct;
	c.map_width = o->map_width;
	c.state_path = o->state;
	c.resume = o->resume;
	c.temp_min = -1;
	c.temp_max = -1;
	c.jrn_fd = -1;
	g_ctx = &c;

	/* alignment rules for O_DIRECT */
	if (c.block % (size_t)d->logical_bs)
		die("%s: block size %zu is not a multiple of the sector size %d",
		    d->name, c.block, d->logical_bs);
	if (c.chunk % c.block)
		die("%s: chunk size %zu is not a multiple of the block size %zu",
		    d->name, c.chunk, c.block);

	c.start = o->start;
	c.end = o->end ? o->end : d->size;
	if (c.end > d->size)
		c.end = d->size;
	c.start -= c.start % (uint64_t)d->logical_bs;
	c.end -= c.end % (uint64_t)d->logical_bs;
	if (c.start >= c.end)
		die("%s: empty test range", d->name);
	c.pos = c.start;
	order_setup(&c, o->segment);

	if (!o->no_smart)
		smart_read(d, &c.smart_before);
	if (c.smart_before.have && c.smart_before.temp_c > 0) {
		c.temp_min = c.smart_before.temp_c;
		c.temp_max = c.smart_before.temp_c;
	}
	if (!d->serial[0] && c.smart_before.serial[0])
		snprintf(d->serial, sizeof(d->serial), "%s", c.smart_before.serial);

	out("%s: %s, %s, %s\n", d->path,
	    d->model[0] ? d->model : "unknown model",
	    human_size(d->size, b1, sizeof(b1)),
	    d->rotational ? "rotational" : "NON-ROTATIONAL");

	if (o->dry_run) {
		out("  dry run: would test %s in %s mode, %zu KiB chunks, "
		    "%zu KiB blocks, %d retries\n",
		    human_size(c.end - c.start, b1, sizeof(b1)),
		    mode_name(c.mode), c.chunk / 1024, c.block / 1024, c.retries);
		return 0;
	}

	c.fd = open_device(d, c.mode, mode_writes(c.mode) || o->rewrite_weak ||
			   o->force_remap, o->force, o->no_direct);
	if (c.fd < 0) {
		msg(PROG ": %s: cannot open: %s%s\n", d->path, strerror(errno),
		    errno == EBUSY ? " (device is in use; --force overrides)" : "");
		return 2;
	}

	if (!d->is_file) {
		uint64_t sz = 0;
		int bs = 0;

		if (!ioctl(c.fd, BLKGETSIZE64, &sz) && sz)
			d->size = sz;
		if (!ioctl(c.fd, BLKSSZGET, &bs) && bs > 0)
			d->logical_bs = bs;
		if (c.end > d->size)
			c.end = d->size;
	}

	if (o->journal && c.mode == MODE_VERIFY) {
		if (jrn_replay(o->journal, d, c.fd) < 0) {
			close(c.fd);
			return 2;
		}
		c.jrn_fd = jrn_open(o->journal);
	} else if (o->journal) {
		msg(PROG ": --journal only applies to --mode verify; ignoring\n");
	}

	if (o->io_timeout > 0)
		timeout_set(d, o->io_timeout);
	c.awre_state = c.arre_state = -1;
	c.rtl_state = -1;
	c.rrc_state = -1;
	c.rcd_state = -1;
	power_probe(d, &c.pm_bg, &c.sleep_timers);
	if (!d->is_file && d->transport == TR_SCSI && have_sdparm()) {
		c.awre_state = (int)sd_field_get(d->name, "AWRE");
		c.arre_state = (int)sd_field_get(d->name, "ARRE");
		c.rtl_state = sd_field_get(d->name, "RTL");
		c.rrc_state = sd_field_get(d->name, "RRC");
		c.rcd_state = sd_field_get(d->name, "RCD");
	}

	/*
	 * --persist saves what the user asked for, never what merely defaulted.
	 * Look-ahead off is this tool's default, so a bare '--persist' would
	 * otherwise leave every scanned drive permanently slower at sequential
	 * reads -- a change nobody asked for, on hardware that is probably
	 * going back into service.
	 */
	save_la = o->persist && o->lookahead_set;
	if (o->lookahead == 0)
		c.la_disabled = (lookahead_disable(d, save_la) == 0);
	/*
	 * Before the first read, never during -- see the note on bms_enable().
	 * The state is read back afterwards either way, so the report says what
	 * the drive is actually doing rather than what we asked for.
	 */
	c.wc_state = -1;
	if (!d->is_file && (d->transport == TR_ATA || d->transport == TR_SCSI))
		c.wc_state = wc_get(d->name, d->transport == TR_SCSI);

	c.bms_state = -1;
	if (!d->is_file && d->transport == TR_SCSI)
		c.bms_state = bms_get(d->name);
	/*
	 * If the drive keeps prefetching, a sequential pass can be handed a
	 * marginal sector out of the drive's buffer and never see the stall.
	 * Say so rather than reporting a clean scan we cannot stand behind.
	 */
	if (!c.la_disabled && !d->is_file && c.order != ORD_RANDOM)
		msg(PROG ": %s: %sthe drive is still prefetching%s; a sequential "
		    "scan can miss a marginal sector that it served from cache. "
		    "Use --order random for a result that does not depend on "
		    "this.\n", d->name, c_yel(), c_off());

	/*
	 * A read-only pass over a PI-formatted drive tests only the blocks
	 * something has already written; the rest refuse the read outright.
	 * Better to say that at the start than to hand back a report whose
	 * coverage is a fraction of the device for reasons nobody expected.
	 */
	if (d->prot_type > 0 && !mode_writes(c.mode))
		msg(PROG ": %s: %sformatted with type %d protection information%s; "
		    "any block never written since that format will refuse to be "
		    "read (guard tag check) and will be skipped, not tested\n",
		    d->name, c_yel(), d->prot_type, c_off());

	if (c.resume && c.state_path && !state_load(&c)) {
		if (c.pos > c.start && c.pos < c.end)
			out("  resuming at %s (%.1f%%)\n",
			    human_size(c.pos, b1, sizeof(b1)),
			    100.0 * (double)(c.pos - c.start) / (double)(c.end - c.start));
	}

	/* thresholds */
	c.auto_thr = (o->chunk_ms <= 0 || o->block_ms <= 0);
	{
		void *cb = alloc_aligned(c.chunk);

		calibrate(&c, cb, &cal);
		free(cb);
		median = cal.typical_us;
	}
	if (o->chunk_ms > 0) {
		c.chunk_thr_us = (uint64_t)(o->chunk_ms * 1000.0);
	} else {
		c.chunk_thr_us = median ? (uint64_t)((double)median * c.auto_factor) : 0;
		if (c.chunk_thr_us < c.floor_us)
			c.chunk_thr_us = c.floor_us;
	}
	if (o->block_ms > 0)
		c.block_thr_us = (uint64_t)(o->block_ms * 1000.0);
	else
		c.block_thr_us = c.chunk_thr_us;
	/*
	 * A chunk is never faster than its slowest sector, so the chunk budget
	 * has to be at least as tight as the sector budget or slow sectors
	 * would hide inside chunks we never drill into.
	 */
	/*
	 * Let the budget follow the platter, but only where it was measured
	 * rather than dictated: an explicit --chunk-slow-ms or --sector-slow-ms
	 * means that number everywhere, not a number this tool then bends.
	 */
	if (cal.valid && cal.nanchor >= 2 && cal.seq_us && median) {
		int k;

		for (k = 0; k < cal.nanchor; k++) {
			c.grad_off[k] = cal.anchor_off[k];
			c.grad_us[k] = cal.anchor_us[k];
		}
		c.grad_n = cal.nanchor;
		c.grad_mid = cal.seq_us;
		c.grad_typ = median;
		c.grad_budget = !o->chunk_ms && !o->block_ms;
	}

	if (c.chunk_thr_us > c.block_thr_us) {
		msg(PROG ": %s: chunk budget raised above the sector budget would "
		    "hide slow sectors; clamping chunk budget to %.1f ms\n",
		    d->name, (double)c.block_thr_us / 1000.0);
		c.chunk_thr_us = c.block_thr_us;
	}

	if (median) {
		out("  calibrated: median %zu KiB read %.2f ms -> budget %.1f ms "
		    "chunk / %.1f ms sector\n", c.chunk / 1024,
		    (double)median / 1000.0, (double)c.chunk_thr_us / 1000.0,
		    (double)c.block_thr_us / 1000.0);
		if (c.grad_budget)
			out("  the budget follows the platter: %.2f ms per chunk "
			    "at the outer edge,\n  %.2f ms at the inner, so the "
			    "same margin applies everywhere\n",
			    (double)expected_at(&c, c.grad_off[0]) / 1000.0,
			    (double)expected_at(&c, c.grad_off[c.grad_n - 1]) / 1000.0);
	} else {
		/*
		 * Calibration read nothing it could time -- on a PI-formatted
		 * drive every anchor can land in blocks that refuse to be
		 * read.  The budget below is a fallback, not a measurement of
		 * this drive, and saying otherwise would make a HEALTHY here
		 * mean less than it looks.
		 */
		c.calib_failed = 1;
		msg(PROG ": %s: %scould not time this drive%s: no calibration "
		    "read succeeded%s. The budget below is a fallback, not a "
		    "measurement of this drive.\n", d->name, c_yel(), c_off(),
		    d->prot_type > 0 ? " (its protection information refused "
		    "them)" : "");
		out("  budget: %.1f ms chunk / %.1f ms sector (fallback, "
		    "not calibrated)\n",
		    (double)c.chunk_thr_us / 1000.0, (double)c.block_thr_us / 1000.0);
	}

	if (c.chunk > DEF_CHUNK)
		out("  %sat %zu KiB chunks a sector has to exceed %.1f ms to be "
		    "noticed%s;\n  the %u KiB default is a tighter bar. "
		    "--sector-slow-ms pins it back,\n  at the cost of drilling "
		    "into more chunks.\n", c_yel(), c.chunk / 1024,
		    (double)c.block_thr_us / 1000.0, c_off(), DEF_CHUNK / 1024);

	out("  testing %s from offset %s%s, %s order\n",
	    human_size(c.end - c.pos, b1, sizeof(b1)),
	    human_size(c.pos, b2, sizeof(b2)),
	    c.sample > 1 ? " (sampled)" : "", order_name(c.order));

	if (cal.valid && median) {
		uint64_t left = c.nchunks > c.step ? c.nchunks - c.step : 0;
		double est;

		if (c.sample > 1)
			left /= c.sample;
		est = (double)left * (double)median / 1e6;
		out("  estimated runtime %s at %.2f ms per %zu KiB chunk\n",
		    human_time(est, b1, sizeof(b1)),
		    (double)median / 1000.0, c.chunk / 1024);
		if (c.order == ORD_RANDOM && cal.seq_us && median > cal.seq_us * 2)
			out("  %sthat is %.0fx what a sequential scan of the same "
			    "drive would take%s\n", c_yel(),
			    (double)median / (double)cal.seq_us, c_off());
		if (cal.big_len && cal.big_us) {
			double small_rate = (double)c.chunk / (double)median;
			double big_rate = (double)cal.big_len / (double)cal.big_us;

			if (big_rate > small_rate * 1.5) {
				double bigest = (double)(c.end - c.pos) /
						(double)cal.big_len *
						(double)cal.big_us / 1e6;

				if (c.sample > 1)
					bigest /= (double)c.sample;
				out("  %smost of that is waiting for the platter, not "
				    "reading it%s: %zu KiB\n"
				    "  took %.2f ms, so '--chunk %zuK' is %.1fx the "
				    "throughput and would\n"
				    "  bring this scan to about %s. The guarantee is "
				    "unchanged -- a chunk is\n"
				    "  never faster than its slowest sector at any size "
				    "-- but the budget\n"
				    "  scales with it, so pin it with --sector-slow-ms to "
				    "keep it tight.\n",
				    c_yel(), c_off(), cal.big_len / 1024,
				    (double)cal.big_us / 1000.0,
				    cal.big_len / 1024, big_rate / small_rate,
				    human_time(bigest, b2, sizeof(b2)));
			}
		}
	}

	c.t_start_us = now_us();
	c.bytes_at_start = c.bytes_done;
	c.rate_floor = rate_floor_of(&c, o->min_rate);
	scan_loop(&c, c.mode == MODE_CHECK);

	if (c.second_pass && c.mode == MODE_WRITE && !g_stop) {
		out("\n  second pass: re-reading and verifying the pattern\n");
		c.pos = c.start;
		c.step = 0;
		scan_loop(&c, 1);
	}
	c.t_end_us = now_us();
	rate_judge(&c, c.t_end_us);

	if (!o->no_smart)
		smart_read(d, &c.smart_after);

	/*
	 * A read never makes a drive repair anything; it only marks the sector
	 * pending.  A write is what forces the firmware to decide - rewrite in
	 * place if the fault was transient, or pull a spare and reallocate.  So
	 * if we rewrote sectors, the SMART deltas tell us what the firmware
	 * actually did, which beats inferring it from latency alone.
	 */
	if (c.smart_before.have && c.smart_after.have) {
		c.realloc_delta = c.smart_after.realloc_ct - c.smart_before.realloc_ct;
		if (c.realloc_delta < 0)
			c.realloc_delta = 0;
		/*
		 * We wrote to sectors the drive had flagged pending and they are
		 * still pending, with no new reallocations: the drive tried to
		 * relocate them and had nowhere to put them.
		 */
		if ((c.rewrite_weak || c.force_remap) && c.blocks_bad &&
		    c.smart_after.pending >= c.smart_before.pending &&
		    c.realloc_delta == 0 && c.smart_after.pending > 0)
			c.spares_exhausted = 1;
	}

	if (c.jrn_fd >= 0) {
		jrn_clear(c.jrn_fd);
		close(c.jrn_fd);
		c.jrn_fd = -1;
	}
	timeout_restore();
	lookahead_restore();
	field_restore_all();
	close(c.fd);
	c.fd = -1;

	report(&c);
	if (json_path)
		write_json(&c, json_path);
	if (csv_path)
		write_csv(&c, csv_path);
	if (bb_path)
		write_badblocks(&c, bb_path, o->bb_blocksize, o->bb_offset);
	if (prom_path)
		write_prometheus(&c, prom_path);

	v = verdict_of(&c, &why);
	free(c.find);
	g_ctx = NULL;
	if (g_stop)
		return 130;
	return v;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

static int dio_alignment(const char *path, const struct stat *st)
{
	int align = 0;

#ifdef STATX_DIOALIGN
	struct statx stx;

	if (statx(AT_FDCWD, path, AT_STATX_SYNC_AS_STAT, STATX_DIOALIGN, &stx) == 0 &&
	    (stx.stx_mask & STATX_DIOALIGN) && stx.stx_dio_offset_align)
		align = (int)stx.stx_dio_offset_align;
#endif
	if (align <= 0 && st->st_blksize > 0 && st->st_blksize <= 65536)
		align = (int)st->st_blksize;
	if (align <= 0)
		align = 4096;
	return align;
}

static int resolve_target(const char *arg, device_t *d)
{
	struct stat st;
	const char *name = arg;

	memset(d, 0, sizeof(*d));
	if (!strncmp(arg, "/dev/", 5))
		name = arg + 5;

	if (!strchr(arg, '/') || !strncmp(arg, "/dev/", 5)) {
		char sys[PATH_MAX];

		snprintf(sys, sizeof(sys), "/sys/block/%s", name);
		if (!stat(sys, &st) && S_ISDIR(st.st_mode))
			return probe_device(name, d);
	}

	if (stat(arg, &st) < 0) {
		msg(PROG ": %s: %s\n", arg, strerror(errno));
		return -1;
	}
	if (S_ISREG(st.st_mode)) {
		d->is_file = 1;
		snprintf(d->name, sizeof(d->name), "%s", arg);
		snprintf(d->path, sizeof(d->path), "%s", arg);
		snprintf(d->model, sizeof(d->model), "regular file (image)");
		d->size = (uint64_t)st.st_size;
		/*
		 * O_DIRECT on a file has to obey the alignment of whatever the
		 * filesystem sits on, which is not always 512.  Guessing low
		 * makes the kernel reject the tail read with EINVAL, which
		 * would look exactly like a dead sector.
		 */
		d->logical_bs = dio_alignment(arg, &st);
		d->physical_bs = d->logical_bs;
		d->rotational = 1;
		return 0;
	}
	if (S_ISBLK(st.st_mode)) {
		msg(PROG ": %s: not a whole disk in /sys/block "
		    "(partitions are not supported - test the whole drive)\n", arg);
		return -1;
	}
	msg(PROG ": %s: not a block device or file\n", arg);
	return -1;
}

/* ------------------------------------------------------------------ *
 * parallel execution across drives
 *
 * Every drive is an independent actuator, so N drives really do scan in 1/N
 * the wall-clock time of doing them one after another.  We deliberately do
 * NOT parallelise within a drive: a second outstanding request on the same
 * spindle makes the head seek between them, and we would be timing queueing
 * delay instead of the media.  One worker process per drive, one request in
 * flight per worker.
 *
 * The run is owned by a supervisor process of its own, not by whatever
 * started it.  That is what lets thirty formats and twenty-four scans be
 * going at once with nobody watching either: the terminal that set them off
 * is a viewer, and closing it takes nothing down.  See "the run store".
 * ------------------------------------------------------------------ */

/*
 * What a supervisor remembers about one drive while it runs it.  Progress is
 * deliberately not in here -- the worker writes that to its own job record,
 * which is what every viewer reads.  The supervisor keeps only what it needs
 * to schedule the next drive and to record how this one ended.
 */
typedef struct {
	device_t *dev;
	pid_t pid;
	unsigned long long pidstart;
	int started, done;
	int stopped;            /* the stop has been passed on; see forward_stop */
	char file[STORE_MAX + 128];     /* this drive's job record */
	char report[PATH_MAX];
} job_t;

static int jrec_cmp_risk(const void *a, const void *b)
{
	const jrec_t *x = a, *y = b;

	if ((x->bad != 0) != (y->bad != 0))
		return x->bad ? -1 : 1;
	if ((x->slow != 0) != (y->slow != 0))
		return x->slow ? -1 : 1;
	if (x->pct != y->pct)
		return x->pct < y->pct ? -1 : 1;
	return 0;
}

static const char *verdict_word(int st)
{
	switch (st) {
	case 0: return "HEALTHY";
	case 1: return "SUSPECT";
	case 130: return "STOPPED";
	default: return "FAILING";
	}
}

/*
 * What one row's STATE column says.  A finished scan says what it found; a
 * job whose worker was killed says so rather than showing a percentage that
 * will never move again, and a format says where the drive is.
 */
static const char *jrec_word(const jrec_t *j, int format)
{
	if (j->state == JS_STOPPED)
		return "STOPPED";
	if (j->state == JS_DONE)
		return format ? (j->verdict == 0 ? "FORMATTED" : "FAILED")
			      : verdict_word(j->verdict);
	if (j->state == JS_ORPHANED)
		return "orphaned";
	if (j->state == JS_QUEUED)
		return "queued";
	if (format)
		return "formatting";
	return j->too_slow ? "too slow" : "scanning";
}

static const char *jrec_color(const jrec_t *j)
{
	if (j->state == JS_ORPHANED)
		return c_yel();
	if (j->state == JS_RUNNING && j->too_slow)
		return c_red();
	if (j->state != JS_DONE && j->state != JS_STOPPED)
		return "";
	return j->verdict == 0 ? c_grn() : j->verdict == 1 ? c_yel() : c_red();
}

/* how the fleet is doing, in one pass over the records */
typedef struct {
	int running, done, queued, orphaned;
	int healthy, suspect, failing, stopped;
	double pct, agg_rate, max_eta;
	uint64_t bytes, bad, weak, slow;
} tally_t;

static void tally(const jrec_t *j, int n, tally_t *t)
{
	int i;

	memset(t, 0, sizeof(*t));
	for (i = 0; i < n; i++) {
		const jrec_t *x = &j[i];

		switch (x->state) {
		case JS_QUEUED:
			t->queued++;
			break;
		case JS_RUNNING:
			t->running++;
			t->agg_rate += x->rate;
			if (x->eta > t->max_eta)
				t->max_eta = x->eta;
			break;
		case JS_ORPHANED:
			t->orphaned++;
			break;
		default:
			t->done++;
			if (x->state == JS_STOPPED || x->verdict == 130)
				t->stopped++;
			else if (x->verdict == 0)
				t->healthy++;
			else if (x->verdict == 1)
				t->suspect++;
			else
				t->failing++;
		}
		t->pct += x->state == JS_DONE ? 100.0 : x->pct;
		t->bytes += x->bytes;
		t->bad += x->bad;
		t->weak += x->weak;
		t->slow += x->slow;
	}
	t->pct /= (double)(n > 0 ? n : 1);
}

static double run_elapsed(const run_t *r)
{
	long long end = r->ended ? r->ended : (long long)time(NULL);

	return (double)(end - r->created);
}

/*
 * The progress block for a terminal that is not running the dashboard: a
 * scripted run, a run being followed over ssh, a log.  Same numbers, drawn
 * by rewriting the last few lines in place.
 */
static int render_parallel(const run_t *r, const jrec_t *j, int n,
			   int prev_lines, int final)
{
	int i, lines = 0, format = !strcmp(r->kind, "format");
	tally_t t;
	char b1[32], b2[32];
	int tty = isatty(STDERR_FILENO);
	int detail = n <= 20;
	int shown;

	tally(j, n, &t);

	if (tty && prev_lines > 0)
		fprintf(stderr, "\033[%dA", prev_lines);

#define LINE(...) do { fprintf(stderr, "\033[2K"); \
		       fprintf(stderr, __VA_ARGS__); lines++; } while (0)

	if (format)
		LINE("%d drives: %d formatting, %d done (%s%d ok%s, %s%d "
		     "failed%s)          \n",
		     n, t.running, t.done, c_grn(), t.healthy, c_off(),
		     c_red(), t.suspect + t.failing, c_off());
	else
		LINE("%d drives: %d scanning, %d done (%s%d healthy%s, %s%d "
		     "suspect%s, %s%d failing%s)          \n",
		     n, t.running, t.done, c_grn(), t.healthy, c_off(),
		     c_yel(), t.suspect, c_off(), c_red(), t.failing, c_off());
	LINE("  overall %5.1f%%   aggregate %s/s   %s tested   elapsed %s   eta %s   \n",
	     t.pct, human_size((uint64_t)t.agg_rate, b1, sizeof(b1)),
	     human_size(t.bytes, b2, sizeof(b2)),
	     human_time(run_elapsed(r), (char[32]){0}, 32),
	     t.running ? human_time(t.max_eta, (char[32]){0}, 32) : "-");
	{
		int w = 48, k, filled = (int)(t.pct / 100.0 * w + 0.5);

		fprintf(stderr, "\033[2K  [");
		for (k = 0; k < w; k++)
			fputc(k < filled ? '=' : '-', stderr);
		fprintf(stderr, "]  bad sectors %" PRIu64 ", weak %" PRIu64
			", slow %" PRIu64 "   \n", t.bad, t.weak, t.slow);
		lines++;
	}
	LINE("  run %s   hddscan --status %s   %s\n", r->id, r->id,
	     final ? "finished" : "runs on if this terminal goes away");

	if (!detail) {
		/* too many drives to list: show the ones that need attention */
		jrec_t *sorted = malloc((size_t)n * sizeof(*sorted));

		if (sorted) {
			memcpy(sorted, j, (size_t)n * sizeof(*sorted));
			qsort(sorted, (size_t)n, sizeof(*sorted), jrec_cmp_risk);
			shown = n < 6 ? n : 6;
			LINE("  needing attention / furthest behind:                    \n");
			for (i = 0; i < shown; i++) {
				jrec_t *x = &sorted[i];

				LINE("    %-8s %5.1f%% %9s/s  bad %-5" PRIu64
				     " weak %-5" PRIu64 " slow %-7" PRIu64
				     " %s%-10s%s        \n",
				     jrec_name(x),
				     x->state == JS_DONE ? 100.0 : x->pct,
				     human_size((uint64_t)x->rate, b1, sizeof(b1)),
				     x->bad, x->weak, x->slow,
				     jrec_color(x), jrec_word(x, format), c_off());
			}
			free(sorted);
		}
	} else {
		for (i = 0; i < n; i++) {
			const jrec_t *x = &j[i];

			LINE("    %-8s %5.1f%% %9s/s  bad %-5" PRIu64 " weak %-5"
			     PRIu64 " slow %-7" PRIu64 " eta %-10s %s%s%s   \n",
			     jrec_name(x), x->state == JS_DONE ? 100.0 : x->pct,
			     human_size((uint64_t)x->rate, b1, sizeof(b1)),
			     x->bad, x->weak, x->slow,
			     x->state == JS_RUNNING ?
			     human_time(x->eta, b2, sizeof(b2)) : "-",
			     jrec_color(x), jrec_word(x, format), c_off());
		}
	}
#undef LINE
	fflush(stderr);
	if (final)
		fputc('\n', stderr);
	return lines;
}

/* ------------------------------------------------------------------ *
 * terminal UI
 *
 * Raw ANSI and termios rather than ncurses, so the tool keeps its "nothing
 * but libc" property and stays a single binary you can scp onto a rescue
 * system that has no packages on it.
 * ------------------------------------------------------------------ */

static struct termios g_tty_saved;
static int g_tty_raw;

static void tui_cooked(void)
{
	g_dash = 0;
	if (!g_tty_raw)
		return;
	tcsetattr(STDIN_FILENO, TCSANOW, &g_tty_saved);
	/* show cursor, leave the alternate screen */
	fputs("\033[?25h\033[?1049l", stdout);
	fflush(stdout);
	g_tty_raw = 0;
}

static int tui_raw(void)
{
	static int registered;
	struct termios t;

	/*
	 * The form is re-entered after every run now, so this must not save
	 * the raw state as "what to put back" the second time through, and
	 * invariant 7 wants the atexit slot claimed exactly once.
	 */
	if (g_tty_raw)
		return 0;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		return -1;
	if (tcgetattr(STDIN_FILENO, &g_tty_saved) < 0)
		return -1;
	t = g_tty_saved;
	t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &t) < 0)
		return -1;
	g_tty_raw = 1;
	if (!registered) {
		atexit(tui_cooked);
		registered = 1;
	}
	/* alternate screen, hide cursor */
	fputs("\033[?1049h\033[?25l", stdout);
	fflush(stdout);
	return 0;
}

static void tui_size(int *rows, int *cols)
{
	struct winsize ws;

	*rows = 24;
	*cols = 80;
	if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	}
}

#define K_UP    1000
#define K_DOWN  1001
#define K_LEFT  1002
#define K_RIGHT 1003

/* returns a key, or -1 if nothing is waiting */
static int tui_key(int timeout_ms)
{
	struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
	unsigned char c;

	if (poll(&pfd, 1, timeout_ms) <= 0)
		return -1;
	if (read(STDIN_FILENO, &c, 1) != 1)
		return -1;
	if (c != 27)
		return c;
	/* escape sequence: read the rest if it is there */
	if (poll(&pfd, 1, 20) <= 0)
		return 27;
	if (read(STDIN_FILENO, &c, 1) != 1 || c != '[')
		return 27;
	if (read(STDIN_FILENO, &c, 1) != 1)
		return 27;
	switch (c) {
	case 'A': return K_UP;
	case 'B': return K_DOWN;
	case 'C': return K_RIGHT;
	case 'D': return K_LEFT;
	default: return 27;
	}
}

static void tui_clear(void) { fputs("\033[2J\033[H", stdout); }
static void tui_at(int r, int c) { printf("\033[%d;%dH", r, c); }
static void tui_eol(void) { fputs("\033[K", stdout); }

static void tui_bar(int width, double frac)
{
	int i, filled;

	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	filled = (int)(frac * width + 0.5);
	fputc('[', stdout);
	for (i = 0; i < width; i++)
		fputc(i < filled ? '=' : '-', stdout);
	fputc(']', stdout);
}

/*
 * The live view of one run.  It draws from the run's job records, not from
 * anything this process owns, so the same screen serves a run this terminal
 * just started, a run started an hour ago from another terminal, and a run
 * whose supervisor is still going after the program that launched it exited.
 *
 * Draw by absolute row, erasing each one, including the blank separators.
 * Chaining newlines and only erasing the rows that carry text leaves the
 * blanks holding whatever last scrolled into them, which is exactly what used
 * to happen.
 */
static void tui_dashboard(const run_t *r, const jrec_t *j, int n, int nruns)
{
	int rows, cols, i, listrows, row = 1;
	int format = !strcmp(r->kind, "format");
	tally_t t;
	char b1[32], b2[32];

	tui_size(&rows, &cols);
	tally(j, n, &t);

	tui_at(row++, 1);
	printf("\033[1m hddscan " VERSION "\033[0m   %s   %d drive%s   "
	       "elapsed %s   %s", r->detail, n, n == 1 ? "" : "s",
	       human_time(run_elapsed(r), b1, sizeof(b1)),
	       r->live ? "" : "\033[1mnot running\033[0m");
	tui_eol();
	tui_at(row++, 1);
	printf(" ");
	tui_bar(cols > 40 ? cols - 24 : 20, t.pct / 100.0);
	printf(" %5.1f%%", t.pct);
	tui_eol();
	tui_at(row++, 1);
	tui_eol();
	tui_at(row++, 1);
	if (format)
		printf("  %d formatting   %d queued   %d done   %s%d ok%s  "
		       "%s%d failed%s", t.running, t.queued, t.done,
		       c_grn(), t.healthy, c_off(),
		       c_red(), t.suspect + t.failing, c_off());
	else
		printf("  %d scanning   %d queued   %d done   %s%d healthy%s  "
		       "%s%d suspect%s  %s%d failing%s", t.running, t.queued,
		       t.done, c_grn(), t.healthy, c_off(), c_yel(), t.suspect,
		       c_off(), c_red(), t.failing, c_off());
	tui_eol();
	tui_at(row++, 1);
	if (format)
		printf("  run %s   started %s ago   reports in %s/", r->id,
		       human_time(run_elapsed(r), b1, sizeof(b1)), r->outdir);
	else
		printf("  %s/s aggregate   %s tested   eta %s",
		       human_size((uint64_t)t.agg_rate, b1, sizeof(b1)),
		       human_size(t.bytes, b2, sizeof(b2)),
		       t.running ? human_time(t.max_eta, (char[32]){0}, 32) : "-");
	tui_eol();
	tui_at(row++, 1);
	if (format)
		printf("  a format cannot be interrupted once the drive has "
		       "started it");
	else
		printf("  bad sectors %" PRIu64 "   weak %" PRIu64
		       "   slow %" PRIu64, t.bad, t.weak, t.slow);
	tui_eol();
	tui_at(row++, 1);
	tui_eol();
	tui_at(row++, 1);
	if (format)
		printf("  the drive reports its own progress; a fast format "
		       "reports none until it ends");
	else
		printf("  latency columns are milliseconds over the last 30 seconds");
	tui_eol();
	tui_at(row++, 1);
	if (format)
		printf("  %-8s %6s %-10s %-20s %-10s %s", "DRIVE", "PCT",
		       "SIZE", "MODEL", "STATE", "LAST MESSAGE");
	else
		printf("  %-8s %6s %11s %7s %7s %8s %6s %6s %6s %6s %6s %9s %s",
		       "DRIVE", "PCT", "RATE", "BAD", "WEAK", "SLOW", "MED",
		       "AVG", "MIN", "MAX", "WMED", "ETA", "STATE");
	tui_eol();

	listrows = rows - row - g_msg_count - 3;
	if (listrows < 1)
		listrows = 1;

	/*
	 * Thirty drives do not fit a terminal as a table -- one row each needs
	 * fifty lines, and a shelf is exactly the case this screen exists for.
	 * When the table would be cut off, drop to a grid of small cells: the
	 * name, the percentage and a bar, several to a line.  Less about each
	 * drive, but every drive, which is the right trade when the question
	 * is "how is the batch going".
	 */
	if (n > listrows) {
		/*
		 * A cell is " name(8) pct(6) " plus a 12-wide bar: 29
		 * columns, and the line starts with one more.  Sizing it as
		 * 27 put a fourth cell on a 112-column line and cut its bar
		 * off at the edge.
		 */
		int cw = 29, percol = (cols - 1) / cw, k;

		if (percol < 1)
			percol = 1;
		/* undo the table's own header line; the grid has none */
		tui_at(row - 1, 1);
		printf("  %d drives, %d to a line - '%s --status %s' lists them "
		       "in full", n, percol, PROG, r->id);
		tui_eol();
		for (i = 0; i < n && row < rows - 1; i += percol) {
			tui_at(row++, 1);
			printf(" ");
			for (k = i; k < n && k < i + percol; k++) {
				const jrec_t *x = &j[k];

				printf(" %s%-8.8s%s %5.1f%% ", jrec_color(x),
				       jrec_name(x), c_off(),
				       x->state == JS_DONE ? 100.0 : x->pct);
				tui_bar(10, (x->state == JS_DONE ? 100.0 :
					     x->pct) / 100.0);
			}
			tui_eol();
		}
		if (i < n) {
			tui_at(row++, 1);
			printf("  ... and %d more", n - i);
			tui_eol();
		}
		goto drives_done;
	}

	for (i = 0; i < n && i < listrows; i++) {
		const jrec_t *x = &j[i];
		const char *col = jrec_color(x);

		tui_at(row++, 1);
		if (format) {
			printf("  %-8.8s %5.1f%% %-10s %-20.20s %s%-10s%s %.*s",
			       jrec_name(x), x->pct,
			       human_size(x->size, b1, sizeof(b1)),
			       x->model[0] ? x->model : "?",
			       col, jrec_word(x, 1), c_off(),
			       cols > 66 ? cols - 64 : 14, x->note);
		} else {
			char lat[64];

			if (x->state == JS_RUNNING && x->lat_max > 0)
				snprintf(lat, sizeof(lat),
					 "%6.1f %6.1f %6.1f %6.1f", x->lat_med,
					 x->lat_avg, x->lat_min, x->lat_max);
			else
				snprintf(lat, sizeof(lat), "%6s %6s %6s %6s",
					 "-", "-", "-", "-");
			/* writes are timed separately: a drive whose writes
			 * crawl reads back at full speed */
			if (x->state == JS_RUNNING && x->wlat_med > 0)
				snprintf(lat + strlen(lat), sizeof(lat) - strlen(lat),
					 " %6.1f", x->wlat_med);
			else
				snprintf(lat + strlen(lat), sizeof(lat) - strlen(lat),
					 " %6s", "-");
			printf("  %-8.8s %5.1f%% %9s/s %7" PRIu64 " %7" PRIu64
			       " %8" PRIu64 " %s %9s %s%s%s",
			       jrec_name(x), x->state == JS_DONE ? 100.0 : x->pct,
			       human_size((uint64_t)x->rate, b1, sizeof(b1)),
			       x->bad, x->weak, x->slow, lat,
			       x->state == JS_RUNNING ?
			       human_time(x->eta, b2, sizeof(b2)) : "-",
			       col, jrec_word(x, 0), c_off());
		}
		tui_eol();
	}

drives_done:
	/* whatever the run had to say, where it cannot corrupt the frame */
	for (i = 0; i < g_msg_count && row < rows - 1; i++) {
		int k = (g_msg_head - g_msg_count + i + MSG_RING * 2) % MSG_RING;

		tui_at(row++, 1);
		printf("  %s%.*s%s", c_yel(), cols > 6 ? cols - 4 : 60,
		       g_msgring[k], c_off());
		tui_eol();
	}

	/* and clear anything an earlier, taller frame or a stray write left */
	while (row < rows) {
		tui_at(row++, 1);
		tui_eol();
	}
	tui_at(rows, 1);
	printf("  %sr runs%s   n new test   x stop   q quit - the run keeps "
	       "going either way   reports in %s/",
	       nruns > 1 ? "\033[1m" : "", nruns > 1 ? "\033[0m" : "",
	       r->outdir);
	tui_eol();
	fflush(stdout);
}

/*
 * Pass the stop on to each worker, once.  The second SIGINT a worker gets is
 * its hard-stop path, which gives up on writing the report -- so a supervisor
 * that re-sent on every tick while it waited would throw away the partial
 * result of every drive it was trying to stop gracefully.
 */
static void forward_stop(job_t *jobs, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (!jobs[i].started || jobs[i].done || jobs[i].pid <= 0)
			continue;
		if (jobs[i].stopped)
			continue;
		jobs[i].stopped = 1;
		kill(jobs[i].pid, SIGINT);
	}
}

/*
 * The end-of-run summary screen.  A scan can run for days, so its result must
 * not scroll away the moment the last drive finishes: hold the verdicts on
 * screen until a key decides what happens next.  Returns 1 to go back to the
 * form for another test, 2 to go to the list of runs, 0 to quit.
 */
static int tui_summary(const run_t *r, const jrec_t *j, int n, int nruns)
{
	int format = !strcmp(r->kind, "format");

	/*
	 * The stop that ended the scan is spent; the workers are gone.  Clear
	 * it so a stopped run still gets to read its summary and choose, and
	 * so a Ctrl-C from here on means "leave the summary", not the signal
	 * handler's hard exit -- which would skip the terminal restore.
	 */
	g_stop = 0;

	for (;;) {
		int rows, cols, i, row = 1, listrows;
		tally_t t;
		char b1[32], b2[32];
		int k;

		tally(j, n, &t);
		tui_size(&rows, &cols);
		tui_at(row++, 1);
		printf("\033[1m hddscan " VERSION "\033[0m   %s finished   "
		       "%d drive%s   took %s", r->detail, n, n == 1 ? "" : "s",
		       human_time(run_elapsed(r), b1, sizeof(b1)));
		tui_eol();
		tui_at(row++, 1);
		tui_eol();
		tui_at(row++, 1);
		if (format)
			printf("  %s%d formatted%s  %s%d failed%s  %d stopped",
			       c_grn(), t.healthy, c_off(), c_red(),
			       t.suspect + t.failing, c_off(), t.stopped);
		else
			printf("  %s%d healthy%s  %s%d suspect%s  %s%d failing%s"
			       "  %d stopped",
			       c_grn(), t.healthy, c_off(), c_yel(), t.suspect,
			       c_off(), c_red(), t.failing, c_off(), t.stopped);
		tui_eol();
		tui_at(row++, 1);
		tui_eol();
		tui_at(row++, 1);
		printf("  %-8s %-10s %-20s %-10s %s",
		       "DRIVE", "SIZE", "MODEL", "VERDICT", "REPORT");
		tui_eol();

		listrows = rows - row - g_msg_count - 4;
		if (listrows < 1)
			listrows = 1;
		for (i = 0; i < n && i < listrows; i++) {
			const jrec_t *x = &j[i];

			tui_at(row++, 1);
			printf("  %-8.8s %-10s %-20.20s %s%-10s%s %.*s",
			       jrec_name(x), human_size(x->size, b2, sizeof(b2)),
			       x->model[0] ? x->model : "?",
			       jrec_color(x), jrec_word(x, format), c_off(),
			       cols > 56 ? cols - 54 : 20, x->report);
			tui_eol();
		}
		if (n > listrows) {
			tui_at(row++, 1);
			printf("  ... and %d more (reports in %s/)",
			       n - listrows, r->outdir);
			tui_eol();
		}

		for (i = 0; i < g_msg_count && row < rows - 1; i++) {
			k = (g_msg_head - g_msg_count + i + MSG_RING * 2)
			    % MSG_RING;
			tui_at(row++, 1);
			printf("  %s%.*s%s", c_yel(),
			       cols > 6 ? cols - 4 : 60, g_msgring[k], c_off());
			tui_eol();
		}

		while (row < rows) {
			tui_at(row++, 1);
			tui_eol();
		}
		tui_at(rows, 1);
		printf("  the full reports are on disk in %s/   "
		       "\033[1mn\033[0m new test   %sr runs   %s"
		       "\033[1mq\033[0m quit", r->outdir,
		       nruns > 1 ? "\033[1m" : "", nruns > 1 ? "\033[0m" : "");
		tui_eol();
		fflush(stdout);

		k = tui_key(200);
		if (k == 'n' || k == 'N' || k == '\r' || k == '\n')
			return 1;
		if (k == 'r' || k == 'R')
			return 2;
		if (k == 'q' || k == 'Q' || k == 27 || k == 3 || g_stop)
			return 0;
	}
}

/*
 * Follow one run on the dashboard, reloading its records as it goes.  This is
 * the only screen that ever shows a scan in flight, and it is deliberately a
 * viewer and nothing more: 'd' walks away from a run that keeps going, 'x'
 * asks the supervisor to stop, and the difference between the two is never
 * left to a guess.
 *
 * Returns 0 to quit the program, 1 for the list of runs, 2 for the form.
 */
static int tui_watch(const char *id, int nruns)
{
	int arm = 0;

	/*
	 * Anything this process has to say from here on goes to the message
	 * ring and is drawn where it cannot corrupt a frame that is painted
	 * by absolute cursor position.
	 */
	g_dash = 1;
	for (;;) {
		run_t r;
		jrec_t *j;
		int n, k;

		{
			char dir[STORE_MAX + 300];
			const char *roots[2];
			int nroot = store_roots(roots, 2), i, got = -1;

			for (i = 0; i < nroot && got < 0; i++) {
				snprintf(dir, sizeof(dir), "%s/runs/%s",
					 roots[i], id);
				got = run_get(dir, &r);
			}
			if (got < 0)
				return 1;       /* forgotten under our feet */
		}
		n = store_jobs(&r, &j);
		{
			tally_t t;

			tally(j, n, &t);
			/*
			 * Every drive accounted for and the supervisor gone:
			 * the run is over, so hold the result on the summary
			 * screen rather than a dashboard that will never
			 * change again.
			 */
			if (n > 0 && !r.live && !t.running && !t.queued) {
				int act = tui_summary(&r, j, n, nruns);

				free(j);
				return act == 1 ? 2 : act == 2 ? 1 : 0;
			}
		}
		tui_dashboard(&r, j, n, nruns);
		if (arm) {
			int rows, cols;

			tui_size(&rows, &cols);
			tui_at(rows, 1);
			printf("  %spress y to stop run %s, any other key to "
			       "leave it running%s", c_red(), r.id, c_off());
			tui_eol();
			fflush(stdout);
		}
		free(j);

		k = tui_key(500);
		if (k < 0)
			continue;
		if (arm) {
			arm = 0;
			if (k == 'y')
				store_stop(&r);
			continue;
		}
		if (k == 'x' || k == 'X') {
			arm = 1;
			continue;
		}
		if (k == 3 || k == 27) {        /* Ctrl-C, escape */
			store_stop(&r);
			continue;
		}
		if (k == 'r' || k == 'R' || k == 'b' || k == K_LEFT)
			return 1;
		if (k == 'd' || k == 'D' || k == 'n' || k == 'N')
			return 2;
		if (k == 'q' || k == 'Q')
			return 0;
	}
}

/*
 * The list of runs: what this machine is doing, and what it did.  This is the
 * screen that makes thirty formats and twenty-four scans navigable -- each
 * run is one row, and opening one is one keystroke.  It is also the first
 * thing shown when the program starts and finds work already in progress,
 * because the alternative is a form that quietly implies nothing is running.
 *
 * Returns 1 for the form (a new test), 0 to quit.
 */
static int tui_runs(void)
{
	int cur = 0, top = 0, arm = 0;

	g_dash = 1;             /* see tui_watch: msg() must not touch the screen */
	for (;;) {
		run_t *v;
		int n = store_list(&v), i, rows, cols, row = 1, listrows, live = 0;
		int k;

		tui_size(&rows, &cols);
		for (i = 0; i < n; i++)
			if (v[i].live)
				live++;
		if (cur >= n)
			cur = n - 1;
		if (cur < 0)
			cur = 0;

		tui_at(row++, 1);
		printf("\033[1m hddscan " VERSION " \033[0m  runs on this machine"
		       "   %d in progress", live);
		tui_eol();
		tui_at(row++, 1);
		tui_eol();
		tui_at(row++, 1);
		printf("  %-17s %-7s %-22s %6s %7s  %-9s %s", "RUN", "KIND",
		       "WHAT", "DRIVES", "PROGRESS", "STATE", "ELAPSED");
		tui_eol();

		listrows = rows - row - 5;
		if (listrows < 1)
			listrows = 1;
		if (cur >= top + listrows)
			top = cur - listrows + 1;
		if (cur < top)
			top = cur;
		for (i = top; i < n && i < top + listrows; i++) {
			run_t *r = &v[i];
			jrec_t *j;
			int nj = store_jobs(r, &j);
			tally_t t;
			char b1[32];
			const char *state;

			tally(j, nj, &t);
			if (r->live)
				state = t.running ? "running" : "starting";
			else if (t.running || t.queued || t.orphaned)
				state = "interrupted";
			else
				state = "finished";
			tui_at(row++, 1);
			printf("%s  %-17.17s %-7.7s %-22.22s %6d %6.1f%%  "
			       "%s%-9s%s %s%s",
			       i == cur ? "\033[7m" : "",
			       r->id, r->kind, r->what, nj, t.pct,
			       !strcmp(state, "interrupted") ? c_yel() : "",
			       state,
			       !strcmp(state, "interrupted") ? c_off() : "",
			       human_time(run_elapsed(r), b1, sizeof(b1)),
			       i == cur ? "\033[0m" : "");
			tui_eol();
			free(j);
		}
		if (!n) {
			tui_at(row++, 1);
			printf("  nothing has been run on this machine yet");
			tui_eol();
		}
		while (row < rows - 1) {
			tui_at(row++, 1);
			tui_eol();
		}
		tui_at(rows - 1, 1);
		if (arm && n)
			printf("  %spress y to stop run %s, any other key to "
			       "leave it alone%s", c_red(), v[cur].id, c_off());
		else if (n)
			printf("  a run keeps going whether or not this program "
			       "is open");
		tui_eol();
		tui_at(rows, 1);
		printf("  enter open   x stop   f forget   n new test   q quit");
		tui_eol();
		fflush(stdout);

		k = tui_key(500);
		/*
		 * A repaint tick is not an answer.  Clearing the confirmation
		 * because half a second passed made "press y to stop" a
		 * prompt that could not be answered at all: the key landed
		 * after the timeout had already cancelled it.
		 */
		if (k < 0) {
			free(v);
			continue;
		}
		if (arm) {
			arm = 0;
			if (k == 'y' && n)
				store_stop(&v[cur]);
			free(v);
			continue;
		}
		if (k == K_UP && cur > 0)
			cur--;
		else if (k == K_DOWN && cur < n - 1)
			cur++;
		else if (n && (k == '\r' || k == '\n' || k == ' ' ||
			       k == K_RIGHT || k == 'o')) {
			char id[48];
			int act;

			snprintf(id, sizeof(id), "%s", v[cur].id);
			free(v);
			act = tui_watch(id, n);
			if (act != 1)
				return act == 2 ? 1 : 0;
			continue;
		} else if (k == 'x' && n) {
			arm = 1;
		} else if (k == 'f' && n) {
			/*
			 * Forgetting is only ever about tidying the list, so
			 * it refuses a live run rather than asking: the run
			 * would keep going with nothing left to watch it
			 * with, which is the one outcome nobody wants.
			 */
			if (!v[cur].live)
				store_rmrun(&v[cur]);
			else
				msg(PROG ": run %s is still going; stop it "
				    "first\n", v[cur].id);
		} else if (k == 'n' || k == 'N') {
			free(v);
			return 1;
		} else if (k == 'q' || k == 'Q' || k == 3) {
			free(v);
			return 0;
		}
		free(v);
	}
}

static const char *derive_path(const char *base, const char *dev, int many,
			       char *buf, size_t n)
{
	const char *dot;

	if (!base)
		return NULL;
	if (!many) {
		snprintf(buf, n, "%s", base);
		return buf;
	}
	dot = strrchr(base, '.');
	if (dot && !strchr(dot, '/'))
		snprintf(buf, n, "%.*s.%s%s", (int)(dot - base), base, dev, dot);
	else
		snprintf(buf, n, "%s.%s", base, dev);
	return buf;
}

/* ---- configuration screen ---- */

typedef struct {
	/*
	 * A heading printed above this field, or NULL.  Headings are not
	 * selectable, so the cursor still maps one-to-one onto fields and
	 * nothing else in here has to know they exist.
	 */
	const char *head;
	/*
	 * Fields that only matter for one mode are hidden the rest of the
	 * time.  Nineteen settings on a short terminal means six visible at
	 * once and the format options three screens down, where they may as
	 * well not exist.  The cursor walks the visible list, so nothing else
	 * has to know they come and go.
	 */
	int only_format;
	const char *label;
	const char *help;
	int kind;               /* 0 = pick from choices, 1 = numeric */
	int nchoices;
	const char *choices[8];
	long long *num;
	long long lo, hi, step;
	int *sel;
} field_t;

static void tui_devices(device_t *devs, int ndev, int *pick, int cur, int top,
			int rows)
{
	int i;

	for (i = 0; i < ndev && i < rows; i++) {
		device_t *d = &devs[top + i];
		char sz[32];
		int usable;

		if (top + i >= ndev)
			break;
		usable = !d->unsafe && d->rotational &&
			 (d->is_file || transport_is_local(d->transport));
		printf("  %s%s [%c] %-8.8s %-7.7s %-10s %-20.20s %-9s %s%s\n",
		       (top + i == cur) ? "\033[7m" : "",
		       "", pick[top + i] ? 'x' : ' ',
		       disp_name(d), d->group, human_size(d->size, sz, sizeof(sz)),
		       d->model[0] ? d->model : "?",
		       d->zoned ? "HDD-SMR" : d->rotational ? "HDD" : "SSD",
		       d->unsafe ? "IN USE" :
		       (!d->is_file && !transport_is_local(d->transport)) ?
		       transport_name(d->transport) :
		       usable ? "ready" : "not a HDD",
		       (top + i == cur) ? "\033[0m" : "");
	}
}

/* one settings row: label, value, and the help for the row under the cursor */
static void form_row(const field_t *fp, int hl, int cols)
{
	char val[64];

	if (fp->kind == 0)
		snprintf(val, sizeof(val), "%s", fp->choices[*fp->sel]);
	else if (fp->lo < 0 && *fp->num < 0)
		snprintf(val, sizeof(val), "keep");
	else
		snprintf(val, sizeof(val), "%lld", *fp->num);
	/*
	 * The help is cut to what the terminal has left: a wrapped row is a
	 * row the form's budget did not count, and the terminal answers by
	 * scrolling the top of the form away.
	 */
	printf("%s   %-22s < %-12s >%s  %.*s\n", hl ? "\033[7m" : "", fp->label,
	       val, hl ? "\033[0m" : "", cols > 47 ? cols - 47 : 0,
	       hl ? fp->help : "");
}

/*
 * Returns 1 to start the scan, 0 to quit, 2 to show the list of runs.
 * Everything it touches lives in the opts_t the caller will hand to the
 * scanner, so the TUI is purely a front end to the same options the command
 * line sets.
 */
static int tui_config(device_t *devs, int ndev, int *pick, opts_t *o)
{
	/*
	 * The two read-only modes are shown together at the top, which is not
	 * the order scan_mode_t declares them in, so the selection is an index
	 * into mode_v and never a scan_mode_t.  Casting the index straight to
	 * the enum gave "check" a verify pass and "verify" a destructive
	 * write -- silent, and on the wrong side of every safety check.
	 */
	static const scan_mode_t mode_v[] = { MODE_READ, MODE_CHECK,
					      MODE_VERIFY, MODE_WRITE };
	static const char *mode_c[] = { "read", "check", "verify", "write",
					"repair", "format", "settings" };
	int mode_sel = 0, order_sel = (int)o->order;
	int la_sel = o->lookahead == 0 ? 0 : 1, bms_sel = o->bms ? 1 : 0;
	int wc_sel = o->write_cache < 0 ? 0 : o->write_cache + 1;
	/*
	 * Chunk size on the form, because it is the biggest lever on how long
	 * a scan takes -- and the default stays at 128 KiB, because it is also
	 * the dial that decides what the scan can see.  The budget is a
	 * multiple of the measured median, so a bigger chunk raises the bar a
	 * sector has to clear to be noticed.  Fast and blind is not the
	 * default this tool should ship.
	 */
	static const size_t chunk_v[] = { 128u * 1024, 256u * 1024,
					  512u * 1024, 1024u * 1024,
					  4096u * 1024 };
	int chunk_sel = 0;
	int prof_sel = 0, prof_was;
	/*
	 * Block size and protection are indexes rather than the values
	 * themselves, because "keep" is not a number and the whole point of
	 * the default is to preserve what the drive already has -- sg_format
	 * strips protection information unless told otherwise.
	 */
	static const int fbs_v[] = { 0, 512, 4096 };
	static const int fpi_v[] = { -1, 0, 1, 2 };
	int fbs_sel = 0, fpi_sel = 0, ffast_sel = 0;
	int persist_sel = o->persist ? 1 : 0, fix_sel = o->fix_config ? 1 : 0;
	int awre_sel = o->awre < 0 ? 0 : o->awre + 1;
	int rc_sel = o->read_cache < 0 ? 0 : o->read_cache + 1;
	long long rtl = o->recovery_ms;
	int la_init = la_sel;   /* to tell "chose off" from "left at off" */
	long long retries = o->retries, segment_mb = (long long)(o->segment >> 20);
	long long parallel = o->max_parallel, sample = (long long)o->sample;
	int cur = 0, sec = 0, arm = 0, i, rows, cols, dtop = 0;
	int ftop = 0, fshow = 0, depline = 0, nscroll = 0;
	int nlive = store_live_count();
	field_t f[] = {
	/*
	 * Two groups, because they answer different questions.  Everything
	 * above the second heading decides how this run behaves; everything
	 * below it changes the drive itself, through sdparm or hdparm, and is
	 * put back afterwards unless "Save to drive" says otherwise.
	 */
	{ "How to test", 0, "Profile",
	  "what you are about to do with the drive; sets the rest",
	  0, 5, { "predeploy", "inservice", "survey", "decay", "repair" },
	  NULL, 0, 0, 0, &prof_sel },
	{ NULL, 0, "Mode", "read is safe; write and repair destroy data; format erases it",
	  0, 7, { "read", "check", "verify", "write", "repair", "format",
		  "settings" },
	  NULL, 0, 0, 0, &mode_sel },
	{ NULL, 0, "Chunk size", "bigger is much faster but raises the bar a sector must clear",
	  0, 5, { "128K", "256K", "512K", "1M", "4M" },
	  NULL, 0, 0, 0, &chunk_sel },
	{ NULL, 0, "Scan order", "sequential is fastest; random defeats drive prefetch",
	  0, 3, { "sequential", "reverse", "random" }, NULL, 0, 0, 0, &order_sel },
	{ NULL, 0, "Retries per sector", "re-reads before judging a suspect sector",
	  1, 0, { 0 }, &retries, 0, 1000, 5, NULL },
	{ NULL, 0, "Shuffle segment (MiB)", "must exceed the drive cache; random order only",
	  1, 0, { 0 }, &segment_mb, 0, 1024 * 64, 256, NULL },
	{ NULL, 0, "Max parallel drives", "0 means all at once",
	  1, 0, { 0 }, &parallel, 0, 512, 1, NULL },
	{ NULL, 0, "Sample 1 chunk in N", "1 tests everything; higher is a quick survey",
	  1, 0, { 0 }, &sample, 0, 4096, 8, NULL },

	{ "Drive settings (sdparm/hdparm, restored on exit)", 0, "Drive look-ahead",
	  "off stops the drive masking a weak sector",
	  0, 2, { "off", "keep" }, NULL, 0, 0, 0, &la_sel },
	{ NULL, 0, "Read cache", "off stops the drive answering any read from DRAM",
	  0, 3, { "keep", "off", "on" }, NULL, 0, 0, 0, &rc_sel },
	{ NULL, 0, "Write cache", "off times the platter instead of the drive's DRAM",
	  0, 3, { "keep", "off", "on" }, NULL, 0, 0, 0, &wc_sel },
	{ NULL, 0, "Auto-reallocate", "lets the drive retire sectors it struggles with",
	  0, 3, { "keep", "off", "on" }, NULL, 0, 0, 0, &awre_sel },
	{ NULL, 0, "Recovery limit (ms)", "how long the drive fights one sector; 0 is forever",
	  1, 0, { 0 }, &rtl, -1, 65535, 500, NULL },
	{ NULL, 0, "Background scan", "SAS: drive sweeps its own media when idle; stays on",
	  0, 2, { "keep", "enable" }, NULL, 0, 0, 0, &bms_sel },
	{ NULL, 0, "Recommended config", "auto-reallocate on, read cache on; saved",
	  0, 2, { "no", "yes" }, NULL, 0, 0, 0, &fix_sel },
	{ NULL, 0, "Save to drive", "permanently keeps the settings above after the scan",
	  0, 2, { "this run", "permanently" }, NULL, 0, 0, 0, &persist_sel },

	{ NULL, 1, "Sector size",
	  "4096 is not offered by every drive; it refuses without erasing",
	  0, 3, { "keep", "512", "4096" }, NULL, 0, 0, 0, &fbs_sel },
	{ NULL, 1, "Protection info",
	  "keep preserves what the drive has; sg_format would strip it",
	  0, 4, { "keep", "none", "type 1", "type 2" },
	  NULL, 0, 0, 0, &fpi_sel },
	{ NULL, 1, "Fast format",
	  "skip visiting every block; hours become minutes when allowed",
	  0, 2, { "no", "yes" }, NULL, 0, 0, 0, &ffast_sel },
	};
	int vis[32], nvis = 0;
	int nf = (int)(sizeof(f) / sizeof(f[0]));
	int nmode = (int)(sizeof(mode_v) / sizeof(mode_v[0]));

	for (i = 0; i < nmode; i++)
		if (mode_v[i] == o->mode)
			mode_sel = i;
	for (i = 0; i < (int)(sizeof(chunk_v) / sizeof(chunk_v[0])); i++)
		if (chunk_v[i] == o->chunk)
			chunk_sel = i;
	for (i = 0; i < NPROFILES; i++)
		if (o->profile && !strcmp(g_profiles[i].name, o->profile))
			prof_sel = i;
	for (i = 0; i < (int)(sizeof(fbs_v) / sizeof(fbs_v[0])); i++)
		if (fbs_v[i] == o->fmt_bs)
			fbs_sel = i;
	for (i = 0; i < (int)(sizeof(fpi_v) / sizeof(fpi_v[0])); i++)
		if (fpi_v[i] == o->fmt_pi)
			fpi_sel = i;
	ffast_sel = o->fmt_fast ? 1 : 0;
	prof_was = prof_sel;
	if (o->repair)
		mode_sel = nmode;       /* the entries past the scan modes */
	if (o->format)
		mode_sel = nmode + 1;
	if (o->apply_only)
		mode_sel = nmode + 2;

	/*
	 * Drives and settings are one cursor space: index < ndev is a drive,
	 * the rest are fields.  Up and down walk straight across the boundary
	 * so nothing has to be switched into before it can be reached.
	 */
	for (;;) {
		int k, nsel = 0, dcur, fcur;

		/*
		 * Low level format is a section of its own, always on the form,
		 * and it does not scroll.  It used to appear only once Mode was
		 * set to format -- the settings above it are long enough that a
		 * short terminal put it screens away -- and hiding it made it
		 * look removed: someone looking for the format settings does
		 * not know they are behind a value of Mode.  So the scan and
		 * drive settings scroll in whatever room is left, and the
		 * format fields, which are the last entries in f[], are drawn
		 * underneath in rows of their own.
		 *
		 * The cursor no longer jumps into the section when format is
		 * chosen, either.  With the section in plain view the jump
		 * earned nothing, and it cost something: the next right-arrow
		 * on Mode changed the sector size instead.
		 */
		nvis = nscroll = 0;
		for (i = 0; i < nf; i++) {
			vis[nvis++] = i;
			if (!f[i].only_format)
				nscroll = nvis;
		}
		if (cur > ndev + nvis - 1)
			cur = ndev + nvis - 1;
		if (cur < 0)
			cur = 0;

		/* 0 the drives, 1 the settings, 2 the format section */
		sec = cur < ndev ? 0 : cur - ndev < nscroll ? 1 : 2;
		dcur = sec == 0 ? cur : -1;
		fcur = sec == 0 ? -1 : cur - ndev;
		if (prof_sel != prof_was) {
			/*
			 * The profile is the question a person can answer;
			 * everything below it is the consequence.  Moving it
			 * resets those, and they stay overridable afterwards.
			 */
			const profile_t *p = &g_profiles[prof_sel];

			prof_was = prof_sel;
			mode_sel = 0;
			for (i = 0; i < nmode; i++)
				if (mode_v[i] == p->mode)
					mode_sel = i;
			if (p->force_remap)
				mode_sel = nmode;       /* the repair entry */
			sample = p->sample ? (long long)p->sample : 0;
			/* reset, not just set: leaving survey has to put the
			 * chunk back or its 1 MiB follows you to every other
			 * profile */
			chunk_sel = 0;
			for (i = 0; p->chunk && i < (int)(sizeof(chunk_v) /
							  sizeof(chunk_v[0])); i++)
				if (chunk_v[i] == p->chunk)
					chunk_sel = i;
		}
		tui_size(&rows, &cols);
		tui_clear();
		g_dash = 0;     /* the form scrolls; messages can go inline */
		nlive = store_live_count();
		printf("\033[1m hddscan " VERSION " \033[0m  configure a run\n");
		depline = 0;
		if (nlive) {
			/*
			 * This form starts *another* run.  Someone who came
			 * back to a machine part way through a shelf needs to
			 * know the work is still going before they configure
			 * anything, and needs a way to it.
			 */
			depline++;
			printf(" %s%d run%s already in progress%s   "
			       "('r' shows them; this form starts another)\n",
			       c_yel(), nlive, nlive == 1 ? "" : "s", c_off());
		}
		{
			char dep[256];

			if (deps_missing_str(dep, sizeof(dep))) {
				depline = 1;
				printf(" %smissing: %s%s   ('hddscan --check-deps' "
				       "prints the install command)\n", c_yel(),
				       dep, c_off());
			}
		}
		printf("\n");

		printf(" %sDrives%s   (space one, a all free HDDs, g this "
		       "controller, n none)\n",
		       sec == 0 ? "\033[7m" : "", sec == 0 ? "\033[0m" : "");
		{
			int show = ndev < 8 ? ndev : 8;

			if (dcur >= 0) {
				if (dcur >= dtop + show)
					dtop = dcur - show + 1;
				if (dcur < dtop)
					dtop = dcur;
			}
			if (dtop > ndev - show)
				dtop = ndev - show;
			if (dtop < 0)
				dtop = 0;
			tui_devices(devs, ndev, pick, dcur, dtop, show);
		}
		for (i = 0; i < ndev; i++)
			if (pick[i])
				nsel++;

		{
			/*
			 * Reserve what the drives, the headings, the profile
			 * line and the footer need; the settings get the rest
			 * and scroll inside it.  Without this the form simply
			 * ran off the bottom of anything shorter than about
			 * thirty rows, taking the profile description and the
			 * start/quit hint with it.
			 */
			int show = ndev < 8 ? ndev : 8;

			/*
			 * Everything that is not a scrolling settings row: the
			 * banner, the optional missing-tools and runs lines, a
			 * blank, the Drives heading, the drive rows, a blank,
			 * the Settings heading, the two group headings inside
			 * it, a blank, the profile line, the selection line, a
			 * blank, the key hints and the newline after them --
			 * and the format section's blank, heading and fields.
			 */
			fshow = rows - show - 13 - depline - (nvis - nscroll + 2);
			if (fshow < 2)
				fshow = 2;
			if (fshow > nscroll)
				fshow = nscroll;
			if (fcur >= 0 && fcur < nscroll) {
				if (fcur >= ftop + fshow)
					ftop = fcur - fshow + 1;
				if (fcur < ftop)
					ftop = fcur;
			}
			if (ftop > nscroll - fshow)
				ftop = nscroll - fshow;
			if (ftop < 0)
				ftop = 0;
		}
		printf("\n %sSettings%s   (left/right changes a value%s)\n",
		       sec == 1 ? "\033[7m" : "", sec == 1 ? "\033[0m" : "",
		       fshow < nscroll ? ", more below" : "");
		for (i = ftop; i < nscroll && i < ftop + fshow; i++) {
			field_t *fp = &f[vis[i]];

			if (fp->head)
				printf("  %s-- %s --%s\n", c_grn(), fp->head,
				       c_off());
			form_row(fp, fcur == i, cols);
		}

		/* the format section: its own heading, never scrolled away */
		printf("\n %sLow level format%s   %s%s%s\n",
		       sec == 2 ? "\033[7m" : "", sec == 2 ? "\033[0m" : "",
		       mode_sel == nmode + 1 ? c_red() : "",
		       mode_sel == nmode + 1 ?
		       "Mode is format: the selected drives are erased" :
		       "used when Mode is set to format",
		       mode_sel == nmode + 1 ? c_off() : "");
		for (i = nscroll; i < nvis; i++)
			form_row(&f[vis[i]], fcur == i, cols);

		printf("\n  %s%s%s: %s\n", c_grn(), g_profiles[prof_sel].name,
		       c_off(), g_profiles[prof_sel].what);
		printf("  %d drive%s selected", nsel, nsel == 1 ? "" : "s");
		if (mode_sel == nmode)
			printf("   %soverwrites everything, then tries to heal "
			       "what is weak%s", c_red(), c_off());
		else if (mode_sel == nmode + 1)
			printf("   %sthis ERASES the drive completely%s",
			       c_red(), c_off());
		else if (mode_sel == nmode + 2)
			printf("   %sapplies the settings and stops, no scan%s",
			       c_yel(), c_off());
		else if (mode_sel >= 2)
			printf("   %sthis mode writes to the drive%s",
			       c_red(), c_off());
		if (bms_sel)
			printf("   %sbackground scan stays on afterwards%s",
			       c_yel(), c_off());
		if (persist_sel || fix_sel)
			printf("   %ssettings are saved to the drive%s",
			       c_yel(), c_off());
		if (arm)
			printf("\n\n  %spress y to start a %s pass on %d "
			       "drive%s%s, any other key to cancel%s\n",
			       c_red(), mode_c[mode_sel], nsel,
			       nsel == 1 ? "" : "s",
			       (persist_sel || fix_sel) ?
			       " and save the settings to them" : "",
			       c_off());
		else
			/*
			 * Eighty columns, always.  A footer that wraps makes the
			 * form a line taller than the row budget above assumed,
			 * and the terminal scrolls the top of it away.  The
			 * banner already says when runs are in progress.
			 */
			printf("\n\n  up/down move  space picks  left/right "
			       "sets  s start  r runs  q quit\n");
		fflush(stdout);

		k = tui_key(200);
		if (k < 0)
			continue;
		if (arm) {
			/* the second key of a two-key start; anything that is
			 * not a yes puts the form back the way it was */
			arm = 0;
			if (k != 'y')
				continue;
			k = 'S';
		}
		if (k == 'q' || k == 3)
			return 0;
		if (k == 'r') {
			/*
			 * The way back to work already in progress.  A form
			 * with no exit but "start" or "quit" is how you end
			 * up killing a terminal to get at a scan you left
			 * running.
			 */
			return 2;
		}
		if (k == '\t') {
			/* up and down already cross the boundary; tab is kept
			 * because the section headings invite it */
			cur = sec == 0 ? ndev : sec == 1 ? ndev + nscroll : 0;
			continue;
		}
		if (k == K_UP) {
			if (cur > 0)
				cur--;
			continue;
		}
		if (k == K_DOWN) {
			if (cur < ndev + nvis - 1)
				cur++;
			continue;
		}
#define USABLE(x) (!devs[x].unsafe && devs[x].rotational && \
		   (devs[x].is_file || transport_is_local(devs[x].transport)))
		if (k == 'a') {
			for (i = 0; i < ndev; i++)
				pick[i] = USABLE(i);
			continue;
		}
		if (k == 'n') {
			for (i = 0; i < ndev; i++)
				pick[i] = 0;
			continue;
		}
		if (k == 'g' && dcur >= 0) {
			/* every free HDD on the same controller as the one
			 * under the cursor: one keystroke per shelf rather
			 * than one per drive */
			const char *g = devs[dcur].group;

			for (i = 0; i < ndev; i++)
				if (!strcmp(devs[i].group, g) && USABLE(i))
					pick[i] = 1;
			continue;
		}
		if (k == 's' || k == 'S') {
			if (!nsel)
				continue;
			/* a writing mode is one keystroke from destroying a
			 * drive now that the form is the default entry point,
			 * so make it two */
			if (k == 's' && ((mode_sel >= 2 &&
					  mode_sel != nmode + 2) ||
					 persist_sel || fix_sel)) {
				arm = 1;
				continue;
			}
			/*
			 * The form comes back after every run, so a dispatch
			 * flag from the previous accept must not leak into
			 * this one: a leftover repair flag would silently turn
			 * a freshly chosen read pass into a write pass.
			 */
			o->repair = 0;
			o->format = 0;
			o->apply_only = 0;
			if (mode_sel == nmode)
				o->repair = 1;
			else if (mode_sel == nmode + 1)
				o->format = 1;
			else if (mode_sel == nmode + 2)
				o->apply_only = 1;
			else
				o->mode = mode_v[mode_sel];
			o->chunk = chunk_v[chunk_sel];
			o->fmt_bs = fbs_v[fbs_sel];
			o->fmt_pi = fpi_v[fpi_sel];
			o->fmt_fast = ffast_sel;
			/*
			 * The extras a profile carries are not fields on this
			 * form, so they follow the profile rather than the
			 * mode the user may have overridden below it.
			 */
			o->profile = g_profiles[prof_sel].name;
			o->rewrite_weak = g_profiles[prof_sel].rewrite_weak;
			o->force_remap = g_profiles[prof_sel].force_remap;
			o->second_pass = g_profiles[prof_sel].second_pass;
			o->order = (order_t)order_sel;
			o->lookahead = la_sel == 0 ? 0 : -1;
			o->bms = bms_sel;
			o->write_cache = wc_sel == 0 ? -1 : wc_sel - 1;
			o->persist = persist_sel;
			o->fix_config = fix_sel;
			o->awre = o->arre = awre_sel == 0 ? -1 : awre_sel - 1;
			o->read_cache = rc_sel == 0 ? -1 : rc_sel - 1;
			o->recovery_ms = rtl;
			/* only a look-ahead the user actually moved counts as
			 * asked for; see the note in scan_device() */
			if (la_sel != la_init)
				o->lookahead_set = 1;
			o->retries = (int)retries;
			o->segment = (uint64_t)segment_mb << 20;
			o->max_parallel = (int)parallel;
			o->sample = (uint64_t)(sample > 1 ? sample : 0);
			return 1;
		}
		if (sec == 0) {
			if (k == ' ')
				pick[cur] = !pick[cur];
		} else if (k == K_LEFT || k == K_RIGHT) {
			int d = (k == K_RIGHT) ? 1 : -1;
			field_t *fp = &f[vis[fcur]];

			if (fp->kind == 0) {
				int v = *fp->sel + d;

				if (v < 0)
					v = fp->nchoices - 1;
				if (v >= fp->nchoices)
					v = 0;
				*fp->sel = v;
			} else {
				long long v;

				/*
				 * A field whose floor is the "keep" sentinel
				 * steps between keep and zero rather than by
				 * its step size, so leaving keep lands on a
				 * round number instead of on -1 + step.
				 */
				if (fp->lo < 0 && *fp->num < 0 && d > 0)
					v = 0;
				else if (fp->lo < 0 && *fp->num == 0 && d < 0)
					v = fp->lo;
				else
					v = *fp->num + d * fp->step;

				if (v < fp->lo)
					v = fp->lo;
				if (v > fp->hi)
					v = fp->hi;
				*fp->num = v;
			}
		}
	}
}

/*
 * Start the worker for one drive.  The worker is the only process that ever
 * touches that drive and the only one that writes its job record; it has no
 * terminal, writes its report to a file, and keeps going whether or not
 * anything is watching.
 */
static void spawn_job(job_t *jobs, int n, int idx, const opts_t *o,
		      const run_t *r)
{
	job_t *j = &jobs[idx];
	pid_t pid;

	pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));

	if (pid == 0) {
		char jb[PATH_MAX], cb[PATH_MAX], bb[PATH_MAX], pb[PATH_MAX];
		char sb[PATH_MAX], ck[STORE_MAX + 128];
		opts_t wo = *o;
		int st;

		g_quiet = 1;
		g_color = 0;
		g_dash = 0;
		if (g_log)
			fclose(g_log);
		g_log = fopen(j->report, "we");
		/*
		 * The record this worker owns.  It claims it by writing its
		 * own pid into it: the supervisor cannot, because the worker
		 * may have overtaken it and written progress already.
		 */
		if (jrec_get(j->file, &g_job) < 0)
			memset(&g_job, 0, sizeof(g_job));
		snprintf(g_job.file, sizeof(g_job.file), "%s", j->file);
		g_job.pid = getpid();
		g_job.pidstart = pid_start(getpid());
		g_job.started = (long long)time(NULL);
		g_job.verdict = -1;
		snprintf(g_job.report, sizeof(g_job.report), "%s", j->report);
		g_job_own = 1;
		job_flush(JS_RUNNING);
		/*
		 * Every drive gets its own checkpoint, inside the run.  One
		 * --state path shared by a whole parallel run had every
		 * worker writing over the others, so nothing could be
		 * resumed; a run directory gives each drive a place of its
		 * own without anyone having to name one.
		 */
		{
			char slug[96];

			report_slug(j->dev->name, slug, sizeof(slug));
			snprintf(ck, sizeof(ck), "%s/%s.ckpt", r->dir, slug);
		}
		/*
		 * An explicit --state is one path per drive here, exactly as
		 * --json and --csv are.  A whole parallel run sharing one
		 * checkpoint file meant every worker overwrote the others and
		 * --resume had nothing usable to come back to.
		 */
		wo.state = wo.state ?
			   derive_path(wo.state, j->dev->name, 1, sb, sizeof(sb)) :
			   ck;
		st = scan_device(j->dev, &wo,
				 derive_path(o->json, j->dev->name, 1, jb, sizeof(jb)),
				 derive_path(o->csv, j->dev->name, 1, cb, sizeof(cb)),
				 derive_path(o->badblocks, j->dev->name, 1, bb, sizeof(bb)),
				 derive_path(o->prometheus, j->dev->name, 1, pb,
					     sizeof(pb)));
		if (g_log)
			fclose(g_log);
		g_job.verdict = st;
		g_job.ended = (long long)time(NULL);
		/*
		 * A drive that was stopped part way keeps the percentage it
		 * reached: it is the honest number, and the report says the
		 * same thing on its Coverage line.  Only a scan that ran to
		 * the end covered the whole drive, whatever its verdict.
		 */
		if (st != 130)
			g_job.pct = 100.0;
		g_job.rate = 0;
		job_flush(st == 130 ? JS_STOPPED : JS_DONE);
		_exit(st);
	}

	j->pid = pid;
	j->pidstart = pid_start(pid);
	j->started = 1;
}

/*
 * A worker that ended without writing its own verdict -- killed, or gone
 * before it could -- would leave a record claiming to be running forever, so
 * the supervisor patches in how each one actually ended.  Read, modify,
 * write: the worker put real numbers in there and none of them should be
 * lost to this.
 */
static void job_reap(job_t *j, int status)
{
	jrec_t x;

	j->done = 1;
	if (jrec_get(j->file, &x) < 0)
		return;
	snprintf(x.file, sizeof(x.file), "%s", j->file);
	x.verdict = WIFEXITED(status) ? WEXITSTATUS(status) :
		    WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 2;
	x.ended = (long long)time(NULL);
	x.updated = x.ended;
	if (x.verdict != 130 && !WIFSIGNALED(status))
		x.pct = 100.0;
	x.rate = 0;
	if (WIFSIGNALED(status) && !x.note[0])
		snprintf(x.note, sizeof(x.note), "killed by signal %d",
			 WTERMSIG(status));
	x.state = (x.verdict == 130 || x.verdict == 128 + SIGINT ||
		   x.verdict == 128 + SIGTERM) ? JS_STOPPED : JS_DONE;
	jrec_put(&x);
}

/*
 * Drive settings are applied to every selected drive here, before anything
 * else happens, rather than inside each scan worker as it reaches its drive.
 * Two reasons.  A setting the user asked for should take effect when they
 * asked for it, not hours into a scan; and a saved setting is the point of
 * the exercise for a shelf of drives being prepared, where waiting for a scan
 * that may not be wanted at all makes no sense.  It also puts the run-scoped
 * restore list in the parent, so a Ctrl-C during a parallel run puts every
 * drive back rather than only the ones whose workers got the signal.
 */
static int settings_wanted(const opts_t *o)
{
	return o->fix_config || o->bms || o->awre >= 0 || o->arre >= 0 ||
	       o->read_cache_set || o->recovery_ms >= 0 ||
	       o->write_cache >= 0 || o->rd_retries >= 0 || o->wr_retries >= 0;
}

static void apply_settings(device_t *t, int n, const opts_t *o)
{
	int i;

	if (!settings_wanted(o) && o->read_cache < 0)
		return;
	if (settings_wanted(o))
		out("\n  Applying settings to %d drive%s, %s\n", n,
		    n == 1 ? "" : "s",
		    (o->persist || o->fix_config) ? "saved to the drive" :
		    "for this run only");
	for (i = 0; i < n; i++) {
		device_t *d = &t[i];

		/*
		 * --fix-config is the advice the report prints, carried out.
		 * It saves what it changes: a configuration fix that reverted
		 * when the run ended would not be a fix.
		 */
		if (o->fix_config) {
			sd_field_apply(d, "AWRE", 1, 1,
				       "auto-reallocate on write");
			sd_field_apply(d, "ARRE", 1, 1,
				       "auto-reallocate on read");
			sd_field_apply(d, "RCD", 0, 1, "read cache");
		}
		if (o->awre >= 0)
			sd_field_apply(d, "AWRE", o->awre, o->persist,
				       "auto-reallocate on write");
		if (o->arre >= 0)
			sd_field_apply(d, "ARRE", o->arre, o->persist,
				       "auto-reallocate on read");
		if (o->read_cache >= 0)
			sd_field_apply(d, "RCD", !o->read_cache, o->persist,
				       "the read cache setting");
		if (o->recovery_ms >= 0)
			sd_field_apply(d, "RTL", o->recovery_ms, o->persist,
				       "recovery time limit (ms)");
		if (o->rd_retries >= 0)
			sd_field_apply(d, "RRC", o->rd_retries, o->persist,
				       "read retry count");
		if (o->wr_retries >= 0)
			sd_field_apply(d, "WRC", o->wr_retries, o->persist,
				       "write retry count");
		if (o->write_cache >= 0)
			wc_set(d, o->write_cache, o->persist);
		if (o->bms)
			bms_enable(d);
	}
	out("\n");
}

/* ------------------------------------------------------------------ *
 * low level format
 *
 * FORMAT UNIT rewrites every sector header on the drive.  It is the only way
 * to change the logical block size or to add or strip protection information,
 * and it is the fix for a drive whose PI was left behind by a previous owner.
 * It is also irreversible, takes hours, and a power cut partway through can
 * leave a drive needing manufacturer tooling to revive.
 *
 * So: the exact sg_format command is printed for every drive before anything
 * runs, and nothing runs without the same --confirm a write mode needs.  The
 * protection setting is always passed explicitly, because sg_format defaults
 * --fmtpinfo to 0 and a plain "change the block size" would otherwise strip
 * protection information as a side effect nobody asked for.
 * ------------------------------------------------------------------ */

/* FMTPINFO / PROTECTION FIELD USAGE encoding for a wanted protection type */
static int fmt_pi_bits(int type, int *pfu)
{
	*pfu = 0;
	switch (type) {
	case 0: return 0;
	case 1: return 2;
	case 2: return 3;
	case 3: *pfu = 1; return 3;
	default: return -1;
	}
}

static void format_cmd(const device_t *d, const opts_t *o, char *cmd, size_t n)
{
	int bs = o->fmt_bs ? o->fmt_bs : d->logical_bs;
	int pfu = 0, fpi;

	fpi = fmt_pi_bits(o->fmt_pi >= 0 ? o->fmt_pi : d->prot_type, &pfu);
	/*
	 * A fast format does not visit every block.  It is only meaningful
	 * when the *physical* sector layout is not changing -- on a 512e drive
	 * the platters are already 4096-byte sectors, so moving the logical
	 * size to 4096 is a remapping rather than a re-lay-out, and the
	 * firmware may well accept it in minutes instead of hours.  Whether it
	 * does is the drive's decision; refusing costs nothing.
	 */
	snprintf(cmd, n,
		 "sg_format --format --quick%s --size=%d --fmtpinfo=%d%s /dev/%s",
		 o->fmt_fast ? " --ffmt=1" : "", bs, fpi,
		 pfu ? " --pfu=1" : "", d->name);
}

/*
 * sg3_utils exit codes carry the sense key that caused them, which is a lot
 * more use than a number.  19 in particular is the one a block size change
 * hits: the drive rejecting the MODE SELECT parameter list, which nearly
 * always means it will not accept that logical block size at all.
 */
static const char *format_why(int st)
{
	switch (st) {
	case 1:  return "syntax error in the sg_format command line";
	case 2:  return "the drive is not ready";
	case 3:  return "medium error";
	case 5:  return "illegal request: the drive rejected the command";
	case 6:  return "unit attention: the drive was reset or the media "
			"changed; try again";
	case 9:  return "illegal request, invalid field in the command -- the "
			"drive does not support what was asked for";
	case 11: return "the drive aborted the command";
	case 15: return "could not open the device";
	case 19: return "the drive rejected the parameter list. For a block "
			"size change this almost always means this drive does "
			"not support that logical block size";
	case 24: return "reservation conflict: something else holds the drive";
	case 33: return "the command timed out";
	default: return NULL;
	}
}

/*
 * sg_format reports its progress as a percentage on a line it keeps
 * rewriting.  Pull the last percentage out of whatever it just said; a line
 * with no percentage in it (and there are several) simply leaves the figure
 * where it was.
 */
static int fmt_pct(const char *s, double *pct)
{
	const char *p, *q;
	int got = 0;

	for (p = s; *p; p++) {
		if (*p != '%')
			continue;
		q = p;
		while (q > s && (isdigit((unsigned char)q[-1]) || q[-1] == '.'))
			q--;
		if (q == p)
			continue;
		*pct = atof(q);
		got = 1;
	}
	return got;
}

/*
 * One drive's format, watched from start to finish by a process of its own.
 * It streams sg_format's output into a log beside the reports and keeps the
 * drive's job record up to date, which is the only reason a shelf of thirty
 * formats can be looked in on hours later from a different terminal.
 */
static void format_child(const run_t *r, const device_t *d, const opts_t *o,
			 const char *jobfile, const char *logpath)
{
	char cmd[512], buf[512];
	size_t len = 0;
	int pfd[2], st = 0;
	pid_t pid;
	FILE *lf;

	if (jrec_get(jobfile, &g_job) < 0)
		memset(&g_job, 0, sizeof(g_job));
	snprintf(g_job.file, sizeof(g_job.file), "%s", jobfile);
	g_job.pid = getpid();
	g_job.pidstart = pid_start(getpid());
	g_job.started = (long long)time(NULL);
	g_job.verdict = -1;
	snprintf(g_job.report, sizeof(g_job.report), "%s", logpath);
	snprintf(g_job.note, sizeof(g_job.note), "FORMAT UNIT issued");
	g_job_own = 1;
	job_flush(JS_RUNNING);

	format_cmd(d, o, cmd, sizeof(cmd));
	lf = fopen(logpath, "we");
	if (lf) {
		fprintf(lf, "# %s\n# %s\n", d->path, cmd);
		fflush(lf);
	}
	if (pipe(pfd) < 0)
		_exit(2);
	pid = fork();
	if (pid < 0)
		_exit(2);
	if (!pid) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		dup2(pfd[1], STDERR_FILENO);
		close(pfd[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);

	for (;;) {
		ssize_t got = read(pfd[0], buf + len, sizeof(buf) - len - 1);

		if (got <= 0) {
			if (got < 0 && errno == EINTR) {
				/*
				 * A stop reaches the watcher, not the platter.
				 * FORMAT UNIT runs inside the drive and cannot
				 * be called back, so say so instead of
				 * implying the drive is idle again.
				 */
				if (g_stop) {
					kill(pid, SIGINT);
					snprintf(g_job.note, sizeof(g_job.note),
						 "stopped watching; the drive "
						 "is probably still formatting");
					job_flush(JS_RUNNING);
				}
				continue;
			}
			break;
		}
		len += (size_t)got;
		buf[len] = 0;
		for (;;) {
			/*
			 * sg_format rewrites one line with a carriage return
			 * as it goes, so \r has to end a line here too.
			 * Splitting on \n alone meant a multi-hour format
			 * showed nothing at all until it finished.
			 */
			char *nl = memchr(buf, '\n', len);
			char *cr = memchr(buf, '\r', len);
			size_t k;

			if (!nl || (cr && cr < nl))
				nl = cr;
			if (!nl)
				break;
			*nl = 0;
			if (nl != buf) {
				double pct;

				if (lf) {
					fprintf(lf, "%s\n", buf);
					fflush(lf);
				}
				snprintf(g_job.note, sizeof(g_job.note),
					 "%s", trim(buf));
				if (fmt_pct(buf, &pct))
					g_job.pct = pct;
				job_flush(JS_RUNNING);
			}
			k = (size_t)(nl - buf) + 1;
			memmove(buf, buf + k, len - k);
			len -= k;
		}
		if (len >= sizeof(buf) - 1)
			len = 0;        /* absurdly long line; drop it */
	}
	if (len) {
		buf[len] = 0;
		if (lf)
			fprintf(lf, "%s\n", buf);
		snprintf(g_job.note, sizeof(g_job.note), "%s", trim(buf));
	}
	close(pfd[0]);
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (lf)
		fclose(lf);

	if (WIFEXITED(st) && !WEXITSTATUS(st)) {
		g_job.pct = 100.0;
		g_job.verdict = 0;
		snprintf(g_job.note, sizeof(g_job.note), "format complete");
	} else if (WIFSIGNALED(st)) {
		g_job.verdict = 2;
		snprintf(g_job.note, sizeof(g_job.note),
			 "sg_format killed by signal %d; the drive may still "
			 "be formatting", WTERMSIG(st));
	} else {
		int code = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
		const char *why = format_why(code);

		g_job.verdict = 2;
		snprintf(g_job.note, sizeof(g_job.note),
			 "sg_format exit %d%s%s", code, why ? ": " : "",
			 why ? why : "");
	}
	g_job.ended = (long long)time(NULL);
	job_flush(g_stop ? JS_STOPPED : JS_DONE);
	_exit(g_job.verdict);
}

/*
 * Every selected drive is formatted at once -- a format is the drive's own
 * work, not this machine's, so there is nothing to be gained by queueing
 * them -- and each gets a watcher process that outlives this one.
 */
static int format_run(const run_t *r, job_t *jobs, device_t *t, int n,
		      const opts_t *o)
{
	int i, worst = 0, live = 0;

	for (i = 0; i < n; i++) {
		pid_t pid = fork();

		if (pid < 0)
			die("fork: %s", strerror(errno));
		if (!pid)
			format_child(r, jobs[i].dev, o, jobs[i].file,
				     jobs[i].report);
		jobs[i].pid = pid;
		jobs[i].pidstart = pid_start(pid);
		jobs[i].started = 1;
		live++;
	}
	while (live > 0) {
		int st;
		pid_t p = waitpid(-1, &st, 0);

		if (p < 0) {
			if (errno == EINTR) {
				if (g_stop)
					forward_stop(jobs, n);
				continue;
			}
			break;
		}
		for (i = 0; i < n; i++) {
			int code;

			if (jobs[i].pid != p || jobs[i].done)
				continue;
			job_reap(&jobs[i], st);
			code = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
			if (code > worst)
				worst = code;
			live--;
		}
	}
	return worst;
}

/* ------------------------------------------------------------------ *
 * runs: creating them, owning them, following them
 * ------------------------------------------------------------------ */

/* the short name of a scan mode, for a column that has to stay narrow */
static const char *mode_word(scan_mode_t m)
{
	switch (m) {
	case MODE_READ: return "read";
	case MODE_VERIFY: return "verify";
	case MODE_WRITE: return "write";
	case MODE_CHECK: return "check";
	}
	return "scan";
}

/*
 * What this run is, said twice: once in the few words the run list has room
 * for, and once in full for the header of the screen that is watching it.
 * The short form is what someone picks a run out of a list by; the long form
 * is what tells them what is being done to twenty-four drives.
 */
static void run_what(run_t *r, const opts_t *o, int format)
{
	if (!format) {
		snprintf(r->what, sizeof(r->what), "%s (%s)",
			 mode_word(o->mode), o->profile ? o->profile : "?");
		snprintf(r->detail, sizeof(r->detail), "%s", mode_name(o->mode));
		return;
	}
	snprintf(r->what, sizeof(r->what), "format %s%s%s",
		 o->fmt_bs ? (o->fmt_bs == 4096 ? "4096B " : "512B ") : "",
		 o->fmt_pi < 0 ? "PI kept" :
		 o->fmt_pi == 0 ? "no PI" :
		 o->fmt_pi == 1 ? "PI type 1" :
		 o->fmt_pi == 2 ? "PI type 2" : "PI type 3",
		 o->fmt_fast ? " fast" : "");
	snprintf(r->detail, sizeof(r->detail),
		 "low level format, %s sectors, %s%s",
		 o->fmt_bs ? (o->fmt_bs == 4096 ? "4096 byte" : "512 byte") :
		 "unchanged",
		 o->fmt_pi < 0 ? "protection information kept" :
		 o->fmt_pi == 0 ? "no protection information" :
		 o->fmt_pi == 1 ? "protection type 1" :
		 o->fmt_pi == 2 ? "protection type 2" : "protection type 3",
		 o->fmt_fast ? ", fast" : "");
}

/*
 * Lay a run out on disk: the directory, the run record, and one job record
 * per drive saying it is queued.  Everything that comes later -- the
 * supervisor, the workers, every viewer -- addresses the run through these
 * files and never through this process.
 */
static int run_create(run_t *r, device_t *t, int n, const opts_t *o,
		      int format, job_t *jobs)
{
	time_t now = time(NULL);
	struct tm tmv;
	int i;

	memset(r, 0, sizeof(*r));
	localtime_r(&now, &tmv);
	/*
	 * The id is the timestamp, because it is what a person has to read
	 * off a screen, type into --status and recognise a week later.  Two
	 * runs starting in the same second are told apart by a suffix, and
	 * the directory itself is what arbitrates: creating it exclusively is
	 * the only test for "someone already has this id" that cannot race.
	 */
	{
		char base[STORE_MAX];
		int try;

		if (snprintf(base, sizeof(base), "%s/runs", store_root()) >=
		    (int)sizeof(base) || mkpath(base) < 0) {
			msg(PROG ": cannot create %s: %s\n", base,
			    strerror(errno));
			return -1;
		}
		for (try = 0; try < 100; try++) {
			snprintf(r->id, sizeof(r->id),
				 "%04d%02d%02d-%02d%02d%02d",
				 tmv.tm_year + 1900, tmv.tm_mon + 1,
				 tmv.tm_mday, tmv.tm_hour, tmv.tm_min,
				 tmv.tm_sec);
			if (try)
				snprintf(r->id + strlen(r->id),
					 sizeof(r->id) - strlen(r->id),
					 "-%d", try + 1);
			if (snprintf(r->dir, sizeof(r->dir), "%s/%s", base,
				     r->id) >= (int)sizeof(r->dir)) {
				msg(PROG ": %s is too deep a path to keep run "
				    "records in\n", store_root());
				return -1;
			}
			if (!mkdir(r->dir, 0755))
				break;
			if (errno != EEXIST) {
				msg(PROG ": cannot create %s: %s\n", r->dir,
				    strerror(errno));
				return -1;
			}
		}
		if (try >= 100) {
			msg(PROG ": cannot find an unused run id under %s\n",
			    base);
			return -1;
		}
	}
	snprintf(r->kind, sizeof(r->kind), "%s", format ? "format" : "scan");
	run_what(r, o, format);
	snprintf(r->profile, sizeof(r->profile), "%s",
		 o->profile ? o->profile : "");
	snprintf(r->outdir, sizeof(r->outdir), "%s", o->outdir);
	r->created = (long long)now;
	r->njobs = n;
	snprintf(g_own_run, sizeof(g_own_run), "%s", r->id);
	run_put(r);

	for (i = 0; i < n; i++) {
		char slug[96];
		jrec_t j;

		report_slug(t[i].name, slug, sizeof(slug));
		memset(jobs + i, 0, sizeof(jobs[i]));
		jobs[i].dev = &t[i];
		snprintf(jobs[i].file, sizeof(jobs[i].file), "%s/%s.job",
			 r->dir, slug);
		snprintf(jobs[i].report, sizeof(jobs[i].report),
			 "%s/" PROG "-%s%s.txt", o->outdir,
			 format ? "format-" : "", slug);

		memset(&j, 0, sizeof(j));
		snprintf(j.file, sizeof(j.file), "%s", jobs[i].file);
		snprintf(j.dev, sizeof(j.dev), "%s", t[i].name);
		snprintf(j.path, sizeof(j.path), "%s", t[i].path);
		snprintf(j.model, sizeof(j.model), "%s", t[i].model);
		snprintf(j.serial, sizeof(j.serial), "%s", t[i].serial);
		snprintf(j.report, sizeof(j.report), "%s", jobs[i].report);
		j.size = t[i].size;
		j.state = JS_QUEUED;
		j.verdict = -1;
		j.updated = (long long)now;
		jrec_put(&j);
	}
	store_prune();
	return 0;
}

/* a drive that never got its turn, or one whose worker was told to stop */
static void job_cancel(const job_t *j, const char *why)
{
	jrec_t x;

	if (jrec_get(j->file, &x) < 0)
		return;
	snprintf(x.file, sizeof(x.file), "%s", j->file);
	x.state = JS_STOPPED;
	x.verdict = 130;
	x.ended = x.updated = (long long)time(NULL);
	snprintf(x.note, sizeof(x.note), "%s", why);
	jrec_put(&x);
}

/*
 * The scan schedule: keep maxpar drives busy, one worker each, until every
 * drive has had its turn.  Nothing is rendered here -- this process has no
 * terminal, by design -- so it blocks in waitpid() and costs nothing while
 * the drives work.
 */
static int scan_run(const run_t *r, job_t *jobs, device_t *t, int n,
		    const opts_t *o)
{
	int maxpar = o->max_parallel > 0 ? o->max_parallel : n;
	int launched = 0, live = 0, finished = 0, worst = 0, i;

	if (maxpar > n)
		maxpar = n;
	while (finished < n) {
		pid_t p;
		int st;

		while (live < maxpar && launched < n && !g_stop) {
			spawn_job(jobs, n, launched++, o, r);
			live++;
		}
		if (!live) {
			/* the stop arrived before the queue had drained */
			for (i = launched; i < n; i++) {
				job_cancel(&jobs[i], "run stopped before this "
					   "drive was reached");
				jobs[i].done = 1;
				finished++;
				if (worst < 130)
					worst = 130;
			}
			break;
		}
		p = waitpid(-1, &st, 0);
		if (p < 0) {
			if (errno == EINTR) {
				if (g_stop)
					forward_stop(jobs, n);
				continue;
			}
			break;
		}
		for (i = 0; i < n; i++) {
			int code;

			if (jobs[i].pid != p || jobs[i].done)
				continue;
			job_reap(&jobs[i], st);
			code = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
			if (code > worst)
				worst = code;
			live--;
			finished++;
		}
		if (g_stop)
			forward_stop(jobs, n);
	}
	return worst;
}

/*
 * Hand the run to a process of its own and come back.
 *
 * This is the whole point of the run store.  The supervisor leaves the
 * terminal's session, so nothing that happens to the terminal reaches it: the
 * window can be closed, the program can be quit, ssh can drop, and thirty
 * formats carry on.  Whoever wants to watch reads the run's records.
 *
 * The drive settings go with it (invariant 7b).  They are applied by the
 * process that owns the run, before the scan starts, and put back when the
 * run ends -- not when a viewer exits, which would quietly change what the
 * remaining drives are being measured under.
 */
static int run_begin(run_t *r, device_t *t, int n, const opts_t *o, int format)
{
	job_t *jobs = calloc((size_t)n, sizeof(job_t));
	pid_t pid;

	if (!jobs)
		die("out of memory");
	if (run_create(r, t, n, o, format, jobs) < 0) {
		free(jobs);
		return -1;
	}
	fflush(NULL);
	pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (!pid) {
		char path[sizeof(r->dir) + 16];
		int fd, st;

		setsid();
		fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			close(fd);
		}
		/*
		 * Everything the run has to say on the way past goes to a log
		 * inside the run.  It cannot go to the terminal: the terminal
		 * is showing a dashboard drawn by absolute cursor position,
		 * and may well not be there at all by then.
		 */
		snprintf(path, sizeof(path), "%s/log", r->dir);
		fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		g_tty_raw = 0;   /* the terminal belongs to the process we left */
		g_dash = 0;
		g_color = 0;
		g_quiet = 0;
		r->sup = getpid();
		r->supstart = pid_start(getpid());
		run_put(r);
		apply_settings(t, n, o);
		st = format ? format_run(r, jobs, t, n, o) :
			      scan_run(r, jobs, t, n, o);
		r->ended = (long long)time(NULL);
		run_put(r);
		/*
		 * exit(), not _exit(): the run-scoped drive settings are put
		 * back by atexit handlers, and this is the process that
		 * changed them.
		 */
		exit(st);
	}
	r->sup = pid;
	r->supstart = pid_start(pid);
	free(jobs);
	return 0;
}

/* the worst verdict any drive in a run reached, which is the exit status */
static int run_worst(const jrec_t *j, int n)
{
	int i, worst = 0;

	for (i = 0; i < n; i++)
		if (j[i].verdict > worst)
			worst = j[i].verdict;
	return worst;
}

static void run_fleet_summary(const run_t *r, const jrec_t *j, int n)
{
	int format = !strcmp(r->kind, "format");
	char b[32];
	int i;

	out("\n");
	out("========================================================================\n");
	out(" Fleet summary - %d drives, %s\n", n,
	    human_time(run_elapsed(r), b, sizeof(b)));
	out("========================================================================\n");
	out("  %-8s %-10s %-22s %-18s %-10s %s\n",
	    "DEVICE", "SIZE", "MODEL", "SERIAL", "VERDICT", "REPORT");
	for (i = 0; i < n; i++) {
		const jrec_t *x = &j[i];

		out("  %-8s %-10s %-22.22s %-18.18s %s%-10s%s %s\n",
		    jrec_name(x), human_size(x->size, b, sizeof(b)),
		    x->model[0] ? x->model : "?",
		    x->serial[0] ? x->serial : "?",
		    jrec_color(x), jrec_word(x, format), c_off(), x->report);
	}
	out("========================================================================\n\n");
	out("  run %s   its records are in %s\n\n", r->id, r->dir);
}

/* re-read a run and its jobs; -1 once the run is gone from the store */
static int run_reload(const char *id, run_t *r, jrec_t **j, int *n)
{
	const char *roots[2];
	int nroot = store_roots(roots, 2), i, got = -1;
	char dir[STORE_MAX + 300];

	for (i = 0; i < nroot && got < 0; i++) {
		snprintf(dir, sizeof(dir), "%s/runs/%s", roots[i], id);
		got = run_get(dir, r);
	}
	if (got < 0)
		return -1;
	*n = store_jobs(r, j);
	return 0;
}

/*
 * Follow a run to the end without the dashboard: a scripted invocation, a
 * pipe, a log.  Ctrl-C here stops the run rather than only this view of it,
 * which is what it has always done and what a script expects; walking away
 * and leaving it running is what --detach is for.
 */
static int run_follow(run_t *rin, const opts_t *o)
{
	char id[48];
	int prev_lines = 0, worst = 0, format = !strcmp(rin->kind, "format");
	int stopped = 0;
	char (*seen)[160] = NULL;
	jrec_t *j = NULL;
	run_t r = *rin;
	int n = 0;

	snprintf(id, sizeof(id), "%s", rin->id);
	for (;;) {
		tally_t t;
		int i;

		free(j);
		j = NULL;
		if (run_reload(id, &r, &j, &n) < 0)
			break;
		tally(j, n, &t);

		if (g_stop && !stopped) {
			stopped = 1;
			store_stop(&r);
			msg(PROG ": stopping run %s\n", r.id);
		}
		if (!g_quiet) {
			if (format) {
				/*
				 * A format has nothing to show but what the
				 * drive says, so print each drive's messages
				 * as they change, tagged with the drive they
				 * came from.  Several drives formatting at
				 * once would otherwise interleave into
				 * nonsense.
				 */
				if (!seen)
					seen = calloc((size_t)(n > 0 ? n : 1),
						      sizeof(*seen));
				for (i = 0; seen && i < n; i++) {
					if (!j[i].note[0] ||
					    !strcmp(seen[i], j[i].note))
						continue;
					snprintf(seen[i], sizeof(seen[i]), "%s",
						 j[i].note);
					out("  %-8s %s\n", jrec_name(&j[i]),
					    j[i].note);
				}
			} else {
				prev_lines = render_parallel(&r, j, n,
							     prev_lines,
							     !r.live);
			}
		}
		if (n > 0 && !r.live && !t.running && !t.queued)
			break;
		poll(NULL, 0, 250);
	}
	if (j) {
		worst = run_worst(j, n);
		if (!format)
			run_fleet_summary(&r, j, n);
		/* with a handful of drives the full reports are still
		 * readable inline */
		if (!format && n <= 8) {
			int i;

			for (i = 0; i < n; i++) {
				FILE *f = fopen(j[i].report, "re");
				char line[4096];

				if (!f)
					continue;
				while (fgets(line, sizeof(line), f))
					out("%s", line);
				fclose(f);
			}
		}
	}
	free(seen);
	free(j);
	return worst;
}

/* ------------------------------------------------------------------ *
 * --status: the same records, for a script or a second terminal
 * ------------------------------------------------------------------ */

static const char *run_state_word(const run_t *r, const tally_t *t)
{
	if (r->live)
		return t->running ? "running" : "starting";
	if (t->running || t->queued || t->orphaned)
		return "interrupted";
	return "finished";
}

static void status_one(const run_t *r, int json)
{
	jrec_t *j;
	int n = store_jobs(r, &j), i;
	tally_t t;
	char b1[32], b2[32];
	int format = !strcmp(r->kind, "format");

	tally(j, n, &t);
	if (json) {
		/*
		 * Everything here is a string somebody else chose: a drive
		 * model, a serial, whatever sg_format last said.  All of it
		 * goes through json_escape, or one backslash in a model name
		 * silently breaks the output of a monitoring check.
		 */
#define JKV(key, val) do { printf("    \"" key "\": \""); \
		json_escape(stdout, val); printf("\",\n"); } while (0)
		printf("  {\n");
		JKV("id", r->id);
		JKV("kind", r->kind);
		JKV("what", r->what);
		JKV("detail", r->detail);
		JKV("profile", r->profile);
		JKV("outdir", r->outdir);
		JKV("dir", r->dir);
		printf("    \"created\": %lld,\n", r->created);
		printf("    \"ended\": %lld,\n", r->ended);
		printf("    \"supervisor\": %d,\n", (int)r->sup);
		printf("    \"live\": %s,\n", r->live ? "true" : "false");
		JKV("state", run_state_word(r, &t));
#undef JKV
		printf("    \"percent\": %.2f,\n", t.pct);
		printf("    \"drives\": [\n");
		for (i = 0; i < n; i++) {
			const jrec_t *x = &j[i];

#define JSV(key, val) do { printf("\"" key "\": \""); \
		json_escape(stdout, val); printf("\", "); } while (0)
			printf("      { ");
			JSV("device", x->dev);
			JSV("path", x->path);
			JSV("model", x->model);
			JSV("serial", x->serial);
			JSV("state", jstate_name(x));
			JSV("report", x->report);
			JSV("note", x->note);
#undef JSV
			printf("\"size\": %" PRIu64 ", \"percent\": %.2f, "
			       "\"rate\": %.0f, \"eta\": %.0f, "
			       "\"bad\": %" PRIu64 ", \"weak\": %" PRIu64 ", "
			       "\"slow\": %" PRIu64 ", \"bytes\": %" PRIu64
			       ", \"verdict\": %d }%s\n",
			       x->size, x->state == JS_DONE ? 100.0 : x->pct,
			       x->rate, x->eta, x->bad, x->weak, x->slow,
			       x->bytes, x->verdict, i + 1 < n ? "," : "");
		}
		printf("    ]\n  }");
		free(j);
		return;
	}

	out("run %s   %s   %s   %d drive%s   %s   elapsed %s\n", r->id,
	    r->kind, r->detail, n, n == 1 ? "" : "s", run_state_word(r, &t),
	    human_time(run_elapsed(r), b1, sizeof(b1)));
	if (r->live && r->sup > 0)
		out("  supervisor pid %d   'hddscan --attach %s' to watch, "
		    "'--stop %s' to stop it\n", (int)r->sup, r->id, r->id);
	out("  overall %.1f%%   %d running   %d queued   %d done",
	    t.pct, t.running, t.queued, t.done);
	if (t.orphaned)
		out("   %s%d orphaned%s", c_yel(), t.orphaned, c_off());
	out("\n  reports in %s/   records in %s\n\n", r->outdir, r->dir);
	out("  %-10s %-9s %-20s %6s %9s %9s %-10s %s\n", "DRIVE", "SIZE",
	    "MODEL", "PCT", "BAD", "SLOW", "STATE", format ? "MESSAGE" : "ETA");
	for (i = 0; i < n; i++) {
		const jrec_t *x = &j[i];

		out("  %-10.10s %-9s %-20.20s %5.1f%% %9" PRIu64 " %9" PRIu64
		    " %s%-10s%s %s\n", jrec_name(x),
		    human_size(x->size, b1, sizeof(b1)),
		    x->model[0] ? x->model : "?",
		    x->state == JS_DONE ? 100.0 : x->pct, x->bad, x->slow,
		    jrec_color(x), jrec_word(x, format), c_off(),
		    format ? x->note :
		    x->state == JS_RUNNING ?
		    human_time(x->eta, b2, sizeof(b2)) : x->note);
	}
	out("\n");
	free(j);
}

/*
 * What is this machine doing?  Answered from the records alone, so it works
 * with no run of our own, from a second terminal, from a script, and after
 * the program that started the work has long since exited.
 */
static int status_print(const char *want, int json)
{
	run_t *v;
	int n, i;

	if (want && *want) {
		run_t r;

		if (store_find(want, &r) < 0) {
			msg(PROG ": no run matches '%s' "
			    "(hddscan --status lists them)\n", want);
			return 1;
		}
		if (json)
			printf("[\n");
		status_one(&r, json);
		if (json)
			printf("\n]\n");
		return 0;
	}

	n = store_list(&v);
	if (json) {
		printf("[\n");
		for (i = 0; i < n; i++) {
			status_one(&v[i], 1);
			printf("%s\n", i + 1 < n ? "," : "");
		}
		printf("]\n");
		free(v);
		return 0;
	}
	if (!n) {
		out(PROG ": nothing has been run on this machine yet "
		    "(records would be in %s/runs)\n", store_root());
		free(v);
		return 0;
	}
	out("  %-17s %-7s %-22s %6s %8s  %-12s %s\n", "RUN", "KIND", "WHAT",
	    "DRIVES", "PROGRESS", "STATE", "ELAPSED");
	for (i = 0; i < n; i++) {
		jrec_t *j;
		int nj = store_jobs(&v[i], &j);
		tally_t t;
		char b1[32];
		const char *state;

		tally(j, nj, &t);
		state = run_state_word(&v[i], &t);
		out("  %-17s %-7s %-22.22s %6d %7.1f%%  %s%-12s%s %s",
		    v[i].id, v[i].kind, v[i].what, nj, t.pct,
		    !strcmp(state, "interrupted") ? c_yel() :
		    v[i].live ? c_grn() : "", state,
		    (v[i].live || !strcmp(state, "interrupted")) ? c_off() : "",
		    human_time(run_elapsed(&v[i]), b1, sizeof(b1)));
		if (!v[i].live && nj) {
			out("   %d ok, %d suspect, %d failing", t.healthy,
			    t.suspect, t.failing);
			if (t.stopped)
				out(", %d stopped", t.stopped);
			if (t.orphaned)
				out(", %d never finished", t.orphaned);
		}
		out("\n");
		free(j);
	}
	out("\n  'hddscan --status <run>' for the drives in one, "
	    "'--attach <run>' to watch it\n");
	free(v);
	return 0;
}

static char g_last_run[48];      /* the run this process most recently made */

/*
 * Start a run, then attend to it -- which is a different thing from running
 * it.  From the moment run_begin() returns, the run belongs to its own
 * supervisor: detached, we say where it went and leave; on a terminal we
 * watch it and can walk away again; on a pipe we follow it to the end, where
 * Ctrl-C stops the run itself, because that is what a script expects of the
 * command it is waiting on.
 */
static int run_dispatch(device_t *t, int n, const opts_t *o, int format)
{
	run_t r;
	int worst = 0;

	if (run_begin(&r, t, n, o, format) < 0)
		return 2;
	snprintf(g_last_run, sizeof(g_last_run), "%s", r.id);
	if (o->detach) {
		out("\n" PROG ": run %s started in the background: "
		    "%d drive%s, %s.\n", r.id, n, n == 1 ? "" : "s", r.what);
		out("  " PROG " --status %s     where it has got to\n"
		    "  " PROG " --attach %s     watch it on the dashboard\n"
		    "  " PROG " --stop %s       stop it\n"
		    "  reports land in %s/\n\n", r.id, r.id, r.id, o->outdir);
		return 0;
	}
	if (o->tui) {
		run_t *v;
		jrec_t *j;
		run_t rr;
		int nv = store_list(&v), nj, act;

		free(v);
		act = tui_watch(r.id, nv);
		if (act == 1)
			act = tui_runs() ? 2 : 0;
		g_tui_again = (act == 2);
		if (!run_reload(r.id, &rr, &j, &nj)) {
			worst = run_worst(j, nj);
			free(j);
		}
		return worst;
	}
	msg(PROG ": run %s - %d drive%s, reports in %s/. It carries on if this "
	    "terminal does not;\n         '" PROG " --status' finds it again, "
	    "Ctrl-C here stops it.\n", r.id, n, n == 1 ? "" : "s", o->outdir);
	return run_follow(&r, o);
}

int main(int argc, char **argv)
{
	opts_t o;
	device_t *targets = NULL;
	device_t *cli = NULL;      /* devices named on the command line */
	int ntargets = 0, ncli = 0, i, worst = 0;
	const char *logpath = NULL;
	struct sigaction sa;

	memset(&o, 0, sizeof(o));
	o.mode = MODE_READ;
	o.chunk = DEF_CHUNK;
	o.block = DEF_BLOCK;
	o.retries = 20;
	o.retry_cap_s = 60.0;
	o.lookahead = 0;   /* disable the drive's own prefetch by default */
	o.write_cache = -1;   /* leave the write cache exactly as we found it */
	o.read_cache = 0;     /* a retry answered from the drive's DRAM proves
			       * nothing about the platter; see the note on the
			       * retry classification in report() */
	o.awre = o.arre = -1;
	o.fmt_pi = -1;
	o.recovery_ms = -1;
	o.rd_retries = o.wr_retries = -1;
	o.segment = 1024ull * 1024 * 1024;
	o.bb_blocksize = 1024;
	o.auto_factor = 3.0;
	o.floor_ms = 25.0;
	o.min_rate = 10.0;
	o.max_temp = 58;
	o.map_width = 64;
	o.seed = 0x5eed1234abcdef01ull;
	o.outdir = ".";
	o.tui = -1;   /* undecided; resolved from the terminal after parsing */

	/*
	 * Profiles are resolved before anything else is parsed, so that an
	 * explicit flag overrides the profile's default no matter which order
	 * the two appear in on the command line.
	 */
	o.profile = "predeploy";
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--profile") && i + 1 < argc)
			o.profile = argv[++i];
	{
		const profile_t *p = profile_by_name(o.profile);

		if (!p) {
			int k;

			fprintf(stderr, PROG ": unknown profile '%s'. "
				"Available:\n", o.profile);
			for (k = 0; k < NPROFILES; k++)
				fprintf(stderr, "  %-10s %s\n",
					g_profiles[k].name, g_profiles[k].what);
			exit(2);
		}
		o.mode = p->mode;
		o.rewrite_weak = p->rewrite_weak;
		o.force_remap = p->force_remap;
		o.second_pass = p->second_pass;
		if (p->sample)
			o.sample = p->sample;
		if (p->chunk)
			o.chunk = p->chunk;
	}

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (die("%s needs an argument", a), ""))
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else if (!strcmp(a, "--version")) {
			printf(PROG " " VERSION "\n");
			return 0;
		} else if (!strcmp(a, "--check-deps")) {
			deps_report(1);
			return 0;
		} else if (!strcmp(a, "--list")) {
			o.list_only = 1;
		} else if (!strcmp(a, "--status") ||
			   !strcmp(a, "--status-json")) {
			o.status = 1;
			o.status_json = !strcmp(a, "--status-json");
			/*
			 * The argument is optional -- "what is running?" is
			 * the common question and needs nothing after it --
			 * so take the next word only if it is not itself an
			 * option.
			 */
			if (i + 1 < argc && argv[i + 1][0] != '-')
				o.runarg = argv[++i];
		} else if (!strcmp(a, "--attach")) {
			o.attach = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				o.runarg = argv[++i];
		} else if (!strcmp(a, "--detach")) {
			o.detach = 1;
		} else if (!strcmp(a, "--stop")) {
			o.stop = NEXT();
		} else if (!strcmp(a, "--forget")) {
			o.forget = NEXT();
		} else if (!strcmp(a, "--state-dir")) {
			snprintf(g_store_dir, sizeof(g_store_dir), "%s", NEXT());
		} else if (!strcmp(a, "--tui") || !strcmp(a, "-i")) {
			o.tui = 1;
		} else if (!strcmp(a, "--no-tui")) {
			o.tui = 0;
		} else if (!strcmp(a, "--all")) {
			o.scan_all = 1;
		} else if (!strcmp(a, "--include-ssd")) {
			o.include_ssd = 1;
		} else if (!strcmp(a, "--include-remote")) {
			o.include_remote = 1;
		} else if (!strcmp(a, "--sequential")) {
			o.sequential = 1;
		} else if (!strcmp(a, "--max-parallel")) {
			o.max_parallel = atoi(NEXT());
			if (o.max_parallel < 0)
				die("--max-parallel must be 0 (unlimited) or more");
		} else if (!strcmp(a, "--parallel")) {
			o.sequential = 0;   /* accepted for compatibility; now default */
		} else if (!strcmp(a, "--mode")) {
			const char *m = NEXT();

			if (!strcmp(m, "read"))
				o.mode = MODE_READ;
			else if (!strcmp(m, "verify"))
				o.mode = MODE_VERIFY;
			else if (!strcmp(m, "write"))
				o.mode = MODE_WRITE;
			else if (!strcmp(m, "check"))
				o.mode = MODE_CHECK;
			else
				die("unknown mode '%s' "
				    "(read, check, verify or write)", m);
		} else if (!strcmp(a, "--confirm")) {
			o.confirm = NEXT();
		} else if (!strcmp(a, "--rewrite-weak")) {
			o.rewrite_weak = 1;
		} else if (!strcmp(a, "--force-remap")) {
			o.force_remap = 1;
		} else if (!strcmp(a, "--profile")) {
			(void)NEXT();   /* already resolved in the pre-pass */
		} else if (!strcmp(a, "--repair")) {
			o.repair = 1;
		} else if (!strcmp(a, "--second-pass")) {
			o.second_pass = 1;
		} else if (!strcmp(a, "--start")) {
			if (parse_size(NEXT(), &o.start))
				die("bad --start value");
		} else if (!strcmp(a, "--end")) {
			if (parse_size(NEXT(), &o.end))
				die("bad --end value");
		} else if (!strcmp(a, "--chunk")) {
			if (parse_size(NEXT(), &o.chunk) || !o.chunk)
				die("bad --chunk value");
		} else if (!strcmp(a, "--block")) {
			if (parse_size(NEXT(), &o.block) || !o.block)
				die("bad --block value");
		} else if (!strcmp(a, "--order")) {
			const char *m = NEXT();

			if (!strcmp(m, "sequential") || !strcmp(m, "seq"))
				o.order = ORD_SEQ;
			else if (!strcmp(m, "reverse"))
				o.order = ORD_REVERSE;
			else if (!strcmp(m, "random"))
				o.order = ORD_RANDOM;
			else
				die("unknown --order '%s' "
				    "(sequential, reverse or random)", m);
		} else if (!strcmp(a, "--segment")) {
			if (parse_size(NEXT(), &o.segment))
				die("bad --segment value");
		} else if (!strcmp(a, "--sample")) {
			o.sample = strtoull(NEXT(), NULL, 10);
		} else if (!strcmp(a, "--max-time")) {
			o.max_time = strtoull(NEXT(), NULL, 10);
		} else if (!strcmp(a, "--max-errors")) {
			o.max_errors = strtoull(NEXT(), NULL, 10);
		} else if (!strcmp(a, "--chunk-slow-ms")) {
			o.chunk_ms = atof(NEXT());
		} else if (!strcmp(a, "--sector-slow-ms") ||
			   !strcmp(a, "--block-slow-ms")) {
			o.block_ms = atof(NEXT());
		} else if (!strcmp(a, "--auto-factor")) {
			o.auto_factor = atof(NEXT());
		} else if (!strcmp(a, "--floor-ms")) {
			o.floor_ms = atof(NEXT());
		} else if (!strcmp(a, "--min-rate")) {
			o.min_rate = atof(NEXT());
			if (o.min_rate < 0)
				die("--min-rate must be 0 or more");
		} else if (!strcmp(a, "--retry-max-seconds")) {
			o.retry_cap_s = atof(NEXT());
		} else if (!strcmp(a, "--retries")) {
			o.retries = atoi(NEXT());
			if (o.retries < 0 || o.retries > MAX_RETRIES)
				die("--retries must be between 0 and %d", MAX_RETRIES);
		} else if (!strcmp(a, "--force")) {
			o.force = 1;
		} else if (!strcmp(a, "--format")) {
			o.format = 1;
		} else if (!strcmp(a, "--format-blocksize")) {
			const char *m = NEXT();

			if (!strcmp(m, "keep"))
				o.fmt_bs = 0;
			else {
				o.fmt_bs = atoi(m);
				if (o.fmt_bs != 512 && o.fmt_bs != 4096)
					die("--format-blocksize takes 512, 4096 "
					    "or 'keep'");
			}
		} else if (!strcmp(a, "--format-fast")) {
			o.fmt_fast = 1;
		} else if (!strcmp(a, "--format-pi")) {
			const char *m = NEXT();

			if (!strcmp(m, "keep"))
				o.fmt_pi = -1;
			else if (!strcmp(m, "none"))
				o.fmt_pi = 0;
			else {
				o.fmt_pi = atoi(m);
				if (o.fmt_pi < 0 || o.fmt_pi > 3)
					die("--format-pi takes 'none', 'keep' "
					    "or a protection type 0..3");
			}
		} else if (!strcmp(a, "--apply-settings")) {
			o.apply_only = 1;
		} else if (!strcmp(a, "--fix-config")) {
			o.fix_config = 1;
		} else if (!strcmp(a, "--awre") || !strcmp(a, "--arre") ||
			   !strcmp(a, "--read-cache")) {
			const char *m = NEXT();
			int v;

			if (!strcmp(m, "on"))
				v = 1;
			else if (!strcmp(m, "off"))
				v = 0;
			else if (!strcmp(m, "keep"))
				v = -1;
			else
				die("%s takes 'on', 'off' or 'keep'", a);
			if (!strcmp(a, "--awre"))
				o.awre = v;
			else if (!strcmp(a, "--arre"))
				o.arre = v;
			else {
				o.read_cache = v;
				o.read_cache_set = 1;
			}
		} else if (!strcmp(a, "--drive-read-retries") ||
			   !strcmp(a, "--drive-write-retries")) {
			const char *m = NEXT();
			long long v;

			v = !strcmp(m, "keep") ? -1 : numarg(a, m, 0, 255);
			if (!strcmp(a, "--drive-read-retries"))
				o.rd_retries = v;
			else
				o.wr_retries = v;
		} else if (!strcmp(a, "--recovery-time")) {
			const char *m = NEXT();

			o.recovery_ms = !strcmp(m, "keep") ? -1 :
					numarg(a, m, 0, 65535);
		} else if (!strcmp(a, "--persist")) {
			o.persist = 1;
		} else if (!strcmp(a, "--write-cache")) {
			const char *m = NEXT();

			if (!strcmp(m, "on"))
				o.write_cache = 1;
			else if (!strcmp(m, "off"))
				o.write_cache = 0;
			else if (!strcmp(m, "keep"))
				o.write_cache = -1;
			else
				die("--write-cache takes 'on', 'off' or 'keep'");
		} else if (!strcmp(a, "--bms")) {
			const char *m = NEXT();

			if (!strcmp(m, "on"))
				o.bms = 1;
			else if (!strcmp(m, "keep"))
				o.bms = 0;
			else
				die("--bms takes 'on' or 'keep'");
		} else if (!strcmp(a, "--drive-lookahead")) {
			const char *m = NEXT();

			if (!strcmp(m, "off"))
				o.lookahead = 0;
			else if (!strcmp(m, "keep"))
				o.lookahead = -1;
			else
				die("--drive-lookahead takes 'off' or 'keep'");
			o.lookahead_set = 1;
		} else if (!strcmp(a, "--io-timeout")) {
			o.io_timeout = atoi(NEXT());
		} else if (!strcmp(a, "--max-temp")) {
			o.max_temp = atoll(NEXT());
		} else if (!strcmp(a, "--no-smart")) {
			o.no_smart = 1;
		} else if (!strcmp(a, "--no-direct")) {
			o.no_direct = 1;
		} else if (!strcmp(a, "--dry-run")) {
			o.dry_run = 1;
		} else if (!strcmp(a, "--json")) {
			o.json = NEXT();
		} else if (!strcmp(a, "--csv")) {
			o.csv = NEXT();
		} else if (!strcmp(a, "--badblocks-list")) {
			o.badblocks = NEXT();
		} else if (!strcmp(a, "--badblocks-blocksize")) {
			if (parse_size(NEXT(), &o.bb_blocksize) || !o.bb_blocksize)
				die("bad --badblocks-blocksize value");
		} else if (!strcmp(a, "--badblocks-offset")) {
			if (parse_size(NEXT(), &o.bb_offset))
				die("bad --badblocks-offset value");
		} else if (!strcmp(a, "--prometheus")) {
			o.prometheus = NEXT();
		} else if (!strcmp(a, "--journal")) {
			o.journal = NEXT();
		} else if (!strcmp(a, "--log")) {
			logpath = NEXT();
		} else if (!strcmp(a, "--state")) {
			o.state = NEXT();
		} else if (!strcmp(a, "--resume")) {
			o.resume = 1;
		} else if (!strcmp(a, "--outdir")) {
			o.outdir = NEXT();
		} else if (!strcmp(a, "--map-width")) {
			o.map_width = atoi(NEXT());
		} else if (!strcmp(a, "--quiet")) {
			g_quiet = 1;
		} else if (!strcmp(a, "--no-color")) {
			g_color = 0;
		} else if (a[0] == '-' && a[1]) {
			die("unknown option '%s' (try --help)", a);
		} else {
			device_t d;

			if (resolve_target(a, &d) < 0)
				return 2;
			targets = realloc(targets, (size_t)(ntargets + 1) * sizeof(*targets));
			if (!targets)
				die("out of memory");
			targets[ntargets++] = d;
		}
#undef NEXT
	}

	/*
	 * The form is the way in by default.  A bare run on a terminal has
	 * nothing to scan and no other way to say what to do, so it gets the
	 * picker rather than a usage error; naming a device, --all or --list
	 * is an explicit instruction and is carried out as given.  Resolve it
	 * here, before anything reads o.tui -- the undecided value is -1 and
	 * every truth test on it would say yes.
	 */
	if (o.tui < 0)
		o.tui = (!ntargets && !o.scan_all && !o.list_only &&
			 isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));


	if (g_color < 0)
		g_color = isatty(STDOUT_FILENO);
	/*
	 * Say what is missing before doing anything, so a degraded run is never
	 * mistaken for a complete one.  The form repeats it in one line of its
	 * own, since this scrolls away the moment the screen is cleared.
	 */
	if (!o.tui)
		deps_report(0);
	if (logpath) {
		g_log = fopen(logpath, "we");
		if (!g_log)
			die("cannot open log %s: %s", logpath, strerror(errno));
	}

	if (o.list_only) {
		list_devices(o.no_smart);
		return 0;
	}

	/*
	 * Everything that is about runs rather than about drives is answered
	 * from the store and gets out of the way before any device is
	 * touched.  None of it needs root, a terminal, or anything to still
	 * be running.
	 */
	if (o.status)
		return status_print(o.runarg, o.status_json);
	if (o.stop) {
		run_t *v;
		int nv, k, hit = 0;

		if (!strcmp(o.stop, "all")) {
			nv = store_list(&v);
			for (k = 0; k < nv; k++) {
				if (!v[k].live || store_stop(&v[k]) < 0)
					continue;
				out(PROG ": stopping run %s\n", v[k].id);
				hit++;
			}
			free(v);
			if (!hit)
				out(PROG ": nothing is running\n");
		} else {
			run_t r;

			if (store_find(o.stop, &r) < 0)
				die("no run matches '%s' (--status lists them)",
				    o.stop);
			if (!r.live)
				die("run %s is not running", r.id);
			if (store_stop(&r) < 0)
				die("could not signal run %s: %s", r.id,
				    strerror(errno));
			out(PROG ": stopping run %s. Each drive still writes "
			    "the report for the part it covered.\n", r.id);
		}
		return 0;
	}
	if (o.forget) {
		run_t *v;
		int nv, k, gone = 0;

		if (!strcmp(o.forget, "all") || !strcmp(o.forget, "finished")) {
			nv = store_list(&v);
			for (k = 0; k < nv; k++) {
				if (v[k].live)
					continue;
				if (!store_rmrun(&v[k]))
					gone++;
			}
			free(v);
			out(PROG ": forgot %d finished run%s (the reports "
			    "themselves are untouched)\n", gone,
			    gone == 1 ? "" : "s");
		} else {
			run_t r;

			if (store_find(o.forget, &r) < 0)
				die("no run matches '%s'", o.forget);
			if (r.live)
				die("run %s is still going. Stop it first, or "
				    "leave it be", r.id);
			if (store_rmrun(&r) < 0)
				die("could not remove %s: %s", r.dir,
				    strerror(errno));
			out(PROG ": forgot run %s\n", r.id);
		}
		return 0;
	}
	if (o.attach) {
		run_t *v;
		int nv = store_list(&v), act;
		char id[48] = "";

		if (o.runarg) {
			run_t r;

			if (store_find(o.runarg, &r) < 0) {
				free(v);
				die("no run matches '%s' (--status lists them)",
				    o.runarg);
			}
			snprintf(id, sizeof(id), "%s", r.id);
		} else if (!nv) {
			free(v);
			out(PROG ": nothing has been run on this machine yet\n");
			return 0;
		}
		if (tui_raw() < 0) {
			free(v);
			die("--attach needs a terminal. Use --status for the "
			    "same thing as text");
		}
		act = id[0] ? tui_watch(id, nv) : tui_runs();
		if (act == 1)
			act = tui_runs() ? 2 : 0;
		free(v);
		if (act != 2) {
			tui_cooked();
			return 0;
		}
		/* the viewer asked for a new test: carry on into the form */
		o.tui = 1;
		g_runs_seen = 1;
	}

	if (o.tui) {
		/*
		 * The form comes back after every run now, and the devices
		 * named on the command line belong in every picker it shows,
		 * so they are kept aside rather than consumed by the first
		 * pass.
		 */
		cli = targets;
		ncli = ntargets;
		targets = NULL;
		ntargets = 0;
	}

tui_again:
	if (o.tui) {
		device_t *all;
		int n, *pick, k, rc;

		if (tui_raw() < 0)
			die("the interactive UI needs a terminal");
		/*
		 * Work already in progress is the first thing to show.  A form
		 * would quietly imply the machine is idle, and the one thing
		 * someone coming back to thirty formats needs is to see them.
		 */
		if (!g_runs_seen) {
			g_runs_seen = 1;
			if (store_live_count() > 0 && !tui_runs()) {
				tui_cooked();
				return worst;
			}
		}
		n = enumerate_devices(&all);

		/* fold in anything named on the command line, image files too */
		for (k = 0; k < ncli; k++) {
			int seen = 0;

			for (i = 0; i < n; i++)
				if (!strcmp(all[i].name, cli[k].name))
					seen = 1;
			if (seen)
				continue;
			all = realloc(all, (size_t)(n + 1) * sizeof(*all));
			if (!all)
				die("out of memory");
			all[n++] = cli[k];
		}
		/*
		 * Every device the picker will show, including the ones named
		 * on the command line -- which used to skip this entirely, so
		 * an image file already being scanned by another run was
		 * offered as free.
		 */
		for (i = 0; i < n; i++)
			safety_check(&all[i]);
		if (n <= 0)
			die("no drives found to choose from");
		pick = calloc((size_t)n, sizeof(int));
		if (!pick)
			die("out of memory");
		for (i = 0; i < n; i++)
			for (k = 0; k < ncli; k++)
				if (!strcmp(all[i].name, cli[k].name) &&
				    !all[i].unsafe)
					pick[i] = 1;
		rc = tui_config(all, n, pick, &o);
		if (rc != 1) {
			free(pick);
			free(all);
			if (rc == 2) {
				/* the form asked for the list of runs */
				if (!tui_runs()) {
					tui_cooked();
					return worst;
				}
				free(targets);
				targets = NULL;
				ntargets = 0;
				goto tui_again;
			}
			tui_cooked();
			return worst;
		}
		free(targets);
		targets = NULL;
		ntargets = 0;
		for (i = 0; i < n; i++) {
			if (!pick[i])
				continue;
			targets = realloc(targets,
					  (size_t)(ntargets + 1) * sizeof(*targets));
			if (!targets)
				die("out of memory");
			targets[ntargets++] = all[i];
		}
		free(pick);
		free(all);
		if (!ntargets) {
			tui_cooked();
			die("no drives selected");
		}
		/*
		 * The screen belongs to the dashboard from here on -- a
		 * format has one too now, fed by what each drive reports
		 * about its own progress.  Two exceptions: applying settings
		 * has no run and nothing to watch, and a format has to print
		 * the exact sg_format command for every drive first
		 * (invariant 11), so both keep the terminal until they are
		 * done with it.
		 */
		tui_clear();
		if (o.apply_only)
			tui_cooked();
		else if (!o.format)
			g_quiet = 1;
	}

	if (!ntargets) {
		device_t *all;
		int n = enumerate_devices(&all);

		if (!o.scan_all)
			die("no device given. Use --list to see drives, "
			    "name one explicitly, --all to scan every free "
			    "disk, or run on a terminal for the picker");
		for (i = 0; i < n; i++) {
			if (!all[i].rotational && !o.include_ssd)
				continue;
			if (!transport_is_local(all[i].transport) && !o.include_remote) {
				msg(PROG ": skipping %s: %s disk, not a locally "
				    "attached drive (--include-remote to test it)\n",
				    all[i].name, transport_name(all[i].transport));
				continue;
			}
			safety_check(&all[i]);
			if (all[i].unsafe && !o.force) {
				msg(PROG ": skipping %s: %s\n", all[i].name,
				    all[i].unsafe_why);
				continue;
			}
			targets = realloc(targets,
					  (size_t)(ntargets + 1) * sizeof(*targets));
			if (!targets)
				die("out of memory");
			targets[ntargets++] = all[i];
		}
		free(all);
		if (!ntargets)
			die("no free rotational disks found (see --list)");
	}

	/*
	 * --repair is the whole sequence a drive needs to be given its best
	 * chance, in one flag: overwrite every sector (which is what refreshes
	 * a weak one), rewrite anything still slow afterwards, force a
	 * reallocation on anything that will not read at all, then re-read the
	 * entire device to prove the repair held.
	 *
	 * It also turns the drive's read cache off, because a repair you
	 * verified out of the drive's DRAM is not a repair you verified.
	 * Everything here stays overridable: a later --read-cache keep wins.
	 */
	if (o.repair) {
		o.mode = MODE_WRITE;
		o.rewrite_weak = 1;
		o.force_remap = 1;
		o.second_pass = 1;
		if (o.read_cache < 0)
			o.read_cache = 0;
	}

	/*
	 * Settings are applied by whoever owns the run: for a scan or a
	 * format that is the supervisor, inside run_begin(), so that they are
	 * put back when the *run* ends rather than when some viewer of it
	 * exits (invariant 7b).  --apply-settings has no run at all, so it
	 * does the work here and now.
	 */
	if (o.apply_only) {
		if (!settings_wanted(&o))
			die("--apply-settings without any setting to apply. "
			    "See --help for the drive settings section");
		apply_settings(targets, ntargets, &o);
		return worst;
	}

	if (o.format) {
		char cmd[512];
		int pfu = 0;

		if (!have_sg("sg_format"))
			die("--format needs sg_format from sg3_utils "
			    "(dnf install sg3_utils / apt install sg3-utils)");
		for (i = 0; i < ntargets; i++) {
			device_t *d = &targets[i];
			int want = o.fmt_pi >= 0 ? o.fmt_pi : d->prot_type;
			int ok;

			safety_check(d);
			ok = o.tui || (o.confirm &&
			     (!strcmp(o.confirm, d->name) ||
			      (d->serial[0] && !strcmp(o.confirm, d->serial))));
			if (d->is_file)
				die("%s is a file; --format is a SCSI FORMAT "
				    "UNIT and needs a real drive", d->name);
			if (d->transport != TR_SCSI)
				die("%s is a %s drive; FORMAT UNIT is a SCSI "
				    "command", d->name,
				    transport_name(d->transport));
			if (d->unsafe && !o.force)
				die("%s: %s\n       refusing to format a drive "
				    "that is in use", d->name, d->unsafe_why);
			if (fmt_pi_bits(want, &pfu) < 0)
				die("%s: cannot work out what protection to "
				    "format with (the drive reports type %d); "
				    "pass --format-pi explicitly", d->name,
				    d->prot_type);
			if (!ok)
				die("%s: --format erases the whole drive and "
				    "needs --confirm %s (or --confirm "
				    "<serial>)", d->path, d->name);
			format_cmd(d, &o, cmd, sizeof(cmd));
		}
		out("\n  Low level format of %d drive%s. This erases everything "
		    "and takes hours.\n"
		    "  Losing power partway through can leave a drive needing "
		    "vendor tooling.\n\n", ntargets, ntargets == 1 ? "" : "s");
		if (o.dry_run) {
			for (i = 0; i < ntargets; i++) {
				format_cmd(&targets[i], &o, cmd, sizeof(cmd));
				out("  dry run, would execute:\n    %s\n", cmd);
			}
			return 0;
		}
		/*
		 * The commands are on screen and the drives are about to be
		 * erased.  On the form this is the last stop before the
		 * dashboard covers them, so it holds here for a key: the
		 * whole point of printing them is that someone reads them.
		 */
		if (o.tui) {
			int k;

			out("\n  %sENTER starts this. Anything else goes back "
			    "to the form.%s\n", c_red(), c_off());
			fflush(stdout);
			do {
				k = tui_key(200);
			} while (k < 0 && !g_stop);
			if (k != '\r' && k != '\n') {
				g_tui_again = 1;
				goto run_done;
			}
			g_quiet = 1;
			tui_clear();
		}
		{
			int st = run_dispatch(targets, ntargets, &o, 1);

			if (st > worst)
				worst = st;
		}
		out("\n");
		goto run_done;
	}

	/* per-target safety and mode gates */
	for (i = 0; i < ntargets; i++) {
		device_t *d = &targets[i];

		safety_check(d);
		if (d->unsafe) {
			if (!o.force)
				die("%s is not safe to test: %s\n"
				    "       unmount it first, or pass --force if you are sure",
				    d->path, d->unsafe_why);
			msg(PROG ": %s: %sWARNING%s testing anyway: %s\n",
			    d->path, c_red(), c_off(), d->unsafe_why);
			if (d->is_root_disk)
				die("%s holds the root filesystem - refusing even with --force",
				    d->path);
		}
		if (!d->rotational && !o.include_ssd && !d->is_file)
			die("%s is not a rotating disk. This tool's latency model is "
			    "meant for HDDs; use --include-ssd to override", d->path);
		if (!d->is_file && !transport_is_local(d->transport) && !o.include_remote)
			die("%s is attached over %s. Its read latency is a property of "
			    "that link and of the storage behind it, not of any platter,\n"
			    "       so a clean result here would say nothing about drive "
			    "health. Use --include-remote if you really want to test it.",
			    d->path, transport_name(d->transport));
		if (mode_writes(o.mode) || o.force_remap || o.rewrite_weak) {
			int ok = o.tui || (o.confirm &&
				 (!strcmp(o.confirm, d->name) ||
				  (d->serial[0] && !strcmp(o.confirm, d->serial))));

			if (!ok)
				die("%s: writing to a drive needs --confirm %s "
				    "(or --confirm <serial>).\n"
				    "       mode=%s%s will modify data on this device.",
				    d->path, d->name,
				    mode_name(o.mode),
				    o.force_remap ? " --force-remap" :
				    o.rewrite_weak ? " --rewrite-weak" : "");
		}
		/*
		 * Shingled drives overlap their tracks, so a zone can only be
		 * written from its start.  A host-managed drive rejects any
		 * other write outright: a write pass would fail on essentially
		 * every request and we would report a healthy drive as dead.
		 */
		if (d->zoned && (mode_writes(o.mode) || o.force_remap || o.rewrite_weak)) {
			if (d->zoned == 2)
				die("%s is a host-managed SMR drive. Its zones can only "
				    "be written sequentially from the start, so a write\n"
				    "       test would fail everywhere and look like "
				    "total media failure. Use --mode read.", d->path);
			msg(PROG ": %s: %shost-aware SMR%s: writes are accepted but the "
			    "firmware rewrites whole bands behind your back, so write "
			    "latency will not reflect the media\n",
			    d->name, c_yel(), c_off());
		}
		if (d->zoned && o.order == ORD_RANDOM && mode_writes(o.mode))
			die("%s is an SMR drive; --order random cannot be combined with "
			    "a write mode, because zones must be written in order",
			    d->path);
		if (o.rewrite_weak && o.mode == MODE_READ)
			msg(PROG ": %s: --rewrite-weak will write to weak sectors "
			    "even in read mode\n", d->name);
	}

	if (geteuid() != 0 && !targets[0].is_file)
		msg(PROG ": warning: not running as root; raw disk access and "
		    "SMART will probably fail\n");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if ((ntargets > 1 || o.tui || o.detach) && !o.sequential && !o.dry_run) {
		int st = run_dispatch(targets, ntargets, &o, 0);

		/* the exit code covers every run this process made */
		if (st > worst)
			worst = st;
	} else {
		/*
		 * One drive at a time, in this process: a single device named
		 * on the command line, or --sequential.  The scan is still
		 * recorded as a run, so '--status' from another terminal can
		 * see it and so it is not the one kind of work this machine
		 * does invisibly -- but nothing is detached, and Ctrl-C ends
		 * it here as it always has.
		 */
		run_t r;
		job_t *jobs = NULL;
		int have_run = 0;

		if (!o.dry_run) {
			jobs = calloc((size_t)ntargets, sizeof(job_t));
			if (!jobs)
				die("out of memory");
			have_run = !run_create(&r, targets, ntargets, &o, 0,
					       jobs);
			if (have_run) {
				r.sup = getpid();
				r.supstart = pid_start(getpid());
				run_put(&r);
				snprintf(g_last_run, sizeof(g_last_run), "%s",
					 r.id);
			}
		}
		apply_settings(targets, ntargets, &o);
		for (i = 0; i < ntargets && !g_stop; i++) {
			char jb[PATH_MAX], cb[PATH_MAX], bb[PATH_MAX], pb[PATH_MAX];
			int st;

			if (have_run) {
				if (jrec_get(jobs[i].file, &g_job) < 0)
					memset(&g_job, 0, sizeof(g_job));
				snprintf(g_job.file, sizeof(g_job.file), "%s",
					 jobs[i].file);
				g_job.pid = getpid();
				g_job.pidstart = pid_start(getpid());
				g_job.started = (long long)time(NULL);
				g_job.verdict = -1;
				/*
				 * This path prints its report to the terminal
				 * rather than to a file, so do not point the
				 * record at one that will never exist.
				 */
				g_job.report[0] = 0;
				g_job_own = 1;
				job_flush(JS_RUNNING);
			}
			st = scan_device(&targets[i], &o,
					     derive_path(o.json, targets[i].name,
							 ntargets > 1, jb, sizeof(jb)),
					     derive_path(o.csv, targets[i].name,
							 ntargets > 1, cb, sizeof(cb)),
					     derive_path(o.badblocks, targets[i].name,
							 ntargets > 1, bb, sizeof(bb)),
					     derive_path(o.prometheus, targets[i].name,
							 ntargets > 1, pb, sizeof(pb)));
			if (have_run) {
				g_job.verdict = st;
				g_job.ended = (long long)time(NULL);
				if (!st)
					g_job.pct = 100.0;
				g_job.rate = 0;
				job_flush(st == 130 ? JS_STOPPED : JS_DONE);
				g_job_own = 0;
			}
			if (st > worst)
				worst = st;
		}
		if (have_run) {
			for (; i < ntargets; i++)
				job_cancel(&jobs[i], "run stopped before this "
					   "drive was reached");
			r.ended = (long long)time(NULL);
			run_put(&r);
		}
		free(jobs);
	}
run_done:

	if (o.tui && g_tui_again) {
		/*
		 * Another test was asked for, from the summary or from a
		 * dashboard that was left running.  Shed this pass's state so
		 * the next one through the form starts the way a fresh
		 * invocation would.
		 *
		 * The restore call is a no-op in every path that gets here,
		 * because run-scoped drive settings now belong to the run's
		 * supervisor and are put back when the *run* ends (invariant
		 * 12).  It stays because this process must never be the one
		 * holding an un-restored setting, and proving that at a
		 * glance is worth one cheap call.
		 */
		field_restore_all();
		g_tui_again = 0;
		g_stop = 0;
		g_dash = 0;
		g_msg_head = 0;
		g_msg_count = 0;
		g_quiet = 0;
		free(targets);
		targets = NULL;
		ntargets = 0;
		goto tui_again;
	}

	if (o.tui) {
		tui_cooked();
		g_quiet = 0;
		/*
		 * Leave the last run's reports in the scrollback: the
		 * dashboard and the summary lived on the alternate screen and
		 * went when it did.  Take the paths from the run's own
		 * records rather than deriving them again -- deriving them a
		 * second way is how an image file given by path used to end up
		 * silently not shown.
		 */
		if (g_last_run[0]) {
			run_t r;
			jrec_t *j;
			int nj, k;

			if (!run_reload(g_last_run, &r, &j, &nj)) {
				for (k = 0; k < nj; k++) {
					FILE *rf;
					char line[4096];

					if (!j[k].report[0])
						continue;
					rf = fopen(j[k].report, "re");
					if (!rf)
						continue;
					while (fgets(line, sizeof(line), rf))
						fputs(line, stdout);
					fclose(rf);
				}
				free(j);
			}
		}
	}
	free(targets);
	free(cli);
	if (g_log)
		fclose(g_log);
	return worst;
}
