# HomeSrv Kobo client — build orchestration.
#
# Outer targets run on the host and drive podman with the NickelTC toolchain
# image (the same toolchain NickelMenu is built with). Inner targets (inner-*)
# are what actually runs inside the container, where the
# arm-nickel-linux-gnueabihf cross toolchain is on PATH.

VERSION        := 0.1.0

NICKELTC_IMAGE ?= ghcr.io/pgaskin/nickeltc:1.0
CROSS_TC       := arm-nickel-linux-gnueabihf
FBINK_VERSION  := v1.25.0
FBINK_REPO     := https://github.com/NiLuJe/FBInk.git
FBINK_DIR      := vendor/FBInk

KOBO_MOUNT     ?= /run/media/$(USER)/KOBOeReader

CFLAGS         ?= -Wall -Wextra -O2 -std=gnu11
CPPFLAGS       += -I$(FBINK_DIR) -DHOMESRV_VERSION='"$(VERSION)"'
LDLIBS         += -lm -lrt

PODMAN_MAKE := podman run --rm \
	--volume="$(CURDIR):$(CURDIR):Z" \
	--userns=keep-id \
	--user="$(shell id -u):$(shell id -g)" \
	--workdir="$(CURDIR)" \
	--entrypoint=make \
	$(NICKELTC_IMAGE)

DIST_APP := dist/.adds/homesrv
DIST_NM  := dist/.adds/nm

.PHONY: all toolchain fbink app dist install clean distclean \
        inner-fbink inner-app

all: dist

toolchain:
	podman pull $(NICKELTC_IMAGE)

$(FBINK_DIR):
	mkdir -p vendor
	git clone --depth 1 --recurse-submodules --shallow-submodules \
		--branch $(FBINK_VERSION) $(FBINK_REPO) $(FBINK_DIR)

fbink: $(FBINK_DIR)/Release/libfbink.a

$(FBINK_DIR)/Release/libfbink.a: | $(FBINK_DIR)
	$(PODMAN_MAKE) inner-fbink

app: build/homesrv

build/homesrv: src/main.c $(FBINK_DIR)/Release/libfbink.a
	$(PODMAN_MAKE) inner-app

dist: build/homesrv scripts/homesrv.sh nm/homesrv
	rm -rf dist
	mkdir -p $(DIST_APP)/bin $(DIST_NM)
	cp build/homesrv $(DIST_APP)/bin/homesrv
	cp scripts/homesrv.sh $(DIST_APP)/homesrv.sh
	cp nm/homesrv $(DIST_NM)/homesrv
	tar -czf dist/homesrv-kobo-$(VERSION).tgz -C dist .adds
	@echo "dist/ staged; copy dist/.adds onto the Kobo or run 'make install'"

install: dist
	@test -d "$(KOBO_MOUNT)/.kobo" || \
		{ echo "error: no Kobo mounted at $(KOBO_MOUNT) (override with KOBO_MOUNT=...)"; exit 1; }
	mkdir -p "$(KOBO_MOUNT)/.adds"
	cp -r dist/.adds/. "$(KOBO_MOUNT)/.adds/"
	sync
	@echo "installed; eject the Kobo and open the NickelMenu menu"

clean:
	rm -rf build dist

distclean: clean
	rm -rf vendor

# ---- inner targets: run inside the NickelTC container only ----

inner-fbink:
	$(MAKE) -C $(FBINK_DIR) staticlib MINIMAL=1 BITMAP=1 CROSS_TC=$(CROSS_TC)

inner-app:
	mkdir -p build
	$(CROSS_TC)-gcc $(CPPFLAGS) $(CFLAGS) -o build/homesrv src/main.c \
		$(FBINK_DIR)/Release/libfbink.a \
		$(FBINK_DIR)/libi2c-staged/lib/libi2c.a $(LDLIBS)
	$(CROSS_TC)-strip --strip-unneeded build/homesrv
