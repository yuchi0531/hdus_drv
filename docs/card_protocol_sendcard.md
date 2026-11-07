# HDUS B-CAS Card Protocol — SendCard Reverse Engineering

Source: `BonDriver_HDUS.dll` (HDUS_Driver_Windows, 2009-09-29), disassembly `/tmp/bd.asm`
(objdump, PE32 i386, ImageBase `0x10000000`).
SendCard `0x10005bc0`, helper USB control transfer `0x10005610`,
ConnectCard `0x10005700`, S-box `.data` `0x1000f2a8` (file off `0xd8a8`).
Table extracted to `tools/card_sbox.h`.

> **Hypothesis / certainty marks:** [FACT] = directly read from disassembly.
> [INF] = strong inference (consistent register/offset use, caller shape).
> [HYP] = plausible meaning, not provable from static code alone (hardware regs).

## 1. USB helper `0x10005610` [FACT]

`this` in `eax`, args in registers at call sites: `bl=bRequest`, `edi=wValue`,
`esi=wIndex`, stack arg = 2-byte buffer pointer.

* If `obj+0x4ec != -1`: `DeviceIoControl` path (obj+0x4f4 vtbl+0x10, obj+0x500).
* Else: direct control transfer via function pointer at `obj+0x500` / `obj+0x20c`,
  setup packet `{0xC0, bRequest=bl, wValue=edi, wIndex=esi, wLength=2}` (8 bytes on stack).
* Buffer: `buf[0]=status, buf[1]=data` [INF — callers read `buf+1`/`buf[5]` after
  the push-adjusted offset, e.g. `0x45(%esp)` for a buffer at `esp+0x40`].
* On `buf[0]==0` failure it logs (`0x1000d480` "Err 00 %02x …") and returns `eax` (Win32 result).

Convention used everywhere below (same as mtvhd `i2c_read/write_byte`) [INF]:

| op | bRequest | wValue | wIndex | meaning |
|---|---|---|---|---|
| read reg | `0x02` | `(reg<<8)\|slave` | `0` | `buf[1]` = reg value |
| write reg | `0x03` | `(reg<<8)\|slave` | `value` | write `value` to `reg` |

`slave` = `obj+0x4e6` (u8, low byte of wValue). A derived byte (see §3) is
`obj+0x4e4`-based: if `obj+0x4e4 ∈ {0,0xff}` use `obj+0x4e6`, else `obj+0x4e6-4`,
stored at `esp+0x1b` and used as the wValue low byte in the first poll only;
all later ops use `obj+0x4e6` directly (`esp+0x1c`). [FACT for offsets, INF for "slave addr"]

`obj+0x4e9` / `obj+0x4ea` [FACT offsets]: status masks (see §5).
`obj+0x52c`: mutex/handle passed to `WaitForSingleObject(.,1000)` (`0x1000c000`),
`ReleaseMutex` (`0x1000c098`), `Sleep` (`0x1000c0a0`).
`obj+0x534`: pointer to a session struct; `+8` = toggle/state byte (§3),
ConnectCard also uses `+0xa` (len) and `+0xb…` (decoded data).

## 2. SendCard signature [FACT offsets, INF names]

Thiscall, `ecx=this` (card object), stack args (ebp-relative):

```c
int SendCard(this,            // ebp+0x08
             const u8 *req,   // ebp+0x0c  memcpy src (0x1000c0f4 = memcpy import)
             int req_len,     // ebp+0x10  (ebx; +3 added)
             const u8 *extra, // ebp+0x14  (esi; may be NULL)
             int extra_len,   // ebp+0x18  (edx; may be 0)
             u8 *resp,        // ebp+0x1c  (saved esp+0x30)
             int resp_max);   // ebp+0x20  (by value, overwritten with actual len)
```

Caller in the DLL (`0x10004eda`, vtbl slot `this+0x2f8` = `0x10005bc0`):
pushes resp-buffer, a small byte (`0x128(%esp)`), `0, 0`, resp-buf2, `0x10c`
in some order — the exact caller arg order is partially obscured by its own
frame offsets, but `req/req_len` (memcpy), `extra/extra_len` (payload loop),
`resp/resp_max` (RX store / length cap) usage inside SendCard is unambiguous.

