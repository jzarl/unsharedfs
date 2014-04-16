PREFIX = /usr/local
INSTALL = install
INSTALL_PROG = $(INSTALL) -p -s -m 0755
INSTALL_FILE = $(INSTALL) -p -m 0644

# compiler flags:
CFLAGS = -g -O2 -Wall `pkg-config fuse --cflags`
LDFLAGS = `pkg-config fuse --libs`
# enable syslog:
CFLAGS += -DHAVE_SYSLOG

.PHONY: all
all: src/unsharedfs

src/unsharedfs: src/unsharedfs.o src/fs.o

.PHONY: install
install:
	$(INSTALL_PROG) -D src/unsharedfs $(PREFIX)/sbin/unsharedfs
	mkdir -p $(PREFIX)/share/doc/unsharedfs
	$(INSTALL_FILE) -t $(PREFIX)/share/doc/unsharedfs README.md COPYING
	$(INSTALL_FILE) -D doc/unsharedfs.8.gz  $(PREFIX)/share/man/man8/unsharedfs.8.gz

.PHONY: uninstall
uninstall:
	rm -f $(PREFIX)/sbin/unsharedfs
	rm -rf $(PREFIX)/share/doc/unsharedfs

.PHONY: clean
clean:
	rm -f src/unsharedfs src/*.o

###
# Rules to update the man-page:
.PHONY: update-man
update-man: doc/unsharedfs.8.gz

%.gz: %
	gzip -f $^

doc/unsharedfs.8: src/unsharedfs doc/unsharedfs.h2m
	help2man --output=$@ --section 8 --no-info $(addprefix --include=,$(filter %.h2m,$^)) $<
