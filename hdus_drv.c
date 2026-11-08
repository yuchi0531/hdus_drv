/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdus_drv.c - SKNET MonsterTV HDUS USB driver.
 *
 * Transcribed from recfriio (Hdus.cpp / Hdus.hpp) control protocol.
 * Exposes /dev/hdusvideoX with PT1/PT3/PTX-compatible ioctls (see
 * hdus_ioctl.h) so BonDriver_LinuxPT(X) / recpt1 can use it.
 *
 * CRITICAL: recfriio Hdus::send_ctl() uses bRequestType 0xC0
 * (USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE) for ALL requests,
 * with a data buffer even for writes (device uses wValue/wIndex).
 * Do NOT use 0x40 OUT here.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/atomic.h>
#include <linux/version.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif

#include "hdus_ioctl.h"
#include "hdus_descrambler.h"

#define HDUS_NAME		"hdus"
#define HDUS_CLASS_NAME	"hdusvideo"
#define HDUS_DEV_NAME	"hdusvideo"
#define HDUS_MAX_DEVICES	4

#define HDUS_URB_COUNT	8
#define HDUS_URB_SIZE	16384	/* TSDATASIZE */
#define HDUS_ATR_MAX	33

/* Card receive polling: the receive-buffer length (reg 0x03 / 0x39) is
 * polled in 20 ms steps; the reference implementation uses 50 ms with the
 * same 42 + 8 counts. See docs/asie5606-coax-reference.md. */
/* Card mode is kept while at least this much TS is still buffered. */
#define HDUS_FIFO_LOW_WATER	(1024 * 1024)
#define HDUS_CARD_POLL_US	20000
#define HDUS_CARD_POLLS		42
#define HDUS_CARD_TAIL_POLLS	8

/* Park the chip in card mode this long after a card op. Larger values
 * batch ECM bursts into one power cycle (better TS throughput) at the
 * cost of a longer TS gap; tunable via the card_hold_ms module param. */
static unsigned int card_hold_ms = 150;
module_param(card_hold_ms, uint, 0644);
MODULE_PARM_DESC(card_hold_ms, "hard cap on card-mode parking after an APDU, ms (default 150)");
#define HDUS_FIFO_SIZE	(4 * 1024 * 1024) /* 4MB (kmalloc limit): absorbs the 1-2s TS gap
					  * caused by tuner<->card mode switches */
#define HDUS_CTL_TIMEOUT	2000
#define HDUS_FW_TIMEOUT	5000

#define HDUS_FW_PATH	"hdus/as11fw.bin"

struct hdus_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct cdev cdev;
	struct device *dev_node;
	unsigned int idx;
	dev_t devno;

	atomic_t open_count;
	atomic_t streaming;
	atomic_t disconnected;

	struct mutex ctl_lock;	/* serializes control transfers */
	spinlock_t desc_lock;	/* serializes descrambler_feed */
	u8 *ctl_buf;		/* DMA-safe control transfer buffer (0x200) */

	u8 saddr;
	u8 r5;

	struct hdus_descrambler desc;
	struct kfifo fifo;
	wait_queue_head_t wq;

	unsigned int ep_in;	/* bulk-IN endpoint address (e.g. 0x81) */
	unsigned int max_packet;

	struct urb *urbs[HDUS_URB_COUNT];
	u8 *pipe_buf[HDUS_URB_COUNT];
	u8 *out_buf[HDUS_URB_COUNT];

	int cur_freq_no;
	int cur_slot;
	int cur_mhz;
	bool inited;

	/* Time-multiplexed card access. dev->lock serializes tuner
	 * SET_CHANNEL vs card ioctls (both take it; helpers below assume
	 * it is held where noted). Card regs live at the ASIE slave
	 * (dev->saddr, 0x4a on HDUSF), tuner regs at 0x20. */
	struct mutex lock;
	u8 card_slave;		/* == saddr after detect */
	u8 card_state;		/* frame state byte, toggled ^0x40 per TX */
	bool card_connected;	/* set by CONNECT, cleared by SET_CHANNEL */
	bool has_channel;		/* cur_* valid, re-tuned by tuner_restore */
	/* Restore batching: after a card op the chip stays in card mode
	 * for HDUS_CARD_HOLD_MS so back-to-back APDUs (ECM bursts) share
	 * one power cycle; read()/SET_CHANNEL restore on demand. */
	bool needs_restore;
	unsigned long card_hold_until;
	bool tuner_table_valid;	/* tuner regs written since last power cycle */
	u8 atr[HDUS_ATR_MAX];	/* card ATR, card bit order */
	u8 atr_len;
};

static struct class *hdus_class;
static dev_t hdus_devno_base;
static struct hdus_dev *hdus_devs[HDUS_MAX_DEVICES];
static DEFINE_MUTEX(hdus_dev_lock);

/* ------------------------------------------------------------------ */
/* Control transfer                                                    */
/* ------------------------------------------------------------------ */

/*
 * All HDUS control transfers use bRequestType 0xC0 (IN|VENDOR|DEVICE),
 * even for writes, with a data buffer (see recfriio Hdus::send_ctl).
 * If out != NULL, the received data is copied to out (at least len bytes).
 */
static int hdus_send_ctl(struct hdus_dev *dev, u8 req, u16 wValue, u16 wIndex,
			 u16 len, u8 *out)
{
	int ret;

	mutex_lock(&dev->ctl_lock);
	memset(dev->ctl_buf, 0, len);
	ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
			      req, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      wValue, wIndex, dev->ctl_buf, len, HDUS_CTL_TIMEOUT);
	if (ret >= 0 && out)
		memcpy(out, dev->ctl_buf, len);
	mutex_unlock(&dev->ctl_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Firmware upload (pre-firmware 1738:5211 device)                     */
/* ------------------------------------------------------------------ */

struct hdus_fw_block {
	u8 req;
	u16 off;
	u16 len;
};

static int hdus_fw_upload(struct usb_device *udev)
{
	const struct firmware *fw;
	static const struct hdus_fw_block blocks[] = {
		{ 0xab, 0x0000, 0x0c00 },
		{ 0xab, 0x2000, 0x0400 },
		{ 0xab, 0x2800, 0x1000 },
		{ 0xac, 0x3800, 0x0800 },
	};
	u8 *body;
	size_t body_len;
	u16 idx;
	int ret = 0;
	unsigned int i;

	ret = request_firmware(&fw, HDUS_FW_PATH, &udev->dev);
	if (ret) {
		dev_info(&udev->dev, "firmware not found, run hdus_as11loader\n");
		return -ENODEV;
	}
	if (fw->size < 2) {
		dev_err(&udev->dev, "firmware too small (%zu bytes)\n", fw->size);
		release_firmware(fw);
		return -EINVAL;
	}

	/* 2-byte header: idx = (b1 << 8) | b0; body follows, length 0x4000. */
	idx = (fw->data[1] << 8) | fw->data[0];
	body_len = fw->size - 2;
	body = kzalloc(0x4000, GFP_KERNEL);
	if (!body) {
		release_firmware(fw);
		return -ENOMEM;
	}
	memcpy(body, fw->data + 2, min(body_len, (size_t)0x4000));

	dev_info(&udev->dev, "uploading firmware idx=0x%04x body=%zu bytes\n",
		 idx, body_len);
	for (i = 0; i < ARRAY_SIZE(blocks); i++) {
		ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				      blocks[i].req,
				      USB_DIR_OUT | USB_TYPE_VENDOR |
				      USB_RECIP_DEVICE,
				      blocks[i].off, idx, body + blocks[i].off,
				      blocks[i].len, HDUS_FW_TIMEOUT);
		if (ret < 0) {
			dev_err(&udev->dev,
				"firmware block req=0x%02x off=0x%04x len=0x%04x failed: %d\n",
				blocks[i].req, blocks[i].off, blocks[i].len, ret);
			break;
		}
	}

	kfree(body);
	release_firmware(fw);
	if (ret < 0)
		return ret;

	/* After upload the device re-enumerates as 3275:6051 (HDUS).
	 * Return -ENODEV so usbcore unbinds; the re-enumerated device
	 * probes again and binds as HDUS. */
	dev_info(&udev->dev,
		 "firmware uploaded, device will re-enumerate as 3275:6051\n");
	return -ENODEV;
}

