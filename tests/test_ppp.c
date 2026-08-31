/*
 * test_ppp.c - HDLC フレーミングと PPP サーバの検証
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * このテストの狙い
 * ===========================================================================
 * PPP は「相手がいないと何も起きない」プロトコルなので、
 * 単体テストだけでは実装の正しさが全く証明できない。
 * そこで本テストは **Windows RAS を模擬するピア** を実装し、
 * vm_ppp.h の「難所 C」で列挙した RAS 固有の振る舞いを全て再現する:
 *
 *   C-1  LCP CR に Callback オプション (0x0D) を載せてくる
 *   C-2  CCP (0x80FD) / IPV6CP (0x8057) / CBCP (0xC029) を投げてくる
 *   C-3  IPCP CR で VJ 圧縮 (option 2) を要求してくる
 *   C-4  IPCP CR で自分の IP を 0.0.0.0 にしてくる
 *   C-5  IPCP CR で DNS(129/131) と NBNS(130/132) を要求してくる
 *   C-6  認証なし構成では Auth-Protocol を送らない
 *   C-7  Magic-Number を載せてくる
 *
 * そして「RAS が IPCP まで上がって IP パケットが双方向に流れる」事を
 * 実際にバイト列レベルで確認する。
 * ===========================================================================
 */
#include "vmodem/vm_ppp.h"
#include "vmodem/vm_hdlc.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;

static void check(const char *name, int ok, const char *detail)
{
    if (ok) { g_pass++; printf("  [ OK ] %-58s %s\n", name, detail ? detail : ""); }
    else    { g_fail++; printf("  [FAIL] %-58s %s\n", name, detail ? detail : ""); }
}

static void group(const char *s) { printf("\n%s\n", s); }

/* ===========================================================================
 * 模擬 RAS ピア
 * ===========================================================================
 * 我々の PPP サーバと「対戦」する相手。
 * vm_ppp とは完全に独立に、素朴だが仕様通りに書く。
 * (同じコードを使い回すと相互のバグが打ち消し合って検出できない)
 */

/*
 * ★★★ 設計上の最重要事項: 再入させない ★★★
 *
 * このハーネスを最初に書いた時、ras_write_cb() の中から直接
 * vm_ppp_input() を呼んでいた。結果は **スタックオーバフローでコア
 * ダンプ** だった。ASan のトレースはこう出た:
 *
 *   ras_send_frame -> vm_ppp_input -> vm_hdlc_input -> deliver_frame
 *     -> handle_lcp -> lcp_handle_conf_req -> send_pkt
 *       -> ras_write_cb -> ras_on_frame -> ras_handle_lcp
 *         -> ras_send_lcp_req_no_callback -> ras_send_frame -> (無限)
 *
 * 原因は 2 つ重なっていた:
 *
 *  (1) 相互再帰そのもの。A が B を呼び、B が同期的に A を呼ぶ。
 *      交渉が 1 往復するごとにスタックが深くなり、必ず溢れる。
 *
 *  (2) さらに悪いのが **デフレーマ状態の破壊**。
 *      ras_write_cb はバイトを 1 つずつ走査しながら fbuf/flen/escaped を
 *      更新する状態機械。そのループの途中で vm_ppp_input() を呼ぶと、
 *      ネストした呼び出しがまた ras_write_cb に入って flen = 0 を書き潰す。
 *      外側のループに戻った時にはフレームが壊れており、壊れた
 *      オプション列に対して我々の PPP が Conf-Rej を返し、
 *      RAS がそれを見て再送し、また壊れ… という発散ループになる。
 *
 * ★ なぜ実機では起きないのか ★
 *   実機の write_cb は vm_serial_write() を呼ぶだけ。バイトは COM ポート
 *   (com0com のリングバッファ) に置かれ、呼び出しはすぐ戻る。
 *   RAS がそれを読むのは *別プロセスの別スレッド* で、我々のスタックとは
 *   無関係。つまり実機は本質的に非同期であり、同期再帰は起こらない。
 *
 * ★ したがってハーネスも非同期にする ★
 *   write_cb は「送信キューに積むだけ」にして、フレームの処理は
 *   トップレベルの pump() が行う。これで:
 *     - スタックは常に浅い (深さ 2 で固定)
 *     - デフレーマ状態を再入で壊さない
 *     - 実機と同じ「書いて戻る、後で相手が読む」意味論になる
 *
 * この経験自体が PPP 実装の教訓なので、恒久的に文書として残す。
 */

#define QCAP 262144   /* 双方向キューの容量 */

typedef struct {
    /* --- 我々の PPP → RAS の生バイトキュー (write_cb が積む) --- */
    uint8_t  q[QCAP];
    int      q_len;
    int      q_pos;
    uint64_t q_total;      /* 累計投入バイト数 (アサーション用) */
    bool     q_overflow;

    /* --- RAS → 我々の PPP の生バイトキュー (pump が流す) --- */
    uint8_t  tq[QCAP];
    int      tq_len;
    int      tq_pos;

    /* --- RAS 側の HDLC 受信状態 (自前で素朴に実装) --- */
    uint8_t  fbuf[4096];
    int      flen;
    bool     in_frame, escaped;

    /* --- RAS 側の交渉状態 --- */
    uint8_t  next_id;
    uint8_t  our_req_id;
    bool     lcp_ack_sent, lcp_ack_rcvd, lcp_open;
    bool     ipcp_ack_sent, ipcp_ack_rcvd, ipcp_open;
    uint32_t magic;
    uint32_t my_ip, dns1, dns2;

    /* --- 観測記録 (アサーション用) --- */
    int      n_callback_rej;   /* Callback を Reject された回数     */
    int      n_ccp_protorej;   /* CCP に Protocol-Reject が来た     */
    int      n_ipv6cp_protorej;
    int      n_cbcp_protorej;
    int      n_vj_rej;         /* VJ を Reject された               */
    int      n_nbns_rej;       /* NBNS を Reject された             */
    int      n_ip_nak;         /* IP アドレスを Nak された          */
    int      n_dns_nak;        /* DNS を Nak された                 */
    int      n_auth_opt_seen;  /* 我々の CR に Auth が載っていたか  */
    int      n_echo_req;       /* Echo-Request が来た               */
    int      n_echo_rep;       /* Echo-Reply が来た                 */
    int      n_term_req;
    int      n_code_rej;

    /* 受け取った IP パケット */
    uint8_t  last_ip[2048];
    int      last_ip_len;
    int      n_ip_rx;

    /* 送信先 (我々の PPP) */
    vm_ppp_t *ppp;
    uint32_t  now;

    /* RAS が送るときのエスケープマップ (我々が CR で指定した値) */
    uint32_t tx_accm;

    bool     verbose;
} ras_t;

/* --- RAS 側 HDLC 送信 --------------------------------------------------- */

static void ras_send_frame(ras_t *r, uint16_t proto,
                           const uint8_t *payload, int len)
{
    uint8_t raw[4096], out[8192];
    int n = 0, i, pos = 0;
    uint16_t fcs = VM_HDLC_FCS_INIT;

    raw[n++] = 0xFF; raw[n++] = 0x03;
    raw[n++] = (uint8_t)(proto >> 8); raw[n++] = (uint8_t)(proto & 0xFF);
    memcpy(raw + n, payload, (size_t)len); n += len;

    fcs = vm_hdlc_fcs16(fcs, raw, n);
    fcs = (uint16_t)(~fcs);
    raw[n++] = (uint8_t)(fcs & 0xFF);
    raw[n++] = (uint8_t)(fcs >> 8);

    out[pos++] = 0x7E;
    for (i = 0; i < n; i++) {
        if (vm_hdlc_needs_escape(r->tx_accm, raw[i])) {
            out[pos++] = 0x7D;
            out[pos++] = (uint8_t)(raw[i] ^ 0x20);
        } else {
            out[pos++] = raw[i];
        }
    }
    out[pos++] = 0x7E;

    /*
     * ★ ここで vm_ppp_input() を呼んではいけない ★
     * (構造体宣言の上にある長いコメントを参照)
     * RAS → 我々方向のキューに積むだけにして、実際の投入は pump() が行う。
     */
    if (r->tq_len + pos <= (int)sizeof(r->tq)) {
        memcpy(r->tq + r->tq_len, out, (size_t)pos);
        r->tq_len += pos;
    } else {
        r->q_overflow = true;
    }
}

static void ras_send_ctrl(ras_t *r, uint16_t proto, uint8_t code, uint8_t id,
                          const uint8_t *data, int dlen)
{
    uint8_t p[2048];
    p[0] = code; p[1] = id;
    p[2] = (uint8_t)((4 + dlen) >> 8); p[3] = (uint8_t)((4 + dlen) & 0xFF);
    if (dlen > 0) memcpy(p + 4, data, (size_t)dlen);
    ras_send_frame(r, proto, p, 4 + dlen);
}

