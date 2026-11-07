# HDUS patched libaribb25 — ECM result cache

Copy of the libaribb25 sources used by `~/projects/recfriio/arib25/`
(`arib_std_b25.c` + the headers/`.c` files it builds with), with **one
minimal change**: `B_CAS_CARD::proc_ecm()` is not called again while the
same ECM (same ECM pid, same section bytes) has already been processed and
the multi2 key it produced is still installed.

Nothing here is used automatically by the rest of the repository — the root
`Makefile`, `hdus_drv.c` and `tools/` are untouched.  Build against *this*
directory instead of `recfriio/arib25` when you want the cache.

## Why

On HDUS the tuner and the B-CAS card share the ASIE registers and the kernel
driver has to power-cycle between the two modes, so one card transaction is
~1.6 s (README "同時使用 (mux)").  ECM sections repeat every 100–500 ms with
identical content, so a card round-trip per ECM packet stalls the TS
(19.8 Mbit/s recording collapses to ~13 KB/s while decoding).

## The patch

`hdus_ecm_cache.diff` is the complete unified diff against the original file
(`diff -u` of `recfriio/arib25/arib_std_b25.c` and the copy here).
Every change is bracketed by `/* HDUS PATCH begin : ... */ ... /* HDUS PATCH
end */`; the upstream licence/description header is untouched.
Six regions in `arib_std_b25.c`:

1. helpers after the `#include` block — `hdus_ecm_hash()` (FNV-1a 32),
   `hdus_ecm_cache_ttl_ms()`, `hdus_ecm_cache_debug_on()`, `hdus_now_ms()`
   (`CLOCK_MONOTONIC`).
2. `DECRYPTOR_ELEM` gains `ecm_cache_valid/pid/hash/stamp/hit/miss`
   (element is `calloc`ed, so all start at zero).
3. `proc_ecm()`: locals + lookup between `p = sect.data;` and
   `bcas->proc_ecm(...)`.
4. `proc_ecm()`: commit of `(pid, hash, now)` immediately after
   `dec->m2->set_scramble_key(dec->m2, res.scramble_key);` — i.e. only after
   a successful card call that actually installed a key.
5. `proc_ecm()`: on `bcas->proc_ecm()` failure the multi2 key is cleared by
   the original code, so the cache entry is invalidated too (a later
   identical ECM must not "hit" with no key installed).

Hit condition (all of):

```
ttl > 0  &&  ecm_cache_valid  &&  ecm_cache_pid == dec->ecm_pid
         &&  ecm_cache_hash == fnv1a(section body)
         &&  dec->m2 != NULL      /* key still there */
         &&  now - ecm_cache_stamp < ttl
```

On a hit `proc_ecm()` jumps to its `LAST:` label: the ECM section is still
returned to the parser (`dec->ecm->ret()`), the existing multi2 key is reused,
the card is not touched.  The hash covers the whole ECM section body
(`sect.data .. sect.tail`, a superset of the `p/length` bytes handed to the
card) plus the pid that is part of the key.

### Switches

| switch | default | meaning |
|---|---|---|
| `ARIB25_ECM_CACHE` (compile time, `-D`) | `1` | `0` compiles the cache out completely (bit-for-bit upstream behaviour) |
| `ARIB25_ECM_CACHE_MS` (env) | `2000` | TTL in ms; `0` disables the cache at runtime; invalid/huge values fall back to the default |
| `ARIB25_ECM_CACHE_DEBUG` (env) | off | `1` logs one `[arib25] ECM cache HIT|MISS ...` line per decision to stderr |

`hdus_ecm_hash()`, `hdus_ecm_cache_ttl_ms()` and `hdus_ecm_cache_debug_on()` are
deliberately non-static so the offline test harness can call them.

## Build

Compile-only check of the patched decoder (clean, no warnings):

```
gcc -O2 -Wall -I. $(pkg-config --cflags libpcsclite) -c arib_std_b25.c
```

The hardware-free test decoder (this is the exact command `run_tests.sh` runs):

```
gcc -O2 -Wall -I.. -o build/ecm_cache_test \
    ../arib_std_b25.c ../multi2.c ../ts_section_parser.c ../b_cas_card.c \
    mock_bcas.c ecm_cache_test.c \
    $(pkg-config --cflags --libs libpcsclite) -lm
```

