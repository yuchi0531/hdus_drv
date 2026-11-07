/* SPDX-License-Identifier: GPL-2.0-or-later */
/* hdus_card.c — HDUS internal B-CAS card slot.
 *
 * Transport selection (single-owner architecture):
 *   Primary: kernel-backed via /dev/hdusvideoX ioctl (hdus.ko owns USB).
 *     The kernel muxes tuner <-> card per transaction, so tuner and card
 *     are usable simultaneously without userspace claiming interface 0.
 *   Fallback: usbfs (USBDEVFS_CONTROL, bRequestType 0xC0, 2-byte buffer
 *     buf[0]=status (1 == ok, as in tools/hdus_card_probe.c), buf[1]=data).
 *     Used when the video node is absent (ENOENT, kernel module not
 *     loaded). This is card-only exclusive mode: it claims interface 0
 *     and must not run concurrently with the tuner/kernel driver.
 *   Register access (docs/card_protocol_sendcard.md section 1):
 *     R(reg): req 0x02, wValue (reg<<8)|slave, wIndex 0,     result buf[1]
 *     W(reg,val): req 0x03, wValue (reg<<8)|slave, wIndex val
 *   Protocol: SendCard 0x10005bc0 / ConnectCard 0x10005700 in
 *   BonDriver_HDUS.dll, see ../../docs/card_protocol_sendcard.md.
 *   S-box: ../card_sbox.h (bit-reverse table, hdus_card_sbox[i]).
 */
#include "hdus_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/types.h>

#include "../../hdus_ioctl.h"

#include "../card_sbox.h"

/* ---- Kernel card ioctl fallback definitions ----
 * hdus_ioctl.h gains these from the kernel-side agent; until that lands
 * (or a different revision), define identical values locally.
 * Values per task spec against /dev/hdusvideoX:
 *   HDUS_CARD_CONNECT  _IO(0x8d, 0x10)
 *   HDUS_CARD_TRANSMIT _IOWR(0x8d, 0x11, struct hdus_card_req)
 *   HDUS_CARD_PRESENT  _IOR(0x8d, 0x12, int *)
 * If the shared header already defines them, the #ifndef guards win. */
#ifndef HDUS_CARD_CONNECT
/* Header does not provide the card ioctls yet, so also provide the
 * struct with the same name/layout as the kernel-side addition. The
 * layout must stay identical once hdus_ioctl.h gains it. */
struct hdus_card_req {
	__u32 req_len, extra_len, resp_len;
	__u8 req[264];
	__u8 extra[264];
	__u8 resp[600];
};
#define HDUS_CARD_CONNECT _IO(0x8d, 0x10)
#define HDUS_CARD_TRANSMIT _IOWR(0x8d, 0x11, struct hdus_card_req)
#define HDUS_CARD_PRESENT _IOR(0x8d, 0x12, int *)
struct hdus_card_atr {
	__u32 len;
	__u8 atr[33];
};
#define HDUS_CARD_GET_ATR _IOR(0x8d, 0x13, struct hdus_card_atr)
#endif

#define HDUS_CARD_REQ_LEN_MAX 264
#define HDUS_CARD_EXTRA_LEN_MAX 264
#define HDUS_CARD_RESP_LEN_MAX 600

/* ioctl arg type actually used below: always struct hdus_card_req,
 * whether it came from the shared header or the fallback above. */
typedef struct hdus_card_req hdus_card_req_t;

#define HDUS_LOG(...) do { fprintf(stderr, "hdus_card: " __VA_ARGS__); \
				fflush(stderr); } while (0)

static int g_fd = -1;	/* usbfs fd (fallback transport only) */
static int g_vfd = -1;	/* /dev/hdusvideoX fd (primary kernel transport) */
static int g_transport = HDUS_CARD_TRANSPORT_NONE;
static uint8_t g_slave = 0x5a;	/* obj+0x4e6, set by saddr detect */
static uint8_t g_state = 0;	/* obj+0x534[8], toggled ^0x40 per frame */
static uint8_t g_atr[HDUS_CARD_ATR_MAX];	/* card ATR, card bit order */
static int g_atr_len = 0;
static int g_opened = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* obj+0x52c */

