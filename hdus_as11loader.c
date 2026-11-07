/*-
 * Copyright (c) 2008 FUKAUMI Naoki.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * hdus_as11loader - userspace firmware loader for SKNET MonsterTV HDUS
 * (ASICEN AUSBDTV, USB 1738:5211, pre-firmware).
 *
 * Based on as11loader by FUKAUMI Naoki (recfriio) and the usbdevfs
 * ioctl approach used by hdus-linux fw_upload.c. No libusb dependency:
 * all transfers go through USBDEVFS_CONTROL ioctls on /dev/bus/usb
 * (fallback: /proc/bus/usb).
 *
 * After a successful upload the device re-enumerates as 3275:6051
 * (MonsterTV HDUS) and the kernel driver binds.
 *
 * Usage:
 *   hdus_as11loader [--dry-run] [firmware]
 *   hdus_as11loader --dry-run [firmware]
 *
 * Firmware formats:
 *   (a) SKNET_AS11Loader.sys - 2-byte header (66 4d = CD1.0 / 21 51 =
 *       CD1.1) at offset 0xa88, 0x4000-byte firmware at offset 0xa90.
 *       idx = (b1 << 8) | b0.
 *   (b) raw as11fw.bin - 2-byte header as above + >= 0x4000-byte body.
 *   (c) raw 0x4000 binary - no header, idx defaults to 0x4d66.
 *
 * Default firmware search (when no argument is given):
 *   ./SKNET_AS11Loader.sys
 *   ./firmware/SKNET_AS11Loader.sys
 *   ~/projects/hdus-linux/firmware/SKNET_AS11Loader.sys
 *   ./as11fw.bin
 *   ./firmware/as11fw.bin
 *
 * --dry-run parses the firmware and prints idx/size/blocks without
 * touching any USB device.
 *
 * Compile:
 *   gcc -O2 -Wall -Wextra -o hdus_as11loader hdus_as11loader.c
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define VID_PRE_FW   0x1738
#define PID_PRE_FW   0x5211
#define VID_POST_FW  0x3275
#define PID_POST_FW  0x6051

#define FIRM_SIZE    0x4000
#define SYS_HDR_OFF  0xa88
#define SYS_FW_OFF   0xa90
#define CTRL_TIMEOUT 5000

struct block {
	uint8_t req;
	uint16_t off;
	uint16_t len;
};

/* From FUKAUMI Naoki's as11loader: 4 blocks covering 0x4000 bytes. */
static const struct block fw_blocks[] = {
	{ 0xab, 0x0000, 0x0c00 },
	{ 0xab, 0x2000, 0x0400 },
	{ 0xab, 0x2800, 0x1000 },
	{ 0xac, 0x3800, 0x0800 },
};
#define N_BLOCKS (sizeof(fw_blocks) / sizeof(fw_blocks[0]))

static int hdr_match(uint8_t b0, uint8_t b1)
{
	return (b0 == 0x66 && b1 == 0x4d) || /* CD 1.0 */
	       (b0 == 0x21 && b1 == 0x51);  /* CD 1.1 */
}

static const char *hdr_name(uint8_t b0)
{
	return (b0 == 0x66) ? "CD1.0" : "CD1.1";
}

/* ------------------------------------------------------------------ */
/* Firmware loading                                                    */
/* ------------------------------------------------------------------ */

struct firmware {
	uint8_t *data;	/* always FIRM_SIZE bytes */
	uint16_t idx;
	char src[512];
	char type[128];
	long file_size;
};

/*
 * Load and validate one firmware file. On success returns 0 and fills
 * *fw (caller frees fw->data). On failure returns -1 and prints a
 * diagnostic naming the format that was expected.
 */
