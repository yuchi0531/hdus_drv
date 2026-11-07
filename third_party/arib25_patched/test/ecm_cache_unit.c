/* HDUS white-box unit test for the ECM result cache.
 *
 * The patched translation unit is #included directly so the test can drive
 * proc_ecm()/DECRYPTOR_ELEM without any B-CAS hardware or PC/SC reader.
 * `dec->ecm->reset()` clears the section parser's "last committed section"
 * dedupe state, which is exactly how an otherwise byte-identical ECM reaches
 * proc_ecm() again (re-sync / host side parser reset).
 *
 *   ecm_cache_unit hash                      FNV-1a vectors
 *   ecm_cache_unit env                       prints env derived settings
 *   ecm_cache_unit logic   [TS]              hit / miss / key / pid checks
 *   ecm_cache_unit ttl-expiry [TS]           needs ARIB25_ECM_CACHE_MS > 0
 *   ecm_cache_unit fail-path [TS]            needs MOCK_ECM_FAIL_AT=2
 *   ecm_cache_unit stats   [TS]              whole file through the public
 *                                            API, then dumps per decryptor
 *                                            parser + cache counters
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mock_bcas.h"

#include "../arib_std_b25.c"   /* white box: proc_ecm() + DECRYPTOR_ELEM */

#define CHECK(cond, ...) do{ \
	if(cond){ \
		printf("  PASS  "); printf(__VA_ARGS__); printf("\n"); \
	}else{ \
		failures += 1; \
		printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); \
	} \
}while(0)

/* ------------------------------------------------------------------ */
/* ECM_S sections straight out of a real scrambled TS                  */
/* ------------------------------------------------------------------ */
typedef struct {
	uint8_t data[512];
	int     len;
} ECM_SEC;

#if ARIB25_ECM_CACHE
static int failures;
#define MAX_ECM 64

static int load_ecm_sections(const char *path, ECM_SEC *out, int max)
{
	FILE *f;
	uint8_t pkt[188];
	int count = 0;
	uint8_t acc[512];
	int acc_len = 0;
	int acc_need = 0;
	int32_t ecm_pid = -1;

	f = fopen(path, "rb");
	if(f == NULL){
		printf("  SKIP  cannot open %s\n", path);
		return 0;
	}

	while( (count < max) && (fread(pkt, 1, 188, f) == 188) ){
		int afc;
		int off;
		int ptr;
		uint8_t *body;
		int n;

		if(pkt[0] != 0x47){
			continue;
		}
		afc = (pkt[3] >> 4) & 0x03;
		if( (ecm_pid < 0) || (((pkt[1] & 0x1f) << 8) | pkt[2]) != ecm_pid ){
			if( ((pkt[1] >> 6) & 0x01) && (afc & 0x01) ){
				off = 4;
				if(afc & 0x02){
					off += 1 + pkt[4];
				}
				if(off < 188){
					ptr = pkt[off];
					if( (off + 1 + ptr < 188) && (pkt[off+1+ptr] == 0x82) ){
						ecm_pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
					}
				}
			}
			continue;
		}
		if(afc & 0x01){
			off = 4;
			if(afc & 0x02){
				off += 1 + pkt[4];
			}
			if(off >= 188){
				continue;
			}
			ptr = pkt[off];
			off += 1;
			if( (off + ptr) >= 188){
				continue;
			}
			if( (ptr > 0) && (acc_need > 0) ){
				/* finish the pending section first */
				n = acc_need - acc_len;
				if(n > ptr){
					n = ptr;
				}
				memcpy(acc + acc_len, pkt + off, n);
				acc_len += n;
				if(acc_len >= acc_need){
					memcpy(out[count].data, acc, acc_need);
					out[count].len = acc_need;
					count += 1;
				}
				acc_need = 0;
			}
			off += ptr;
			if(off >= 188){
				continue;
			}
			body = pkt + off;
			n = 188 - off;
			if(body[0] != 0x82){
				acc_need = 0;
				continue;
			}
			acc_need = 3 + (((body[1] & 0x0f) << 8) | body[2]);
			if(acc_need > 512){
				acc_need = 0;
				continue;
			}
			if(acc_need <= n){
				if( (count < max) && !((acc_need > 0) && (acc_need > 500)) ){
					memcpy(out[count].data, body, acc_need);
					out[count].len = acc_need;
					count += 1;
				}
				acc_need = 0;
			}else{
				memcpy(acc, body, n);
				acc_len = n;
			}
		}else if(acc_need > 0){
			uint8_t *p = pkt + 4;
			int n = 184;

			if(afc & 0x02){
				p += 1 + pkt[4];
				n = 188 - (p - pkt);
			}
			if(n > 0){
				int need = acc_need - acc_len;
				if(need > n){
					need = n;
				}
				memcpy(acc + acc_len, p, need);
				acc_len += need;
				if(acc_len >= acc_need){
					if(count < max){
						memcpy(out[count].data, acc, acc_need);
						out[count].len = acc_need;
						count += 1;
					}
					acc_need = 0;
				}
			}
		}
	}
	fclose(f);
	return count;
}

