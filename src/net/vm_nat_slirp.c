/*
 * vm_nat_slirp.c - libslirp バックエンド
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *   (このファイル自体は BSD。libslirp は LGPL-2.1+ で **動的リンク**する。
 *    詳細なライセンス隔離の議論は include/vmodem/vm_nat.h を参照)
 *
 * ===========================================================================
 * ビルド条件
 * ===========================================================================
 * VMODEM_HAVE_LIBSLIRP が定義されている時のみ実体を持つ。
 * 未定義なら vm_nat_ops_slirp() が NULL を返し、上位が
 * 「libslirp が組み込まれていません」と案内する。
 *
 * libslirp.h が必要な理由 (難所 5) はヘッダに書いた通り。要点だけ再掲:
 *   SlirpConfig / SlirpCb は **値渡しの構造体** で、version により
 *   メンバが増える。手書きで再現すると DLL バージョン差でスタックを
 *   踏み抜き、「動くが時々落ちる」最悪の症状になる。
 *   よって必ず本物のヘッダを使い、動的リンク (import library) にする。
 * ===========================================================================
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "vm_nat_internal.h"

#ifndef VMODEM_HAVE_LIBSLIRP

/* libslirp 無しビルド: 利用不可を返すだけ */
const vm_nat_backend_ops_t *vm_nat_ops_slirp(void)
{
    return NULL;
}

#else /* VMODEM_HAVE_LIBSLIRP */

#include <libslirp.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifndef POLLIN
     /* WSAPoll の定数は winsock2.h にあるが古い SDK では欠ける */
#    define POLLRDNORM 0x0100
#    define POLLRDBAND 0x0200
#    define POLLIN     (POLLRDNORM | POLLRDBAND)
#    define POLLPRI    0x0400
#    define POLLWRNORM 0x0010
#    define POLLOUT    (POLLWRNORM)
#    define POLLERR    0x0001
#    define POLLHUP    0x0002
#    define POLLNVAL   0x0004
#  endif
   typedef WSAPOLLFD vm_pollfd_t;
#  define VM_POLL(fds, n, to) WSAPoll((fds), (ULONG)(n), (int)(to))
#else
#  include <poll.h>
#  include <errno.h>
   typedef struct pollfd vm_pollfd_t;
#  define VM_POLL(fds, n, to) poll((fds), (nfds_t)(n), (int)(to))
#endif

/* --------------------------------------------------------------------------
 * 監視 fd の上限
 * --------------------------------------------------------------------------
 * libslirp は TCP セッション 1 本ごとに 1 fd を要求する。
 * 電話回線速度 (最大 33.6kbps) で同時に張れるセッション数は
 * 現実には数十本が上限だが、ブラウザは平気で 100 本開けようとするので
 * 余裕を持たせる。
 */
#define VM_SLIRP_MAX_FDS 512

/* libslirp のタイマ登録上限 */
#define VM_SLIRP_MAX_TIMERS 64

typedef struct {
    void (*cb)(void *opaque);
    void     *cb_opaque;
    int64_t   expire_ns;      /* 0 = 停止中 */
    bool      used;
} slirp_timer_t;

typedef struct {
    Slirp        *slirp;
    vm_nat_t     *nat;        /* 逆参照 (コールバックから使う) */

    /* ---- ポーリング用 (難所 7) ---- */
    vm_pollfd_t   fds[VM_SLIRP_MAX_FDS];
    int           nfds;

    /* ---- タイマ (難所 6) ---- */
    slirp_timer_t timers[VM_SLIRP_MAX_TIMERS];
    int           n_timers;

    uint64_t      poll_calls;
    uint64_t      timer_fires;
} slirp_impl_t;

