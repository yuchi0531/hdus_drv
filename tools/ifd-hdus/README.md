# HDUS B-CAS PC/SC IFD handler (`tools/ifd-hdus/`)

Exposes the HDUS tuner's internal B-CAS card slot as a PC/SC reader,
**"HDUS B-CAS"**, so libaribb25-based tools (`tools/bcas_check`, recfriio)
can use the card through the normal `SCardTransmit` path.

## Architecture — simultaneity via kernel mux

The HDUS tuner and the B-CAS card share one USB device (ASIE card
interface behind the tuner), so only one owner may drive USB at a time.
The kernel driver `hdus.ko` is the **single owner** of the USB device:

- The **kernel** (`hdus.ko`, see `hdus_drv.c` / `hdus_ioctl.h`) claims the
  USB interface and muxes tuner and card access **per transaction**:
  each card ioctl (`HDUS_CARD_CONNECT` / `HDUS_CARD_TRANSMIT` /
  `HDUS_CARD_PRESENT` on `/dev/hdusvideoX`) switches the device to card
  mode, performs the whole transaction, and **restores tuner mode**
  before returning, so the tuner TS stream continues around each card
  access (a brief TS gap during the transaction is normal and
  expected). Tuner and card are therefore usable **simultaneously**
  from userspace.
- **Userspace** (`hdus_card.c`) never touches USB directly when the
  video node exists: `hdus_card_open()` opens `/dev/hdusvideoX` and
  every operation is an ioctl on that fd.

If `/dev/hdusvideoX` is absent (kernel module not loaded), `hdus_card.c`
falls back to a **usbfs transport** that claims the interface itself.
That fallback is **card-only exclusive mode**: while it runs, the tuner
cannot be used (no other process may hold the interface). It exists so
the card code keeps working standalone until `hdus.ko` is loaded — it is
never the right mode when tuner and card must coexist.

## Files

| file | what it is |
|---|---|
| `hdus_card.h` / `hdus_card.c` | userspace transport implementing the BonDriver_HDUS SendCard/ConnectCard protocol (`../../docs/card_protocol_sendcard.md`, S-box in `../card_sbox.h`); kernel-video primary, usbfs fallback |
| `hdus_ifd.c` | PC/SC IFD handler (pcsc-lite **IFDHandler v3.0**, `IFDH*` symbols — see "API note") |
| `Makefile` | builds `libhdus_ifd.so` + `hdus_card_test` |
| `reader.conf.example` | `/etc/reader.conf` snippet |
| `README.md` | this file |

ConnectCard behaviour comes from `../../docs/card_protocol_connect.md`
(presence `R(0x04)` masks, strobe/poll/session-read sequence, init tail)
and `../../docs/card_protocol_sendcard.md` (SendCard framing). Live
hardware status (2026-11-07): power/saddr/R(0x04) presence/session read
all work over the usbfs path; SendCard APDUs currently return zero-length-ish
responses (4 bytes, all zero) - see root README "B-CAS status".

## Build

```sh
make -C tools/ifd-hdus all
```

Needs only `libpcsclite-dev` headers (`/usr/include/PCSC/`); the handler
does not link `-lpcsclite` (pcscd dlopen()s it). `hdus_card_test` carries
its `main()` via `-DHDUS_CARD_TEST_MAIN` in `hdus_card.c`, so no extra
test source file is needed.

## Install

Kernel transport (simultaneous tuner + card) requires `hdus.ko` loaded:

```sh
make -C tools/ifd-hdus install        # -> /usr/lib/pcsc/drivers/libhdus_ifd.so
# merge reader.conf.example into /etc/reader.conf
# (DEVICENAME is the video node when present, e.g. /dev/hdusvideo0;
#  the usbfs node from `lsusb` + HDUS_USB_PATH are the fallback only)
sudo systemctl restart pcscd
# Load hdus.ko and verify /dev/hdusvideo0 exists before starting pcscd
```

The HDUS video node / usbfs node is root-only by default. Either run
pcscd/test binaries as root, or add a udev rule granting your user
access (`SUBSYSTEM=="video4linux", KERNEL=="hdusvideo*", MODE="0660",
GROUP="video"` for the kernel path; the USB usbfs rule below for the
fallback).

## Usage / test

