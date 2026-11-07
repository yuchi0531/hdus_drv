/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hdus_b25.c - TS filter: B25 (Multi2) decode via PC/SC B-CAS card
 *
 * The HDUS kernel driver outputs descrambled (ASIE XOR/DES) but still
 * Multi2/B25-encrypted TS from /dev/hdusvideoX.  B25 decoding is done
 * here in userspace via PC/SC + libaribb25, exactly like
 * `recfriio --b25` / `recpt1 --b25`.
 *
 * Usage: hdus_b25 [--round N=4] [--strip] [--emm] [--reader NAME]
 *                [-i infile|-] [-o outfile|-]
 *        (default stdin -> stdout)
 *
 * Startup replicates recbond/decoder.c b25_startup():
 *   create_arib_std_b25(), set_multi2_round(round), set_strip(strip),
 *   set_emm_proc(emm), create_b_cas_card(), bcas->init() (SCard* via
 *   winscard.h, T1), set_b_cas_card().  Then a put/get loop with
 *   ARIB_STD_B25_BUFFER { uint8_t *data; int32_t size; } (arib_std_b25.h);
 *   one get() drains the decoder's output buffer, so one get per put.
 *
 * README / limitation:
 *   libaribb25's B_CAS_CARD has NO API to select a named reader:
 *   init_b_cas_card() (b_cas_card.c) lists readers and uses the first
 *   that connects.  --reader is accepted for CLI compatibility and a
 *   warning is printed, but the card is still auto-selected (first
 *   reader).  To use a second reader, pass it through via pcscd /
 *   unplug the first reader, or patch b_cas_card.c.
 *
 * Fallback (like recpt1 recording encrypted on decode failure):
 *   - b25_startup fails (e.g. no card): print error, COPY input to
 *     output unmodified, exit 3.
 *   - put/get fails mid-stream: print code, write the current chunk
 *     unmodified, copy the remainder unmodified, exit 3.
 *   So exit 3 always means "output is NOT B25-decoded (still encrypted
 *   copy)"; exit 0 means decoded (or empty input).
 *
 * Pipeline:
 *   hdus_test (from /dev/hdusvideo0) | hdus_b25 | out.ts
 *   # or: hdus_test ... -o - | hdus_b25 -o out.ts
 *
 * Build (ARIB25_DIR defaults to ~/projects/recfriio/arib25):
 *   gcc -O2 -Wall -I ~/projects/recfriio/arib25 tools/hdus_b25.c \
 *       ~/projects/recfriio/arib25/arib_std_b25.c \
 *       ~/projects/recfriio/arib25/b_cas_card.c \
 *       ~/projects/recfriio/arib25/multi2.c \
 *       ~/projects/recfriio/arib25/ts_section_parser.c \
 *       $(pkg-config --cflags --libs libpcsclite) -lm -o /tmp/hdus_b25
 * (td.c is deliberately omitted: it is the upstream standalone test
 * program with its own main(), not a library object.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arib_std_b25.h"
#include "arib_std_b25_error_code.h"
#include "b_cas_card.h"
#include "b_cas_card_error_code.h"

#define CHUNK_SIZE 65536
#define TS_PACKET 188

struct b25_ctx {
	ARIB_STD_B25 *b25;
	B_CAS_CARD *bcas;
};

static const char *arib_error_name(int code)
{
	switch (code) {
	case ARIB_STD_B25_ERROR_INVALID_PARAM:
		return "ARIB_STD_B25_ERROR_INVALID_PARAM";
	case ARIB_STD_B25_ERROR_NO_ENOUGH_MEMORY:
		return "ARIB_STD_B25_ERROR_NO_ENOUGH_MEMORY";
	case ARIB_STD_B25_ERROR_NON_TS_INPUT_STREAM:
		return "ARIB_STD_B25_ERROR_NON_TS_INPUT_STREAM";
	case ARIB_STD_B25_ERROR_NO_PAT_IN_HEAD_16M:
		return "ARIB_STD_B25_ERROR_NO_PAT_IN_HEAD_16M";
	case ARIB_STD_B25_ERROR_NO_PMT_IN_HEAD_32M:
		return "ARIB_STD_B25_ERROR_NO_PMT_IN_HEAD_32M";
	case ARIB_STD_B25_ERROR_NO_ECM_IN_HEAD_32M:
		return "ARIB_STD_B25_ERROR_NO_ECM_IN_HEAD_32M";
	case ARIB_STD_B25_ERROR_EMPTY_B_CAS_CARD:
		return "ARIB_STD_B25_ERROR_EMPTY_B_CAS_CARD";
	case ARIB_STD_B25_ERROR_INVALID_B_CAS_STATUS:
		return "ARIB_STD_B25_ERROR_INVALID_B_CAS_STATUS";
	case ARIB_STD_B25_ERROR_ECM_PROC_FAILURE:
		return "ARIB_STD_B25_ERROR_ECM_PROC_FAILURE";
	case ARIB_STD_B25_ERROR_DECRYPT_FAILURE:
		return "ARIB_STD_B25_ERROR_DECRYPT_FAILURE";
	case ARIB_STD_B25_ERROR_PAT_PARSE_FAILURE:
		return "ARIB_STD_B25_ERROR_PAT_PARSE_FAILURE";
	case ARIB_STD_B25_ERROR_PMT_PARSE_FAILURE:
		return "ARIB_STD_B25_ERROR_PMT_PARSE_FAILURE";
	case ARIB_STD_B25_ERROR_ECM_PARSE_FAILURE:
		return "ARIB_STD_B25_ERROR_ECM_PARSE_FAILURE";
	case ARIB_STD_B25_ERROR_CAT_PARSE_FAILURE:
		return "ARIB_STD_B25_ERROR_CAT_PARSE_FAILURE";
	case ARIB_STD_B25_ERROR_EMM_PARSE_FAILURE:
		return "ARIB_STD_B25_ERROR_EMM_PARSE_FAILURE";
	case ARIB_STD_B25_ERROR_EMM_PROC_FAILURE:
		return "ARIB_STD_B25_ERROR_EMM_PROC_FAILURE";
	case ARIB_STD_B25_WARN_UNPURCHASED_ECM:
		return "ARIB_STD_B25_WARN_UNPURCHASED_ECM";
	case ARIB_STD_B25_WARN_TS_SECTION_ID_MISSMATCH:
		return "ARIB_STD_B25_WARN_TS_SECTION_ID_MISSMATCH";
	case ARIB_STD_B25_WARN_BROKEN_TS_SECTION:
		return "ARIB_STD_B25_WARN_BROKEN_TS_SECTION";
	default:
		return "ARIB_STD_B25_ERROR_UNKNOWN";
	}
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

/* b25_startup(): exact pattern of recbond decoder.c.
 * is_bcas_out tells the caller which error table names code_out
 * (B_CAS_CARD and ARIB_STD_B25 codes overlap numerically, e.g. -3). */
static struct b25_ctx *b25_startup(int round, int strip, int emm,
				   const char **err_out, int *code_out,
				   int *is_bcas_out)
{
	struct b25_ctx *ctx;
	int code;

	*err_out = NULL;
	*code_out = 0;
	*is_bcas_out = 0;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		*err_out = "calloc failed";
		*code_out = ARIB_STD_B25_ERROR_NO_ENOUGH_MEMORY;
		return NULL;
	}

	ctx->b25 = create_arib_std_b25();
	if (!ctx->b25) {
		*err_out = "create_arib_std_b25 failed";
		*code_out = ARIB_STD_B25_ERROR_NO_ENOUGH_MEMORY;
		goto fail;
	}

	code = ctx->b25->set_multi2_round(ctx->b25, round);
	if (code < 0) {
		*err_out = "set_multi2_round failed";
		*code_out = code;
		goto fail;
	}

	code = ctx->b25->set_strip(ctx->b25, strip);
	if (code < 0) {
		*err_out = "set_strip failed";
		*code_out = code;
		goto fail;
	}

	code = ctx->b25->set_emm_proc(ctx->b25, emm);
	if (code < 0) {
		*err_out = "set_emm_proc failed";
		*code_out = code;
		goto fail;
	}

	ctx->bcas = create_b_cas_card();
	if (!ctx->bcas) {
		*err_out = "create_b_cas_card failed";
		*code_out = B_CAS_CARD_ERROR_NO_ENOUGH_MEMORY;
		*is_bcas_out = 1;
		goto fail;
	}
	code = ctx->bcas->init(ctx->bcas);
	if (code < 0) {
		*err_out = "bcas->init failed";
		*code_out = code;
		*is_bcas_out = 1;
		fprintf(stderr, "hdus_b25: bcas->init failed: %d (%s)\n",
			code, bcas_error_name(code));
		goto fail;
	}

	code = ctx->b25->set_b_cas_card(ctx->b25, ctx->bcas);
	if (code < 0) {
		*err_out = "set_b_cas_card failed";
		*code_out = code;
		goto fail;
	}

	return ctx;

fail:
	if (ctx->b25)
		ctx->b25->release(ctx->b25);
	if (ctx->bcas)
		ctx->bcas->release(ctx->bcas);
	free(ctx);
	return NULL;
}

static void b25_shutdown(struct b25_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->b25)
		ctx->b25->release(ctx->b25);
	if (ctx->bcas)
		ctx->bcas->release(ctx->bcas);
	free(ctx);
}

