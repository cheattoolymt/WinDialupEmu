/*
 * vm_nat.c - ユーザモード NAT 層 共通部
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 設計背景と難所 5/6/7 は include/vmodem/vm_nat.h 冒頭を参照。
 */
/*
 * clock_gettime / CLOCK_MONOTONIC は POSIX.1-2001 の機能なので、
 * glibc では機能テストマクロが必要。Windows ビルドでは無害。
 * #include より先に定義しなければ効かない。
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "vm_nat_internal.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

/* ==========================================================================
 * 単調増加クロック (難所 6)
 * ==========================================================================
 * libslirp の clock_get_ns はこれを直接使う。
 * 「単調増加」が絶対条件である理由:
 *   libslirp は TCP の再送タイマをこの値の差分で管理している。
 *   一瞬でも巻き戻ると、期限切れ判定が反転して再送が止まり、
 *   接続が無言でハングする。
 *
 * GetTickCount() は 32bit ms なので 49.7 日で 0 に戻る。使ってはいけない。
 * QueryPerformanceCounter は単調増加が保証され、分解能も十分。
 */
int64_t vm_nat_now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int           freq_ok = 0;
    LARGE_INTEGER        now;

    if (!freq_ok) {
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
            /* 最終手段。GetTickCount64 は 64bit なので巻き戻らない。 */
            return (int64_t)GetTickCount64() * 1000000LL;
        }
        freq_ok = 1;
    }
    QueryPerformanceCounter(&now);

    /*
     * (now * 1e9) / freq を素朴に書くと now が大きい時に桁溢れする。
     * 秒と余りに分けて計算する。
     */
    {
        int64_t sec  = (int64_t)(now.QuadPart / freq.QuadPart);
        int64_t rem  = (int64_t)(now.QuadPart % freq.QuadPart);
        return sec * 1000000000LL + (rem * 1000000000LL) / (int64_t)freq.QuadPart;
    }
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

/* ==========================================================================
 * バックエンド名
 * ========================================================================== */
static const char *const backend_names[VM_NAT__COUNT] = {
    "none", "loopback", "slirp"
};

const char *vm_nat_backend_name(vm_nat_backend_t b)
{
    if ((int)b < 0 || (int)b >= VM_NAT__COUNT)
        return "invalid";
    return backend_names[(int)b];
}

static int str_ieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

vm_nat_backend_t vm_nat_backend_from_string(const char *s)
{
    int i;

    if (s == NULL || *s == '\0')
        return VM_NAT_SLIRP;

    for (i = 0; i < VM_NAT__COUNT; i++) {
        if (str_ieq(s, backend_names[i]))
            return (vm_nat_backend_t)i;
    }

    /* 別名も受ける */
    if (str_ieq(s, "user") || str_ieq(s, "nat"))
        return VM_NAT_SLIRP;
    if (str_ieq(s, "off") || str_ieq(s, "disabled"))
        return VM_NAT_NONE;
    if (str_ieq(s, "test") || str_ieq(s, "echo"))
        return VM_NAT_LOOPBACK;

    return VM_NAT_SLIRP;
}

bool vm_nat_backend_available(vm_nat_backend_t b)
{
    switch (b) {
    case VM_NAT_NONE:
    case VM_NAT_LOOPBACK:
        return true;
    case VM_NAT_SLIRP:
        return vm_nat_ops_slirp() != NULL;
    default:
        return false;
    }
}

/* ==========================================================================
 * 既定設定
 * ==========================================================================
 * 192.168.99.0/24 を使う。理由:
 *   - RFC 1918 のプライベート範囲であること
 *   - 家庭用ルータの既定 (192.168.0.0/24, 192.168.1.0/24, 192.168.11.0/24)
 *     と衝突しにくい値を選ぶこと。衝突するとホスト側の経路と混ざり、
 *     「繋がるが一部のサイトだけ見えない」という切り分け困難な症状になる。
 */
void vm_nat_cfg_defaults(vm_nat_cfg_t *cfg)
{
    if (cfg == NULL)
        return;

    memset(cfg, 0, sizeof(*cfg));
    cfg->backend  = VM_NAT_SLIRP;
    cfg->network  = 0xC0A86300u;    /* 192.168.99.0   */
    cfg->netmask  = 0xFFFFFF00u;    /* 255.255.255.0  */
    cfg->host_ip  = 0xC0A86301u;    /* 192.168.99.1   */
    cfg->guest_ip = 0xC0A86302u;    /* 192.168.99.2   */
    cfg->dns_ip   = 0xC0A86303u;    /* 192.168.99.3   */
    cfg->mtu      = 1500;
    cfg->restricted = false;
    cfg->disable_host_loopback = false;
}

