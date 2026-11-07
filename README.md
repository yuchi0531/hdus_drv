# hdus — SKNET MonsterTV HDUS Linux ドライバ

SKNET（エス・ケー・ネット）製 USB 地上デジタルチューナー
**MonsterTV HDUS** を Linux で使うための out-of-tree カーネルモジュールです。

- カーネルモジュール名: `hdus`
- 文字デバイス: `/dev/hdusvideoX`（`read()` は復号済み TS を返す）
- ioctl ABI: recpt1 / BonDriver PT1・PT3 互換（magic `0x8d`）
- 対象ハードウェア: USB `1738:5211`（ファームウェアロード後 `3275:6051`）

## 概要

| 項目 | 内容 |
| --- | --- |
| 対応機種 | SKNET MonsterTV HDUS |
| USB ID（未書込） | `1738:5211` |
| USB ID（書込後） | `3275:6051` |
| キャラクタデバイス | `/dev/hdusvideo0`（複数台は `hdusvideo1` …） |
| ドライバファイル | `hdus_drv.c`（USB/char デバイス本体・ファームウェア転送・ioctl）、`hdus_descrambler.c`（デスクランブラ） |
| ioctl ヘッダ | `hdus_ioctl.h` |
| ファームウェアローダ | `hdus_as11loader.c` → `hdus_as11loader`（ユーザースペース） |
| テストツール | `tools/hdus_test.c` → `tools/hdus_test` |
| B25 デコーダ | `tools/hdus_b25.c` → `tools/hdus_b25`（PC/SC B-CAS 経由、libaribb25） |
| B-CAS 診断 | `tools/bcas_check.c` → `tools/bcas_check`（PC/SC 診断） |

### ioctl ABI（recpt1 互換、magic `0x8d`）

| ioctl | 値 | 意味 |
| --- | --- | --- |
| `SET_CHANNEL` | `_IOW(0x8d, 1, struct hdus_frequency)` | チャンネル設定。`struct hdus_frequency { int freq_no; int slot; }`（PT の `FREQUENCY` と同レイアウト） |
| `START_REC` | `_IO(0x8d, 2)` | 録画（TS 配信）開始 |
| `STOP_REC` | `_IO(0x8d, 3)` | 録画停止 |
| `GET_SIGNAL_STRENGTH` | `_IOR(0x8d, 4, int *)` | 信号強度（生値）を取得 |
| `LNB_ENABLE` | `_IOW(0x8d, 5, int)` | LNB 電源 ON |
| `LNB_DISABLE` | `_IO(0x8d, 6)` | LNB 電源 OFF |
| `PTX_SET_SYSTEM_MODE` | `_IOW(0x8d, 0x0b, int)` | システムモード設定（ISDB-T のみ） |
| `HDUS_GET_STATUS` | `_IOR(0x8d, 7, int *)` | ストリーミング中か |
| `HDUS_GET_TS_ERROR_PACKET_COUNT` | `_IOR(0x8d, 0x0a, unsigned int *)` | 同期外れで捨てたパケット数 |
| `HDUS_CARD_CONNECT` | `_IO(0x8d, 0x10)` | 内蔵 B-CAS を初期化（ATR 取得）。0 / -2 / -3 |
| `HDUS_CARD_TRANSMIT` | `_IOWR(0x8d, 0x11, struct hdus_card_req)` | T=1 で APDU を送受信。戻りは応答長 |
| `HDUS_CARD_PRESENT` | `_IOR(0x8d, 0x12, int *)` | カード挿入状態 |
| `HDUS_CARD_GET_ATR` | `_IOR(0x8d, 0x13, struct hdus_card_atr)` | 初期化時に取得した ATR（キャッシュ） |

`read()` で復号済み MPEG-TS（188 バイトパケット、同期バイト `0x47`）が
ストリームとして読み出せます。

## ビルド

カーネルヘッダが必要です（例: `linux-headers-$(uname -r)`）。

```sh
# モジュール + ファームウェアローダ + テストツールを全部ビルド
make

# 個別ビルド
make modules   # hdus.ko のみ
make loader    # hdus_as11loader
make test      # tools/hdus_test

# 別カーネルツリーを指定
make KERNELDIR=/lib/modules/$(uname -r)/build
make KERNELDIR=/usr/src/linux-headers-$(uname -r)

# クリーン
make clean
```

