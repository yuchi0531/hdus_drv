/* HDUS test driver: feed a raw (scrambled) TS through the patched
 * libaribb25 decoder using a mock B_CAS_CARD, without any hardware.
 *
 *   ecm_cache_test INPUT.ts OUTPUT.ts [--ecm-pid N] [--pace-ms N] [--chunk N]
 *
 * --chunk N feeds the decoder in N byte put()/get() rounds, like the real
 * recording loop (tools/hdus_b25.c); the default is one 188 byte TS packet
 * per round.
 *
 * The decoder is arib_std_b25.c from this directory (patched) plus
 * multi2.c / ts_section_parser.c / b_cas_card.c (unmodified upstream code).
 * All card traffic goes to the mock in mock_bcas.c, so the number of
 * proc_ecm() calls reported here is exactly the number of "real" card
 * round-trips the patch would have cost on the HDUS (~1.6 s each).
 *
 * Output is written to OUTPUT.ts so that cache-on / cache-off runs can be
 * compared byte for byte (the mock derives the scramble key from the ECM
 * payload, so an incorrect cache hit would visibly corrupt the output).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arib_std_b25.h"
#include "mock_bcas.h"

#define TS_SIZE 188

static int32_t get_pid(const uint8_t *p)
{
	return ((p[1] & 0x1f) << 8) | p[2];
}

/* table_id of the section that starts in this packet, -1 if none */
static int section_table_id(const uint8_t *p, int *payload_offset)
{
	int afc;
	int off;
	int pointer;

	afc = (p[3] >> 4) & 0x03;
	if( ((p[1] >> 6) & 0x01) == 0 ){      /* payload_unit_start_indicator */
		return -1;
	}
	if( (afc & 0x01) == 0 ){
		return -1;
	}
	off = 4;
	if(afc & 0x02){
		off += 1 + p[4];
	}
	if(off >= TS_SIZE){
		return -1;
	}
	pointer = p[off];
	off += 1 + pointer;
	if(off >= TS_SIZE){
		return -1;
	}
	if(payload_offset != NULL){
		*payload_offset = off;
	}
	return p[off];
}

int main(int argc, char **argv)
{
	const char *in_path;
	const char *out_path;
	int32_t ecm_pid;
	int32_t pace_ms;
	int32_t chunk;
	int32_t auto_pid;
	int32_t pid;
	int32_t ttl_hint;

	long packets;
	long ecm_sections;
	long put_errors;
	long i;

	FILE *fi;
	FILE *fo;
	unsigned char *cbuf;
	size_t got;

	ARIB_STD_B25 *b25;
	B_CAS_CARD *bcas;
	ARIB_STD_B25_BUFFER in;
	ARIB_STD_B25_BUFFER out;

	MOCK_BCAS_STATS stats;
	struct timespec t0, t1;
	double elapsed;

	if(argc < 3){
		fprintf(stderr,
		        "usage: %s INPUT.ts OUTPUT.ts [--ecm-pid N] [--pace-ms N] "
		        "[--chunk N]\n",
		        argv[0]);
		return 2;
	}
	in_path  = argv[1];
	out_path = argv[2];
	ecm_pid  = -1;
	pace_ms  = 0;
	chunk    = TS_SIZE;

	for(i = 3; i + 1 < argc; i += 2){
		if(strcmp(argv[i], "--ecm-pid") == 0){
			ecm_pid = (int32_t)strtol(argv[i+1], NULL, 0);
		}else if(strcmp(argv[i], "--pace-ms") == 0){
			pace_ms = (int32_t)strtol(argv[i+1], NULL, 0);
		}else if(strcmp(argv[i], "--chunk") == 0){
			chunk = (int32_t)strtol(argv[i+1], NULL, 0);
		}else{
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	fi = fopen(in_path, "rb");
	if(fi == NULL){
		perror(in_path);
		return 1;
	}
	fo = fopen(out_path, "wb");
	if(fo == NULL){
		perror(out_path);
		fclose(fi);
		return 1;
	}

	memset(&stats, 0, sizeof(stats));
	if(getenv("MOCK_ECM_FAIL_AT") != NULL){
		stats.fail_at = strtol(getenv("MOCK_ECM_FAIL_AT"), NULL, 0);
		stats.fail_count = 1;
		if(getenv("MOCK_ECM_FAIL_COUNT") != NULL){
			stats.fail_count = strtol(getenv("MOCK_ECM_FAIL_COUNT"), NULL, 0);
		}
	}

	b25 = create_arib_std_b25();
	if(b25 == NULL){
		fprintf(stderr, "create_arib_std_b25 failed\n");
		return 1;
	}
	bcas = create_mock_b_cas_card(&stats);
	if(bcas == NULL){
		fprintf(stderr, "create_mock_b_cas_card failed\n");
		return 1;
	}
	b25->set_multi2_round(b25, 4);
	b25->set_b_cas_card(b25, bcas);

	packets     = 0;
	ecm_sections = 0;
	put_errors  = 0;
	auto_pid    = -1;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	if(chunk < TS_SIZE){
		chunk = TS_SIZE;
	}
	chunk -= (chunk % TS_SIZE);
	cbuf = (unsigned char *)malloc((size_t)chunk);
	if(cbuf == NULL){
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	while( (got = fread(cbuf, 1, (size_t)chunk, fi)) > 0 ){
		size_t k;
		int pace_here = 0;

		for(k = 0; (k + TS_SIZE) <= got; k += TS_SIZE){
			uint8_t *pp = cbuf + k;

			packets += 1;
			if(pp[0] != 0x47){
				continue;
			}
			pid = get_pid(pp);
			if(section_table_id(pp, NULL) == 0x82){   /* ECM_S */
				ecm_sections += 1;
				if(auto_pid < 0){
					auto_pid = pid;
				}
				if( (ecm_pid < 0) || (pid == ecm_pid) ){
					pace_here = 1;
				}
			}
		}
		if( (pace_here) && (pace_ms > 0) ){
			usleep((useconds_t)pace_ms * 1000);
		}

		in.data = cbuf;
		in.size = (int32_t)got;
		{
			int rc = b25->put(b25, &in);
			if(rc < 0){
				if(put_errors == 0){
					fprintf(stderr, "first put error: %d\n", rc);
				}
				put_errors += 1;
			}
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

	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	ttl_hint = -1;
	if(getenv("ARIB25_ECM_CACHE_MS") != NULL){
		ttl_hint = (int32_t)strtol(getenv("ARIB25_ECM_CACHE_MS"), NULL, 0);
	}

	printf("ts_packets=%ld\n",            packets);
	printf("ecm_sections_in_ts=%ld\n",    ecm_sections);
	printf("ecm_pid=0x%04x\n",            (unsigned)((ecm_pid >= 0) ? ecm_pid : auto_pid));
	printf("put_errors=%ld\n",            put_errors);
	printf("card_proc_ecm_calls=%ld\n",   stats.ecm_calls);
	printf("card_proc_emm_calls=%ld\n",   stats.emm_calls);
	printf("env_ARIB25_ECM_CACHE_MS=%d\n", ttl_hint);
	printf("chunk_bytes=%d\n",             chunk);
	printf("wall_seconds=%.3f\n",         elapsed);

	free(cbuf);
	fclose(fi);
	fclose(fo);
	b25->release(b25);
	bcas->release(bcas);
	return (put_errors > 0) ? 3 : 0;
}
