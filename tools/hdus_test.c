/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hdus_test.c - recpt1-style userspace test tool for the hdus
 *               SKNET MonsterTV HDUS kernel module.
 *
 * Device: /dev/hdusvideoX (char device, read() returns descrambled TS)
 * ABI:    recpt1-compatible PT1/PT3 ioctls, magic 0x8d
 *         (see hdus_ioctl.h at the project root, created by agent A)
 *
 * Build (standalone, works even when hdus_ioctl.h does not exist yet):
 *   gcc -O2 -Wall -o /tmp/hdus_test tools/hdus_test.c
 *   # or: make test
 *
 * Usage:
 *   hdus_test [-d /dev/hdusvideo0] [-c freq_no] [-s slot] [-t seconds]
 *             [-o outfile] [--signal-only]
 *   hdus_test -T <phys ch 13-62>    # UHF physical channel
 *   hdus_test -C <CATV ch 13-63>    # CATV channel
 *
 * Examples:
 *   hdus_test -d /dev/hdusvideo0 -c 63 -t 10 -o out.ts
 *   hdus_test -T 13 -t 10 -o out.ts          # ch13 == freq_no 63
 *   hdus_test -C 13 -t 10 -o out.ts          # C13  == freq_no 3
 *   hdus_test -c 63 --signal-only
 *   hdus_test -d /dev/hdusvideo0 -T 13 -t 5 -o -
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* hdus_ioctl.h : use it when present, fall back to local definitions */
/* ------------------------------------------------------------------ */
#if defined(__has_include)
#  if __has_include("../hdus_ioctl.h")
#    include "../hdus_ioctl.h"
#    define HDUS_HAVE_IOCTL_HEADER 1
#  endif
#endif

#ifndef HDUS_HAVE_IOCTL_HEADER
/*
 * Fallback definitions. Must match hdus_ioctl.h (agent A) exactly:
 * recpt1-compatible PT1/PT3 ABI, magic 0x8d.
 *
 *   SET_CHANNEL        _IOW(0x8d, 1, FREQUENCY)
 *   START_REC          _IO(0x8d, 2)
 *   STOP_REC           _IO(0x8d, 3)
 *   GET_SIGNAL_STRENGTH _IOR(0x8d, 4, int)
 *   LNB_ENABLE         _IOW(0x8d, 5, int)
 *   LNB_DISABLE        _IO(0x8d, 6)
 *   PTX_SET_SYSTEM_MODE _IOW(0x8d, 0x0b, int)
 */
/* Channel descriptor: same name/layout as hdus_ioctl.h
 * (struct hdus_frequency { int freq_no; int slot; }), so this
 * file works unchanged whether the real header is present or
 * the fallback below is used. */
struct hdus_frequency {
	int freq_no; /* recpt1 freq_no (UHF/CATV table index) */
	int slot;    /* slot / sub-channel number */
};
typedef struct hdus_frequency FREQUENCY;

#  ifndef HDUS_SET_CHANNEL
#    define HDUS_SET_CHANNEL         _IOW(0x8d, 1, FREQUENCY)
#  endif
#  ifndef HDUS_START_REC
#    define HDUS_START_REC           _IO(0x8d, 2)
#  endif
#  ifndef HDUS_STOP_REC
#    define HDUS_STOP_REC            _IO(0x8d, 3)
#  endif
#  ifndef HDUS_GET_SIGNAL_STRENGTH
#    define HDUS_GET_SIGNAL_STRENGTH _IOR(0x8d, 4, int)
#  endif
#  ifndef HDUS_LNB_ENABLE
#    define HDUS_LNB_ENABLE          _IOW(0x8d, 5, int)
#  endif
#  ifndef HDUS_LNB_DISABLE
#    define HDUS_LNB_DISABLE         _IO(0x8d, 6)
#  endif
#  ifndef HDUS_PTX_SET_SYSTEM_MODE
#    define HDUS_PTX_SET_SYSTEM_MODE _IOW(0x8d, 0x0b, int)
#  endif
#endif /* !HDUS_HAVE_IOCTL_HEADER */

/* If the real header uses slightly different macro names, map the
 * canonical names used by this file onto them. (No-op when the
 * header already provides HDUS_SET_CHANNEL etc.) */
