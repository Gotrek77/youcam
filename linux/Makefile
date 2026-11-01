PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin
APPDIR := $(PREFIX)/share/applications
ICONDIR := $(PREFIX)/share/icons/hicolor
PKGS := gtk+-3.0 gstreamer-1.0 gstreamer-pbutils-1.0
CFLAGS ?= -O2
CFLAGS += -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

CORE := geometry.o support.o prefs.o camera_layer.o
MEDIA := writer.o audio.o
APP := main.o app.o overlay.o panel.o region.o area_overlay.o statusicon.o \
       recording.o portal.o hotkeys.o camera.o $(MEDIA) $(CORE)
ICON_SIZES := 16 32 48 128 256 512

.PHONY: all test smoke-test install uninstall clean

all: youcam

youcam: $(APP)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tests: tests.o $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test-media: test_media.o $(MEDIA) $(CORE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

make-icon: make_icon.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(APP) tests.o test_media.o make_icon.o: youcam.h

test: tests test-media
	./tests
	./test-media

smoke-test: youcam
	YOUCAM_CONFIG=$$(mktemp -u)/prefs.ini ./youcam --smoke-test

icons: make-icon
	./make-icon icons $(ICON_SIZES)
	touch icons

# The desktop file gives the portal an app id, which global shortcuts require.
install: youcam icons
	install -Dm755 youcam $(BINDIR)/youcam
	for s in $(ICON_SIZES); do install -Dm644 icons/youcam-$$s.png $(ICONDIR)/$${s}x$${s}/apps/youcam.png; done
	sed 's|@BINDIR@|$(BINDIR)|' app.youcam.YouCam.desktop | install -Dm644 /dev/stdin $(APPDIR)/app.youcam.YouCam.desktop
	-update-desktop-database $(APPDIR)
	-gtk-update-icon-cache -q -t $(ICONDIR)

uninstall:
	rm -f $(BINDIR)/youcam $(APPDIR)/app.youcam.YouCam.desktop
	for s in $(ICON_SIZES); do rm -f $(ICONDIR)/$${s}x$${s}/apps/youcam.png; done

clean:
	rm -rf *.o youcam tests test-media make-icon icons
