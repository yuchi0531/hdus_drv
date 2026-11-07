/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hdus_card_probe - probe HDUS internal B-CAS card slot via usbfs.
 *
 * Runs the recfriio Hdus UsbProcBegin init sequence (power + ASIE5606
 * tuner init + saddr detect), then dumps ASIE registers and tries
 * card-presence reads. Requires write access to the USB device node
 * (run with sudo, or install 99-hdusvideo.rules udev rule).
 *
 * Usage: hdus_card_probe [/dev/bus/usb/002/008]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

static int fd_;

static int send_ctl(uint8_t req, uint16_t val, uint16_t idx, uint16_t len,
		    uint8_t *data)
{
	struct usbdevfs_ctrltransfer c;
	memset(&c, 0, sizeof(c));
	c.bRequestType = 0xc0; /* USB_DIR_IN|USB_TYPE_VENDOR|USB_RECIP_DEVICE */
	c.bRequest = req;
	c.wValue = val;
	c.wIndex = idx;
	c.wLength = len;
	c.timeout = 2000;
	c.data = data;
	int r = ioctl(fd_, USBDEVFS_CONTROL, &c);
	if (r < 0) {
		fprintf(stderr, "ctl req=0x%02x val=0x%04x idx=0x%04x: %s\n",
			req, val, idx, strerror(errno));
		return -1;
	}
	return r;
}

static int ctl_read(uint8_t req, uint16_t val, uint16_t idx, uint16_t len,
		    uint8_t *out)
{
	uint8_t buf[0x200];
	int r = send_ctl(req, val, idx, len, buf);
	if (r < 0)
		return r;
	memcpy(out, buf, len);
	return 0;
}

static uint8_t saddr = 0x5a;
static uint8_t r5;

static int reg_read(uint8_t reg)
{
	uint8_t b[2] = {0, 0};
	if (ctl_read(0x02, (reg << 8) | saddr, 0, 2, b) < 0)
		return -1;
	if (b[0] != 1)
		return -2;
	return b[1];
}