/* ==========================================================================
 * 生成 / 破棄
 * ========================================================================== */
vm_err_t vm_nat_create(vm_nat_t **out, const vm_nat_cfg_t *cfg,
                       vm_nat_ip_cb cb, void *user)
{
    vm_nat_t *n;
    vm_err_t  rc;

    if (out == NULL || cfg == NULL)
        return VM_ERR_INVAL;

    *out = NULL;

    n = (vm_nat_t *)calloc(1, sizeof(*n));
    if (n == NULL)
        return VM_ERR_NOMEM;

    n->cfg     = *cfg;
    n->ip_cb   = cb;
    n->ip_user = user;

    if (n->cfg.mtu <= 0 || n->cfg.mtu > 1500)
        n->cfg.mtu = 1500;

    /*
     * 偽イーサネット層。guest_mac は guest_ip から決定的に生成させる
     * (NULL を渡す)。
     */
    vm_eth_init(&n->eth, n->cfg.guest_ip, n->cfg.host_ip, NULL);

    n->backend = n->cfg.backend;

    switch (n->backend) {
    case VM_NAT_NONE:
        n->ops = NULL;
        VM_LOGI("nat: バックエンド none (IP パケットは破棄される)");
        *out = n;
        return VM_OK;

    case VM_NAT_LOOPBACK:
        n->ops = vm_nat_ops_loopback();
        break;

    case VM_NAT_SLIRP:
        n->ops = vm_nat_ops_slirp();
        if (n->ops == NULL) {
            /*
             * libslirp がリンクされていない。ここで黙って loopback に
             * 落とすと「なぜかインターネットに出られない」という
             * 分かりにくい状態になるので、必ず警告してエラーを返す。
             * フォールバックの判断は呼び出し側に委ねる。
             */
            VM_LOGE("nat: libslirp が組み込まれていません。"
                    "scripts/setup-libslirp.ps1 を実行して再ビルドしてください");
            free(n);
            return VM_ERR_UNSUPPORTED;
        }
        break;

    default:
        free(n);
        return VM_ERR_INVAL;
    }

    if (n->ops == NULL || n->ops->open == NULL) {
        free(n);
        return VM_ERR_UNSUPPORTED;
    }

    rc = n->ops->open(n);
    if (rc != VM_OK) {
        VM_LOGE("nat: バックエンド '%s' の初期化に失敗 (%s)",
                n->ops->name, vm_strerror(rc));
        free(n);
        return rc;
    }

    VM_LOGI("nat: バックエンド '%s' 起動 MTU=%d", n->ops->name, n->cfg.mtu);
    *out = n;
    return VM_OK;
}

void vm_nat_destroy(vm_nat_t *n)
{
    if (n == NULL)
        return;

    if (n->link_up)
        vm_nat_link_down(n);

    if (n->ops != NULL && n->ops->close != NULL)
        n->ops->close(n);

    free(n);
}

/* ==========================================================================
 * リンクアップ / ダウン
 * ========================================================================== */
vm_err_t vm_nat_link_up(vm_nat_t *n)
{
    int len;

    if (n == NULL)
        return VM_ERR_INVAL;

    if (n->link_up)
        return VM_OK;

    n->link_up = true;

    if (n->ops != NULL && n->ops->link_up != NULL)
        n->ops->link_up(n);

    /*
     * ★ vm_eth.h 難所 1 の対策 (a) ★
     * リンクアップ直後に ARP Request を 1 発打ち込む。
     * Sender = 我々 / Target = ゲートウェイ という通常形にすることで
     *   - libslirp の arp_input() が Request の送信元を arp_table_add() し
     *     guest_ip -> guest_mac が登録される
     *   - Target が相手自身なので ARP Reply が返り、我々も host_mac を得る
     * の双方が 1 パケットで成立する。
     * 以降 slirp からゲストへ向かうパケットは最初の 1 発から ARP 解決済みで、
     * 「最初の TCP SYN が消えて数秒固まる」現象が原理的に起こらなくなる。
     */
    if (n->ops != NULL && n->ops->send_frame != NULL) {
        len = vm_eth_build_startup_arp(&n->eth, n->fbuf, (int)sizeof(n->fbuf));
        if (len > 0)
            (void)n->ops->send_frame(n, n->fbuf, len);
    }

    VM_LOGI("nat: リンクアップ (backend=%s)", vm_nat_backend_name(n->backend));
    return VM_OK;
}

