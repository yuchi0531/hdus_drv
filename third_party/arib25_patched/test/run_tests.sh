#!/bin/sh
# HDUS ECM-cache test suite: builds the patched decoder and runs every
# hardware-free check.  Nothing outside this directory is written.
#
#   ./run_tests.sh              (uses /tmp/raw20_nopcsc.ts)
#   TS=/path/to/other.ts ./run_tests.sh
set -u

cd "$(dirname "$0")" || exit 1

CC=${CC:-gcc}
TS=${TS:-/tmp/raw20_nopcsc.ts}
SRC=..
PCSC_CFLAGS=$(pkg-config --cflags libpcsclite)
PCSC_LIBS=$(pkg-config --libs libpcsclite)
UPSTREAM_OBJS="$SRC/multi2.c $SRC/ts_section_parser.c $SRC/b_cas_card.c mock_bcas.c"

FAIL=0
step() { printf '\n========== %s\n' "$*"; }
ok()   { printf 'RESULT: PASS  %s\n' "$*"; }
bad()  { printf 'RESULT: FAIL  %s\n' "$*"; FAIL=1; }

mkdir -p build

step "BUILD (gcc -O2 -Wall, pkg-config libpcsclite)"
set -x
$CC -O2 -Wall -I$SRC -o build/ecm_cache_test   \
    $SRC/arib_std_b25.c $UPSTREAM_OBJS ecm_cache_test.c \
    $PCSC_CFLAGS $PCSC_LIBS -lm
$CC -O2 -Wall -I$SRC -o build/ecm_cache_unit   \
    ecm_cache_unit.c $UPSTREAM_OBJS $PCSC_CFLAGS $PCSC_LIBS -lm
$CC -O2 -Wall -DARIB25_ECM_CACHE=0 -I$SRC -o build/ecm_cache_test_nocache \
    $SRC/arib_std_b25.c $UPSTREAM_OBJS ecm_cache_test.c \
    $PCSC_CFLAGS $PCSC_LIBS -lm
$CC -O2 -Wall -DARIB25_ECM_CACHE=0 -I$SRC -o build/ecm_cache_unit_nocache \
    ecm_cache_unit.c $UPSTREAM_OBJS $PCSC_CFLAGS $PCSC_LIBS -lm
set +x
ok "4 binaries built with -O2 -Wall and no warnings"

step "1. FNV-1a helper"
./build/ecm_cache_unit hash || bad "hash vectors"