#ifndef HDUS_SET_CHANNEL
#  ifdef HDUS_IOC_SET_CHANNEL
#    define HDUS_SET_CHANNEL HDUS_IOC_SET_CHANNEL
#  endif
#endif

#define TS_PACKET_SIZE   188
#define READ_BUF_SIZE    (TS_PACKET_SIZE * 1024)
#define DEFAULT_DEVICE   "/dev/hdusvideo0"
#define DEFAULT_OUTFILE  "out.ts"

static void usage(FILE *fp, const char *prog)
{
	fprintf(fp,
		"Usage: %s [options]\n"
		"\n"
		"recpt1-style test for /dev/hdusvideoX (SKNET MonsterTV HDUS)\n"
		"\n"
		"Options:\n"
		"  -d, --device <path>   char device (default: %s)\n"
		"  -c, --freq <freq_no>  recpt1 freq_no for SET_CHANNEL\n"
		"  -s, --slot <n>        slot / sub-channel (default: 0)\n"
		"  -T, --uhf <ch>        UHF physical channel 13-62\n"
		"                        (freq_no = ch + 50, e.g. ch13 -> 63)\n"
		"  -C, --catv <ch>       CATV channel 13-63\n"
		"                        (freq_no = ch-10 for 13-22, ch-1 for 23-63;\n"
		"                         e.g. C13 -> 3, C23 -> 22, C63 -> 62)\n"
		"  -t, --time <sec>      capture duration in seconds (default: 10)\n"
		"  -o, --out <file>      TS output file (default: %s, '-' = stdout)\n"
		"      --signal-only     only GET_SIGNAL_STRENGTH, then exit\n"
		"      --lnb-on          LNB_ENABLE before SET_CHANNEL\n"
		"      --lnb-off         LNB_DISABLE before SET_CHANNEL\n"
		"  -h, --help            show this help\n"
		"\n"
		"Examples:\n"
		"  %s -d /dev/hdusvideo0 -c 63 -t 10 -o out.ts\n"
		"  %s -T 13 -t 10 -o out.ts\n"
		"  %s -C 13 -t 10 -o out.ts\n"
		"  %s -c 63 --signal-only\n",
		prog, DEFAULT_DEVICE, DEFAULT_OUTFILE, prog, prog, prog, prog);
}

static int parse_int(const char *s, const char *what, long min, long max)
{
	char *end = NULL;
	long v;

	errno = 0;
	v = strtol(s, &end, 0);
	if (errno || !end || *end != '\0' || v < min || v > max) {
		fprintf(stderr, "hdus_test: invalid %s '%s' (range %ld-%ld)\n",
			what, s, min, max);
		exit(2);
	}
	return (int)v;
}

/* UHF physical channel -> recpt1 freq_no */
static int uhf_to_freq_no(int ch)
{
	if (ch < 13 || ch > 62) {
		fprintf(stderr, "hdus_test: UHF channel %d out of range 13-62\n", ch);
		exit(2);
	}
	return ch + 50;
}

/* CATV channel -> recpt1 freq_no : C13-22 -> 3-12, C23-63 -> 22-62 */
static int catv_to_freq_no(int ch)
{
	if (ch < 13 || ch > 63) {
		fprintf(stderr, "hdus_test: CATV channel %d out of range 13-63\n", ch);
		exit(2);
	}
	return (ch <= 22) ? (ch - 10) : (ch - 1);
}