/* ==========================================================================
 * poll フラグ変換 (難所 7 の Windows 固有部)
 * ==========================================================================
 * libslirp の SLIRP_POLL_* は **独自 enum** であり、POSIX の POLLIN 等と
 * 値が一致する保証はない。ヘッダを見ると
 *      SLIRP_POLL_IN = 1 << 0, OUT = 1 << 1, PRI = 1 << 2,
 *      ERR = 1 << 3, HUP = 1 << 4
 * だが、これに依存して直接ビットを渡すと libslirp の更新で壊れる。
 * 必ず変換関数を通す。
 */
static int slirp_to_native(int ev)
{
    int r = 0;
    if (ev & SLIRP_POLL_IN)  r |= POLLIN;
    if (ev & SLIRP_POLL_OUT) r |= POLLOUT;
    if (ev & SLIRP_POLL_PRI) r |= POLLPRI;
    if (ev & SLIRP_POLL_ERR) r |= POLLERR;
    if (ev & SLIRP_POLL_HUP) r |= POLLHUP;
    return r;
}

static int native_to_slirp(int ev)
{
    int r = 0;
    if (ev & POLLIN)   r |= SLIRP_POLL_IN;
    if (ev & POLLOUT)  r |= SLIRP_POLL_OUT;
    if (ev & POLLPRI)  r |= SLIRP_POLL_PRI;
    if (ev & POLLERR)  r |= SLIRP_POLL_ERR;
    if (ev & POLLHUP)  r |= SLIRP_POLL_HUP;
    /*
     * POLLNVAL (無効な fd) は libslirp に SLIRP_POLL_ERR として伝える。
     * これを落とすと libslirp が閉じたソケットを永久に監視し続け、
     * poll が即座に返り続けて 100% CPU になる。
     */
    if (ev & POLLNVAL) r |= SLIRP_POLL_ERR;
    return r;
}

/* ==========================================================================
 * SlirpCb: ゲストへのフレーム送出
 * ==========================================================================
 * libslirp が「ゲスト (= Windows RAS) へこのフレームを届けろ」と言ってくる。
 * 我々は偽 Ethernet 層で剥がして PPP へ流す。その処理は共通部の
 * vm_nat_backend_recv_frame() に集約してあるので、ここは中継だけ。
 *
 * 戻り値は「送れたバイト数」。負値や 0 を返すと libslirp は
 * 送信失敗と見なして再送を試みるので、必ず len を返す。
 * (PPP 側でバッファが溢れても、ここで嘘をつく方が全体としては安定する。
 *  33.6kbps という帯域では TCP の輻輳制御が自然に落ち着かせてくれる)
 */
static ssize_t cb_send_packet(const void *buf, size_t len, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;

    if (si == NULL || si->nat == NULL || buf == NULL || len == 0)
        return (ssize_t)len;

    if (len > VM_NAT_FRAME_MAX) {
        VM_LOGW("nat(slirp): 過大なフレーム %u バイトを破棄",
                (unsigned)len);
        return (ssize_t)len;
    }

    vm_nat_backend_recv_frame(si->nat, (const uint8_t *)buf, (int)len);
    return (ssize_t)len;
}

/* ==========================================================================
 * SlirpCb: ゲストエラー通知
 * ========================================================================== */
static void cb_guest_error(const char *msg, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;
    if (si != NULL)
        vm_nat_backend_guest_error(si->nat, msg);
}

/* ==========================================================================
 * SlirpCb: クロック (難所 6)
 * ==========================================================================
 * 単調増加が絶対条件。実装は vm_nat_now_ns() に集約済み。
 */
static int64_t cb_clock_get_ns(void *opaque)
{
    (void)opaque;
    return vm_nat_now_ns();
}

/* ==========================================================================
 * SlirpCb: タイマ (難所 6)
 * ==========================================================================
 * libslirp は TCP の再送・遅延 ACK・DHCP リースなどのために
 * タイマを要求する。NULL を置くと最初の TCP 接続で即クラッシュする。
 *
 * 重要な仕様:
 *   - timer_mod(t, expire_ms) の expire_ms は **絶対時刻 (ms)** で、
 *     clock_get_ns() / 1000000 と同じ基準。相対時間ではない。
 *     ここを相対だと勘違いすると、タイマが即座に発火し続けて
 *     CPU を焼くか、逆に永久に発火しなくなる。
 *   - 同じタイマに対して timer_mod が何度も呼ばれる。上書きが正しい。
 *   - timer_free 後に発火させてはいけない。
 *
 * 本実装は malloc を避け固定配列にする。理由: libslirp は
 * TCP セッションごとにタイマを作らず、グローバルに数本しか作らない
 * (fasttimo / slowtimo / DHCP / ICMP)。64 本で十分すぎる。
 */