/* --- RAS 側: 我々の Configure-Request への応答 -------------------------- */

static void ras_on_lcp_conf_req(ras_t *r, uint8_t id,
                                const uint8_t *d, int len)
{
    int i = 0;
    /*
     * RAS は我々の CR を素直に全部 Ack する。
     * ただし Auth-Protocol が載っていたかどうかは記録する (C-6 の検証)。
     */
    while (i + 2 <= len) {
        int l = d[i + 1];
        if (l < 2 || i + l > len) break;
        if (d[i] == VM_LCP_OPT_AUTH) r->n_auth_opt_seen++;
        i += l;
    }
    ras_send_ctrl(r, VM_PPP_PROTO_LCP, VM_PPP_CONF_ACK, id, d, len);
    r->lcp_ack_sent = true;
    if (r->lcp_ack_rcvd) r->lcp_open = true;
}

static void ras_on_ipcp_conf_req(ras_t *r, uint8_t id,
                                 const uint8_t *d, int len)
{
    /* 我々 (サーバ) の IP 要求は素直に Ack */
    ras_send_ctrl(r, VM_PPP_PROTO_IPCP, VM_PPP_CONF_ACK, id, d, len);
    r->ipcp_ack_sent = true;
    if (r->ipcp_ack_rcvd) r->ipcp_open = true;
}

/*
 * ★ C-1 + C-7 を含む RAS の LCP Configure-Request ★
 * 実際の Windows RAS が送るオプション構成を再現する。
 */
static void ras_send_lcp_req(ras_t *r)
{
    uint8_t o[64];
    int n = 0;

    /* MRU 1500 */
    o[n++] = VM_LCP_OPT_MRU; o[n++] = 4; o[n++] = 0x05; o[n++] = 0xDC;

    /* ACCM 0x000A0000 : RAS は CR/LF だけエスケープを要求する事がある */
    o[n++] = VM_LCP_OPT_ACCM; o[n++] = 6;
    o[n++] = 0x00; o[n++] = 0x0A; o[n++] = 0x00; o[n++] = 0x00;

    /* Magic-Number (C-7) */
    o[n++] = VM_LCP_OPT_MAGIC; o[n++] = 6;
    o[n++] = (uint8_t)(r->magic >> 24); o[n++] = (uint8_t)(r->magic >> 16);
    o[n++] = (uint8_t)(r->magic >> 8);  o[n++] = (uint8_t)(r->magic);

    /* PFC / ACFC : RAS は圧縮を要求してくる */
    o[n++] = VM_LCP_OPT_PFC;  o[n++] = 2;
    o[n++] = VM_LCP_OPT_ACFC; o[n++] = 2;

    /* ★ C-1: Callback オプション (MS 拡張) → Reject されるべき ★ */
    o[n++] = VM_LCP_OPT_CALLBACK; o[n++] = 3; o[n++] = 0x06;

    r->our_req_id = r->next_id++;
    ras_send_ctrl(r, VM_PPP_PROTO_LCP, VM_PPP_CONF_REQ, r->our_req_id, o, n);
}

/* Callback を抜いた再送 (Reject を受けた後) */
static void ras_send_lcp_req_no_callback(ras_t *r)
{
    uint8_t o[64];
    int n = 0;
    o[n++] = VM_LCP_OPT_MRU; o[n++] = 4; o[n++] = 0x05; o[n++] = 0xDC;
    o[n++] = VM_LCP_OPT_ACCM; o[n++] = 6;
    o[n++] = 0x00; o[n++] = 0x0A; o[n++] = 0x00; o[n++] = 0x00;
    o[n++] = VM_LCP_OPT_MAGIC; o[n++] = 6;
    o[n++] = (uint8_t)(r->magic >> 24); o[n++] = (uint8_t)(r->magic >> 16);
    o[n++] = (uint8_t)(r->magic >> 8);  o[n++] = (uint8_t)(r->magic);
    o[n++] = VM_LCP_OPT_PFC;  o[n++] = 2;
    o[n++] = VM_LCP_OPT_ACFC; o[n++] = 2;
    r->our_req_id = r->next_id++;
    ras_send_ctrl(r, VM_PPP_PROTO_LCP, VM_PPP_CONF_REQ, r->our_req_id, o, n);
}

/*
 * ★ C-3 + C-4 + C-5 を含む RAS の IPCP Configure-Request ★
 */
static void ras_send_ipcp_req(ras_t *r)
{
    uint8_t o[64];
    int n = 0;

    /* C-3: VJ 圧縮 → Reject されるべき */
    o[n++] = VM_IPCP_OPT_VJ; o[n++] = 6;
    o[n++] = 0x00; o[n++] = 0x2D; o[n++] = 0x0F; o[n++] = 0x01;

    /* C-4: 自分の IP を 0.0.0.0 → Nak で割り当てを貰う */
    o[n++] = VM_IPCP_OPT_ADDR; o[n++] = 6;
    o[n++] = (uint8_t)(r->my_ip >> 24); o[n++] = (uint8_t)(r->my_ip >> 16);
    o[n++] = (uint8_t)(r->my_ip >> 8);  o[n++] = (uint8_t)(r->my_ip);

    /* C-5: DNS → Nak で実値を貰う */
    o[n++] = VM_IPCP_OPT_DNS1; o[n++] = 6;
    o[n++] = 0; o[n++] = 0; o[n++] = 0; o[n++] = 0;
    /* C-5: NBNS → Reject されるべき */
    o[n++] = VM_IPCP_OPT_NBNS1; o[n++] = 6;
    o[n++] = 0; o[n++] = 0; o[n++] = 0; o[n++] = 0;
    o[n++] = VM_IPCP_OPT_DNS2; o[n++] = 6;
    o[n++] = 0; o[n++] = 0; o[n++] = 0; o[n++] = 0;
    o[n++] = VM_IPCP_OPT_NBNS2; o[n++] = 6;
    o[n++] = 0; o[n++] = 0; o[n++] = 0; o[n++] = 0;

    r->our_req_id = r->next_id++;
    ras_send_ctrl(r, VM_PPP_PROTO_IPCP, VM_PPP_CONF_REQ, r->our_req_id, o, n);
}

/* Reject/Nak を反映した IPCP 再送 */
static void ras_send_ipcp_req_fixed(ras_t *r)
{
    uint8_t o[64];
    int n = 0;
    o[n++] = VM_IPCP_OPT_ADDR; o[n++] = 6;
    o[n++] = (uint8_t)(r->my_ip >> 24); o[n++] = (uint8_t)(r->my_ip >> 16);
    o[n++] = (uint8_t)(r->my_ip >> 8);  o[n++] = (uint8_t)(r->my_ip);
    o[n++] = VM_IPCP_OPT_DNS1; o[n++] = 6;
    o[n++] = (uint8_t)(r->dns1 >> 24); o[n++] = (uint8_t)(r->dns1 >> 16);
    o[n++] = (uint8_t)(r->dns1 >> 8);  o[n++] = (uint8_t)(r->dns1);
    o[n++] = VM_IPCP_OPT_DNS2; o[n++] = 6;
    o[n++] = (uint8_t)(r->dns2 >> 24); o[n++] = (uint8_t)(r->dns2 >> 16);
    o[n++] = (uint8_t)(r->dns2 >> 8);  o[n++] = (uint8_t)(r->dns2);
    r->our_req_id = r->next_id++;
    ras_send_ctrl(r, VM_PPP_PROTO_IPCP, VM_PPP_CONF_REQ, r->our_req_id, o, n);
}

/* --- RAS 側: 受信したフレームの処理 ------------------------------------ */

static void ras_handle_lcp(ras_t *r, const uint8_t *p, int len)
{
    uint8_t code = p[0], id = p[1];
    int plen = (p[2] << 8) | p[3];
    const uint8_t *d = p + 4;
    int dlen = plen - 4;

    if (plen < 4 || plen > len) return;

    switch (code) {
    case VM_PPP_CONF_REQ:
        ras_on_lcp_conf_req(r, id, d, dlen);
        break;
    case VM_PPP_CONF_ACK:
        if (id == r->our_req_id) {
            r->lcp_ack_rcvd = true;
            if (r->lcp_ack_sent) r->lcp_open = true;
        }
        break;
    case VM_PPP_CONF_REJ: {
        /* Reject の中身を調べて、何が拒否されたか記録 */
        int i = 0;
        while (i + 2 <= dlen) {
            int l = d[i + 1];
            if (l < 2 || i + l > dlen) break;
            if (d[i] == VM_LCP_OPT_CALLBACK) r->n_callback_rej++;
            i += l;
        }
        /* C-1 の正しい振る舞い: Callback を外して再送 */
        ras_send_lcp_req_no_callback(r);
        break;
    }
    case VM_PPP_CONF_NAK:
        ras_send_lcp_req_no_callback(r);
        break;
    case VM_PPP_PROTO_REJ: {
        uint16_t rej = (uint16_t)((d[0] << 8) | d[1]);
        if (rej == VM_PPP_PROTO_CCP)    r->n_ccp_protorej++;
        if (rej == VM_PPP_PROTO_IPV6CP) r->n_ipv6cp_protorej++;
        if (rej == VM_PPP_PROTO_CBCP)   r->n_cbcp_protorej++;
        break;
    }
    case VM_PPP_ECHO_REQ: {
        /* RAS は Echo-Reply を返す */
        uint8_t o[8];
        r->n_echo_req++;
        o[0] = (uint8_t)(r->magic >> 24); o[1] = (uint8_t)(r->magic >> 16);
        o[2] = (uint8_t)(r->magic >> 8);  o[3] = (uint8_t)(r->magic);
        ras_send_ctrl(r, VM_PPP_PROTO_LCP, VM_PPP_ECHO_REP, id, o, 4);
        break;
    }
    case VM_PPP_ECHO_REP:
        r->n_echo_rep++;
        break;
    case VM_PPP_TERM_REQ:
        r->n_term_req++;
        ras_send_ctrl(r, VM_PPP_PROTO_LCP, VM_PPP_TERM_ACK, id, NULL, 0);
        break;
    case VM_PPP_CODE_REJ:
        r->n_code_rej++;
        break;
    default:
        break;
    }
}

