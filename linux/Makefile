PREFIX ?= $(HOME)/.local
LIBDIR := $(PREFIX)/share/youcam
BINDIR := $(PREFIX)/bin
APPDIR := $(PREFIX)/share/applications
SOURCES := youcam.py logic.py media.py portal.py region.py

.PHONY: run test smoke-test install uninstall

run:
	python3 youcam.py

test:
	python3 logic.py
	python3 test_media.py

smoke-test:
	python3 youcam.py --smoke-test

# The desktop file gives the portal an app id, which global shortcuts require.
install:
	install -Dm644 -t $(LIBDIR) $(SOURCES)
	mkdir -p $(BINDIR)
	printf '#!/bin/sh\nexec python3 %s/youcam.py "$$@"\n' "$(LIBDIR)" > $(BINDIR)/youcam
	chmod 755 $(BINDIR)/youcam
	sed 's|@BINDIR@|$(BINDIR)|' app.youcam.YouCam.desktop | install -Dm644 /dev/stdin $(APPDIR)/app.youcam.YouCam.desktop
	-update-desktop-database $(APPDIR)

uninstall:
	rm -rf $(LIBDIR) $(BINDIR)/youcam $(APPDIR)/app.youcam.YouCam.desktop
