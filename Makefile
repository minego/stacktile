SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

CFLAGS=-Wall -Wextra -Wpedantic -Wno-unused-parameter
LIBS=-lwayland-client
OBJ=layout.o river-layout-unstable-v1.o river-options-unstable-v1.o
GEN=river-layout-unstable-v1.h river-layout-unstable-v1.c river-options-unstable-v1.h river-options-unstable-v1.c

layout: $(OBJ)
	$(CC)$ $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

install:
	install -D -t $(DESTDIR)$(BINDIR) layout

clean:
	rm -f layout $(GEN) $(OBJ)

.PHONY: clean install