static void ras_handle_ipcp(ras_t *r, const uint8_t *p, int len)
{
    uint8_t code = p[0], id = p[1];
    int plen = (p[2] << 8) | p[3];
    const uint8_t *d = p + 4;
    int dlen = plen - 4;
    int i;

    if (plen < 4 || plen > len) return;

    switch (code) {
    case VM_PPP_CONF_REQ:
        ras_on_ipcp_conf_req(r, id, d, dlen);
        break;
    case VM_PPP_CONF_ACK:
        if (id == r->our_req_id) {
            r->ipcp_ack_rcvd = true;
            if (r->ipcp_ack_sent) r->ipcp_open = true;
        }
        break;
    case VM_PPP_CONF_REJ:
        i = 0;
        while (i + 2 <= dlen) {
            int l = d[i + 1];
            if (l < 2 || i + l > dlen) break;
            if (d[i] == VM_IPCP_OPT_VJ)     r->n_vj_rej++;
            if (d[i] == VM_IPCP_OPT_NBNS1 ||
                d[i] == VM_IPCP_OPT_NBNS2)  r->n_nbns_rej++;
            i += l;
        }
        ras_send_ipcp_req_fixed(r);
        break;
    case VM_PPP_CONF_NAK:
        /* Nak で提示された値を取り込む (C-4 / C-5 の正しい振る舞い) */
        i = 0;
        while (i + 2 <= dlen) {
            int l = d[i + 1];
            if (l < 2 || i + l > dlen) break;
            if (d[i] == VM_IPCP_OPT_ADDR && l == 6) {
                r->my_ip = ((uint32_t)d[i+2] << 24) | ((uint32_t)d[i+3] << 16) |
                           ((uint32_t)d[i+4] << 8)  |  (uint32_t)d[i+5];
                r->n_ip_nak++;
            }
            if (d[i] == VM_IPCP_OPT_DNS1 && l == 6) {
                r->dns1 = ((uint32_t)d[i+2] << 24) | ((uint32_t)d[i+3] << 16) |
                          ((uint32_t)d[i+4] << 8)  |  (uint32_t)d[i+5];
                r->n_dns_nak++;
            }
            if (d[i] == VM_IPCP_OPT_DNS2 && l == 6) {
                r->dns2 = ((uint32_t)d[i+2] << 24) | ((uint32_t)d[i+3] << 16) |
                          ((uint32_t)d[i+4] << 8)  |  (uint32_t)d[i+5];
                r->n_dns_nak++;
            }
            i += l;
        }
        ras_send_ipcp_req_fixed(r);
        break;
    default:
        break;
    }
}

static void ras_on_frame(ras_t *r, uint16_t proto, const uint8_t *p, int len)
{
    if (r->verbose)
        printf("      RAS<-: proto=%04X len=%d code=%u\n",
               (unsigned)proto, len, len > 0 ? p[0] : 0);

    switch (proto) {
    case VM_PPP_PROTO_LCP:  ras_handle_lcp(r, p, len);  break;
    case VM_PPP_PROTO_IPCP: ras_handle_ipcp(r, p, len); break;
    case VM_PPP_PROTO_IP:
        r->n_ip_rx++;
        r->last_ip_len = len < (int)sizeof(r->last_ip) ? len
                                                       : (int)sizeof(r->last_ip);
        memcpy(r->last_ip, p, (size_t)r->last_ip_len);
        break;
    default: break;
    }
}

/*
 * 我々の PPP サーバの write_cb。
 *
 * ★ ここでは「積むだけ」で、絶対にフレーム処理をしない ★
 * 実機の vm_serial_write() と同じ意味論 (バイトを置いて即座に戻る)。
 * デフレームは ras_consume() が、pump() から呼ばれて行う。
 */
static int ras_write_cb(void *user, const uint8_t *data, int len)
{
    ras_t *r = (ras_t *)user;

    if (r->q_len + len <= (int)sizeof(r->q)) {
        memcpy(r->q + r->q_len, data, (size_t)len);
        r->q_len += len;
    } else {
        r->q_overflow = true;
    }
    r->q_total += (uint64_t)len;
    return len;
}

/*
 * キューに溜まった「我々 → RAS」のバイトを RAS 側でデフレームする。
 * ここは vm_hdlc.c を *使わずに* 素朴に書く (相互検証のため)。
 *
 * この関数の中から ras_send_frame() が呼ばれる事はあるが、
 * それは RAS→我々キュー (tq) に積むだけなので再入しない。
 */
static void ras_consume(ras_t *r)
{
    while (r->q_pos < r->q_len) {
        uint8_t c = r->q[r->q_pos++];

        if (c == 0x7E) {
            if (!r->escaped && r->flen >= 6) {
                uint16_t fcs = vm_hdlc_fcs16(VM_HDLC_FCS_INIT, r->fbuf, r->flen);
                if (fcs == VM_HDLC_FCS_GOOD) {
                    const uint8_t *p = r->fbuf;
                    int n = r->flen - 2;
                    uint16_t proto;
                    if (n >= 2 && p[0] == 0xFF && p[1] == 0x03) { p += 2; n -= 2; }
                    if (n >= 1 && (p[0] & 1)) { proto = p[0]; p++; n--; }
                    else if (n >= 2) { proto = (uint16_t)((p[0] << 8) | p[1]); p += 2; n -= 2; }
                    else { proto = 0; }
                    if (proto) ras_on_frame(r, proto, p, n);
                }
            }
            r->flen = 0; r->escaped = false; r->in_frame = true;
            continue;
        }
        if (!r->in_frame) continue;
        if (r->escaped) { r->escaped = false; c ^= 0x20; }
        else if (c == 0x7D) { r->escaped = true; continue; }
        if (r->flen < (int)sizeof(r->fbuf)) r->fbuf[r->flen++] = c;
    }

    /* 消費済み領域を回収 (長時間の転送テストで溢れないように) */
    if (r->q_pos > 0 && r->q_pos == r->q_len) {
        r->q_pos = 0;
        r->q_len = 0;
    }
}

/*
 * ★ 交渉ポンプ ★
 *
 * 両方向のキューが空になるまで交互に流す。これが「実機で 2 つの
 * プロセスが非同期にバイトを読み書きする」事の決定論的な等価物。
 *
 *   1. RAS→我々キュー (tq) を vm_ppp_input() に流す
 *      → 我々の PPP が応答を q に積む (write_cb 経由)
 *   2. 我々→RASキュー (q) を ras_consume() に流す
 *      → RAS が応答を tq に積む (ras_send_frame 経由)
 *   3. 両方空になるまで繰り返す
 *
 * スタックの深さは常に一定。交渉が何往復しても溢れない。
 * max_rounds は「交渉が収束しないバグ」を検出するための保険で、
 * 正常な LCP+IPCP は 6 ラウンド程度で収束する。
 */
static int pump(ras_t *r, uint32_t now)
{
    int rounds = 0;
    const int MAX_ROUNDS = 64;

    r->now = now;

    while (rounds < MAX_ROUNDS) {
        bool did = false;

        if (r->tq_pos < r->tq_len) {
            int n = r->tq_len - r->tq_pos;
            /*
             * バッファを一旦コピーしてから投入する。
             * vm_ppp_input() の中で ras_write_cb → (何もしない) となるので
             * tq が書き換わる事は無いが、明示的に切り離しておく方が安全。
             */
            static uint8_t tmp[QCAP];
            memcpy(tmp, r->tq + r->tq_pos, (size_t)n);
            r->tq_pos = 0;
            r->tq_len = 0;
            vm_ppp_input(r->ppp, tmp, n, now);
            did = true;
        }

        if (r->q_pos < r->q_len) {
            ras_consume(r);
            did = true;
        }

        if (!did) break;
        rounds++;
    }
    return rounds;
}