static FILE *open_input(const char *path)
{
	FILE *f;

	if (!path || !strcmp(path, "-"))
		return stdin;
	f = fopen(path, "rb");
	return f;
}

static FILE *open_output(const char *path)
{
	FILE *f;

	if (!path || !strcmp(path, "-"))
		return stdout;
	f = fopen(path, "wb");
	return f;
}

/* Copy in -> out unmodified.  Returns 0 on success, -1 on I/O error.
 * If first_chunk/first_len are given, they are written first (the chunk
 * whose put() just failed, so no input bytes are lost). */
static int copy_fallback(FILE *in, FILE *out,
			 const uint8_t *first_chunk, size_t first_len,
			 uint64_t *total_out)
{
	static uint8_t buf[CHUNK_SIZE];

	if (first_chunk && first_len > 0) {
		if (fwrite(first_chunk, 1, first_len, out) != first_len)
			return -1;
		*total_out += first_len;
	}
	for (;;) {
		size_t n = fread(buf, 1, sizeof(buf), in);

		if (n > 0) {
			if (fwrite(buf, 1, n, out) != n)
				return -1;
			*total_out += n;
		}
		if (n < sizeof(buf)) {
			if (ferror(in))
				return -1;
			break;
		}
	}
	return 0;
}

static void print_stats(const char *mode, uint64_t in_bytes,
			uint64_t out_bytes)
{
	fprintf(stderr,
		"hdus_b25: mode=%s in_bytes=%llu out_bytes=%llu "
		"packets~%llu (out/%d)\n",
		mode,
		(unsigned long long)in_bytes,
		(unsigned long long)out_bytes,
		(unsigned long long)(out_bytes / TS_PACKET),
		TS_PACKET);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--round N] [--strip] [--emm] [--reader NAME] "
		"[-i infile|-] [-o outfile|-]\n"
		"\n"
		"B25 (Multi2) decode filter via PC/SC B-CAS + libaribb25.\n"
		"Defaults: stdin -> stdout, --round 4, no strip, no emm proc.\n"
		"\n"
		"  --round N    Multi2 rounds (default 4)\n"
		"  --strip      strip mode (set_strip(1))\n"
		"  --emm        EMM processing on (set_emm_proc(1))\n"
		"  --reader NAME accepted but IGNORED except for a warning:\n"
		"               libaribb25 B_CAS_CARD init() always auto-selects\n"
		"               the first connectable PC/SC reader (no API to\n"
		"               pass a reader name through create_b_cas_card).\n"
		"  -i infile    input file, or - / omitted for stdin\n"
		"  -o outfile   output file, or - / omitted for stdout\n"
		"\n"
		"Exit: 0 = decoded, 3 = startup/mid-stream failure, output is an\n"
		"      unmodified (still B25-encrypted) copy, 1 = I/O/usage error.\n"
		"\n"
		"Pipeline: hdus_test (from /dev/hdusvideo0) | hdus_b25 | out.ts\n",
		prog);
}

