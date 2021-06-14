SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

CFLAGS=-Wall -Wextra -Wpedantic -Wno-unused-parameter
LIBS=-lwayland-client
OBJ=stacktile.o river-layout-v2.o
GEN=river-layout-v2.h river-layout-v2.c

stacktile: $(OBJ)
	$(CC)$ $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

install:
	install -D stacktile   $(DESTDIR)$(BINDIR)/stacktile
	install -D stacktile.1 $(DESTDIR)$(MANDIR)/man1/stacktile.1

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/stacktile
	$(RM) $(DESTDIR)$(MANDIR)/man1/stacktile.1

clean:
	$(RM) stacktile $(GEN) $(OBJ)

.PHONY: clean install