/* ------------------------------------------------------------------ */
/* Hardware init (transcribed from Hdus::UsbProcBegin)                 */
/* ------------------------------------------------------------------ */

/* Power GPIO sequence (Hdus.cpp UsbProcBegin, verbatim usleeps).
 * Shared by hdus_hw_init and hdus_card_power_cycle. */
static void hdus_power_gpio_seq(struct hdus_dev *dev)
{
	(void)hdus_send_ctl(dev, 0x0c, 0x0000, 0x0000, 0x003a, NULL);
	(void)hdus_send_ctl(dev, 0x17, 0x0002, 0x0000, 0x0003, NULL);
	(void)hdus_send_ctl(dev, 0x08, 0xfb07, 0x0000, 0x0001, NULL);
	(void)hdus_send_ctl(dev, 0x10, 0x0200, 0x0000, 0x0001, NULL);

	/* Power off. Delays are halved vs. the vendor sequence: this runs on
	 * every tuner<->card switch, so latency is paid per APDU. */
	msleep(20);
	(void)hdus_send_ctl(dev, 0x08, 0x0100, 0x0000, 0x0001, NULL);
	msleep(40);
	(void)hdus_send_ctl(dev, 0x08, 0xdd0c, 0x0000, 0x0001, NULL);
	msleep(40);

	(void)hdus_send_ctl(dev, 0x17, 0x0002, 0x0000, 0x0003, NULL);

	/* Tuner on */
	(void)hdus_send_ctl(dev, 0x08, 0xd500, 0x0000, 0x0001, NULL);
	msleep(20);
	(void)hdus_send_ctl(dev, 0x08, 0x0800, 0x0000, 0x0001, NULL);
	(void)hdus_send_ctl(dev, 0x08, 0x4040, 0x0000, 0x0001, NULL);
	usleep_range(5000, 6000);
	(void)hdus_send_ctl(dev, 0x08, 0x8080, 0x0000, 0x0001, NULL);
	msleep(20);
	(void)hdus_send_ctl(dev, 0x08, 0x0404, 0x0000, 0x0001, NULL);
	msleep(20);
}

/* ASIE5606 on (the 0x10 bit in the gpio word is the card power bit). */
static void hdus_asie_on(struct hdus_dev *dev)
{
	(void)hdus_send_ctl(dev, 0x08, 0x11ff, 0x0000, 0x0001, NULL);
	msleep(20);
	(void)hdus_send_ctl(dev, 0x08, 0x1100, 0x0000, 0x0001, NULL);
	msleep(20);
	(void)hdus_send_ctl(dev, 0x08, 0x11ff, 0x0000, 0x0001, NULL);
	msleep(40);
}

/* Power-only init for card mode: gpio sequence + ASIE on, NO tuner
 * register table (the full table, e.g. reg 0x11=0x21, overwrites the
 * shared ASIE card-interface regs and kills card access until a full
 * power cycle - verified on hardware 2026-11-07). Caller holds
 * dev->lock (or is in probe, before the device is exposed). */
static void __maybe_unused hdus_card_power_cycle(struct hdus_dev *dev)
{
	dev_info(&dev->udev->dev, "card: power cycle (card mode)\n");
	hdus_power_gpio_seq(dev);
	hdus_asie_on(dev);
	/* Power cycle wipes the tuner register file. */
	dev->tuner_table_valid = false;
}

/* Full tuner register table (Hdus.cpp), factored out of hdus_hw_init
 * so hdus_tuner_restore can re-run it after every card op. */
static void hdus_tuner_init_table(struct hdus_dev *dev)
{
	/* Tuner init */
	(void)hdus_send_ctl(dev, 0x02, 0x00a8, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0120, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0220, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0320, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0420, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0520, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0620, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0720, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0820, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0c20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1120, 0x0021, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1220, 0x000b, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1320, 0x002c, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1420, 0x000a, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1520, 0x0040, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1620, 0x000a, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1720, 0x0070, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1820, 0x0031, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1920, 0x0013, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1a20, 0x0031, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1b20, 0x0013, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1c20, 0x002a, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1d20, 0x00aa, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1e20, 0x00a2, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x1f20, 0x0008, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2020, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2120, 0x00ff, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2220, 0x0080, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2320, 0x004c, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2420, 0x004c, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2520, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2620, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2720, 0x000c, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2820, 0x0060, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2920, 0x006b, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2a20, 0x0040, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2b20, 0x0040, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2c20, 0x00ff, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2d20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2e20, 0x00ff, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x2f20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3020, 0x0028, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3120, 0x000f, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3220, 0x00a0, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3420, 0x003f, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3820, 0x0001, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3920, 0x0070, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3a20, 0x002d, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3b20, 0x0021, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3c20, 0x003f, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3d20, 0x0010, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3e20, 0x0008, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x3f20, 0x000c, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4020, 0x000c, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4120, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4220, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4320, 0x004f, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4420, 0x00ff, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4620, 0x0020, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4720, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4820, 0x0090, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4920, 0x00e6, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4a20, 0x0002, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4b20, 0x0054, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x4c20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5020, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5120, 0x0058, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5220, 0x0020, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5420, 0x0057, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5520, 0x00f1, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5620, 0x0020, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5720, 0x0070, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5820, 0x0060, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5c20, 0x0050, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5d20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x5f20, 0x0080, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x7020, 0x0018, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x7120, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x7220, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x7520, 0x0022, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x7620, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x7720, 0x0001, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x7c20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x7d20, 0x0052, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xba20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xbb20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xbc20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xc220, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xc720, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xe420, 0x001a, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xe920, 0x0008, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xec20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0xef20, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x02, 0x00a8, 0x0000, 0x0002, NULL);
}

/* Crypto config: ASV5211 high speed + ASIE decrypt mode + key/XOR
 * slots. Factored out of hdus_hw_init for hdus_tuner_restore. */
/* Software descrambling happens in hdus_descrambler, so the ASIE's own
 * decrypt configuration is optional; it is what writes registers 0x05..0x1f
 * on the card's I2C slave (0x4a) at init. Skipping it keeps the card
 * interface untouched at bring-up. */
static unsigned int asie_crypto = 1;
module_param(asie_crypto, uint, 0644);
MODULE_PARM_DESC(asie_crypto, "configure ASIE descrambling at init (default 1)");

static void hdus_tuner_crypto_config(struct hdus_dev *dev)
{
	u32 val;

	if (!asie_crypto)
		return;

	/* ASV5211 is high speed */
	(void)hdus_send_ctl(dev, 0x0a, 0x0000, 0x0000, 0x0001, NULL);
	msleep(30);
	/* ASIE decrypt mode configuration */
	(void)hdus_send_ctl(dev, 0x03, 0x0500 | dev->saddr, dev->r5, 0x0002,
			    NULL);

	if (dev->r5 == 0x91) {
		/* Set XOR values which are used against XOR table */
		for (val = 0x1000; val < 0x2000; val += 0x0100)
			(void)hdus_send_ctl(dev, 0x03,
					    (val & 0xff00) | dev->saddr, 0x00,
					    0x0002, NULL);
	} else {
		/* Set key */
		for (val = 0x1000; val < 0x2000; val += 0x0100)
			(void)hdus_send_ctl(dev, 0x03, val | dev->saddr, 0x0000,
					    0x0002, NULL);
	}
}