Real-hardware check without PC/SC (needs root or the udev rule; the
test prints which transport it used):

```sh
# Kernel transport (hdus.ko loaded, video node present):
sudo ./tools/ifd-hdus/hdus_card_test            # opens /dev/hdusvideo0
sudo HDUS_VIDEO_PATH=/dev/hdusvideo0 ./tools/ifd-hdus/hdus_card_test
sudo ./tools/ifd-hdus/hdus_card_test /dev/hdusvideo0   # explicit video node

# usbfs fallback (kernel module NOT loaded, video node absent):
sudo ./tools/ifd-hdus/hdus_card_test [/dev/bus/usb/002/008]
# or: sudo HDUS_USB_PATH=/dev/bus/usb/002/008 ./tools/ifd-hdus/hdus_card_test
```

Both modes open, connect, then send B-CAS APDU `90 30 00 00 00`
(initial settings) and `90 32 00 00 00` (card ID acquire) and hexprint
the responses. Expect the first line after open to read `transport:
kernel ioctl (...)` or `transport: usbfs fallback (...)`.

Through PC/SC:

```sh
pcsc_scan                       # should list "HDUS B-CAS"
./tools/bcas_check              # libaribb25 SCardTransmit path
```

## API (`hdus_card.h`)

```c
int  hdus_card_open(const char *usb_path);   /* primary: $HDUS_VIDEO_PATH -> /dev/hdusvideo0;
                                                fallback: $HDUS_USB_PATH -> default usbfs;
                                                a /dev/hdusvideo* arg names the video node */
void hdus_card_close(void);
int  hdus_card_connect(void);                /* 0 ok, -2 not-ready, -3 not-present */
int  hdus_card_transmit(const uint8_t *req, int req_len,
                        const uint8_t *extra, int extra_len,
                        uint8_t *resp, int resp_max); /* resp_len or negative */
int  hdus_card_present(void);                /* 1 / 0 */
int  hdus_card_transport(void);              /* 0 none, 1 kernel ioctl, 2 usbfs */
```

## Protocol summary

Kernel transport: ioctls on the video node — `HDUS_CARD_CONNECT`
`_IO(0x8d,0x10)`, `HDUS_CARD_TRANSMIT` `_IOWR(0x8d,0x11,struct
hdus_card_req)` (`req_len/extra_len/resp_len` + 264/264/600-byte
buffers), `HDUS_CARD_PRESENT` `_IOR(0x8d,0x12,int*)` (1/0). The kernel
runs the whole tuner<->card mode switch and restore internally; userspace
passes only req/extra buffers and receives the response bytes.

usbfs transport (fallback): usbdevfs `USBDEVFS_CONTROL`, bRequestType=0xC0,
`buf[2]={status,data}` (`status==1` means ok, as in
`tools/hdus_card_probe.c`). Interface 0 is claimed, then the Hdus.cpp
`UsbProcBegin` power sequence + ASIE5606 tuner table + ASIE-on gpio
sequence (`0x08` writes `0x11ff/0x1100/0x11ff`, card power bit `0x10`)
run — copied into `hdus_card_power_init()` — then saddr detect
(`req 0x02`, `wValue 0x0900|saddr`, `0x5a` then `0x4a`).

Card registers: `R(reg)` = req `0x02`, `W(reg,val)` = req `0x03`, both
with `wValue=(reg<<8)|slave`. A frame is `00 <state> <len> req
[len extra 00]`, bit-reversed via `hdus_card_sbox` with an XOR checksum
of the transformed bytes appended; written in 0x40-byte FIFO chunks
(regs `0x40/0x80/...`, `W(0x3a,0/1)` interleave), started with
`W(0x00,0x0a)`. The reply length is `sbox[R(0x42)]+4`, read back in
0x40 chunks, sbox-decoded and checksum-verified (XOR of raw bytes).

APDU mapping (from `arib25/b_cas_card.c`, verified in source): libaribb25
sends each APDU as a single T=1 `SCardTransmit` including the header.
`90 30` / `90 32` / `90 80` map to `req` = whole APDU; ECM (`90 34`) and
EMM (`90 36`) are built as `header(4) + len + data + 00` in one transmit,
which the handler splits back into `req` = 4-byte header,
`extra` = the data block (when `byte[4]==len` and the last byte is `00`).