/* ---- USB layer (same shape as tools/hdus_card_probe.c) ---- */

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
	int r = ioctl(g_fd, USBDEVFS_CONTROL, &c);
	if (r < 0)
		HDUS_LOG("ctl req=0x%02x val=0x%04x idx=0x%04x: %s\n",
			 req, val, idx, strerror(errno));
	return r;
}

static int ctl_read2(uint8_t req, uint16_t val, uint16_t idx, uint8_t b[2])
{
	memset(b, 0, 2);
	return send_ctl(req, val, idx, 2, b) < 0 ? -1 : 0;
}

/* R(reg)/W(reg,val) per sendcard doc section 1. buf[0]!=1 is failure
 * (empirical: tools/hdus_card_probe.c reg_read()). */
static int card_r(uint8_t reg, uint8_t *out)
{
	uint8_t b[2];
	if (ctl_read2(0x02, (uint16_t)((reg << 8) | g_slave), 0, b) < 0)
		return -1;
	if (b[0] != 1)
		return -1;
	*out = b[1];
	return 0;
}

static int card_w(uint8_t reg, uint16_t val)
{
	uint8_t b[2];
	if (ctl_read2(0x03, (uint16_t)((reg << 8) | g_slave), val, b) < 0)
		return -1;
	return b[0] != 1 ? -1 : 0;
}

static void msleep_(int ms)
{
	usleep((useconds_t)ms * 1000);
}

/* ---- Power / tuner init: copied from tools/hdus_card_probe.c ----
 * Hdus.cpp UsbProcBegin power sequence + ASIE5606 tuner register table
 * + ASIE-on gpio sequence (0x11ff/0x1100/0x11ff; the 0x10 bit is the card
 * power bit) + saddr detect. The card slot lives behind the ASIE, so the
 * card init cannot run before this. */
static int hdus_card_power_init(void)
{
	uint8_t data[0x200];
	uint8_t b[2] = {0, 0};

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

	/* NOTE (verified on hardware 2026-11-07): the full tuner register
	 * table must NOT be written for card access - it overwrites the
	 * shared ASIE card-interface regs (0x38-0x44) and breaks card
	 * presence/session reads (all NACK). Power + ASIE-on only. */

	/* ASIE5606 on (card power bit 0x10 included) */
	send_ctl(0x08, 0x11ff, 0x0000, 0x0001, data);
	msleep_(50);
	send_ctl(0x08, 0x1100, 0x0000, 0x0001, data);
	msleep_(50);
	send_ctl(0x08, 0x11ff, 0x0000, 0x0001, data);
	msleep_(100);

	/* saddr detect: req 0x02, wValue 0x0900|saddr, 0x5a then 0x4a */
	g_slave = 0x5a;
	ctl_read2(0x02, (uint16_t)(0x0900 | g_slave), 0, b);
	if (b[0] != 1) {
		g_slave = 0x4a;
		ctl_read2(0x02, (uint16_t)(0x0900 | g_slave), 0, b);
	}
	HDUS_LOG("saddr=0x%02x status=%d val=0x%02x\n", g_slave, b[0], b[1]);
	return b[0] == 1 ? 0 : -1;
}

int hdus_card_transport(void)
{
	return g_transport;
}

/* Resolve a video-node path: explicit video node via argument, else
 * $HDUS_VIDEO_PATH, else the default video node. A NULL/invalid argument
 * yields the env/default video path (never NULL). */
static const char *resolve_video_path(const char *path)
{
	const char *v;

	if (path && path[0] && strstr(path, "hdusvideo"))
		return path;
	v = getenv(HDUS_CARD_ENV_VIDEO_PATH);
	if (v && v[0])
		return v;
	return HDUS_CARD_DEFAULT_VIDEO_PATH;
}

