/*
 * vm_nat_loopback.c - 内蔵の最小応答器バックエンド
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * なぜこのバックエンドが必要なのか
 * ===========================================================================
 * libslirp は外部 DLL であり、CI や開発環境に必ずあるとは限らない。
 * しかし「PPP → 偽 Ethernet 層 → NAT → 偽 Ethernet 層 → PPP」の
 * **往復経路そのもの**は libslirp と無関係にテストできるべきである。
 *
 * このバックエンドは実インターネットには一切出ず、以下だけを行う。
 *
 *   - ARP Request が来たら (共通部が) 応答する
 *   - ICMP Echo Request に Echo Reply を返す      (ping が通る)
 *   - UDP パケットを送信元へ折り返す              (UDP echo)
 *   - それ以外は破棄
 *
 * これにより
 *   「Windows で ping 192.168.99.1 が通るか」
 * という最小の疎通確認が libslirp 抜きで可能になり、切り分けが容易になる。
 * 実際に「PPP は繋がるのに ping が通らない」時、このバックエンドで
 * 試せば原因が PPP/Ethernet 層か libslirp かを即座に切り分けられる。
 *
 * ===========================================================================
 * 難所 8: ICMP Echo Reply のチェックサム差分更新
 * ===========================================================================
 * ICMP Echo Reply を作るには Type を 8 -> 0 に書き換える。
 * この時チェックサムを全計算し直してもよいが、RFC 1624 の差分更新を
 * 使うのが定石である。ただし **素朴な差分計算は間違う**。
 *
 * Type を 8 から 0 に変える = 上位バイトが 8 減る = 値が 0x0800 減る。
 * ICMP チェックサムは 1 の補数和の補数なので、
 *
 *      new_sum = ~(~old_sum - 0x0800)
 *
 * となり、桁借りの畳み込みが必要。ここを
 *      checksum += 0x0800;
 * と書くと 1 の補数演算の折り返しを無視するため、
 * 「稀に (0.0015% 程度の確率で) チェックサム誤りの Reply を返す」
 * という極めて再現困難なバグになる。
 *
 * 本実装は安全側に倒し、**全計算し直す**。ICMP パケットは小さく、
 * 電話回線速度 (最大 33.6kbps) では計算コストは完全に無視できる。
 * 「賢い最適化より、正しい素朴さ」を選ぶべき典型例。
 * ===========================================================================
 */
#include <string.h>
#include <stdio.h>

#include "vm_nat_internal.h"

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP  17

#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

typedef struct {
    uint8_t  buf[VM_NAT_FRAME_MAX];   /* 応答フレーム組み立て用 */
    uint64_t icmp_replied;
    uint64_t udp_echoed;
    uint64_t tcp_dropped;
    uint64_t other_dropped;
} loop_impl_t;

/* -------------------------------------------------------------------------- */
static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* ==========================================================================
 * IP ヘッダの送信元/宛先を入れ替えて、チェックサムを張り直す
 * ==========================================================================
 * IP ヘッダチェックサムは送信元・宛先アドレスを含むが、
 * **入れ替えるだけなら 1 の補数和は変化しない** (加算は可換)。
 * つまり厳密にはチェックサムを再計算する必要はない。
 *
 * しかし TTL を戻す処理を入れると変わるので、混乱を避けるため常に
 * 再計算する。ここでも「賢い最適化より正しい素朴さ」。
 */
static void ip_swap_addrs(uint8_t *ip, int ihl)
{
    uint8_t tmp[4];

    memcpy(tmp, ip + 12, 4);
    memcpy(ip + 12, ip + 16, 4);
    memcpy(ip + 16, tmp, 4);

    ip[8] = 64;                      /* TTL を戻す */
    put_be16(ip + 10, 0);            /* チェックサム欄を 0 にしてから計算 */
    put_be16(ip + 10, vm_ip_checksum(ip, ihl));
}

/* ==========================================================================
 * ICMP Echo 応答
 * ========================================================================== */