`tools/hdus_test.c` は `hdus_ioctl.h` が無い環境でも
`__has_include` フォールバックで単体コンパイルできます:

```sh
gcc -O2 -Wall -o /tmp/hdus_test tools/hdus_test.c
/tmp/hdus_test --help
```

### DKMS

`dkms.conf`（`PACKAGE_NAME="hdus"`, `PACKAGE_VERSION="0.1.0"`,
`BUILT_MODULE_NAME[0]="hdus"`, `DEST_MODULE_LOCATION[0]="/extra"`,
`AUTOINSTALL="yes"`）が同梱されています。DKMS 管理したい場合は
ソースを `/usr/src/hdus-0.1.0/` に置いて:

```sh
sudo cp -r . /usr/src/hdus-0.1.0/
sudo dkms add hdus/0.1.0
sudo dkms build hdus/0.1.0
sudo dkms install hdus/0.1.0
```

## ファームウェア

HDUS は電源投入直後は未書込デバイス（`1738:5211`）として現れます。
ファームウェアを転送すると `3275:6051` として再列挙され、ドライバが使えます。

```sh
./hdus_as11loader --dry-run   # 解析のみ（USB には触らない）
sudo ./hdus_as11loader        # usbfs 経由で転送
lsusb | grep 3275             # 3275:6051 になっていれば成功
```

引数を省略すると `./firmware/` と `./` を順に見ます。`SKNET_AS11Loader.sys`
（`0xa88` の header、`0xa90` から本体）を直接渡すこともできます。

転送は電源投入ごとに 1 回で、抜くまで `3275:6051` のままです。

## インストールとロード

```sh
sudo make install    # hdus.ko -> /lib/modules/$(uname -r)/extra/
                     # hdus_as11loader -> /usr/local/bin/
                     # 99-hdusvideo.rules -> /etc/udev/rules.d/
                     # firmware/ -> /lib/firmware/hdus/

sudo make load       # insmod / modprobe（ファームウェア書込済みであること）
# または
sudo modprobe hdus

ls -l /dev/hdusvideo*
dmesg | tail -20
```

アンインストール:

```sh
sudo make unload
sudo make uninstall
```

udev ルール `99-hdusvideo.rules`:

- `/dev/hdusvideo*` を `video` グループ・`0666`・`uaccess` タグ付きにする
- USB `1738:5211` 接続時に（任意）自動で `hdus_as11loader` を実行する

## 使用方法（/dev/hdusvideo0）

recpt1 スタイルのテストツール `tools/hdus_test` を使います。

```sh
# ch13 (freq_no = 63) を 10 秒録画して out.ts へ
tools/hdus_test -d /dev/hdusvideo0 -c 63 -t 10 -o out.ts

# 物理チャンネル指定（UHF ch13-62: freq_no = ch + 50）
tools/hdus_test -T 13 -t 10 -o out.ts

# CATV チャンネル指定（C13-63: C13-22 は ch-10、C23-63 は ch-1）
tools/hdus_test -C 13 -t 10 -o out.ts

# 信号強度だけ確認
tools/hdus_test -c 63 --signal-only

# stdout へバイナリ出力
tools/hdus_test -T 13 -t 5 -o -
```

オプション:

```
-d, --device <path>   デバイス (default: /dev/hdusvideo0)
-c, --freq <freq_no>  freq_no を直接指定
-s, --slot <n>        スロット/サブチャンネル (default: 0)
-T, --uhf <ch>        UHF 物理 13-62
-C, --catv <ch>       CATV 13-63
-t, --time <sec>      録画秒数 (default: 10)
-o, --out <file>      出力先 (default: out.ts、- は stdout)
    --signal-only     信号強度のみ
    --lnb-on/--lnb-off LNB 制御
-h, --help
```

### recpt1 / BonDriver_LinuxPT(X) で使う

`hdus` の ioctl ABI は recpt1 の PT1/PT3 と同等なので、
BonDriver_LinuxPT / BonDriver_LinuxPT(X) 系のドライバから
そのまま `/dev/hdusvideo0` を開けます。

