/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * asie_table_probe - verify that the tuner register table really belongs to
 * I2C slave 0x20 (the demodulator), not to 0x4a (the ASIE5606 card
 * interface), and that the card therefore survives tuner initialisation.
 *
 * Background: Hdus.cpp writes each table entry as
 *   send_ctl(req 0x03, wValue = (reg << 8) | 0x20, wIndex = value, 2 bytes)
 * i.e. slave 0x20 carries the register number and the *value* travels in
 * wIndex. The first HDUS driver transcription flattened that into
 * wValue = (reg << 8) | value with wIndex = 0 and slave 0x4a, which writes
 * garbage to the card chip and kills its register file.
 *
 * Usage: asie_table_probe <usb-dev-node>
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

#define SLAVE_TUNER 0x20
#define SLAVE_CARD  0x4a

static int fd_;

static int ctl(uint8_t req, uint16_t val, uint16_t idx, uint16_t len, uint8_t *d)
{
	struct usbdevfs_ctrltransfer c;
	memset(&c, 0, sizeof(c));
	c.bRequestType = 0xc0; c.bRequest = req;
	c.wValue = val; c.wIndex = idx; c.wLength = len;
	c.timeout = 2000; c.data = d;
	return ioctl(fd_, USBDEVFS_CONTROL, &c);
}

static int R(uint8_t slave, uint8_t reg)
{
	uint8_t b[2] = {0, 0};
	ctl(0x02, (uint16_t)((reg << 8) | slave), 0, 2, b);
	return b[0] == 1 ? b[1] : -1;
}

static void W(uint8_t slave, uint8_t reg, uint16_t v)
{
	uint8_t b[2] = {0, 0};
	ctl(0x03, (uint16_t)((reg << 8) | slave), v, 2, b);
}

static uint8_t rev8(uint8_t b)
{
	uint8_t r = 0; int i;
	for (i = 0; i < 8; i++)
		r = (uint8_t)((r << 1) | ((b >> i) & 1));
	return r;
}

