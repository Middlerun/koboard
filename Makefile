# HomeSrv Kobo client — build orchestration.

VERSION        := 0.2.0

NICKELTC_IMAGE ?= ghcr.io/pgaskin/nickeltc:1.0
CROSS_TC       := arm-nickel-linux-gnueabihf
FBINK_VERSION  := v1.25.0
FBINK_REPO     := https://github.com/NiLuJe/FBInk.git
FBINK_DIR      := vendor/FBInk

CJSON_URL      := https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.15/cJSON.c
CJSON_H_URL    := https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.15/cJSON.h
CJSON_DIR      := vendor/cJSON

FONT_NAME      := DejaVuSans.ttf
FONT_VER       := version_2_37
FONT_ZIP_URL   := https://github.com/dejavu-fonts/dejavu-fonts/releases/download/$(FONT_VER)/dejavu-fonts-ttf-2.37.zip
FONTS_DIR      := fonts

KOBO_MOUNT     ?= /run/media/$(USER)/KOBOeReader

CFLAGS         ?= -Wall -Wextra -O2 -std=gnu11
CPPFLAGS       += -I$(FBINK_DIR) -I$(CJSON_DIR) \
                  -I$(FBINK_DIR)/libevdev-staged/include/libevdev-1.0 \
                  -DHOMESRV_VERSION='"$(VERSION)"'
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

SRCS := src/main.c src/http.c src/json.c \
        $(CJSON_DIR)/cJSON.c
OBJS := $(patsubst %.c,build/%.o,$(SRCS))

DIST_APP := dist/.adds/homesrv
DIST_NM  := dist/.adds/nm

.PHONY: all toolchain fbink font app dist install clean distclean \
        inner-fbink inner-app

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

$(FONTS_DIR)/$(FONT_NAME):
	mkdir -p $(FONTS_DIR)
	curl -sL "$(FONT_ZIP_URL)" -o /tmp/opencode/dejavu-fonts.zip
	unzip -o -j /tmp/opencode/dejavu-fonts.zip \
		"dejavu-fonts-ttf-2.37/ttf/$(FONT_NAME)" -d $(FONTS_DIR)/
	rm -f /tmp/opencode/dejavu-fonts.zip

font: $(FONTS_DIR)/$(FONT_NAME)

fbink: $(FBINK_DIR)/Release/libfbink.a

$(FBINK_DIR)/Release/libfbink.a: | $(FBINK_DIR)
	$(PODMAN_MAKE) inner-fbink

app: build/homesrv

build/homesrv: $(SRCS) $(CJSON_DIR)/cJSON.c $(FBINK_DIR)/Release/libfbink.a
	mkdir -p build
	$(PODMAN_MAKE) inner-app

dist: build/homesrv scripts/homesrv.sh nm/homesrv $(FONTS_DIR)/$(FONT_NAME)
	rm -rf dist
	mkdir -p $(DIST_APP)/bin $(DIST_APP)/fonts $(DIST_NM)
	cp build/homesrv $(DIST_APP)/bin/homesrv
	cp scripts/homesrv.sh $(DIST_APP)/homesrv.sh
	cp $(FONTS_DIR)/$(FONT_NAME) $(DIST_APP)/fonts/
	cp nm/homesrv $(DIST_NM)/homesrv
	tar -czf dist/homesrv-kobo-$(VERSION).tgz -C dist .adds
	@echo "dist/ staged; copy dist/.adds onto the Kobo or run 'make install'"

install: dist
	@test -d "$(KOBO_MOUNT)/.kobo" || \
		{ echo "error: no Kobo mounted at $(KOBO_MOUNT) (override with KOBO_MOUNT=...)"; exit 1; }
	mkdir -p "$(KOBO_MOUNT)/.adds"
	cp -r dist/.adds/. "$(KOBO_MOUNT)/.adds/"
	@test -f "$(KOBO_MOUNT)/.adds/homesrv/config" || { \
		printf "Server URL (e.g. http://192.168.1.50:8080): "; \
		read srv; \
		printf "API token (from HomeSrv Settings page): "; \
		read tok; \
		echo "SERVER_URL=$$srv" > "$(KOBO_MOUNT)/.adds/homesrv/config"; \
		echo "API_TOKEN=$$tok" >> "$(KOBO_MOUNT)/.adds/homesrv/config"; \
	}
	sync
	@echo "installed; eject the Kobo and open the NickelMenu menu"

clean:
	rm -rf build dist

distclean: clean
	rm -rf vendor
	rm -rf $(FONTS_DIR)

# ---- inner targets: run inside the NickelTC container only ----

inner-fbink:
	$(MAKE) -C $(FBINK_DIR) staticlib MINIMAL=1 OPENTYPE=1 CROSS_TC=$(CROSS_TC)
	$(MAKE) -C $(FBINK_DIR) libevdev.built CROSS_TC=$(CROSS_TC)

inner-app:
	mkdir -p build/src build/vendor/cJSON
	$(CROSS_TC)-gcc $(CPPFLAGS) $(CFLAGS) -c src/http.c -o build/src/http.o
	$(CROSS_TC)-gcc $(CPPFLAGS) $(CFLAGS) -c src/json.c -o build/src/json.o
	$(CROSS_TC)-gcc $(CPPFLAGS) $(CFLAGS) -c src/main.c -o build/src/main.o
	$(CROSS_TC)-gcc $(CPPFLAGS) $(CFLAGS) -c $(CJSON_DIR)/cJSON.c -o build/vendor/cJSON/cJSON.o
	$(CROSS_TC)-gcc $(CPPFLAGS) $(CFLAGS) \
		-o build/homesrv \
		build/src/main.o build/src/http.o build/src/json.o \
		build/vendor/cJSON/cJSON.o \
		$(FBINK_DIR)/Release/libfbink.a \
		$(FBINK_DIR)/libi2c-staged/lib/libi2c.a \
		$(EVDEV_LDFLAGS) $(EVDEV_LIBS) \
		$(LDLIBS)
	$(CROSS_TC)-strip --strip-unneeded build/homesrv