static void msleep_(int ms)
{
	usleep(ms * 1000);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/bus/usb/002/008";
	uint8_t data[0x200];
	unsigned int iface = 0;
	int r;

	fd_ = open(path, O_RDWR);
	if (fd_ < 0) {
		perror(path);
		fprintf(stderr, "hint: run with sudo (device node is root-only)\n");
		return 1;
	}
	r = ioctl(fd_, USBDEVFS_CLAIMINTERFACE, &iface);
	if (r < 0) {
		perror("claim interface 0 (kernel driver loaded?)");
		return 1;
	}

	/* ---- Hdus.cpp UsbProcBegin (power + tuner init) ---- */
	send_ctl(0x0c, 0x0000, 0x0000, 0x003a, data);
	send_ctl(0x17, 0x0002, 0x0000, 0x0003, data);
	send_ctl(0x08, 0xfb07, 0x0000, 0x0001, data);
	send_ctl(0x10, 0x0200, 0x0000, 0x0001, data);
	msleep_(50);
	send_ctl(0x08, 0x0100, 0x0000, 0x0001, data);
	msleep_(100);
	send_ctl(0x08, 0xdd0c, 0x0000, 0x0001, data);
	msleep_(100);
	send_ctl(0x17, 0x0002, 0x0000, 0x0003, data);
	send_ctl(0x08, 0xd500, 0x0000, 0x0001, data);
	msleep_(50);
	send_ctl(0x08, 0x0800, 0x0000, 0x0001, data);
	send_ctl(0x08, 0x4040, 0x0000, 0x0001, data);
	msleep_(5);
	send_ctl(0x08, 0x8080, 0x0000, 0x0001, data);
	msleep_(50);
	send_ctl(0x08, 0x0404, 0x0000, 0x0001, data);
	msleep_(50);

	/* tuner register table: send_ctl(0x03, wValue, 0x0000, 2) from Hdus.cpp */
	static const uint16_t tuner_vals[] = {
		0x00a8, 0x0120, 0x0220, 0x0320, 0x0420, 0x0520, 0x0620,
		0x0720, 0x0820, 0x0c20, 0x1121, 0x120b, 0x132c, 0x140a,
		0x1540, 0x160a, 0x1770, 0x1831, 0x1913, 0x1a31, 0x1b13,
		0x1c2a, 0x1daa, 0x1ea2, 0x1f08, 0x2000, 0x21ff, 0x2280,
		0x234c, 0x244c, 0x2500, 0x2600, 0x270c, 0x2860, 0x296b,
		0x2a40, 0x2b40, 0x2cff, 0x2d00, 0x2eff, 0x2f00, 0x3028,
		0x310f, 0x32a0, 0x343f, 0x3801, 0x3970, 0x3a2d, 0x3b21,
		0x3c3f, 0x3d10, 0x3e08, 0x3f0c, 0x400c, 0x4100, 0x4200,
		0x434f, 0x44ff, 0x4620, 0x4700, 0x4890, 0x49e6, 0x4a02,
		0x4b54, 0x4c00, 0x5000, 0x5158, 0x5220, 0x5457, 0x55f1,
		0x5620, 0x5770, 0x5860, 0x5c50, 0x5d00, 0x5f80, 0x7018,
		0x7100, 0x7200, 0x7522, 0x7600, 0x7701, 0x7c00, 0x7d52,
		0xba00, 0xbb00, 0xbc00, 0xc200, 0xc700, 0xe41a, 0xe908,
		0xec00, 0xef00,
	};
	/* regs are the high byte of each wValue (0x00,0x01,...) except the
	 * first 0x02/0x00a8 entry which uses req 0x02 in Hdus.cpp. */
	send_ctl(0x02, 0x00a8, 0x0000, 0x0002, data);
	for (size_t i = 1; i < sizeof(tuner_vals) / sizeof(tuner_vals[0]); i++)
		send_ctl(0x03, tuner_vals[i], 0x0000, 0x0002, data);
	send_ctl(0x02, 0x00a8, 0x0000, 0x0002, data);

	/* ASIE5606 on (card power bit 0x10 included) */
	send_ctl(0x08, 0x11ff, 0x0000, 0x0001, data);
	msleep_(50);
	send_ctl(0x08, 0x1100, 0x0000, 0x0001, data);
	msleep_(50);
	send_ctl(0x08, 0x11ff, 0x0000, 0x0001, data);
	msleep_(100);

	/* saddr detect */
	uint8_t b[2] = {0, 0};
	ctl_read(0x02, 0x0900 | saddr, 0, 2, b);
	if (b[0] != 1) {
		saddr = 0x4a;
		ctl_read(0x02, 0x0900 | saddr, 0, 2, b);
	}
	printf("saddr=0x%02x status=%d val=0x%02x\n", saddr, b[0], b[1]);
	r5 = ((b[1] & 7) == 6) ? 0x80 : 0x91;
	printf("crypto mode r5=0x%02x (%s)\n", r5, r5 == 0x91 ? "XOR" : "DES");

	/* ---- register dump + card presence probes ---- */
	printf("reg dump (saddr=0x%02x):\n", saddr);
	for (int reg = 0; reg < 0x50; reg++) {
		int v = reg_read(reg);
		if (v >= 0)
			printf("  [0x%02x] = 0x%02x\n", reg, v);
	}
	printf("card-related regs (BonDriver ConnectCard/SendCard candidates):\n");
	static const uint8_t probes[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x09, 0x38, 0x39, 0x3a,
		0x3b, 0x3c, 0x40, 0x41, 0x42, 0x43, 0x44, 0x80, 0x81,
	};
	for (size_t i = 0; i < sizeof(probes); i++) {
		int v = reg_read(probes[i]);
		printf("  probe [0x%02x] -> %s\n", probes[i],
		       v >= 0 ? "ok" : (v == -2 ? "status!=1" : "ioerr"));
		if (v >= 0)
			printf("    value = 0x%02x\n", v);
	}

	close(fd_);
	return 0;
}