static void *cb_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;
    int           i;

    if (si == NULL)
        return NULL;

    for (i = 0; i < VM_SLIRP_MAX_TIMERS; i++) {
        if (!si->timers[i].used) {
            si->timers[i].used      = true;
            si->timers[i].cb        = cb;
            si->timers[i].cb_opaque = cb_opaque;
            si->timers[i].expire_ns = 0;
            si->n_timers++;
            return &si->timers[i];
        }
    }

    VM_LOGE("nat(slirp): タイマ枠が枯渇 (%d 本)", VM_SLIRP_MAX_TIMERS);
    return NULL;
}

static void cb_timer_free(void *timer, void *opaque)
{
    slirp_impl_t  *si = (slirp_impl_t *)opaque;
    slirp_timer_t *t  = (slirp_timer_t *)timer;

    if (si == NULL || t == NULL)
        return;

    if (t->used) {
        t->used      = false;
        t->cb        = NULL;
        t->cb_opaque = NULL;
        t->expire_ns = 0;
        si->n_timers--;
    }
}

static void cb_timer_mod(void *timer, int64_t expire_time_ms, void *opaque)
{
    slirp_timer_t *t = (slirp_timer_t *)timer;
    (void)opaque;

    if (t == NULL || !t->used)
        return;

    /*
     * expire_time_ms は絶対時刻 (ms)。ns に直して保持する。
     * 0 以下を渡されたら「即発火」の意味なので現在時刻にする。
     */
    if (expire_time_ms <= 0)
        t->expire_ns = vm_nat_now_ns();
    else
        t->expire_ns = expire_time_ms * 1000000LL;
}

/* ==========================================================================
 * SlirpCb: poll fd の登録通知
 * ==========================================================================
 * Windows では実質何もする必要がないが、**NULL を置いてはいけない**。
 * libslirp のソースは NULL チェックせずに呼ぶ。
 *
 * POSIX で意味を持つのは「この fd を epoll に足しておけ」という
 * ヒントを与えたい実装向け。我々は毎回 fill でリストを作り直すので不要。
 */
static void cb_register_poll_fd(int fd, void *opaque)
{
    (void)fd; (void)opaque;
}

static void cb_unregister_poll_fd(int fd, void *opaque)
{
    (void)fd; (void)opaque;
}

/* ==========================================================================
 * SlirpCb: 別スレッドからの起床通知
 * ==========================================================================
 * libslirp が内部スレッド (DNS 解決など) から「poll を抜けろ」と
 * 言ってくる。我々は poll のタイムアウトを短く保っているので
 * 空実装でよいが、NULL は不可。
 */
static void cb_notify(void *opaque)
{
    (void)opaque;
}

/* ==========================================================================
 * SlirpCb 構造体
 * ==========================================================================
 * ★難所 6 の核心★
 * version を明示的に 1 にしている。理由:
 *   version 2 以上を宣言すると libslirp は init_completed /
 *   timer_new_opaque を「存在する」と見なして呼びに来る。
 *   我々がそれらを埋めていなければ NULL 呼び出しでクラッシュする。
 *   逆に version 1 を宣言しておけば、新しい libslirp でも
 *   version 1 の範囲しか触られないため **前方互換で安全**。
 *
 * 「使える機能を最大限使う」より「宣言した範囲を確実に埋める」。
 */