The copied upstream `Makefile` still builds the `b25` command line test
program from these sources (`make CFLAGS="-Wall -O2 $(pkg-config --cflags
libpcsclite)"`, then `make clean`).

Compiles with `-O2 -Wall` without warnings.  The one upstream wart fixed in
this copy is a header-guard typo in `ts_section_parser_error_code.h`
(`TS_SECTION_PARESR_...` → `TS_SECTION_PARSER_...`, in the diff, unrelated to
the cache).

## Tests (no hardware, no PC/SC reader)

```
cd third_party/arib25_patched/test
./run_tests.sh                 # TS=/tmp/raw20_nopcsc.ts by default
```

* `test/mock_bcas.c` — a `B_CAS_CARD` implementation with no reader: counts
  `proc_ecm()` calls and derives the 16-byte scramble key from the ECM payload
  (so a wrongly skipped card call corrupts the output and is detectable).
* `test/ecm_cache_test.c` — feeds a raw TS through `create_arib_std_b25()` +
  `set_b_cas_card(mock)` in `put()`/`get()` rounds (`--chunk`, `--pace-ms`).
* `test/ecm_cache_unit.c` — white-box (`#include "../arib_std_b25.c"`) test of
  `proc_ecm()` with real ECM sections taken from the TS; drives the hit, miss,
  pid-change, TTL-expiry, card-error and `ARIB25_ECM_CACHE=0` paths.

Results on `/tmp/raw20_nopcsc.ts` (49,475,772 B, ECM pid 0x1140):

```
  chunk=188    ecm_packets=198  card_calls(cache off)=10  card_calls(cache on)=10  hits=0 misses=10  identical_output=yes
  chunk=49068  ecm_packets=198  card_calls(cache off)=10  card_calls(cache on)=10  hits=0 misses=10  identical_output=yes
  decryptor pid=0x1140 ecm_total=192 ecm_unique=10 ecm_error=0 cache_hit=0 cache_miss=10 m2=yes
```

`ecm_total=192` is what the ECM section parser was fed, `ecm_unique=10` what it
committed.  **Upstream `ts_section_parser.c` already suppresses a section that
is byte-identical to the previously committed one** (`compare_elem_section()` /
`cancel_elem_same()`), so on a stream whose ECM content only changes every few
seconds the card is already only asked once per *change*, not once per packet.
The cache therefore reports 0 hits on this fixture — it is a second line of
defence for the cases where an identical ECM *does* reach `proc_ecm()`
(parser state reset after a re-sync, integration that re-presents the section,
dedupe defeated by the pool/refcount handling).  In exactly that case the
verified saving is one card transaction per repeat:

```
PASS  cold ECM #0            -> card call  (calls=1)
PASS  identical repeat       -> CACHE HIT (calls=1 hits=1)
PASS  changed ECM content    -> card call  (calls=2)
PASS  repeat after change    -> CACHE HIT (calls=2 hits=2)
PASS  same payload, other pid -> card call (calls=3)
PASS  TTL expired (300 ms)   -> card call  (calls=2)
PASS  ECM A after failure    -> card call #3 (stale key dropped)
```

Ceiling for this file: if all 192 ECM sections had reached `proc_ecm()`
instead of 10, the cache would have removed 182 of 192 card calls (94.8 %),
~291 s of card time over this 49 MB file.  To find out which regime the live
HDUS pipeline is in, run it with `ARIB25_ECM_CACHE_DEBUG=1` and compare the
number of `MISS` lines with the number of ECM packets.

### Limits worth knowing

* Single slot per decryptor: it only helps *consecutive* identical ECMs.
  If the stream alternates A,B,A,B (odd/even ECM), neither this cache nor the
  parser dedupe saves anything; a 2–4 slot ring keyed by hash would.
* If the live ECM bytes differ on every packet (e.g. a counter/CRC that is not
  derived from the key material), a whole-body hash can never match and the
  cache stays inert; the hash would then have to be narrowed, which risks
  reusing a stale key.
* `ARIB_STD_B25::reset()` tears the decryptors down, so a pipeline that resets
  the decoder around every card transaction also throws this cache away.
