# ConnectCard — B-CAS 卡槽检测/上电/初始化协议

Reverse engineering of `ConnectCard` at `0x10005700` in
`BonDriver_HDUS.dll` (PE32 i386, disassembly `/tmp/bd.asm`,
`objdump -d`). Cross-references:
`~/projects/recfriio/recfriior5/Hdus.cpp`,
`/tmp/mtvhd/mtvhd-v1.c`, `/tmp/mtvhd/mtvhd-v2.c`, `/tmp/mtvhd/mtvhd.h`.

**Conventions in this document**

- `FACT` = directly observed in disassembly / source.
- `HYPOTHESIS` = inferred meaning, not directly proven by the DLL.

---

## 1. Transport layer (FACT)

### 1.1 Helper `0x10005610` — I2C-only wrapper used by ConnectCard

Register calling convention (thiscall-ish, callee-saved regs):

| Register / stack | Meaning |
|---|---|
| `eax` | `this` (device object) |
| `bl` | `bRequest` (`0x02` = read, `0x03` = write) |
| `edi` | `wValue` = `(reg << 8) \| slave_addr` |
| `esi` | `wIndex` = `0` for reads, value byte for writes |
| stack `push buf` | 2-byte response buffer: `buf[0]=status (1=ok)`, `buf[1]=value (reads)` |

It builds USB setup packet `{0xC0, bRequest, wValue, wIndex, wLength=2}`
and calls `WinUsb_ControlTransfer` via function pointer at `obj+0x500`
(handle from `obj+0x4f4 -> +0x10`), when `obj+0x4ec != -1`.
When `obj+0x4ec == -1` it falls back to `DeviceIoControl` with
IOCTL `0x22000c`, input size `0x17`, output size `0x10`
(`*0x1000c09c` = `DeviceIoControl`). On `buf[0]==0` it logs
`"Err 00 %02x %02x %02x %02x %02x %02x %02x\n"` (`0x1000d480`) via
`0x10001050` and still returns the transfer result in `eax`.

This matches recfriio `Hdus::send_ctl(data, fd, req, val, idx, len)`
(`Hdus.cpp:537-547`, `USB_DIR_IN|USB_TYPE_VENDOR|USB_RECIP_DEVICE`)
and mtvhd `mtvhd_i2c_read_byte` (`req 0x02, wValue=(reg<<8)|addr`)
/ `mtvhd_i2c_write_byte` (`req 0x03, wIndex=val`), except the DLL
helper hard-codes `wLength=2`.

### 1.2 Helper `0x10007fb0` — general control wrapper (power / probe)

Signature (same as recfriio `send_ctl`):

```
eax=this, ebx=wValue, ecx=wLength, edi=buf,
stack arg1=bRequest, stack arg2=wIndex
```

Used for GPIO (`bRequest=0x08, wLength=1`) and for the ASIE probe
read (`bRequest=0x02, wLength=2`). Same WinUsb / DeviceIoControl
`0x22000c` dual path, same `buf[0]=status` convention.

### 1.3 Imports used by ConnectCard (FACT, from PE import table)

| Address | Import | Use in ConnectCard |
|---|---|---|
| `*0x1000c000` | `WaitForSingleObject` | mutex acquire, 1000 ms |
| `*0x1000c098` | `ReleaseMutex` | mutex release on every exit when locked |
| `*0x1000c09c` | `DeviceIoControl` | fallback transport in helper |
| `*0x1000c0a0` | `Sleep` | delays: `0x32`=50 ms, `0xc8`=200 ms |

---

## 2. Object fields (FACT: offsets; values traced to setters)

Constructor at `0x10009450` zeroes `obj+0x4e0..0x4ea`,
sets `obj+0x4ec=-1`, `obj+0x52c=-1` (mutex), `obj+0x534=0` (card buffer ptr).
Other setters: `0x1000965f`/`0x10009f19`/`0x1000a20f` set `obj+0x4e4=0xff`,
`0x100097cf`/`0x1000a038` set `obj+0x4e4=bl` (config/EEPROM path).