int main(int argc, char **argv)
{
	int round = 4, strip = 0, emm = 0;
	const char *reader_name = NULL;
	const char *in_path = NULL, *out_path = NULL;
	FILE *in = NULL, *out = NULL;
	struct b25_ctx *ctx = NULL;
	const char *err = NULL;
	int err_code = 0, err_is_bcas = 0;
	uint64_t in_bytes = 0, out_bytes = 0;
	static uint8_t inbuf[CHUNK_SIZE];
	int i, rc = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--round") && i + 1 < argc) {
			round = atoi(argv[++i]);
		} else if (!strncmp(argv[i], "--round=", 8)) {
			round = atoi(argv[i] + 8);
		} else if (!strcmp(argv[i], "--strip")) {
			strip = 1;
		} else if (!strcmp(argv[i], "--emm")) {
			emm = 1;
		} else if (!strcmp(argv[i], "--reader") && i + 1 < argc) {
			reader_name = argv[++i];
		} else if (!strncmp(argv[i], "--reader=", 9)) {
			reader_name = argv[i] + 9;
		} else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			in_path = argv[++i];
		} else if (!strncmp(argv[i], "-i", 2) && argv[i][2]) {
			in_path = argv[i] + 2;
		} else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			out_path = argv[++i];
		} else if (!strncmp(argv[i], "-o", 2) && argv[i][2]) {
			out_path = argv[i] + 2;
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
			"hdus_b25: warning: --reader %s ignored: libaribb25 "
			"B_CAS_CARD init() auto-selects the first connectable "
			"reader\n", reader_name);

	in = open_input(in_path);
	if (!in) {
		fprintf(stderr, "hdus_b25: cannot open input %s\n",
			in_path ? in_path : "-");
		return 1;
	}
	out = open_output(out_path);
	if (!out) {
		fprintf(stderr, "hdus_b25: cannot open output %s\n",
			out_path ? out_path : "-");
		if (in != stdin)
			fclose(in);
		return 1;
	}

	ctx = b25_startup(round, strip, emm, &err, &err_code, &err_is_bcas);
	if (!ctx) {
		/* No card / startup failure: copy unmodified (exit 3). */
		fprintf(stderr,
			"hdus_b25: %s: %d (%s); copying input unmodified "
			"(still B25-encrypted)\n",
			err ? err : "b25_startup failed", err_code,
			err_is_bcas ? bcas_error_name(err_code)
				    : arib_error_name(err_code));
		if (copy_fallback(in, out, NULL, 0, &out_bytes) != 0) {
			fprintf(stderr, "hdus_b25: copy fallback I/O error\n");
			rc = 1;
		} else {
			in_bytes = out_bytes;
			rc = 3;
		}
		print_stats("fallback-copy(startup failed)", in_bytes,
			    out_bytes);
		goto done;
	}

	for (;;) {
		size_t n = fread(inbuf, 1, sizeof(inbuf), in);
		{
			ARIB_STD_B25_BUFFER sbuf, dbuf;
			int code;

			if (n == 0) {
				if (ferror(in)) {
					fprintf(stderr,
						"hdus_b25: read error\n");
					rc = 1;
				}
				break;
			}
			in_bytes += n;

			sbuf.data = inbuf;
			sbuf.size = (int32_t)n;
			code = ctx->b25->put(ctx->b25, &sbuf);
			if (code < 0) {
				fprintf(stderr,
					"hdus_b25: b25->put failed: %d (%s); "
					"falling back to copy for remainder\n",
					code, arib_error_name(code));
				if (copy_fallback(in, out, inbuf, n,
						  &out_bytes) != 0) {
					fprintf(stderr,
						"hdus_b25: fallback copy "
						"I/O error\n");
					rc = 1;
				} else {
					rc = 3;
				}
				print_stats("fallback-copy(put failed)",
					    in_bytes, out_bytes);
				b25_shutdown(ctx);
				ctx = NULL;
				goto done;
			}
			if (code > 0)
				fprintf(stderr,
					"hdus_b25: b25->put warning: %d (%s)\n",
					code, arib_error_name(code));

			dbuf.data = NULL;
			dbuf.size = 0;
			code = ctx->b25->get(ctx->b25, &dbuf);
			if (code < 0) {
				fprintf(stderr,
					"hdus_b25: b25->get failed: %d (%s); "
					"falling back to copy for remainder\n",
					code, arib_error_name(code));
				if (copy_fallback(in, out, NULL, 0,
						  &out_bytes) != 0) {
					fprintf(stderr,
						"hdus_b25: fallback copy "
						"I/O error\n");
					rc = 1;
				} else {
					rc = 3;
				}
				print_stats("fallback-copy(get failed)",
					    in_bytes, out_bytes);
				b25_shutdown(ctx);
				ctx = NULL;
				goto done;
			}
			if (code > 0)
				fprintf(stderr,
					"hdus_b25: b25->get warning: %d (%s)\n",
					code, arib_error_name(code));

			if (dbuf.data && dbuf.size > 0) {
				if (fwrite(dbuf.data, 1, (size_t)dbuf.size,
					   out) != (size_t)dbuf.size) {
					fprintf(stderr,
						"hdus_b25: write error\n");
					rc = 1;
					b25_shutdown(ctx);
					ctx = NULL;
					goto done;
				}
				out_bytes += (uint64_t)dbuf.size;
			}
		}
	}

	/* EOF: flush + final get, exactly like decoder.c b25_finish(). */
	{
		ARIB_STD_B25_BUFFER dbuf;
		int code = ctx->b25->flush(ctx->b25);

		if (code < 0) {
			fprintf(stderr, "hdus_b25: b25->flush failed: %d (%s)\n",
				code, arib_error_name(code));
		} else {
			dbuf.data = NULL;
			dbuf.size = 0;
			code = ctx->b25->get(ctx->b25, &dbuf);
			if (code < 0)
				fprintf(stderr,
					"hdus_b25: final b25->get failed: %d (%s)\n",
					code, arib_error_name(code));
			else if (dbuf.data && dbuf.size > 0) {
				if (fwrite(dbuf.data, 1, (size_t)dbuf.size,
					   out) != (size_t)dbuf.size) {
					fprintf(stderr,
						"hdus_b25: write error\n");
					rc = 1;
				} else {
					out_bytes += (uint64_t)dbuf.size;
				}
			}
		}
	}
	print_stats("decoded", in_bytes, out_bytes);
	b25_shutdown(ctx);
	ctx = NULL;

done:
	if (out && out != stdout)
		fclose(out);
	else if (out)
		fflush(out);
	if (in && in != stdin)
		fclose(in);
	return rc;
}