/* 我々の PPP が上位 (NAT) へ渡した IP パケットの受け皿 */
typedef struct {
    uint8_t pkt[2048];
    int     len;
    int     count;
} ipsink_t;

static void ip_sink_cb(void *user, const uint8_t *pkt, int len)
{
    ipsink_t *s = (ipsink_t *)user;
    s->count++;
    s->len = len < (int)sizeof(s->pkt) ? len : (int)sizeof(s->pkt);
    memcpy(s->pkt, pkt, (size_t)s->len);
}

static void ras_init(ras_t *r, vm_ppp_t *ppp)
{
    memset(r, 0, sizeof(*r));
    r->ppp     = ppp;
    r->next_id = 1;
    r->magic   = 0xDEADBEEFu;
    r->my_ip   = 0;                /* C-4: 0.0.0.0 */
    r->tx_accm = 0xFFFFFFFFu;      /* 交渉前は全エスケープ */
    r->in_frame = false;
}

/* ===========================================================================
 * [1] HDLC フレーミング単体
 * =========================================================================== */

typedef struct {
    uint16_t proto;
    uint8_t  payload[2048];
    int      len;
    int      count;
} hdlc_sink_t;

static void hdlc_sink_cb(void *user, uint16_t proto,
                         const uint8_t *payload, int len)
{
    hdlc_sink_t *s = (hdlc_sink_t *)user;
    s->count++;
    s->proto = proto;
    s->len = len < (int)sizeof(s->payload) ? len : (int)sizeof(s->payload);
    if (s->len > 0) memcpy(s->payload, payload, (size_t)s->len);
}

static void test_hdlc(void)
{
    char detail[256];

    group("[1] RFC 1662 HDLC フレーミング");

    /* --- FCS の既知値 --- */
    {
        /*
         * RFC 1662 の FCS は CRC-16/X-25。
         * 有名な検査ベクタ "123456789" に対する X-25 の CRC は 0x906E。
         * ただし X-25 の定義は「初期値 0xFFFF、最後に反転」なので、
         * fcs16(0xFFFF,...) の生の結果を反転した値と比べる。
         */
        uint16_t f = vm_hdlc_fcs16(VM_HDLC_FCS_INIT,
                                   (const uint8_t *)"123456789", 9);
        uint16_t x25 = (uint16_t)(~f);
        snprintf(detail, sizeof(detail),
                 "CRC-16/X-25(\"123456789\") = 0x%04X (期待 0x906E)",
                 (unsigned)x25);
        check("FCS-16 は CRC-16/X-25 と一致する", x25 == 0x906E, detail);
    }

    /* --- エンコード → デコード 往復 --- */
    {
        vm_hdlc_t tx, rx;
        hdlc_sink_t sink;
        uint8_t buf[VM_HDLC_TXBUF_SIZE];
        const uint8_t pay[] = { 0x01, 0x02, 0x00, 0x08, 0xAA, 0xBB };
        int n;

        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&tx, NULL, NULL);
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);

        n = vm_hdlc_encode(&tx, VM_PPP_PROTO_LCP, pay, 6,
                           buf, (int)sizeof(buf));
        snprintf(detail, sizeof(detail),
                 "encode -> %d bytes (7E FF 03 C0 21 ... FCS 7E)", n);
        check("LCP フレームをエンコードできる", n > 0, detail);

        vm_hdlc_input(&rx, buf, n);
        snprintf(detail, sizeof(detail),
                 "frames=%d proto=%04X len=%d", sink.count,
                 (unsigned)sink.proto, sink.len);
        check("同じフレームをデコードして元に戻る",
              sink.count == 1 && sink.proto == VM_PPP_PROTO_LCP &&
              sink.len == 6 && memcmp(sink.payload, pay, 6) == 0, detail);
    }

    /* --- バイトスタッフィング: 0x7E / 0x7D / 制御文字 --- */
    {
        vm_hdlc_t tx, rx;
        hdlc_sink_t sink;
        uint8_t buf[VM_HDLC_TXBUF_SIZE];
        uint8_t pay[256];
        int i, n, esc = 0;

        for (i = 0; i < 256; i++) pay[i] = (uint8_t)i;

        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&tx, NULL, NULL);
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);
        /* 既定 ACCM = 全制御文字エスケープ */
        n = vm_hdlc_encode(&tx, VM_PPP_PROTO_IP, pay, 256,
                           buf, (int)sizeof(buf));
        for (i = 0; i < n; i++) if (buf[i] == 0x7D) esc++;

        /*
         * 期待するエスケープ数:
         *   ペイロード中の 0x00-0x1F = 32 個
         *   ペイロード中の 0x7E, 0x7D = 2 個
         *   ヘッダ FF 03 の 0x03 = 1 個
         *   Protocol 00 21 の 0x00 = 1 個
         *   合計 36 個 + FCS 次第
         */
        snprintf(detail, sizeof(detail),
                 "0x00-0xFF の 256 バイトで 0x7D が %d 個出現 (raw %d bytes)",
                 esc, n);
        check("全制御文字 + 7E/7D がエスケープされる", esc >= 36, detail);

        vm_hdlc_input(&rx, buf, n);
        snprintf(detail, sizeof(detail),
                 "frames=%d len=%d memcmp=%d", sink.count, sink.len,
                 sink.len == 256 ? memcmp(sink.payload, pay, 256) : -1);
        check("スタッフィングを外して 256 バイトが完全復元される",
              sink.count == 1 && sink.len == 256 &&
              memcmp(sink.payload, pay, 256) == 0, detail);
    }

    /* --- ACCM=0 で 8bit クリーン送信 --- */
    {
        vm_hdlc_t tx, rx;
        hdlc_sink_t sink;
        uint8_t buf[VM_HDLC_TXBUF_SIZE];
        uint8_t pay[256];
        int i, n, esc = 0;

        for (i = 0; i < 256; i++) pay[i] = (uint8_t)i;
        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&tx, NULL, NULL);
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);
        tx.tx_accm = 0x00000000u;   /* 交渉後: エスケープ不要 */

        n = vm_hdlc_encode(&tx, VM_PPP_PROTO_IP, pay, 256,
                           buf, (int)sizeof(buf));
        for (i = 0; i < n; i++) if (buf[i] == 0x7D) esc++;

        snprintf(detail, sizeof(detail),
                 "ACCM=0 で 0x7D が %d 個 (7E/7D の 2 個のみが理想)", esc);
        check("ACCM=0 なら制御文字はエスケープされない", esc <= 3, detail);

        vm_hdlc_input(&rx, buf, n);
        check("ACCM=0 のフレームも正しくデコードされる",
              sink.count == 1 && sink.len == 256 &&
              memcmp(sink.payload, pay, 256) == 0, "");
    }

    /* --- FCS 破損は黙って捨てる --- */
    {
        vm_hdlc_t tx, rx;
        hdlc_sink_t sink;
        uint8_t buf[VM_HDLC_TXBUF_SIZE];
        const uint8_t pay[] = { 0x01, 0x02, 0x00, 0x06 };
        int n;

        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&tx, NULL, NULL);
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);
        n = vm_hdlc_encode(&tx, VM_PPP_PROTO_LCP, pay, 4, buf, (int)sizeof(buf));
        buf[3] ^= 0xFF;   /* 1 バイト壊す */
        vm_hdlc_input(&rx, buf, n);

        snprintf(detail, sizeof(detail),
                 "frames=%d fcs_err=%llu", sink.count,
                 (unsigned long long)rx.stats.rx_fcs_err);
        check("FCS 不一致フレームは黙って破棄される",
              sink.count == 0 && rx.stats.rx_fcs_err == 1, detail);
    }

    /* --- 連続フラグ / 空フレーム / abort --- */
    {
        vm_hdlc_t rx;
        hdlc_sink_t sink;
        uint8_t junk[] = {
            0x7E, 0x7E, 0x7E,              /* 連続フラグ: 無視 */
            0x7E, 0x01, 0x7D, 0x7E,        /* abort: 破棄     */
            0x7E, 0x7E
        };
        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);
        vm_hdlc_input(&rx, junk, (int)sizeof(junk));

        snprintf(detail, sizeof(detail),
                 "frames=%d abort=%llu short=%llu", sink.count,
                 (unsigned long long)rx.stats.rx_abort,
                 (unsigned long long)rx.stats.rx_too_short);
        check("連続フラグ・空フレーム・abort で誤検出しない",
              sink.count == 0 && rx.stats.rx_abort == 1, detail);
    }

    /* --- 同期前のゴミを読み飛ばす --- */
    {
        vm_hdlc_t tx, rx;
        hdlc_sink_t sink;
        uint8_t buf[VM_HDLC_TXBUF_SIZE];
        uint8_t all[VM_HDLC_TXBUF_SIZE + 32];
        const uint8_t pay[] = { 0x01, 0x02, 0x00, 0x06 };
        const char *garbage = "CLIENTCLIENT\r\n";
        int n, g = (int)strlen(garbage);

        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&tx, NULL, NULL);
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);
        n = vm_hdlc_encode(&tx, VM_PPP_PROTO_LCP, pay, 4, buf, (int)sizeof(buf));

        memcpy(all, garbage, (size_t)g);
        memcpy(all + g, buf, (size_t)n);
        vm_hdlc_input(&rx, all, g + n);

        snprintf(detail, sizeof(detail),
                 "\"%s\" の後のフレームを frames=%d で取得", garbage, sink.count);
        check("フラグ同期前のゴミ (RAS の \"CLIENT\" 等) を捨てる",
              sink.count == 1, detail);
    }

    /* --- 1 バイトずつ投入しても同じ結果 (ストリーム分割耐性) --- */
    {
        vm_hdlc_t tx, rx;
        hdlc_sink_t sink;
        uint8_t buf[VM_HDLC_TXBUF_SIZE];
        uint8_t pay[1500];
        int i, n;

        for (i = 0; i < 1500; i++) pay[i] = (uint8_t)(i * 7 + 3);
        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&tx, NULL, NULL);
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);
        n = vm_hdlc_encode(&tx, VM_PPP_PROTO_IP, pay, 1500,
                           buf, (int)sizeof(buf));
        for (i = 0; i < n; i++) vm_hdlc_input(&rx, buf + i, 1);

        snprintf(detail, sizeof(detail),
                 "1500B フレームを %d バイトに分割投入 -> frames=%d len=%d",
                 n, sink.count, sink.len);
        check("1 バイト単位の分割投入でも復元できる",
              sink.count == 1 && sink.len == 1500 &&
              memcmp(sink.payload, pay, 1500) == 0, detail);
    }

    /* --- MRU 超過からの復帰 --- */
    {
        vm_hdlc_t rx;
        hdlc_sink_t sink;
        vm_hdlc_t tx;
        uint8_t big[VM_HDLC_RXBUF_SIZE + 64];
        uint8_t buf[VM_HDLC_TXBUF_SIZE];
        const uint8_t pay[] = { 0x01, 0x02, 0x00, 0x06 };
        int i, n;

        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);
        vm_hdlc_init(&tx, NULL, NULL);

        /* 巨大な (フラグで終わらない) ゴミを流す */
        big[0] = 0x7E;
        for (i = 1; i < (int)sizeof(big); i++) big[i] = 0x41;
        vm_hdlc_input(&rx, big, (int)sizeof(big));

        /* その後に正常フレームを流す */
        n = vm_hdlc_encode(&tx, VM_PPP_PROTO_LCP, pay, 4, buf, (int)sizeof(buf));
        vm_hdlc_input(&rx, buf, n);

        snprintf(detail, sizeof(detail),
                 "over-MRU=%llu の後に frames=%d",
                 (unsigned long long)rx.stats.rx_too_long, sink.count);
        check("MRU 超過を捨てた後、次のフレームで復帰する",
              rx.stats.rx_too_long >= 1 && sink.count == 1, detail);
    }

    /* --- ACFC / PFC で圧縮された受信フレーム --- */
    {
        vm_hdlc_t rx;
        hdlc_sink_t sink;
        /*
         * ACFC + PFC を両方使った最小の IP フレームを手で組む:
         *   Proto = 0x21 (PFC 圧縮された 0x0021)
         *   Info  = 0x45 0x00
         */
        uint8_t body[] = { 0x21, 0x45, 0x00 };
        uint8_t frame[16];
        uint16_t fcs = vm_hdlc_fcs16(VM_HDLC_FCS_INIT, body, 3);
        int n = 0, i;

        fcs = (uint16_t)(~fcs);
        frame[n++] = 0x7E;
        for (i = 0; i < 3; i++) {
            /* body に制御文字 0x00 が含まれるのでエスケープが必要 */
            if (vm_hdlc_needs_escape(0xFFFFFFFFu, body[i])) {
                frame[n++] = 0x7D; frame[n++] = (uint8_t)(body[i] ^ 0x20);
            } else frame[n++] = body[i];
        }
        {
            uint8_t f0 = (uint8_t)(fcs & 0xFF), f1 = (uint8_t)(fcs >> 8);
            if (vm_hdlc_needs_escape(0xFFFFFFFFu, f0)) {
                frame[n++] = 0x7D; frame[n++] = (uint8_t)(f0 ^ 0x20);
            } else frame[n++] = f0;
            if (vm_hdlc_needs_escape(0xFFFFFFFFu, f1)) {
                frame[n++] = 0x7D; frame[n++] = (uint8_t)(f1 ^ 0x20);
            } else frame[n++] = f1;
        }
        frame[n++] = 0x7E;

        memset(&sink, 0, sizeof(sink));
        vm_hdlc_init(&rx, hdlc_sink_cb, &sink);
        vm_hdlc_input(&rx, frame, n);

        snprintf(detail, sizeof(detail),
                 "frames=%d proto=%04X len=%d (ACFC+PFC 圧縮フレーム)",
                 sink.count, (unsigned)sink.proto, sink.len);
        check("ACFC/PFC で圧縮されたフレームを展開できる",
              sink.count == 1 && sink.proto == VM_PPP_PROTO_IP &&
              sink.len == 2, detail);
    }

    /* --- LCP は圧縮してはいけない --- */
    {
        vm_hdlc_t tx;
        uint8_t buf[VM_HDLC_TXBUF_SIZE];
        const uint8_t pay[] = { 0x01, 0x02, 0x00, 0x06 };
        int n_lcp, n_ip;

        vm_hdlc_init(&tx, NULL, NULL);
        tx.tx_acfc = true; tx.tx_pfc = true; tx.tx_accm = 0;

        n_lcp = vm_hdlc_encode(&tx, VM_PPP_PROTO_LCP, pay, 4,
                               buf, (int)sizeof(buf));
        /* 7E FF 03 C0 21 [4] FCS(2) 7E = 12 */
        snprintf(detail, sizeof(detail),
                 "LCP フレーム = %d bytes (FF 03 C0 21 が省略されていない)",
                 n_lcp);
        check("ACFC/PFC 合意後も LCP は無圧縮で送る", n_lcp == 12, detail);

        n_ip = vm_hdlc_encode(&tx, VM_PPP_PROTO_IP, pay, 4,
                              buf, (int)sizeof(buf));
        /* 7E 21 [4] FCS(2) 7E = 9 */
        snprintf(detail, sizeof(detail),
                 "IP フレーム = %d bytes (FF 03 省略 + Proto 1 バイト)", n_ip);
        check("IP フレームは ACFC+PFC で 3 バイト縮む", n_ip == 9, detail);
    }
}