| Offset | Size | Meaning | Values (FACT) |
|---|---|---|---|
| `+0x4e2` | u16 | device ID word | `0x6051` / `0x6052` select power branch (0x100080c0, 0x10008220) and DES type-3 check (0x10008571) |
| `+0x4e4` | u8 | ASIE type / state | `0xff` init/none, `0` none, `1` variant-A, `3` DES variant, `4` special card-init variant (see §4.3) |
| `+0x4e6` | u8 | ASIE I2C slave addr (raw) | `0x5a` primary, `0x5e` if type==1; fallback `+0xf0` → `0x4a` / `0x4e` (§3) |
| `+0x4e7` | u8 | crypto mode | `0x91` XOR (reg09 & 7 != 6), `0x80` DES (reg09 & 7 == 6) |
| `+0x4e8` | u8 | probe state | `0` none, `1` probed, `2` fallback slave used, `3` DES, `4` special (ConnectCard only reads it) |
| `+0x4e9` | u8 | **ready mask** for reg `0x04` | default `0x80`; DES+type1: `0x60` |
| `+0x4ea` | u8 | **present mask** for reg `0x04` | default `0x01`; DES+type1: `0x02` |
| `+0x4ec` | u32 | USB handle / transport select | `-1` = DeviceIoControl fallback, else WinUsb |
| `+0x52c` | u32 | mutex handle | `WaitForSingleObject(...,1000)` / `ReleaseMutex` |
| `+0x534` | ptr | card data buffer | `+0x08`=flag byte, `+0x0a`=length, `+0x0b..`=data (bit-reversed) |

Mask setters are only at `0x10008420` (ASIE probe, §3):
default `+0x4e9=0x80, +0x4ea=0x01`; at `0x1000854d` (type1 + DES):
`+0x4e9=0x60, +0x4ea=0x02`. No other writes to `+0x4e9/+0x4ea` exist
in the DLL (FACT, grep of `/tmp/bd.asm`).

---

## 3. Slave address derivation (FACT)

### 3.1 ASIE probe — function `0x10008420` (called from tuner init `0x10008cd0`)

Entry: if `obj+0x4e6 != 0` return `1` (already probed, `0x1000859c`).

1. GPIO check via helper `0x10007fb0`:
   `req=0x08, wValue=0x0000, wIndex=0, len=1`.
   Expected mask byte computed from type:
   `mask = (type==1) ? 0x80 : 0x11`
   (compiler idiom `dec/neg/sbb/and 0x91/sub 0x80` at `0x10008432`).
   Test `(buf[0] & mask) == mask`. If it fails:
   `req=0x08, wValue=(mask<<8)|0xff, wIndex=0, len=1`, then `Sleep(50)`.
   - i.e. `wValue=0x11ff` (mask `0x11`, val `0xff`) for the common case —
     identical to recfriio `Hdus.cpp:435`
     `send_ctl(...,0x08,0x11ff,...)` ("ASIE5606 on") and mtvhd-v1
     `mtvhd_asv5211_gpio(dev,0x11,0xFF)` (`mtvhd-v1.c:178`).
   - `wValue=0x80ff` for type==1 (HYPOTHESIS: second adapter / ASIE5607 variant).
2. Set `state(+0x4e8)=1`, `present(+0x4ea)=1`, `ready(+0x4e9)=0x80`.
3. Probe read: `req=0x02, wValue=0x0900|slave, wIndex=0, len=2`,
   `slave = (type==1) ? 0x5e : 0x5a`.
   This is the same reg-`0x09` probe as recfriio `Hdus.cpp:442`
   (`0x0900|saddr`) and mtvhd `mtvhd_asie560x_probe(adap,0x09)`.
4. If `buf[0] != 1` (no ACK): `slave += 0xf0` (`0x5a→0x4a`, `0x5e→0x4e`),
   `state=2`, retry the same reg-`0x09` read. If still `buf[0]!=1`:
   `slave=0, state=0`, return `0`.
   - Matches mtvhd.h: `MTVHD_I2C_ASIE5606_OLD=0x5A`, `MTVHD_I2C_ASIE5606=0x4A`,
     `MTVHD_I2C_ASIE5607_1=0x5E`, `MTVHD_I2C_ASIE5606_1=0x4E`;
     recfriio `saddr=0x5a`, fallback `saddr=0x4a` (`Hdus.cpp:309,444`).
