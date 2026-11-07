# hdus - SKNET MonsterTV HDUS out-of-tree kernel module
#
# Driver objects:
#   hdus_drv.o         - USB/char-device core, firmware upload,
#                        recpt1-compatible PT1/PT3 ioctl ABI (magic 0x8d)
#   hdus_descrambler.o - descrambler (read() returns descrambled TS)
#
# Firmware upload lives in hdus_drv.c (no separate hdus_fw.c is
# required; if a future hdus_fw.c is split out, add hdus_fw.o to
# hdus-objs below).

obj-m += hdus.o
hdus-objs := hdus_drv.o hdus_descrambler.o

# Kernel build tree. Override at build time, e.g.:
#   make KERNELDIR=/lib/modules/$(uname -r)/build
#   make KERNELDIR=/usr/src/linux-headers-$(uname -r)
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

KVER      := $(shell uname -r)

CC        ?= gcc
CFLAGS_TEST := -O2 -Wall -Wextra

LOADER_SRC := hdus_as11loader.c
LOADER_BIN := hdus_as11loader
TEST_SRC   := tools/hdus_test.c
TEST_BIN   := tools/hdus_test

# B25 / PC/SC tools (libaribb25). ARIB25_DIR points at a recfriio
# checkout's arib25/ (override: make ARIB25_DIR=/path/to/arib25 b25).
ARIB25_DIR ?= $(HOME)/projects/recfriio/arib25
# ECM-caching fork of libaribb25 (third_party/arib25_patched); used by
# tools/hdus_b25_ecm, which removes redundant card round-trips.
ARIB25_PATCHED_DIR ?= third_party/arib25_patched
ARIB25_PATCHED_OBJS := $(ARIB25_PATCHED_DIR)/arib_std_b25.c \
                       $(ARIB25_PATCHED_DIR)/b_cas_card.c \
                       $(ARIB25_PATCHED_DIR)/multi2.c \
                       $(ARIB25_PATCHED_DIR)/ts_section_parser.c
ARIB25_OBJS := $(ARIB25_DIR)/arib_std_b25.c $(ARIB25_DIR)/b_cas_card.c \
               $(ARIB25_DIR)/multi2.c $(ARIB25_DIR)/ts_section_parser.c
# NOTE: td.c is intentionally omitted - it is the upstream standalone
# test program with its own main().
PCSC_CFLAGS := $(shell pkg-config --cflags libpcsclite 2>/dev/null)
PCSC_LIBS   := $(shell pkg-config --libs libpcsclite 2>/dev/null)
B25_BIN     := tools/hdus_b25
B25ECM_BIN  := tools/hdus_b25_ecm
BCAS_BIN    := tools/bcas_check
DESC_TEST   := tools/descrambler_test

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

.PHONY: all modules loader test b25 bcas desctest clean install uninstall load unload help

all: modules loader test b25 b25ecm bcas desctest

## Build the kernel module only.
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

## Build the userspace AS11 firmware loader (hdus_as11loader.c).
loader: $(LOADER_BIN)

$(LOADER_BIN): $(LOADER_SRC)
	$(CC) $(CFLAGS_TEST) -o $@ $<

## Build the userspace test tool (tools/hdus_test.c).
## tools/hdus_test.c includes ../hdus_ioctl.h when present and
## falls back to locally defined ioctl numbers when it is not.
test: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) hdus_ioctl.h
	$(CC) $(CFLAGS_TEST) -o $@ $<

## B25 decoder (TS stdin -> decoded stdout, via PC/SC B-CAS card).
b25: $(B25_BIN)

$(B25_BIN): tools/hdus_b25.c $(ARIB25_OBJS)
	$(CC) $(CFLAGS_TEST) $(PCSC_CFLAGS) -I$(ARIB25_DIR) -o $@ tools/hdus_b25.c $(ARIB25_OBJS) $(PCSC_LIBS) -lm

## B25 decoder with the ECM result cache (fewer card round-trips).
b25ecm: $(B25ECM_BIN)

$(B25ECM_BIN): tools/hdus_b25.c $(ARIB25_PATCHED_OBJS)
	$(CC) $(CFLAGS_TEST) $(PCSC_CFLAGS) -I$(ARIB25_PATCHED_DIR) -o $@ tools/hdus_b25.c $(ARIB25_PATCHED_OBJS) $(PCSC_LIBS) -lm

## B-CAS card PC/SC diagnostic.
bcas: $(BCAS_BIN)

$(BCAS_BIN): tools/bcas_check.c $(ARIB25_OBJS)
	$(CC) $(CFLAGS_TEST) $(PCSC_CFLAGS) -I$(ARIB25_DIR) -o $@ tools/bcas_check.c $(ARIB25_OBJS) $(PCSC_LIBS) -lm

## Descrambler userspace self-test.
desctest: $(DESC_TEST)

