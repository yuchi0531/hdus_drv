# HDUS SendCard TX/RX — Exact Register Trace

Source: `BonDriver_HDUS.dll` disassembly `/tmp/bd.asm` (PE32 i386, ImageBase
`0x10000000`), DLL binary `/home/yuchi0531/projects/hdus-linux/HDUS_Driver_Windows/BonDriver_HDUS.dll`
(only for the `.data` table dump at file-off `0xd9ac`, which is not in the
`.text`-only disassembly). SendCard `0x10005bc0`, USB helper `0x10005610`,
ConnectCard `0x10005700`, caller `0x10004db0` (call site `0x10004eda`).

> **[FACT]** = read directly from the disassembly (address given).
> **[INF]** = strong inference from register/offset use.
> Earlier doc `docs/card_protocol_sendcard.md` §5.2/§5.3 chunk model
> ("fixed reg `0x40` per chunk, chunk base `0x40,0x80,…`") is **wrong** —
> see §1/§3 for the correct model.

## 0. Helper convention (needed to read every call) [FACT]

`0x10005610`: `eax=this`, `bl=bRequest`, `edi=wValue`, `esi=wIndex`,
one stack arg = 2-byte buffer pointer (`ebp` in helper).
Setup packet built at `0x1000565b-0x10005669`: `{0x00, bl, di(LE), si(LE), 2}`.
Buffer layout: `buf[0]=status, buf[1]=data` — callers read `buf+1`
(e.g. `0x45(%esp)` while the buffer push is active = physical `buf+1`,
`0x15(%esp)` in ConnectCard where the buffer base is `esp+0x14`).

```
R(reg) = bRequest 0x02, wValue (reg<<8)|slave, wIndex 0,      result buf[1]
W(reg,val) = bRequest 0x03, wValue (reg<<8)|slave, wIndex val
```

`slave` = `obj+0x4e6` low byte, via the derived byte at `[esp+0x1b]`
(`0x4e4∈{0,0xff} ? 0x4e6 : 0x4e6-4`, `0x10005bea-0x10005c15`) copied as a
zero-extended word to `[esp+0x1c]` (`0x10005cbc`). All later ops use
`[esp+0x1c]` (physical; `0x1c(%esp)` with no extra push active,
`0x20(%esp)`/`0x18(%esp)` etc. with one push active — same slot).
Live value in the reported test: `slave=0x4a`.

**Stack-offset rule used throughout below [FACT]:** every `push buf` for a
helper call shifts subsequent `%esp` offsets by 4 until the matching
`add $4,%esp`. E.g. `[esp+0x13]` (no push) and `[esp+0x17]` (one push)
are the **same physical byte**. This resolves the two flag offsets.

---

## 1. TX loop `0x10005e7e-0x10005f50` [FACT]

### 1.1 Slot initialisation

| slot (no-push esp) | init site | value | meaning |
|---|---|---|---|
| `[esp+0x13]` (byte) | `0x10005bf4 movb $0xff,0x13(%esp)` | `0xff` | `W(0x3a)` flag: `0xff`=unset, `0`=committed-low, `1`=committed-high |
| `[esp+0x14]` | `0x10005bf0 mov %edi,0x14(%esp)` | `this(obj)` | `this` for helper calls |
| `[esp+0x1a]` (byte) | `0x10005c36` / `0x10005c7c`, then `0x10005e14 incb 0x1a(%esp)` | `frame_len` incl. checksum | total TX bytes |
| `[esp+0x1b]` (byte) | `0x10005c09/0x10005c15` | derived slave byte | see §0 |
| `[esp+0x1c]` (word/dword) | `0x10005cbc mov %di,0x1c(%esp)` | zero-extended slave | low byte of every wValue |
| `[esp+0x20]` | `0x10005e8c mov %eax,0x20(%esp)` (`eax=0x40`) | `0x40` | TX raw counter = `0x40 + byte_index` |
| `[esp+0x24]` | `0x10005e72 mov %eax,0x24(%esp)` (`eax=movzbl 0x1e(%esp)` pushed = physical `0x1a`) | `frame_len` | TX loop limit (compared at `0x10005f4c`) |
| `[esp+0x28]` | `0x10005e90 mov %ecx,0x28(%esp)` (`ecx=lea 0x140(%esp); sub $0x40,%ecx`) | `frame_ptr - 0x40` | data-source base; **never written in the TX loop** |
| `[esp+0x2c]` | `0x10005ea9 mov %eax,0x2c(%esp)` per iteration | chunk/reg base (see §1.2) | `wValue` high byte before `<<8` |
| `[esp+0x30]` | `0x10005be6 mov %eax,0x30(%esp)` | `resp` ptr (ebp+0x1c) | RX destination (see §3) |
| frame base | `0x10005e83 lea 0x140(%esp),%ecx; sub $0x40,%ecx` | `esp+0x140` | `frame[0]` (checksum store `0x10005e1c mov %dl,0x140(%esp,%esi,1)`) |

