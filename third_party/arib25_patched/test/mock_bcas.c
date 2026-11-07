/* HDUS test fixture: mock B_CAS_CARD (no PC/SC, no hardware). */
#include <stdlib.h>
#include <string.h>

#include "mock_bcas.h"

typedef struct {
	B_CAS_CARD       vtable;      /* must stay first */
	MOCK_BCAS_STATS *stats;
} MOCK_BCAS;

uint32_t mock_fnv1a(const uint8_t *p, int len)
{
	uint32_t h = 2166136261u;
	int i;

	for(i = 0; i < len; i++){
		h ^= (uint32_t)p[i];
		h *= 16777619u;
	}
	return h;
}

static void mock_release(void *bcas)
{
	free(bcas);
}

static int mock_init(void *bcas)
{
	(void)bcas;
	return 0;
}

static int mock_get_init_status(void *bcas, B_CAS_INIT_STATUS *stat)
{
	(void)bcas;
	if(stat == NULL){
		return -1;
	}
	memset(stat, 0, sizeof(*stat));
	/* canned Multi2 system key / init cbc */
	{
		int i;
		for(i = 0; i < 32; i++){
			stat->system_key[i] = (uint8_t)(0x11 * (i + 1));
		}
		for(i = 0; i < 8; i++){
			stat->init_cbc[i] = (uint8_t)(0xa0 + i);
		}
	}
	stat->bcas_card_id = 0x0123456789abcdefLL;
	stat->card_status  = 0;
	/* ISDB B-CAS CA_system_id is 0x0005; override for other fixtures */
	stat->ca_system_id = 0x0005;
	if(getenv("MOCK_CA_SYSTEM_ID") != NULL){
		stat->ca_system_id = (int32_t)strtol(getenv("MOCK_CA_SYSTEM_ID"), NULL, 0);
	}
	return 0;
}

static int mock_get_id(void *bcas, B_CAS_ID *dst)
{
	(void)bcas;
	if(dst != NULL){
		dst->data  = NULL;
		dst->count = 0;
	}
	return 0;
}

static int mock_get_pwr_on_ctrl(void *bcas, B_CAS_PWR_ON_CTRL_INFO *dst)
{
	(void)bcas;
	if(dst != NULL){
		dst->data  = NULL;
		dst->count = 0;
	}
	return 0;
}

static int mock_proc_ecm(void *bcas, B_CAS_ECM_RESULT *dst, uint8_t *src, int len)
{
	MOCK_BCAS *m;
	uint32_t h1;
	uint32_t h2;
	int i;

	m = (MOCK_BCAS *)bcas;
	m->stats->ecm_calls += 1;

	if( (m->stats->fail_at > 0) &&
	    (m->stats->ecm_calls >= m->stats->fail_at) &&
	    (m->stats->ecm_calls <  (m->stats->fail_at + m->stats->fail_count)) ){
		return -1; /* simulate a card error: must not be cached */
	}

	h1 = mock_fnv1a(src, len);
	h2 = mock_fnv1a(src, len) ^ 0x9e3779b9u;
	for(i = 0; i < 16; i++){
		h1 *= 16777619u;
		h2 ^= (uint32_t)(i + 7);
		h2 *= 16777619u;
		dst->scramble_key[i] = (uint8_t)((i & 1) ? (h2 >> 16) : (h1 >> 8));
	}
	dst->return_code = 0x0800; /* purchased */
	m->stats->last_ecm_hash = mock_fnv1a(src, len);
	return 0;
}

static int mock_proc_emm(void *bcas, uint8_t *src, int len)
{
	MOCK_BCAS *m;

	(void)src;
	(void)len;
	m = (MOCK_BCAS *)bcas;
	m->stats->emm_calls += 1;
	return 0;
}

B_CAS_CARD *create_mock_b_cas_card(MOCK_BCAS_STATS *stats)
{
	MOCK_BCAS *m;

	if(stats == NULL){
		return NULL;
	}
	m = (MOCK_BCAS *)calloc(1, sizeof(MOCK_BCAS));
	if(m == NULL){
		return NULL;
	}
	m->stats = stats;

	m->vtable.private_data     = m;
	m->vtable.release          = mock_release;
	m->vtable.init             = mock_init;
	m->vtable.get_init_status  = mock_get_init_status;
	m->vtable.get_id           = mock_get_id;
	m->vtable.get_pwr_on_ctrl  = mock_get_pwr_on_ctrl;
	m->vtable.proc_ecm         = mock_proc_ecm;
	m->vtable.proc_emm         = mock_proc_emm;

	return &(m->vtable);
}