## API note — header signature mismatch

The task brief named libccid-style symbols (`IFDInit`, `IFDProcessSegment`,
`IFDExit`, `IFDStatusSIM`, `IFDListSlot`, `IFDListReader`). The pcsc-lite
header installed here (`/usr/include/PCSC/ifdhandler.h`) defines
**IFDHandler v3.0** and pcscd dlopen()s the `IFDH*` symbols. `hdus_ifd.c`
therefore implements the real v3 API (`IFDHCreateChannelByName`,
`IFDHCreateChannel`, `IFDHCloseChannel`, `IFDHICCPresence`,
`IFDHPowerICC`, `IFDHTransmitToICC`, `IFDHGetCapabilities`,
`IFDHSetCapabilities`, `IFDHSetProtocolParameters`, `IFDHControl`)
instead of the libccid names. `IFDHControl` is the escape entry point
(control code `!= 0` returns `IFD_ERROR_TAG`). The reader name comes from
`reader.conf` `FRIENDLYNAME` (kept in sync with `HDUS_IFD_READER_NAME`).

## Troubleshooting

- `open /dev/hdusvideo0: Permission denied` — run with `sudo` or add
  the video udev rule above (also for pcscd: it runs as root, fine).
- `hdus_card_open failed` with `transport: usbfs fallback` when tuner
  should be running — `/dev/hdusvideo0` is missing: `ls /dev/hdusvideo*`;
  `hdus.ko` is not loaded. Load it (module per root README) so the kernel
  path is used; the usbfs path by itself does not allow simultaneous
  tuner use.
- `claim interface 0 ... Device or resource busy` — **EBUSY/claim busy
  while `hdus.ko` is loaded**: the kernel owns USB, so the usbfs fallback
  can never claim the interface. This happens when the video node went
  missing (module unloaded or bound to another device) and userspace
  fell back. Fix: reload `hdus.ko` until `/dev/hdusvideo0` exists and
  confirm `hdus_card_test` prints `transport: kernel ioctl`.
- Device node moved — `/dev/hdusvideo0` numbering can vary (use
  `ls /dev/hdusvideo*` and set `HDUS_VIDEO_PATH`); for the usbfs fallback
  `/dev/bus/usb/002/008` is just the current node, find the HDUS via
  `lsusb` and update `DEVICENAME` or `HDUS_USB_PATH`.
- Reader shows but card never appears — check the raw logs
  (`hdus_card:` lines on stderr of pcscd / the test binary; kernel
  transport logs the ioctl result), see "open questions" below.
- Card present but APDUs fail with `data error` / `transmit failed` —
  confirm from `hdus_card_test` whether raw responses come back at all
  before debugging the checksum/state handling.

## Open questions

1. **Presence masks**: recovered from BonDriver RE
   (`docs/card_protocol_connect.md`): `HDUS_CARD_MASK_PRESENT=0x01`,
   `HDUS_CARD_MASK_READY=0x80`. Live-verified: `R(0x04)=0x01`
   (present) before init, `0x81` (ready) after ConnectCard.
2. **Slave address**: ASIE/card slave is `0x4a` on the current HDUSF
   unit (reg09 val 0x06 = DES). I2C scan found only `0x20` (demod),
   `0x4a` (ASIE/card); the DLL `raw-4` formula (`0x46`) NACKs on this
   unit, so the code uses the ASIE slave directly.
3. **Response length/checksum byte**: `hdus_card_transmit()` returns the
   DLL `resp_len` including the trailing checksum-byte position (the
   decoded payload is `resp[0..resp_len-2]`). If libaribb25 ever starts
   rejecting responses, strip that last byte in `IFDHTransmitToICC`.
4. **No ATR**: B-CAS cards have no ISO 7816 ATR, so `IFDHPowerICC`
   returns success with a zero-length ATR. If `SCardConnect` in pcsc-lite
   refuses to connect, a synthetic ATR will be needed.
5. **FIFO chunk registers above `0xc0`**: the DLL's chunk-base register
   sequence wraps (`0x40, 0x80, 0xc0, 0x00, ...`); implemented as-is from
   the disassembly, worth verifying with long ECM/EMM blocks (>0xc0 bytes).
