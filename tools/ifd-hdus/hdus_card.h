/* SPDX-License-Identifier: GPL-2.0-or-later */
/* hdus_card.h — HDUS internal B-CAS card slot.
 *
 * Transport: kernel-backed via /dev/hdusvideoX ioctls when the video node
 * exists (kernel driver owns USB, tuner+card usable simultaneously), with
 * a usbfs fallback when the video node is absent. Implements the
 * BonDriver_HDUS SendCard/ConnectCard protocol documented in
 * ../../docs/card_protocol_sendcard.md (no card_protocol_connect.md exists in
 * this repo; ConnectCard behaviour is taken from sendcard doc section 5 plus
 * the init tail quoted there).
 */
#ifndef HDUS_CARD_H
#define HDUS_CARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HDUS_CARD_DEFAULT_USB_PATH "/dev/bus/usb/002/008"
#define HDUS_CARD_ENV_PATH "HDUS_USB_PATH"

#define HDUS_CARD_DEFAULT_VIDEO_PATH "/dev/hdusvideo0"
#define HDUS_CARD_ENV_VIDEO_PATH "HDUS_VIDEO_PATH"

/* Transport actually selected by hdus_card_open(). */
#define HDUS_CARD_TRANSPORT_NONE   0
#define HDUS_CARD_TRANSPORT_KERNEL 1
#define HDUS_CARD_TRANSPORT_USBFS  2

/* Presence masks: obj+0x4e9 / obj+0x4ea in BonDriver_HDUS.dll.
 * Values are UNKNOWN (no card_protocol_connect.md in this repo, and the
 * DLL bytes are not initialised in any source we have). They are 0x00
 * (placeholder) which selects the fallback presence heuristic in
 * hdus_card.c: R(0x04) read twice, stable, and != 0xff. To use the real
 * DLL masks once they are recovered, set these two values only — the
 * DLL mask logic in card_presence_check() then takes over. */
#define HDUS_CARD_MASK_PRESENT 0x01 /* obj+0x4ea present mask (BonDriver RE) */
#define HDUS_CARD_MASK_READY   0x80 /* obj+0x4e9 ready mask (BonDriver RE) */

/* Real card ATR, as collected by hdus_card_connect(). Matches the
 * HDUS_CARD_GET_ATR ioctl payload (../../hdus_ioctl.h). */
#define HDUS_CARD_ATR_MAX 33

typedef struct hdus_card_atr {
	uint32_t len;
	uint8_t atr[HDUS_CARD_ATR_MAX];
} hdus_card_atr_t;

/* Error codes (BonDriver SendCard convention, docs/card_protocol_sendcard.md) */
#define HDUS_CARD_OK            0
#define HDUS_CARD_ERR_IO       (-1)
#define HDUS_CARD_ERR_NOT_READY (-2) /* reader not ready */
#define HDUS_CARD_ERR_NOT_PRESENT (-3) /* card not present */
#define HDUS_CARD_ERR_TX       (-6)  /* transmit failed / data error */

/* Open the card transport:
 *   primary: $HDUS_VIDEO_PATH -> /dev/hdusvideo0; if open fails with
 *            ENOENT (kernel module not loaded), fall back to usbfs:
 *            claim interface 0, run the Hdus.cpp UsbProcBegin power/tuner
 *            init (ASIE powered via the gpio 0x11ff sequence) and the saddr
 *            detect (req 0x02, wValue 0x0900|saddr, 0x5a then 0x4a).
 * usb_path is the usbfs fallback path, may be NULL -> $HDUS_USB_PATH ->
 * HDUS_CARD_DEFAULT_USB_PATH. If usb_path names a video node
 * (/dev/hdusvideo*), it is used directly as the video path (usbfs path
 * then falls back to env/default).
 * Returns 0 on success, negative on failure. May be called once. */
int hdus_card_open(const char *usb_path);

/* Release interface 0 (usbfs transport) and close the transport fd.
 * Safe to call when not open. */
void hdus_card_close(void);

/* Transport selected by the most recent hdus_card_open():
 * HDUS_CARD_TRANSPORT_NONE before open/after close,
 * HDUS_CARD_TRANSPORT_KERNEL for the ioctl path,
 * HDUS_CARD_TRANSPORT_USBFS for the fallback path. */
int hdus_card_transport(void);

/* Presence poll R(0x04) + mask/fallback logic, then the ConnectCard init
 * tail: W(0x00,0x0c) W(0x01,1) W(0x02,5) W(0x38,0) W(0x3a,0) W(0x40,0)
 * W(0x41,0x83) W(0x42,0x80) W(0x43,0x7f) W(0x44,0x7c) W(0x00,0x0a),
 * sleep 200ms, state byte = 0.
 * Returns 0 ok, -2 reader-not-ready, -3 card-not-present, -1 io error. */
int hdus_card_connect(void);

/* Copy the ATR captured by the last hdus_card_connect() into atr (card
 * bit order, e.g. 3B F0 12 ...), returning its length, or 0 when none
 * was collected. Kernel transport reads it from the driver's cache (no
 * USB traffic). */
int hdus_card_atr(uint8_t *atr, int max);

/* SendCard semantics: build frame 00 <state> <len> req [len extra 00],
 * sbox-transform + XOR checksum, chunked FIFO write, poll, length probe
 * len = sbox[R(0x42)]+4, chunked read, sbox-decode + checksum verify.
 * extra may be NULL when extra_len == 0.
 * Returns response length (>=0, as in the DLL, i.e. including the trailing
 * checksum byte position) or a negative HDUS_CARD_ERR_* code. */
int hdus_card_transmit(const uint8_t *req, int req_len,
		       const uint8_t *extra, int extra_len,
		       uint8_t *resp, int resp_max);

/* 1 if the R(0x04) presence check passes (mask logic, or fallback
 * heuristic when the masks are still placeholders), else 0. */
int hdus_card_present(void);

#ifdef __cplusplus
}
#endif

#endif /* HDUS_CARD_H */