int hdus_card_open(const char *usb_path)
{
	const char *vpath;
	const char *path;
	unsigned int iface = 0;
	int r;

	if (g_opened)
		return 0;

	/* Primary: kernel video node (single-owner USB). */
	vpath = resolve_video_path(usb_path);
	g_vfd = open(vpath, O_RDWR);
	if (g_vfd >= 0) {
		g_transport = HDUS_CARD_TRANSPORT_KERNEL;
		g_opened = 1;
		HDUS_LOG("transport: kernel ioctl via %s\n", vpath);
		return 0;
	}
	if (errno != ENOENT) {
		HDUS_LOG("open %s: %s\n", vpath, strerror(errno));
		return HDUS_CARD_ERR_IO;
	}
	HDUS_LOG("video node %s absent (%s); falling back to usbfs\n",
		 vpath, strerror(errno));

	/* Fallback: usbfs. Card-only exclusive mode: claims interface 0,
	 * required until hdus.ko is loaded (when the video node exists,
	 * the kernel owns USB and this path is never taken). */
	path = usb_path;
	if (path && strstr(path, "hdusvideo"))
		path = NULL; /* explicit video node: usb path from env/default */
	if (!path || !path[0])
		path = getenv(HDUS_CARD_ENV_PATH);
	if (!path || !path[0])
		path = HDUS_CARD_DEFAULT_USB_PATH;

	g_fd = open(path, O_RDWR);
	if (g_fd < 0) {
		HDUS_LOG("open %s: %s\n", path, strerror(errno));
		return HDUS_CARD_ERR_IO;
	}
	r = ioctl(g_fd, USBDEVFS_CLAIMINTERFACE, &iface);
	if (r < 0) {
		HDUS_LOG("claim interface 0 on %s: %s (kernel driver loaded?)\n",
			 path, strerror(errno));
		close(g_fd);
		g_fd = -1;
		return HDUS_CARD_ERR_IO;
	}
	if (hdus_card_power_init() < 0)
		HDUS_LOG("power init / saddr detect reported failure "
			 "(continuing; card regs may still answer)\n");
	g_transport = HDUS_CARD_TRANSPORT_USBFS;
	g_opened = 1;
	HDUS_LOG("transport: usbfs via %s\n", path);
	return 0;
}

void hdus_card_close(void)
{
	if (!g_opened)
		return;
	if (g_transport == HDUS_CARD_TRANSPORT_KERNEL) {
		if (g_vfd >= 0) {
			close(g_vfd);
			g_vfd = -1;
		}
	} else if (g_transport == HDUS_CARD_TRANSPORT_USBFS && g_fd >= 0) {
		unsigned int iface = 0;
		ioctl(g_fd, USBDEVFS_RELEASEINTERFACE, &iface);
		close(g_fd);
		g_fd = -1;
	}
	g_transport = HDUS_CARD_TRANSPORT_NONE;
	g_opened = 0;
}

/* ---- Presence (sendcard doc section 5.1) ----
 * DLL logic (used when the masks are recovered, see hdus_card.h):
 *   mask = MASK_PRESENT | MASK_READY
 *   (d & mask) == mask          -> ready
 *   (d & MASK_READY) == MASK_READY -> ConnectCard, else -2
 *   (d & MASK_PRESENT) == MASK_PRESENT -> W(0x04,0), -3
 *   else -> -3
 * Fallback (masks == 0, current state): R(0x04) twice, equal and != 0xff
 * means a card is answering; the raw value is logged. */
