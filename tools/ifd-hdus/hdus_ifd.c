/* SPDX-License-Identifier: GPL-2.0-or-later */
/* hdus_ifd.c — PC/SC IFD handler exposing the HDUS B-CAS slot as the
 * reader "HDUS B-CAS".
 *
 * HEADER API NOTE (adaptation): this system ships pcsc-lite's
 * /usr/include/PCSC/ifdhandler.h, which defines IFDHandler v3.0 — the
 * exported symbols are IFDHCreateChannelByName / IFDHCreateChannel /
 * IFDHCloseChannel / IFDHGetCapabilities / IFDHSetCapabilities /
 * IFDHSetProtocolParameters / IFDHPowerICC / IFDHTransmitToICC /
 * IFDHICCPresence / IFDHControl. It does NOT declare libccid-style
 * IFDInit / IFDProcessSegment / IFDExit / IFDStatusSIM / IFDListSlot /
 * IFDListReader symbols, and pcscd dlopen()s the IFDH* names directly,
 * so those libccid names are intentionally not implemented here.
 * Mapping from the task's libccid naming to this header:
 *   IFDInit            -> IFDHCreateChannelByName (+ IFDHCreateChannel)
 *   IFDExit            -> IFDHCloseChannel
 *   IFDProcessSegment  -> IFDHTransmitToICC (APDU exchange) + IFDHControl (escape)
 *   IFDStatusSIM       -> IFDHICCPresence (+ IFDHPowerICC for power/reset)
 *   IFDListSlot/Reader -> not IFDHandler symbols in this header; the reader
 *                         name comes from reader.conf FRIENDLYNAME
 *                         (== HDUS_IFD_READER_NAME below) instead.
 */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <ifdhandler.h> /* pulls in <pcsclite.h> */
#include <reader.h> /* SCARD_ATTR_ATR_STRING (not included by ifdhandler.h) */
#include "hdus_card.h"

#define HDUS_IFD_READER_NAME "HDUS B-CAS"
#define HDUS_IFD_LOG(...) do { fprintf(stderr, "hdus_ifd: " __VA_ARGS__); \
			      fflush(stderr); } while (0)

/* The reset the driver performs hands back the card's real ATR
 * (3B F0 12 ... on a B-CAS card, see docs/asie5606-coax-reference.md).
 * A minimal synthetic T=1 ATR is used only when that was not collected:
 * pcsc-lite needs some ATR, else SCARD_PROTOCOL_T1 requests fail with
 * SCARD_E_PROTO_MISMATCH. */
static const uint8_t g_synth_atr[] = { 0x3B, 0x80, 0x01, 0xBA };

static uint8_t g_atr[MAX_ATR_SIZE];
static DWORD g_atr_len = 0;
static int g_opened = 0;
static int g_presence = 0;
static int g_presence_known = 0;

/* refresh_atr pulls the ATR collected by hdus_card_connect() and caches
 * it for IFDHGetCapabilities. */
static void refresh_atr(void)
{
	int n;

	if (!g_opened)
		return;
	n = hdus_card_atr(g_atr, (int)sizeof(g_atr));
	if (n > 0) {
		g_atr_len = (DWORD)n;
		HDUS_IFD_LOG("ATR from card (%d bytes)\n", n);
		return;
	}
	memcpy(g_atr, g_synth_atr, sizeof(g_synth_atr));
	g_atr_len = sizeof(g_synth_atr);
}

RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPSTR DeviceName)
{
	const char *path = NULL;
	(void)Lun;

	/* DEVICENAME from reader.conf: accept a usbfs path verbatim; anything
	 * else (usb:VID/PID style, empty) falls back to $HDUS_USB_PATH /
	 * the default device node inside hdus_card_open(). */
	if (DeviceName && DeviceName[0] == '/')
		path = DeviceName;

	if (hdus_card_open(path) != 0) {
		HDUS_IFD_LOG("IFDHCreateChannelByName: cannot open HDUS device\n");
		return IFD_COMMUNICATION_ERROR;
	}
	g_opened = 1;
	memset(g_atr, 0, sizeof(g_atr));
	g_atr_len = 0;

	/* Reader present even when no card is: pcsc convention is to succeed
	 * init and let IFDHICCPresence() report the absent card. */
	int cr = hdus_card_connect();
	refresh_atr();
	if (cr == 0)
		HDUS_IFD_LOG("reader \"%s\" ready, B-CAS card present\n",
			     HDUS_IFD_READER_NAME);
	else
		HDUS_IFD_LOG("reader \"%s\" ready, card not present "
			     "(hdus_card_connect=%d)\n",
			     HDUS_IFD_READER_NAME, cr);
	return IFD_SUCCESS;
}