static int load_firmware_file(const char *path, struct firmware *fw)
{
	FILE *f;
	long sz;
	uint8_t *buf;
	size_t n;

	memset(fw, 0, sizeof(*fw));
	snprintf(fw->src, sizeof(fw->src), "%s", path);

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "hdus_as11loader: cannot open %s: %s\n",
			path, strerror(errno));
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "hdus_as11loader: cannot size %s: %s\n",
			path, strerror(errno));
		fclose(f);
		return -1;
	}
	fw->file_size = sz;
	buf = malloc(sz > 0 ? (size_t)sz : 1);
	if (!buf) {
		fprintf(stderr, "hdus_as11loader: out of memory\n");
		fclose(f);
		return -1;
	}
	n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (n != (size_t)sz) {
		fprintf(stderr, "hdus_as11loader: short read on %s\n", path);
		free(buf);
		return -1;
	}

	fw->data = malloc(FIRM_SIZE);
	if (!fw->data) {
		fprintf(stderr, "hdus_as11loader: out of memory\n");
		free(buf);
		return -1;
	}

	/* (a) SKNET_AS11Loader.sys: header at 0xa88, fw at 0xa90. */
	if (sz >= SYS_FW_OFF + 2 &&
	    hdr_match(buf[SYS_HDR_OFF], buf[SYS_HDR_OFF + 1])) {
		if (sz < SYS_FW_OFF + FIRM_SIZE) {
			fprintf(stderr,
				"hdus_as11loader: %s: header at 0xa88 ok "
				"but file too small for 0x4000-byte fw at "
				"0xa90 (size %ld, need %d)\n",
				path, sz, SYS_FW_OFF + FIRM_SIZE);
			goto fail;
		}
		fw->idx = (uint16_t)(((uint16_t)buf[SYS_HDR_OFF + 1] << 8) |
				     buf[SYS_HDR_OFF]);
		memcpy(fw->data, buf + SYS_FW_OFF, FIRM_SIZE);
		snprintf(fw->type, sizeof(fw->type),
			 "SKNET_AS11Loader.sys (header at 0xa88, fw at 0xa90, %s)",
			 hdr_name(buf[SYS_HDR_OFF]));
		free(buf);
		return 0;
	}

	/* (b) Raw firmware with 2-byte header. */
	if (sz >= 2 && hdr_match(buf[0], buf[1])) {
		if (sz - 2 < FIRM_SIZE) {
			fprintf(stderr,
				"hdus_as11loader: %s: 2-byte header ok "
				"but body %ld bytes < 0x4000\n",
				path, sz - 2);
			goto fail;
		}
		fw->idx = (uint16_t)(((uint16_t)buf[1] << 8) | buf[0]);
		memcpy(fw->data, buf + 2, FIRM_SIZE);
		snprintf(fw->type, sizeof(fw->type),
			 "raw firmware with 2-byte header (%s)",
			 hdr_name(buf[0]));
		free(buf);
		return 0;
	}

	/* (c) Raw 0x4000 binary, default idx. Exact size only, so that a
	 * larger PE/.sys file is never silently mistaken for firmware. */
	if (sz == FIRM_SIZE) {
		fw->idx = 0x4d66;
		memcpy(fw->data, buf, FIRM_SIZE);
		snprintf(fw->type, sizeof(fw->type),
			 "raw 0x4000 binary (default idx=0x4d66)");
		free(buf);
		return 0;
	}

	fprintf(stderr,
		"hdus_as11loader: %s: unrecognised firmware format "
		"(size %ld, first2 %02x %02x",
		path, sz,
		sz >= 2 ? buf[0] : 0, sz >= 2 ? buf[1] : 0);
	if (sz >= SYS_HDR_OFF + 2)
		fprintf(stderr, ", bytes at 0xa88: %02x %02x",
			buf[SYS_HDR_OFF], buf[SYS_HDR_OFF + 1]);
	fprintf(stderr,
		")\n"
		"  expected: (a) SKNET_AS11Loader.sys with 66 4d / 21 51 "
		"at 0xa88 + 0x4000 bytes at 0xa90,\n"
		"            (b) 2-byte 66 4d / 21 51 header + >=0x4000 body, or\n"
		"            (c) exactly 0x4000 raw bytes\n");
	if (sz >= 2 && buf[0] == 'M' && buf[1] == 'Z')
		fprintf(stderr,
			"  note: this looks like a PE/Windows driver file "
			"without the 0xa88 header - an older SKNET_AS11Loader.sys "
			"(or a pre-extracted as11fw.bin) is required\n");
