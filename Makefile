# Koboard — build orchestration.

VERSION        := 0.3.0

NICKELTC_IMAGE ?= ghcr.io/pgaskin/nickeltc:1.0
CROSS_TC       := arm-nickel-linux-gnueabihf
FBINK_VERSION  := v1.25.0
FBINK_REPO     := https://github.com/NiLuJe/FBInk.git
FBINK_DIR      := vendor/FBInk

CJSON_URL      := https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.15/cJSON.c
CJSON_H_URL    := https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.15/cJSON.h
CJSON_DIR      := vendor/cJSON

FONT_NAME      := DejaVuSans.ttf
FONT_BOLD_NAME := DejaVuSans-Bold.ttf
FONTS_DIR      := fonts
FONTS          := $(FONTS_DIR)/$(FONT_NAME) $(FONTS_DIR)/$(FONT_BOLD_NAME)
# The fonts' licences require their text to travel with every copy, the
# device package included.
FONT_LICENSES  := $(FONTS_DIR)/LICENSE-DejaVu.txt $(FONTS_DIR)/OFL-NotoEmoji.txt

KOBO_MOUNT     ?= /run/media/$(USER)/KOBOeReader

CFLAGS         ?= -Wall -Wextra -Wno-missing-field-initializers -O2 -std=gnu11
UNIBREAK_DIR   := $(FBINK_DIR)/libunibreak/src
STB_DIR        := $(FBINK_DIR)/stb
CPPFLAGS       += -I$(FBINK_DIR) -I$(CJSON_DIR) -I$(STB_DIR) -I$(UNIBREAK_DIR) \
                  -I$(FBINK_DIR)/libevdev-staged/include/libevdev-1.0 \
                  -DKOBOARD_VERSION='"$(VERSION)"'
LDLIBS         += -lm -lrt
EVDEV_LDFLAGS  := -L$(FBINK_DIR)/libevdev-staged/lib
EVDEV_LIBS     := -l:libevdev.a

PODMAN_MAKE := podman run --rm \
	--volume="$(CURDIR):$(CURDIR):Z" \
	--userns=keep-id \
	--user="$(shell id -u):$(shell id -g)" \
	--workdir="$(CURDIR)" \
	--entrypoint=make \
	$(NICKELTC_IMAGE)

# The UI engine (tree, layout, text measurement, paint walk) is portable: it is
# compiled unchanged into both the device binary and the host preview.
UI_SRCS := src/ui.c src/ui_layout.c src/ui_paint.c src/ui_text.c \
           src/ui_stbtt.c src/ui_json.c src/screens.c
UB_SRCS := $(UNIBREAK_DIR)/linebreak.c $(UNIBREAK_DIR)/linebreakdata.c \
           $(UNIBREAK_DIR)/linebreakdef.c $(UNIBREAK_DIR)/unibreakbase.c \
           $(UNIBREAK_DIR)/unibreakdef.c

# libunibreak arrives with the FBInk checkout. Saying so is what lets a fresh
# clone build: without it, make looks for these files before it has cloned
# anything, and stops.
$(UB_SRCS): | $(FBINK_DIR)