RESPONSECODE IFDHCreateChannel(DWORD Lun, DWORD Channel)
{
	(void)Channel; /* no DEVICENAME available; use env/default path */
	return IFDHCreateChannelByName(Lun, NULL);
}

RESPONSECODE IFDHCloseChannel(DWORD Lun)
{
	(void)Lun;
	if (g_opened) {
		hdus_card_close();
		g_opened = 0;
	}
	g_presence_known = 0;
	return IFD_SUCCESS;
}

RESPONSECODE IFDHICCPresence(DWORD Lun)
{
	/* pcscd polls presence continuously. Each real check costs a
	 * tuner<->card mode switch (~1-2 s of lost TS), so probe the
	 * hardware only once per channel open and cache the answer:
	 * a B-CAS card stays seated for the whole session. */
	(void)Lun;
	if (!g_opened)
		return IFD_ICC_NOT_PRESENT;
	if (!g_presence_known) {
		g_presence = hdus_card_present() ? 1 : 0;
		g_presence_known = 1;
	}
	return g_presence ? IFD_SUCCESS : IFD_ICC_NOT_PRESENT;
}

RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action, PUCHAR Atr, PDWORD AtrLength)
{
	(void)Lun;
	if (Action == IFD_POWER_DOWN) {
		memset(g_atr, 0, sizeof(g_atr));
		g_atr_len = 0;
		if (Atr)
			memset(Atr, 0, MAX_ATR_SIZE);
		if (AtrLength)
			*AtrLength = 0;
		return IFD_SUCCESS;
	}
	if (Action != IFD_POWER_UP && Action != IFD_RESET)
		return IFD_NOT_SUPPORTED;
	if (!g_opened || !hdus_card_present()) {
		if (AtrLength)
			*AtrLength = 0;
		return IFD_ERROR_POWER_ACTION;
	}
	hdus_card_connect();
	refresh_atr();
	if (Atr)
		memcpy(Atr, g_atr, g_atr_len);
	if (AtrLength)
		*AtrLength = g_atr_len;
	return IFD_SUCCESS;
}

RESPONSECODE IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci,
				PUCHAR TxBuffer, DWORD TxLength,
				PUCHAR RxBuffer, PDWORD RxLength,
				PSCARD_IO_HEADER RecvPci)
{
	static uint8_t resp[4096];
	int resp_max, n;

	(void)Lun;
	HDUS_IFD_LOG("transmit: TxLength=%lu RxLength=%lu proto=%lu\n",
		     (unsigned long)TxLength,
		     RxLength ? (unsigned long)*RxLength : 0UL,
		     (unsigned long)SendPci.Protocol);
	/* Capture the caller's buffer size BEFORE clearing the out param:
	 * pcscd passes the max rapdu length in *RxLength (e.g. 65548). */
	resp_max = RxLength ? (int)*RxLength : 0;
	if (RxLength)
		*RxLength = 0;
	if (!g_opened)
		return IFD_COMMUNICATION_ERROR;
	if (!TxBuffer || TxLength == 0 || !RxBuffer || !RxLength)
		return IFD_COMMUNICATION_ERROR;
	HDUS_IFD_LOG("transmit: present=%d transport=%d\n",
		     g_opened ? hdus_card_present() : -9,
		     g_opened ? hdus_card_transport() : -9);
	if (!hdus_card_present())
		return IFD_ICC_NOT_PRESENT;

	/* libaribb25 (arib25/b_cas_card.c) sends one SCARD_PCI_T1 transmit
	 * per APDU, full APDU including header. ECM/EMM are built by
	 * setup_ecm_receive_command()/setup_emm_receive_command() as:
	 *   90 34 00 00 <len> <data...> 00   (len = data length, low byte)
	 * i.e. exactly the SendCard req+extra shape (4-byte header, then a
	 * length-prefixed extra block with NUL terminator). Split it back:
	 *   req   = first 4 bytes
	 *   extra = bytes[5 .. 5+len-1]  when byte[4]==len and last byte 0
	 * Everything else (90 30, 90 32, 90 80) is req = whole APDU. */
	const uint8_t *req = TxBuffer;
	int req_len = (int)TxLength;
	const uint8_t *extra = NULL;
	int extra_len = 0;

	if (TxLength >= 6 && TxBuffer[4] != 0 &&
	    (int)TxBuffer[4] + 6 == (int)TxLength &&
	    TxBuffer[TxLength - 1] == 0) {
		req = TxBuffer;
		req_len = 4;
		extra = TxBuffer + 5;
		extra_len = TxBuffer[4];
	}

	if (resp_max > (int)sizeof(resp))
		resp_max = (int)sizeof(resp);
	if (resp_max <= 0)
		return IFD_ERROR_INSUFFICIENT_BUFFER;
	n = hdus_card_transmit(req, req_len, extra, extra_len, resp, resp_max);
	if (n < 0) {
		*RxLength = 0;
		return n == HDUS_CARD_ERR_NOT_PRESENT ? IFD_ICC_NOT_PRESENT
						     : IFD_COMMUNICATION_ERROR;
	}
	if (n > resp_max) { /* cannot happen (resp_max clamp), be safe */
		*RxLength = 0;
		return IFD_ERROR_INSUFFICIENT_BUFFER;
	}
	memcpy(RxBuffer, resp, (size_t)n);
	*RxLength = (DWORD)n; /* rapdu = card response as returned by the card */
	if (RecvPci) {
		RecvPci->Protocol = SendPci.Protocol;
		RecvPci->Length = 0;
	}
	return IFD_SUCCESS;
}

RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag, PDWORD Length,
				 PUCHAR Value)
{
	(void)Lun;
	if (!Length || !Value)
		return IFD_ERROR_TAG;

	switch (Tag) {
	case TAG_IFD_ATR: /* 0x0303, mandatory */
	case SCARD_ATTR_ATR_STRING: /* same value via reader.h class */
		if (*Length < g_atr_len)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		memcpy(Value, g_atr, g_atr_len);
		*Length = g_atr_len;
		return IFD_SUCCESS;
	case TAG_IFD_SLOTS_NUMBER: /* 0x0FAE */
		if (*Length < 1)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		Value[0] = 1;
		*Length = 1;
		return IFD_SUCCESS;
	case TAG_IFD_SIMULTANEOUS_ACCESS: /* 0x0FAF */
		if (*Length < 1)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		Value[0] = 1;
		*Length = 1;
		return IFD_SUCCESS;
	case TAG_IFD_THREAD_SAFE: /* 0x0FAD */
		if (*Length < 1)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		Value[0] = 0; /* single-slot, serialised by hdus_card mutex */
		*Length = 1;
		return IFD_SUCCESS;
	default:
		return IFD_ERROR_TAG;
	}
}

RESPONSECODE IFDHSetCapabilities(DWORD Lun, DWORD Tag, DWORD Length,
				 PUCHAR Value)
{
	(void)Lun; (void)Tag; (void)Length; (void)Value;
	return IFD_ERROR_TAG;
}

RESPONSECODE IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol,
				       UCHAR Flags, UCHAR PTS1,
				       UCHAR PTS2, UCHAR PTS3)
{
	(void)Lun; (void)Flags; (void)PTS1; (void)PTS2; (void)PTS3;
	/* B-CAS APDUs run as T=1 (SCARD_PCI_T1 in libaribb25). */
	if (Protocol != SCARD_PROTOCOL_T1 && Protocol != SCARD_PROTOCOL_ANY)
		return IFD_PROTOCOL_NOT_SUPPORTED;
	return IFD_SUCCESS;
}

RESPONSECODE IFDHControl(DWORD Lun, DWORD dwControlCode, PUCHAR TxBuffer,
			 DWORD TxLength, PUCHAR RxBuffer, DWORD RxLength,
			 LPDWORD pdwBytesReturned)
{
	(void)Lun; (void)TxBuffer; (void)TxLength; (void)RxBuffer;
	(void)RxLength;
	if (pdwBytesReturned)
		*pdwBytesReturned = 0;
	/* Escape/control: only code 0 is accepted (no-op), per spec. */
	if (dwControlCode != 0)
		return IFD_ERROR_TAG;
	return IFD_SUCCESS;
}
