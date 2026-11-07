# firmware

`hdus_as11loader` needs SKNET's loader image to bring the tuner out of its
pre-firmware state (`1738:5211` → `3275:6051`).

The firmware is **not** distributed here (it is SKNET's property). Supply
one of:

| file | where it comes from |
|---|---|
| `SKNET_AS11Loader.sys` (older, ~20–57 KB, header `66 4d` / `21 51` at offset `0xa88`) | SKNET "MTVHDU_080701_Drv" style driver packages |
| `as11fw.bin` (2-byte header + `0x4000` body) | extract the firmware body from `SKNET_AS11Loader.sys` (offset `0xa90`, `0x4000` bytes, prefixed with the 2-byte header found at `0xa88`) |

Put the file in this directory or pass its path:

```sh
./hdus_as11loader --dry-run firmware/SKNET_AS11Loader.sys   # verify first
sudo ./hdus_as11loader firmware/SKNET_AS11Loader.sys
```

`--dry-run` parses the image and prints the firmware id and blocks without
touching USB. The loader accepts `SKNET_AS11Loader.sys` directly, so
extracting `as11fw.bin` is optional.