`[esp+0x17]` at `0x10005ecd`/`0x10005f02` is `[esp+0x13]` **with the
helper-buffer push active** — same flag byte. [FACT, push arithmetic]

### 1.2 Per-iteration math [FACT]

Loop head `0x10005ea0`: `eax=[esp+0x20]` (raw counter, starts `0x40`).
`[esp+0x2c]=eax` (`0x10005ea9`).

* `eax < 0x80` (`0x10005ea4`/`0x10005ead`): if flag `!=0`, `W(0x3a,0)`
  (`edi=slave|0x3a00, esi=0, bl=3`, `0x10005eb6-0x10005ed2`), flag:=0.
  Chunk base = `eax` (no adjustment).
* `eax >= 0x80`: if flag `!=1`, `W(0x3a,1)` (`esi=1`, `0x10005eeb-0x10005f06`),
  flag:=1. Then `[esp+0x2c]=eax-0x40` (`0x10005f12 lea -0x40(%eax),%ecx`).

Per-byte write `0x10005f19-0x10005f38`:

```
ecx = [esp+0x28] = frame_ptr-0x40            (constant)
edi = [esp+0x2c]                             (chunk/reg base)
si  = movzbw (%ecx,%eax,1) = word at frame_ptr-0x40+eax = frame[eax-0x40]   ; word load!
edi = (edi<<8) | [esp+0x1c]                  ; wValue = (reg<<8)|slave
bl  = 3 ; esi = si ; buf = esp+0x40 ; call 0x10005610
```

Loop tail `0x10005f41-0x10005f50`:
`eax++ ; mov %eax,0x24(%esp)` — **offset `0x24` is with the buffer push
still active**, i.e. physical `[esp+0x20]` (raw counter incremented);
`eax-=0x40 ; add $4,%esp ; cmp 0x24(%esp),%eax ; jl 0x10005ea0`
— compare is `byte_index (=eax-0x40) < frame_len (=physical [esp+0x24])`.

So with `i = byte_index = eax-0x40` (0-based):

```
reg(i) = 0x40+i        if i < 0x40   (eax 0x40..0x7f, reg 0x40..0x7f)
         i             if i >= 0x40  (eax 0x80..  , reg 0x40..; eax-0x40)
         i.e. reg = i+0x40 for the first 64 bytes, then reg = i
              (0x40..0x7f, 0x40..0x7f, 0x80..0xbf, 0xc0..)
data source = frame[i], but wIndex is loaded as a WORD:
    wIndex = frame[i] | (frame[i+1]<<8)     ; low byte = frame[i] [FACT movzbw]
```

`W(0x3a)` sequence for a monotonic counter [FACT]:
flag starts `0xff` → before byte 0, `W(0x3a,0)`, flag=0; before byte
`0x40` (eax=`0x80`), `W(0x3a,1)`, flag=1; never toggles again
(counter never goes back below `0x80`).

