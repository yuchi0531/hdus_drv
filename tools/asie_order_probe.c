/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * asie_order_probe - test whether the HDUS card interface and the tuner
 * can be used together, i.e. whether the order of initialisation matters.
 *
 * Why: the tuner register table includes ASIE5606 reg 0x11 = 0x21, and
 * after it is written the card registers (same I2C address 0x4a) stop
 * answering until the chip is power cycled. If bringing the card up
 * *first* and then writing the tuner table keeps the card alive while the
 * tuner still tunes, true simultaneous use is possible and the mux
 * (power cycle per card session) is unnecessary.
 *
 * Usage: asie_order_probe <usb-dev-node> [1|2]
 *   1 = card first, then tuner table + tune, then one APDU
 *   2 = tuner first (the driver's current order), then card init
 *
 * Must not run while the hdus kernel module holds the interface.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

static int fd_;

static int ctl(uint8_t req, uint16_t val, uint16_t idx, uint16_t len,
	       uint8_t *d)
{
	struct usbdevfs_ctrltransfer c;
	memset(&c, 0, sizeof(c));
	c.bRequestType = 0xc0;
	c.bRequest = req;
	c.wValue = val;
	c.wIndex = idx;
	c.wLength = len;
	c.timeout = 2000;
	c.data = d;
	return ioctl(fd_, USBDEVFS_CONTROL, &c);
}

static int R(uint8_t reg)
{
	uint8_t b[2] = {0, 0};
	ctl(0x02, (reg << 8) | 0x4a, 0, 2, b);
	return b[0] == 1 ? b[1] : -1;
}

static void W(uint8_t reg, uint16_t v)
{
	uint8_t b[2] = {0, 0};
	ctl(0x03, (reg << 8) | 0x4a, v, 2, b);
}

static uint8_t rev8(uint8_t b)
{
	uint8_t r = 0;
	int i;

	for (i = 0; i < 8; i++)
		r = (uint8_t)((r << 1) | ((b >> i) & 1));
	return r;
}

static float sig(void)
{
	uint8_t b[2] = {0, 0};
	unsigned long v = 0;

	ctl(0x02, 0x8b20, 0, 2, b); v = (unsigned long)b[1] << 16;
	ctl(0x02, 0x8c20, 0, 2, b); v |= (unsigned long)b[1] << 8;
	ctl(0x02, 0x8d20, 0, 2, b); v |= b[1];
	if (!v)
		return -1;
	return 20.0 * log10(5505024.0 / (double)v);
}

static void power(void)
{
	uint8_t d[0x200] = {0};

	ctl(0x0c, 0, 0, 0x3a, d); ctl(0x17, 0x0002, 0, 3, d);
	ctl(0x08, 0xfb07, 0, 1, d); ctl(0x10, 0x0200, 0, 1, d);
	usleep(50000);
	ctl(0x08, 0x0100, 0, 1, d); usleep(100000);
	ctl(0x08, 0xdd0c, 0, 1, d); usleep(100000);
	ctl(0x17, 0x0002, 0, 3, d);
	ctl(0x08, 0xd500, 0, 1, d); usleep(50000);
	ctl(0x08, 0x0800, 0, 1, d); ctl(0x08, 0x4040, 0, 1, d);
	usleep(5000);
	ctl(0x08, 0x8080, 0, 1, d); usleep(50000);
	ctl(0x08, 0x0404, 0, 1, d); usleep(50000);
	ctl(0x08, 0x11ff, 0, 1, d); usleep(50000);
	ctl(0x08, 0x1100, 0, 1, d); usleep(50000);
	ctl(0x08, 0x11ff, 0, 1, d); usleep(100000);
}

static const uint16_t tv[] = {
	0x00a8, 0x0120, 0x0220, 0x0320, 0x0420, 0x0520, 0x0620, 0x0720,
	0x0820, 0x0c20, 0x1121, 0x120b, 0x132c, 0x140a, 0x1540, 0x160a,
	0x1770, 0x1831, 0x1913, 0x1a31, 0x1b13, 0x1c2a, 0x1daa, 0x1ea2,
	0x1f08, 0x2000, 0x21ff, 0x2280, 0x234c, 0x244c, 0x2500, 0x2600,
	0x270c, 0x2860, 0x296b, 0x2a40, 0x2b40, 0x2cff, 0x2d00, 0x2eff,
	0x2f00, 0x3028, 0x310f, 0x32a0, 0x343f, 0x3801, 0x3970, 0x3a2d,
	0x3b21, 0x3c3f, 0x3d10, 0x3e08, 0x3f0c, 0x400c, 0x4100, 0x4200,
	0x434f, 0x44ff, 0x4620, 0x4700, 0x4890, 0x49e6, 0x4a02, 0x4b54,
	0x4c00, 0x5000, 0x5158, 0x5220, 0x5457, 0x55f1, 0x5620, 0x5770,
	0x5860, 0x5c50, 0x5d00, 0x5f80, 0x7018, 0x7100, 0x7200, 0x7522,
	0x7600, 0x7701, 0x7c00, 0x7d52, 0xba00, 0xbb00, 0xbc00, 0xc200,
	0xc700, 0xe41a, 0xe908, 0xec00, 0xef00,
};

static void tuner_table(void)
{
	uint8_t d[8] = {0};
	size_t i;

	ctl(0x02, 0x00a8, 0, 2, d);
	for (i = 1; i < sizeof(tv) / sizeof(tv[0]); i++)
		ctl(0x03, tv[i], 0, 2, d);
	ctl(0x02, 0x00a8, 0, 2, d);
}

static void tune(int mhz)
{
	uint8_t d[8] = {0};
	int val = (mhz + 57) * 7;

	ctl(0x0d, 0xfe00, (val & 0xff00) | 0xc0, 4, d);
	ctl(0x0d, ((val & 0xff) << 8) | 0x03, 0x8491, 4, d);
	ctl(0x0d, 0xe306, 0, 2, d);
	ctl(0x0e, 0x0020, 0, 8, d);
	ctl(0x03, 0x0120, 0x0040, 2, d);
	ctl(0x09, 0x0100, 0, 1, d);
	ctl(0x04, 0x0040, 0, 2, d);
	ctl(0x05, 0x0f40, 0, 2, d);
	ctl(0x06, 0, 0, 1, d);
	usleep(150000);
}

static int wait_len(int min, int polls)
{
	int p;

	for (p = 0; p < polls; p++) {
		int lo, hi, n;

		usleep(20000);
		lo = R(0x03);
		hi = R(0x39);
		if (lo < 0 || hi < 0)
			return -1;
		n = lo | ((hi & 1) << 8);
		if (n >= min)
			return n;
	}
	return 0;
}

static int card_init(void)
{
	W(0x00, 0x80); usleep(10000);
	W(0x00, 0x00); W(0x04, 0x0000); W(0x00, 0x00); usleep(100000);
	if (!(R(0x04) & 0x01))
		return -3;
	W(0x00, 0x08); W(0x04, 0x0080); usleep(200000);
	W(0x00, 0x09); usleep(50000);
	(void)wait_len(13, 42);
	W(0x01, 0x0001);
	return 0;
}

static int apdu(uint8_t *resp, int max)
{
	uint8_t ap[5] = {0x90, 0x30, 0, 0, 0};
	uint8_t frame[16];
	uint8_t lrc = 0;
	uint8_t x = 0;
	int page = -1, n, i;

	frame[0] = 0; frame[1] = 0; frame[2] = 5;
	memcpy(frame + 3, ap, 5);
	for (i = 0; i < 8; i++) {
		frame[i] = rev8(frame[i]);
		lrc ^= frame[i];
	}
	frame[8] = lrc;

	W(0x00, 0x0c); W(0x02, 9); W(0x38, 0);
	for (i = 0; i < 9; i++) {
		int raw = 0x40 + i;
		int reg = raw < 0x80 ? raw : raw - 0x40;
		int pg = raw < 0x80 ? 0 : 1;

		if (pg != page) { W(0x3a, pg); page = pg; }
		W((uint8_t)reg, frame[i]);
	}
	W(0x00, 0x0a); usleep(50000);
	n = wait_len(3, 42);
	if (n < 3)
		return -6;
	if (n > max)
		n = max;
	page = -1;
	for (i = 0; i < n; i++) {
		int raw = 0x40 + i;
		int reg = raw < 0x80 ? raw : raw - 0x40;
		int pg = raw < 0x80 ? 0 : 1;

		if (pg != page) { W(0x3a, pg); page = pg; }
		resp[i] = (uint8_t)R((uint8_t)reg);
		x ^= resp[i];
	}
	W(0x00, 0x0c);
	if (x)
		return -7;
	for (i = 0; i < n - 1; i++)
		resp[i] = rev8(resp[i]);
	return n;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/bus/usb/002/002";
	int mode = argc > 2 ? atoi(argv[2]) : 1;
	unsigned int iface = 0;
	uint8_t resp[128];
	int n;

	fd_ = open(path, O_RDWR);
	if (fd_ < 0) {
		perror(path);
		return 1;
	}
	if (ioctl(fd_, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
		perror("claim interface 0");
		return 1;
	}
	{
		struct usbdevfs_setinterface si = {0, 0};

		ioctl(fd_, USBDEVFS_SETINTERFACE, &si);
	}
	power();
	printf("mode=%d\n", mode);

	if (mode == 1) {
		printf("1) card init first\n");
		printf("   card_init rc=%d R04=%02x\n", card_init(), R(0x04));
		printf("2) tuner table + tune\n");
		tuner_table();
		tune(473);
		printf("   signal=%.1f dB R04=%02x\n", sig(), R(0x04));
		n = apdu(resp, (int)sizeof(resp));
		printf("3) APDU while tuned: %d %s\n", n, n > 0 ? "OK" : "FAIL");
		if (n > 0) {
			printf("   payload:");
			for (int i = 3; i < n - 1 && i < 24; i++)
				printf(" %02x", resp[i]);
			printf("\n");
		}
	} else {
		printf("tuner first (the driver's current order)\n");
		tuner_table();
		tune(473);
		printf("   signal=%.1f dB R04=%02x\n", sig(), R(0x04));
		printf("   card_init rc=%d\n", card_init());
		n = apdu(resp, (int)sizeof(resp));
		printf("   APDU: %d\n", n);
	}
	close(fd_);
	return 0;
}
