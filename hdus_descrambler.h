/*-
 * Copyright (c) 2008 FUKAUMI Naoki.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Ported from recfriio recfriior5 (Hdus.cpp / decrypt.c by FUKAUMI Naoki)
 * for SKNET MonsterTV HDUS / ASV5211 bulk TS descrambling.
 */
#ifndef HDUS_DESCRAMBLER_H
#define HDUS_DESCRAMBLER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define HDUS_TS_PACKET_SIZE 188
#define HDUS_TS_SYNC_BYTE   0x47

/*
 * Stream descrambler state.
 *
 * @r5:           0x91 = XOR mode, anything else (e.g. 0x80) = DES-like mode.
 * @leftover:     pending partial / complete-but-not-yet-emitted packet.
 *                Invariant: leftover_len == 0, or leftover[0] == 0x47 and
 *                leftover_len <= 188. Garbage before a sync byte is never
 *                retained (it is counted in @dropped).
 * @leftover_len: bytes valid in @leftover (0..188).
 * @packets:      complete 188-byte packets descrambled and emitted.
 * @dropped:      garbage bytes skipped while scanning for 0x47.
 */
struct hdus_descrambler {
	u8     r5;
	u8     leftover[HDUS_TS_PACKET_SIZE];
	size_t leftover_len;
	u64    packets;
	u64    dropped;
};

/* Initialise (or reset) a descrambler for the given r5 mode. */
void hdus_descrambler_init(struct hdus_descrambler *d, u8 r5);

/*
 * Feed a chunk of the scrambled bulk stream.
 *
 * Stream-filter semantics, mirroring recfriio Hdus::getStream():
 *  - scan for 0x47, dropping garbage before sync (counted in ->dropped);
 *  - once synced, treat the next 188 bytes as one packet regardless of
 *    any 0x47 bytes inside its payload (sequential scan);
 *  - descramble each complete packet into @out, in order;
 *  - a trailing partial packet is retained in ->leftover for the next
 *    feed() call.
 *
 * Never writes more than @out_cap bytes to @out. If @out_cap cannot hold
 * a whole packet, that packet is retained internally (leftover_len == 188)
 * and will be emitted by a later call with a large enough @out_cap.
 *
 * Returns the number of descrambled bytes written to @out (multiple of 188).
 * NULL @d, @in (with @len > 0), or @out (with @out_cap > 0) returns 0.
 */
size_t hdus_descrambler_feed(struct hdus_descrambler *d, const u8 *in,
			     size_t len, u8 *out, size_t out_cap);

/*
 * Descramble a single 188-byte packet in place.
 * @pkt188 must point to at least 188 bytes; its first 4 bytes (TS header)
 * are left unchanged. r5 == 0x91 selects XOR, otherwise DES-like decrypt.
 */
void hdus_descrambler_packet(u8 r5, u8 *pkt188);

/* Counters for GET_TS_ERROR_PACKET_COUNT style reporting. */
static inline u64 hdus_descrambler_packets(const struct hdus_descrambler *d)
{
	return d ? d->packets : 0;
}

static inline u64 hdus_descrambler_dropped(const struct hdus_descrambler *d)
{
	return d ? d->dropped : 0;
}

#endif /* HDUS_DESCRAMBLER_H */