### 1.3 Byte-by-byte table — 9-byte frame, slave `0x4a`

Frame supplied in the task: `00 00 05 | a0 09 4c 00 00 00 + checksum`.
TX transform (`0x10005df0`, §7) replaces each byte with `sbox[byte]`
(bit-reverse), checksum = XOR of *transformed* bytes, appended raw:

```
frame  (pre) : 00    00    05    a0    09    4c    00    00    00    cksum
sbox       : 00    00    a0    05    90    32    00    00    00    —
XOR = 00^00^a0^05^90^32^00^00^00 = 0x07
frame  (TX)  : 00    00    a0    05    90    32    00    00    00    07   (10 bytes total)
```

Per-write table for the 9 pre-checksum bytes (add write #9 for the
checksum byte; `wIndex` high byte = next frame byte, `frame[10]` on the
last write is the stack byte past the frame — [FACT load, content INF=0]):

| # | i | eax (raw) | reg (`[0x2c]`) | wValue `(reg<<8)\|0x4a` | wIndex (16-bit) | bRequest | data (low byte) |
|---|---|---|---|---|---|---|---|
| 0 | 0 | `0x40` | `0x40` | `0x404a` | `0x0000` (`00\|00<<8`) | `0x03` | `0x00` |
| 1 | 1 | `0x41` | `0x41` | `0x414a` | `0xa000` (`00\|a0<<8`) | `0x03` | `0x00` |
| 2 | 2 | `0x42` | `0x42` | `0x424a` | `0x05a0` (`a0\|05<<8`) | `0x03` | `0xa0` |
| 3 | 3 | `0x43` | `0x43` | `0x434a` | `0x9005` (`05\|90<<8`) | `0x03` | `0x05` |
| 4 | 4 | `0x44` | `0x44` | `0x444a` | `0x3290` (`90\|32<<8`) | `0x03` | `0x90` |
| 5 | 5 | `0x45` | `0x45` | `0x454a` | `0x0032` (`32\|00<<8`) | `0x03` | `0x32` |
| 6 | 6 | `0x46` | `0x46` | `0x464a` | `0x0000` | `0x03` | `0x00` |
| 7 | 7 | `0x47` | `0x47` | `0x474a` | `0x0000` | `0x03` | `0x00` |
| 8 | 8 | `0x48` | `0x48` | `0x484a` | `0x0700` (`00\|07<<8`) | `0x03` | `0x00` |
| 9 | 9 | `0x49` | `0x49` | `0x494a` | `0x??07` (`07\|?<<8`) | `0x03` | `0x07` (checksum) |

Preceded by [FACT]: `W(0x00,0x0c)` (`0x10005e24 esi=0x0c, edi=[0x1c]`),
`W(0x02, frame_len|(slave_derived<<8))` (`edi=slave|0x200`, `esi=movzbw
0x1e(%esp)` pushed = physical `0x1a` word = frame_len + slave_derived<<8,
`0x10005e30-0x10005e48`), `W(0x38,0)` (`edi=slave|0x3800, esi=0`,
`0x10005e51-0x10005e65`), then `W(0x3a,0)` before byte 0 (above).
Followed by [FACT]: `W(0x00,0x0a)` (`esi=0x0a, edi=[0x1c]`, `0x10005f56`),
`Sleep(50)` (`0x10005f72`).

**This is the bug that explains zero response on live hardware [INF,
strong]:** the earlier port wrote every byte to fixed reg `0x40`
(fixed `wValue=0x404a`), overwriting the same FIFO slot 9–10 times.
The card/RX FIFO only ever contained the last byte at `0x40`; bytes
`0x41..0x48` were never written. Correct model: **reg increments
per byte** (`0x40,0x41,0x42,…`), not per chunk.

---

## 2. RX poll loop `0x10005fa1-0x100060f5` [FACT]

Init `0x10005f7a-0x10005f9d`: `[0x28]=[0x20]=[0x24]=[0x2c]=0`;
`[0x38]=slave|0x3900`, `[0x3c]=slave|0x0300`.