static const SlirpCb slirp_cb = {
    /* .send_packet         = */ cb_send_packet,
    /* .guest_error         = */ cb_guest_error,
    /* .clock_get_ns        = */ cb_clock_get_ns,
    /* .timer_new           = */ cb_timer_new,
    /* .timer_free          = */ cb_timer_free,
    /* .timer_mod           = */ cb_timer_mod,
    /* .register_poll_fd    = */ cb_register_poll_fd,
    /* .unregister_poll_fd  = */ cb_unregister_poll_fd,
    /* .notify              = */ cb_notify,
    /* 以降 (version >= 2) は 0 のまま。version=1 なので呼ばれない。 */
};

/* ==========================================================================
 * ポーリング用コールバック (難所 7)
 * ==========================================================================
 * slirp_pollfds_fill が add_poll を呼んでくる。
 * **返した index を slirp が覚えていて、後で get_revents で聞いてくる**。
 * したがって index は fds 配列の添字と 1:1 で一致させなければならない。
 * ここでズレると slirp が別のソケットのイベントを誤認し、
 * 「TCP が異常に遅い / 時々止まる」という症状になる。
 */
static int cb_add_poll(int fd, int events, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;
    int           idx;

    if (si == NULL || si->nfds >= VM_SLIRP_MAX_FDS)
        return -1;

    idx = si->nfds++;
#ifdef _WIN32
    si->fds[idx].fd = (SOCKET)fd;
#else
    si->fds[idx].fd = fd;
#endif
    si->fds[idx].events  = (short)slirp_to_native(events);
    si->fds[idx].revents = 0;

    return idx;                 /* ← この値が後で get_revents に渡る */
}

static int cb_get_revents(int idx, void *opaque)
{
    slirp_impl_t *si = (slirp_impl_t *)opaque;

    if (si == NULL || idx < 0 || idx >= si->nfds)
        return 0;

    return native_to_slirp((int)si->fds[idx].revents);
}

/* ==========================================================================
 * タイマ処理
 * ========================================================================== */
static void run_expired_timers(slirp_impl_t *si)
{
    int64_t now = vm_nat_now_ns();
    int     i;

    for (i = 0; i < VM_SLIRP_MAX_TIMERS; i++) {
        slirp_timer_t *t = &si->timers[i];

        if (!t->used || t->expire_ns == 0 || t->cb == NULL)
            continue;

        if (t->expire_ns <= now) {
            /*
             * 発火前に expire_ns を 0 にする。
             * コールバック内で timer_mod が呼ばれて再設定されるのが
             * 通常の流れなので、先に 0 にしないと再設定が上書きされて
             * 「タイマが二度と発火しない」ことになる。
             */
            t->expire_ns = 0;
            si->timer_fires++;
            t->cb(t->cb_opaque);
        }
    }
}

/* 次のタイマ期限までの ms。無ければ -1。 */
static int next_timer_ms(slirp_impl_t *si)
{
    int64_t now  = vm_nat_now_ns();
    int64_t best = -1;
    int     i;

    for (i = 0; i < VM_SLIRP_MAX_TIMERS; i++) {
        slirp_timer_t *t = &si->timers[i];
        if (!t->used || t->expire_ns == 0)
            continue;
        if (best < 0 || t->expire_ns < best)
            best = t->expire_ns;
    }

    if (best < 0)
        return -1;
    if (best <= now)
        return 0;

    return (int)((best - now) / 1000000LL);
}

/* ==========================================================================
 * vtable 実装
 * ========================================================================== */
