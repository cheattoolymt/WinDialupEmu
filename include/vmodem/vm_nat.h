/*
 * vm_nat.h - ユーザモード NAT 層 (libslirp バックエンド)
 *
 * VModem - Windows 用 本物のダイアルアップモデムエミュレータ
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * この層の役割
 * ===========================================================================
 * 仕様書の「接続後は LAN 経由でインターネットに接続」を実現する部分。
 *
 *   Windows RAS ──PPP──> vm_ppp ──生 IP──> vm_eth ──Eth フレーム──> vm_nat
 *                                                                      │
 *                                              ホスト OS のソケット <──┘
 *
 * PPP で受け取った生 IP パケットを、**管理者権限もドライバも使わずに**
 * 実インターネットへ出す必要がある。方式は 3 つ考えられた。
 *
 *   (1) WinTUN + Windows の ICS (インターネット接続共有)
 *         → WinTUN のドライバ署名と管理者権限が必要。ICS はレジストリを
 *           触る上、Windows 11 では設定が不安定。ユーザ体験が悪い。
 *
 *   (2) Raw socket で自前 NAT
 *         → Windows の raw socket は Vista 以降 TCP 送信が禁止されている。
 *           そもそも実装不可。
 *
 *   (3) ユーザモード TCP/IP スタック (libslirp)  ★採用★
 *         → 権限不要。libslirp が IP パケットを解釈し、TCP/UDP を
 *           **通常の Winsock ソケット** に変換して外に出す。
 *           QEMU の -net user と全く同じ仕組みで、20 年以上の実績がある。
 *
 * ===========================================================================
 * ライセンス隔離 (仕様書の OSS 方針)
 * ===========================================================================
 * libslirp は **LGPL-2.1+**。仕様書は「GPL 汚染を伝播させないこと」を
 * 求めている。LGPL は以下を満たせば我々のコード (BSD-2-Clause) に
 * 波及しない。
 *
 *   - 動的リンクであること (静的リンクは条項 6 の制約を受ける)
 *   - ユーザが libslirp を差し替えられること
 *   - libslirp 自身の改変部分のみ LGPL で公開すること (我々は改変しない)
 *
 * よって libslirp は **DLL として動的リンク**する。
 * (scripts/setup-libslirp.ps1 が MSYS2 から libslirp-0.dll を取得する)
 *
 * ===========================================================================
 * 難所 5: 「実行時 dlopen で構造体を推測する」のは罠
 * ===========================================================================
 * 一見、LoadLibrary + GetProcAddress で完全に実行時解決すれば
 * ヘッダも不要で綺麗に見える。しかしこれは危険である。
 *
 * libslirp の API は **構造体を値で渡す**:
 *
 *      Slirp *slirp_new(const SlirpConfig *cfg, const SlirpCb *cb, void *op);
 *
 * SlirpConfig には version フィールドがあり、version 1 から 6 まで
 * メンバが後方に追加され続けている。SlirpCb も同様で、
 * version 2 で init_completed / timer_new_opaque が増えた。
 * つまり **構造体レイアウトを我々が手書きで再現すると、DLL の
 * バージョンが違った瞬間にスタックを踏み抜く**。しかも症状は
 * 「slirp_new が NULL を返す」ではなく「動くが時々クラッシュする」。
 *
 * したがって:
 *   - 構造体定義は必ず本物の libslirp.h から取る (ビルド時にヘッダ必須)
 *   - リンクは DLL への動的リンク (import library 経由) にする
 *     → これで LGPL の要件は満たされ、かつ ABI 安全
 *   - libslirp が無い環境でもビルドが通るよう、コンパイル時に
 *     VMODEM_HAVE_LIBSLIRP で切り替える
 *
 * ===========================================================================
 * 難所 6: libslirp のコールバックは「NULL 可」ではない
 * ===========================================================================
 * SlirpCb の関数ポインタのうち、以下は **必ず実装しなければならない**。
 * NULL を置くと初回のタイマ登録で即クラッシュする。
 *
 *      send_packet         : slirp からゲストへのフレーム (必須)
 *      clock_get_ns        : 単調増加クロック。NULL だと TCP 再送が壊れる
 *      timer_new / _free / _mod : slirp 内部の TCP タイマ。NULL は即死
 *      guest_error         : ゲストの不正パケット通知 (ログのみでよい)
 *      register_poll_fd    : Windows では実質何もしなくてよいが NULL 不可
 *      unregister_poll_fd  : 同上
 *      notify              : 別スレッドからの起床通知。単一スレッドなら空実装
 *
 * clock_get_ns は「単調増加」が絶対条件。GetTickCount() は 49.7 日で
 * 巻き戻るので使えない。QueryPerformanceCounter か
 * GetTickCount64 を使う (本実装は vm_nat_now_ns() に集約)。
 *
 * ===========================================================================
 * 難所 7: ポーリングモデルの噛み合わせ
 * ===========================================================================
 * libslirp は select/poll 前提の API を持つ:
 *
 *      void slirp_pollfds_fill(Slirp *, uint32_t *timeout,
 *                              SlirpAddPollCb add_poll, void *opaque);
 *      void slirp_pollfds_poll(Slirp *, int select_error,
 *                              SlirpGetREventsCb get_revents, void *opaque);
 *
 * 使い方が独特で、間違えると「繋がるが異常に遅い」という症状になる。
 *
 *   1. slirp_pollfds_fill を呼ぶと、監視すべき fd が add_poll コールバック
 *      経由で 1 つずつ渡される。**我々はそれを配列に溜める**。
 *      返り値ではなく引数の timeout も更新される (次に起きるべき時刻)。
 *   2. 我々が select() (Windows なら WSAPoll) を実行する。
 *   3. slirp_pollfds_poll を呼ぶ。slirp は get_revents コールバックで
 *      「index 番目の fd の結果は?」と聞いてくるので、
 *      **fill の時に付けた index と同じ順序**で答える必要がある。
 *      ここで順序を崩すと、slirp が別のソケットのイベントを誤認する。
 *
 * さらに Windows 固有の罠:
 *   - libslirp の poll フラグ (SLIRP_POLL_IN 等) は POSIX の POLLIN とは
 *     別の独自 enum。必ず変換テーブルを通す。
 *   - Windows の select() は fd_set が「ソケットの配列」なので
 *     FD_SETSIZE = 64 の制限がある。WSAPoll を使うか FD_SETSIZE を
 *     再定義する。本実装は WSAPoll を使う。
 *   - timeout が 0 のまま select すると 100% CPU を焼く。下限を設ける。
 * ===========================================================================
 */
