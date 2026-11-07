# ASIE5606 カードインタフェース — coax (px3) 実装との突き合わせ

参照: https://github.com/Till0196/coax/blob/main/docs/reference/card-reader.md
       内部実装 `internal/coaxd/smartcard/asie5606/asie5606.go` (Apache-2.0 OR GPL-2.0+)
       bridge: `internal/coaxd/usb/bridge/asv5211/asv5211.go`

HDUS のカードスロットも PX3 と同じ ASIE5606 (ASV5211 ブリッジの I2C 上) なので、
coax の実装は **そのまま仕様書として使える**。以下は coax 由来の確定情報と、
HDUSF (3275:6051) 実機で測った差分。

## 1. レジスタファイル（coax 由来、FACT）

| reg | 名前 | 意味 |
|---|---|---|
| 0x00 | command | 0x00 idle / 0x80 prepare / 0x08 power / 0x09 reset / 0x0a transmit / 0x0c acknowledge |
| 0x01 | protocol | リセット後 0x01 を書く（T=1） |
| 0x02 | transmit length low | 送るブロック長 |
| 0x03 | receive length low | 受信バッファに溜まっているバイト数 |
| 0x04 | status | bit0 = カード挿入, bit7 (0x80) = カード電源（書くと ON） |
| 0x09 | version | bit1-5 が IC リビジョン |
| 0x38 | transmit length high | 拡張リビジョンのみ |
| 0x39 | receive length high | 同上（bit0 のみ有効） |
| 0x3a | window page | ウィンドウのページ選択 |
| 0x40 | window | 64 バイトのスライド窓（送受信バッファ） |

これは HDUS 側で我々が RE で推定していた値と一致する:
- 0x04 の 0x01 = present / 0x80 = ready → **present と power-on ビット**だった
- 0x39 は「ポール用カウンタ」ではなく **受信長の上位バイト**
- 0x40 は FIFO ではなく **64 バイト窓**、0x3a がページ
- 0x02/0x03 は **長さレジスタ**（FIFO ポインタではない）

## 2. カード初期化（coax `Reset`、FACT）

```
command(0x80)          # prepare（前のオーナーの半端なセッションを捨てる）
sleep 10ms
command(0x00)          # idle
write 0x04 = 0x00      # カード電源 OFF
command(0x00)
sleep 100ms
read 0x04 & 0x01       # 0 なら ErrNoCard
command(0x08)          # power
write 0x04 = 0x80      # カード電源 ON
sleep 200ms
command(0x09)          # reset
<ATR を受信（ヘッド 13 バイト）>
write 0x01 = 0x01      # T=1
sequence = 0
# 拡張リビジョンのみ: S-block 00 C1 01 FE で情報フィールド長を通知
```

DLL (BonDriver_HDUS) の ConnectCard はこれの短縮版で、
`W(0x00,0x08) → W(0x04,0x80) → W(0x00,0x09)` を発行したあと
セッション情報を 0x40.. から読む。**ATR は読んでいない**。

## 3. T=1 ブロック（coax 由来、FACT）

```
block = [NAD=0x00, PCB, LEN, INF..., LRC]
全バイトを bit-reverse（LEN も含む）
LRC = XOR(反転後の全バイト)
```

- PCB: `0x40` = N(S)（I-block の送信シーケンス。1 ブロック完了ごとに XOR 1）
- PCB: `0x20` = more data（チェーン継続）
- PCB: `0x80` = R-block、`0x10` = N(R)
- `LEN` は INF フィールド長。ブロック全体長 = LEN + 4

我々の DLL 由来実装との対応:
- `state ^= 0x40` = N(S) トグル → **一致**
- `len = Reverse8(R[0x42]) + 4` = ブロック全体長 → **一致**（0x42 は窓の 3 バイト目 = LEN）
- チェックサム = 反転後バイトの XOR → **一致**

つまり我々の SendCard 実装は T=1 として正しい。coax はさらに
**チェーン受信**（more data のとき R-block で N(R) を返す）と
**LRC 検証**（全バイト XOR == 0）を実装している。

## 4. 受信手順（coax `receive`、FACT）

```
poll: 50ms 間隔、最大 42 回（ヘッド）
  count = R(0x03) | (R(0x39)&1)<<8
  count >= 3 で成功
窓から count バイト読む（ページ跨ぎは 0x3a で切替）
total = Reverse8(block[2]) + 4
total > count なら追加ポール（最大 8 回）して残りを読む
command(0x0c)  # acknowledge
```