static vm_err_t slirp_be_open(vm_nat_t *n)
{
    slirp_impl_t *si;
    SlirpConfig   cfg;
    struct in_addr net, mask, host, dhcp_start, dns;

    si = (slirp_impl_t *)calloc(1, sizeof(*si));
    if (si == NULL)
        return VM_ERR_NOMEM;

    si->nat = n;

    /*
     * ★難所 5 の実践★
     * memset で全域を 0 にしてから version を立て、必要なメンバだけ埋める。
     * こうすれば libslirp が version 1 より後に追加したメンバは
     * すべて 0 (= 既定動作) になり、ABI が食い違わない。
     * 構造体を手書きで再現しないこと、version を明示すること、
     * この 2 点で ABI 安全が保てる。
     */
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 1;

    /*
     * libslirp のアドレスはネットワークバイトオーダの in_addr。
     * 我々はホストオーダで持っているので htonl する。
     * ここを間違えると「192.168.99.1 のはずが 1.99.168.192 になる」
     * という分かりやすい壊れ方をするので、まだ幸運な部類。
     */
    net.s_addr        = htonl(n->cfg.network);
    mask.s_addr       = htonl(n->cfg.netmask);
    host.s_addr       = htonl(n->cfg.host_ip);
    dns.s_addr        = htonl(n->cfg.dns_ip);
    /*
     * DHCP は使わない (IP は PPP の IPCP で払い出す) が、
     * libslirp は dhcp_start が 0 だと内部で経路を作らないことがある。
     * guest_ip を指定しておく。
     */
    dhcp_start.s_addr = htonl(n->cfg.guest_ip);

    cfg.restricted           = n->cfg.restricted;
    cfg.in_enabled           = true;
    cfg.vnetwork             = net;
    cfg.vnetmask             = mask;
    cfg.vhost                = host;
    cfg.in6_enabled          = false;      /* IPV6CP は Reject 済み */
    cfg.vhostname            = "vmodem";
    cfg.tftp_server_name     = NULL;
    cfg.tftp_path            = NULL;
    cfg.bootfile             = NULL;
    cfg.vdhcp_start          = dhcp_start;
    cfg.vnameserver          = dns;
    cfg.vdnssearch           = NULL;
    cfg.vdomainname          = NULL;
    cfg.if_mtu               = (size_t)n->cfg.mtu;
    cfg.if_mru               = (size_t)n->cfg.mtu;
    cfg.disable_host_loopback = n->cfg.disable_host_loopback;
    cfg.enable_emu           = false;      /* 非推奨機能。使わない */

    si->slirp = slirp_new(&cfg, &slirp_cb, si);
    if (si->slirp == NULL) {
        VM_LOGE("nat(slirp): slirp_new に失敗");
        free(si);
        return VM_ERR_IO;
    }

    n->impl = si;

    {
        char a[16], b[16];
        snprintf(a, sizeof(a), "%u.%u.%u.%u",
                 (unsigned)((n->cfg.host_ip >> 24) & 0xFFu),
                 (unsigned)((n->cfg.host_ip >> 16) & 0xFFu),
                 (unsigned)((n->cfg.host_ip >> 8) & 0xFFu),
                 (unsigned)(n->cfg.host_ip & 0xFFu));
        snprintf(b, sizeof(b), "%u.%u.%u.%u",
                 (unsigned)((n->cfg.guest_ip >> 24) & 0xFFu),
                 (unsigned)((n->cfg.guest_ip >> 16) & 0xFFu),
                 (unsigned)((n->cfg.guest_ip >> 8) & 0xFFu),
                 (unsigned)(n->cfg.guest_ip & 0xFFu));
        VM_LOGI("nat(slirp): 起動 gateway=%s guest=%s mtu=%d restricted=%d",
                a, b, n->cfg.mtu, (int)n->cfg.restricted);
    }

    return VM_OK;
}

static void slirp_be_close(vm_nat_t *n)
{
    slirp_impl_t *si = (slirp_impl_t *)n->impl;

    if (si == NULL)
        return;

    VM_LOGI("nat(slirp): 終了 poll=%llu timer_fires=%llu",
            (unsigned long long)si->poll_calls,
            (unsigned long long)si->timer_fires);

    if (si->slirp != NULL)
        slirp_cleanup(si->slirp);

    free(si);
    n->impl = NULL;
}