static float sig(void)
{
	uint8_t b[2] = {0, 0}; unsigned long v = 0;
	ctl(0x02, 0x8b20 | SLAVE_TUNER << 0, 0, 2, b);
	uint16_t wv = (uint16_t)((0x8b << 8) | SLAVE_TUNER);
	ctl(0x02, wv, 0, 2, b); v = (unsigned long)b[1] << 16;
	wv = (uint16_t)((0x8c << 8) | SLAVE_TUNER);
	ctl(0x02, wv, 0, 2, b); v |= (unsigned long)b[1] << 8;
	wv = (uint16_t)((0x8d << 8) | SLAVE_TUNER);
	ctl(0x02, wv, 0, 2, b); v |= b[1];
	return v ? 20.0 * log10(5505024.0 / (double)v) : -1;
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

/* Table exactly as Hdus.cpp writes it: reg at slave 0x20, value in wIndex. */
static const struct { uint8_t reg; uint16_t val; } tv[] = {
	{ 0x01, 0x0000 }, 	{ 0x02, 0x0000 }, 	{ 0x03, 0x0000 }, 	{ 0x04, 0x0000 },
	{ 0x05, 0x0000 }, 	{ 0x06, 0x0000 }, 	{ 0x07, 0x0000 }, 	{ 0x08, 0x0000 },
	{ 0x0c, 0x0000 }, 	{ 0x11, 0x0021 }, 	{ 0x12, 0x000b }, 	{ 0x13, 0x002c },
	{ 0x14, 0x000a }, 	{ 0x15, 0x0040 }, 	{ 0x16, 0x000a }, 	{ 0x17, 0x0070 },
	{ 0x18, 0x0031 }, 	{ 0x19, 0x0013 }, 	{ 0x1a, 0x0031 }, 	{ 0x1b, 0x0013 },
	{ 0x1c, 0x002a }, 	{ 0x1d, 0x00aa }, 	{ 0x1e, 0x00a2 }, 	{ 0x1f, 0x0008 },
	{ 0x20, 0x0000 }, 	{ 0x21, 0x00ff }, 	{ 0x22, 0x0080 }, 	{ 0x23, 0x004c },
	{ 0x24, 0x004c }, 	{ 0x25, 0x0000 }, 	{ 0x26, 0x0000 }, 	{ 0x27, 0x000c },
	{ 0x28, 0x0060 }, 	{ 0x29, 0x006b }, 	{ 0x2a, 0x0040 }, 	{ 0x2b, 0x0040 },
	{ 0x2c, 0x00ff }, 	{ 0x2d, 0x0000 }, 	{ 0x2e, 0x00ff }, 	{ 0x2f, 0x0000 },
	{ 0x30, 0x0028 }, 	{ 0x31, 0x000f }, 	{ 0x32, 0x00a0 }, 	{ 0x34, 0x003f },
	{ 0x38, 0x0001 }, 	{ 0x39, 0x0070 }, 	{ 0x3a, 0x002d }, 	{ 0x3b, 0x0021 },
	{ 0x3c, 0x003f }, 	{ 0x3d, 0x0010 }, 	{ 0x3e, 0x0008 }, 	{ 0x3f, 0x000c },
	{ 0x40, 0x000c }, 	{ 0x41, 0x0000 }, 	{ 0x42, 0x0000 }, 	{ 0x43, 0x004f },
	{ 0x44, 0x00ff }, 	{ 0x46, 0x0020 }, 	{ 0x47, 0x0000 }, 	{ 0x48, 0x0090 },
	{ 0x49, 0x00e6 }, 	{ 0x4a, 0x0002 }, 	{ 0x4b, 0x0054 }, 	{ 0x4c, 0x0000 },
	{ 0x50, 0x0000 }, 	{ 0x51, 0x0058 }, 	{ 0x52, 0x0020 }, 	{ 0x54, 0x0057 },
	{ 0x55, 0x00f1 }, 	{ 0x56, 0x0020 }, 	{ 0x57, 0x0070 }, 	{ 0x58, 0x0060 },
	{ 0x5c, 0x0050 }, 	{ 0x5d, 0x0000 }, 	{ 0x5f, 0x0080 }, 	{ 0x70, 0x0018 },
	{ 0x71, 0x0000 }, 	{ 0x72, 0x0000 }, 	{ 0x75, 0x0022 }, 	{ 0x76, 0x0000 },
	{ 0x77, 0x0001 }, 	{ 0x7c, 0x0000 }, 	{ 0x7d, 0x0052 }, 	{ 0xba, 0x0000 },
	{ 0xbb, 0x0000 }, 	{ 0xbc, 0x0000 }, 	{ 0xc2, 0x0000 }, 	{ 0xc7, 0x0000 },
	{ 0xe4, 0x001a }, 	{ 0xe9, 0x0008 }, 	{ 0xec, 0x0000 }, 	{ 0xef, 0x0000 },
};

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/bus/usb/002/004";
	uint8_t d[8] = {0};
	uint8_t ap[5] = {0x90, 0x30, 0, 0, 0};
	uint8_t frame[16], resp[128];
	uint8_t lrc = 0, x = 0;
	unsigned int iface = 0;
	size_t i;
	int page, n, j;

	fd_ = open(path, O_RDWR);
	if (fd_ < 0) { perror(path); return 1; }
	if (ioctl(fd_, USBDEVFS_CLAIMINTERFACE, &iface) < 0) { perror("claim"); return 1; }
	{ struct usbdevfs_setinterface si = {0, 0};
	  ioctl(fd_, USBDEVFS_SETINTERFACE, &si); }

	power();
	printf("card before table: R04=%02x\n", R(SLAVE_CARD, 0x04));

	/* Verify the demod answers at 0x20 and the card chip at 0x4a. */
	printf("reg09@0x20=%02x reg09@0x4a=%02x\n",
	       R(SLAVE_TUNER, 0x09), R(SLAVE_CARD, 0x09));

	ctl(0x02, (uint16_t)((0xa8 << 8) | SLAVE_TUNER), 0, 2, d);
	for (i = 0; i < sizeof(tv) / sizeof(tv[0]); i++)
		ctl(0x03, (uint16_t)((tv[i].reg << 8) | SLAVE_TUNER),
		    tv[i].val, 2, d);
	ctl(0x02, (uint16_t)((0xa8 << 8) | SLAVE_TUNER), 0, 2, d);
	printf("table written to slave 0x%02x; card R04=%02x\n",
	       SLAVE_TUNER, R(SLAVE_CARD, 0x04));

	/* Channel set, exactly as Hdus::setChannel (slave 0x20). */
	{
		int mhz = 473, val = (mhz + 57) * 7;
		ctl(0x0d, 0xfe00, (uint16_t)((val & 0xff00) | 0xc0), 4, d);
		ctl(0x0d, (uint16_t)(((val & 0xff) << 8) | 0x03), 0x8491, 4, d);
		ctl(0x0d, 0xe306, 0, 2, d);
		ctl(0x0e, 0x0020, 0, 8, d);
		ctl(0x03, 0x0120, 0x0040, 2, d);
		ctl(0x09, 0x0100, 0, 1, d);
		ctl(0x04, 0x0040, 0, 2, d);
		ctl(0x05, 0x0f40, 0, 2, d);
		ctl(0x06, 0, 0, 1, d);
		usleep(150000);
	}
	printf("tuned ch13: signal=%.1f dB, card R04=%02x\n",
	       sig(), R(SLAVE_CARD, 0x04));

	/* Card bring-up (coax Reset). */
	W(SLAVE_CARD, 0x00, 0x80); usleep(10000);
	W(SLAVE_CARD, 0x00, 0x00); W(SLAVE_CARD, 0x04, 0x0000);
	W(SLAVE_CARD, 0x00, 0x00); usleep(100000);
	printf("present=%02x\n", R(SLAVE_CARD, 0x04));
	W(SLAVE_CARD, 0x00, 0x08); W(SLAVE_CARD, 0x04, 0x0080); usleep(200000);
	W(SLAVE_CARD, 0x00, 0x09); usleep(50000);
	for (n = 0; n < 42; n++) {
		int lo, hi;
		usleep(20000);
		lo = R(SLAVE_CARD, 0x03); hi = R(SLAVE_CARD, 0x39);
		if (lo >= 0 && (lo | ((hi & 1) << 8)) >= 13) break;
	}
	{
		int len = R(SLAVE_CARD, 0x03) | ((R(SLAVE_CARD, 0x39) & 1) << 8);
		uint8_t atr[16];
		printf("ATR len=%d:", len);
		page = -1;
		for (j = 0; j < len && j < 13; j++) {
			int raw = 0x40 + j, reg = raw < 0x80 ? raw : raw - 0x40;
			int pg = raw < 0x80 ? 0 : 1;
			if (pg != page) { W(SLAVE_CARD, 0x3a, pg); page = pg; }
			atr[j] = (uint8_t)R(SLAVE_CARD, (uint8_t)reg);
			printf(" %02x", rev8(atr[j]));
		}
		printf("\n");
	}
	W(SLAVE_CARD, 0x01, 0x0001);

	/* One APDU while the tuner stays tuned. */
	frame[0] = 0; frame[1] = 0; frame[2] = 5;
	memcpy(frame + 3, ap, 5);
	for (j = 0; j < 8; j++) { frame[j] = rev8(frame[j]); lrc ^= frame[j]; }
	frame[8] = lrc;
	W(SLAVE_CARD, 0x00, 0x0c); W(SLAVE_CARD, 0x02, 9); W(SLAVE_CARD, 0x38, 0);
	page = -1;
	for (j = 0; j < 9; j++) {
		int raw = 0x40 + j, reg = raw < 0x80 ? raw : raw - 0x40;
		int pg = raw < 0x80 ? 0 : 1;
		if (pg != page) { W(SLAVE_CARD, 0x3a, pg); page = pg; }
		W(SLAVE_CARD, (uint8_t)reg, frame[j]);
	}
	W(SLAVE_CARD, 0x00, 0x0a); usleep(50000);
	for (n = 0; n < 42; n++) {
		int lo, hi;
		usleep(20000);
		lo = R(SLAVE_CARD, 0x03); hi = R(SLAVE_CARD, 0x39);
		if (lo >= 0 && (lo | ((hi & 1) << 8)) >= 3) break;
	}
	n = R(SLAVE_CARD, 0x03) | ((R(SLAVE_CARD, 0x39) & 1) << 8);
	printf("APDU resp len=%d\n", n);
	if (n >= 3 && n <= (int)sizeof(resp)) {
		page = -1;
		for (j = 0; j < n; j++) {
			int raw = 0x40 + j, reg = raw < 0x80 ? raw : raw - 0x40;
			int pg = raw < 0x80 ? 0 : 1;
			if (pg != page) { W(SLAVE_CARD, 0x3a, pg); page = pg; }
			resp[j] = (uint8_t)R(SLAVE_CARD, (uint8_t)reg);
			x ^= resp[j];
		}
		printf("LRC ok=%d signal after=%.1f dB\n", x == 0, sig());
	}
	close(fd_);
	return 0;
}