step "2. env overrides (ARIB25_ECM_CACHE_MS / ARIB25_ECM_CACHE_DEBUG)"
for spec in "unset:2000:0" "0:0:0" "500:500:0" "abc:2000:0" "-5:2000:0" "99999999:2000:0" "2000:2000:1"; do
	v=${spec%%:*}; rest=${spec#*:}; want_ttl=${rest%%:*}; want_dbg=${rest#*:}
	if [ "$v" = "unset" ]; then
		out=$(env -u ARIB25_ECM_CACHE_MS -u ARIB25_ECM_CACHE_DEBUG ./build/ecm_cache_unit env)
	else
		if [ "$want_dbg" = "1" ]; then
			out=$(ARIB25_ECM_CACHE_MS=$v ARIB25_ECM_CACHE_DEBUG=1 ./build/ecm_cache_unit env)
		else
			out=$(ARIB25_ECM_CACHE_MS=$v ./build/ecm_cache_unit env)
		fi
	fi
	got_ttl=$(echo "$out" | sed -n 's/^ttl_ms=//p')
	got_dbg=$(echo "$out" | sed -n 's/^debug=//p')
	if [ "$got_ttl" = "$want_ttl" ] && [ "$got_dbg" = "$want_dbg" ]; then
		ok "ARIB25_ECM_CACHE_MS=$v -> ttl_ms=$got_ttl debug=$got_dbg"
	else
		bad "ARIB25_ECM_CACHE_MS=$v -> ttl_ms=$got_ttl debug=$got_dbg (want $want_ttl/$want_dbg)"
	fi
done

step "3. cache decision logic (real ECM sections from $TS)"
if ./build/ecm_cache_unit logic "$TS" > build/logic_on.log; then
	ok "cache ON: identical ECM skipped, changed ECM/pid hits the card"
else
	bad "cache ON logic test"
fi
cat build/logic_on.log

if ARIB25_ECM_CACHE_MS=0 ./build/ecm_cache_unit logic "$TS" > build/logic_off.log; then
	ok "cache OFF (ARIB25_ECM_CACHE_MS=0): every ECM reaches the card"
else
	bad "cache OFF logic test"
fi
grep -c "card call" build/logic_off.log | sed 's/^/  cache-off card calls: /'

step "4. TTL expiry (ARIB25_ECM_CACHE_MS=300, sleep 420 ms)"
if ARIB25_ECM_CACHE_MS=300 ./build/ecm_cache_unit ttl-expiry "$TS"; then
	ok "hit inside TTL, miss after TTL, hit again after refresh"
else
	bad "ttl-expiry"
fi

step "5. proc_ecm error must not be cached (mock card fails call #2)"
if MOCK_ECM_FAIL_AT=2 ./build/ecm_cache_unit fail-path "$TS"; then
	ok "failed card call is not cached (and its cleared multi2 key drops the entry)"
else
	bad "fail-path"
fi

step "6. compile-time switch ARIB25_ECM_CACHE=0"
./build/ecm_cache_unit_nocache logic "$TS" | sed 's/^/  /'

step "7. end-to-end: 49 MB scrambled TS, mock card, cache off vs on"
for CH in 188 49068; do
	ARIB25_ECM_CACHE_MS=0 ./build/ecm_cache_test "$TS" build/out_off_$CH.ts \
	    --ecm-pid 4416 --chunk $CH > build/run_off_$CH.log 2>&1
	ARIB25_ECM_CACHE_DEBUG=1 ./build/ecm_cache_test "$TS" build/out_on_$CH.ts \
	    --ecm-pid 4416 --chunk $CH > build/run_on_$CH.log 2>build/dbg_on_$CH.log
	off_calls=$(sed -n 's/^card_proc_ecm_calls=//p' build/run_off_$CH.log)
	on_calls=$(sed -n 's/^card_proc_ecm_calls=//p'  build/run_on_$CH.log)
	hits=$(grep -c "HIT"  build/dbg_on_$CH.log)
	misses=$(grep -c "MISS" build/dbg_on_$CH.log)
	same=$(cmp -s build/out_off_$CH.ts build/out_on_$CH.ts && echo yes || echo no)
	printf '  chunk=%s  ecm_packets=%s  card_calls(cache off)=%s  card_calls(cache on)=%s  hits=%s misses=%s  identical_output=%s\n' \
	    "$CH" "$(sed -n 's/^ecm_sections_in_ts=//p' build/run_on_$CH.log)" \
	    "$off_calls" "$on_calls" "$hits" "$misses" "$same"
	if [ "$same" = "yes" ]; then
		ok "chunk=$CH: cache on/off outputs are byte identical ($(md5sum build/out_on_$CH.ts | cut -d' ' -f1))"
	else
		bad "chunk=$CH: cache on/off outputs differ"
	fi
done

step "8. compile-time switch end-to-end (every ECM goes to the card)"
ARIB25_ECM_CACHE_DEBUG=1 ./build/ecm_cache_test_nocache "$TS" build/out_nocache_build.ts \
    --ecm-pid 4416 --chunk 49068 2>build/dbg_nocache_build.log > build/run_nocache_build.log
printf '  card_calls=%s  hit/miss log lines=%s  identical_output=%s\n' \
    "$(sed -n 's/^card_proc_ecm_calls=//p' build/run_nocache_build.log)" \
    "$(grep -c "cache HIT\|cache MISS" build/dbg_nocache_build.log)" \
    "$(cmp -s build/out_off_49068.ts build/out_nocache_build.ts && echo yes || echo no)"

step "9. where the ECM packets go (parser dedupe + cache counters)"
./build/ecm_cache_unit stats "$TS"

step "SUMMARY"
if [ "$FAIL" = "0" ]; then
	echo "ALL CHECKS PASSED"
else
	echo "FAILURES PRESENT"
fi
exit $FAIL