**我々の実装の誤り**: `R(0x03)` を「0,1,2,3 と数えるカウンタ」と解釈して
4 ポール待っていた（=200ms の無駄）。正しくは「受信長 >= 3」で即成功。

## 5. HDUSF 実機で測った差分（FACT、2026-11-07）

| 機能 | coax (PX3) | HDUSF 実機 |
|---|---|---|
| 複数バイトのレジスタ読み (`0x02` len=N) | 可（最大 0x20） | **不可**。`[status, value, 0xff, 0xff...]` を返す（値は 1 バイトのみ） |
| I2C ブロック書き (`0x0d` ×n + `0x0e`) | 可（3 バイトずつ） | 呼び出しは成功するが未検証（`0x0e` の noStop は危険） |
| シーケンシャル読み (`0x19`) | 可（demod 用） | **使用禁止**。発行するとファームがハングし、USB デバイスごと固まる（要再挿入） |

→ HDUSF では**1 レジスタ 1 コントロール転送**が上限（実測 1.36 ms/転送）。
　高速化の余地は「転送回数」ではなく「待ち時間」側にある。

## 6. 共存の結論（2026-08-08 実機で確定）

**チューナーとカードは同時に使える。** 以前「排他」と判断したのは
HDUS ドライバ側の転写ミスが原因だった:

- `Hdus.cpp` のチューナーテーブルは
  `send_ctl(req 0x03, wValue=(reg<<8)|0x20, wIndex=値, 2)`、
  つまり **I2C スレーブ 0x20（復調器）への書き込み**。
- カードは **スレーブ 0x4a（ASIE5606）**。別デバイス。
- 初期実装はテーブルを `(reg<<8)|値` に潰してスレーブ 0x4a へ送っていたため、
  カード側レジスタを破壊し「排他」に見えていた。

修正後: カード操作に power cycle も tuner restore も不要。
録画しながら PC/SC で APDU を投げて **19 Mbit/s・TS ギャップゼロ**を実測。

## 6.1 旧: 電力・モード共存に関する示唆

coax は px3 で
> すでにストリームが流れている IC はリセットしない（ストリームが切れる）

と明記している。HDUS では更に厳しく、
- チューナーテーブルの `reg 0x11 = 0x21` を書くとカードが死ぬ
- カードのセッション末尾で `0x40..0x44` を書くとチューナー信号が落ちる

つまり **同じ ASIE5606 のレジスタを奪い合う**ため、HDUS は
フル power cycle + 再選局が必要（coax の px3 はこの競合が無いか、
ストリーム側レジスタを触らない運用で回避している）。

ただし coax の Reset は **power cycle を含まない**（IC はそのまま、
カードだけ power/reset）。HDUS でも「ASIE の電源は落とさず、
カード側だけ prepare→power→reset」で済む可能性がある。
現在の実装は `0x08 dd0c`（ASIE 電源断）を含むため 260ms を余分に払っている。
これは要検証項目（デバイス復帰後）。

## 7. 採用予定の変更

1. **受信ポールを正しい意味に修正**: `count = R(0x03)|(R(0x39)&1)<<8`、
   `count >= 3` で成功 → 1 APDU あたり 200ms 削減。
2. **LRC 検証**を追加（全バイト XOR == 0）。今は「反転後 XOR 一致」だけ。
3. **ATR の実取得**: reset 後に ATR を読み、IFD ハンドラの合成 ATR
   (`3B 80 01 BA`) を実 ATR に置き換え。PROTO_MISMATCH の根本解決。
4. **power cycle の省略**（要検証）: ASIE 電源を落とさず
   prepare/power/reset のみ。260ms 削減。
5. **restore の最小化**: チューナー復帰で全テーブルではなく
   衝突した `0x40..0x44` + PID kick のみ書き直す。
6. チェーン受信（more data）対応。ECM の長い応答で必要になる可能性。

## 8. 注意（恒久）

- request `0x19` と `0x0e` noStop は HDUSF ファームで**デバイスを固める**。
  カーネルドライバ・ツール双方で使用禁止。
- 複数バイト読みは使えないので、窓の読み出しは 1 バイトずつ。