static int card_presence_check(int *raw_out)
{
	uint8_t d;
#if !(HDUS_CARD_MASK_PRESENT || HDUS_CARD_MASK_READY)
	uint8_t d2;
#endif

	if (card_r(0x04, &d) < 0)
		return HDUS_CARD_ERR_IO;
	if (raw_out)
		*raw_out = d;
	HDUS_LOG("R(0x04)=0x%02x\n", d);

#if HDUS_CARD_MASK_PRESENT || HDUS_CARD_MASK_READY
	{
		/* DLL ConnectCard (docs/card_protocol_connect.md sec.4.3):
		 * (d & READY) == READY -> success, no init;
		 * else (d & PRESENT) == PRESENT -> present, run init;
		 * else -> "Card not present" -3. */
		if ((d & HDUS_CARD_MASK_READY) == HDUS_CARD_MASK_READY)
			return 0;
		if ((d & HDUS_CARD_MASK_PRESENT) == HDUS_CARD_MASK_PRESENT)
			return HDUS_CARD_ERR_NOT_READY; /* caller runs ConnectCard */
		return HDUS_CARD_ERR_NOT_PRESENT;
	}
#else
	if (card_r(0x04, &d2) < 0)
		return HDUS_CARD_ERR_IO;
	HDUS_LOG("R(0x04) second read=0x%02x\n", d2);
	if (d == 0xff || d2 == 0xff || d != d2)
		return HDUS_CARD_ERR_NOT_PRESENT;
	return 0;
#endif
}

/* ---- Kernel transport helpers ----
 * The kernel handler returns either 0/-2/-3/-6 style codes directly
 * (older-style handlers) or -1 with errno set (copy_from_user failures,
 * ENOTTY/ENODEV when the handler is absent). Map both to the public
 * HDUS_CARD_ERR_* codes. */
static int kernel_err_map(int r)
{
	if (r >= 0)
		return 0;
	if (r == HDUS_CARD_ERR_NOT_READY || r == HDUS_CARD_ERR_NOT_PRESENT ||
	    r == HDUS_CARD_ERR_TX)
		return r;
	if (r == -1) {
		if (errno == ENOENT)
			return HDUS_CARD_ERR_NOT_READY;
		if (errno == ENOTDIR || errno == ENODEV)
			return HDUS_CARD_ERR_NOT_PRESENT;
	}
	return HDUS_CARD_ERR_IO;
}

static int hdus_card_present_kernel(void)
{
	int present = 0;
	int r;

	if (g_vfd < 0)
		return 0;
	r = ioctl(g_vfd, HDUS_CARD_PRESENT, &present);
	if (r < 0) {
		HDUS_LOG("HDUS_CARD_PRESENT: %s\n", strerror(errno));
		return 0;
	}
	return present ? 1 : 0;
}

static int hdus_card_connect_kernel(void)
{
	int r;

	if (g_vfd < 0)
		return HDUS_CARD_ERR_IO;
	r = ioctl(g_vfd, HDUS_CARD_CONNECT);
	if (r < 0) {
		HDUS_LOG("HDUS_CARD_CONNECT: %s\n", strerror(errno));
		return kernel_err_map(r);
	}
	/* Kernel may return 0/-2/-3 directly as the ioctl result. */
	if (r == HDUS_CARD_ERR_NOT_READY || r == HDUS_CARD_ERR_NOT_PRESENT)
		return r;
	return r >= 0 ? 0 : HDUS_CARD_ERR_IO;
}