static bool handle_icmp(vm_nat_t *n, loop_impl_t *li,
                        const uint8_t *ip, int tot, int ihl)
{
    uint8_t *pkt  = li->buf;
    uint8_t *icmp;
    int      icmp_len = tot - ihl;

    if (icmp_len < 8)
        return false;

    if (ip[ihl] != ICMP_ECHO_REQUEST)
        return false;

    if (tot > (int)sizeof(li->buf))
        return false;

    memcpy(pkt, ip, (size_t)tot);
    icmp = pkt + ihl;

    /* Type 8 -> 0 */
    icmp[0] = ICMP_ECHO_REPLY;

    /*
     * ★ 難所 8 ★
     * 差分更新ではなく全計算。ICMP チェックサムは
     * 「ICMP ヘッダ + ペイロード全体」を対象とする
     * (IP 疑似ヘッダは含まない。TCP/UDP と違う点)。
     */
    put_be16(icmp + 2, 0);
    put_be16(icmp + 2, vm_ip_checksum(icmp, icmp_len));

    ip_swap_addrs(pkt, ihl);

    li->icmp_replied++;
    VM_LOGT("nat(loopback): ICMP Echo Reply を生成 (%d バイト)", tot);

    /* 共通部の経路に戻すため、Ethernet でくるんで recv_frame へ渡す */
    {
        uint8_t frame[VM_NAT_FRAME_MAX];
        int     flen;
        uint8_t save_mac[VM_ETH_ALEN];
        bool    save_known;

        /*
         * ここは「NAT からゲストへ送る」方向なので、送信元 MAC は
         * ゲートウェイ側でなければならない。vm_eth_encap() は
         * guest_mac を送信元に使う設計なので、一時的に入れ替える。
         *
         * 実際の libslirp では slirp 自身が正しい MAC を付けるため
         * この操作は不要。loopback バックエンド固有の細工である。
         */
        memcpy(save_mac, n->eth.host_mac, VM_ETH_ALEN);
        save_known = n->eth.host_mac_known;

        flen = VM_ETH_HLEN + tot;
        if (flen > (int)sizeof(frame))
            return false;

        memcpy(frame, n->eth.guest_mac, VM_ETH_ALEN);        /* 宛先=ゲスト */
        memcpy(frame + VM_ETH_ALEN, save_known ? save_mac
                                               : n->eth.guest_mac,
               VM_ETH_ALEN);                                 /* 送信元=GW   */
        put_be16(frame + 12, VM_ETH_P_IP);
        memcpy(frame + VM_ETH_HLEN, pkt, (size_t)tot);

        /* 60 バイト未満ならパディング (難所 4 の検証にもなる) */
        if (flen < VM_ETH_MIN_FRAME) {
            memset(frame + flen, 0, (size_t)(VM_ETH_MIN_FRAME - flen));
            flen = VM_ETH_MIN_FRAME;
        }

        vm_nat_backend_recv_frame(n, frame, flen);
    }

    return true;
}

/* ==========================================================================
 * UDP 折り返し
 * ========================================================================== */
static bool handle_udp(vm_nat_t *n, loop_impl_t *li,
                       const uint8_t *ip, int tot, int ihl)
{
    uint8_t *pkt = li->buf;
    uint8_t *udp;
    int      udp_len = tot - ihl;

    if (udp_len < 8 || tot > (int)sizeof(li->buf))
        return false;

    memcpy(pkt, ip, (size_t)tot);
    udp = pkt + ihl;

    /* 送信元ポートと宛先ポートを入れ替える */
    {
        uint8_t tmp[2];
        memcpy(tmp, udp + 0, 2);
        memcpy(udp + 0, udp + 2, 2);
        memcpy(udp + 2, tmp, 2);
    }

    /*
     * UDP チェックサムは IPv4 では省略可 (0 = 検証しない)。
     * 疑似ヘッダを含む正しい計算は煩雑なので、折り返しでは 0 にする。
     * これは RFC 768 で明示的に許されている。
     */
    put_be16(udp + 6, 0);

    ip_swap_addrs(pkt, ihl);

    li->udp_echoed++;
    VM_LOGT("nat(loopback): UDP を折り返し (%d バイト)", tot);

    {
        uint8_t frame[VM_NAT_FRAME_MAX];
        int     flen = VM_ETH_HLEN + tot;

        if (flen > (int)sizeof(frame))
            return false;

        memcpy(frame, n->eth.guest_mac, VM_ETH_ALEN);
        memcpy(frame + VM_ETH_ALEN,
               n->eth.host_mac_known ? n->eth.host_mac : n->eth.guest_mac,
               VM_ETH_ALEN);
        put_be16(frame + 12, VM_ETH_P_IP);
        memcpy(frame + VM_ETH_HLEN, pkt, (size_t)tot);

        if (flen < VM_ETH_MIN_FRAME) {
            memset(frame + flen, 0, (size_t)(VM_ETH_MIN_FRAME - flen));
            flen = VM_ETH_MIN_FRAME;
        }

        vm_nat_backend_recv_frame(n, frame, flen);
    }

    return true;
}

/* ==========================================================================
 * vtable 実装
 * ========================================================================== */
static vm_err_t loop_open(vm_nat_t *n)
{
    static loop_impl_t inst;      /* 単一インスタンス前提 */

    memset(&inst, 0, sizeof(inst));
    n->impl = &inst;

    VM_LOGI("nat(loopback): 内蔵応答器を使用 "
            "(ICMP Echo / UDP echo のみ。実インターネットには出ません)");
    return VM_OK;
}

static void loop_close(vm_nat_t *n)
{
    loop_impl_t *li = (loop_impl_t *)n->impl;

    if (li != NULL) {
        VM_LOGI("nat(loopback): icmp=%llu udp=%llu tcp_drop=%llu other=%llu",
                (unsigned long long)li->icmp_replied,
                (unsigned long long)li->udp_echoed,
                (unsigned long long)li->tcp_dropped,
                (unsigned long long)li->other_dropped);
    }
    n->impl = NULL;
}

