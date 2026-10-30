# kindle-mtp-mount - build with: make
# Needs macFUSE (libfuse3 in /usr/local) and libmtp 1.1.23 (brew install libmtp).
BREW   ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
FUSE   ?= /usr/local
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += -I$(FUSE)/include/fuse3 -I$(BREW)/include
LDLIBS += -L$(FUSE)/lib -lfuse3 -L$(BREW)/lib -lmtp -lpthread
PREFIX ?= $(HOME)/kindle-stage/tools

all: kindle-mtpfs

kindle-mtpfs: src/kindle-mtpfs.c src/redact.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/kindle-mtpfs.c $(LDFLAGS) $(LDLIBS)

install: kindle-mtpfs
	install -m 755 kindle-mtpfs scripts/kindle-mount scripts/kindle-unmount $(PREFIX)/

# unit tests: run the FUSE ops against an in-memory fake device (no Kindle, no FUSE needed)
test:
	$(CC) -g -Wall -Wno-unused-function -Wno-unused-variable -Itest/stub -o test/test_ops test/test_ops.c test/mockmtp.c -lpthread
	./test/test_ops

clean:
	rm -f kindle-mtpfs test/test_ops

.PHONY: all install test clean