Per pass (`0x10005fa1`):

1. `Sleep(50)` (`0x10005fa1`).
2. Read #1: `edi=[0x38]` → `R(0x39)`, `esi=0`, `bl=2`, buf `esp+0x40`
   (`0x10005fa9-0x10005fb6`). Result **discarded** (buf reused below).
3. Read #2: `edi=[0x40(%esp)]` pushed = physical `[0x3c]` → `R(0x03)`,
   buf = same `esp+0x40` (pushed `lea 0x44(%esp)` = physical `+0x40`,
   `0x10005fbb-0x10005fcb`). `eax = movzbl 0x45(%esp)` pushed = `buf[1]`
   = `R(0x03)` data.
4. Compare: `edx=[0x24(%esp)]` pushed = physical `[0x20]` = poll counter;
   `cmp %eax,%edx ; jne 0x10006014` (`0x10005fd0-0x10005fde`).
   * **Mismatch:** `[0x20]=0` (`0x10006014 movl $0,0x24(%esp)` pushed),
     fall to the status re-check (§2.1).
   * **Match:** `esi=counter; counter++; cmp $3,%esi; jle 0x1000601c`
     (`0x10005fe0-0x10005fee`). If old counter `>3`, exit poll:
     `resp_max(ebp+0x20)` shrinks to counter if smaller (`0x10005ff0`),
     if `resp_max<=0` go to `0x10006277`, else `[0x24]=0x40`
     (`0x10006006-0x1000600b`) and jump to RX data loop `0x100061c4`.
     So poll success = `R(0x03)` counting `0,1,2,3,4` in consecutive
     passes (counter resets to 0 on any mismatch). [INF meaning: status
     reg counts up when the card FIFO is ready]

### 2.1 Status re-check / length probe (inside every non-exiting pass)

`0x1000601c`: `edi=[0x34(%esp)]` — [INF: offset anomaly, see note],
`[0x20]=eax` (R(0x03) data), then `R(?)` (`esi=0, bl=2`, `0x1000602c-0x1000602f`),
mask checks identical to §5.1 of the earlier doc (`obj+0x4ea`/`0x4e9`
vs `buf[1]`, `W(0x04,0)`+`-3` at `0x10006120`, ConnectCard+`-2` at
`0x1000617d`). [FACT control flow]

> Note on `[0x34]`: no store to physical `[0x34]` exists in SendCard;
> all other status reads use `[0x38]=slave|0x3900` (first poll read) or
> `slave|0x400` (pre-TX, `0x10005ccc`). The mask-check shape proves this
> is a status-reg re-read; the exact wValue high byte is the one
> unresolved static ambiguity in this function. [FACT ambiguity]

Length probe `0x10006063-0x100060f1`, executed only when
`[0x28]==0` (`cmp %esi,0x28(%esp); jne 0x100060f5`, `esi=0` from
`0x1000602d` — so probe runs at most once, since len≥4 after):

```
eax = [0x20] (R(0x03) data)                  ; 0x1000606d
if eax < 3: skip                              ; 0x10006071
if eax >= resp_max(ebp+0x20): skip            ; 0x10006076
if flag != 0: W(0x3a,0), flag=0               ; 0x1000607b-0x100060a1
d = R(0x42)   ; edi=slave|0x4200, esi=0, bl=2 ; 0x100060a4-0x100060b7
len = sbox[d] + 4                             ; 0x100060bc-0x100060c8
[0x28] = len                                  ; 0x100060d1
if len > resp_max: resp_max(ebp+0x20) = len   ; 0x100060ce-0x100060d7
if len == 4: log "Request Length = %d"        ; 0x100060da-0x100060ee
```

### 2.2 Pass limit / exits