# Headers are listed as prerequisites too: without them, editing a struct or a
# constant leaves a stale binary behind.
HDRS := $(wildcard src/*.h)

SRCS := src/main.c src/http.c src/config.c src/policy.c src/touch.c \
        src/paint_fbink.c \
        $(UI_SRCS) $(UB_SRCS) $(CJSON_DIR)/cJSON.c
OBJS := $(patsubst %.c,build/%.o,$(SRCS))

# ---- host preview ----
HOST_CC       ?= cc
HOST_CFLAGS   ?= -Wall -Wextra -Wno-missing-field-initializers -O2 -std=gnu11 -g
HOST_CPPFLAGS := -Isrc -I$(CJSON_DIR) -I$(STB_DIR) -I$(UNIBREAK_DIR)
PREVIEW_SRCS  := src/preview.c src/paint_stb.c src/http.c src/config.c \
                 $(UI_SRCS) $(UB_SRCS) $(CJSON_DIR)/cJSON.c
PREVIEW_BIN   := build/koboard-preview

# Which screens to render for `make preview`; both by default, so a layout is
# never tuned for one panel by accident.
PREVIEW_DEVICES ?= glo glohd
FIXTURE         ?= typical
# Fixed clock, so previews and golden files are reproducible.
PREVIEW_NOW     ?= 1758880860
# A fixture is a UI tree, exactly what a server sends; UI=path renders any other
# tree file instead (a server's own output, say).
UI              ?= fixtures/$(FIXTURE).json

DIST_APP := dist/.adds/koboard
DIST_NM  := dist/.adds/nm

.PHONY: all toolchain fbink font app dist install clean distclean \
        inner-fbink inner-app preview preview-bin test golden open-preview

all: dist

toolchain:
	podman pull $(NICKELTC_IMAGE)

$(FBINK_DIR):
	mkdir -p vendor
	git clone --depth 1 --recurse-submodules --shallow-submodules \
		--branch $(FBINK_VERSION) $(FBINK_REPO) $(FBINK_DIR)

$(CJSON_DIR)/cJSON.c $(CJSON_DIR)/cJSON.h:
	mkdir -p $(CJSON_DIR)
	curl -sL $(CJSON_URL) -o $(CJSON_DIR)/cJSON.c
	curl -sL $(CJSON_H_URL) -o $(CJSON_DIR)/cJSON.h

vendor: $(FBINK_DIR) $(CJSON_DIR)/cJSON.c

# The fonts are DejaVu Sans with the weather glyphs from Noto Emoji merged in
# (FBInk's fbink_add_ot_font replaces rather than appends, so they can't be a
# separate fallback). scripts/make-font.sh reproduces that; the stamp keeps make
# from re-downloading on every build once the fonts are present. Run
# scripts/make-font.sh directly to regenerate a checked-in font.
$(FONTS): $(FONTS_DIR)/.dejavu-stamp
$(FONTS_DIR)/.dejavu-stamp: scripts/make-font.sh scripts/merge-emoji.py
	@mkdir -p $(FONTS_DIR)
	@if [ -f "$(FONTS_DIR)/$(FONT_NAME)" ] && [ -f "$(FONTS_DIR)/$(FONT_BOLD_NAME)" ]; then \
		echo "fonts present; run scripts/make-font.sh to regenerate"; \
	else \
		scripts/make-font.sh; \
	fi
	@touch $@

font: $(FONTS)

fbink: $(FBINK_DIR)/Release/libfbink.a

$(FBINK_DIR)/Release/libfbink.a: | $(FBINK_DIR)
	$(PODMAN_MAKE) inner-fbink

app: build/koboard

build/koboard: $(SRCS) $(HDRS) $(FBINK_DIR)/Release/libfbink.a
	mkdir -p build
	$(PODMAN_MAKE) inner-app

dist: build/koboard scripts/koboard.sh nm/koboard $(FONTS) $(FONT_LICENSES) LICENSE
	rm -rf dist
	mkdir -p $(DIST_APP)/bin $(DIST_APP)/fonts $(DIST_NM)
	cp build/koboard $(DIST_APP)/bin/koboard
	cp LICENSE $(DIST_APP)/LICENSE
	cp scripts/koboard.sh $(DIST_APP)/koboard.sh
	cp $(FONTS) $(FONT_LICENSES) $(DIST_APP)/fonts/
	cp nm/koboard $(DIST_NM)/koboard
	tar -czf dist/koboard-$(VERSION).tgz -C dist .adds
	@echo "dist/ staged; copy dist/.adds onto the Kobo or run 'make install'"

install: dist
	@test -d "$(KOBO_MOUNT)/.kobo" || \
		{ echo "error: no Kobo mounted at $(KOBO_MOUNT) (override with KOBO_MOUNT=...)"; exit 1; }
	mkdir -p "$(KOBO_MOUNT)/.adds"
	cp -r dist/.adds/. "$(KOBO_MOUNT)/.adds/"
	@test -f "$(KOBO_MOUNT)/.adds/koboard/config" || { \
		printf "Screen URL (e.g. http://192.168.1.10:8080/dashboard): "; \
		read srv; \
		printf "API token (leave blank if the server needs none): "; \
		read tok; \
		cfg="$(KOBO_MOUNT)/.adds/koboard/config"; \
		echo "SERVER_1_NAME=Home" > "$$cfg"; \
		echo "SERVER_1_URL=$$srv" >> "$$cfg"; \
		echo "SERVER_1_TOKEN=$$tok" >> "$$cfg"; \
		echo "" >> "$$cfg"; \
		echo "# Add more servers to switch between from the menu:" >> "$$cfg"; \
		echo "#SERVER_2_NAME=Test" >> "$$cfg"; \
		echo "#SERVER_2_URL=" >> "$$cfg"; \
		echo "#SERVER_2_TOKEN=" >> "$$cfg"; \
	}
	sync
	@echo "installed; eject the Kobo and open the NickelMenu menu"

clean:
	rm -rf build dist

distclean: clean
	rm -rf vendor
	rm -rf $(FONTS_DIR)

# ---- host preview: renders the real layout without a Kobo ----
#
# Builds natively (no podman, no cross-compiler) against the same UI sources
# the device uses, so the edit -> look loop is a second rather than a reinstall.

preview-bin: $(PREVIEW_BIN)

$(PREVIEW_BIN): $(PREVIEW_SRCS) $(HDRS) $(CJSON_DIR)/cJSON.c | $(FBINK_DIR)
	mkdir -p build
	$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -o $@ $(PREVIEW_SRCS) -lm

# SCREEN=menu previews the client-drawn menu instead of a fixture dashboard.
SCREEN ?= dashboard

preview: $(PREVIEW_BIN) $(FONTS)
ifeq ($(SCREEN),menu)
	@for dev in $(PREVIEW_DEVICES); do \
		$(PREVIEW_BIN) --screen menu --config fixtures/servers.config \
			--device $$dev $(PREVIEW_ARGS) \
			--font $(FONTS_DIR)/$(FONT_NAME) \
			--font-bold $(FONTS_DIR)/$(FONT_BOLD_NAME) \
			--out build/preview-menu-$$dev.png || exit 1; \
	done
else
	@for dev in $(PREVIEW_DEVICES); do \
		$(PREVIEW_BIN) --ui $(UI) --device $$dev \
			--now $(PREVIEW_NOW) $(PREVIEW_ARGS) \
			--font $(FONTS_DIR)/$(FONT_NAME) \
			--font-bold $(FONTS_DIR)/$(FONT_BOLD_NAME) \
			--out build/preview-$(FIXTURE)-$$dev.png || exit 1; \
	done
endif

# Same, but fetched from a running server, the way the device fetches it.
preview-live: $(PREVIEW_BIN) $(FONTS)
	@test -n "$(URL)" || { echo "usage: make preview-live URL=http://host:8080/dashboard [TOKEN=...]"; exit 1; }
	$(PREVIEW_BIN) --url "$(URL)" --token "$(TOKEN)" \
		--device $(firstword $(PREVIEW_DEVICES)) \
		--font $(FONTS_DIR)/$(FONT_NAME) \
		--font-bold $(FONTS_DIR)/$(FONT_BOLD_NAME) \
		--out build/preview-live.png

open-preview: preview
ifeq ($(SCREEN),menu)
	@xdg-open build/preview-menu-$(firstword $(PREVIEW_DEVICES)).png 2>/dev/null || true
else
	@xdg-open build/preview-$(FIXTURE)-$(firstword $(PREVIEW_DEVICES)).png 2>/dev/null || true
endif

# Golden layout tests: every node's computed rect, compared against a checked-in
# dump. Catches layout regressions without a device and without eyeballing.
# Unit tests for the protocol side: URLs, the envelope, the refresh policy.
UNIT_BIN  := build/unit-tests
UNIT_SRCS := tests/unit.c src/http.c src/policy.c src/ui.c src/ui_json.c \
             $(CJSON_DIR)/cJSON.c

$(UNIT_BIN): $(UNIT_SRCS) $(HDRS)
	mkdir -p build
	$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -o $@ $(UNIT_SRCS) -lm

test: $(PREVIEW_BIN) $(UNIT_BIN) $(FONTS)
	@$(UNIT_BIN)
	@scripts/run-tests.sh $(PREVIEW_BIN) check

golden: $(PREVIEW_BIN) $(FONTS)
	@scripts/run-tests.sh $(PREVIEW_BIN) update

# ---- inner targets: run inside the NickelTC container only ----

inner-fbink:
	$(MAKE) -C $(FBINK_DIR) staticlib MINIMAL=1 OPENTYPE=1 CROSS_TC=$(CROSS_TC)
	$(MAKE) -C $(FBINK_DIR) libevdev.built CROSS_TC=$(CROSS_TC)

inner-app:
	mkdir -p $(sort $(dir $(OBJS)))
	$(foreach src,$(SRCS),\
		$(CROSS_TC)-gcc $(CPPFLAGS) $(CFLAGS) -c $(src) -o build/$(src:.c=.o) &&) true
	$(CROSS_TC)-gcc $(CPPFLAGS) $(CFLAGS) \
		-o build/koboard $(OBJS) \
		$(FBINK_DIR)/Release/libfbink.a \
		$(FBINK_DIR)/libi2c-staged/lib/libi2c.a \
		$(EVDEV_LDFLAGS) $(EVDEV_LIBS) \
		$(LDLIBS)
	$(CROSS_TC)-strip --strip-unneeded build/koboard
