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
gcc -O2 -std=c99 -Wall -Wextra -Iinclude -DVMODEM_HAVE_LIBSLIRP src/main.c src/core/vm_config.c src/core/vm_log.c src/core/vm_ringbuf.c src/core/vm_types.c src/audio/vm_audio.c src/audio/vm_audio_null.c src/audio/vm_audio_wasapi.c src/dsp/vm_handshake.c src/dsp/vm_resample.c src/dsp/vm_tone.c src/dsp/vm_v34.c src/dsp/vm_v8.c src/modem/vm_at.c src/modem/vm_modem.c src/modem/vm_sequence.c src/net/vm_eth.c src/net/vm_hdlc.c src/net/vm_hostroute.c src/net/vm_routeguard.c src/net/vm_rate.c src/net/vm_netguard.c src/net/vm_nat.c src/net/vm_nat_loopback.c src/net/vm_nat_slirp.c src/net/vm_ppp.c src/net/vm_winsock.c src/serial/vm_serial.c src/serial/vm_serial_win32.c -o vmodem.exe -L. -lslirp -lws2_32 -liphlpapi -lole32 -lwinmm -lfwpuclnt -lm
```

### リンクするライブラリの理由

| ライブラリ | 用途 |
|---|---|
| `-lslirp` | libslirp (ユーザモード NAT) |
| `-lws2_32` | Winsock2。`WSAStartup` / `WSAPoll` |
| `-liphlpapi` | IP Helper。`GetBestRoute` / `GetAdaptersAddresses` (経路検出) |
| `-lfwpuclnt` | Windows Filtering Platform。一時的な LAN / Wi-Fi 利用制限 |
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

### 4. 管理者として VModem を起動してから接続

```bash
vmodem.exe
```

`mode = slirp` は管理者権限と Windows の **Base Filtering Engine (BFE)**
サービスが必要です。NAT や通信制限を初期化できない場合は起動を中止し、
「繋がったように見えるだけ」のループバック接続へは切り替えません。

起動後に Windows 側で「接続」を押します。RAS の IPv4 詳細設定で
**「リモート ネットワークでデフォルト ゲートウェイを使う」を有効**にしてください。

### LAN / Wi-Fi の専用利用と解除

`mode = slirp` では、起動完了から終了まで Ethernet / Wi-Fi 上の通常の
アプリ通信を IPv4 / IPv6 ともに制限します。VModem の実行ファイルだけが
物理回線へ直接通信でき、他のアプリは RAS (PPP) 経由で IPv4 通信します。
全 Ethernet / Wi-Fi インターフェースに適用するため、別の LAN や IPv6 への
迂回も制限します。物理アダプタ自体は有効なままです。
localhost と DHCP の維持通信は例外です。独自の VPN / 仮想アダプタを通る経路の
制御は対象外です。

- ダイアル前・RAS 切断後も、VModem 起動中は通常アプリから直接接続できません。
- **Ctrl-C で VModem を終了すると制限が解除され、LAN / Wi-Fi の通常利用に戻ります。**
- WFP の動的セッションなので、プロセスの強制終了やクラッシュ時も Windows が
  フィルタを回収します。アダプタ設定・経路表・永続ファイアウォール設定は変更しません。
- `loopback` / `none` は通信検証専用で、物理回線の制限を行いません。

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

`dns1` / `dns2` は RAS にそのまま通知し、NAT 経由で利用します。
既定値は `8.8.8.8` / `8.8.4.4` です。外部 DNS が制限されているネットワークでは、
ルーターなど実際に利用できる DNS サーバーの IPv4 アドレスを設定してください。

以前の実装は DNS を仮想プロキシ `192.168.99.3` へ強制的に上書きしていました。
同一 PC の RAS がその DNS をホストへ登録すると、libslirp のホスト DNS 自動検出が
仮想プロキシ自身を選び、名前解決が自己参照する可能性があります。
現在はホスト DNS の自動検出・仮想 DNS プロキシを使いません。

---

## トラブルシューティング

### PPP は繋がるのにインターネットに出られない

RAS の接続状態だけでは NAT の疎通は確認できません。次を確認してください。

1. `mode = slirp` でビルドし、libslirp DLL を配置する。
   NAT 初期化失敗時はエラーで終了します。`loopback` / `none` にインターネット機能はありません。
2. 管理者として、利用できる LAN / Wi-Fi に接続した状態で起動する。
   起動ログの `outbound=... ifindex=...` に物理インターフェースが表示されること。
3. RAS のデフォルトゲートウェイを有効にする。無効だと通常アプリの直接通信は
   制限されるため、PPP を経由できません。
4. IP 宛の TCP 接続は通るが名前解決が失敗する場合は `[ppp] dns1` / `dns2` を確認する。
   Windows の「インターネットなし」という表示だけで判断せず、実際の名前解決と HTTPS 接続で確認する。

実装では libslirp の各ソケットに送信元 IPv4 と **`IP_UNICAST_IF`** を設定し、
RAS のデフォルト経路へ NAT 通信が戻ることを防ぎます。
送信元 IP の `bind()` だけでは、宛先に対する経路選択を固定できません。
また、最初の UDP が即座に期限切れにならないよう、リンクアップ時に
libslirp の時刻を更新してからパケットを処理します。

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

## 検証

既存の `tests/test_nat.c` は PPP / HDLC / ARP / ループバック NAT を検証します。
追加の `tests/test_slirp.c` は POSIX 上で **実際の libslirp** とローカルサーバーを
使い、最初の UDP の往復と TCP ハンドシェイク・双方向ペイロードを確認します。

```bash
gcc -O2 -std=c99 -Wall -Wextra -Werror -DVMODEM_HAVE_LIBSLIRP -Iinclude -Isrc/net tests/test_slirp.c src/net/vm_nat.c src/net/vm_nat_slirp.c src/net/vm_nat_loopback.c src/net/vm_eth.c src/net/vm_hostroute.c src/net/vm_winsock.c src/core/vm_log.c src/core/vm_types.c -lslirp -pthread -lm -o test_slirp
./test_slirp
```

Windows の制限・復元は RAS を切断した状態で、以下のヘルパーをビルドして
**管理者 PowerShell** から確認できます (COM ポート不要)。

```powershell
New-Item -ItemType Directory -Force work
gcc -O2 -std=c99 -Wall -Wextra -Werror -Iinclude tests/test_netguard_win32.c src/net/vm_netguard.c src/net/vm_winsock.c src/core/vm_log.c -lfwpuclnt -lole32 -lws2_32 -o work/test_netguard.exe
powershell -ExecutionPolicy Bypass -File tests/test_netguard.ps1
# IPv6 も到達できる環境の場合:
# powershell -ExecutionPolicy Bypass -File tests/test_netguard.ps1 -IPv6 2606:4700:4700::1111
```

このテストはヘルパー自身の外向き通信、他プロセスの直接 TCP 接続の遮断と、
正常終了・強制終了の両方での復元を確認します。
実機ではさらに VModem を起動し、ダイアル前の通信不可 → RAS 接続後の DNS / HTTPS
疎通 → RAS 切断後の通信不可 → VModem 終了後の LAN 復元を確認してください。
有線のみ・Wi-Fi のみ・両方が有効な場合を対象にします。

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

### Windows 11 ネットワーク精査版 (2026-11-12)

[精査結果と検証範囲](docs/win11-network-audit.md)。RAS接続後に物理NICの
`DisableDefaultRoutes` が起動前から変更された場合、その項目だけを復元する。
ルート表に既定経路が見えていても無効扱いとなるケースへの対策。
COM待機時のCPU空回りと、シリアル/WSAPoll失敗の握り潰しも修正。
IPv6 PPPとWindowsのDNSサーバー選択は今回変更していない。
実Windowsでのインターネット到達はホスト試験では保証できない。

### 実転送速度の制限

接続速度を表示するだけだった不具合を修正し、IPv4パケットの上り・下りを
それぞれ交渉速度に制限する。33.6Kbpsなら各方向最大約4.2KB/秒で、
アプリの実データ速度はヘッダー分だけ下がる。有線LANの1Gbps表示は変わらない。
[方式・試験・制限](docs/bandwidth-limit.md)を参照。
