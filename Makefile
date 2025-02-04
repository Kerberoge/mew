# mew - dynamic menu
# See LICENSE file for copyright and license details.
.POSIX:

# pkg-config
PKG_CONFIG = pkg-config

# paths
PREFIX = /usr/local

# includes and libs
PKGS = fcft pixman-1 wayland-client xkbcommon
INCS = `$(PKG_CONFIG) --cflags $(PKGS)`
LIBS = `$(PKG_CONFIG) --libs $(PKGS)`

# flags
EMCPPFLAGS = -D_POSIX_C_SOURCE=200809L
EMCFLAGS   = -pedantic -Wall $(INCS) $(EMCPPFLAGS) $(CFLAGS)
LDLIBS     = $(LIBS)

all: mew

.c.o:
	$(CC) -c $(EMCFLAGS) $<

mew.o: config.h wlr-layer-shell-unstable-v1-protocol.h xdg-shell-protocol.h

mew: wlr-layer-shell-unstable-v1-protocol.o xdg-shell-protocol.o mew.o
	$(CC) $(LDFLAGS) -o $@ wlr-layer-shell-unstable-v1-protocol.o xdg-shell-protocol.o mew.o $(LDLIBS)

WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) client-header $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) client-header wlr-layer-shell-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code wlr-layer-shell-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.o: xdg-shell-protocol.o

clean:
	rm -f mew *.o *-protocol.*

install: all
	install -s -D -t $(DESTDIR)$(PREFIX)/bin mew

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mew

.PHONY: all clean install uninstall