static int hdus_card_transmit_kernel(const uint8_t *req, int req_len,
				     const uint8_t *extra, int extra_len,
				     uint8_t *resp, int resp_max)
{
	hdus_card_req_t cr;
	int r, resp_len;

	if (g_vfd < 0 || !req || req_len <= 0 || !resp || resp_max <= 0)
		return HDUS_CARD_ERR_IO;
	if (extra_len < 0 || (extra_len > 0 && !extra))
		return HDUS_CARD_ERR_IO;
	if (req_len > HDUS_CARD_REQ_LEN_MAX ||
	    extra_len > HDUS_CARD_EXTRA_LEN_MAX)
		return HDUS_CARD_ERR_TX;

	memset(&cr, 0, sizeof(cr));
	cr.req_len = (uint32_t)req_len;
	cr.extra_len = (uint32_t)extra_len;
	cr.resp_len = (uint32_t)(resp_max < HDUS_CARD_RESP_LEN_MAX
				 ? resp_max : HDUS_CARD_RESP_LEN_MAX);
	if (req_len > 0)
		memcpy(cr.req, req, (size_t)req_len);
	if (extra_len > 0)
		memcpy(cr.extra, extra, (size_t)extra_len);

	HDUS_LOG("TRANSMIT ioctl: req_len=%d resp_max=%d g_vfd=%d\n",
		 req_len, resp_max, g_vfd);
	r = ioctl(g_vfd, HDUS_CARD_TRANSMIT, &cr);
	HDUS_LOG("TRANSMIT ioctl: r=%d errno=%d resp_len=%u\n", r, errno,
		 cr.resp_len);
	if (r < 0) {
		HDUS_LOG("HDUS_CARD_TRANSMIT: %s\n", strerror(errno));
		return kernel_err_map(r);
	}
	/* Kernel returns resp_len >= 0, or a negative HDUS_CARD_ERR_* code. */
	if (r < 0)
		return r;
	resp_len = (int)cr.resp_len;
	if (resp_len < 0)
		return HDUS_CARD_ERR_TX;
	if (resp_len > resp_max)
		resp_len = resp_max;
	if (resp_len > HDUS_CARD_RESP_LEN_MAX)
		resp_len = HDUS_CARD_RESP_LEN_MAX;
	if (resp_len > 0)
		memcpy(resp, cr.resp, (size_t)resp_len);
	return resp_len;
}

/* hdus_card_atr_kernel fetches the ATR the kernel collected during
 * CONNECT. It is cached in the driver, so this costs no USB traffic. */
static int hdus_card_atr_kernel(uint8_t *atr, int max)
{
	hdus_card_atr_t uatr;

	if (g_vfd < 0 || !atr || max <= 0)
		return 0;
	memset(&uatr, 0, sizeof(uatr));
	if (ioctl(g_vfd, HDUS_CARD_GET_ATR, &uatr) < 0)
		return 0;
	if ((int)uatr.len > max)
		return max;
	if ((int)uatr.len > (int)sizeof(uatr.atr))
		return 0;
	memcpy(atr, uatr.atr, (size_t)uatr.len);
	return (int)uatr.len;
}

int hdus_card_atr(uint8_t *atr, int max)
{
	if (g_transport == HDUS_CARD_TRANSPORT_KERNEL)
		return hdus_card_atr_kernel(atr, max);
	/* usbfs path keeps the ATR in g_atr captured by connect. */
	if (g_atr_len <= 0)
		return 0;
	if ((int)g_atr_len > max)
		return max;
	memcpy(atr, g_atr, (size_t)g_atr_len);
	return (int)g_atr_len;
}

int hdus_card_present(void)
{
	int raw;
	if (!g_opened)
		return 0;
	if (g_transport == HDUS_CARD_TRANSPORT_KERNEL) {
		pthread_mutex_lock(&g_lock);
		int p = hdus_card_present_kernel();
		pthread_mutex_unlock(&g_lock);
		return p;
	}
	pthread_mutex_lock(&g_lock);
	int r = card_presence_check(&raw);
	pthread_mutex_unlock(&g_lock);
	return r == 0 ? 1 : 0;
}