/* ===========================================================================
 * [2] RAS 相手のフル交渉
 * =========================================================================== */

static void test_negotiation(void)
{
    vm_ppp_t  ppp;
    vm_ppp_cfg_t cfg;
    ras_t     ras;
    ipsink_t  sink;
    char      detail[256];
    char      sbuf[128];
    uint32_t  now = 1000;
    int       round;

    group("[2] Windows RAS 模擬ピアとのフル交渉 (認証なし)");

    memset(&sink, 0, sizeof(sink));
    vm_ppp_cfg_defaults(&cfg);
    cfg.require_auth = false;

    vm_ppp_init(&ppp, &cfg, ras_write_cb, &ras, ip_sink_cb, &sink, now);
    ras_init(&ras, &ppp);

    /* 回線確立 → 我々が LCP CR を送る */
    vm_ppp_open(&ppp, now);
    check("vm_ppp_open で ESTABLISH に入る",
          ppp.state == VM_PPP_ESTABLISH, vm_ppp_state_name(ppp.state));

    snprintf(detail, sizeof(detail),
             "我々が先に %llu bytes 送出 (RAS を待たない)",
             (unsigned long long)ras.q_total);
    check("我々から先に LCP Configure-Request を送る",
          ras.q_total > 0, detail);

    /* RAS も LCP CR を送る (Callback 付き = C-1) */
    ras_send_lcp_req(&ras);
    pump(&ras, now);

    snprintf(detail, sizeof(detail),
             "Callback(0x0D) が Reject された回数 = %d", ras.n_callback_rej);
    check("C-1: LCP Callback オプションを Configure-Reject する",
          ras.n_callback_rej == 1, detail);

    snprintf(detail, sizeof(detail),
             "我々の CR に Auth-Protocol が載っていた回数 = %d",
             ras.n_auth_opt_seen);
    check("C-6: 認証不要なら Auth-Protocol を載せない",
          ras.n_auth_opt_seen == 0, detail);

    /* LCP が両方向 Opened になったか */
    snprintf(detail, sizeof(detail),
             "our lcp=%s / ras lcp_open=%d state=%s",
             vm_ppp_status(&ppp, sbuf, sizeof(sbuf)), (int)ras.lcp_open,
             vm_ppp_state_name(ppp.state));
    check("難所A: 双方の CR が Ack されて LCP が Opened になる",
          ppp.lcp.state == VM_CP_OPENED && ras.lcp_open, detail);

    snprintf(detail, sizeof(detail),
             "tx_accm=%08X (RAS が要求した 0x000A0000 を採用)",
             (unsigned)ppp.hdlc.tx_accm);
    check("難所1: 送信 ACCM は相手の CR から取る",
          ppp.hdlc.tx_accm == 0x000A0000u, detail);

    snprintf(detail, sizeof(detail), "pfc=%d acfc=%d (RAS が要求した)",
             (int)ppp.hdlc.tx_pfc, (int)ppp.hdlc.tx_acfc);
    check("相手が要求した PFC/ACFC を送信側で有効化する",
          ppp.hdlc.tx_pfc && ppp.hdlc.tx_acfc, detail);

    snprintf(detail, sizeof(detail), "peer_magic=%08X (RAS の 0xDEADBEEF)",
             (unsigned)ppp.peer_magic);
    check("C-7: 相手の Magic-Number を記録する",
          ppp.peer_magic == 0xDEADBEEFu, detail);

    /* C-2: RAS が CCP / IPV6CP / CBCP を投げてくる */
    {
        uint8_t o[8] = { 0x01, 0x01, 0x00, 0x04 };
        ras_send_frame(&ras, VM_PPP_PROTO_CCP,    o, 4);
        ras_send_frame(&ras, VM_PPP_PROTO_IPV6CP, o, 4);
        ras_send_frame(&ras, VM_PPP_PROTO_CBCP,   o, 4);
        pump(&ras, now);
    }
    snprintf(detail, sizeof(detail),
             "CCP=%d IPV6CP=%d CBCP=%d に Protocol-Reject",
             ras.n_ccp_protorej, ras.n_ipv6cp_protorej, ras.n_cbcp_protorej);
    check("C-2: 未実装プロトコルに LCP Protocol-Reject を返す",
          ras.n_ccp_protorej == 1 && ras.n_ipv6cp_protorej == 1 &&
          ras.n_cbcp_protorej == 1, detail);

    /* 認証なしなので LCP Opened で即 NETWORK に入っているはず */
    check("認証不要なら LCP Opened から直接 NETWORK へ",
          ppp.state == VM_PPP_NETWORK || ppp.state == VM_PPP_RUNNING,
          vm_ppp_state_name(ppp.state));

    /* IPCP: RAS が VJ/0.0.0.0/DNS/NBNS 付きの CR を送る */
    ras_send_ipcp_req(&ras);

    /*
     * Rej → 再送 → Nak → 再送 の往復を pump が全部回す。
     * pump() は「両方向のキューが空になるまで」なので、
     * 交渉が収束するまで 1 回の呼び出しで完結する。
     */
    round = pump(&ras, now);
    snprintf(detail, sizeof(detail),
             "pump %d ラウンドで収束 (Rej→再送→Nak→再送→Ack)", round);
    check("IPCP の Rej/Nak 往復が有限ラウンドで収束する",
          round > 0 && round < 64, detail);

    snprintf(detail, sizeof(detail), "VJ が Reject された回数 = %d",
             ras.n_vj_rej);
    check("C-3: IPCP VJ 圧縮 (option 2) を Configure-Reject する",
          ras.n_vj_rej >= 1, detail);

    snprintf(detail, sizeof(detail), "NBNS(130/132) Reject = %d 個",
             ras.n_nbns_rej);
    check("C-5: NBNS (WINS) は Configure-Reject する",
          ras.n_nbns_rej >= 2, detail);

    {
        char a[16];
        snprintf(detail, sizeof(detail),
                 "RAS の IP が 0.0.0.0 -> %s に Nak で誘導 (%d 回)",
                 vm_ipv4_str(ras.my_ip, a, sizeof(a)), ras.n_ip_nak);
        check("C-4: 0.0.0.0 を Reject せず Nak でアドレスを配る",
              ras.n_ip_nak >= 1 && ras.my_ip == cfg.client_ip, detail);
    }
    {
        char a[16], b[16];
        snprintf(detail, sizeof(detail), "dns1=%s dns2=%s (Nak %d 個)",
                 vm_ipv4_str(ras.dns1, a, sizeof(a)),
                 vm_ipv4_str(ras.dns2, b, sizeof(b)), ras.n_dns_nak);
        check("C-5: DNS は Reject せず Nak で実値を配る",
              ras.dns1 == cfg.dns1 && ras.dns2 == cfg.dns2, detail);
    }

    snprintf(detail, sizeof(detail), "%s / ras ipcp_open=%d",
             vm_ppp_status(&ppp, sbuf, sizeof(sbuf)), (int)ras.ipcp_open);
    check("IPCP が双方向 Opened になり RUNNING へ",
          ppp.state == VM_PPP_RUNNING && vm_ppp_is_running(&ppp) &&
          ras.ipcp_open, detail);

    /* --- データ転送 --- */
    {
        /* RAS → インターネット方向 */
        uint8_t ip[64];
        int i;
        memset(ip, 0, sizeof(ip));
        ip[0] = 0x45; ip[1] = 0x00; ip[2] = 0x00; ip[3] = 0x40;
        ip[9] = 1;    /* ICMP */
        for (i = 20; i < 64; i++) ip[i] = (uint8_t)(i * 3);
        ras_send_frame(&ras, VM_PPP_PROTO_IP, ip, 64);
        pump(&ras, now);

        snprintf(detail, sizeof(detail),
                 "count=%d len=%d memcmp=%d", sink.count, sink.len,
                 sink.len == 64 ? memcmp(sink.pkt, ip, 64) : -1);
        check("RAS が送った IP パケットが NAT 層へ届く",
              sink.count == 1 && sink.len == 64 &&
              memcmp(sink.pkt, ip, 64) == 0, detail);
    }
    {
        /* インターネット → RAS 方向 */
        uint8_t ip[1400];
        int i;
        vm_err_t e;
        memset(ip, 0, sizeof(ip));
        ip[0] = 0x45;
        for (i = 20; i < 1400; i++) ip[i] = (uint8_t)(i ^ 0x5A);
        e = vm_ppp_send_ip(&ppp, ip, 1400);
        pump(&ras, now);

        snprintf(detail, sizeof(detail),
                 "err=%s ras n_ip_rx=%d len=%d memcmp=%d",
                 vm_strerror(e), ras.n_ip_rx, ras.last_ip_len,
                 ras.last_ip_len == 1400 ? memcmp(ras.last_ip, ip, 1400) : -1);
        check("NAT 層の IP パケットが RAS へ届く (1400 bytes)",
              e == VM_OK && ras.n_ip_rx == 1 && ras.last_ip_len == 1400 &&
              memcmp(ras.last_ip, ip, 1400) == 0, detail);
    }
    {
        /* MRU 超過は捨てる */
        uint8_t big[2000];
        vm_err_t e;
        int before = ras.n_ip_rx;
        memset(big, 0x42, sizeof(big));
        big[0] = 0x45;
        e = vm_ppp_send_ip(&ppp, big, 2000);
        pump(&ras, now);
        snprintf(detail, sizeof(detail),
                 "2000 bytes > peer MRU %d -> err=%s (n_ip_rx %d->%d)",
                 ppp.peer_mru, vm_strerror(e), before, ras.n_ip_rx);
        check("peer MRU を超える IP パケットは送らない",
              e != VM_OK && ras.n_ip_rx == before, detail);
    }

    /* --- 統計 --- */
    {
        snprintf(detail, sizeof(detail),
                 "ip_rx=%llu/%lluB ip_tx=%llu/%lluB lcp=%llu/%llu ipcp=%llu/%llu",
                 (unsigned long long)ppp.stats.ip_rx_pkts,
                 (unsigned long long)ppp.stats.ip_rx_bytes,
                 (unsigned long long)ppp.stats.ip_tx_pkts,
                 (unsigned long long)ppp.stats.ip_tx_bytes,
                 (unsigned long long)ppp.stats.lcp_rx,
                 (unsigned long long)ppp.stats.lcp_tx,
                 (unsigned long long)ppp.stats.ipcp_rx,
                 (unsigned long long)ppp.stats.ipcp_tx);
        check("統計が正しく積算されている",
              ppp.stats.ip_rx_pkts == 1 && ppp.stats.ip_rx_bytes == 64 &&
              ppp.stats.ip_tx_pkts == 1 && ppp.stats.ip_tx_bytes == 1400,
              detail);
    }

    /* --- Echo による死活監視 --- */
    {
        int before = ras.n_echo_req;
        now += VM_PPP_ECHO_INTERVAL_MS + 10;
        vm_ppp_tick(&ppp, now);
        pump(&ras, now);
        snprintf(detail, sizeof(detail),
                 "Echo-Request %d -> %d, echo_missed=%d (Reply で 0 に戻る)",
                 before, ras.n_echo_req, ppp.echo_missed);
        check("30 秒無通信で LCP Echo-Request を送り、応答で復帰",
              ras.n_echo_req == before + 1 && ppp.echo_missed == 0, detail);
    }

    /* --- 正常切断 --- */
    {
        now += 100;
        vm_ppp_close(&ppp, true, now);
        pump(&ras, now);
        snprintf(detail, sizeof(detail),
                 "RAS が受けた Term-Req = %d, our state=%s",
                 ras.n_term_req, vm_ppp_state_name(ppp.state));
        check("vm_ppp_close で Terminate-Request → Ack で DEAD",
              ras.n_term_req == 1 && ppp.state == VM_PPP_DEAD, detail);
    }
}

