# Process substitution in mancheck needs a real bash, not whatever /bin/sh is.
SHELL  := /bin/bash

CC      ?= gcc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -Wno-unused-parameter
LDFLAGS ?=
PREFIX  ?= /usr/local

BIN = hddscan
SRC = hddscan.c
MAN = hddscan.8
MANDIR ?= $(PREFIX)/share/man/man8

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# A static build has no glibc version floor, so it runs on older systems and on
# rescue images with no packages installed.  The dynamic build picks up glibc's
# C23 strtol/sscanf redirects (a side effect of _GNU_SOURCE) and therefore
# requires glibc >= 2.38 on the machine that runs it.
static: $(SRC)
	$(CC) $(CFLAGS) -static -o $(BIN)-static $< $(LDFLAGS)

# AddressSanitizer and UBSan.  This is the binary that parses /proc, kernel log
# lines, smartctl output, checkpoints and the map's metadata, and one of its
# modes writes to drives, so a memory error here is worse than a crash.  Not
# part of `make` or `make test`: it is about twice as slow, and tests/tui.py
# keeps time.  Threads there are none, so there is no TSan target; MSan wants
# libc rebuilt with it.
#
# -fsanitize-recover and halt_on_error=0 keep a run going past its first
# report so one pass lists everything, and the reports go to files rather
# than stderr, because the suites read stderr and assert on it.  Leak
# detection is off: the workers and the supervisor end through exit() and
# hold what they hold until then on purpose.
SAN        := -fsanitize=address,undefined -fsanitize-recover=address,undefined
ASAN_BIN   := $(BIN)-asan
ASAN_LOGS  ?= $(CURDIR)/asan-logs

asan: $(SRC)
	$(CC) $(CFLAGS) -O1 -fno-omit-frame-pointer $(SAN) -o $(ASAN_BIN) $< $(LDFLAGS)

test-asan: asan
	@rm -rf $(ASAN_LOGS) && mkdir -p $(ASAN_LOGS)
	@export ASAN_OPTIONS="detect_leaks=0:halt_on_error=0:abort_on_error=0:log_path=$(ASAN_LOGS)/asan"; \
	export UBSAN_OPTIONS="print_stacktrace=1:log_path=$(ASAN_LOGS)/ubsan"; \
	HDDSCAN=./$(ASAN_BIN) tests/cli.sh; a=$$?; \
	BIN="./$(ASAN_BIN) dm" bash tests/dm.sh; b=$$?; \
	n=$$(ls $(ASAN_LOGS) | wc -l); \
	if [ $$n -gt 0 ]; then \
		echo "sanitizer: $$n report file(s) in $(ASAN_LOGS)"; \
		grep -h -E 'ERROR: AddressSanitizer|runtime error' $(ASAN_LOGS)/* \
			| sed 's/^[^ ]* *//' | sort | uniq -c | sort -rn | head -40; \
		exit 1; \
	fi; \
	[ $$a -eq 0 ] && [ $$b -eq 0 ]

# A release tarball: the static binary, stripped, with the man page, the boot
# unit for the device-mapper maps, and the licence.  The same target builds the
# GitHub release, so what is published can be reproduced by hand.
VERSION := $(shell sed -n 's/^\#define VERSION "\(.*\)"/\1/p' $(SRC))
ARCH    ?= $(shell uname -m)
DIST    := $(BIN)-$(VERSION)-linux-$(ARCH)

dist: static
	rm -rf $(DIST) $(DIST).tar.gz $(DIST).tar.gz.sha256
	mkdir -p $(DIST)/contrib
	cp $(BIN)-static $(DIST)/$(BIN)
	strip $(DIST)/$(BIN)
	ln -s $(BIN) $(DIST)/dm-badblocks
	cp $(MAN) README.md LICENSE $(DIST)/
	cp contrib/dm-badblocks.service $(DIST)/contrib/
	tar czf $(DIST).tar.gz --owner=0 --group=0 $(DIST)
	sha256sum $(DIST).tar.gz > $(DIST).tar.gz.sha256
	rm -rf $(DIST)
	@echo "$(DIST).tar.gz"

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/sbin/$(BIN)
	ln -sf $(BIN) $(DESTDIR)$(PREFIX)/sbin/dm-badblocks
	install -d $(DESTDIR)$(MANDIR)
	install -m 0644 $(MAN) $(DESTDIR)$(MANDIR)/$(MAN)

# Every option in --help must appear in the man page and the other way round.
# Prints nothing when they agree.
mancheck: $(BIN)
	@diff <(./$(BIN) --help | grep -oE -e '^ +(-[a-z], )?--[a-z-]+' | \
		grep -oE -e '--[a-z-]+' | sort -u) \
	      <(sed 's/\\-/-/g' $(MAN) | grep -oE -e '--[a-z][a-z-]+' | \
		sort -u | grep -vE -e '^--(clear|save|version)$$') && \
		echo "$(MAN) and --help agree"

# The suite runs entirely against image files: no root, no real device, and
# nothing it does can reach /dev/sd*.
test: $(BIN)
	@tests/cli.sh
	@bash tests/dm.sh
	@python3 tests/tui.py

# The site in docs/: screenshots drawn from hddscan's own output, and an option
# reference generated from --help.  sitecheck prints nothing when the reference
# is current, the way mancheck does for the man page.
site: $(BIN)
	@python3 tools/shots.py
	@python3 tools/options.py

sitecheck: $(BIN)
	@python3 tools/options.py --check

clean:
	rm -rf $(BIN) $(BIN)-static $(BIN)-asan asan-logs $(BIN)-*-linux-*.tar.gz*

.PHONY: all static dist install clean mancheck sitecheck site test asan test-asan
