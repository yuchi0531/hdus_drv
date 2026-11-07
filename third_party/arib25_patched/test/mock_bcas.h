/* HDUS test fixture: mock B_CAS_CARD (no PC/SC, no hardware).
 *
 * The mock records how many times the decoder really talked to the "card"
 * (proc_ecm) and derives the returned scramble key from the ECM payload
 * itself, so a wrongly skipped card call shows up as a different output TS
 * when the cache-enabled run is compared with the cache-disabled run.
 */
#ifndef MOCK_BCAS_H
#define MOCK_BCAS_H

#include "b_cas_card.h"

typedef struct {
	long      ecm_calls;      /* proc_ecm() invocations                */
	long      emm_calls;      /* proc_emm() invocations                */
	long      fail_at;        /* fail the N-th ecm call (0 = never)    */
	long      fail_count;     /* how many calls to fail (default 1)    */
	uint32_t  last_ecm_hash;  /* hash of last ECM seen by the "card"   */
} MOCK_BCAS_STATS;

extern B_CAS_CARD *create_mock_b_cas_card(MOCK_BCAS_STATS *stats);

/* FNV-1a 32, same function as ari_std_b25.c uses (kept local to the mock so
   the fixture does not depend on libaribb25 internals). */
extern uint32_t mock_fnv1a(const uint8_t *p, int len);

#endif /* MOCK_BCAS_H */