`0x100060f5`: `ecx=resp_max(ebp+0x20); incl [0x2c]; cmp %ecx,0x20(%esp)`
(`[0x20]` = R(0x03) data); `jge 0x10005ffc` (R(0x03) ≥ resp_max → exit to
RX-data/finish path); `cmpl $0x14,0x2c(%esp); jge 0x100061b5`
(`[0x2c]` = pass counter, zeroed at `0x10005f88`, incremented once per
pass here → **max 20 passes**); `0x100061b5: edx=[0x20]; jmp 0x10005ff4`
(exit to the same RX-data path). Otherwise `edi=[0x38]; jmp 0x10005fa1`.

---

## 3. RX data loop `0x100061c0-0x10006277` [FACT]

Entry from `0x1000600f` (`ecx=0x40`) or loop head `0x100061c0`
(`ecx=[0x24]`):

| slot | init | meaning |
|---|---|---|
| `[0x24]` | `0x1000600b mov $0x40,%ecx; mov %ecx,0x24(%esp)` | RX raw counter = `0x40+byte_index` |
| `[0x28]` | `0x100061ca mov %ecx,0x28(%esp)` per iteration | chunk/reg base (copy of counter, or counter-0x40 if ≥0x80) |
| `[0x34]`(pushed) | — | physical `[0x30]` = `resp` ptr (`0x10006257`) |

Per iteration `0x100061c4-0x10006271`, identical `W(0x3a)` logic to TX:
`ecx=[0x24]`; `[0x28]=ecx`; if `ecx<0x80`: if flag≠0 `W(0x3a,0)`, flag=0;
else if flag≠1 `W(0x3a,1)`, flag=1, `[0x28]=ecx-0x40` (`0x1000622f`).

Per-byte read `0x10006236-0x1000624a`:

```
edi = [0x28] ; edi = (edi<<8) | [0x1c]        ; wValue=(reg<<8)|slave
esi = 0 ; bl = 2 ; buf = esp+0x40 ; call 0x10005610   ; R(reg)
eax = [0x28(%esp)] pushed = physical [0x24] = raw counter
cl  = 0x45(%esp) pushed = buf[1] = R(reg) data
edx = [0x34(%esp)] pushed = physical [0x30] = resp
resp[eax-0x40] = cl                            ; 0x1000625b mov %cl,-0x40(%edx,%eax,1)
eax++ ; mov %eax,0x28(%esp) pushed = physical [0x24] (counter++)
eax-=0x40 ; cmp 0x20(%ebp),%eax ; jl 0x100061c0   ; byte_index < resp_len
```

**Resolution of `esp+0x28` [FACT]:** `[0x28]` is the **reg/chunk base,
not the byte offset**. The byte offset is `raw_counter-0x40` read from
physical `[0x24]` (`0x1000624f`). The two coincide only numerically for
the first chunk (`reg=0x40+i`, `resp[i]`). `resp_len` usage: loop while
`(counter-0x40) < resp_len(ebp+0x20)`; `resp_len` here is the
probe-shrunk value from §2.1.

### 3.1 Byte-by-byte table — `resp_len=57`, slave `0x4a`

All 57 bytes have raw counter `0x40..0x78 < 0x80`, so `reg=raw`,
`[0x28]=raw`, and **no `W(0x3a,1)`** in this loop; `W(0x3a,0)` only if
flag≠0 on entry (after a 9-byte TX the flag is already 0, so none).
`bRequest=0x02`, `wIndex=0` for every read.

| i | raw counter | reg | wValue | bRequest | wIndex | dest |
|---|---|---|---|---|---|---|
| 0 | `0x40` | `0x40` | `0x404a` | `0x02` | `0` | `resp[0]` |
| 1 | `0x41` | `0x41` | `0x414a` | `0x02` | `0` | `resp[1]` |
| 2 | `0x42` | `0x42` | `0x424a` | `0x02` | `0` | `resp[2]` |
| … | … | … | … | `0x02` | `0` | … |
| 54 | `0x76` | `0x76` | `0x764a` | `0x02` | `0` | `resp[54]` |
| 55 | `0x77` | `0x77` | `0x774a` | `0x02` | `0` | `resp[55]` |
| 56 | `0x78` | `0x78` | `0x784a` | `0x02` | `0` | `resp[56]` |