/* ===========================================================================
 * [3] PAP 認証あり
 * =========================================================================== */

static void test_pap(void)
{
    vm_ppp_t ppp;
    vm_ppp_cfg_t cfg;
    ras_t ras;
    ipsink_t sink;
    char detail[256];
    uint32_t now = 5000;
    int round;

    group("[3] PAP 認証あり (RFC 1334)");

    memset(&sink, 0, sizeof(sink));
    vm_ppp_cfg_defaults(&cfg);
    cfg.require_auth = true;
    snprintf(cfg.username, sizeof(cfg.username), "%s", "guest");
    snprintf(cfg.password, sizeof(cfg.password), "%s", "guest");

    vm_ppp_init(&ppp, &cfg, ras_write_cb, &ras, ip_sink_cb, &sink, now);
    ras_init(&ras, &ppp);
    vm_ppp_open(&ppp, now);

    ras_send_lcp_req(&ras);
    pump(&ras, now);

    snprintf(detail, sizeof(detail),
             "我々の CR に Auth-Protocol が %d 回載っていた",
             ras.n_auth_opt_seen);
    check("認証要求時は CR に Auth-Protocol(PAP) を載せる",
          ras.n_auth_opt_seen >= 1, detail);

    check("LCP Opened 後は AUTHENTICATE で待つ",
          ppp.state == VM_PPP_AUTHENTICATE, vm_ppp_state_name(ppp.state));

    /* 間違ったパスワード */
    {
        uint8_t o[64];
        int n = 0;
        const char *u = "guest", *p = "wrong";
        o[n++] = (uint8_t)strlen(u); memcpy(o + n, u, strlen(u)); n += (int)strlen(u);
        o[n++] = (uint8_t)strlen(p); memcpy(o + n, p, strlen(p)); n += (int)strlen(p);
        ras_send_ctrl(&ras, VM_PPP_PROTO_PAP, VM_PAP_AUTH_REQ, 1, o, n);
        pump(&ras, now);

        snprintf(detail, sizeof(detail), "state=%s auth_ok=%d",
                 vm_ppp_state_name(ppp.state), (int)ppp.auth_ok);
        check("誤ったパスワードでは認証が通らない",
              !ppp.auth_ok && ppp.state == VM_PPP_AUTHENTICATE, detail);
    }

    /* 正しいパスワード */
    {
        uint8_t o[64];
        int n = 0;
        const char *u = "guest", *p = "guest";
        o[n++] = (uint8_t)strlen(u); memcpy(o + n, u, strlen(u)); n += (int)strlen(u);
        o[n++] = (uint8_t)strlen(p); memcpy(o + n, p, strlen(p)); n += (int)strlen(p);
        ras_send_ctrl(&ras, VM_PPP_PROTO_PAP, VM_PAP_AUTH_REQ, 2, o, n);
        pump(&ras, now);

        snprintf(detail, sizeof(detail), "state=%s auth_ok=%d",
                 vm_ppp_state_name(ppp.state), (int)ppp.auth_ok);
        check("正しい資格情報で認証が通り NETWORK へ進む",
              ppp.auth_ok && (ppp.state == VM_PPP_NETWORK ||
                              ppp.state == VM_PPP_RUNNING), detail);
    }

    /* IPCP まで通す */
    ras_send_ipcp_req(&ras);
    round = pump(&ras, now);
    snprintf(detail, sizeof(detail), "%s (pump %d ラウンド)",
             vm_ppp_state_name(ppp.state), round);
    check("PAP 経由でも最終的に RUNNING に到達する",
          vm_ppp_is_running(&ppp), detail);
}

