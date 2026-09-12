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

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/sbin/$(BIN)
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
	@python3 tests/tui.py

clean:
	rm -f $(BIN) $(BIN)-static

.PHONY: all static install clean mancheck test
