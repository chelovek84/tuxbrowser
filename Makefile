# Tux Browser - fast, lightweight WebKitGTK web browser
PREFIX  ?= /usr/local
CC      ?= cc
PKGS     = webkit2gtk-4.1 gtk+-3.0 sqlite3 libsoup-3.0
CFLAGS  ?= -O2 -g
LDFLAGS += -rdynamic
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -Wno-deprecated-declarations $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

all: tuxbrowser

tuxbrowser: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c src/tuxbrowser.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: tuxbrowser
	install -Dm755 tuxbrowser $(DESTDIR)$(PREFIX)/bin/tuxbrowser
	install -Dm644 data/tuxbrowser.desktop $(DESTDIR)$(PREFIX)/share/applications/tuxbrowser.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tuxbrowser $(DESTDIR)$(PREFIX)/share/applications/tuxbrowser.desktop

clean:
	rm -rf build tuxbrowser

.PHONY: all install uninstall clean