int hdus_card_connect(void)
{
	if (!g_opened)
		return HDUS_CARD_ERR_IO;
	if (g_transport == HDUS_CARD_TRANSPORT_KERNEL) {
		pthread_mutex_lock(&g_lock);
		int kr = hdus_card_connect_kernel();
		pthread_mutex_unlock(&g_lock);
		return kr;
	}
	pthread_mutex_lock(&g_lock);

	int r = card_presence_check(NULL);
	if (r == HDUS_CARD_ERR_NOT_READY) {
		/* DLL ConnectCard init (docs/card_protocol_connect.md sec.5):
		 * strobes + poll + session read, verified on hardware. */
		uint8_t d;
		card_w(0x00, 0x08);
		card_w(0x04, HDUS_CARD_MASK_READY);
		card_w(0x00, 0x09);
		msleep_(50);
		int prev = -1;
		for (int i = 0; i < 20; i++) {
			msleep_(50);
			card_r(0x39, &d);
			if (card_r(0x03, &d) < 0)
				break;
			if ((int)d == prev)
				break;
			prev = d;
			if (card_r(0x04, &d) < 0 ||
			    (d & HDUS_CARD_MASK_READY) != HDUS_CARD_MASK_READY) {
				pthread_mutex_unlock(&g_lock);
				return HDUS_CARD_ERR_NOT_READY;
			}
		}
		card_w(0x3a, 0);
		msleep_(50);
		/* This read is the ATR (see docs/asie5606-coax-reference.md). */
		int slen = prev < 0 ? 0 : (prev < 13 ? prev : 13);
		for (int i = 0; i < slen; i++) {
			uint8_t v = 0;
			card_r((uint8_t)(0x40 + i), &v);
			g_atr[i] = hdus_card_sbox[v];
		}
		g_atr_len = slen;
		HDUS_LOG("ConnectCard ATR len=%d\n", slen);
		r = 0;
	} else if (r != 0) {
		pthread_mutex_unlock(&g_lock);
		return r;
	}

	/* ConnectCard init tail (sendcard doc section 5, end). */
	static const struct { uint8_t reg, val; } tail[] = {
		{0x00, 0x0c}, {0x01, 1},    {0x02, 5},    {0x38, 0},
		{0x3a, 0},    {0x40, 0},    {0x41, 0x83}, {0x42, 0x80},
		{0x43, 0x7f}, {0x44, 0x7c}, {0x00, 0x0a},
	};
	for (size_t i = 0; i < sizeof(tail) / sizeof(tail[0]); i++)
		card_w(tail[i].reg, tail[i].val);
	msleep_(200);
	g_state = 0;

	pthread_mutex_unlock(&g_lock);
	return 0;
}

/* Chunked FIFO transfer shared by TX (write) and RX (read), sendcard doc
 * sections 5.2/5.3: chunk size 0x40, chunk base starts at 0x40, W(0x3a,0)
 * before the first chunk, W(0x3a,1) before later 0x40-blocks once the
 * chunk base reaches 0x80. */
static int fifo_write(const uint8_t *frame, int frame_len)
{
	/* Per-byte reg increment (docs/card_txrx_trace.md sec.1). */
	uint8_t flag = 0xff;
	for (int i = 0; i < frame_len; i++) {
		int raw = 0x40 + i;
		int reg = (raw < 0x80) ? raw : raw - 0x40;
		if (raw < 0x80) {
			if (flag != 0) { card_w(0x3a, 0); flag = 0; }
		} else {
			if (flag != 1) { card_w(0x3a, 1); flag = 1; }
		}
		uint16_t wi = (uint16_t)frame[i] |
			((i + 1 < frame_len) ? ((uint16_t)frame[i + 1] << 8) : 0);
		card_w((uint8_t)reg, wi);
	}
	return 0;
}

static int fifo_read(uint8_t *resp, int resp_len)
{
	/* Per-byte reg increment (docs/card_txrx_trace.md sec.3). */
	uint8_t flag = 0xff;
	for (int i = 0; i < resp_len; i++) {
		int raw = 0x40 + i;
		int reg = (raw < 0x80) ? raw : raw - 0x40;
		if (raw < 0x80) {
			if (flag != 0) { card_w(0x3a, 0); flag = 0; }
		} else {
			if (flag != 1) { card_w(0x3a, 1); flag = 1; }
		}
		uint8_t v = 0;
		card_r((uint8_t)reg, &v);
		resp[i] = v;
	}
	return 0;
}

