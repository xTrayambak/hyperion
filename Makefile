INCLUDE = include

CC     = cc
CFLAGS = -pedantic -O0 -g -Wall -Wextra -I${INCLUDE} -DWLR_USE_UNSTABLE

WAYLAND_PROTOCOLS = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER   = $(shell pkg-config --variable=wayland_scanner wayland-scanner)
LIBS = \
	 $(shell pkg-config --cflags --libs wlroots) \
	 $(shell pkg-config --cflags --libs wayland-server) \
	 $(shell pkg-config --cflags --libs libdrm) \
	 $(shell pkg-config --cflags --libs pangocairo) \
	 $(shell pkg-config --cflags --libs pixman-1) \
	 $(shell pkg-config --cflags --libs xkbcommon)

all: hyperion

hyperion: src/main.c ${INCLUDE}/xdg-shell-protocol.h ${INCLUDE}/xdg-shell-protocol.c
	${CC} ${CFLAGS} -o $@ $< ${LIBS}

# wayland-scanner generates C headers and rigging for Wayland protocols,
# which are specified in XML. wlroots requires you to rig these up to
# your build system yourself and provide them in the include path.

${INCLUDE}/xdg-shell-protocol.h:
	${WAYLAND_SCANNER} server-header \
		${WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml $@

${INCLUDE}/xdg-shell-protocol.c: ${INCLUDE}/xdg-shell-protocol.h
	${WAYLAND_SCANNER} private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

clean:
	rm -f hyperion ${INCLUDE}/xdg-shell-protocol.h ${INCLUDE}/xdg-shell-protocol.c

.PHONY: all clean