5. `crypto = buf[1] & 0x07`:
   - `==6` → DES: `+0x4e7=0x80`; if type==1 also `ready=0x60, present=0x02,
     state=0`; else if device word `+0x4e2 != 0x6051` then `state=3`.
   - `!=6` → XOR: `+0x4e7=0x91` (mtvhd-v1 treats `4`=XOR, `6`=DES;
     DLL treats everything except `6` as XOR — FACT).
   Return `1`.

### 3.2 Card-reader slave used by ConnectCard (FACT)

At ConnectCard entry (`0x1000570c`) and identically in `SendCard`
(`0x10005bc0`):

```
al = obj+0x4e4                       ; type
if (al == 0 || al == 0xff)
    card_slave = obj+0x4e6           ; raw
else
    card_slave = obj+0x4e6 - 4       ; raw - 4
```

| Raw ASIE slave (`+0x4e6`) | Card slave (`raw-4`) |
|---|---|
| `0x5a` | `0x56` |
| `0x5e` | `0x5a` |
| `0x4a` (fallback) | `0x46` |
| `0x4e` (fallback) | `0x4a` |

HYPOTHESIS: the B-CAS reader is a separate I2C device whose address is
4 below the ASIE5606/5607 address on the same bus. The four
type-4-only writes (§5, steps 2–5) deliberately use the **raw** slave,
all other ConnectCard transfers use `card_slave`.

---

## 4. ConnectCard entry conditions and control flow (FACT)

Signature: `ConnectCard(this, lock_flag)` — `this` at `ebp+8`,
`lock_flag` at `ebp+0xc`. Callers:

- `0x10001789`: `ConnectCard(eax, 1)` — lock enabled; on return `>=0`
  calls `0x10005030` and stores result at caller-object `+0xf54`.
- `0x10005d72` (in `SendCard` `0x10005bc0`): `ConnectCard(eax, 0)` —
  no lock; on return `<0` logs `"SendCard: Reader not ready\n"`
  (`0x1000d590`) and returns `-2`.

### 4.1 Mutex

If `lock_flag != 0`:
`WaitForSingleObject(obj+0x52c, 1000)` (`0x10005749`).
If result `== 0x102` (`WAIT_TIMEOUT`): return `-6` (`0xfffffffa`),
no log. Every other exit path calls `ReleaseMutex(obj+0x52c)`
(`*0x1000c098`) before returning when `lock_flag != 0`.

### 4.2 No-device gate

If `obj+0x4e8 == 0`: return `-3` (`0xfffffffd`) immediately —
**zero USB transfers, no log** (`0x10005736` → `0x100057eb`).

### 4.3 First status read and three-way decision (FACT)

One transfer: `req=0x02, wValue=0x0400|card_slave, wIndex=0, len=2`.
Let `d = buf[1]` (reg `0x04` value), `ready = obj+0x4e9`,
`present = obj+0x4ea`.

```
if ((d & ready) == ready)          ; at 0x1000578d, jne 0x100057b9
    ; already ready -> success, no further transfers
    goto SUCCESS                   ; 0x10005bb1
else if ((d & present) == present) ; at 0x100057b9, je 0x100057f7
    ; present but not ready -> run init sequence (§5)
    goto INIT
else
    log "ConnectCard: Card not present\n"   ; 0x1000d4d8
    return -3                    ; 0x100057eb
```

SUCCESS epilogue (`0x10005bb1`): return `obj+0x4e4 + 1`
(e.g. type `1`→`2`, type `3`→`4`, type `4`→`5`).

Return codes summary (FACT):

| Code | Meaning | Log |
|---|---|---|
| `type+1` (>=1) | success (already ready, or init completed) | — |
| `-3` (`0xfffffffd`) | `state==0`, or present-mask failed | `"Card not present"` only in the latter case |
| `-6` (`0xfffffffa`) | mutex timeout, or reader-not-ready (§5 step 10) | `"reader not ready"` only in the latter case |

---

## 5. Full transfer sequence (FACT)