Full form: for `i=0..56`, `wValue=((0x40+i)<<8)|0x4a`, `resp[i]=R(0x40+i)`.
For `i≥0x40` (len>64) the reg formula of §1.2 applies unchanged
(`reg=i` once raw≥`0x80`), while `resp[i]` is still `resp[raw-0x40]`.
After the loop [FACT]: `W(0x00,0x0c)` (`0x10006277`); if `resp_len<=0`:
`Sleep(50)`, `W(0x04,0)`, `ReleaseMutex`, log "transmit failed", `-6`
(`0x10006296-0x100062d7`).

---

## 4. Length probe summary [FACT]

`0x10005fb6-0x100061b5` (inside the poll loop, §2.1):
regs read per pass are `R(0x39)` (discarded), `R(0x03)` (compared to
poll counter), status re-read (mask checks), then once
`R(0x42)` → **`len = sbox[R(0x42)] + 4`**, stored to `[0x28]` and,
if larger, to `resp_max(ebp+0x20)` (shrinking the caller's max to the
actual length). The live test's `R42 len probe 0 → len 4 zeros` is
consistent with `sbox[0]=0` → `len=4`, i.e. the FIFO was empty —
expected if TX never wrote bytes `0x41+` (see §1.3).

---

## 5. `obj+0x534[8]` state byte — all sites [FACT]

`obj+0x534` is a pointer; `+8` is the state byte, `+0xa` session len,
`+0xb…` session data.

| address | access | effect |
|---|---|---|
| `0x10005a23` (ConnectCard read loop) | read `0x534` ptr only | `session+0xb+index = sbox[R(0x40+index)]` (`0x10005a33`), index `[0x1c]`→physical `[0x20]` pushed |
| `0x10005a48-0x10005a56` (ConnectCard) | read ptr, write `+0xa` | `session[0xa] = session_len` (byte) |
| `0x10005b98-0x10005b9e` (ConnectCard tail) | write `+8` | `session[8] = 0` (only `+8` write in `0x10005700-0x10005c00`) |
| `0x10005dba-0x10005dcf` (SendCard) | read `+8` → `frame[1]` (`0x141(%esp)`) | frame state byte |
| `0x10005dda-0x10005de3` (SendCard) | read `+8`, `xor $0x40`, write `+8` | **`session[8] ^= 0x40`** after reading frame[1] |

No other `+8` read/write in `0x10005700-0x10005c00` or in the caller
`0x10004db0-0x10004f54` region [FACT, grep of `0x534(%…)` in range:
only the three ConnectCard sites above]. Other `0x534` sites at
`0x10008a05`+ are in unrelated functions (allocation/init of the
pointer itself, `movl $0,0x534` at `0x10009171`/`0x10009330`) [FACT].
Session-read writes at `+0x0b` do **not** touch `+8` [FACT].

---

## 6. SendCard caller `0x10004db0` / call site `0x10004eda` [FACT]

`0x10004db0` (thiscall, `ret $4`): `esi = this+8` (`0x10004dd4`).
Before the call it copies a length-prefixed command from the object
ring buffer into a stack buffer (`0x10004e77-0x10004eab`):

```
start = [esi+0xc0] = [this+0xc8]
len   = [esi+0xbc] - start = [this+0xc4] - [this+0xc8]   (capped 0x10c)
src   = this+0xcc + start    (lea 0xc4(%eax,%esi) = esi+0xc4+start)
dst   = stack buffer (lea 0x130(%esp) with 2 pushes active)
memcpy(dst, src, len); [this+0xc8] += len; then [this+0xc4]=[this+0xc8]=0
```

Call site pushes (right-to-left, first push = last arg) [FACT]:

```
push $0x10c                                   ; resp_max = 0x10c
push resp_buf (lea 0x20(%esp) after 1 push)   ; resp
push %ebx  (ebx=0)                            ; extra_len = 0
push %ebx  (ebx=0)                            ; extra = NULL
push %ecx  (movzbl 0x128(%esp) = dst[0])      ; req_len = dst[0]
push req   (lea 0x13e(%esp) after 5 pushes = dst+2) ; req = dst+2
ecx = (%edi)                                  ; this = card object
call *(%eax)  where eax = [edi+0x2f8]         ; = 0x10005bc0
```

So the copied buffer is `[req_len][pad][req…]`: `req_len=dst[0]`,
`req=dst+2`. After the call, `ebx=eax` (resp_len); if `>=4`,
`edi=ebx-2` etc. (`0x10004f2b`) [FACT].

### First command bytes

* This caller's object is initialised at `0x100050c2-0x10005141` [FACT]:
  `[this+0xcc]=5, [this+0xcd]=0, [this+0xce]=dword [0x1000f3bc],
  [this+0xd2]=byte [0x1000f3c0], [this+0xc4]=7, [this+0xc8]=0`.
  File-off `0xd9bc` (= VA `0x1000f3bc`, `.data` VA `0x1000f000`→off `0xd600`):
  **`90 30 00 00 00 00 00 00`**.
  First copied buffer = `05 00 90 30 00 00 00` →
  **`req_len=5, req=90 30 00 00 00`** (`INITIAL_SETTING_CONDITIONS_CMD`).
* The task's file-off `0xd9ac` entry is a **different init site**
  (`0x1000338e-0x100033bf`): `[obj+0xc6]=dword [0x1000f3ac]`,
  `[obj+0xca]=byte [0x1000f3b0]`, `[obj+0xc4]=5` (movb), i.e. that
  object's req sits at `+0xc6` with len at `+0xc4`. Dumped 8 bytes at
  file-off `0xd9ac` [FACT, DLL binary]: **`90 32 00 00 00 00 00 00`**
  (`CARD_ID_INFORMATION_ACQUIRE_CMD`, 5 bytes + padding).
  Neighbouring entries at `0xd9b4`: `90 34 00 00 90 36 00 00`,
  `0xd9bc`: `90 30 00 00 00 00 00 00` [FACT].
* `extra=NULL, extra_len=0, resp_max=0x10c` for the first call [FACT].

### frame[1] at first call

`frame[1]` is read **after** the §5.1 presence check and after the
possible in-SendCard `ConnectCard(this,0)` call (`0x10005d6f-0x10005d72`,
only if the reader-ready mask bit is set without the full mask).
ConnectCard clears `session[8]=0` at its tail (`0x10005b9e`), and
SendCard reads `frame[1]=session[8]` then toggles it `^=0x40`
(`0x10005dba-0x10005de3`). Therefore at the first SendCard:
**`frame[1]=0x00`, and `session[8]` becomes `0x40` for the next call**
[FACT control flow; INF that no other code sets `[8]` in between —
§5 shows no other writer]. The task's probe frame
(`00 00 05 | …`, state byte `0x00`) is consistent with this.

---

## 7. Confirmations

* **TX wIndex = transformed, not raw [FACT]:** the sbox loop
  `0x10005df0-0x10005e0e` runs *before* the TX loop, in place
  (`frame[i]=sbox[frame[i]]`, `dl ^= sbox[frame[i]]`), checksum appended
  raw at `frame[frame_len]` (`0x10005e1c`). All TX writes therefore
  carry transformed bytes (plus the word-load high-byte artefact, §1.2).
* **RX decode starts at `resp[0]`, with sbox [FACT]:**
  `0x100062dc-0x1000630d`: `dl=0; for i in 0..resp_len-2:
  dl ^= resp[i] (raw); resp[i]=sbox[resp[i]]` (esi = `[0x30]` resp ptr,
  `0x100062f4`). Final check `resp[resp_len-1]==dl` (`0x10006314-0x1000631c`),
  else "data error" `-6`. Note the confirmed asymmetry: TX XORs
  *transformed* bytes, RX XORs *raw* bytes (earlier doc §4 stands).