Return values [FACT]: `>=0` = resp_len on success; `-2` reader-not-ready;
`-3` (`0xfffffffd`) card-not-present; `-6` (`0xfffffffa`) transmit-failed /
data-error / mutex-timeout. Log strings (UTF-16LE, `.rdata`):

| VA | string | use |
|---|---|---|
| `0x1000d558` | `SendCard: Card not present\n` | -3 path |
| `0x1000d590` | `SendCard: Reader not ready\n` | -2 path |
| `0x1000d5c8` | `SendCard: Request Length = %d\n` | RX len == 4 |
| `0x1000d608` | `SendCard: transmit failed\n` | resp_len<=0 path |
| `0x1000d640` | `SendCard: data error\n` | checksum mismatch |

## 3. APDU frame build (stack buffer `esp+0x140`) [FACT]

```
frame[0]      = 0x00                     ; movb $0,0x148(%esp) with 2 pushes active
frame[1]      = obj+0x534[8]             ; state byte; after read: state ^= 0x40, store back
frame[2]      = (u8)(frame_len - 3)      ; see below
frame[3..]    = req[0..req_len-1]        ; memcpy(dst=frame+3, req, req_len)
[if extra && extra_len]
frame[3+req_len]        = (u8)extra_len  ; single length-prefix byte (low byte of dword)
frame[4+req_len ..]     = extra[0..extra_len-1]
frame[3+req_len+1+extra_len] = 0x00      ; NUL terminator
frame_len (esp+0x1a)    = 3+req_len (+1+extra_len+1 if extra)   ; includes NUL when extra present
frame[2]      = frame_len - 3            ; = req_len, or req_len+extra_len+2 with extra
```

Note: without `extra`, no NUL is written and `frame_len=3+req_len`.
With `extra`, the NUL **is** part of `frame_len` and is included in the
checksum loop (§4) — `sbox[0]=0`, xor contribution 0, so it is harmless [FACT].

`frame_len` is a byte (al) — frames >255 bytes wrap [FACT, practical B-CAS
APDUs are < 200].

### What SendCard is called with — libaribb25 cross-ref

`~/projects/recfriio/arib25/b_cas_card.c` (standard B-CAS / ARIB TR-B14 APDUs,
sent via `SCardTransmit` T=1 there; HDUS sends the same APDUs through the
proprietary USB/I2C framing above):

| libaribb25 | APDU | mapping to SendCard [INF] |
|---|---|---|
| `INITIAL_SETTING_CONDITIONS_CMD` (connect) | `90 30 00 00 00` (5) | `req` len 5 |
| `CARD_ID_INFORMATION_ACQUIRE_CMD` | `90 32 00 00 00` (5) | `req` len 5 |
| `POWER_ON_CONTROL_INFORMATION_REQUEST_CMD` | `90 80 00 00 01 00 00` (7, byte5 = index) | `req` len 7 |
| `ECM_RECEIVE_CMD_HEADER` | `90 34 00 00` + `len_byte` + ECM data + `00` | `req`=header(4), `extra`=ECM data — exactly the length-prefixed `extra` segment + NUL above |
| `EMM_RECEIVE_CMD_HEADER` | `90 36 00 00` + `len_byte` + EMM data + `00` | same `extra` pattern |

So the `extra` path exists for ECM/EMM: caller passes the 4-byte header as
`req` and the ciphertext block as `extra`. Responses expected: ECM ≥ 25 bytes
(16-byte scramble key at `rbuf+6`, code at `rbuf+4`), EMM ≥ 6, ID ≥ 19,
power-on-control ≥ 18 (libaribb25 checks) — resp buffer in HDUS caller is
`0x10c` max [FACT at caller].

## 4. Checksum / S-box transform [FACT]

Table `0x1000f2a8`: **all 256 bytes, verified `sbox[i] == bitreverse8(i)`**
(see `tools/card_sbox.h`).

TX (`0x10005df0`, over `frame[0..frame_len-1]`):

```c
u8 dl = 0;
for (i = 0; i < frame_len; i++) {
    u8 b = hdus_card_sbox[frame[i]];
    frame[i] = b;
    dl ^= b;                       // XOR of *transformed* bytes
}
frame[frame_len] = dl;             // checksum appended
frame_len++;
```

RX decode (`0x100062f0`, over `resp[0..resp_len-2]`):