$(DESC_TEST): tools/descrambler_test.c hdus_descrambler.c hdus_descrambler.h
	$(CC) $(CFLAGS_TEST) -o $@ tools/descrambler_test.c hdus_descrambler.c

# Note: hdus_ioctl.h is created alongside this Makefile (agent A).
# It is deliberately not a hard prerequisite: tools/hdus_test.c
# compiles standalone via __has_include fallback when the header
# is not present yet, e.g.:
#   gcc -O2 -Wall -o tools/hdus_test tools/hdus_test.c

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean || true
	rm -f $(LOADER_BIN) $(TEST_BIN) $(B25_BIN) $(B25ECM_BIN) $(BCAS_BIN) \
	      $(DESC_TEST) \
	      *.ko *.o *.mod *.mod.c *.mod.o \
	      *.symvers *.order .*.cmd Module.symvers modules.order \
	      tools/*.o
	rm -rf .tmp_versions

## Install module, loader, udev rule and firmware directory.
## Requires sudo. Firmware files (if any) are expected in ./firmware/.
install: modules loader test b25 bcas
	sudo install -D -m 644 hdus.ko /lib/modules/$(KVER)/extra/hdus.ko
	sudo depmod -a
	sudo install -D -m 755 $(LOADER_BIN) /usr/local/bin/$(LOADER_BIN)
	sudo install -D -m 755 $(TEST_BIN) /usr/local/bin/hdus_test
	sudo install -D -m 755 $(B25_BIN) /usr/local/bin/hdus_b25
	sudo install -D -m 755 $(BCAS_BIN) /usr/local/bin/bcas_check
	sudo install -D -m 644 99-hdusvideo.rules /etc/udev/rules.d/99-hdusvideo.rules
	sudo udevadm control --reload-rules || true
	sudo udevadm trigger --subsystem-match=usb || true
	if [ -d firmware ] && [ -n "$$(ls -A firmware 2>/dev/null)" ]; then \
		sudo install -d -m 755 /lib/firmware/hdus; \
		sudo install -m 644 firmware/* /lib/firmware/hdus/; \
		echo "firmware installed to /lib/firmware/hdus/"; \
	else \
		echo "no firmware files in ./firmware/ - skipped /lib/firmware/hdus/"; \
	fi
	@echo "installed: hdus.ko -> /lib/modules/$(KVER)/extra/, $(LOADER_BIN) -> /usr/local/bin/, 99-hdusvideo.rules -> /etc/udev/rules.d/"
	@echo "next: sudo make load   (or reboot / plug the device in)"

## Remove installed files.
uninstall:
	sudo rm -f /lib/modules/$(KVER)/extra/hdus.ko
	sudo rm -f /usr/local/bin/$(LOADER_BIN) /usr/local/bin/hdus_test \
		/usr/local/bin/hdus_b25 /usr/local/bin/bcas_check
	sudo rm -f /etc/udev/rules.d/99-hdusvideo.rules
	sudo rm -rf /lib/firmware/hdus
	sudo depmod -a
	sudo udevadm control --reload-rules || true
	@echo "uninstalled hdus"

## Load the module. The device must be in firmware-loaded state
## (VID:PID 3275:6051); run hdus_as11loader first if needed.
## Usage: sudo make load [DEV=hdusvideo0]
load: modules
	sudo insmod hdus.ko || sudo modprobe hdus
	@echo "loaded. check: ls -l /dev/hdusvideo* ; dmesg | tail -20"
	@echo "if the device shows 1738:5211 (not 3275:6051), run:"
	@echo "  sudo /usr/local/bin/hdus_as11loader  (needs SKNET_AS11Loader.sys)"

## Unload the module.
unload:
	-sudo rmmod hdus || true
	@echo "unloaded (if it was loaded)"

help:
	@echo "hdus build targets:"
	@echo "  make            - modules + loader + test + b25 + bcas + desctest"
	@echo "  make modules    - kernel module only (hdus.ko)"
	@echo "  make loader     - hdus_as11loader (firmware loader)"
	@echo "  make test       - tools/hdus_test (recpt1-style test tool)"
	@echo "  make b25        - tools/hdus_b25 (B25 decode via PC/SC B-CAS)"
	@echo "  make bcas       - tools/bcas_check (PC/SC B-CAS diagnostic)"
	@echo "  make desctest   - tools/descrambler_test (descrambler self-test)"
	@echo "  make install    - install module/loader/udev/firmware (sudo)"
	@echo "  make uninstall  - remove installed files (sudo)"
	@echo "  make load       - insmod/modprobe hdus (firmware must be loaded)"
	@echo "  make unload     - rmmod hdus"
	@echo "  make clean      - clean build artifacts"
	@echo "  KERNELDIR=...   - override kernel build tree (default /lib/modules/\$$(uname -r)/build)"