int hdus_card_transmit(const uint8_t *req, int req_len,
		       const uint8_t *extra, int extra_len,
		       uint8_t *resp, int resp_max)
{
	uint8_t frame[600];
	int frame_len;
	if (!g_opened || !req || req_len <= 0 || !resp || resp_max <= 0)
		return HDUS_CARD_ERR_IO;
	if (extra_len < 0 || (extra_len > 0 && !extra))
		return HDUS_CARD_ERR_IO;

	HDUS_LOG("wrapper: transport=%d opened=%d\n", g_transport, g_opened);
	if (g_transport == HDUS_CARD_TRANSPORT_KERNEL) {
		pthread_mutex_lock(&g_lock);
		int kr = hdus_card_transmit_kernel(req, req_len, extra,
						   extra_len, resp, resp_max);
		pthread_mutex_unlock(&g_lock);
		if (kr > 4) { /* strip media header [00,state,len] + checksum */
			memmove(resp, resp + 3, (size_t)kr - 4);
			kr -= 4;
		}
		return kr;
	}

	pthread_mutex_lock(&g_lock);

	/* 5.1 presence poll */
	int pr = card_presence_check(NULL);
	if (pr == HDUS_CARD_ERR_NOT_READY) {
		/* DLL runs ConnectCard and retries; we report -2 so the
		 * caller can reconnect once (libaribb25 retries similarly). */
		pthread_mutex_unlock(&g_lock);
		HDUS_LOG("SendCard: Reader not ready\n");
		return HDUS_CARD_ERR_NOT_READY;
	}
	if (pr != 0) {
		pthread_mutex_unlock(&g_lock);
		HDUS_LOG("SendCard: Card not present\n");
		return HDUS_CARD_ERR_NOT_PRESENT;
	}

	/* 3. frame build */
	frame[0] = 0x00;
	frame[1] = g_state;
	memcpy(frame + 3, req, (size_t)req_len);
	frame_len = 3 + req_len;
	if (extra && extra_len) {
		frame[3 + req_len] = (uint8_t)extra_len;
		memcpy(frame + 4 + req_len, extra, (size_t)extra_len);
		frame[4 + req_len + extra_len] = 0x00;
		frame_len = 3 + req_len + 1 + extra_len + 1;
	}
	if (frame_len >= (int)sizeof(frame)) {
		pthread_mutex_unlock(&g_lock);
		return HDUS_CARD_ERR_TX;
	}
	frame[2] = (uint8_t)(frame_len - 3);
	g_state ^= 0x40; /* state byte toggles after the frame is built */

	/* 4. TX sbox transform + checksum (XOR of transformed bytes) */
	uint8_t dl = 0;
	for (int i = 0; i < frame_len; i++) {
		uint8_t b = hdus_card_sbox[frame[i]];
		frame[i] = b;
		dl ^= b;
	}
	frame[frame_len] = dl;
	frame_len++;

	/* 5.2 TX setup (trace doc sec.1.3): W00=0x0c, W02=wIndex frame_len|slave<<8 */
	card_w(0x00, 0x0c);
	card_w(0x02, (uint16_t)frame_len | ((uint16_t)g_slave << 8));
	card_w(0x38, 0);
	fifo_write(frame, frame_len);
	card_w(0x00, 0x0a);
	msleep_(50);

	/* 5.3 RX poll: R(0x39) compared against a counter, up to 0x14
	 * passes of Sleep(50). The DLL also reads R(0x03) in this loop
	 * (disassembly ambiguous) — read and discard it for fidelity. */
	/* 0x03 is the receive-buffer length low byte and 0x39 its high
	 * byte, not a poll counter (docs/asie5606-coax-reference.md). */
	int got = 0;
	for (int pass = 0; pass < 42; pass++) {
		uint8_t lo = 0, hi = 0;
		usleep(20000);
		if (card_r(0x03, &lo) < 0 || card_r(0x39, &hi) < 0)
			break;
		got = (int)lo | ((int)(hi & 1) << 8);
		if (got >= 3)
			break;
	}

	/* Length probe: len = sbox[R(0x42)] + 4; re-read once if it
	 * shrinks the buffer (sendcard doc 5.3 step 12). */
	int resp_len = got > resp_max ? resp_max : got;
	if (resp_len == 4)
		HDUS_LOG("SendCard: Request Length = %d\n", resp_len);
	if (resp_len <= 0) {
		card_w(0x00, 0x0c);
		msleep_(50);
		card_w(0x04, 0);
		pthread_mutex_unlock(&g_lock);
		HDUS_LOG("SendCard: transmit failed\n");
		return HDUS_CARD_ERR_TX;
	}
	if (resp_len > resp_max) {
		HDUS_LOG("SendCard: resp_len %d exceeds resp_max %d, clamping\n",
			 resp_len, resp_max);
		resp_len = resp_max;
	}

	/* RX data loop, then W(0x00,0x0c) */
	fifo_read(resp, resp_len);
	card_w(0x00, 0x0c);

	/* 5.4 decode + verify: XOR of RAW bytes, sbox each, last == dl */
	dl = 0;
	for (int i = 0; i < resp_len - 1; i++) {
		uint8_t raw = resp[i];
		dl ^= raw;
		resp[i] = hdus_card_sbox[raw];
	}
	if (resp_len >= 1 && resp[resp_len - 1] == dl) {
		pthread_mutex_unlock(&g_lock);
		if (resp_len > 4) { /* strip media header + checksum */
			memmove(resp, resp + 3, (size_t)resp_len - 4);
			return resp_len - 4;
		}
		return resp_len;
	}
	msleep_(50);
	card_w(0x04, 0);
	pthread_mutex_unlock(&g_lock);
	HDUS_LOG("SendCard: data error\n");
	return HDUS_CARD_ERR_TX;
}