static int hdus_hw_init(struct hdus_dev *dev)
{
	u8 data[0x200];
	int ret;

	dev->r5 = 0x91;
	dev->saddr = 0x5a;

	hdus_power_gpio_seq(dev);
	hdus_asie_on(dev);
	/* The tuner table lives on I2C slave 0x20 (demodulator) while the
	 * card interface sits on 0x4a (ASIE5606), so both stay usable after
	 * initialisation and no card-status probe is needed here. */
	hdus_tuner_init_table(dev);
	dev->tuner_table_valid = true;

	/* saddr detect (fatal on failure) */
	memset(data, 0, sizeof(data));
	ret = hdus_send_ctl(dev, 0x02, 0x0900 | dev->saddr, 0x0000, 0x0002, data);
	if (ret < 0 || data[0] != 1) {
		dev->saddr = 0x4a; /* HDUSF */
		memset(data, 0, sizeof(data));
		ret = hdus_send_ctl(dev, 0x02, 0x0900 | dev->saddr, 0x0000,
				    0x0002, data);
		if (ret < 0 || data[0] != 1)
			return -ENODEV;
	}
	if ((data[1] & 7) == 6)
		dev->r5 = 0x80; /* DES, else 0x91 XOR */

	hdus_tuner_crypto_config(dev);

	/* Card regs live at the ASIE slave (== saddr, 0x4a on HDUSF), a
	 * different I2C device from the demodulator that owns the tuner
	 * table (slave 0x20). Both therefore stay usable at the same time:
	 * no power cycle and no tuner restore are needed per card op. */
	dev->card_slave = dev->saddr;
	dev->card_state = 0;
	dev->card_connected = false;
	{
		u8 b[2] = { 0, 0 };

		(void)hdus_send_ctl(dev, 0x02,
				    (u16)((0x04 << 8) | dev->card_slave),
				    0x0000, 0x0002, b);
		dev_dbg(&dev->udev->dev,
			"card status at init end: ack=%d R(0x04)=0x%02x\n",
			b[0], b[1]);
	}

	dev_info(&dev->udev->dev,
		 "hdus hw init done (saddr=0x%02x r5=0x%02x)\n",
		 dev->saddr, dev->r5);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Hardware shutdown (transcribed from Hdus::UsbProcEnd)               */
/* ------------------------------------------------------------------ */

static void hdus_hw_shutdown(struct hdus_dev *dev)
{
	if (!dev->saddr)
		return;

	msleep(100);
	(void)hdus_send_ctl(dev, 0x07, 0x0000, 0x0000, 0x0001, NULL);

	(void)hdus_send_ctl(dev, 0x09, 0x0100, 0x0000, 0x0001, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x8000 | dev->saddr, 0x0000, 0x0002,
			    NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0400 | dev->saddr, 0x0000, 0x0002,
			    NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0000 | dev->saddr, 0x0000, 0x0002,
			    NULL);

	/* Power off */
	msleep(50);
	msleep(50);
	(void)hdus_send_ctl(dev, 0x08, 0x0100, 0x0000, 0x0001, NULL);
	msleep(100);
	(void)hdus_send_ctl(dev, 0x08, 0xdd0c, 0x0000, 0x0001, NULL);
	msleep(100);

	dev->saddr = 0;
}

/* ------------------------------------------------------------------ */
/* Channel / frequency                                                 */
/* ------------------------------------------------------------------ */

static int hdus_freq_to_mhz(int freq_no, int *mhz)
{
	int ch;

	if (freq_no >= 63 && freq_no <= 112) {
		/* UHF */
		ch = freq_no - 50; /* 13..62 */
		*mhz = 473 + (ch - 13) * 6;
	} else if (freq_no >= 3 && freq_no <= 12) {
		/* CATV */
		ch = freq_no + 10; /* 13..22 */
		*mhz = 111 + (ch - 13) * 6;
	} else if (freq_no >= 22 && freq_no <= 62) {
		/* CATV */
		ch = freq_no + 1; /* 23..63 */
		if (ch == 23)
			*mhz = 225;
		else if (ch <= 27)
			*mhz = 233 + (ch - 24) * 6;
		else
			*mhz = 225 + (ch - 23) * 6;
	} else {
		return -EINVAL;
	}
	return 0;
}

/* Transcribed from Hdus::setChannel.
 * Caller must hold dev->lock (ioctl SET_CHANNEL and hdus_tuner_restore
 * both do). Sets card_connected=0: tuning clobbers card mode. */
static int hdus_set_channel_locked(struct hdus_dev *dev, int freq_no,
				   int slot)
{
	u8 data[0x200];
	int freq, val;
	int xpid_min = 0x1fff, xpid_max = 0x1fff;
	int ret;

	(void)slot; /* ignored: HDUS has a single tuner */
	if (!dev->saddr)
		return -ENODEV;

	ret = hdus_freq_to_mhz(freq_no, &freq);
	if (ret)
		return ret;

	/* Set frequency */
	val = (freq + 57) * 7;
	msleep(50);
	(void)hdus_send_ctl(dev, 0x0d, 0xfe00, (val & 0xff00) | 0xc0, 0x0004,
			    NULL);
	(void)hdus_send_ctl(dev, 0x0d, ((val & 0xff) << 8) | 0x03,
			    (freq < 420) ? 0x8291 : 0x8491, 0x0004, NULL);
	(void)hdus_send_ctl(dev, 0x0d, 0xe306, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x0e, 0x0020, 0x0000, 0x0008, NULL);
	(void)hdus_send_ctl(dev, 0x03, 0x0120, 0x0040, 0x0002, NULL);

	(void)hdus_send_ctl(dev, 0x09, 0x0100, 0x0000, 0x0001, NULL);
	if (dev->r5 == 0x91) {
		memset(data, 0, sizeof(data));
		(void)hdus_send_ctl(dev, 0x04, 0x0000, 0x0000, 0x0042, data);
		val = data[65];
		(void)hdus_send_ctl(dev, 0x05, 0x003f, (val & 0xff) | 0x04, 0x0003,
				    NULL);
		(void)hdus_send_ctl(dev, 0x05, 0x0f40, 0x0000, 0x0002, NULL);
	}

	/* Set PID filter */
	memset(data, 0, sizeof(data));
	(void)hdus_send_ctl(dev, 0x04, 0x0040, 0x0000, 0x0002, data);
	(void)hdus_send_ctl(dev, 0x05,
			    ((data[1] | (dev->r5 == 0x91 ? 0x03 : 0x0f)) << 8) |
			    0x40,
			    0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x05, (xpid_min & 0xff00) | 0x41,
			    (xpid_min & 0xff), 0x0003, NULL);
	(void)hdus_send_ctl(dev, 0x05, (xpid_max & 0xff00) | 0x43,
			    (xpid_max & 0xff), 0x0003, NULL);

	/* kick ASV5211 bulk transfer */
	(void)hdus_send_ctl(dev, 0x06, 0x0000, 0x0000, 0x0001, NULL);
	(void)hdus_send_ctl(dev, 0x04, 0x0040, 0x0000, 0x0002, NULL);
	(void)hdus_send_ctl(dev, 0x05, 0x0f40, 0x0000, 0x0002, NULL);
	msleep(150);

	dev->cur_freq_no = freq_no;
	dev->cur_slot = slot;
	dev->cur_mhz = freq;
	dev->has_channel = true;
	dev->card_connected = false;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Card access (time-multiplexed with tuner)                           */
/* ------------------------------------------------------------------ */
/* All *_locked helpers assume dev->lock is held. Lock ordering:
 * dev->lock (outer) -> ctl_lock (inside hdus_send_ctl, leaf). No path
 * takes dev->lock while holding ctl_lock, and URB completion never
 * takes dev->lock, so card ops holding dev->lock while calling
 * hdus_set_channel_locked (via hdus_tuner_restore) cannot deadlock. */

#define HDUS_CARD_MASK_READY	0x80
#define HDUS_CARD_MASK_PRESENT	0x01
#define HDUS_CARD_REQ_MAX		264
#define HDUS_CARD_EXTRA_MAX	264
#define HDUS_CARD_RESP_MAX	600

/* sbox is bitreverse8. */
static u8 hdus_card_sbox(u8 b)
{
	b = (u8)(((b & 0x55) << 1) | ((b >> 1) & 0x55));
	b = (u8)(((b & 0x33) << 2) | ((b >> 2) & 0x33));
	return (u8)((b << 4) | (b >> 4));
}

/* R(reg): req 0x02, wValue (reg<<8)|card_slave; ok iff ret>=0 && b[0]==1. */
static int hdus_card_r(struct hdus_dev *dev, u8 reg, u8 *out)
{
	u8 b[2];
	int ret;

	ret = hdus_send_ctl(dev, 0x02, (u16)((reg << 8) | dev->card_slave),
			    0x0000, 0x0002, b);
	if (ret < 0 || b[0] != 1)
		return -1;
	*out = b[1];
	return 0;
}

/* W(reg,val): req 0x03, wValue (reg<<8)|card_slave, wIndex val. */
static int hdus_card_w(struct hdus_dev *dev, u8 reg, u16 val)
{
	u8 b[2];
	int ret;

	ret = hdus_send_ctl(dev, 0x03, (u16)((reg << 8) | dev->card_slave),
			    val, 0x0002, b);
	if (ret < 0 || b[0] != 1)
		return -1;
	return 0;
}

/* Restore tuner mode after a card op: full tuner table + crypto
 * config + re-tune the current channel (PID kick in setChannel
 * restarts TS; bulk URBs keep running). Caller holds dev->lock.
 * Must run after EVERY card op: card config writes (0x38-0x44)
 * degrade tuner signal, and the tuner table kills card regs (so it
 * must only run here, never before card access). */
static void hdus_tuner_restore(struct hdus_dev *dev)
{
	/* Skip restore when no channel was ever tuned: chip may stay
	 * in card mode (card_connected stays true) so pcscd polling
	 * and consecutive card ops stay cheap. SET_CHANNEL (below)
	 * re-inits the tuner table before tuning in that case. */
	if (!dev->has_channel)
		return;
	unsigned long t0 = jiffies;

	dev_info(&dev->udev->dev, "card: tuner restore\n");
	/* Tuning alone brings the tuner back after a card op: the full
	 * register table costs ~1.2 s of the ~1.4 s restore and is only
	 * required after a power cycle. */
	if (!dev->tuner_table_valid) {
		hdus_tuner_init_table(dev);
		hdus_tuner_crypto_config(dev);
		dev->tuner_table_valid = true;
	}
	dev_info(&dev->udev->dev, "card: table+crypto %u ms\n",
		 jiffies_to_msecs(jiffies - t0));
	(void)hdus_set_channel_locked(dev, dev->cur_freq_no,
				      dev->cur_slot);
	dev_info(&dev->udev->dev, "card: retune %u ms\n",
		 jiffies_to_msecs(jiffies - t0));
}

/* Presence from one R(0x04): 1 ready-or-present, 0 absent, -1 IO. */
static int hdus_card_presence_locked(struct hdus_dev *dev)
{
	u8 d;

	if (hdus_card_r(dev, 0x04, &d) < 0)
		return -1;
	if ((d & HDUS_CARD_MASK_READY) == HDUS_CARD_MASK_READY ||
	    (d & HDUS_CARD_MASK_PRESENT) == HDUS_CARD_MASK_PRESENT)
		return 1;
	return 0;
}

/* ConnectCard init (BonDriver_HDUS.dll): presence, strobes, poll,
 * session read, tail config. Caller holds dev->lock and has already
 * run hdus_card_power_cycle. Returns 0 / -2 / -3. */
static int hdus_card_connect_locked(struct hdus_dev *dev)
{
	static const struct { u8 reg, val; } tail[] = {
		{ 0x00, 0x0c }, { 0x01, 0x01 }, { 0x02, 0x05 },
		{ 0x38, 0x00 }, { 0x3a, 0x00 }, { 0x40, 0x00 },
		{ 0x41, 0x83 }, { 0x42, 0x80 }, { 0x43, 0x7f },
		{ 0x44, 0x7c }, { 0x00, 0x0a },
	};
	u8 d;
	int prev = -1;
	int i, slen;
	size_t t;

	dev_dbg(&dev->udev->dev, "card: connect\n");
	if (hdus_card_r(dev, 0x04, &d) < 0) {
		dev_dbg(&dev->udev->dev, "card: connect R(0x04) failed\n");
		return -2;
	}
	dev_dbg(&dev->udev->dev, "card: connect R(0x04)=0x%02x\n", d);
	if ((d & HDUS_CARD_MASK_READY) == HDUS_CARD_MASK_READY)
		return 0; /* already ready, no init needed */
	if ((d & HDUS_CARD_MASK_PRESENT) != HDUS_CARD_MASK_PRESENT)
		return -3;

	/* Strobes */
	(void)hdus_card_w(dev, 0x00, 0x08);
	(void)hdus_card_w(dev, 0x04, HDUS_CARD_MASK_READY);
	(void)hdus_card_w(dev, 0x00, 0x09);
	msleep(50);

	/* Poll R39/R03 (20x50ms, ready re-check each pass) */
	for (i = 0; i < 20; i++) {
		msleep(50);
		(void)hdus_card_r(dev, 0x39, &d);
		if (hdus_card_r(dev, 0x03, &d) < 0)
			break;
		if ((int)d == prev)
			break;
		prev = d;
		if (hdus_card_r(dev, 0x04, &d) < 0 ||
		    (d & HDUS_CARD_MASK_READY) != HDUS_CARD_MASK_READY)
			return -2;
	}

	(void)hdus_card_w(dev, 0x3a, 0);
	msleep(50);

	/* This 13-byte read is the card's ATR (the reference
	 * implementation calls it that): keep it, bit-reversed into card
	 * order, so PC/SC can report the real ATR instead of a synthetic
	 * one. docs/asie5606-coax-reference.md sec.2. */
	slen = prev < 0 ? 0 : (prev < HDUS_ATR_MAX ? prev : HDUS_ATR_MAX);
	for (i = 0; i < slen; i++) {
		u8 v = 0;

		(void)hdus_card_r(dev, (u8)(0x40 + i), &v);
		dev->atr[i] = hdus_card_sbox(v);
	}
	dev->atr_len = (u8)slen;
	dev_info(&dev->udev->dev, "card: ATR len=%d: %*phN\n", slen,
		 slen, dev->atr);

	/* Tail config */
	for (t = 0; t < ARRAY_SIZE(tail); t++)
		(void)hdus_card_w(dev, tail[t].reg, tail[t].val);
	msleep(200);
	dev->card_state = 0;
	return 0;
}

/* Chunked FIFO write (sendcard doc 5.2): 0x40-byte chunks, base
 * starts 0x40, W(0x3a,0/1) interleave once base reaches 0x80.
 * Mirrors fifo_write in tools/ifd-hdus/hdus_card.c. */
static int hdus_card_fifo_write(struct hdus_dev *dev, const u8 *frame,
				int frame_len)
{
	/* Per-byte reg increment (docs/card_txrx_trace.md sec.1):
	 * reg = 0x40+i for i<64 (raw counter 0x40..0x7f), then reg = i
	 * (raw counter 0x80.. wraps back). wIndex is a word load
	 * frame[i] | frame[i+1]<<8 (low byte is what matters). */
	u8 flag = 0xff;
	int i;

	for (i = 0; i < frame_len; i++) {
		int raw = 0x40 + i;
		int reg = (raw < 0x80) ? raw : raw - 0x40;
		u16 wi;

		if (raw < 0x80) {
			if (flag != 0) {
				(void)hdus_card_w(dev, 0x3a, 0);
				flag = 0;
			}
		} else {
			if (flag != 1) {
				(void)hdus_card_w(dev, 0x3a, 1);
				flag = 1;
			}
		}
		wi = (u16)frame[i] |
			((i + 1 < frame_len) ? ((u16)frame[i + 1] << 8) : 0);
		(void)hdus_card_w(dev, (u8)reg, wi);
	}
	return 0;
}

/* Chunked FIFO read (sendcard doc 5.3), mirrors fifo_read. */
/* Read `to - from` bytes of the 0x40 window into resp[from..to).
 * The window is 64 bytes per page (0x3a selects the page) and the
 * bridge exposes one register byte per control transfer, so this walks
 * the buffer one byte at a time. */
static int hdus_card_window_read(struct hdus_dev *dev, u8 *resp, int from,
				 int to)
{
	u8 flag = 0xff;
	int i;

	for (i = from; i < to; i++) {
		int raw = 0x40 + i;
		int reg = (raw < 0x80) ? raw : raw - 0x40;
		u8 v = 0;

		if (raw < 0x80) {
			if (flag != 0) {
				(void)hdus_card_w(dev, 0x3a, 0);
				flag = 0;
			}
		} else {
			if (flag != 1) {
				(void)hdus_card_w(dev, 0x3a, 1);
				flag = 1;
			}
		}
		(void)hdus_card_r(dev, (u8)reg, &v);
		resp[i] = v;
	}
	return 0;
}

static int hdus_card_fifo_read(struct hdus_dev *dev, u8 *resp, int resp_len)
{
	return hdus_card_window_read(dev, resp, 0, resp_len);
}

static int hdus_card_fifo_read_from(struct hdus_dev *dev, u8 *resp, int from,
				    int to)
{
	return hdus_card_window_read(dev, resp, from, to);
}

/* SendCard transmit. Caller holds dev->lock and the chip is in card
 * mode with the card connected. Returns resp_len (>=0) or -2/-3/-6. */
static int hdus_card_transmit_locked(struct hdus_dev *dev,
				     const u8 *req, int req_len,
				     const u8 *extra, int extra_len,
				     u8 *resp, int resp_max)
{
	u8 frame[600];
	int frame_len, resp_len = 0;
	u8 d = 0, dl;
	int pr, pass, i, total = 0, got = 0;

	pr = hdus_card_presence_locked(dev);
	if (pr < 0)
		return -2;
	if (pr == 0)
		return -3;
	/* Present but not ready? presence helper says ready-or-present;
	 * re-check ready bit to distinguish -2 (not ready) from TX. */
	if (hdus_card_r(dev, 0x04, &d) == 0 &&
	    (d & HDUS_CARD_MASK_READY) != HDUS_CARD_MASK_READY)
		return -2;

	/* Frame build: [0, state, len-3, req, (extra: len+data+NUL)] */
	frame[0] = 0x00;
	frame[1] = dev->card_state;
	memcpy(frame + 3, req, req_len);
	frame_len = 3 + req_len;
	if (extra_len > 0) {
		frame[3 + req_len] = (u8)extra_len;
		memcpy(frame + 4 + req_len, extra, extra_len);
		frame[4 + req_len + extra_len] = 0x00;
		frame_len = 3 + req_len + 1 + extra_len + 1;
	}
	if (frame_len >= (int)sizeof(frame))
		return -6;
	frame[2] = (u8)(frame_len - 3);
	dev->card_state ^= 0x40; /* toggles after the frame is built */

	/* Sbox transform + XOR checksum of transformed bytes */
	dl = 0;
	for (i = 0; i < frame_len; i++) {
		u8 b = hdus_card_sbox(frame[i]);

		frame[i] = b;
		dl ^= b;
	}
	frame[frame_len] = dl;
	frame_len++;

	/* TX setup (trace doc sec.1.3): W00=0x0c, then W02 with
	 * wIndex = frame_len | slave_derived<<8, then W38=0. */
	(void)hdus_card_w(dev, 0x00, 0x0c);
	(void)hdus_card_w(dev, 0x02, (u16)frame_len |
			    ((u16)dev->card_slave << 8));
	(void)hdus_card_w(dev, 0x38, 0);
	hdus_card_fifo_write(dev, frame, frame_len);
	(void)hdus_card_w(dev, 0x00, 0x0a);
	msleep(50);

	/* RX poll, using the register semantics documented in
	 * docs/asie5606-coax-reference.md: 0x03 is the receive-buffer length
	 * low byte and 0x39 its high byte (bit 0 only), NOT a poll counter.
	 * The block is ready once at least the 3-byte header is in. */
	total = 0;
	got = 0;
	for (pass = 0; pass < HDUS_CARD_POLLS; pass++) {
		u8 lo = 0, hi = 0;

		usleep_range(HDUS_CARD_POLL_US, HDUS_CARD_POLL_US + 3000);
		if (hdus_card_r(dev, 0x03, &lo) < 0)
			break;
		if (hdus_card_r(dev, 0x39, &hi) < 0)
			break;
		got = (int)lo | ((int)(hi & 1) << 8);
		if (got >= 3)
			break;
	}
	if (got < 3) {
		(void)hdus_card_w(dev, 0x00, 0x0c);
		msleep(20);
		return -6;
	}

	/* The receive buffer holds the whole block, LRC included; read it. */
	resp_len = got > resp_max ? resp_max : got;
	hdus_card_fifo_read(dev, resp, resp_len);

	/* LEN is bit-reversed like the rest of the block and covers the
	 * information field only, so the block is LEN + 4 bytes long. A
	 * short buffer means the tail is still arriving: wait for it. */
	total = hdus_card_sbox(resp[2]) + 4;
	if (total > resp_len && total <= resp_max) {
		got = 0;
		for (pass = 0; pass < HDUS_CARD_TAIL_POLLS; pass++) {
			u8 lo = 0, hi = 0;

			usleep_range(HDUS_CARD_POLL_US,
				     HDUS_CARD_POLL_US + 3000);
			if (hdus_card_r(dev, 0x03, &lo) < 0 ||
			    hdus_card_r(dev, 0x39, &hi) < 0)
				break;
			got = (int)lo | ((int)(hi & 1) << 8);
			if (got >= total)
				break;
		}
		if (got >= total)
			hdus_card_fifo_read_from(dev, resp, resp_len, total);
	}
	if (total > resp_max) {
		dev_info(&dev->udev->dev,
			 "card: block %d exceeds cap %d, clamping\n",
			 total, resp_max);
		total = resp_max;
	}
	resp_len = total > resp_len ? total : resp_len;
	if (resp_len > resp_max)
		resp_len = resp_max;
	(void)hdus_card_w(dev, 0x00, 0x0c);

	/* LRC: the XOR of every byte of the block, in card bit order, is
	 * zero (see docs/asie5606-coax-reference.md). Then the bytes are
	 * un-reversed, leaving the payload for the caller. */
	dl = 0;
	for (i = 0; i < resp_len; i++)
		dl ^= resp[i];
	if (dl != 0) {
		dev_info(&dev->udev->dev, "card: block failed its LRC\n");
		msleep(20);
		(void)hdus_card_w(dev, 0x04, 0);
		return -6;
	}
	for (i = 0; i < resp_len - 1; i++)
		resp[i] = hdus_card_sbox(resp[i]);
	return resp_len;
	msleep(50);
	(void)hdus_card_w(dev, 0x04, 0);
	return -6;
}

static int hdus_get_signal(struct hdus_dev *dev, int *val_out)
{
	u8 d[2];
	u32 val;
	int ret;

	ret = hdus_send_ctl(dev, 0x02, 0x8b20, 0x0000, 0x0002, d);
	if (ret < 0)
		return ret;
	val = ((u32)d[1]) << 16;
	ret = hdus_send_ctl(dev, 0x02, 0x8c20, 0x0000, 0x0002, d);
	if (ret < 0)
		return ret;
	val |= ((u32)d[1]) << 8;
	ret = hdus_send_ctl(dev, 0x02, 0x8d20, 0x0000, 0x0002, d);
	if (ret < 0)
		return ret;
	val |= d[1];

	*val_out = (int)val;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Streaming URBs                                                      */
/* ------------------------------------------------------------------ */

static void hdus_urb_complete(struct urb *urb)
{
	struct hdus_dev *dev = urb->context;
	unsigned int i;
	size_t n = 0;
	unsigned long flags;

	for (i = 0; i < HDUS_URB_COUNT; i++)
		if (dev->urbs[i] == urb)
			break;
	if (i >= HDUS_URB_COUNT)
		return;

	/* While the chip is parked in card mode the tuner produces no
	 * valid TS: drop the URB payloads so the FIFO drains and read()
	 * can restore tuner mode. */
	if (READ_ONCE(dev->needs_restore))
		goto resubmit;

	if (urb->status == 0 && urb->actual_length > 0) {
		spin_lock_irqsave(&dev->desc_lock, flags);
		n = hdus_descrambler_feed(&dev->desc,
					  urb->transfer_buffer,
					  urb->actual_length, dev->out_buf[i],
					  HDUS_URB_SIZE);
		spin_unlock_irqrestore(&dev->desc_lock, flags);
		if (n > 0) {
			kfifo_in(&dev->fifo, dev->out_buf[i], n);
			wake_up_interruptible(&dev->wq);
		}
	}

resubmit:
	if (!atomic_read(&dev->disconnected) &&
	    atomic_read(&dev->streaming) &&
	    urb->status != -ENOENT && urb->status != -ESHUTDOWN &&
	    urb->status != -ENODEV)
		usb_submit_urb(urb, GFP_ATOMIC);
}

static int hdus_start_urbs(struct hdus_dev *dev)
{
	int i, ret;

	if (atomic_read(&dev->streaming))
		return 0;
	atomic_set(&dev->streaming, 1);
	for (i = 0; i < HDUS_URB_COUNT; i++) {
		ret = usb_submit_urb(dev->urbs[i], GFP_KERNEL);
		if (ret) {
			atomic_set(&dev->streaming, 0);
			while (i > 0)
				usb_kill_urb(dev->urbs[--i]);
			return ret;
		}
	}
	return 0;
}

static void hdus_stop_urbs(struct hdus_dev *dev)
{
	int i;

	if (!atomic_read(&dev->streaming))
		return;
	atomic_set(&dev->streaming, 0);
	for (i = 0; i < HDUS_URB_COUNT; i++)
		usb_kill_urb(dev->urbs[i]);
	wake_up_interruptible(&dev->wq);
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

static int hdus_open(struct inode *inode, struct file *file)
{
	struct hdus_dev *dev = container_of(inode->i_cdev, struct hdus_dev,
					    cdev);

	/* Multi-open allowed: tuner streaming (hdus_test / recpt1 via
	 * BonDriver) and card access (pcscd IFD handler) must coexist.
	 * Exclusivity is enforced by dev->lock around tuner/card mode
	 * switches, not by single-open. */
	if (atomic_read(&dev->disconnected))
		return -ENODEV;
	atomic_inc(&dev->open_count);
	file->private_data = dev;
	return 0;
}

static int hdus_release(struct inode *inode, struct file *file)
{
	struct hdus_dev *dev = file->private_data;

	hdus_stop_urbs(dev);
	atomic_dec(&dev->open_count);
	return 0;
}

/* Return to tuner mode if a card op left the chip in card mode and the
 * hold window has elapsed. Called from read()/poll() contexts. */
static void __maybe_unused hdus_maybe_restore(struct hdus_dev *dev)
{
	if (!dev->needs_restore)
		return;
	/* While streaming, stay in card mode as long as the FIFO can still
	 * feed the reader: every APDU served from one card session saves a
	 * full power cycle plus tuner restore. Return to the tuner when the
	 * buffer runs low, and on a hard cap so a stalled reader cannot
	 * park the chip indefinitely. */
	if (atomic_read(&dev->streaming) &&
	    time_before(jiffies, dev->card_hold_until))
		return;
	if (!mutex_trylock(&dev->lock)) {
		dev_info(&dev->udev->dev, "card: restore deferred (lock busy)\n");
		return;
	}
	if (dev->needs_restore) {
		dev_info(&dev->udev->dev, "card: restoring tuner (has_channel=%d)\n",
			 dev->has_channel);
		hdus_tuner_restore(dev);
		dev->needs_restore = false;
	}
	mutex_unlock(&dev->lock);
}

/* Park the chip in card mode for a short window instead of restoring
 * immediately: consecutive APDUs (ECM bursts arrive every 100-500 ms)
 * then share a single ~1.6 s power-cycle cycle. */
static void __maybe_unused hdus_defer_restore(struct hdus_dev *dev)
{
	/* Unused since the card interface proved independent of the tuner;
	 * kept for boards whose ASIE shares the stream registers. */
	dev->needs_restore = true;
	dev->card_hold_until = jiffies + msecs_to_jiffies(card_hold_ms);
	dev_info(&dev->udev->dev, "card: defer restore (hold %u ms)\n",
		 card_hold_ms);
}

static ssize_t hdus_read(struct file *file, char __user *buf, size_t count,
			 loff_t *pos)
{
	struct hdus_dev *dev = file->private_data;
	unsigned int copied = 0;
	int ret;

	if (atomic_read(&dev->disconnected))
		return -ENODEV;

	/* Tuner data is only produced in tuner mode: come back if a card
	 * op parked the chip and the hold window expired. */
	hdus_maybe_restore(dev);

	while (!kfifo_len(&dev->fifo)) {
		if (file->f_flags & O_NONBLOCK) {
			hdus_maybe_restore(dev);
			if (!kfifo_len(&dev->fifo))
				return -EAGAIN;
			break;
		}
		if (!atomic_read(&dev->streaming) ||
		    atomic_read(&dev->disconnected))
			return 0;
		/* Poll with a timeout so a parked card mode cannot stall
		 * the reader forever: each wake-up retries the restore. */
		ret = wait_event_interruptible_timeout(dev->wq,
			kfifo_len(&dev->fifo) > 0 ||
			!atomic_read(&dev->streaming) ||
			atomic_read(&dev->disconnected),
			msecs_to_jiffies(100));
		if (ret == -ERESTARTSYS)
			return -ERESTARTSYS;
		hdus_maybe_restore(dev);
	}

	ret = kfifo_to_user(&dev->fifo, buf, count, &copied);
	if (ret)
		return ret;
	return copied;
}

static __poll_t hdus_poll(struct file *file, poll_table *wait)
{
	struct hdus_dev *dev = file->private_data;
	__poll_t mask = 0;

	/* A parked card mode stops TS production; poll() is the only wake
	 * point when the FIFO is empty, so restore from here too. */
	hdus_maybe_restore(dev);
	poll_wait(file, &dev->wq, wait);
	if (kfifo_len(&dev->fifo) > 0)
		mask |= POLLIN | POLLRDNORM;
	return mask;
}

static long hdus_ioctl(struct hdus_dev *dev, unsigned int cmd,
		       unsigned long arg, bool compat)
{
	void __user *uarg = compat ? compat_ptr(arg) : (void __user *)arg;
	struct hdus_frequency freq;
	int mode;
	int status;
	int val;
	unsigned int err_count;
	int ret;

	if (atomic_read(&dev->disconnected))
		return -ENODEV;

	switch (cmd) {
	case HDUS_SET_CHANNEL:
		if (copy_from_user(&freq, uarg, sizeof(freq)))
			return -EFAULT;
		/* dev->lock guards tuner vs card mode (see card section). */
		mutex_lock(&dev->lock);
		if (dev->needs_restore || dev->card_connected) {
			/* Chip is in card mode: rewrite tuner table before
			 * tuning (hdus_tuner_restore() bails out when no
			 * channel has been set yet). */
			hdus_tuner_init_table(dev);
			hdus_tuner_crypto_config(dev);
			dev->needs_restore = false;
			dev->card_connected = false;
		}
		ret = hdus_set_channel_locked(dev, freq.freq_no, freq.slot);
		mutex_unlock(&dev->lock);
		return ret;

	case HDUS_START_REC:
		if (dev->needs_restore) {
			mutex_lock(&dev->lock);
			hdus_tuner_init_table(dev);
			hdus_tuner_crypto_config(dev);
			if (dev->has_channel)
				(void)hdus_set_channel_locked(dev,
					dev->cur_freq_no, dev->cur_slot);
			dev->needs_restore = false;
			dev->card_connected = false;
			mutex_unlock(&dev->lock);
		}
		return hdus_start_urbs(dev);

	case HDUS_STOP_REC:
		hdus_stop_urbs(dev);
		return 0;

	case HDUS_GET_SIGNAL_STRENGTH:
		ret = hdus_get_signal(dev, &val);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &val, sizeof(val)))
			return -EFAULT;
		return 0;

	case HDUS_LNB_ENABLE:
		/* no-op: HDUS has no LNB */
		return 0;

	case HDUS_LNB_DISABLE:
		/* no-op: HDUS has no LNB */
		return 0;

	case HDUS_GET_STATUS:
		status = atomic_read(&dev->streaming) ? 1 : 0;
		if (copy_to_user(uarg, &status, sizeof(status)))
			return -EFAULT;
		return 0;

	case HDUS_SET_TEST_MODE_ON:
	case HDUS_SET_TEST_MODE_OFF:
		return -EINVAL;

	case HDUS_GET_TS_ERROR_PACKET_COUNT:
		err_count = (unsigned int)dev->desc.dropped;
		if (copy_to_user(uarg, &err_count, sizeof(err_count)))
			return -EFAULT;
		return 0;

	case HDUS_PTX_SET_SYSTEM_MODE:
		if (copy_from_user(&mode, uarg, sizeof(mode)))
			return -EFAULT;
		/* ISDB-T (0x10) ok; ISDB-S (0x20) / other -> -EINVAL */
		return (mode == 0x10) ? 0 : -EINVAL;

	case HDUS_CARD_CONNECT: {
		/* Streaming URBs keep running across the mode switch
		 * (PID kick in tuner restore restarts TS); just log it. */
		dev_info(&dev->udev->dev, "card: CONNECT ioctl streaming=%d\n",
			 atomic_read(&dev->streaming));
		/* The card interface (I2C slave 0x4a) is independent of the
		 * demodulator (slave 0x20) that owns the tuner table, so a
		 * card session needs neither a power cycle nor a tuner
		 * restore: the stream keeps running. */
		mutex_lock(&dev->lock);
		if (!dev->card_connected) {
			ret = hdus_card_connect_locked(dev);
			if (ret == 0)
				dev->card_connected = true;
		} else {
			ret = 0;
		}
		mutex_unlock(&dev->lock);
		return ret;
	}

	case HDUS_CARD_TRANSMIT: {
		struct hdus_card_req *creq;
		int resp_cap, actual;

		/* Heap: struct hdus_card_req is 1140B, over the 1K
		 * stack-frame warning threshold. */
		creq = kzalloc(sizeof(*creq), GFP_KERNEL);
		if (!creq)
			return -ENOMEM;
		if (copy_from_user(creq, uarg, sizeof(*creq))) {
			kfree(creq);
			return -EFAULT;
		}
		if (creq->req_len == 0 || creq->req_len > HDUS_CARD_REQ_MAX ||
		    creq->extra_len > HDUS_CARD_EXTRA_MAX) {
			kfree(creq);
			return -6;
		}
		resp_cap = creq->resp_len ? (int)creq->resp_len :
			HDUS_CARD_RESP_MAX;
		if (resp_cap > HDUS_CARD_RESP_MAX)
			resp_cap = HDUS_CARD_RESP_MAX;
		if (resp_cap <= 0) {
			kfree(creq);
			return -6;
		}

		mutex_lock(&dev->lock);
		dev_dbg(&dev->udev->dev,
			 "card: TRANSMIT req_len=%u extra_len=%u cap=%d connected=%d\n",
			 creq->req_len, creq->extra_len, resp_cap,
			 dev->card_connected);
		if (!dev->card_connected) {
			ret = hdus_card_connect_locked(dev);
			if (ret == 0)
				dev->card_connected = true;
		} else {
			ret = 0;
		}
		if (ret == 0)
			ret = hdus_card_transmit_locked(dev, creq->req,
							creq->req_len,
							creq->extra,
							creq->extra_len,
							creq->resp, resp_cap);
		actual = ret;
		mutex_unlock(&dev->lock);
		if (actual < 0) {
			kfree(creq);
			return actual;
		}
		creq->resp_len = (u32)actual;
		ret = copy_to_user(uarg, creq, sizeof(*creq)) ? -EFAULT :
			actual;
		kfree(creq);
		return ret;
	}

	case HDUS_CARD_GET_ATR: {
		struct hdus_card_atr uatr;

		memset(&uatr, 0, sizeof(uatr));
		uatr.len = dev->atr_len;
		if (uatr.len > sizeof(uatr.atr))
			uatr.len = sizeof(uatr.atr);
		memcpy(uatr.atr, dev->atr, uatr.len);
		if (copy_to_user(uarg, &uatr, sizeof(uatr)))
			return -EFAULT;
		return 0;
	}

	case HDUS_CARD_PRESENT: {
		/* Cheap path when chip is already in card mode (pcscd polls
		 * presence every second); otherwise full internal cycle. */
		int present;

		mutex_lock(&dev->lock);
		present = hdus_card_presence_locked(dev);
		mutex_unlock(&dev->lock);
		if (present < 0)
			present = 0;
		if (copy_to_user(uarg, &present, sizeof(present)))
			return -EFAULT;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static long hdus_ioctl_unlocked(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	return hdus_ioctl(file->private_data, cmd, arg, false);
}

#ifdef CONFIG_COMPAT
static long hdus_ioctl_compat(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	return hdus_ioctl(file->private_data, cmd, arg, true);
}
#endif

static const struct file_operations hdus_fops = {
	.owner = THIS_MODULE,
	.open = hdus_open,
	.release = hdus_release,
	.read = hdus_read,
	.poll = hdus_poll,
	.unlocked_ioctl = hdus_ioctl_unlocked,
#ifdef CONFIG_COMPAT
	.compat_ioctl = hdus_ioctl_compat,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	.llseek = NULL,
#else
	.llseek = no_llseek,
#endif
};

/* ------------------------------------------------------------------ */
/* USB driver core                                                     */
/* ------------------------------------------------------------------ */

/* Only interface 0 carries the bulk-IN stream endpoint; interface 1 has no
 * 0x81 and would otherwise fail to probe (and log an error) on every load. */
static const struct usb_device_id hdus_usb_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x3275, 0x6051, 0) }, /* HDUS */
	{ USB_DEVICE_INTERFACE_NUMBER(0x3275, 0x6052, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x3275, 0x6081, 0) }, /* FS100U */
	{ USB_DEVICE_INTERFACE_NUMBER(0x3275, 0x6091, 0) },
	{ USB_DEVICE_INTERFACE_NUMBER(0x3275, 0x6111, 0) }, /* HDP2 */
	{ USB_DEVICE_INTERFACE_NUMBER(0x3275, 0x7010, 0) }, /* HDP */
	{ USB_DEVICE_INTERFACE_NUMBER(0x3765, 0x6001, 0) }, /* HDU */
	{ USB_DEVICE_INTERFACE_NUMBER(0x1738, 0x5211, 0) }, /* pre-firmware */
	{ }
};
MODULE_DEVICE_TABLE(usb, hdus_usb_table);

static int hdus_probe(struct usb_interface *intf,
		      const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *iface;
	struct usb_endpoint_descriptor *ep = NULL;
	struct hdus_dev *dev;
	unsigned int i;
	int ret;

	/* Pre-firmware device: upload firmware, then unbind; the device
	 * re-enumerates as 3275:6051 and probes again as HDUS. */
	if (le16_to_cpu(udev->descriptor.idVendor) == 0x1738 &&
	    le16_to_cpu(udev->descriptor.idProduct) == 0x5211)
		return hdus_fw_upload(udev);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_lock(&hdus_dev_lock);
	for (i = 0; i < HDUS_MAX_DEVICES; i++)
		if (!hdus_devs[i])
			break;
	if (i >= HDUS_MAX_DEVICES) {
		mutex_unlock(&hdus_dev_lock);
		dev_err(&udev->dev, "too many hdus devices\n");
		kfree(dev);
		return -ENOSPC;
	}
	hdus_devs[i] = dev; /* reserve slot */
	mutex_unlock(&hdus_dev_lock);

	dev->udev = udev;
	dev->intf = intf;
	dev->idx = i;
	dev->devno = MKDEV(MAJOR(hdus_devno_base), i);
	atomic_set(&dev->open_count, 0);
	atomic_set(&dev->streaming, 0);
	atomic_set(&dev->disconnected, 0);
	mutex_init(&dev->ctl_lock);
	mutex_init(&dev->lock);
	spin_lock_init(&dev->desc_lock);
	init_waitqueue_head(&dev->wq);

	ret = -ENODEV;
	/* Claim interface 0 alt 0 (recfriio claims it via usbfs). */
	iface = &intf->altsetting[0];
	if (iface->desc.bAlternateSetting != 0) {
		ret = usb_set_interface(udev, 0, 0);
		if (ret)
			goto out_free;
	}

	/* Find bulk-IN endpoint (expect 0x81, wMaxPacketSize 512). */
	for (i = 0; i < iface->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *d = &iface->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(d) &&
		    d->bEndpointAddress == 0x81) {
			ep = d;
			break;
		}
	}
	if (!ep) {
		dev_err(&udev->dev, "bulk-IN endpoint 0x81 not found\n");
		goto out_free;
	}
	dev->ep_in = ep->bEndpointAddress;
	dev->max_packet = le16_to_cpu(ep->wMaxPacketSize);
	dev_info(&udev->dev, "bulk-IN ep=0x%02x max_packet=%u\n",
		 dev->ep_in, dev->max_packet);

	dev->ctl_buf = kzalloc(0x200, GFP_KERNEL);
	if (!dev->ctl_buf) {
		ret = -ENOMEM;
		goto out_free;
	}

	ret = hdus_hw_init(dev);
	if (ret) {
		dev_err(&udev->dev, "hdus_hw_init failed: %d\n", ret);
		goto out_ctl;
	}
	dev->inited = true;

	/* r5 was set by hdus_hw_init (saddr detect). */
	hdus_descrambler_init(&dev->desc, dev->r5);

	ret = kfifo_alloc(&dev->fifo, HDUS_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto out_ctl;

	for (i = 0; i < HDUS_URB_COUNT; i++) {
		dev->pipe_buf[i] = kmalloc(HDUS_URB_SIZE, GFP_KERNEL);
		dev->out_buf[i] = kmalloc(HDUS_URB_SIZE, GFP_KERNEL);
		if (!dev->pipe_buf[i] || !dev->out_buf[i]) {
			ret = -ENOMEM;
			goto out_urb;
		}
		dev->urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!dev->urbs[i]) {
			ret = -ENOMEM;
			goto out_urb;
		}
		usb_fill_bulk_urb(dev->urbs[i], udev,
				  usb_rcvbulkpipe(udev,
				  dev->ep_in & USB_ENDPOINT_NUMBER_MASK),
				  dev->pipe_buf[i], HDUS_URB_SIZE,
				  hdus_urb_complete, dev);
	}

	cdev_init(&dev->cdev, &hdus_fops);
	dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dev->cdev, dev->devno, 1);
	if (ret)
		goto out_urb;

	dev->dev_node = device_create(hdus_class, &udev->dev, dev->devno, dev,
				      HDUS_DEV_NAME "%u", dev->idx);
	if (IS_ERR(dev->dev_node)) {
		ret = PTR_ERR(dev->dev_node);
		dev->dev_node = NULL;
		cdev_del(&dev->cdev);
		goto out_urb;
	}

	mutex_lock(&hdus_dev_lock);
	hdus_devs[dev->idx] = dev;
	mutex_unlock(&hdus_dev_lock);

	usb_set_intfdata(intf, dev);
	dev_info(&udev->dev, "hdusvideo%u registered\n", dev->idx);
	return 0;

out_urb:
	for (i = 0; i < HDUS_URB_COUNT; i++) {
		if (dev->urbs[i])
			usb_free_urb(dev->urbs[i]);
		kfree(dev->pipe_buf[i]);
		kfree(dev->out_buf[i]);
	}
	kfifo_free(&dev->fifo);
out_ctl:
	kfree(dev->ctl_buf);
out_free:
	mutex_lock(&hdus_dev_lock);
	if (hdus_devs[dev->idx] == dev)
		hdus_devs[dev->idx] = NULL;
	mutex_unlock(&hdus_dev_lock);
	kfree(dev);
	return ret;
}

