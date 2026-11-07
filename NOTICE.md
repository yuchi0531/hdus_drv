# Licensing / ライセンス

This tree mixes code with several origins. Each file carries its own
header; the summary below is only a map.

| Component | License |
|---|---|
| `hdus_drv.c`, `hdus_ioctl.h` (kernel module) | `GPL-2.0` (kernel module, SPDX in each file) |
| `hdus_descrambler.c/.h`, `hdus_as11loader.c` | BSD 2-clause, derived from FUKAUMI Naoki's recfriio work (copyright notice kept in the files) |
| `tools/`, `tools/ifd-hdus/` (userspace) | `GPL-2.0-or-later` (SPDX in each file) |
| `third_party/arib25_patched/` | ARIB STD-B25 test program license + the original libaribb25 terms; see the file headers and `third_party/arib25_patched/README.md`. Only `arib_std_b25.c` is modified (clearly bracketed `HDUS PATCH` sections), everything else is byte-identical to upstream. |
| `docs/` | CC0 / public domain (design notes written for this project) |

## On protocol knowledge

The `ioctl` numbering follows the PT1/PT3 convention used by `recpt1`
and `BonDriver_LinuxPT(X)`; the card protocol was reconstructed from
`BonDriver_HDUS.dll` (SKNET) and cross-checked against the
`asie5606` driver in Till0196/coax (Apache-2.0 OR GPL-2.0-or-later).
No source code was copied from those projects; only documented
behaviour (register addresses, T=1 framing, timings) is used.

`hdus_test.c`'s channel numbering follows `recpt1`/`BonDriver_LinuxPT`
(`freq_no`), so existing channel tables keep working.

## Firmware

The SKNET firmware (`SKNET_AS11Loader.sys` / `as11fw.bin`) is **not**
included in this repository: it is SKNET's property. `hdus_as11loader`
extracts it from a `SKNET_AS11Loader.sys` that you supply (see
`firmware/README.md`).

## No warranty

Provided AS-IS. Recording and descrambling rules differ by country;
you are responsible for how you use this.