#ifdef HDUS_CARD_TEST_MAIN
/* hdus_card_test: real-hardware verification tool (built by the Makefile
 * with -DHDUS_CARD_TEST_MAIN; not part of libhdus_ifd.so). */
static void hexprint(const char *tag, const uint8_t *p, int len)
{
	printf("%s (%d bytes):", tag, len);
	for (int i = 0; i < len; i++)
		printf(" %02x", p[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : NULL;
	uint8_t resp[4096];

	if (hdus_card_open(path) != 0) {
		fprintf(stderr, "hdus_card_open failed\n");
		return 1;
	}
	printf("transport: %s\n",
	       hdus_card_transport() == HDUS_CARD_TRANSPORT_KERNEL
	       ? "kernel ioctl (/dev/hdusvideoX)"
	       : hdus_card_transport() == HDUS_CARD_TRANSPORT_USBFS
	       ? "usbfs fallback (card-only exclusive)"
	       : "none");
	int pr = hdus_card_present();
	printf("hdus_card_present = %d\n", pr);

	int cr = hdus_card_connect();
	printf("hdus_card_connect = %d (%s)\n", cr,
	       cr == 0 ? "ok" : cr == -2 ? "reader-not-ready" :
	       cr == -3 ? "card-not-present" : "io error");
	if (cr != 0) {
		hdus_card_close();
		return 2;
	}

	static const uint8_t init_apdu[] = {0x90, 0x30, 0x00, 0x00, 0x00};
	static const uint8_t id_apdu[]   = {0x90, 0x32, 0x00, 0x00, 0x00};
	int n;

	n = hdus_card_transmit(init_apdu, (int)sizeof(init_apdu), NULL, 0,
			       resp, (int)sizeof(resp));
	printf("90 30 00 00 00 -> %d\n", n);
	if (n > 0)
		hexprint("response", resp, n);

	n = hdus_card_transmit(id_apdu, (int)sizeof(id_apdu), NULL, 0,
			       resp, (int)sizeof(resp));
	printf("90 32 00 00 00 -> %d\n", n);
	if (n > 0)
		hexprint("response", resp, n);

	hdus_card_close();
	return 0;
}
#endif
