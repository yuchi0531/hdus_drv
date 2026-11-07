/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bcas_check.c - pure PC/SC + libaribb25 B-CAS diagnostic
 *
 * Usage: bcas_check [--reader NAME] [--verbose]
 *
 * Phase 1: direct PC/SC probe (winscard.h / pcsclite):
 *          SCardEstablishContext(SCARD_SCOPE_USER), SCardListReaders,
 *          SCardConnect(SCARD_SHARE_SHARED, SCARD_PROTOCOL_T1), SCardStatus.
 * Phase 2: libaribb25 B_CAS_CARD API:
 *          create_b_cas_card() -> init() -> get_init_status() -> get_id()
 *          -> release().  init() itself does SCardEstablishContext /
 *          SCardListReaders and tries each reader in order with
 *          SCardConnect(T1) + the B-CAS initial-setting command
 *          (0x90 0x30 0x00 0x00 0x00), caching B_CAS_INIT_STATUS.
 *
 * README / limitation:
 *   libaribb25's B_CAS_CARD has NO API to select a named reader:
 *   init_b_cas_card() (b_cas_card.c) iterates SCardListReaders() in
 *   order and uses the first reader that connects.  --reader therefore
 *   only affects the Phase-1 direct PC/SC probe; Phase 2 still
 *   auto-selects the first connectable reader.  A warning is printed
 *   when --reader is given.
 *
 *   There is no public pcsclite pcsc_strerror() (that symbol is a
 *   Windows winscard thing), so PC/SC LONG codes are named via the
 *   small table below (pcsclite.h values) + printed as hex.
 *
 * Exit codes: 0 = B-CAS card responded, 1 = error / no card response,
 *             2 = no PC/SC readers at all.
 *
 * Build (ARIB25_DIR defaults to ~/projects/recfriio/arib25, override
 * with make ARIB25_DIR=... or by editing the -I / source paths):
 *   gcc -O2 -Wall -I ~/projects/recfriio/arib25 tools/bcas_check.c \
 *       ~/projects/recfriio/arib25/b_cas_card.c \
 *       ~/projects/recfriio/arib25/multi2.c \
 *       ~/projects/recfriio/arib25/arib_std_b25.c \
 *       ~/projects/recfriio/arib25/ts_section_parser.c \
 *       $(pkg-config --cflags --libs libpcsclite) -lm -o /tmp/bcas_check
 * (b_cas_card.c alone would suffice for this tool; the other arib25
 * objects are listed so the same command works for hdus_b25.  td.c is
 * deliberately omitted: it is the upstream standalone test program
 * with its own main(), not a library object - the Makefile's OBJS list
 * includes it only because that Makefile builds the td test binary.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <winscard.h>

#include "b_cas_card.h"
#include "b_cas_card_error_code.h"

static int g_verbose;

static void print_hex(const char *label, const uint8_t *p, size_t n)
{
	size_t i;

	printf("%s:", label);
	for (i = 0; i < n; i++)
		printf(" %02x", p[i]);
	printf("\n");
}

struct scard_name { LONG code; const char *name; };

static const char *scard_error_name(LONG code)
{
	static const struct scard_name tbl[] = {
#ifdef SCARD_S_SUCCESS
		{ SCARD_S_SUCCESS, "SCARD_S_SUCCESS" },
#endif
#ifdef SCARD_E_CANCELLED
		{ SCARD_E_CANCELLED, "SCARD_E_CANCELLED" },
#endif
#ifdef SCARD_E_INVALID_HANDLE
		{ SCARD_E_INVALID_HANDLE, "SCARD_E_INVALID_HANDLE" },
#endif
#ifdef SCARD_E_INVALID_PARAMETER
		{ SCARD_E_INVALID_PARAMETER, "SCARD_E_INVALID_PARAMETER" },
#endif
#ifdef SCARD_E_INVALID_TARGET
		{ SCARD_E_INVALID_TARGET, "SCARD_E_INVALID_TARGET" },
#endif
#ifdef SCARD_E_NO_MEMORY
		{ SCARD_E_NO_MEMORY, "SCARD_E_NO_MEMORY" },
#endif
#ifdef SCARD_E_INSUFFICIENT_BUFFER
		{ SCARD_E_INSUFFICIENT_BUFFER, "SCARD_E_INSUFFICIENT_BUFFER" },
#endif
#ifdef SCARD_E_UNKNOWN_READER
		{ SCARD_E_UNKNOWN_READER, "SCARD_E_UNKNOWN_READER" },
#endif
#ifdef SCARD_E_TIMEOUT
		{ SCARD_E_TIMEOUT, "SCARD_E_TIMEOUT" },
#endif
#ifdef SCARD_E_SHARING_VIOLATION
		{ SCARD_E_SHARING_VIOLATION, "SCARD_E_SHARING_VIOLATION" },
#endif
#ifdef SCARD_E_NO_SMARTCARD
		{ SCARD_E_NO_SMARTCARD, "SCARD_E_NO_SMARTCARD" },
#endif
#ifdef SCARD_E_UNKNOWN_CARD
		{ SCARD_E_UNKNOWN_CARD, "SCARD_E_UNKNOWN_CARD" },
#endif
#ifdef SCARD_E_PROTO_MISMATCH
		{ SCARD_E_PROTO_MISMATCH, "SCARD_E_PROTO_MISMATCH" },
#endif
#ifdef SCARD_E_NOT_READY
		{ SCARD_E_NOT_READY, "SCARD_E_NOT_READY" },
#endif
#ifdef SCARD_E_INVALID_VALUE
		{ SCARD_E_INVALID_VALUE, "SCARD_E_INVALID_VALUE" },
#endif
#ifdef SCARD_E_READER_UNAVAILABLE
		{ SCARD_E_READER_UNAVAILABLE, "SCARD_E_READER_UNAVAILABLE" },
#endif
#ifdef SCARD_E_NO_SERVICE
		{ SCARD_E_NO_SERVICE, "SCARD_E_NO_SERVICE" },
#endif
#ifdef SCARD_E_SERVICE_STOPPED
		{ SCARD_E_SERVICE_STOPPED, "SCARD_E_SERVICE_STOPPED" },
#endif
#ifdef SCARD_E_UNEXPECTED
		{ SCARD_E_UNEXPECTED, "SCARD_E_UNEXPECTED" },
#endif
#ifdef SCARD_E_NO_READERS_AVAILABLE
		{ SCARD_E_NO_READERS_AVAILABLE, "SCARD_E_NO_READERS_AVAILABLE" },
#endif
#ifdef SCARD_E_COMM_DATA_LOST
		{ SCARD_E_COMM_DATA_LOST, "SCARD_E_COMM_DATA_LOST" },
#endif
#ifdef SCARD_W_UNSUPPORTED_CARD
		{ SCARD_W_UNSUPPORTED_CARD, "SCARD_W_UNSUPPORTED_CARD" },
#endif
#ifdef SCARD_W_UNRESPONSIVE_CARD
		{ SCARD_W_UNRESPONSIVE_CARD, "SCARD_W_UNRESPONSIVE_CARD" },
#endif
#ifdef SCARD_W_UNPOWERED_CARD
		{ SCARD_W_UNPOWERED_CARD, "SCARD_W_UNPOWERED_CARD" },
#endif
#ifdef SCARD_W_RESET_CARD
		{ SCARD_W_RESET_CARD, "SCARD_W_RESET_CARD" },
#endif
#ifdef SCARD_W_REMOVED_CARD
		{ SCARD_W_REMOVED_CARD, "SCARD_W_REMOVED_CARD" },
#endif
	};

	size_t i;

	for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
		if (tbl[i].code == code)
			return tbl[i].name;
	}
	return "SCARD_UNKNOWN";
}