#ifndef VMODEM_VM_NAT_H
#define VMODEM_VM_NAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vmodem/vm_types.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * バックエンド種別
 * -------------------------------------------------------------------------- */
typedef enum {
    VM_NAT_NONE = 0,    /* NAT 無効。IP パケットは破棄 (PPP 単体試験用)  */
    VM_NAT_LOOPBACK,    /* 内蔵の最小応答器。ICMP Echo と UDP Echo に応答 */
    VM_NAT_SLIRP,       /* libslirp (実インターネット接続)                */
    VM_NAT__COUNT
} vm_nat_backend_t;

const char      *vm_nat_backend_name(vm_nat_backend_t b);
vm_nat_backend_t vm_nat_backend_from_string(const char *s);

/* 実行中のバイナリで利用可能か (libslirp がリンクされているか) */
bool vm_nat_backend_available(vm_nat_backend_t b);

/* --------------------------------------------------------------------------
 * 設定
 * -------------------------------------------------------------------------- */
typedef struct {
    vm_nat_backend_t backend;

    /* 仮想 LAN の構成。すべてホストバイトオーダ。 */
    uint32_t network;       /* 例 192.168.99.0  */
    uint32_t netmask;       /* 例 255.255.255.0 */
    uint32_t host_ip;       /* 例 192.168.99.1  仮想ゲートウェイ */
    uint32_t guest_ip;      /* 例 192.168.99.2  RAS へ払い出した IP */
    uint32_t dns_ip;        /* 例 192.168.99.3  slirp の DNS 代理 */

    int  mtu;               /* 0 なら 1500 */
    bool restricted;        /* true でホスト LAN へのアクセスを禁止 */
    bool disable_host_loopback; /* true で 127.0.0.0/8 へのアクセスを禁止 */
} vm_nat_cfg_t;