---

## 8. Corrected pseudocode for `hdus_card_transmit`

```c
// slave = obj+0x4e6 (derived byte: 0x4e4 in {0,0xff} ? slave : slave-4)
// helper: usb_ctrl(bRequest, wValue=(reg<<8)|slave, wIndex, buf[2])
static int card_r(u8 slave, u8 reg, u8 *out);   // 0x02, wIndex 0, buf[1]
static int card_w(u8 slave, u8 reg, u16 val);   // 0x03, wIndex val

int hdus_card_transmit(const u8 *req, int req_len,
                       const u8 *extra, int extra_len,   // NULL/0 for first call
                       u8 *resp, int *resp_len)          // *resp_len: in=max(0x10c), out=actual
{
    // §5.1 presence poll R(0x04) vs 0x4e9/0x4ea masks -> -2/-3 (unchanged)
    // frame build: frame[0]=0, frame[1]=state, frame[2]=frame_len-3,
    //              req..., [extra_len byte, extra..., NUL]
    // sbox-transform frame[0..frame_len), checksum = XOR(transformed),
    // frame[frame_len++]=checksum; state ^= 0x40;   // frame[1] read BEFORE toggle
    card_w(slave, 0x00, 0x0c);
    card_w(slave, 0x02, (u16)frame_len | (slave_derived<<8)); // low byte = frame_len
    card_w(slave, 0x38, 0);
    int flag = 0xff;
    for (int i = 0; i < frame_len; i++) {        // TX — reg INCREMENTS per byte
        int raw = 0x40 + i;
        int reg = (raw < 0x80) ? raw : raw - 0x40;
        if (raw < 0x80) { if (flag != 0) { card_w(slave,0x3a,0); flag=0; } }
        else            { if (flag != 1) { card_w(slave,0x3a,1); flag=1; reg=raw-0x40; } }
        // wIndex is a WORD load: frame[i] | frame[i+1]<<8 (low byte matters)
        card_w(slave, reg, (u16)(frame[i] | (frame[i+1]<<8)));
    }
    card_w(slave, 0x00, 0x0a); sleep(50);
    // poll: R(0x39) discarded, R(0x03) must count 0,1,2,3,4 consecutively;
    //       status re-check + len probe (len=sbox[R(0x42)]+4) inside loop;
    //       max 20 passes; exit also if R(0x03) >= resp_max
    // RX — reg INCREMENTS per byte, dest is resp[i] (not reg-based):
    for (int i = 0; i < resp_len; i++) {
        int raw = 0x40 + i;
        int reg = (raw < 0x80) ? raw : raw - 0x40;
        if (raw < 0x80) { if (flag != 0) { card_w(slave,0x3a,0); flag=0; } }
        else            { if (flag != 1) { card_w(slave,0x3a,1); flag=1; } }
        card_r(slave, reg, &resp[i]);
    }
    card_w(slave, 0x00, 0x0c);
    // decode: dl=0; for i<resp_len-1 { dl^=resp[i]; resp[i]=sbox[resp[i]]; }
    // return resp_len if resp[resp_len-1]==dl else -6
}
```

**Correct chunk model (replaces earlier doc):** there is no fixed-reg
per-chunk write. The "chunk base" variable (`[esp+0x2c]` TX,
`[esp+0x28]` RX) is a **per-byte register address**: `0x40+i` for the
first 64 bytes, then `i` (i.e. it wraps `0x40..0x7f → 0x40..0x7f → 0x80..`
as the raw counter crosses `0x80`). `W(0x3a,0)` fires once before byte 0,
`W(0x3a,1)` once before byte `0x40`. Any port that writes all bytes to
`0x40` (or to `0x40,0x80,0xc0…` chunk bases) will get zero response —
matching the live SKNET HDUSF result (`R42=0 → len=4`).