void vm_nat_link_down(vm_nat_t *n)
{
    if (n == NULL || !n->link_up)
        return;

    n->link_up = false;

    if (n->ops != NULL && n->ops->link_down != NULL)
        n->ops->link_down(n);

    /*
     * 学習した host_mac を忘れる。次のダイアルアップでは
     * 改めて Gratuitous ARP から始める。
     */
    n->eth.host_mac_known = false;

    VM_LOGI("nat: リンクダウン tx=%llu pkts / rx=%llu pkts",
            (unsigned long long)n->stats.tx_pkts,
            (unsigned long long)n->stats.rx_pkts);
}

/* ==========================================================================
 * ゲスト -> インターネット
 * ========================================================================== */
vm_err_t vm_nat_input_ip(vm_nat_t *n, const uint8_t *pkt, int len)
{
    int      flen;
    vm_err_t rc;

    if (n == NULL || pkt == NULL || len <= 0)
        return VM_ERR_INVAL;

    if (n->ops == NULL || n->ops->send_frame == NULL) {
        /* backend = none */
        n->stats.drops++;
        return VM_OK;
    }

    if (!n->link_up) {
        /*
         * IPCP が Opened になる前に IP が来ることは通常ないが、
         * 相手が先に喋る実装もありうる。暗黙にリンクアップさせる。
         */
        VM_LOGD("nat: リンクアップ前の IP を受信 -> 暗黙にリンクアップ");
        (void)vm_nat_link_up(n);
    }

    /* 生 IP に偽 Ethernet ヘッダを付ける (vm_eth.h の全体設計) */
    flen = vm_eth_encap(&n->eth, pkt, len, n->fbuf, (int)sizeof(n->fbuf));
    if (flen < 0) {
        n->stats.drops++;
        return (vm_err_t)flen;
    }

    if (vm_log_get_level() >= VM_LOG_TRACE) {
        char d[96];
        VM_LOGT("nat tx: %s", vm_ip4_describe(pkt, len, d, sizeof(d)));
    }

    rc = n->ops->send_frame(n, n->fbuf, flen);
    if (rc != VM_OK) {
        n->stats.drops++;
        return rc;
    }

    n->stats.tx_pkts++;
    n->stats.tx_bytes += (uint64_t)len;
    return VM_OK;
}

/* ==========================================================================
 * インターネット -> ゲスト
 * ==========================================================================
 * バックエンドはここを呼ぶだけでよい。ARP の面倒は共通部が見る。
 */
void vm_nat_backend_recv_frame(vm_nat_t *n, const uint8_t *frame, int len)
{
    vm_eth_result_t r;
    int             out_len = 0;

    if (n == NULL || frame == NULL || len <= 0)
        return;

    r = vm_eth_decap(&n->eth, frame, len,
                     n->pbuf, (int)sizeof(n->pbuf), &out_len);

    switch (r) {
    case VM_ETH_IP:
        if (out_len > n->cfg.mtu) {
            /*
             * MTU 超過。PPP の MRU を超えるフレームは相手が捨てるので
             * ここで落とす。本来は ICMP Fragmentation Needed を返すべきだが、
             * libslirp 側が MTU を守るよう設定しているので通常発生しない。
             */
            VM_LOGW("nat: MTU 超過パケットを破棄 (%d > %d)",
                    out_len, n->cfg.mtu);
            n->stats.drops++;
            return;
        }
        if (vm_log_get_level() >= VM_LOG_TRACE) {
            char d[96];
            VM_LOGT("nat rx: %s",
                    vm_ip4_describe(n->pbuf, out_len, d, sizeof(d)));
        }
        n->stats.rx_pkts++;
        n->stats.rx_bytes += (uint64_t)out_len;
        if (n->ip_cb != NULL)
            n->ip_cb(n->ip_user, n->pbuf, out_len);
        break;

    case VM_ETH_REPLY:
        /*
         * ARP Reply。これを返さないと slirp からのパケットが
         * 永久に届かない (vm_eth.h 難所 1)。
         */
        if (n->ops != NULL && n->ops->send_frame != NULL)
            (void)n->ops->send_frame(n, n->pbuf, out_len);
        break;

    case VM_ETH_CONSUMED:
        /* ARP Reply の学習など。正常処理なので drops に数えない。 */
        break;

    case VM_ETH_DROP:
    default:
        n->stats.drops++;
        break;
    }
}