fail:
	free(buf);
	free(fw->data);
	fw->data = NULL;
	return -1;
}

static void print_firmware_info(const struct firmware *fw, int dry_run)
{
	size_t i;

	printf("firmware: %s\n", fw->src);
	printf("  type: %s\n", fw->type);
	printf("  file size: %ld bytes\n", fw->file_size);
	printf("  idx: 0x%04x\n", fw->idx);
	printf("  firmware body: 0x%x bytes (%d blocks)\n",
	       FIRM_SIZE, (int)N_BLOCKS);
	for (i = 0; i < N_BLOCKS; i++)
		printf("    block %zu: req=0x%02x off=0x%04x len=0x%04x\n",
		       i + 1, fw_blocks[i].req, fw_blocks[i].off,
		       fw_blocks[i].len);
	if (dry_run)
		printf("  dry-run: no USB transfer sent\n");
	fflush(stdout);
}

/*
 * Resolve the firmware: explicit path, or the default search list.
 * In default mode a candidate that exists but fails validation is
 * reported and the next candidate is tried.
 */
static int resolve_firmware(const char *explicit, struct firmware *fw)
{
	static const char *const rel[] = {
		"./SKNET_AS11Loader.sys",
		"./firmware/SKNET_AS11Loader.sys",
		"~/projects/hdus-linux/firmware/SKNET_AS11Loader.sys",
		"./as11fw.bin",
		"./firmware/as11fw.bin",
	};
	size_t i;
	int tried = 0;

	if (explicit)
		return load_firmware_file(explicit, fw);

	for (i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
		char path[1024];
		const char *p = rel[i];

		if (p[0] == '~' && p[1] == '/') {
			const char *home = getenv("HOME");
			if (!home)
				continue;
			snprintf(path, sizeof(path), "%s/%s", home, p + 2);
			p = path;
		}
		if (access(p, R_OK) != 0)
			continue;
		tried++;
		if (load_firmware_file(p, fw) == 0)
			return 0;
		fprintf(stderr,
			"hdus_as11loader: skipping invalid default "
			"firmware %s\n", p);
	}
	if (tried == 0)
		fprintf(stderr,
			"hdus_as11loader: no default firmware found "
			"(looked for ./SKNET_AS11Loader.sys, "
			"./firmware/SKNET_AS11Loader.sys, "
			"~/projects/hdus-linux/firmware/SKNET_AS11Loader.sys, "
			"./as11fw.bin, ./firmware/as11fw.bin)\n");
	else
		fprintf(stderr,
			"hdus_as11loader: no valid firmware found in "
			"default search paths\n");
	fprintf(stderr,
		"  usage: hdus_as11loader [--dry-run] [firmware]\n");
	return -1;
}

/* ------------------------------------------------------------------ */
/* USB device scan via usbdevfs ioctl                                  */
/* ------------------------------------------------------------------ */

struct scan_result {
	int fd;			/* open fd for 1738:5211, or -1 */
	char path[1024];	/* path of 1738:5211 */
	int already_loaded;	/* 3275:6051 seen */
	char loaded_path[1024];
	int perm_denied;
};

/* GET_DESCRIPTOR (device), 18 bytes; vid/pid at offsets 8/10. */
static int get_vid_pid(int fd, uint16_t *vid, uint16_t *pid)
{
	uint8_t desc[18];
	struct usbdevfs_ctrltransfer ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.bRequestType = 0x80;	/* USB_TYPE_STANDARD | USB_DIR_IN */
	ctrl.bRequest = 6;		/* USB_REQ_GET_DESCRIPTOR */
	ctrl.wValue = 0x0100;		/* USB_DT_DEVICE << 8 */
	ctrl.wIndex = 0;
	ctrl.wLength = sizeof(desc);
	ctrl.timeout = 1000;
	ctrl.data = desc;

	if (ioctl(fd, USBDEVFS_CONTROL, &ctrl) < 0)
		return -1;
	*vid = (uint16_t)(desc[8] | ((uint16_t)desc[9] << 8));
	*pid = (uint16_t)(desc[10] | ((uint16_t)desc[11] << 8));
	return 0;
}