static void hdus_disconnect(struct usb_interface *intf)
{
	struct hdus_dev *dev = usb_get_intfdata(intf);
	unsigned int i;

	if (!dev)
		return;

	atomic_set(&dev->disconnected, 1);
	hdus_stop_urbs(dev);
	wake_up_interruptible(&dev->wq);

	if (dev->dev_node) {
		device_destroy(hdus_class, dev->devno);
		dev->dev_node = NULL;
	}
	cdev_del(&dev->cdev);

	if (dev->inited)
		hdus_hw_shutdown(dev);

	for (i = 0; i < HDUS_URB_COUNT; i++) {
		if (dev->urbs[i])
			usb_free_urb(dev->urbs[i]);
		kfree(dev->pipe_buf[i]);
		kfree(dev->out_buf[i]);
	}
	kfifo_free(&dev->fifo);
	kfree(dev->ctl_buf);

	mutex_lock(&hdus_dev_lock);
	if (hdus_devs[dev->idx] == dev)
		hdus_devs[dev->idx] = NULL;
	mutex_unlock(&hdus_dev_lock);

	kfree(dev);
}

static int hdus_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct hdus_dev *dev = usb_get_intfdata(intf);

	if (dev)
		hdus_stop_urbs(dev);
	return 0;
}

static int hdus_resume(struct usb_interface *intf)
{
	/* Nothing: next START_REC restarts the bulk URBs. */
	return 0;
}