/* table_id of the section starting in this TS packet, -1 if none */
static int section_table_id(const uint8_t *p, int *payload_offset)
{
	int afc;
	int off;
	int pointer;

	afc = (p[3] >> 4) & 0x03;
	if( ((p[1] >> 6) & 0x01) == 0 ){
		return -1;
	}
	if( (afc & 0x01) == 0 ){
		return -1;
	}
	off = 4;
	if(afc & 0x02){
		off += 1 + p[4];
	}
	if(off >= 188){
		return -1;
	}
	pointer = p[off];
	off += 1 + pointer;
	if(off >= 188){
		return -1;
	}
	if(payload_offset != NULL){
		*payload_offset = off;
	}
	return p[off];
}

/* ------------------------------------------------------------------ */
/* feed one ECM_S section into dec->ecm (pointer_field 0)              */
/* ------------------------------------------------------------------ */
static int feed_section(TS_SECTION_PARSER *parser, int32_t pid,
                        const ECM_SEC *sec, int cc)
{
	uint8_t pkt[188];
	TS_HEADER hdr;

	memset(pkt, 0xff, sizeof(pkt));
	pkt[0] = 0x47;
	pkt[1] = 0x40 | ((pid >> 8) & 0x1f);      /* PUSI */
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10 | (cc & 0x0f);              /* payload only */
	pkt[4] = 0x00;                            /* pointer_field */
	memcpy(pkt + 5, sec->data, sec->len);

	memset(&hdr, 0, sizeof(hdr));
	extract_ts_header(&hdr, pkt);

	return parser->put(parser, &hdr, pkt + 4, 188 - 4);
}

static int dec_feed(DECRYPTOR_ELEM *dec, const ECM_SEC *sec, int cc)
{
	return feed_section(dec->ecm, dec->ecm_pid, sec, cc);
}

static DECRYPTOR_ELEM *make_decryptor(int32_t pid)
{
	DECRYPTOR_ELEM *dec;

	dec = (DECRYPTOR_ELEM *)calloc(1, sizeof(DECRYPTOR_ELEM));
	if(dec == NULL){
		return NULL;
	}
	dec->ref     = 1;
	dec->ecm_pid = pid;
	dec->ecm     = create_ts_section_parser();
	if(dec->ecm == NULL){
		free(dec);
		return NULL;
	}
	return dec;
}

static void drop_decryptor(DECRYPTOR_ELEM *dec)
{
	if(dec == NULL){
		return;
	}
	if(dec->m2 != NULL){
		dec->m2->release(dec->m2);
	}
	if(dec->ecm != NULL){
		dec->ecm->release(dec->ecm);
	}
	free(dec);
}

/* run one ECM through the decoder: 1 if the card was called */
static int run_ecm(DECRYPTOR_ELEM *dec, B_CAS_CARD *bcas,
                   const ECM_SEC *sec, int cc, long *calls)
{
	long before;

	before = *calls;
	CHECK(dec_feed(dec, sec, cc) >= 0, "parser accepted ECM (%d bytes)", sec->len);
	CHECK(dec->ecm->get_count(dec->ecm) > 0, "section queued for proc_ecm");
	(void)proc_ecm(dec, bcas, 4);
	return (*calls != before) ? 1 : 0;
}

#endif /* ARIB25_ECM_CACHE : helpers only used by the cache tests */

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
	const char *mode;
	const char *ts;
	ECM_SEC *secs;
	int nsec;

	if(argc < 2){
		fprintf(stderr, "usage: %s hash|env|logic|ttl-expiry|fail-path [TS]\n",
		        argv[0]);
		return 2;
	}
	mode = argv[1];
	ts   = (argc > 2) ? argv[2] : "/tmp/raw20_nopcsc.ts";

#if !ARIB25_ECM_CACHE
	(void)mode;
	(void)ts;
	(void)secs;
	(void)nsec;
	printf("  SKIP  built with ARIB25_ECM_CACHE=0: cache helpers are not "
	       "compiled in\n");
	return 0;