void vm_nat_backend_guest_error(vm_nat_t *n, const char *msg)
{
    if (n == NULL)
        return;
    n->stats.guest_errors++;
    VM_LOGW("nat: ゲストの不正パケット: %s", msg != NULL ? msg : "(不明)");
}

/* ==========================================================================
 * ポーリング (難所 7)
 * ========================================================================== */
int vm_nat_poll(vm_nat_t *n, int max_block_ms)
{
    if (n == NULL)
        return 100;

    n->stats.polls++;

    if (n->ops == NULL || n->ops->poll == NULL)
        return max_block_ms > 0 ? max_block_ms : 10;

    return n->ops->poll(n, max_block_ms);
}

/* ==========================================================================
 * 統計 / 状態
 * ========================================================================== */
void vm_nat_get_stats(const vm_nat_t *n, vm_nat_stats_t *st)
{
    if (st == NULL)
        return;
    if (n == NULL) {
        memset(st, 0, sizeof(*st));
        return;
    }
    *st = n->stats;
}

vm_nat_backend_t vm_nat_active_backend(const vm_nat_t *n)
{
    return (n != NULL) ? n->backend : VM_NAT_NONE;
}

uint32_t vm_nat_dns_ip(const vm_nat_t *n)
{
    /*
     * ★なぜこのアクセサが必要なのか★
     *
     * PPP の IPCP はクライアント (Windows RAS) に DNS サーバのアドレスを
     * 配る。従来はここに config.ini の dns1 = 8.8.8.8 をそのまま流していた。
     *
     * ところが libslirp の DNS 代理は、宛先が **vnameserver と完全一致**
     * した時だけ働く (src/socket.c sotranslate_out4):
     *
     *     if (!s->disable_dns &&
     *         so->so_faddr.s_addr == s->vnameserver_addr.s_addr) {
     *         return (so->so_fport == htons(53) &&
     *                 get_dns_addr(&sin->sin_addr, &sin->sin_port) >= 0);
     *     }
     *
     * つまり 192.168.99.3:53 宛だけがホストの実 DNS
     * (Windows では GetNetworkParams() で得た値) へ差し替えられる。
     *
     * 8.8.8.8 を配ると DNS クエリは「ただの外部 UDP」として NAT される。
     * それでも本来は通るはずだが、次の理由で不利になる:
     *
     *   1. 企業/家庭のルータやプロバイダが外部 DNS (53/udp) を
     *      ブロックまたは透過リダイレクトしている環境が珍しくない。
     *   2. libslirp の DNS ソケットは SO_EXPIREFAST (10 秒) で回収される。
     *      応答が遅い外部 DNS だと取りこぼしが増える。
     *   3. ホストが VPN や社内 DNS を使っている場合、内部名が引けない。
     *
     * ゲストに vnameserver を教えれば、名前解決は必ずホストと同じ
     * 経路・同じ結果になる。これが slirp を使う時の正しい作法。
     */
    return (n != NULL) ? n->cfg.dns_ip : 0u;
}

const char *vm_nat_status(const vm_nat_t *n, char *buf, size_t size)
{
    if (buf == NULL || size == 0)
        return "";

    if (n == NULL) {
        snprintf(buf, size, "nat: (none)");
        return buf;
    }

    snprintf(buf, size,
             "nat[%s] %s tx=%llu/%lluB rx=%llu/%lluB drop=%llu "
             "arp(req_rx=%llu rep_tx=%llu) pad=%llu",
             vm_nat_backend_name(n->backend),
             n->link_up ? "UP" : "DOWN",
             (unsigned long long)n->stats.tx_pkts,
             (unsigned long long)n->stats.tx_bytes,
             (unsigned long long)n->stats.rx_pkts,
             (unsigned long long)n->stats.rx_bytes,
             (unsigned long long)n->stats.drops,
             (unsigned long long)n->eth.stats.arp_req_rx,
             (unsigned long long)n->eth.stats.arp_rep_tx,
             (unsigned long long)n->eth.stats.pad_stripped);

    return buf;
}