```c
u8 dl = 0;
for (i = 0; i < resp_len - 1; i++) {
    u8 raw = resp[i];
    dl ^= raw;                     // XOR of *raw* bytes (before transform)
    resp[i] = hdus_card_sbox[raw];
}
// final check: resp[resp_len-1] == dl
```

ConnectCard (`0x10005a29`): `decoded[i] = sbox[raw[i]]`, no checksum.
The TX/RX xor-order asymmetry is [FACT] as coded; whether the card hardware
expects it that way is [INF] (it is what both loops do).

## 5. Register sequence (in order) [FACT ops; reg meanings HYP]

`W(reg,val)` = req `0x03`, wValue `(reg<<8)|slave`, wIndex `val`.
`R(reg)` = req `0x02`, wValue `(reg<<8)|slave`, wIndex `0`, result `buf[1]`.

### 5.1 Pre-TX poll & presence check

1. `WaitForSingleObject(obj+0x52c, 1000)`; if `==0x102` (timeout) return `-6`.
2. `d = R(0x04)`
3. `mask = obj+0x4e9 | obj+0x4ea`; if `(d & mask) == mask` → ready, continue.
4. Else if `(d & obj+0x4ea) == obj+0x4ea` → call ConnectCard(`this,0`);
   if it returns `<0` log "Reader not ready", return `-2`; else continue.
5. Else if `(d & obj+0x4e9) == obj+0x4e9` → `W(0x04,0)` (clear), then
   log "Card not present", return `-3`.
6. Else log "Card not present", return `-3`.

[HYP] Reg `0x04` is a status/interrupt register; `0x4e9` bit = card-present,
`0x4ea` bit = reader-ready; connect-card re-init when only reader bit is set.

### 5.2 TX setup & write

7. Build frame (§3), sbox-transform + checksum (§4), toggle `obj+0x534[8] ^= 0x40`.
8. `W(0x02, 0x0c)`; `W(0x38, 0)`
9. Chunked write, chunk size `0x40`, chunk base starts at `0x40`:
   - if `chunk >= 0x80` and flag-byte (`esp+0x13`, init `0xff`) `!= 1`:
     `W(0x3a, 1)`, `chunk -= 0x40`
   - else if `chunk < 0x80` and flag `!= 0`: `W(0x3a, 0)` then write chunk
     (i.e. the `0x3a` write happens once before the first chunk, and with
     value 1 before subsequent 0x40-blocks when total > 0x80).
   - per byte `i` in chunk: `W(chunk_base, frame[chunk_base-0x40+i])`
     — i.e. wValue reg = `0x40, 0x80, 0xc0, …`, wIndex = frame byte.
     [FACT: `edi=(offset<<8)|slave`, `esi=frame byte`]
   - loop while `offset+0x40 < frame_len` (signed).
10. `W(0x00, 0x0a)` (`esi=0x0a`, wValue = slave only); `Sleep(50)`.

[HYP] Regs `0x40/0x80/…` are a FIFO/RAM window of the card-media controller
(MTVHD-style), `0x3a` = FIFO commit/trigger, `0x38`/`0x02` = mode/timing setup,
`0x00` = start command. The exact hardware meaning cannot be proven statically.

### 5.3 RX poll, length probe, read