static void try_usb_node(const char *path, struct scan_result *r,
			 int keep_fd)
{
	int fd;
	uint16_t vid, pid;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		if (errno == EACCES || errno == EPERM)
			r->perm_denied = 1;
		return;
	}
	if (get_vid_pid(fd, &vid, &pid) != 0) {
		close(fd);
		return;
	}
	if (vid == VID_POST_FW && pid == PID_POST_FW) {
		r->already_loaded = 1;
		snprintf(r->loaded_path, sizeof(r->loaded_path), "%s", path);
		close(fd);
		return;
	}
	if (vid == VID_PRE_FW && pid == PID_PRE_FW) {
		if (r->fd < 0) {
			snprintf(r->path, sizeof(r->path), "%s", path);
			if (keep_fd)
				r->fd = fd;
			else
				close(fd);
		} else {
			close(fd);
		}
	}
}

static void scan_dir(const char *base, struct scan_result *r, int keep_fd)
{
	DIR *d;
	struct dirent *ent;

	d = opendir(base);
	if (!d)
		return;
	while ((ent = readdir(d)) != NULL) {
		char buspath[512];
		DIR *bd;
		struct dirent *bent;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(buspath, sizeof(buspath), "%s/%s", base,
			 ent->d_name);
		bd = opendir(buspath);
		if (!bd)
			continue;
		while ((bent = readdir(bd)) != NULL) {
			char path[1024];
			if (bent->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%s", buspath,
				 bent->d_name);
			try_usb_node(path, r, keep_fd);
		}
		closedir(bd);
	}
	closedir(d);
}

/*
 * Scan /dev/bus/usb (all bus/device nodes), falling back to
 * /proc/bus/usb if that tree is absent or yields nothing.
 * With keep_fd, the fd for 1738:5211 is left open in r->fd.
 */
static void scan_usb(struct scan_result *r, int keep_fd)
{
	memset(r, 0, sizeof(*r));
	r->fd = -1;
	scan_dir("/dev/bus/usb", r, keep_fd);
	if (r->fd < 0 && !r->already_loaded)
		scan_dir("/proc/bus/usb", r, keep_fd);
}

/* ------------------------------------------------------------------ */
/* Firmware upload                                                     */
/* ------------------------------------------------------------------ */

static int ctrl_out(int fd, uint8_t req, uint16_t val, uint16_t idx,
		    uint8_t *data, uint16_t len)
{
	struct usbdevfs_ctrltransfer ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.bRequestType = 0x40;	/* USB_TYPE_VENDOR | USB_DIR_OUT */
	ctrl.bRequest = req;
	ctrl.wValue = val;
	ctrl.wIndex = idx;
	ctrl.wLength = len;
	ctrl.timeout = CTRL_TIMEOUT;
	ctrl.data = data;
	return ioctl(fd, USBDEVFS_CONTROL, &ctrl);
}

static int send_block(int fd, uint8_t req, uint16_t idx, uint8_t *firm,
		      uint16_t off, uint16_t len)
{
	int ret;

	printf("  sending: req=0x%02x off=0x%04x len=0x%04x ... ", req, off,
	       len);
	fflush(stdout);
	ret = ctrl_out(fd, req, off, idx, firm + off, len);
	if (ret != len) {
		printf("FAILED\n");
		fprintf(stderr,
			"hdus_as11loader: block req=0x%02x off=0x%04x "
			"len=0x%04x returned %d: %s\n",
			req, off, len, ret,
			ret < 0 ? strerror(errno) : "short transfer");
		return -1;
	}
	printf("ok\n");
	return 0;
}

static void print_perm_hint(void)
{
	fprintf(stderr,
		"hdus_as11loader: permission denied opening USB device "
		"nodes.\n"
		"  Run with sudo, or add your user to the plugdev group "
		"and install a udev rule for 1738:5211, e.g.:\n"
		"    SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1738\", "
		"ATTR{idProduct}==\"5211\", MODE=\"0660\", GROUP=\"plugdev\"\n");
}

