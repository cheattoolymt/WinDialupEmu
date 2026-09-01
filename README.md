# WinDialupEmu (VModem)

Windows 用の **本物のダイアルアップモデム エミュレータ**。

com0com の仮想 COM ポートを「電話線」として使い、Windows 標準のダイアルアップ
接続 (RAS) から見て本物のアナログモデムとして振る舞います。AT コマンドに応答し、
発信音・DTMF・呼出音・V.34 のハンドシェイク音をスピーカーから鳴らし、PPP で
IP アドレスを払い出し、libslirp の NAT 経由で**実際にインターネットへ接続**します。

```
┌─────────────────┐   COM   ┌──────────────┐        ┌──────────┐
│ Windows ダイアル │◄──────►│   VModem     │◄──────►│ 実インター │
│ アップ (RAS)     │ com0com │ AT/PPP/NAT   │libslirp│  ネット    │
│  CNCA0          │  ペア    │   CNCB0      │        │          │
└─────────────────┘         └──────┬───────┘        └──────────┘
                                    │ WASAPI
                                    ▼
                              🔊 ネゴシエーション音
```

---

## 特徴

- **AT コマンド インタプリタ** — Windows RAS が発行する初期化・発信・切断の
  一連のコマンドに応答
- **音の再現** — 発信音 (日本仕様 400Hz 連続)、DTMF (ITU-T Q.24 準拠)、
  呼出音 (1s ON / 2s OFF)、V.8/V.34 ハンドシェイク音を WASAPI で合成再生
- **複数の変調方式** — V.21 (300bps) 〜 V.90 (56kbps)。番号ごとに指定可能
- **仮想電話番号** — 設定した番号にダイアルした時だけ接続。
  ハイフン・括弧・ダイアル修飾子 (`W , ; ! @ T P`) を無視して数字のみで照合
- **PPP** — LCP / PAP / IPCP を実装。IP と DNS を払い出す
- **NAT** — libslirp によるユーザモード NAT で実インターネットへ接続。
  疑似 Ethernet + ARP 応答層を挟んで生の PPP IP パケットを橋渡し

---

## 必要なもの