11. Poll loop (counter `esp+0x24`, limit `0x14` = 20, `Sleep(50)` each pass):
    - `R(0x39)` twice-ish (second read reuses wValue from first buffer dword —
      disassembly at `0x10005fb6/0x10005fcb` is ambiguous [FACT ambiguity];
      treat as "read status regs `0x39`/`0x03` pair, wValue `slave|0x3900`
      and `slave|0x0300` were both prepared on stack").
    - `d2 = R(0x39)` data; if `d2 == counter` break; `counter++`;
      if `counter > 3` reset to 0 on mismatch; if counter reaches `0x14`
      proceed anyway (timeout does not abort here) [FACT control flow].
    - Mismatch sub-checks repeat the `0x4e9/0x4ea` mask logic with
      `W(0x04,0)` / "Card not present" / ConnectCard / "Reader not ready"
      exits, same as §5.1.
12. Length probe: `d = R(0x42)`; `len = sbox[d] + 4`.
    - If `len >= 3` and `len < resp_max`: write `W(0x3a,0)` if flag, then
      re-read `R(0x42)`, recompute `len = sbox[d]+4`, update `resp_max` slot
      (`ebp+0x20`) to `len` (it is a max, so it *shrinks* to actual).
    - If `len == 4`: log "Request Length = %d" (args 2,2 — likely a debug
      artefact, value itself is not the format arg) [FACT].
13. RX data loop, chunk base starts `0x40`, chunk size `0x40`:
    - Same `0x3a` interleave as TX (value 0 before first chunk, value 1
      between 0x40-blocks when `chunk>=0x80`).
    - `resp[offset] = R(chunk_base)` per byte, `offset` increments,
      loop while `offset < resp_len`.
14. `W(0x00, 0x0c)`; if `resp_len <= 0`: `Sleep(50)`, `W(0x04,0)`,
    `ReleaseMutex(obj+0x52c)`, log "transmit failed", return `-6`.

### 5.4 Verify & finish

15. S-box decode + checksum (§4). If `resp_len>=1 && resp[resp_len-1]==dl`:
    `ReleaseMutex(obj+0x52c)`, return `resp_len`.
16. Else: `Sleep(50)`, `W(0x04,0)`, `ReleaseMutex(obj+0x52c)`,
    log "data error", return `-6`.

ConnectCard register tail (for context, runs before SendCard can succeed):
`W(0x00,0x0c)`, `W(0x01,1)`, `W(0x02,5)`, `W(0x38,0)`, `W(0x3a,0)`,
`W(0x40,0)`, `W(0x41,0x83)`, `W(0x42,0x80)`, `W(0x43,0x7f)`, `W(0x44,0x7c)`,
`W(0x00,0x0a)`, `Sleep(200)`, `obj+0x534[8]=0`.
[HYP] `0x40..0x44` with `0x83/0x80/0x7f/0x7c` are FIFO thresholds/levels:
131/128/127/124 — a descending watermark set. Same values appear in
SendCard's caller tail, confirming they are init, not per-transfer.

## 6. Port-ready pseudocode

```c
#include <stdint.h>
#include "card_sbox.h"          // tools/card_sbox.h
typedef uint8_t u8;
// Provided by the USB layer (wraps 0x10005610):
//   int usb_ctrl(u8 bRequest, u16 wValue, u16 wIndex, u8 buf[2]);
static int card_r(u8 slave, u8 reg, u8 *out) {
    u8 buf[2] = {0,0};
    int r = usb_ctrl(0x02, (reg<<8)|slave, 0, buf);
    if (r < 0 || buf[0] != 0) return -1;   // [INF] buf[0] is status
    *out = buf[1]; return 0;
}
static int card_w(u8 slave, u8 reg, u8 val) {
    u8 buf[2] = {0,0};
    int r = usb_ctrl(0x03, (reg<<8)|slave, val, buf);
    return (r < 0 || buf[0] != 0) ? -1 : 0;
}
// obj state: slave, mask_e9, mask_ea, mutex, proc_state[9]
int hdus_card_transmit(const u8 *req, int req_len,
                       const u8 *extra, int extra_len,
                       u8 *resp, int *resp_len)
{
    /* 5.1 presence poll -> -2 / -3 on failure (see §5.1) */
    /* build frame (§3) in u8 frame[600]; frame_len = 3+req_len
       (+1+extra_len+1 with extra, incl. NUL) */
    /* 4: sbox + xor checksum, append */
    /* 5.2: W(0x02,0x0c); W(0x38,0); chunked W(chunk,frame[i]) with
       W(0x3a,0/1) interleave; W(0x00,0x0a); sleep 50 */
    /* 5.3: poll R(0x39) up to 20 x sleep50; len = sbox[R(0x42)]+4;
       clamp to *resp_len; chunked R(0x40+i) into resp;
       W(0x00,0x0c) */
    /* 5.4: decode + verify -> return len, or -6 */
    return 0; /* placeholder — expand §5.1-5.4 verbatim */
}
```

Full register-for-register expansion is §5 above; the helper macros preserve
the exact `(reg<<8)|slave` / wIndex=value encoding so it drops onto
mtvhd-style `i2c_read_byte`/`i2c_write_byte` backends by replacing
`usb_ctrl` with the existing transport.
```