#else
	if(strcmp(mode, "hash") == 0){
		CHECK(hdus_ecm_hash((const uint8_t *)"", 0) == 0x811c9dc5u,
		      "FNV-1a(\"\")      = 0x811c9dc5");
		CHECK(hdus_ecm_hash((const uint8_t *)"a", 1) == 0xe40c292cu,
		      "FNV-1a(\"a\")     = 0xe40c292c");
		CHECK(hdus_ecm_hash((const uint8_t *)"abc", 3) == 0x1a47e90bu,
		      "FNV-1a(\"abc\")   = 0x1a47e90b");
		{
			ECM_SEC s1;
			ECM_SEC s2;
			memset(&s1, 0xa5, sizeof(s1));
			memcpy(&s2, &s1, sizeof(s1));
			s1.len = 120;
			s2.len = 120;
			CHECK(hdus_ecm_hash(s1.data, s1.len) == hdus_ecm_hash(s2.data, s2.len),
			      "identical payload -> identical hash");
			s2.data[7] ^= 0x01;
			CHECK(hdus_ecm_hash(s1.data, s1.len) != hdus_ecm_hash(s2.data, s2.len),
			      "1 bit flipped     -> different hash");
		}
		return failures ? 1 : 0;
	}

	if(strcmp(mode, "env") == 0){
		printf("ttl_ms=%d\n", hdus_ecm_cache_ttl_ms());
		printf("debug=%d\n",  hdus_ecm_cache_debug_on());
		return 0;
	}

	secs = (ECM_SEC *)calloc(MAX_ECM, sizeof(ECM_SEC));
	if(secs == NULL){
		return 1;
	}
	nsec = load_ecm_sections(ts, secs, MAX_ECM);
	if(nsec < 2){
		printf("  SKIP  need at least 2 ECM sections in %s (got %d)\n", ts, nsec);
		free(secs);
		return 0;
	}
	printf("fixture: %d ECM_S sections from %s (first %d bytes)\n",
	       nsec, ts, secs[0].len);

	if(strcmp(mode, "logic") == 0){
		MOCK_BCAS_STATS st;
		B_CAS_CARD *card;
		DECRYPTOR_ELEM *dec;
		int32_t pid = 0x1140;
		int32_t ttl = hdus_ecm_cache_ttl_ms();
		int first_diff;
		int cc = 0;
		int call;

		/* find the first section that differs from secs[0], i.e. a real
		   ECM content change in the stream */
		for(first_diff = 1; first_diff < nsec; first_diff++){
			if( (secs[first_diff].len != secs[0].len) ||
			    (memcmp(secs[first_diff].data, secs[0].data, secs[0].len) != 0) ){
				break;
			}
		}
		printf("ttl_ms=%d  pid=0x%04x  distinct ECM at index %d\n",
		       ttl, (unsigned)pid, first_diff);

		memset(&st, 0, sizeof(st));
		card = create_mock_b_cas_card(&st);
		dec  = make_decryptor(pid);

		/* 1: first ECM -> card call */
		call = run_ecm(dec, card, &secs[0], cc, &st.ecm_calls);
		cc += 1;
		CHECK(call == 1, "cold ECM #0            -> card call  (calls=%ld)", st.ecm_calls);
		CHECK(dec->ecm_cache_valid == 1, "cache armed after success");
		CHECK(dec->m2 != NULL, "multi2 key installed");

		/* 2: identical ECM after a parser reset -> cache hit (ttl > 0) */
		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[0], cc, &st.ecm_calls);
		cc += 1;
		if(ttl > 0){
			CHECK(call == 0, "identical repeat       -> CACHE HIT (calls=%ld hits=%llu)",
			      st.ecm_calls, (unsigned long long)dec->ecm_cache_hit);
		}else{
			CHECK(call == 1, "identical repeat, cache off -> card call (calls=%ld)",
			      st.ecm_calls);
		}

		/* 3: different ECM content -> must reach the card */
		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[first_diff], cc, &st.ecm_calls);
		cc += 1;
		CHECK(call == 1, "changed ECM content    -> card call  (calls=%ld)", st.ecm_calls);

		/* 4: repeat of the new content -> hit again */
		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[first_diff], cc, &st.ecm_calls);
		cc += 1;
		if(ttl > 0){
			CHECK(call == 0, "repeat after change    -> CACHE HIT (calls=%ld hits=%llu)",
			      st.ecm_calls, (unsigned long long)dec->ecm_cache_hit);
		}else{
			CHECK(call == 1, "repeat after change, cache off -> card call (calls=%ld)",
			      st.ecm_calls);
		}

		/* 5: same ECM payload but different ECM pid -> must not hit */
		dec->ecm_pid = pid + 1;
		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[first_diff], cc, &st.ecm_calls);
		cc += 1;
		CHECK(call == 1, "same payload, other pid -> card call (calls=%ld)", st.ecm_calls);
		dec->ecm_pid = pid;

		/* 6: the multi2 object survives a hit (key reuse precondition) */
		CHECK(dec->m2 != NULL, "multi2 still installed at end");

		drop_decryptor(dec);
		card->release(card);
		free(secs);
		return failures ? 1 : 0;
	}

	if(strcmp(mode, "ttl-expiry") == 0){
		MOCK_BCAS_STATS st;
		B_CAS_CARD *card;
		DECRYPTOR_ELEM *dec;
		int32_t ttl = hdus_ecm_cache_ttl_ms();
		int call;

		if(ttl <= 0){
			printf("  SKIP  set ARIB25_ECM_CACHE_MS>0 for this test\n");
			free(secs);
			return 0;
		}
		printf("ttl_ms=%d\n", ttl);
		memset(&st, 0, sizeof(st));
		card = create_mock_b_cas_card(&st);
		dec  = make_decryptor(0x1140);

		call = run_ecm(dec, card, &secs[0], 0, &st.ecm_calls);
		CHECK(call == 1, "cold ECM              -> card call (calls=%ld)", st.ecm_calls);

		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[0], 1, &st.ecm_calls);
		CHECK(call == 0, "within TTL            -> CACHE HIT (calls=%ld)", st.ecm_calls);

		dec->ecm->reset(dec->ecm);
		usleep((useconds_t)(ttl + 120) * 1000);
		call = run_ecm(dec, card, &secs[0], 2, &st.ecm_calls);
		CHECK(call == 1, "TTL expired (%d ms) -> card call (calls=%ld)", ttl, st.ecm_calls);

		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[0], 3, &st.ecm_calls);
		CHECK(call == 0, "hit again after refresh -> CACHE HIT (calls=%ld)", st.ecm_calls);

		drop_decryptor(dec);
		card->release(card);
		free(secs);
		return failures ? 1 : 0;
	}

	if(strcmp(mode, "fail-path") == 0){
		const char *fa = getenv("MOCK_ECM_FAIL_AT");
		MOCK_BCAS_STATS st;
		B_CAS_CARD *card;
		DECRYPTOR_ELEM *dec;
		int call;

		if(fa == NULL){
			printf("  SKIP  set MOCK_ECM_FAIL_AT=2 for this test\n");
			free(secs);
			return 0;
		}
		int idx = 0;

		while( (idx < nsec) && (secs[idx].len == secs[0].len) &&
		       (memcmp(secs[idx].data, secs[0].data, secs[0].len) == 0) ){
			idx += 1;   /* first ECM with a different payload */
		}
		if(idx >= nsec){
			printf("  SKIP  fixture has only one distinct ECM\n");
			free(secs);
			return 0;
		}

		memset(&st, 0, sizeof(st));
		st.fail_at    = strtol(fa, NULL, 0);
		st.fail_count = 1;
		card = create_mock_b_cas_card(&st);
		dec  = make_decryptor(0x1140);

		/* ECM A -> card call #1, result cached */
		call = run_ecm(dec, card, &secs[0], 0, &st.ecm_calls);
		CHECK(call == 1, "ECM A                  -> card call #1 (calls=%ld)",
		      st.ecm_calls);

		/* ECM B (different content, cache miss) -> card call #2 fails */
		dec->ecm->reset(dec->ecm);
		CHECK(dec_feed(dec, &secs[idx], 1) >= 0, "parser accepted ECM B");
		CHECK(proc_ecm(dec, card, 4) < 0, "proc_ecm reports the card error");
		CHECK(st.ecm_calls == 2, "ECM B, forced failure  -> card call #2 (calls=%ld)",
		      st.ecm_calls);

		/* the multi2 key was cleared by the failure, so the cache entry for
		   ECM A must have been dropped: ECM A has to go to the card again
		   (re-arming the key) instead of being "hit" with no key installed */
		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[0], 2, &st.ecm_calls);
		CHECK(call == 1, "ECM A after failure    -> card call #3 (stale key dropped, "
		      "calls=%ld)", st.ecm_calls);

		/* ECM B retried: still a miss, now succeeds and replaces the entry */
		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[idx], 3, &st.ecm_calls);
		CHECK(call == 1, "ECM B retry            -> card call #4 (calls=%ld)",
		      st.ecm_calls);

		dec->ecm->reset(dec->ecm);
		call = run_ecm(dec, card, &secs[idx], 4, &st.ecm_calls);
		CHECK(call == 0, "ECM B again            -> CACHE HIT (calls=%ld)", st.ecm_calls);

		drop_decryptor(dec);
		card->release(card);
		free(secs);
		return failures ? 1 : 0;
	}

	if(strcmp(mode, "stats") == 0){
		/* Whole file through the public API (exactly like tools/hdus_b25.c),
		   then peek at the internal counters to show where the ECM packets
		   are absorbed. */
		MOCK_BCAS_STATS st;
		B_CAS_CARD *card;
		ARIB_STD_B25 *b25;
		ARIB_STD_B25_PRIVATE_DATA *prv;
		ARIB_STD_B25_BUFFER in;
		ARIB_STD_B25_BUFFER out;
		DECRYPTOR_ELEM *dec;
		FILE *fi;
		FILE *fo;
		uint8_t *cbuf;
		size_t got;
		long packets = 0;
		long ecm_pkts = 0;
		long long total_bytes = 0;
		int chunk = 48 * 1024;
		char *tmp_path = "/tmp/ecm_cache_stats_out.ts";

		memset(&st, 0, sizeof(st));
		card = create_mock_b_cas_card(&st);
		b25  = create_arib_std_b25();
		if( (card == NULL) || (b25 == NULL) ){
			printf("  FAIL  allocation\n");
			return 1;
		}
		b25->set_multi2_round(b25, 4);
		b25->set_b_cas_card(b25, card);

		chunk -= (chunk % 188);   /* keep the reader on TS packet bounds */
		fi = fopen(ts, "rb");
		fo = fopen(tmp_path, "wb");
		cbuf = (uint8_t *)malloc((size_t)chunk);
		if( (fi == NULL) || (fo == NULL) || (cbuf == NULL) ){
			printf("  SKIP  cannot open fixture or output\n");
			return 0;
		}
		while( (got = fread(cbuf, 1, (size_t)chunk, fi)) > 0 ){
			size_t k;

			total_bytes += (long long)got;
			for(k = 0; (k + 188) <= got; k += 188){
				uint8_t *pp = cbuf + k;
				if(pp[0] != 0x47){
					continue;
				}
				packets += 1;
				if(section_table_id(pp, NULL) == 0x82){
					ecm_pkts += 1;
				}
			}
			in.data = cbuf;
			in.size = (int32_t)got;
			if(b25->put(b25, &in) < 0){
				/* counted below via mock */
			}
			while(1){
				memset(&out, 0, sizeof(out));
				if(b25->get(b25, &out) < 0){
					break;
				}
				if( (out.data == NULL) || (out.size <= 0) ){
					break;
				}
				fwrite(out.data, 1, (size_t)out.size, fo);
			}
		}
		b25->flush(b25);
		while(1){
			memset(&out, 0, sizeof(out));
			if(b25->get(b25, &out) < 0){
				break;
			}
			if( (out.data == NULL) || (out.size <= 0) ){
				break;
			}
			fwrite(out.data, 1, (size_t)out.size, fo);
		}
		fclose(fi);
		fclose(fo);
		free(cbuf);

		printf("bytes_read=%lld\n", total_bytes);
		printf("ts_packets=%ld\n", packets);
		printf("ecm_packets_in_ts=%ld\n", ecm_pkts);
		printf("card_proc_ecm_calls=%ld\n", st.ecm_calls);

		prv = (ARIB_STD_B25_PRIVATE_DATA *)(b25->private_data);
		dec = prv->decrypt.head;
		while(dec != NULL){
			TS_SECTION_PARSER_STAT ps;

			memset(&ps, 0, sizeof(ps));
			if(dec->ecm != NULL){
				dec->ecm->get_stat(dec->ecm, &ps);
			}
			printf("decryptor pid=0x%04x ecm_total=%lld ecm_unique=%lld "
			       "ecm_error=%lld cache_hit=%llu cache_miss=%llu m2=%s\n",
			       (unsigned)dec->ecm_pid,
			       (long long)ps.total, (long long)ps.unique,
			       (long long)ps.error,
			       (unsigned long long)dec->ecm_cache_hit,
			       (unsigned long long)dec->ecm_cache_miss,
			       (dec->m2 != NULL) ? "yes" : "no");
			dec = (DECRYPTOR_ELEM *)(dec->next);
		}

		b25->release(b25);
		card->release(card);
		free(secs);
		return failures ? 1 : 0;
	}

	fprintf(stderr, "unknown mode %s\n", mode);
	free(secs);
	return 2;
#endif /* ARIB25_ECM_CACHE */
}
