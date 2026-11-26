CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lX11 -lm
PREFIX ?= $(HOME)/.local

all: xlgr

xlgr: xlgr.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f xlgr

# Install the binary, a desktop entry and an icon. GNOME shows the icon from
# the desktop entry, matched by WM_CLASS, and ignores the window's own icon.
install: xlgr
	install -Dm755 xlgr $(DESTDIR)$(PREFIX)/bin/xlgr
	install -Dm644 xlgr.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/xlgr.svg
	sed 's|^Exec=.*|Exec=$(PREFIX)/bin/xlgr|' xlgr.desktop > xlgr.desktop.tmp
	install -Dm644 xlgr.desktop.tmp $(DESTDIR)$(PREFIX)/share/applications/xlgr.desktop
	rm -f xlgr.desktop.tmp
	-gtk-update-icon-cache -q -t $(DESTDIR)$(PREFIX)/share/icons/hicolor
	-update-desktop-database -q $(DESTDIR)$(PREFIX)/share/applications

.PHONY: all clean install
