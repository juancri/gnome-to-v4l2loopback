CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2
LDFLAGS =

# PipeWire dependencies
PIPEWIRE_CFLAGS = $(shell pkg-config --cflags libpipewire-0.3)
PIPEWIRE_LIBS = $(shell pkg-config --libs libpipewire-0.3)

# Additional dependencies
SPA_CFLAGS = $(shell pkg-config --cflags libspa-0.2)
SPA_LIBS = $(shell pkg-config --libs libspa-0.2)

# GIO/GLib dependencies for D-Bus portal communication
GIO_CFLAGS = $(shell pkg-config --cflags gio-2.0 gio-unix-2.0)
GIO_LIBS = $(shell pkg-config --libs gio-2.0 gio-unix-2.0)

# libyuv dependencies for color conversion
LIBYUV_CFLAGS =
LIBYUV_LIBS = -lyuv

# EGL/OpenGL ES dependencies for DMA buffer handling
EGL_CFLAGS = $(shell pkg-config --cflags egl glesv2 2>/dev/null)
EGL_LIBS = $(shell pkg-config --libs egl glesv2 2>/dev/null)

# V4L2 is part of kernel headers (no pkg-config needed)
V4L2_CFLAGS =
V4L2_LIBS =

# Combine all flags
ALL_CFLAGS = $(CFLAGS) $(PIPEWIRE_CFLAGS) $(SPA_CFLAGS) $(GIO_CFLAGS) $(LIBYUV_CFLAGS) $(EGL_CFLAGS) $(V4L2_CFLAGS)
ALL_LIBS = $(PIPEWIRE_LIBS) $(SPA_LIBS) $(GIO_LIBS) $(LIBYUV_LIBS) $(EGL_LIBS) $(V4L2_LIBS)

SRCDIR = src
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/portal.c $(SRCDIR)/gl_handler.c
TARGET = gnome-to-v4l2loopback

.PHONY: all clean install deps-check

all: deps-check $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LIBS)

clean:
	rm -f $(TARGET)

deps-check:
	@echo "Checking dependencies..."
	@pkg-config --exists libpipewire-0.3 || (echo "Error: libpipewire-0.3 not found. Install pipewire development packages." && exit 1)
	@pkg-config --exists libspa-0.2 || (echo "Error: libspa-0.2 not found. Install spa development packages." && exit 1)
	@pkg-config --exists gio-2.0 || (echo "Error: gio-2.0 not found. Install glib development packages." && exit 1)
	@pkg-config --exists gio-unix-2.0 || (echo "Error: gio-unix-2.0 not found. Install glib development packages." && exit 1)
	@pkg-config --exists egl 2>/dev/null || echo "Warning: EGL not found. DMA buffer support will be limited."
	@pkg-config --exists glesv2 2>/dev/null || echo "Warning: OpenGL ES 2.0 not found. DMA buffer support will be limited."
	@echo "Dependencies OK"

install: $(TARGET)
	install -D $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

# Development dependencies installation help
deps-install-ubuntu:
	sudo apt update
	sudo apt install build-essential pkg-config libpipewire-0.3-dev libspa-0.2-dev libglib2.0-dev libegl1-mesa-dev libgles2-mesa-dev linux-headers-$(shell uname -r)

deps-install-fedora:
	sudo dnf install gcc make pkg-config pipewire-devel glib2-devel mesa-libEGL-devel mesa-libGLES-devel kernel-headers

deps-install-arch:
	sudo pacman -S base-devel pkg-config pipewire glib2 libyuv mesa linux-headers