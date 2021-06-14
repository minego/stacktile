SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

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
	install -D stacktile $(DESTDIR)$(BINDIR)/stacktile

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/stacktile

clean:
	$(RM) stacktile $(GEN) $(OBJ)

.PHONY: clean install

