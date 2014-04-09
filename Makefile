PREFIX = /usr/local
INSTALL = install
INSTALL_PROG = $(INSTALL) -p -s -m 0755
INSTALL_FILE = $(INSTALL) -p -m 0644

# compiler flags:
CFLAGS = -g -O2 -Wall `pkg-config fuse --cflags`
LDFLAGS = `pkg-config fuse --libs`
# enable syslog:
CFLAGS += -DHAVE_SYSLOG

all: src/unshared

install:
	$(INSTALL_PROG) -D src/unshared $(PREFIX)/sbin/unshared
	mkdir -p $(PREFIX)/share/doc/unsharedfs
	$(INSTALL_FILE) -t $(PREFIX)/share/doc/unsharedfs README.md COPYING

uninstall:
	rm -f $(PREFIX)/sbin/unshared
	rm -rf $(PREFIX)/share/doc/unsharedfs

clean:
	rm -f src/unshared