Notation: `R reg` = `req 0x02, wValue=(reg<<8)|slave, wIndex=0`;
`W reg=v` = `req 0x03, wValue=(reg<<8)|slave, wIndex=v`.
`C` = card slave (`raw-4`), `A` = raw ASIE slave.
All transfers are exactly 2 bytes (`buf[0]=status`, `buf[1]=data`).

Steps 2–5 run **only if `obj+0x4e8 == 4`** (`0x100057f7`); otherwise skipped.

| # | Addr | Op | Slave | wValue | wIndex | Notes |
|---|---|---|---|---|---|---|
| 1 | `0x10005785` | R `0x04` | C | `0x0400\|C` | `0x0000` | status probe, §4.3 |
| 2 | `0x10005823` | W `0x3b=0x0a` | A | `0x3b00\|A` | `0x000a` | type-4 only |
| 3 | `0x10005848` | W `0x3c=0xa0` | A | `0x3c00\|A` | `0x00a0` | type-4 only |
| 4 | `0x1000586d` | W `0x3d=0xaa` | A | `0x3d00\|A` | `0x00aa` | type-4 only |
| 5 | `0x10005892` | W `0x3e=0xb4` | A | `0x3e00\|A` | `0x00b4` | type-4 only |
| 6 | `0x100058ad` | W `0x00=0x08` | C | `0x0000\|C` | `0x0008` | strobe/command (HYPOTHESIS) |
| 7 | `0x100058c9` | W `0x04=ready` | C | `0x0400\|C` | `ready` (`0x80`/`0x60`) | writes ready mask back to reg04 |
| 8 | `0x100058e2` | W `0x00=0x09` | C | `0x0000\|C` | `0x0009` | strobe/command (HYPOTHESIS) |
| — | `0x100058ec` | `Sleep(50)` | — | — | — | `0x32` ms |
| 9 | `0x1000592b` | R `0x39` | C | `0x3900\|C` | `0x0000` | poll loop, see below |
| 10 | `0x1000593f` | R `0x03` | C | `0x0300\|C` | `0x0000` | poll loop; `buf[1]` = length/counter |
| 11 | `0x10005964` | R `0x04` | C | `0x0400\|C` | `0x0000` | ready re-check inside loop |
| — | `0x10005919` | `Sleep(50)` | — | — | — | each loop iteration |
| 12 | `0x100059ea` | W `0x3a=0x00` | C | `0x3a00\|C` | `0x0000` | |
| 13 | `0x10005a16` ×N | R `0x40+i` | C | `((0x40+i)<<8)\|C` | `0x0000` | N = length, §5.1 |
| 14 | `0x10005a65` | W `0x00=0x0c` | C | `0x0000\|C` | `0x000c` | |
| 15 | `0x10005a80` | W `0x01=0x01` | C | `0x0100\|C` | `0x0001` | |
| 16 | `0x10005a9f` | W `0x02=0x05` | C | `0x0200\|C` | `0x0005` | |
| 17 | `0x10005abb` | W `0x38=0x00` | C | `0x3800\|C` | `0x0000` | |
| 18 | `0x10005acf` | W `0x39=0x00` | C | `0x3900\|C` | `0x0000` | |
| 19 | `0x10005ae9` | W `0x40=0x00` | C | `0x4000\|C` | `0x0000` | |
| 20 | `0x10005b08` | W `0x41=0x83` | C | `0x4100\|C` | `0x0083` | |
| 21 | `0x10005b27` | W `0x42=0x80` | C | `0x4200\|C` | `0x0080` | |
| 22 | `0x10005b46` | W `0x43=0x7f` | C | `0x4300\|C` | `0x007f` | |
| 23 | `0x10005b65` | W `0x44=0x7c` | C | `0x4400\|C` | `0x007c` | |
| 24 | `0x10005b7e` | W `0x00=0x0a` | C | `0x0000\|C` | `0x000a` | |
| — | `0x10005b8b` | `Sleep(200)` | — | — | — | `0xc8` ms |
| — | `0x10005b9e` | `obj+0x534[8]=0` | — | — | — | then SUCCESS return |

No `bRequest=0x08` (GPIO/power) transfer occurs anywhere in
ConnectCard — power is handled before ConnectCard (§6).

