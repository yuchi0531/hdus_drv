/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Userspace self-test for hdus_descrambler. Build:
 *   gcc -O2 -Wall -o /tmp/desc_test tools/descrambler_test.c hdus_descrambler.c
 *   /tmp/desc_test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../hdus_descrambler.h"

#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); exit(1); } \
} while (0)

/* Build a plaintext TS packet; payload deliberately contains 0x47 bytes
 * (false syncs) at several offsets. Caller scrambles it via the public
 * packet API (XOR is its own inverse). */
static void make_plain(u8 *pkt, u8 seed)
{
	int i;
	pkt[0] = 0x47; pkt[1] = 0x40; pkt[2] = 0x00; pkt[3] = (u8)(0x10 | (seed & 0x0f));
	for (i = 4; i < 188; i++)
		pkt[i] = (u8)(seed + i * 7);
	pkt[10] = 0x47; pkt[11] = 0x47; pkt[50] = 0x47; pkt[100] = 0x47; pkt[187] = 0x47;
}

int main(void)
{
	u8 plain[2][188], scr[2][188];
	u8 stream[7 + 2 * 188]; /* 7 bytes garbage + 2 packets */
	u8 out[4 * 188];
	struct hdus_descrambler d;
	size_t n, total = 0;
	int i, k;

	make_plain(plain[0], 0x11);
	make_plain(plain[1], 0x22);
	for (k = 0; k < 2; k++) {
		memcpy(scr[k], plain[k], 188);
		hdus_descrambler_packet(0x91, scr[k]); /* scramble */
		CHECK(memcmp(scr[k], plain[k], 188) != 0, "xor scramble changes bytes");
		CHECK(scr[k][0] == 0x47, "xor scramble keeps header byte 0");
	}

	memset(stream, 0xAB, 7); /* garbage, no 0x47 */
	memcpy(stream + 7, scr[0], 188);
	memcpy(stream + 7 + 188, scr[1], 188);

	hdus_descrambler_init(&d, 0x91);

	/* Feed in awkward splits: 1, 3, 100, 83, 188, rest. */
	{
		static const size_t splits[] = { 1, 3, 100, 83, 188 };
		size_t pos = 0, si = 0;
		while (pos < sizeof(stream)) {
			size_t chunk = (si < sizeof(splits)/sizeof(splits[0])) ? splits[si++] : (sizeof(stream) - pos);
			if (pos + chunk > sizeof(stream)) chunk = sizeof(stream) - pos;
			n = hdus_descrambler_feed(&d, stream + pos, chunk, out + total, sizeof(out) - total);
			total += n;
			pos += chunk;
		}
	}
	CHECK(total == 2 * 188, "feed emitted exactly 2 packets");
	CHECK(memcmp(out, plain[0], 188) == 0, "packet 0 descrambled == plain");
	CHECK(memcmp(out + 188, plain[1], 188) == 0, "packet 1 descrambled == plain");
	CHECK(hdus_descrambler_packets(&d) == 2, "packets counter == 2");
	CHECK(hdus_descrambler_dropped(&d) == 7, "dropped counter == garbage len");
	CHECK(d.leftover_len == 0, "no leftover after complete stream");

	/* Partial trailing packet retained, emitted on next feed. */
	{
		struct hdus_descrambler d2;
		u8 one[188], out2[188];
		hdus_descrambler_init(&d2, 0x91);
		make_plain(one, 0x33);
		memcpy(scr[0], one, 188);
		hdus_descrambler_packet(0x91, scr[0]);
		n = hdus_descrambler_feed(&d2, scr[0], 100, out2, sizeof(out2));
		CHECK(n == 0 && d2.leftover_len == 100, "partial packet retained");
		n = hdus_descrambler_feed(&d2, scr[0] + 100, 88, out2, sizeof(out2));
		CHECK(n == 188 && memcmp(out2, one, 188) == 0, "completed packet emitted");
	}

	/* Small out_cap: packet retained, not truncated, emitted later. */
	{
		struct hdus_descrambler d3;
		u8 one[188], out3[188], tiny[10];
		hdus_descrambler_init(&d3, 0x91);
		make_plain(one, 0x44);
		memcpy(scr[0], one, 188);
		hdus_descrambler_packet(0x91, scr[0]);
		n = hdus_descrambler_feed(&d3, scr[0], 188, tiny, sizeof(tiny));
		CHECK(n == 0 && d3.leftover_len == 188, "small out_cap retains packet");
		n = hdus_descrambler_feed(&d3, NULL, 0, out3, sizeof(out3));
		CHECK(n == 188 && memcmp(out3, one, 188) == 0, "retained packet emitted next");
	}

	/* DES mode: header preserved, payload changes, deterministic. */
	{
		u8 in[188], a[188], b[188];
		struct hdus_descrambler d4;
		size_t m;
		make_plain(in, 0x55);
		memcpy(a, in, 188); hdus_descrambler_packet(0x80, a);
		memcpy(b, in, 188); hdus_descrambler_packet(0x80, b);
		CHECK(memcmp(a, b, 188) == 0, "DES decrypt deterministic");
		CHECK(memcmp(a, in, 4) == 0, "DES keeps first 4 header bytes");
		CHECK(memcmp(a + 4, in + 4, 184) != 0, "DES changes payload");
		hdus_descrambler_init(&d4, 0x80);
		m = hdus_descrambler_feed(&d4, in, 188, out, sizeof(out));
		CHECK(m == 188 && memcmp(out, a, 188) == 0, "feed DES == packet DES");
		for (i = 0; i < 4; i++) CHECK(out[i] == in[i], "feed DES header preserved");
	}

	printf("PASS\n");
	return 0;
}