static const char *bcas_error_name(int code)
{
	switch (code) {
	case B_CAS_CARD_ERROR_INVALID_PARAMETER:
		return "B_CAS_CARD_ERROR_INVALID_PARAMETER";
	case B_CAS_CARD_ERROR_NOT_INITIALIZED:
		return "B_CAS_CARD_ERROR_NOT_INITIALIZED";
	case B_CAS_CARD_ERROR_NO_SMART_CARD_READER:
		return "B_CAS_CARD_ERROR_NO_SMART_CARD_READER";
	case B_CAS_CARD_ERROR_ALL_READERS_CONNECTION_FAILED:
		return "B_CAS_CARD_ERROR_ALL_READERS_CONNECTION_FAILED";
	case B_CAS_CARD_ERROR_NO_ENOUGH_MEMORY:
		return "B_CAS_CARD_ERROR_NO_ENOUGH_MEMORY";
	case B_CAS_CARD_ERROR_TRANSMIT_FAILED:
		return "B_CAS_CARD_ERROR_TRANSMIT_FAILED";
	default:
		return "B_CAS_CARD_ERROR_UNKNOWN";
	}
}

static void print_scard_error(const char *what, LONG ret)
{
	fprintf(stderr, "%s failed: 0x%08lx (%s)\n", what,
		(unsigned long)ret, scard_error_name(ret));
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--reader NAME] [--verbose]\n"
		"\n"
		"Pure PC/SC diagnostic for a B-CAS card reader.\n"
		"  --reader NAME  probe this reader in the direct PC/SC phase\n"
		"                 (libaribb25 B_CAS_CARD init() still auto-selects\n"
		"                 the first connectable reader - see header note)\n"
		"  --verbose      extra detail (ATR, hex dumps)\n"
		"\n"
		"Exit: 0 = B-CAS card responded, 1 = error/no card, 2 = no readers\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *reader_name = NULL;
	SCARDCONTEXT ctx = 0;
	LONG ret;
	DWORD len = 0;
	char *readers = NULL;
	char *target = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--reader") && i + 1 < argc) {
			reader_name = argv[++i];
		} else if (!strncmp(argv[i], "--reader=", 9)) {
			reader_name = argv[i] + 9;
		} else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) {
			g_verbose = 1;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	if (reader_name)
		fprintf(stderr,
			"note: libaribb25 B_CAS_CARD init() auto-selects the first "
			"connectable reader; --reader only affects the direct "
			"PC/SC probe\n");

	/* ---- Phase 1: direct PC/SC ---- */
	ret = SCardEstablishContext(SCARD_SCOPE_USER, NULL, NULL, &ctx);
	if (ret != SCARD_S_SUCCESS) {
		print_scard_error("SCardEstablishContext", ret);
		return 1;
	}
	printf("SCardEstablishContext: OK\n");

	ret = SCardListReaders(ctx, NULL, NULL, &len);
	if (ret != SCARD_S_SUCCESS) {
		/* pcsclite reports an empty list as this error, not len<=1 */
		if (ret == SCARD_E_NO_READERS_AVAILABLE) {
			printf("no PC/SC readers (card reader not visible on "
			       "USB? check lsusb / passthrough)\n");
			SCardReleaseContext(ctx);
			return 2;
		}
		print_scard_error("SCardListReaders(len)", ret);
		SCardReleaseContext(ctx);
		return 1;
	}
	if (len <= 1) {
		printf("no PC/SC readers (card reader not visible on USB? "
		       "check lsusb / passthrough)\n");
		SCardReleaseContext(ctx);
		return 2;
	}

	readers = malloc(len);
	if (!readers) {
		fprintf(stderr, "out of memory\n");
		SCardReleaseContext(ctx);
		return 1;
	}
	ret = SCardListReaders(ctx, NULL, readers, &len);
	if (ret != SCARD_S_SUCCESS) {
		print_scard_error("SCardListReaders", ret);
		free(readers);
		SCardReleaseContext(ctx);
		return 1;
	}

	printf("PC/SC readers:\n");
	{
		char *p = readers;
		int n = 0;

		while (p < readers + len && *p) {
			printf("  [%d] %s\n", n++, p);
			if (reader_name && !strcmp(p, reader_name))
				target = p;
			p += strlen(p) + 1;
		}
		if (n == 0) {
			printf("no PC/SC readers (card reader not visible on USB? "
			       "check lsusb / passthrough)\n");
			free(readers);
			SCardReleaseContext(ctx);
			return 2;
		}
	}

	if (reader_name && !target) {
		fprintf(stderr, "reader not found: %s\n", reader_name);
		free(readers);
		SCardReleaseContext(ctx);
		return 1;
	}
	if (!target)
		target = readers;

	printf("direct PC/SC probe: reader = %s\n", target);
	{
		SCARDHANDLE card = 0;
		DWORD proto = 0;

		ret = SCardConnect(ctx, target, SCARD_SHARE_SHARED,
				   SCARD_PROTOCOL_T1, &card, &proto);
		if (ret != SCARD_S_SUCCESS) {
			print_scard_error("SCardConnect(SHARED,T1)", ret);
			free(readers);
			SCardReleaseContext(ctx);
			return 1;
		}
		printf("SCardConnect: OK (protocol=0x%lx)\n",
		       (unsigned long)proto);

		if (g_verbose) {
			char state_name[64];
			DWORD state = 0, atrlen = 0;
			uint8_t atr[64];

			ret = SCardStatus(card, NULL, NULL, &state,
					  &proto, atr, &atrlen);
			if (ret == SCARD_S_SUCCESS) {
				snprintf(state_name, sizeof(state_name),
					 "0x%lx", (unsigned long)state);
				printf("SCardStatus: state=%s\n", state_name);
				print_hex("ATR", atr, atrlen);
			} else {
				print_scard_error("SCardStatus", ret);
			}
		}
		SCardDisconnect(card, SCARD_LEAVE_CARD);
	}

	/* ---- Phase 2: libaribb25 B_CAS_CARD ---- */
	{
		B_CAS_CARD *bcas;
		B_CAS_INIT_STATUS stat;
		B_CAS_ID id;
		int code;

		bcas = create_b_cas_card();
		if (!bcas) {
			fprintf(stderr, "create_b_cas_card failed\n");
			free(readers);
			SCardReleaseContext(ctx);
			return 1;
		}

		code = bcas->init(bcas);
		if (code < 0) {
			fprintf(stderr, "bcas->init failed: %d (%s)\n",
				code, bcas_error_name(code));
			bcas->release(bcas);
			free(readers);
			SCardReleaseContext(ctx);
			return 1;
		}
		printf("B_CAS_CARD init: OK\n");

		memset(&stat, 0, sizeof(stat));
		code = bcas->get_init_status(bcas, &stat);
		if (code < 0) {
			fprintf(stderr, "get_init_status failed: %d (%s)\n",
				code, bcas_error_name(code));
			bcas->release(bcas);
			free(readers);
			SCardReleaseContext(ctx);
			return 1;
		}
		/* B_CAS_INIT_STATUS fields (b_cas_card.h):
		 *   uint8_t system_key[32]; uint8_t init_cbc[8];
		 *   int64_t bcas_card_id; int32_t card_status;
		 *   int32_t ca_system_id;                       */
		printf("init_status: card_status=%d ca_system_id=%d "
		       "bcas_card_id=0x%012llx (%lld)\n",
		       stat.card_status, stat.ca_system_id,
		       (unsigned long long)stat.bcas_card_id,
		       (long long)stat.bcas_card_id);
		print_hex("init_status.system_key", stat.system_key,
			  sizeof(stat.system_key));
		print_hex("init_status.init_cbc", stat.init_cbc,
			  sizeof(stat.init_cbc));
		if (g_verbose)
			print_hex("init_status(raw struct)",
				  (const uint8_t *)&stat, sizeof(stat));

		memset(&id, 0, sizeof(id));
		code = bcas->get_id(bcas, &id);
		if (code < 0) {
			fprintf(stderr, "get_id failed: %d (%s)\n",
				code, bcas_error_name(code));
			bcas->release(bcas);
			free(readers);
			SCardReleaseContext(ctx);
			return 1;
		}
		/* B_CAS_ID fields (b_cas_card.h):
		 *   int64_t *data; int32_t count;
		 * Each data[i] is a 48-bit big-endian card ID word. */
		printf("card_id: count=%d\n", id.count);
		for (i = 0; i < id.count; i++)
			printf("  id[%d]=0x%012llx (%lld)\n", i,
			       (unsigned long long)id.data[i],
			       (long long)id.data[i]);
		if (g_verbose && id.count > 0)
			print_hex("card_id(raw int64 array)",
				  (const uint8_t *)id.data,
				  (size_t)id.count * sizeof(int64_t));

		bcas->release(bcas);
	}

	free(readers);
	SCardReleaseContext(ctx);
	printf("B-CAS card responded\n");
	return 0;
}