static void print_usage(const char *argv0)
{
	printf("usage: %s [--dry-run] [firmware]\n"
	       "  --dry-run   parse firmware, print idx/size/blocks, "
	       "do not touch USB\n"
	       "  firmware  SKNET_AS11Loader.sys, raw as11fw.bin "
	       "(2B header + 0x4000 body),\n"
	       "            or a raw 0x4000-byte binary (idx=0x4d66)\n"
	       "  default   ./SKNET_AS11Loader.sys, "
	       "./firmware/SKNET_AS11Loader.sys,\n"
	       "            ~/projects/hdus-linux/firmware/"
	       "SKNET_AS11Loader.sys,\n"
	       "            ./as11fw.bin, ./firmware/as11fw.bin\n",
	       argv0);
}

int main(int argc, char *argv[])
{
	const char *fw_arg = NULL;
	int dry_run = 0;
	int i;
	struct scan_result scan;
	struct firmware fw;
	size_t b;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dry-run") == 0)
			dry_run = 1;
		else if (strcmp(argv[i], "-h") == 0 ||
			 strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-' && argv[i][1] != '\0') {
			fprintf(stderr,
				"hdus_as11loader: unknown option %s\n",
				argv[i]);
			print_usage(argv[0]);
			return 2;
		} else if (!fw_arg) {
			fw_arg = argv[i];
		} else {
			fprintf(stderr,
				"hdus_as11loader: too many arguments\n");
			print_usage(argv[0]);
			return 2;
		}
	}

	if (dry_run) {
		if (resolve_firmware(fw_arg, &fw) != 0)
			return 1;
		print_firmware_info(&fw, 1);
		free(fw.data);
		return 0;
	}

	/* Find the pre-firmware device; exit early if already loaded. */
	scan_usb(&scan, 1);
	if (scan.already_loaded) {
		printf("device already loaded: %04x:%04x at %s "
		       "(firmware already uploaded)\n",
		       VID_POST_FW, PID_POST_FW, scan.loaded_path);
		if (scan.fd >= 0)
			close(scan.fd);
		return 0;
	}
	if (scan.fd < 0) {
		if (scan.perm_denied)
			print_perm_hint();
		fprintf(stderr,
			"hdus_as11loader: device %04x:%04x not found "
			"(scanned /dev/bus/usb/*/* and /proc/bus/usb/*/*; "
			"check: lsusb | grep 1738)\n",
			VID_PRE_FW, PID_PRE_FW);
		return 1;
	}
	printf("found device %04x:%04x at %s\n",
	       VID_PRE_FW, PID_PRE_FW, scan.path);

	/* Load firmware before sending anything. */
	if (resolve_firmware(fw_arg, &fw) != 0) {
		close(scan.fd);
		return 1;
	}
	print_firmware_info(&fw, 0);

	/* Upload the 4 blocks, verifying each transfer length. */
	printf("uploading firmware (idx=0x%04x) to %s:\n", fw.idx,
	       scan.path);
	for (b = 0; b < N_BLOCKS; b++) {
		if (send_block(scan.fd, fw_blocks[b].req, fw.idx, fw.data,
			       fw_blocks[b].off, fw_blocks[b].len) != 0) {
			free(fw.data);
			close(scan.fd);
			return 1;
		}
	}
	free(fw.data);
	close(scan.fd);

	printf("firmware upload complete\n");
	printf("device will re-enumerate as %04x:%04x (MonsterTV HDUS); "
	       "polling up to 5s...\n", VID_POST_FW, PID_POST_FW);

	/* Poll up to 5s for the re-enumerated device. */
	{
		int t;
		struct scan_result again;

		for (t = 0; t < 10; t++) {
			usleep(500000);
			scan_usb(&again, 0);
			if (again.already_loaded) {
				printf("device re-enumerated as %04x:%04x "
				       "at %s - firmware loaded\n",
				       VID_POST_FW, PID_POST_FW,
				       again.loaded_path);
				return 0;
			}
		}
		printf("device %04x:%04x not seen within 5s; check "
		       "lsusb | grep 3275\n", VID_POST_FW, PID_POST_FW);
	}
	return 0;
}