static vm_err_t slirp_be_send_frame(vm_nat_t *n, const uint8_t *frame, int len)
{
    slirp_impl_t *si = (slirp_impl_t *)n->impl;

    if (si == NULL || si->slirp == NULL)
        return VM_ERR_STATE;

    /*
     * slirp_input は void。失敗を通知しない。
     * 内部で長さ不足なら黙って捨てるので、こちらで検算しておく。
     */
    if (len < VM_ETH_HLEN)
        return VM_ERR_INVAL;

    slirp_input(si->slirp, frame, len);
    return VM_OK;
}

/* --------------------------------------------------------------------------
 * ポーリング本体 (難所 7 の 3 段構え)
 * -------------------------------------------------------------------------- */
static int slirp_be_poll(vm_nat_t *n, int max_block_ms)
{
    slirp_impl_t *si = (slirp_impl_t *)n->impl;
    uint32_t      timeout;
    int           rc;
    int           tmr_ms;

    if (si == NULL || si->slirp == NULL)
        return 10;

    si->poll_calls++;

    /* ---- 段 1: 監視すべき fd を集める ---- */
    si->nfds = 0;
    timeout  = (max_block_ms > 0) ? (uint32_t)max_block_ms : 0u;

    slirp_pollfds_fill(si->slirp, &timeout, cb_add_poll, si);

    /*
     * ★ CPU 焼きの罠 ★
     * slirp_pollfds_fill は timeout を「もっと短くしたい」方向にだけ
     * 書き換える。TCP セッションが活発だと 0 になることがあり、
     * そのまま poll(…, 0) すると busy loop になって 1 コアを焼き切る。
     * 電話回線速度では 1ms の遅延など無意味なので、下限を設ける。
     */
    if (timeout == 0)
        timeout = 1;
    if (max_block_ms >= 0 && timeout > (uint32_t)max_block_ms)
        timeout = (uint32_t)max_block_ms;

    /* ---- 段 2: 実際に待つ ---- */
    if (si->nfds > 0) {
        rc = VM_POLL(si->fds, si->nfds, (int)timeout);
    } else {
        /*
         * 監視対象が無い。poll(NULL, 0, t) は POSIX では単なる sleep だが
         * Windows の WSAPoll は fds が空だと WSAEINVAL を返す。
         * 移植性のため明示的に分岐する (これも典型的な Windows の罠)。
         */
#ifdef _WIN32
        Sleep(timeout);
#else
        struct timespec ts;
        ts.tv_sec  = (time_t)(timeout / 1000u);
        ts.tv_nsec = (long)((timeout % 1000u) * 1000000u);
        nanosleep(&ts, NULL);
#endif
        rc = 0;
    }

    /* ---- 段 3: 結果を slirp に返す ---- */
    /*
     * select_error の意味に注意。libslirp のソースは
     *      if (select_error) { ... 全 fd をエラー扱い ... }
     * としているので、**タイムアウト (rc == 0) はエラーではない**。
     * ここで rc <= 0 を渡すと、待つたびに全 TCP セッションが
     * エラー扱いされて接続が全部切れる。
     */
    slirp_pollfds_poll(si->slirp, (rc < 0) ? 1 : 0, cb_get_revents, si);

    /* ---- タイマ処理 ---- */
    run_expired_timers(si);

    n->stats.timers_active = (uint64_t)si->n_timers;

    /* 次回までの推奨待ち時間 */
    tmr_ms = next_timer_ms(si);
    if (tmr_ms < 0)
        return (max_block_ms > 0) ? max_block_ms : 10;
    if (tmr_ms < 1)
        tmr_ms = 1;
    if (max_block_ms > 0 && tmr_ms > max_block_ms)
        tmr_ms = max_block_ms;

    return tmr_ms;
}

static const vm_nat_backend_ops_t slirp_ops = {
    "slirp",
    slirp_be_open,
    slirp_be_close,
    slirp_be_send_frame,
    slirp_be_poll,
    NULL,
    NULL
};

const vm_nat_backend_ops_t *vm_nat_ops_slirp(void)
{
    return &slirp_ops;
}

#endif /* VMODEM_HAVE_LIBSLIRP */
