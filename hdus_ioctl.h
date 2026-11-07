/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdus_ioctl.h - SKNET MonsterTV HDUS userspace/kernel ioctl interface.
 *
 * PT1/PT3/PTX-compatible ioctl numbers (recpt1-compatible) so that
 * BonDriver_LinuxPT(X) / recpt1 can use this driver through /dev/hdusvideoX.
 */
#ifndef _HDUS_IOCTL_H_
#define _HDUS_IOCTL_H_

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <stdint.h>
#include <linux/types.h>
#endif

/* Channel descriptor. Identical layout to PT FREQUENCY / ptx_freq. */
struct hdus_frequency {
	int freq_no;
	int slot;
};

#define HDUS_SET_CHANNEL		_IOW(0x8d, 0x01, struct hdus_frequency)
#define HDUS_START_REC		_IO(0x8d, 0x02)
#define HDUS_STOP_REC		_IO(0x8d, 0x03)
/* Returns raw signal register value (0x8b20 reg); BonDriver converts to CNR. */
#define HDUS_GET_SIGNAL_STRENGTH	_IOR(0x8d, 0x04, int *)
#define HDUS_LNB_ENABLE		_IOW(0x8d, 0x05, int)	/* no-op: HDUS has no LNB */
#define HDUS_LNB_DISABLE		_IO(0x8d, 0x06)		/* no-op: HDUS has no LNB */
#define HDUS_GET_STATUS		_IOR(0x8d, 0x07, int *)	/* streaming ? 1 : 0 */
#define HDUS_SET_TEST_MODE_ON	_IO(0x8d, 0x08)		/* unsupported: -EINVAL */
#define HDUS_SET_TEST_MODE_OFF	_IO(0x8d, 0x09)		/* unsupported: -EINVAL */
#define HDUS_GET_TS_ERROR_PACKET_COUNT	_IOR(0x8d, 0x0a, unsigned int *)
/* PTX: ISDB-T (0x10) ok, ISDB-S (0x20) / other -> -EINVAL */
#define HDUS_PTX_SET_SYSTEM_MODE	_IOW(0x8d, 0x0b, int)

/* Internal B-CAS card (time-multiplexed with tuner, mode-exclusive).
 * The tuner and card share the ASIE5606 register file; every card op
 * power-cycles into card mode and restores the tuner afterwards.
 * Return values use BonDriver SendCard/ConnectCard conventions:
 *   0 ok, -2 reader-not-ready, -3 card-not-present, -6 transmit failed.
 * Note: as kernel ioctl returns these surface to userspace via errno
 * (e.g. -2 -> errno ENOENT); positive resp_len is returned normally. */
struct hdus_card_req {
	__u32 req_len;	/* in: APDU length in req[] (<=264) */
	__u32 extra_len;	/* in: extra length in extra[] (<=264, 0 = none) */
	__u32 resp_len;	/* in: max response capacity; out: actual length */
	__u8 req[264];
	__u8 extra[264];
	__u8 resp[600];
};

/* Run ConnectCard init (power cycle + presence + strobes + tail config).
 * Returns 0 ok, -2 reader-not-ready, -3 card-not-present. */
#define HDUS_CARD_CONNECT		_IO(0x8d, 0x10)
/* Transmit one APDU frame. resp_len in = max cap (<=600), out = actual.
 * Returns resp_len (>=0) or -2/-3/-6. */
#define HDUS_CARD_TRANSMIT	_IOWR(0x8d, 0x11, struct hdus_card_req)
/* 1 present / 0 absent. Runs power cycle + presence + tuner restore
 * internally, so the result is always valid. */
#define HDUS_CARD_PRESENT		_IOR(0x8d, 0x12, int *)
/* Real card ATR collected by CONNECT (0 = none yet). Cached, no USB
 * traffic needed to read it. */
struct hdus_card_atr {
	__u32 len;
	__u8 atr[33];
};
#define HDUS_CARD_GET_ATR		_IOR(0x8d, 0x13, struct hdus_card_atr)

/* PT1/PT3 alias names (so BonDriver code including this header works). */
#ifndef SET_CHANNEL
#define SET_CHANNEL			HDUS_SET_CHANNEL
#endif
#ifndef START_REC
#define START_REC			HDUS_START_REC
#endif
#ifndef STOP_REC
#define STOP_REC			HDUS_STOP_REC
#endif
#ifndef GET_SIGNAL_STRENGTH
#define GET_SIGNAL_STRENGTH		HDUS_GET_SIGNAL_STRENGTH
#endif
#ifndef LNB_ENABLE
#define LNB_ENABLE			HDUS_LNB_ENABLE
#endif
#ifndef LNB_DISABLE
#define LNB_DISABLE			HDUS_LNB_DISABLE
#endif
#ifndef GET_STATUS
#define GET_STATUS			HDUS_GET_STATUS
#endif
#ifndef SET_TEST_MODE_ON
#define SET_TEST_MODE_ON		HDUS_SET_TEST_MODE_ON
#endif
#ifndef SET_TEST_MODE_OFF
#define SET_TEST_MODE_OFF		HDUS_SET_TEST_MODE_OFF
#endif
#ifndef GET_TS_ERROR_PACKET_COUNT
#define GET_TS_ERROR_PACKET_COUNT	HDUS_GET_TS_ERROR_PACKET_COUNT
#endif
#ifndef PTX_SET_SYSTEM_MODE
#define PTX_SET_SYSTEM_MODE		HDUS_PTX_SET_SYSTEM_MODE
#endif

#endif /* _HDUS_IOCTL_H_ */