static struct usb_driver hdus_driver = {
	.name = HDUS_NAME,
	.id_table = hdus_usb_table,
	.probe = hdus_probe,
	.disconnect = hdus_disconnect,
	.suspend = hdus_suspend,
	.resume = hdus_resume,
};

static int __init hdus_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&hdus_devno_base, 0, HDUS_MAX_DEVICES,
				  HDUS_CLASS_NAME);
	if (ret)
		return ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	hdus_class = class_create(HDUS_CLASS_NAME);
#else
	hdus_class = class_create(THIS_MODULE, HDUS_CLASS_NAME);
#endif
	if (IS_ERR(hdus_class)) {
		ret = PTR_ERR(hdus_class);
		hdus_class = NULL;
		unregister_chrdev_region(hdus_devno_base, HDUS_MAX_DEVICES);
		return ret;
	}

	ret = usb_register(&hdus_driver);
	if (ret) {
		class_destroy(hdus_class);
		hdus_class = NULL;
		unregister_chrdev_region(hdus_devno_base, HDUS_MAX_DEVICES);
		return ret;
	}
	return 0;
}

static void __exit hdus_exit(void)
{
	usb_deregister(&hdus_driver);
	class_destroy(hdus_class);
	hdus_class = NULL;
	unregister_chrdev_region(hdus_devno_base, HDUS_MAX_DEVICES);
}

module_init(hdus_init);
module_exit(hdus_exit);

MODULE_AUTHOR("hdus_drv contributors");
MODULE_DESCRIPTION("SKNET MonsterTV HDUS USB driver");
MODULE_LICENSE("GPL");