static vm_err_t loop_send_frame(vm_nat_t *n, const uint8_t *frame, int len)
{
    loop_impl_t *li = (loop_impl_t *)n->impl;
    uint16_t     proto;
    const uint8_t *ip;
    int          tot = 0, ihl;

    if (li == NULL || len < VM_ETH_HLEN)
        return VM_ERR_INVAL;

    proto = get_be16(frame + 12);

    /*
     * ARP Request が来たら学習して Reply を返す。
     * 本物の libslirp では arp_input() がこれを行う。loopback でも
     * 同じ挙動を再現しておかないと、共通部の Gratuitous ARP 経路が
     * テストされないままになる。
     */
    if (proto == VM_ETH_P_ARP) {
        if (len >= VM_ARP_FRAME_LEN) {
            const uint8_t *b = frame + VM_ETH_HLEN;
            if (get_be16(b + 6) == VM_ARP_OP_REQUEST) {
                /*
                 * 送信元を学習した、という体で ARP Reply を返す。
                 * Target IP が host_ip (ゲートウェイ) の時だけ答える。
                 */
                uint32_t tip = ((uint32_t)b[24] << 24) | ((uint32_t)b[25] << 16) |
                               ((uint32_t)b[26] << 8) | (uint32_t)b[27];
                if (tip == n->cfg.host_ip) {
                    uint8_t rep[VM_ARP_FRAME_LEN];
                    uint8_t gw_mac[VM_ETH_ALEN];
                    uint8_t *rb;

                    /* ゲートウェイの偽 MAC を host_ip から生成 */
                    vm_eth_make_mac(n->cfg.host_ip, gw_mac);
                    gw_mac[3] ^= 0x80u;   /* guest_mac と必ず異ならせる */

                    memcpy(rep, b + 8, VM_ETH_ALEN);         /* 宛先 */
                    memcpy(rep + VM_ETH_ALEN, gw_mac, VM_ETH_ALEN);
                    put_be16(rep + 12, VM_ETH_P_ARP);

                    rb = rep + VM_ETH_HLEN;
                    put_be16(rb + 0, VM_ARP_HRD_ETHER);
                    put_be16(rb + 2, VM_ARP_PRO_IP);
                    rb[4] = VM_ETH_ALEN;
                    rb[5] = 4;
                    put_be16(rb + 6, VM_ARP_OP_REPLY);
                    memcpy(rb + 8, gw_mac, VM_ETH_ALEN);
                    rb[14] = (uint8_t)(n->cfg.host_ip >> 24);
                    rb[15] = (uint8_t)(n->cfg.host_ip >> 16);
                    rb[16] = (uint8_t)(n->cfg.host_ip >> 8);
                    rb[17] = (uint8_t)(n->cfg.host_ip);
                    memcpy(rb + 18, b + 8, VM_ETH_ALEN);
                    memcpy(rb + 24, b + 14, 4);

                    vm_nat_backend_recv_frame(n, rep, VM_ARP_FRAME_LEN);
                }
            }
        }
        return VM_OK;
    }

    if (proto != VM_ETH_P_IP) {
        li->other_dropped++;
        return VM_OK;
    }

    ip = frame + VM_ETH_HLEN;
    if (!vm_ip4_check(ip, len - VM_ETH_HLEN, &tot)) {
        vm_nat_backend_guest_error(n, "IPv4 ヘッダ不正");
        return VM_OK;
    }
    ihl = (int)(ip[0] & 0x0Fu) * 4;

    switch (ip[9]) {
    case IP_PROTO_ICMP:
        if (!handle_icmp(n, li, ip, tot, ihl))
            li->other_dropped++;
        break;
    case IP_PROTO_UDP:
        if (!handle_udp(n, li, ip, tot, ihl))
            li->other_dropped++;
        break;
    case IP_PROTO_TCP:
        /*
         * TCP は状態を持つので loopback では扱わない。
         * 実インターネットへの TCP は libslirp バックエンドの担当。
         */
        li->tcp_dropped++;
        VM_LOGT("nat(loopback): TCP は非対応 (libslirp を使ってください)");
        break;
    default:
        li->other_dropped++;
        break;
    }

    return VM_OK;
}

static int loop_poll(vm_nat_t *n, int max_block_ms)
{
    (void)n;
    /*
     * ソケットを一切持たないのでポーリングする対象が無い。
     * ただし 0 を返すと呼び出し側が全力で回って CPU を焼くので、
     * 最低 10ms は待たせる。
     */
    return (max_block_ms > 0 && max_block_ms < 10) ? max_block_ms : 10;
}

static const vm_nat_backend_ops_t loop_ops = {
    "loopback",
    loop_open,
    loop_close,
    loop_send_frame,
    loop_poll,
    NULL,
    NULL
};

const vm_nat_backend_ops_t *vm_nat_ops_loopback(void)
{
    return &loop_ops;
}