void vm_nat_cfg_defaults(vm_nat_cfg_t *cfg);

/* --------------------------------------------------------------------------
 * 統計
 * -------------------------------------------------------------------------- */
typedef struct {
    uint64_t tx_pkts;       /* ゲスト -> インターネット */
    uint64_t tx_bytes;
    uint64_t rx_pkts;       /* インターネット -> ゲスト */
    uint64_t rx_bytes;
    uint64_t drops;
    uint64_t guest_errors;  /* libslirp が報告したゲストの不正パケット */
    uint64_t polls;
    uint64_t timers_active;
} vm_nat_stats_t;

/* --------------------------------------------------------------------------
 * ゲストへの送出コールバック
 * ゲストへ届けるべき生 IPv4 パケットが渡される。実装は vm_ppp_send_ip()
 * を呼ぶだけでよい。
 * -------------------------------------------------------------------------- */
typedef void (*vm_nat_ip_cb)(void *user, const uint8_t *pkt, int len);

/* 不完全型。実体は src/net/vm_nat.c 内。 */
typedef struct vm_nat_s vm_nat_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/*
 * 生成。cfg.backend が利用不可なら VM_ERR_UNSUPPORTED を返す
 * (呼び出し側は VM_NAT_LOOPBACK にフォールバックしてよい)。
 */
vm_err_t vm_nat_create(vm_nat_t **out, const vm_nat_cfg_t *cfg,
                       vm_nat_ip_cb cb, void *user);

void vm_nat_destroy(vm_nat_t *n);

/*
 * リンクアップ通知。
 * ここで Gratuitous ARP を打ち込み、slirp の ARP テーブルを先に埋める
 * (vm_eth.h 難所 1)。IPCP が Opened になった直後に呼ぶ。
 */
vm_err_t vm_nat_link_up(vm_nat_t *n);

/* リンクダウン通知。確立中の TCP セッションは破棄される。 */
void vm_nat_link_down(vm_nat_t *n);

/*
 * ゲスト (PPP) から来た生 IPv4 パケットを投入する。
 * 内部で Ethernet ヘッダを付けてバックエンドへ渡す。
 */
vm_err_t vm_nat_input_ip(vm_nat_t *n, const uint8_t *pkt, int len);

/*
 * 定期処理。ソケットのポーリングとタイマ処理を行う。
 * 戻り値: 次に呼ぶべきまでの推奨待ち時間 (ms)。
 * ゲストへ届くパケットは cb 経由で通知される。
 *
 * max_block_ms は select の最大ブロック時間。0 を渡すと非ブロッキング。
 */
int vm_nat_poll(vm_nat_t *n, int max_block_ms);

/* 統計取得 */
void vm_nat_get_stats(const vm_nat_t *n, vm_nat_stats_t *st);

/* 人間可読な 1 行状態 */
const char *vm_nat_status(const vm_nat_t *n, char *buf, size_t size);

/* 実際に使われているバックエンド */
vm_nat_backend_t vm_nat_active_backend(const vm_nat_t *n);

/*
 * NAT が用意した DNS 代理アドレス (ホストオーダ)。
 *
 * slirp バックエンドではこのアドレスの 53/udp 宛だけが
 * ホストの実 DNS サーバへ差し替えられる。したがって PPP の IPCP で
 * ゲストに配る DNS はこの値でなければならない。
 * 詳しい理屈は src/net/vm_nat.c の実装コメントを参照。
 *
 * n == NULL の時は 0 を返す。
 */
uint32_t vm_nat_dns_ip(const vm_nat_t *n);

/* 単調増加ナノ秒クロック (難所 6)。バックエンド実装からも使う。 */
int64_t vm_nat_now_ns(void);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_NAT_H */