- `.ini` / `.conf` の `Device=/dev/hdusvideo0`（または `Device0=` 等）に指定
- チャンネルは recpt1 の `freq_no` 体系で渡します。
  UHF/CATV の物理チャンネル → `freq_no` 変換表は下記参照。
- 録画は `SET_CHANNEL → START_REC → read() → STOP_REC` の順で動作します。

## B-CAS / PC/SC (B25 デコード)

カーネル側の復号は DVB チューナ内 ASIE によるスクランブル解除までで、
`/dev/hdusvideoX` の出力は Multi2/B25 暗号化された TS です。
B25 デコードはユーザースペースで PC/SC 経由の B-CAS カード
（libaribb25: `create_b_cas_card()` → `SCard*` / `SCardTransmit` T1）
により行います。これは recfriio `--b25` / recpt1 `--b25` と同じ方式です。

必要なもの: PC/SC 対応 IC カードリーダ（USB）、`pcscd`
（socket 連携で自動起動。`systemctl status pcscd` で確認）、
`libpcsclite-dev`（ビルド時）、B-CAS カード。

```sh
make bcas b25            # ARIB25_DIR=... で arib25 ソースを指定可
                         # (default: ~/projects/recfriio/arib25)

tools/bcas_check         # リーダ列挙 + B-CAS カード ID/初期状態表示
                         # exit 0 = カード応答あり / 2 = リーダなし

# 録画 + B25 デコードのパイプライン
tools/hdus_test -T 13 -t 10 -o - | tools/hdus_b25 > out.ts
# または既存 TS ファイルのデコード
tools/hdus_b25 -i in.ts -o out.ts [--round 4] [--strip] [--emm]
```

`hdus_b25` の終了コード: `0` = デコード済み、`3` =
カードなし等で暗号化のままコピー（フォールバック）、`1` = I/O・引数エラー。
libaribb25 にはリーダ名指定 API がないため、最初に接続できる
リーダが自動選択されます（`--reader` は警告付きで受けます）。

現状この VM では USB カードリーダが未接続（`lsusb` に表示なし）のため
`bcas_check` は exit 2、`hdus_b25` はフォールバックとなります。
リーダをパススルー接続すればそのままデコードできます。

## チャンネル表

### 地上デジタル（UHF 物理チャンネル）

`freq_no = 物理チャンネル + 50`（ch13 → 63、ch62 → 112）

| 物理 ch | freq_no |
| --- | --- |
| 13 | 63 |
| 14 | 64 |
| … | … |
| 62 | 112 |

### CATV チャンネル

C13–22 は `freq_no = ch - 10`、C23–63 は `freq_no = ch - 1`

| CATV ch | freq_no |
| --- | --- |
| C13 | 3 |
| C14 | 4 |
| … | … |
| C22 | 12 |
| C23 | 22 |
| C24 | 23 |
| … | … |
| C63 | 62 |

## トラブルシューティング