| 項目 | 内容 |
|---|---|
| OS | Windows 10 / 11 (x64) |
| コンパイラ | MinGW-w64 (推奨: [WinLibs](https://winlibs.com/) POSIX/MSVCRT) |
| 仮想 COM | [com0com](https://sourceforge.net/projects/com0com/) (署名版を推奨) |
| NAT ライブラリ | libslirp **4.8.0 以降** (`libslirp-0.dll` + `libslirp.dll.a`) |

### libslirp の入手

本リポジトリは API ヘッダ (`include/libslirp.h`, `include/libslirp-version.h`)
のみ同梱しています。DLL とインポート ライブラリは `.gitignore` の対象なので
別途用意してください。

MSYS2 を使う場合:

```bash
pacman -S mingw-w64-x86_64-libslirp
# libslirp-0.dll   -> /mingw64/bin/
# libslirp.dll.a   -> /mingw64/lib/
```

これらをリポジトリのルートにコピーします。

> **バージョンについて**
> 同梱ヘッダは 4.9.3 ですが、DLL が 4.8.0 でも動作します。
> 本プログラムは起動時に `slirp_version_string()` で DLL の実バージョンを読み、
> 渡す `SlirpConfig.version` と使用する API を自動的に切り替えます
> (詳細は下の「libslirp のバージョン差異」)。

---

## ビルド

MinGW-w64 の環境で、リポジトリのルートで以下を実行します（1 行）。

```bash
gcc -O2 -std=c99 -Wall -Wextra -Iinclude -DVMODEM_HAVE_LIBSLIRP src/main.c src/core/vm_config.c src/core/vm_log.c src/core/vm_ringbuf.c src/core/vm_types.c src/audio/vm_audio.c src/audio/vm_audio_null.c src/audio/vm_audio_wasapi.c src/dsp/vm_handshake.c src/dsp/vm_resample.c src/dsp/vm_tone.c src/dsp/vm_v34.c src/dsp/vm_v8.c src/modem/vm_at.c src/modem/vm_modem.c src/modem/vm_sequence.c src/net/vm_eth.c src/net/vm_hdlc.c src/net/vm_hostroute.c src/net/vm_nat.c src/net/vm_nat_loopback.c src/net/vm_nat_slirp.c src/net/vm_ppp.c src/net/vm_winsock.c src/serial/vm_serial.c src/serial/vm_serial_win32.c -o vmodem.exe -L. -lslirp -lws2_32 -liphlpapi -lole32 -lwinmm -lm
```

### リンクするライブラリの理由

| ライブラリ | 用途 |
|---|---|
| `-lslirp` | libslirp (ユーザモード NAT) |
| `-lws2_32` | Winsock2。`WSAStartup` / `WSAPoll` |
| `-liphlpapi` | IP Helper。`GetBestRoute` / `GetAdaptersAddresses` (経路検出) |
| `-lole32` | COM。WASAPI の初期化 |
| `-lwinmm` | マルチメディア タイマ |
| `-lm` | 数学関数 (音声合成) |

> `-lshlwapi` は**不要**です。`StrStrIA` への依存を除去し、ロケール非依存の
> 自前 ASCII 比較に置き換えたため (トルコ語ロケールの `I` 問題も回避)。

### libslirp なしでビルドする場合

`-DVMODEM_HAVE_LIBSLIRP` と `-lslirp` を外すと、NAT バックエンドが
`loopback` / `none` のみになります (PPP の動作確認は可能)。

---

## セットアップ

### 1. com0com で仮想 COM ペアを作る

com0com のセットアップ GUI で `CNCA0` ↔ `CNCB0` のペアを作成します。

- `CNCA0` … Windows のダイアルアップ (モデム) 側
- `CNCB0` … VModem 側 (`config.ini` の `com_port`)

### 2. 標準モデムをインストール

デバイス マネージャー → 操作 → レガシ ハードウェアの追加
→ 手動で選択 → モデム → 「モデムを一覧から選択する」
→ **通信ケーブル経由の標準モデム** を選び、ポートに `CNCA0` を指定。

### 3. ダイアルアップ接続を作成

設定 → ネットワーク → ダイヤルアップ →
電話番号に `config.ini` に書いた番号 (既定は `0120-000-0000`) を入力。

### 4. VModem を起動してから接続

```bash
vmodem.exe
```

起動後に Windows 側で「接続」を押します。

---

## 実行時オプション

```
vmodem.exe [オプション]

  -c, --config <path>   設定ファイル (既定: config.ini)
  -p, --port <name>     COM ポート名を上書き (例: CNCB0)
  -v, --verbose         ログレベルを DEBUG に
      --trace           ログレベルを TRACE に (バイト単位のダンプ)
  -q, --quiet           音を出さない
      --wav <path>      合成音を WAV に書き出す (デバッグ用)
      --net <mode>      slirp | loopback | none
  -h, --help            ヘルプ
```

---

## 設定 (config.ini)

主要な項目のみ抜粋します。全項目にコメントが付いているので実ファイルも参照してください。

```ini
[general]
com_port     = CNCB0        ; VModem が開く側
audio_enable = true
log_level    = 3            ; 0=none 1=error 2=warn 3=info 4=debug 5=trace

[ppp]
server_ip = 192.168.99.1    ; VModem (仮想 ISP) 側
client_ip = 192.168.99.2    ; Windows RAS に払い出すアドレス
dns1      = 8.8.8.8
dns2      = 8.8.4.4

[network]
mode = slirp                ; slirp | loopback | none

[ISP_1]
number   = 0120-000-0000
speed    = 33600
protocol = V34PLUS          ; V21 V22 V22BIS V32 V32BIS V34 V34PLUS V90
```

### DNS について

`mode = slirp` の時、`dns1` / `dns2` は **libslirp の内蔵 DNS プロキシの
アドレスで自動的に上書き**されます (既定 `192.168.99.3`)。

これは libslirp の仕様上必須です。libslirp は宛先が `vnameserver` と
**完全に一致する時だけ** DNS を代理してくれます
(`src/socket.c` の `sotranslate_out4`)。`8.8.8.8` をそのままゲストに教えると、
libslirp はそれを「ただの外部宛 UDP」として扱い、NAT の外に出そうとして失敗します。

---

## トラブルシューティング

### PPP は繋がるのにインターネットに出られない

**本バージョンで修正済みの症状です。** 参考のため原因を記録します。

Windows の RAS はダイアルアップ接続が確立すると、既定で

```
0.0.0.0/0 → 192.168.99.1 (PPP インタフェース)
```

というデフォルト経路を追加します (「リモート ネットワークでデフォルト
ゲートウェイを使う」)。VPN を繋ぐと全通信が VPN に入るのと同じ挙動です。

その結果、**VModem 自身**が libslirp 経由で `sendto(8.8.8.8)` を発行すると、
OS は経路表に従って「PPP インタフェースへ送れ」と判断します。つまり自分が
作った仮想回線に自分で送り返す**自己参照ループ**になり、Windows は即座に
`WSAENETUNREACH` を返します。パケットは NIC から 1 バイトも出ていません。

このとき libslirp は送信失敗を ICMP Destination Unreachable としてゲストに
返すため、ログには次のように「送信だけがひたすら出て ICMP だけが返る」という
特徴的なパターンが現れます。

```
nat tx: 192.168.99.2 > 8.8.8.8 UDP 69B      ← 大量に出る
nat rx: 192.168.99.1 > 192.168.99.2 ICMP 97B ← ICMP だけ返る
```

**対策 (実装済み)**
起動時に `GetBestRoute` と `GetAdaptersAddresses` でホストの実 NIC の
アドレスを検出し、`SlirpConfig.outbound_addr` に設定します。libslirp は
全ての外向きソケットをこのアドレスに `bind()` するため、Windows の
strong host model (RFC 6419, Vista 以降 IPv4 の既定) により送信インタフェースが
実 NIC に固定され、PPP へのデフォルト経路を迂回できます。

自動検出に失敗した場合は次の警告が出ます。

```
nat(slirp): ホストの外向きアドレスを特定できなかった。...
```

その場合は手動で回避してください。
ダイアルアップ接続のプロパティ → ネットワーク → TCP/IPv4 → 詳細設定 →
**「リモート ネットワークでデフォルト ゲートウェイを使う」のチェックを外す**。

### 「エントリ ポイント slirp_pollfds_fill_socket が見つかりません」

libslirp 4.8.0 の DLL でこのエラーが出る場合、古いビルドを使っています。
本バージョンでは `slirp_pollfds_fill_socket` を静的にインポートせず
`GetProcAddress` で動的に解決するため、このエラーは発生しません。

### `slirp_new に失敗` と出る

同梱ヘッダ (4.9.3) を見て `SlirpConfig.version = 6` を渡し、DLL が 4.8.0
(上限 5) だったために `slirp_new` が NULL を返す、という古いビルドの症状です。
本バージョンでは DLL の実バージョンから上限を決めるため発生しません。

### 音が出ない

- `audio_enable = true` を確認
- `audio_device` を空にして既定デバイスを使う
- `-q` を付けていないか確認

---

## libslirp のバージョン差異

`libslirp-0.dll` は 4.8.0 と 4.9.x で ABI が異なります。本プログラムは
**実行時に**差異を吸収します。

| 項目 | 4.8.0 | 4.9.x |
|---|---|---|
| `SLIRP_CONFIG_VERSION_MAX` | 5 | 6 |
| `slirp_pollfds_fill_socket` | なし | あり |
| `slirp_os_socket` 型 | なし | あり |
| `SlirpCb.register_poll_socket` | なし | あり |
| `SlirpConfig.outbound_addr` | **あり** (version ≥ 2) | **あり** (version ≥ 2) |

処理の要点:

1. `slirp_version_string()` (両バージョンに存在) で DLL の実バージョンを取得
2. 渡す `cfg.version` を「ヘッダの上限」と「DLL の上限」の小さい方に決定。
   ただし `outbound_addr` を有効に保つため **2 未満には下げない**
3. `slirp_pollfds_fill_socket` は `GetProcAddress` で解決し、
   無ければ `slirp_pollfds_fill` にフォールバック

3 が重要です。存在しない関数を静的にインポートすると Windows のローダが
起動時点で解決に失敗し、exe が全く動かなくなります。

---

## Windows 固有の注意点 (実装メモ)

移植で踏みやすい罠を `include/vmodem/vm_winsock.h` に集約しています。

### 1. `winsock2.h` は `windows.h` より先に

順序を逆にすると `windows.h` が古い `winsock.h` を取り込み、
`struct sockaddr_in` などが二重定義になります。

### 2. `WSAStartup` は自分で 2.2 を要求する

libslirp は内部で `MAKEWORD(2, 0)` しか要求せず (`src/slirp.c`)、
さらに `atexit(winsock_cleanup)` を登録します。`WSAPoll` は **2.2 が必要**
なので、アプリ側で明示的に `WSAStartup(MAKEWORD(2, 2), ...)` を呼びます
(`src/net/vm_winsock.c`)。

### 3. `WSAPoll` の `events` に立てて良いフラグは限られる

MSDN の規定により `events` に指定できるのは
`POLLRDNORM` / `POLLRDBAND` / `POLLWRNORM` (と合成の `POLLIN` / `POLLOUT`) のみ。
`POLLERR` / `POLLHUP` / `POLLNVAL` は **revents 専用**で、
`POLLPRI` を立てると **`WSAPoll` 自体が失敗**します。

libslirp は `SLIRP_POLL_ERR` / `HUP` / `PRI` を要求してくるため、そのまま
`WSAPoll` に渡すと `WSAEINVAL` で失敗します。すると
`slirp_pollfds_poll(slirp, select_error=1, ...)` となり、libslirp は
**全ソケットの読み書き処理をスキップ**します
(`src/slirp.c` の `if (!select_error)`)。
結果として「PPP は動くが通信だけ一切起きない」状態になります。

対策として `src/net/vm_nat_slirp.c` の `slirp_to_native()` は
`POLLIN` / `POLLOUT` (と Win32 では `POLLRDBAND`) のみを出力します。

### 4. Win64 の `SOCKET` を `int` に切り詰めない

`SOCKET` は `UINT_PTR` (8 バイト) ですが、旧 API の `SlirpAddPollCb` は
`int fd` を取ります。libslirp 内部で切り詰めが起き、
`Truncating socket to int failed!` の警告と共に
**一部の接続だけが突然死ぬ**という再現性の低い不具合になります。
4.9 以降の `slirp_pollfds_fill_socket` を使えば回避できます。

### 5. `SlirpConfig` / `SlirpCb` は値渡しのバージョン付き構造体

`version` フィールドで libslirp が読むメンバが決まります。手書きで再現すると
DLL のバージョン差でスタックを踏み抜くため、必ず本物のヘッダを使い、
`memset` で 0 埋めしてから必要なメンバだけ埋めます。

### 6. `outbound_addr` はポインタのまま保持される

libslirp は `slirp->outbound_addr = cfg->outbound_addr;` と代入するだけで
**コピーしません** (`src/slirp.c`)。そして送信ソケットを作るたびに
`slirp_bind_outbound()` からこの領域を参照します。

スタック上の `sockaddr_in` を渡すと、関数を抜けた瞬間に解放済みスタックを
読み続けることになります。最初の数パケットは偶然通り、その後ランダムに
壊れるという最悪の不具合になるため、実体を `Slirp` と同じ寿命を持つ
構造体 (`slirp_impl_t`) の中に置いています。

### 7. `WSAPoll` は fds が空だと失敗する

POSIX の `poll(NULL, 0, t)` は単なる sleep ですが、`WSAPoll` は
`WSAEINVAL` を返します。監視対象が無い時は `Sleep()` に分岐します。

### 8. poll のタイムアウト下限

`slirp_pollfds_fill` はタイムアウトを短くする方向にのみ書き換えるため、
TCP が活発だと 0 になり busy loop で 1 コアを焼き切ります。
電話回線速度では 1ms の遅延は無意味なので下限 1ms を設けています。

---

## ディレクトリ構成

```
include/vmodem/     公開ヘッダ (各ファイル冒頭に設計意図と罠を記載)
include/libslirp*.h libslirp の API ヘッダ (BSD-3-Clause)
src/core/           設定・ログ・リングバッファ
src/audio/          WASAPI / null 出力
src/dsp/            トーン合成・V.8 / V.34 ハンドシェイク音・リサンプラ
src/modem/          AT コマンド・接続シーケンス・全体制御
src/net/            PPP / HDLC / 疑似 Ethernet / NAT / Winsock / 経路検出
src/serial/         COM ポート入出力
tests/              各モジュールの単体テスト
```

---

## ライセンス

本プロジェクトのソースコードは **BSD-2-Clause** です。詳細および
サードパーティ コンポーネント (libslirp: LGPL-2.1-or-later を動的リンク、
com0com: GPL-3.0 を別プロセスとして利用) の扱いは [LICENSE](LICENSE) を
参照してください。

libslirp は動的リンクのみで利用し、ソースを取り込んでいないため、
本プロジェクト自身のライセンスは BSD-2-Clause を維持できます。

---

## 免責

本ソフトウェアは学習・懐古・検証を目的としたエミュレータです。
実在の電話網には一切接続しません。設定する電話番号には**実在しない番号**を
使用してください。