/* ===========================================================================
 * [4] 認証前の IPCP / 異常系
 * =========================================================================== */

static void test_edge_cases(void)
{
    char detail[256];

    group("[4] 異常系と RFC 準拠の細部");

    /* --- 認証前の IPCP は無視する (Protocol-Reject を返さない) --- */
    {
        vm_ppp_t ppp;
        vm_ppp_cfg_t cfg;
        ras_t ras;
        ipsink_t sink;
        uint32_t now = 9000;
        uint64_t rej_before;

        memset(&sink, 0, sizeof(sink));
        vm_ppp_cfg_defaults(&cfg);
        cfg.require_auth = true;
        snprintf(cfg.username, sizeof(cfg.username), "%s", "u");
        snprintf(cfg.password, sizeof(cfg.password), "%s", "p");

        vm_ppp_init(&ppp, &cfg, ras_write_cb, &ras, ip_sink_cb, &sink, now);
        ras_init(&ras, &ppp);
        vm_ppp_open(&ppp, now);
        ras_send_lcp_req(&ras);
        pump(&ras, now);

        rej_before = ppp.stats.proto_rej_tx;
        ras_send_ipcp_req(&ras);
        pump(&ras, now);

        snprintf(detail, sizeof(detail),
                 "state=%s proto_rej %llu->%llu ipcp=%s",
                 vm_ppp_state_name(ppp.state),
                 (unsigned long long)rej_before,
                 (unsigned long long)ppp.stats.proto_rej_tx,
                 ppp.ipcp.state == VM_CP_INITIAL ? "INITIAL" : "進んだ");
        check("認証前の IPCP は無視する (Protocol-Reject しない)",
              ppp.state == VM_PPP_AUTHENTICATE &&
              ppp.stats.proto_rej_tx == rej_before, detail);
    }

    /* --- 認証タイムアウト --- */
    {
        vm_ppp_t ppp;
        vm_ppp_cfg_t cfg;
        ras_t ras;
        ipsink_t sink;
        uint32_t now = 20000;

        memset(&sink, 0, sizeof(sink));
        vm_ppp_cfg_defaults(&cfg);
        cfg.require_auth = true;
        snprintf(cfg.username, sizeof(cfg.username), "%s", "u");
        snprintf(cfg.password, sizeof(cfg.password), "%s", "p");

        vm_ppp_init(&ppp, &cfg, ras_write_cb, &ras, ip_sink_cb, &sink, now);
        ras_init(&ras, &ppp);
        vm_ppp_open(&ppp, now);
        ras_send_lcp_req(&ras);
        pump(&ras, now);

        now += VM_PPP_AUTH_TIMEOUT_MS + 100;
        vm_ppp_tick(&ppp, now);
        pump(&ras, now);

        snprintf(detail, sizeof(detail), "%d ms 無応答 -> state=%s term_req=%d",
                 VM_PPP_AUTH_TIMEOUT_MS, vm_ppp_state_name(ppp.state),
                 ras.n_term_req);
        check("PAP が来ないまま 20 秒でタイムアウト切断",
              ppp.state == VM_PPP_DEAD || ras.n_term_req >= 1, detail);
    }

    /* --- LCP Max-Configure で諦める --- */
    {
        vm_ppp_t ppp;
        ras_t ras;   /* 応答しないピア */
        ipsink_t sink;
        uint32_t now = 30000;
        int i;
        uint64_t tx_before;

        memset(&sink, 0, sizeof(sink));
        memset(&ras, 0, sizeof(ras));
        ras.ppp = &ppp;
        ras.tx_accm = 0xFFFFFFFFu;

        vm_ppp_init(&ppp, NULL, ras_write_cb, &ras, ip_sink_cb, &sink, now);
        vm_ppp_open(&ppp, now);
        tx_before = ppp.stats.lcp_tx;

        /* 誰も応答しないまま時間だけ進める */
        for (i = 0; i < VM_PPP_MAX_CONFIGURE + 3; i++) {
            now += VM_PPP_RESTART_MS + 10;
            vm_ppp_tick(&ppp, now);
        }

        snprintf(detail, sizeof(detail),
                 "CR を %llu 回再送して state=%s (Max-Configure=%d)",
                 (unsigned long long)(ppp.stats.lcp_tx - tx_before),
                 vm_ppp_state_name(ppp.state), VM_PPP_MAX_CONFIGURE);
        check("無応答なら Max-Configure 回で DEAD になる",
              ppp.state == VM_PPP_DEAD &&
              (ppp.stats.lcp_tx - tx_before) <= VM_PPP_MAX_CONFIGURE + 1,
              detail);
    }

    /* --- Echo 無応答で回線断を検出 --- */
    {
        vm_ppp_t ppp;
        vm_ppp_cfg_t cfg;
        ras_t ras;
        ipsink_t sink;
        uint32_t now = 40000;
        int round, i;

        memset(&sink, 0, sizeof(sink));
        vm_ppp_cfg_defaults(&cfg);
        vm_ppp_init(&ppp, &cfg, ras_write_cb, &ras, ip_sink_cb, &sink, now);
        ras_init(&ras, &ppp);
        vm_ppp_open(&ppp, now);
        ras_send_lcp_req(&ras);
        ras_send_ipcp_req(&ras);
        round = pump(&ras, now);
        (void)round;

        /* RAS が消える: write_cb を捨てて応答を止める */
        ppp.write_cb = NULL;

        for (i = 0; i <= VM_PPP_ECHO_MAX_MISS + 1; i++) {
            now += VM_PPP_ECHO_INTERVAL_MS + 10;
            vm_ppp_tick(&ppp, now);
        }

        snprintf(detail, sizeof(detail),
                 "Echo 無応答 %d 回 -> state=%s (閾値 %d)",
                 ppp.echo_missed, vm_ppp_state_name(ppp.state),
                 VM_PPP_ECHO_MAX_MISS);
        check("Echo 無応答が続けば回線断とみなして DEAD",
              ppp.state == VM_PPP_DEAD, detail);
    }

    /* --- 未接続状態での IP 送信は拒否 --- */
    {
        vm_ppp_t ppp;
        ras_t ras;
        ipsink_t sink;
        uint8_t ip[64];
        vm_err_t e;

        memset(&sink, 0, sizeof(sink));
        memset(&ras, 0, sizeof(ras));
        ras.ppp = &ppp; ras.tx_accm = 0xFFFFFFFFu;
        memset(ip, 0x45, sizeof(ip));

        vm_ppp_init(&ppp, NULL, ras_write_cb, &ras, ip_sink_cb, &sink, 0);
        e = vm_ppp_send_ip(&ppp, ip, 64);
        snprintf(detail, sizeof(detail), "state=%s err=%s",
                 vm_ppp_state_name(ppp.state), vm_strerror(e));
        check("DEAD 状態で vm_ppp_send_ip は VM_ERR_STATE",
              e == VM_ERR_STATE, detail);
    }

    /* --- 不正な TLV (length < 2) で無限ループしない --- */
    {
        vm_ppp_t ppp;
        ras_t ras;
        ipsink_t sink;
        uint8_t evil[8];

        memset(&sink, 0, sizeof(sink));
        memset(&ras, 0, sizeof(ras));
        ras.ppp = &ppp; ras.tx_accm = 0xFFFFFFFFu; ras.next_id = 1;
        ras.in_frame = false;

        vm_ppp_init(&ppp, NULL, ras_write_cb, &ras, ip_sink_cb, &sink, 0);
        vm_ppp_open(&ppp, 0);

        /* type=1, length=0 の不正オプション */
        evil[0] = 1; evil[1] = 0; evil[2] = 1; evil[3] = 1;
        ras_send_ctrl(&ras, VM_PPP_PROTO_LCP, VM_PPP_CONF_REQ, 1, evil, 4);
        pump(&ras, 0);

        check("length<2 の不正 TLV でハングしない (到達すれば合格)",
              1, "opt_next() が false を返して走査終了");
    }

    /* --- IP アドレス文字列変換 --- */
    {
        uint32_t v;
        char b[16];
        bool ok1 = vm_ipv4_parse("192.168.99.1", &v);
        const char *s = vm_ipv4_str(v, b, sizeof(b));
        bool ok2 = !vm_ipv4_parse("192.168.99", &v);
        bool ok3 = !vm_ipv4_parse("256.1.1.1", &v);
        bool ok4 = !vm_ipv4_parse("1.2.3.4.5", &v);

        snprintf(detail, sizeof(detail),
                 "\"192.168.99.1\" -> 0x%08X -> \"%s\"; 不正入力 3 種を拒否=%d%d%d",
                 (unsigned)v, s, (int)ok2, (int)ok3, (int)ok4);
        check("IPv4 文字列の相互変換と入力検証",
              ok1 && v == 0xC0A86301u && strcmp(s, "192.168.99.1") == 0 &&
              ok2 && ok3 && ok4, detail);
    }
}

