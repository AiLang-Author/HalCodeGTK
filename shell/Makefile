# Thin GTK3 blit shell. Headers: pkg-config, else Ailang-Self-Hosting third_party.
# Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.

AILANG_ROOT ?= /home/bob/Ailang-Self-Hosting-
GTKH := $(AILANG_ROOT)/third_party/gtk3-headers
GLIB_INC := /usr/include/glib-2.0
GLIB_GEN := /usr/lib/x86_64-linux-gnu/glib-2.0/include
LIBDIR := /usr/lib/x86_64-linux-gnu

PKG := $(shell pkg-config --exists gtk+-3.0 cairo && echo yes)

ifeq ($(PKG),yes)
  GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 cairo)
  GTK_LIBS := $(shell pkg-config --libs gtk+-3.0 cairo)
else
  GTK_CFLAGS := \
	-I$(GTKH)/usr/include/gtk-3.0 \
	-I$(GTKH)/usr/include/cairo \
	-I$(GTKH)/usr/include/pango-1.0 \
	-I$(GTKH)/usr/include/gdk-pixbuf-2.0 \
	-I$(GTKH)/usr/include/atk-1.0 \
	-I$(GTKH)/usr/include/harfbuzz \
	-I$(GTKH)/usr/include/fribidi \
	-I$(GTKH)/usr/include/freetype2 \
	-I$(GTKH)/usr/include \
	-I$(GLIB_INC) \
	-I$(GLIB_GEN)
  GTK_LIBS := \
	$(LIBDIR)/libgtk-3.so.0 \
	$(LIBDIR)/libgdk-3.so.0 \
	$(LIBDIR)/libcairo.so.2 \
	$(LIBDIR)/libpango-1.0.so.0 \
	$(LIBDIR)/libpangocairo-1.0.so.0 \
	$(LIBDIR)/libgdk_pixbuf-2.0.so.0 \
	-lgobject-2.0 -lglib-2.0 -lm
endif

AILANG ?= $(shell command -v ailang.x)

.PHONY: all clean run desk

all: halcode_shell_gtk write_hello_frame desk

desk: halcode_desk.x

halcode_desk.x: halcode_desk.ailang
	@test -n "$(AILANG)" || (echo "ailang.x not on PATH"; exit 1)
	cd "$(AILANG_ROOT)" && $(AILANG) "$(CURDIR)/halcode_desk.ailang" "$(CURDIR)/halcode_desk.x"

halcode_shell_gtk: halcode_shell_gtk.c
	cc -O2 -o $@ $< $(GTK_CFLAGS) $(GTK_LIBS)

write_hello_frame: write_hello_frame.c
	cc -O2 -o $@ $<

clean:
	rm -f halcode_shell_gtk write_hello_frame halcode_desk.x

run: all
	mkdir -p /tmp/halcode_app
	./halcode_desk.x & echo $$! > /tmp/halcode_desk.pid
	sleep 0.4
	HALCODE_APP_STATE=/tmp/halcode_app ./halcode_shell_gtk /tmp/halcode_app
	if [ -f /tmp/halcode_desk.pid ]; then kill $$(cat /tmp/halcode_desk.pid) 2>/dev/null || true; fi