### 5.1 Poll loop (§5 steps 9–11, FACT)

Initial state: `prev_reg03 = -1`, `retry = 0`, `len = 13` (`0x0d`).

```
loop:
    Sleep(50)                                   ; 0x10005919
    R 0x39            ; value discarded
    d3 = R 0x03       ; buf[1]
    if (d3 == prev_reg03) goto DONE             ; stable -> exit 0x100059c7
    prev_reg03 = d3
    d4 = R 0x04
    if ((d4 & ready) != ready)
        log "ConnectCard: reader not ready\n"   ; 0x1000d518
        return -6                               ; 0x100059b7
    retry++
    if (retry >= 0x14) goto DONE                ; 20 retries, 0x100059c3
    goto loop
DONE:
    len = min(prev_reg03, 13)                   ; at 0x100059c7: if <13 use d3
```

Consequences (FACT):

- Total added delay in the loop: `50 ms` (before first poll) +
  `50 ms` per iteration, max `20 × 50 ms = 1000 ms` plus the final
  `Sleep(200)`.
- "Reader not ready" is **not** a timeout: a single poll where the
  ready bits are clear aborts immediately with `-6`. The 20-count
  only bounds how long reg `0x03` may keep changing while ready.
- Exit by stability (`d3 == prev`) is the normal path.

### 5.2 Card data read (§5 step 13, FACT)

`len` is stored at `obj+0x534 + 0x0a`. For `i = 0 .. len-1`:

```
d = R (0x40 + i)                     ; req 0x02
obj+0x534[0x0b + i] = BITREVERSE8[d] ; table at 0x1000f2a8
```

`BITREVERSE8` table at `0x1000f2a8` (FACT, first entries
`00 80 40 c0 20 a0 60 e0 10 90 …`): entry `n` is the bit-reversal of
`n`. HYPOTHESIS: the card interface is LSB-first on the wire and the
driver normalises to MSB-first bytes; the resulting buffer is most
likely the B-CAS card's ATR / initial data block (HYPOTHESIS).

---

## 6. Power (FACT: no GPIO in ConnectCard; power is in tuner init)

ConnectCard itself issues **no** `req 0x08` transfer. Power must already
be on when ConnectCard runs. The DLL's power sequences are in
`0x100080c0` (power-on) and `0x10008220` (power-off), both via helper
`0x10007fb0` with `req=0x08, wIndex=0, len=1, wValue=(mask<<8)|val`,
branching on device word `obj+0x4e2`:

**Power-on `0x100080c0`, non-`0x6051/0x6052`:**

| wValue | mask / val | Sleep after |
|---|---|---|
| `0x0808` | `0x08` / `0x08` | 50 ms |
| `0x0800` | `0x08` / `0x00` | 150 ms |
| `0x11ff` | `0x11` / `0xff` | 50 ms |
| `0xd500` | `0xd5` / `0x00` | 50 ms |
| `0xd5ff` | `0xd5` / `0xff` | 100 ms |

**Power-on `0x100080c0`, `0x6051/0x6052`:**
`0xd500` (50 ms), `0x0800`, `0x4040` (5 ms), `0x8080` (50 ms),
`0x0404` (50 ms).

**Power-off `0x10008220`, non-`0x6051/0x6052`:**
`0xd400`, `0x0808`, Sleep 50 ms.

**Power-off `0x10008220`, `0x6051/0x6052`:**
Sleep 50 ms, `0x1000`, Sleep 100 ms, `0xdd0c`, Sleep 100 ms.

Cross-references (FACT):

- recfriio `Hdus.cpp:326-334` tuner-on:
  `0xd500, 0x0800, 0x4040, 0x8080, 0x0404` with the same sleeps —
  identical to the `0x6051/0x6052` branch.
- recfriio `Hdus.cpp:435-440` ASIE-on:
  `0x11ff` (50 ms), `0x1100` (50 ms), `0x11ff` (100 ms).
- mtvhd-v1 `mtvhd_asv5211_gpio(d,0x11,0xFF)` (`mtvhd-v1.c:178`) —
  same `(mask<<8)|val` encoding, mask `0x11`.