static int do_signal_only(int fd)
{
	int strength = 0;

	if (ioctl(fd, HDUS_GET_SIGNAL_STRENGTH, &strength) < 0) {
		perror("ioctl(HDUS_GET_SIGNAL_STRENGTH)");
		return 1;
	}
	/* recpt1 reports CNR scaled; print raw value plus an
	 * approximate CNR in dB (raw / 100). Treat as indicative. */
	printf("signal_strength_raw=%d cnr_approx=%.2f dB\n",
	       strength, strength / 100.0);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *device = DEFAULT_DEVICE;
	const char *outfile = DEFAULT_OUTFILE;
	int freq_no = -1;	/* unset */
	int slot = 0;
	int seconds = 10;
	bool signal_only = false;
	bool lnb_on = false;
	bool lnb_off = false;
	int fd;
	int client_argc = argc;
	char **client_argv = argv;

	(void)client_argc;
	(void)client_argv;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val = NULL;

		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout, argv[0]);
			return 0;
		} else if (!strcmp(a, "--signal-only")) {
			signal_only = true;
		} else if (!strcmp(a, "--lnb-on")) {
			lnb_on = true;
		} else if (!strcmp(a, "--lnb-off")) {
			lnb_off = true;
		} else if (!strcmp(a, "-d") || !strcmp(a, "--device")) {
			if (++i >= argc) { usage(stderr, argv[0]); return 2; }
			device = argv[i];
		} else if (!strcmp(a, "-c") || !strcmp(a, "--freq")) {
			if (++i >= argc) { usage(stderr, argv[0]); return 2; }
			freq_no = parse_int(argv[i], "freq_no", 0, 10000);
		} else if (!strcmp(a, "-s") || !strcmp(a, "--slot")) {
			if (++i >= argc) { usage(stderr, argv[0]); return 2; }
			slot = parse_int(argv[i], "slot", 0, 10000);
		} else if (!strcmp(a, "-T") || !strcmp(a, "--uhf")) {
			if (++i >= argc) { usage(stderr, argv[0]); return 2; }
			freq_no = uhf_to_freq_no(parse_int(argv[i], "UHF channel", 13, 62));
		} else if (!strcmp(a, "-C") || !strcmp(a, "--catv")) {
			if (++i >= argc) { usage(stderr, argv[0]); return 2; }
			freq_no = catv_to_freq_no(parse_int(argv[i], "CATV channel", 13, 63));
		} else if (!strcmp(a, "-t") || !strcmp(a, "--time")) {
			if (++i >= argc) { usage(stderr, argv[0]); return 2; }
			seconds = parse_int(argv[i], "seconds", 1, 86400);
		} else if (!strcmp(a, "-o") || !strcmp(a, "--out")) {
			if (++i >= argc) { usage(stderr, argv[0]); return 2; }
			outfile = argv[i];
		} else if (!strncmp(a, "--device=", 9)) {
			device = a + 9;
		} else if (!strncmp(a, "--freq=", 7)) {
			val = a + 7;
			freq_no = parse_int(val, "freq_no", 0, 10000);
		} else if (!strncmp(a, "--slot=", 7)) {
			val = a + 7;
			slot = parse_int(val, "slot", 0, 10000);
		} else if (!strncmp(a, "--uhf=", 6)) {
			val = a + 6;
			freq_no = uhf_to_freq_no(parse_int(val, "UHF channel", 13, 62));
		} else if (!strncmp(a, "--catv=", 7)) {
			val = a + 7;
			freq_no = catv_to_freq_no(parse_int(val, "CATV channel", 13, 63));
		} else if (!strncmp(a, "--time=", 7)) {
			val = a + 7;
			seconds = parse_int(val, "seconds", 1, 86400);
		} else if (!strncmp(a, "--out=", 6)) {
			outfile = a + 6;
		} else {
			fprintf(stderr, "hdus_test: unknown option '%s'\n", a);
			usage(stderr, argv[0]);
			return 2;
		}
	}

	if (freq_no < 0 && !signal_only) {
		fprintf(stderr, "hdus_test: channel required: use -c <freq_no>, "
				    "-T <13-62> or -C <13-63>\n");
		usage(stderr, argv[0]);
		return 2;
	}
	if (freq_no < 0 && signal_only) {
		/* Still allow bare --signal-only for a manual check. */
		fprintf(stderr, "hdus_test: warning: no channel given, "
				    "querying signal strength only\n");
	}

	fd = open(device, O_RDONLY);
	if (fd < 0) {
		perror(device);
		fprintf(stderr, "hdus_test: open %s failed\n", device);
		if (errno == ENOENT)
			fprintf(stderr, "hdus_test: hint: is hdus.ko loaded and "
					    "firmware uploaded (3275:6051)?\n");
		return 1;
	}

	if (lnb_on) {
		int on = 1;
		if (ioctl(fd, HDUS_LNB_ENABLE, &on) < 0)
			perror("ioctl(HDUS_LNB_ENABLE)");
	}
	if (lnb_off) {
		if (ioctl(fd, HDUS_LNB_DISABLE) < 0)
			perror("ioctl(HDUS_LNB_DISABLE)");
	}

	if (freq_no >= 0) {
		/* struct hdus_frequency is defined by hdus_ioctl.h when
		 * present, and by the fallback above otherwise. */
		struct hdus_frequency freq = { .freq_no = freq_no,
					       .slot = slot };
		if (ioctl(fd, HDUS_SET_CHANNEL, &freq) < 0) {
			perror("ioctl(HDUS_SET_CHANNEL)");
			close(fd);
			return 1;
		}
	}

	if (signal_only) {
		int rc = do_signal_only(fd);
		close(fd);
		return rc;
	}

	if (ioctl(fd, HDUS_START_REC) < 0) {
		perror("ioctl(HDUS_START_REC)");
		close(fd);
		return 1;
	}

	/* Open output (binary). "-" means stdout. */
	FILE *out = stdout;
	bool close_out = false;
	if (strcmp(outfile, "-") != 0) {
		out = fopen(outfile, "wb");
		if (!out) {
			perror(outfile);
			ioctl(fd, HDUS_STOP_REC);
			close(fd);
			return 1;
		}
		close_out = true;
	}

	fprintf(stderr, "hdus_test: %s freq_no=%d slot=%d duration=%ds out=%s\n",
		device, freq_no, slot, seconds, outfile);

	struct timespec deadline;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += seconds;

	uint64_t total_bytes = 0;
	uint64_t packets = 0;
	uint64_t sync_ok = 0;
	uint64_t reads = 0;
	static uint8_t buf[READ_BUF_SIZE];

	while (1) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long remain_ms = (deadline.tv_sec - now.tv_sec) * 1000L
			+ (deadline.tv_nsec - now.tv_nsec) / 1000000L;
		if (remain_ms <= 0)
			break;

		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, remain_ms > 1000 ? 1000 : (int)remain_ms);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (pr == 0)
			continue;

		ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror("read");
			break;
		}
		if (n == 0)
			continue;

		reads++;
		total_bytes += (uint64_t)n;
		if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
			perror("fwrite");
			break;
		}

		/* TS sync verification: stream is 188-byte packets,
		 * each starting with 0x47. Count packets by scanning
		 * for 0x47 at 188-byte boundaries from the start of
		 * the read buffer; if the stream is not aligned to
		 * our reads this is approximate but indicative. */
		for (ssize_t off = 0; off + TS_PACKET_SIZE <= n;
		     off += TS_PACKET_SIZE) {
			packets++;
			if (buf[off] == 0x47)
				sync_ok++;
		}
	}

	if (ioctl(fd, HDUS_STOP_REC) < 0)
		perror("ioctl(HDUS_STOP_REC)");
	if (close_out)
		fclose(out);
	close(fd);

	double ratio = packets ? (100.0 * (double)sync_ok / (double)packets) : 0.0;
	double rate = seconds ? (double)total_bytes / (double)seconds : 0.0;

	/* With -o - the TS owns stdout, so the summary goes to stderr. */
	FILE *summary = strcmp(outfile, "-") == 0 ? stderr : stdout;

	fprintf(summary,
		"done: bytes=%llu packets=%llu sync_ok=%llu sync_ratio=%.2f%% "
	       "reads=%llu rate=%.1f B/s (%.2f Mbit/s) outfile=%s\n",
	       (unsigned long long)total_bytes,
	       (unsigned long long)packets,
	       (unsigned long long)sync_ok,
	       ratio,
	       (unsigned long long)reads,
	       rate, rate * 8.0 / 1e6,
	       outfile);

	if (packets == 0) {
		fprintf(stderr, "hdus_test: no TS received "
				    "(check channel, antenna, firmware, dmesg)\n");
		return 1;
	}
	/* A healthy descrambled TS stream should be ~100% 0x47 sync. */
	if (ratio < 90.0)
		fprintf(stderr, "hdus_test: warning: low 0x47 sync ratio "
				    "(%.2f%%) - stream may be misaligned\n", ratio);
	return 0;
}