```sh
# デバイスが見つからない
lsusb | grep -E '1738:5211|3275:6051'
#   1738:5211 = ファームウェア未書込 → hdus_as11loader で転送
#   3275:6051 = 書込済み → hdus モジュールが使える状態

# モジュールが読み込まれていない
lsmod | grep hdus
dmesg | tail -50

# /dev/hdusvideo0 が無い / 権限エラー
ls -l /dev/hdusvideo*
# video グループに入れる（udev ルールで 0666 になるはず）
sudo usermod -aG video "$USER"
# （再ログイン後に有効）

# 信号が来ない / TS が出ない
tools/hdus_test -c 63 --signal-only   # 信号強度の生値を確認
tools/hdus_test -T 13 -t 10 -o out.ts # 0x47 同期率を確認
# アンテナ接続・チャンネル・dmesg のエラーを確認

# udev ルールが効かない
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## ライセンスについて

- 本ドライバ（`hdus` カーネルモジュール）は recpt1 / recfriio の
  PT ioctl ABI（PT1/PT3 互換）と
  BonDriver_LinuxPT(X) の利用形態に合わせて開発されています。
- recfriio の PT 関連コードは BSD ライセンスです。
- ioctl ABI の互換性のため、recfriio 由来の定義・構造体は
  ライセンス条件を満たす形で取り込んでいます。
  参照した recfriio / BonDriver_LinuxPT(X) のライセンス表記を
  各ソースファイルのヘッダに保持してください。
- ioctl ABI（magic `0x8d` の番号体系）自体は互換のための
  インターフェースであり、GPL カーネルモジュールとして
  配布する場合の互換性に配慮しています。本モジュールは
  GPLv2 のカーネルモジュールとして提供します。
- ファームウェア（`SKNET_AS11Loader.sys` 由来）は SKNET の
  所有物であり、本プロジェクトには含まれません。各自で
  正規のドライバパッケージから入手してください。

## ファイル一覧

```
Makefile             ビルド / インストール / ロード用
dkms.conf            DKMS 用設定
99-hdusvideo.rules   udev ルール（/dev/hdusvideo* 権限 + 自動 fw ロード）
hdus_ioctl.h         ioctl 定義（PT1/PT3/PTX 互換）
hdus_drv.c           ドライバ本体（USB/char デバイス/ファームウェア転送）
hdus_descrambler.c   デスクランブラ（XOR/DES + TS 再同期）
hdus_as11loader.c    ファームウェアローダ（ユーザースペース）
tools/hdus_test.c    recpt1 スタイルのテストツール
tools/hdus_b25.c     B25 デコーダ（PC/SC B-CAS + libaribb25）
tools/bcas_check.c   B-CAS PC/SC 診断ツール
tools/descrambler_test.c  デスクランブラ自己テスト
firmware/as11fw.bin  ASV5211 ファームウェア（-> /lib/firmware/hdus/）
```

## B-CAS 内蔵スロットの扱い (tools/ifd-hdus)

HDUS 本体スロットを PC/SC 読取器 **"HDUS B-CAS"** として公開します
（BonDriver_HDUS.dll の ConnectCard/SendCard を RE し再実装:
`docs/card_protocol_connect.md`, `docs/card_protocol_sendcard.md`）。

```sh
make -C tools/ifd-hdus all
sudo make -C tools/ifd-hdus install   # libhdus_ifd.so -> /usr/lib/pcsc/drivers/
# reader.conf.example を /etc/reader.conf にマージ後
sudo systemctl restart pcscd
pcsc_scan                              # "HDUS B-CAS" が見えること
tools/bcas_check                       # libaribb25 経由でカード ID 表示
sudo ./tools/ifd-hdus/hdus_card_test  # 実機直接テスト (90 30 / 90 32 APDU)
```

### 実機検証結果 (2026-11-08)

- ASIE/card スレーブ `0x4a`（DES 機、reg09=0x06）
- カード初期化で本物の ATR を取得: `3B F0 12 00 FF 91 81 B1 7C 45 1F 03 99`
- APDU: `90 30` → 61 B（return code `0x2100`, card_id, system_key）、
  `90 32` → 19 B。`bcas_check` で `ca_system_id=5` を確認
- チューナー: 17–19 Mbit/s、TS 同期 100%
- 同時使用: 録画しながら APDU を投げて **19.09 Mbit/s / ギャップなし**

### チューナーとカードの同時使用（実測）

**同時使用できます。** 以前の「モード排他」はドライバ側の転写ミスが原因でした。

- `Hdus.cpp` のチューナーテーブルは
  `send_ctl(0x03, wValue=(reg<<8)|0x20, wIndex=値)`、
  つまり **I2C スレーブ 0x20（復調器）** への書き込みです。
- カードは **スレーブ 0x4a（ASIE5606）** で、まったく別のデバイスです。
- 初期実装はテーブルを `(reg<<8)|値` に潰して 0x4a に送っていたため、
  カード側レジスタを壊し「排他」に見えていました。

修正後はカード操作ごとの power cycle も tuner restore も不要です。

| 条件 | 実測 |
|---|---|
| チューナー単体 | 17–19 Mbit/s |
| 録画しながら PC/SC で APDU | 19.09 Mbit/s、TS ギャップなし、power cycle 0 回 |
| 録画 + B25 デコード（パイプ） | 18.78 Mbit/s、`mode=decoded` |
| オフライン B25 デコード | 15 MB を 1.5 秒 |

信号レベルも改善（正しいテーブルで初期化されるため）。