/* ===========================================================================
 * [5] 大量転送のストレス
 * =========================================================================== */

static void test_throughput(void)
{
    vm_ppp_t ppp;
    vm_ppp_cfg_t cfg;
    ras_t ras;
    ipsink_t sink;
    char detail[256];
    uint32_t now = 60000;
    int round, i;
    int sent = 0, recvd_ok = 0;
    uint8_t ip[1500];

    group("[5] 連続転送 (双方向 1000 パケット)");

    memset(&sink, 0, sizeof(sink));
    vm_ppp_cfg_defaults(&cfg);
    vm_ppp_init(&ppp, &cfg, ras_write_cb, &ras, ip_sink_cb, &sink, now);
    ras_init(&ras, &ppp);
    vm_ppp_open(&ppp, now);
    ras_send_lcp_req(&ras);
    ras_send_ipcp_req(&ras);
    round = pump(&ras, now);
    (void)round;

    if (!vm_ppp_is_running(&ppp)) {
        check("転送前に RUNNING に到達", 0, vm_ppp_state_name(ppp.state));
        return;
    }

    /* インターネット → RAS を 1000 パケット */
    for (i = 0; i < 1000; i++) {
        int len = 40 + (i % 1400);
        int j;
        for (j = 0; j < len; j++) ip[j] = (uint8_t)((i * 31 + j * 7) & 0xFF);
        ip[0] = 0x45;
        if (vm_ppp_send_ip(&ppp, ip, len) == VM_OK) {
            /* 1 パケットごとに流す (キューを溢れさせない) */
            pump(&ras, now);
            sent++;
            if (ras.last_ip_len == len && memcmp(ras.last_ip, ip, (size_t)len) == 0)
                recvd_ok++;
        }
    }

    snprintf(detail, sizeof(detail),
             "送信 %d / 完全一致受信 %d (tx %llu bytes)",
             sent, recvd_ok, (unsigned long long)ppp.stats.ip_tx_bytes);
    check("1000 パケットが 1 バイトの誤りもなく RAS へ届く",
          sent == 1000 && recvd_ok == 1000, detail);

    /* RAS → インターネットを 1000 パケット */
    {
        int ok = 0;
        for (i = 0; i < 1000; i++) {
            int len = 40 + (i % 1400);
            int j;
            for (j = 0; j < len; j++) ip[j] = (uint8_t)((i * 17 + j * 11) & 0xFF);
            ip[0] = 0x45;
            sink.count = 0;
            ras_send_frame(&ras, VM_PPP_PROTO_IP, ip, len);
            pump(&ras, now);
            if (sink.count == 1 && sink.len == len &&
                memcmp(sink.pkt, ip, (size_t)len) == 0) ok++;
        }
        snprintf(detail, sizeof(detail),
                 "完全一致 %d/1000 (rx %llu bytes, FCS err %llu)",
                 ok, (unsigned long long)ppp.stats.ip_rx_bytes,
                 (unsigned long long)ppp.hdlc.stats.rx_fcs_err);
        check("1000 パケットが誤りなく NAT 層へ届く",
              ok == 1000 && ppp.hdlc.stats.rx_fcs_err == 0, detail);
    }

    /* --- HDLC 統計の健全性 --- */
    snprintf(detail, sizeof(detail),
             "rx_frames=%llu tx_frames=%llu fcs_err=%llu too_long=%llu abort=%llu",
             (unsigned long long)ppp.hdlc.stats.rx_frames,
             (unsigned long long)ppp.hdlc.stats.tx_frames,
             (unsigned long long)ppp.hdlc.stats.rx_fcs_err,
             (unsigned long long)ppp.hdlc.stats.rx_too_long,
             (unsigned long long)ppp.hdlc.stats.rx_abort);
    check("エラー統計がすべてゼロ",
          ppp.hdlc.stats.rx_fcs_err == 0 && ppp.hdlc.stats.rx_too_long == 0 &&
          ppp.hdlc.stats.rx_abort == 0, detail);
}

/* =========================================================================== */

int main(void)
{
    printf("=======================================================\n");
    printf(" VModem PPP サーバ / HDLC フレーミング 検証テスト\n");
    printf("=======================================================\n");

    vm_log_init(VM_LOG_NONE, NULL);

    test_hdlc();
    test_negotiation();
    test_pap();
    test_edge_cases();
    test_throughput();

    printf("\n=======================================================\n");
    printf(" 結果: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");
    return g_fail == 0 ? 0 : 1;
}