- mtvhd-v2 BCAS mode (`mtvhd-v2.c:202-222,265-272`): adapter-0 ASIE
  power mask is `0x10` (adapter-1: `0x80`), and tuner+ASIE-0 power-on
  is a single `gpio(0x55,0xFF)` (bits `0x01|0x04|0x10|0x40`); with
  `CONFIG_DVB_USB_MTVHD_BCAS` adapter-0 ASIE power is **kept on**
  at power-off so the B-CAS card stays accessible. HYPOTHESIS: the
  HDUS `0x11` mask bit `0x10` is the same ASIE/card power bit, and
  the DLL's separate `0x11ff` step is the HDUS equivalent of
  mtvhd-v2's `gpio(0x10,0xFF)` / `gpio(0x55,0xFF)`.

---

## 7. Register map (hypotheses marked)

All registers below are on the card slave `C` except `0x3b–0x3e`
(type-4 only, raw ASIE slave `A`).

| Reg | Access in ConnectCard | Hypothesised meaning |
|---|---|---|
| `0x00` | W `0x08`, `0x09`, `0x0c`, `0x0a` | command/strobe register (HYPOTHESIS) |
| `0x01` | W `0x01` | enable/config (HYPOTHESIS) |
| `0x02` | W `0x05` | config (HYPOTHESIS) |
| `0x03` | R (polled) | card data length / transfer counter; capped at 13; loop exits when stable (FACT for behaviour, HYPOTHESIS for meaning) |
| `0x04` | R (status), W `ready` | status: bit `0x01`/`0x02` = card present, bits `0x80`/`0x60` = reader ready (FACT for bits; HYPOTHESIS for naming) |
| `0x09` | R (probe, §3) | ASIE ID/crypto selector: `&7 == 6` → DES, else XOR (FACT, matches mtvhd) |
| `0x38` | W `0x00` | config, cleared at end (HYPOTHESIS) |
| `0x39` | R (polled), W `0x00` | busy/ready poll paired with reg `0x03` (HYPOTHESIS) |
| `0x3a` | W `0x00` | written 0 before the `0x40+` data read (HYPOTHESIS) |
| `0x3b–0x3e` | W `0x0a,0xa0,0xaa,0xb4` (type-4 only, slave A) | ASIE card-interface config: clock/baud/timing (HYPOTHESIS) |
| `0x40–0x44` | R `0x40..0x40+len-1`, W `0x00,0x83,0x80,0x7f,0x7c` | card data block in / CAS config out (HYPOTHESIS) |

Note the same register numbers appear in mtvhd's *demodulator* init
tables (`mtvhd-v1.c:251-277`, `mtvhd-v2.c:288-…`) at I2C address
`0x20` — those are a different device (demodulator) and must not be
confused with the card/ASIE registers here, which live at
`0x46–0x5a`.

---

## 8. Presence-bit specification (FACT)

Reg `0x04` read at `card_slave`:

| Mode | Ready mask (`+0x4e9`) | Present mask (`+0x4ea`) | Decision |
|---|---|---|---|
| default (XOR, or DES non-type1) | `0x80` (bit 7) | `0x01` (bit 0) | `(d&0x80)==0x80` → ready; else `(d&0x01)==0x01` → present, run init; else "Card not present" |
| DES + type1 | `0x60` (bits 6:5) | `0x02` (bit 1) | same, with `0x60`/`0x02` |

The poll loop (§5.1) re-tests only the ready mask against reg `0x04`;
the present mask is tested once, on the first read.

---

## 9. Probe recipe (userspace / usbfs)

Prerequisite: device open, tuner powered (§6). All transfers below are
USB control transfers with setup packet:

```
bmRequestType = 0xC0  (vendor, device, IN)
bRequest      = req
wValue, wIndex, wLength = 2   (1 for GPIO steps)
data[2]: data[0]=status (expect 1), data[1]=value
```

Equivalent C:

```c
static int ctl(int fd, uint8_t req, uint16_t wValue, uint16_t wIndex,
               uint16_t len, uint8_t *buf) {
    struct usbdevfs_ctrltransfer t = {
        .requesttype = 0xC0, .request = req,
        .value = wValue, .index = wIndex, .length = len,
        .timeout = 1000, .data = buf,
    };
    return ioctl(fd, USBDEVFS_CONTROL, &t);
}
#define R(fd,reg,slave)  ctl(fd,0x02,((reg)<<8)|(slave),0,2,buf)  /* buf[1]=data */
#define W(fd,reg,slave,v) ctl(fd,0x03,((reg)<<8)|(slave),(v),2,buf)
```

### Step 0 — power (only if not already powered by the driver)

```
ctl(fd, 0x08, 0x0808, 0, 1, buf);  usleep(50_000);
ctl(fd, 0x08, 0x0800, 0, 1, buf);  usleep(150_000);
ctl(fd, 0x08, 0x11ff, 0, 1, buf);  usleep(50_000);   /* ASIE/card power */
ctl(fd, 0x08, 0xd500, 0, 1, buf);  usleep(50_000);
ctl(fd, 0x08, 0xd5ff, 0, 1, buf);  usleep(100_000);
```

### Step 1 — find ASIE slave (reg 0x09 probe)

```
for slave in {0x5a, 0x4a}:            /* HDUS; use {0x5e,0x4e} for type-1 variant */
    R(fd, 0x09, slave);
    if (buf[0] == 1) { found = slave; crypto = buf[1] & 7; break; }
if (!found) -> no ASIE; stop.
ready_mask   = (crypto==6 && type1) ? 0x60 : 0x80;
present_mask = (crypto==6 && type1) ? 0x02 : 0x01;
card_slave   = found - 4;             /* 0x56 / 0x46 for 0x5a / 0x4a */
```

### Step 2 — presence test (single transfer, the whole ConnectCard decision)

```
R(fd, 0x04, card_slave);
if (buf[0] != 1)          -> reader/ASIE not answering; stop.
d = buf[1];
if ((d & ready_mask) == ready_mask)
    -> already ready; card present, no init needed. stop (success).
if ((d & present_mask) != present_mask)
    -> "Card not present". stop.
```

### Step 3 — init (only if present but not ready)

```
W(fd, 0x00, card_slave, 0x08);
W(fd, 0x04, card_slave, ready_mask);
W(fd, 0x00, card_slave, 0x09);
usleep(50_000);

/* poll loop, max 20 iterations */
prev = -1; retry = 0;
for (;;) {
    usleep(50_000);
    R(fd, 0x39, card_slave);            /* discarded */
    R(fd, 0x03, card_slave); d3 = buf[1];
    if (d3 == prev) break;
    prev = d3;
    R(fd, 0x04, card_slave);
    if ((buf[1] & ready_mask) != ready_mask)
        -> "reader not ready"; stop (error -6).
    if (++retry >= 20) break;
}
len = min(prev, 13);

W(fd, 0x3a, card_slave, 0x00);
for (i = 0; i < len; i++) {
    R(fd, 0x40 + i, card_slave);
    card[i] = bitreverse8(buf[1]);
}
W(fd, 0x00, card_slave, 0x0c);
W(fd, 0x01, card_slave, 0x01);
W(fd, 0x02, card_slave, 0x05);
W(fd, 0x38, card_slave, 0x00);
W(fd, 0x39, card_slave, 0x00);
W(fd, 0x40, card_slave, 0x00);
W(fd, 0x41, card_slave, 0x83);
W(fd, 0x42, card_slave, 0x80);
W(fd, 0x43, card_slave, 0x7f);
W(fd, 0x44, card_slave, 0x7c);
W(fd, 0x00, card_slave, 0x0a);
usleep(200_000);
/* success */
```

Type-4 variant (HYPOTHESIS: only for `state==4` devices): insert
before `W 0x00=0x08`, using the **raw** ASIE slave:

```
W(fd, 0x3b, asie_slave, 0x0a);
W(fd, 0x3c, asie_slave, 0xa0);
W(fd, 0x3d, asie_slave, 0xaa);
W(fd, 0x3e, asie_slave, 0xb4);
```

`bitreverse8` table (first 16 entries, full table at DLL `0x1000f2a8`):
`00 80 40 c0 20 a0 60 e0 10 90 50 d0 30 b0 70 f0`.
