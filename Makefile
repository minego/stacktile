SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

CFLAGS=-Wall -Wextra -Wpedantic -Wno-unused-parameter
LIBS=-lwayland-client
OBJ=layout.o river-layout-v2.o
GEN=river-layout-v2.h river-layout-v2.c

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

