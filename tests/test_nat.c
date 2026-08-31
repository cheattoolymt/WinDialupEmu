/*
 * test_nat.c - 偽イーサネット層 / ARP / NAT 経路の検証
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * 何を検証するのか
 * ===========================================================================
 * このテストの主眼は「vm_eth.h に書いた 4 つの難所が本当に回避できて
 * いるか」を機械的に確認することにある。特に:
 *
 *   難所 1 (ARP)      : Gratuitous ARP が正しい形で出るか、
 *                       ARP Request に必ず応答するか
 *   難所 2 (bcast)    : ブロードキャスト宛を捨てていないか
 *   難所 3 (MAC)      : 生成 MAC がユニキャストかつローカル管理か
 *   難所 4 (padding)  : 60 バイト未満のフレームのパディングを
 *                       IP パケットに混ぜていないか
 *
 * さらに最終グループでは **PPP を実際に通して** 疎通を確認する。
 *   test_ppp.c で作った mock RAS ─ vm_ppp ─ vm_eth ─ vm_nat(loopback)
 * を全部繋ぎ、Windows から ping を打った時と同じ経路を再現する。
 * ここが通れば「PPP は繋がるのに ping が通らない」系のバグは
 * 構造的に排除できている。
 *
 * ===========================================================================
 * 再入に関する注意 (test_ppp.c で得た教訓の再適用)
 * ===========================================================================
 * loopback バックエンドは send_frame の中から
 * vm_nat_backend_recv_frame() を **同期的に** 呼ぶ。
 * これは「NAT からゲストへ」の向きなので、そこから先は
 * vm_ppp_send_ip() → HDLC encode → write_cb と進む。
 *
 * ここで write_cb が再び vm_ppp_input() を呼ぶと test_ppp.c で
 * 経験したスタックオーバフローが再発する。したがって本テストでも
 * **write_cb はキューに詰めるだけ** とし、ドレインは必ず
 * トップレベルの pump() から行う。
 * ===========================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "vmodem/vm_eth.h"
#include "vmodem/vm_nat.h"
#include "vmodem/vm_ppp.h"
#include "vmodem/vm_hdlc.h"
#include "vmodem/vm_log.h"

/*
 * ホワイトボックステストなので内部構造体を覗く。
 * vm_nat_t は公開ヘッダでは不完全型 (実装隠蔽) なので、
 * テストだけが内部ヘッダを直接 include する。
 * 本番コードからこのヘッダを include してはいけない。
 */
#include "vm_nat_internal.h"

/* ==========================================================================
 * テスト基盤
 * ========================================================================== */
static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool ok, const char *fmt, ...)
{
    char    detail[512];
    va_list ap;

    detail[0] = '\0';
    if (fmt != NULL) {
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }

    if (ok) {
        g_pass++;
        printf("  [ OK ] %-58s %s\n", name, detail);
    } else {
        g_fail++;
        printf("  [FAIL] %-58s %s\n", name, detail);
    }
}

static void group(const char *title)
{
    printf("\n[%s]\n", title);
}

/* ==========================================================================
 * バイト列ヘルパ
 * ========================================================================== */
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v & 0xFF);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

#define GUEST_IP 0xC0A86302u    /* 192.168.99.2 */
#define HOST_IP  0xC0A86301u    /* 192.168.99.1 */

/* ==========================================================================
 * IPv4 パケット組み立て
 * ========================================================================== */
static int build_ipv4(uint8_t *out, uint8_t proto,
                      uint32_t src, uint32_t dst,
                      const uint8_t *payload, int plen)
{
    int tot = 20 + plen;

    memset(out, 0, 20);
    out[0] = 0x45;                          /* v4, IHL=5 */
    out[1] = 0x00;                          /* DSCP/ECN  */
    put_be16(out + 2, (uint16_t)tot);       /* Total Length */
    put_be16(out + 4, 0x1234);              /* ID */
    put_be16(out + 6, 0x4000);              /* DF */
    out[8] = 64;                            /* TTL */
    out[9] = proto;
    put_be32(out + 12, src);
    put_be32(out + 16, dst);
    put_be16(out + 10, 0);
    put_be16(out + 10, vm_ip_checksum(out, 20));

    if (payload != NULL && plen > 0)
        memcpy(out + 20, payload, (size_t)plen);

    return tot;
}

static int build_icmp_echo(uint8_t *out, uint32_t src, uint32_t dst,
                           uint16_t id, uint16_t seq, int datalen)
{
    uint8_t icmp[2048];
    int     ilen = 8 + datalen;
    int     i;

    if (ilen > (int)sizeof(icmp))
        return -1;

    memset(icmp, 0, sizeof(icmp));
    icmp[0] = 8;                            /* Echo Request */
    icmp[1] = 0;
    put_be16(icmp + 4, id);
    put_be16(icmp + 6, seq);
    for (i = 0; i < datalen; i++)
        icmp[8 + i] = (uint8_t)('a' + (i % 26));

    put_be16(icmp + 2, 0);
    put_be16(icmp + 2, vm_ip_checksum(icmp, ilen));

    return build_ipv4(out, 1, src, dst, icmp, ilen);
}

static int build_udp(uint8_t *out, uint32_t src, uint32_t dst,
                     uint16_t sport, uint16_t dport,
                     const char *data)
{
    uint8_t udp[1024];
    int     dlen = (int)strlen(data);
    int     ulen = 8 + dlen;

    put_be16(udp + 0, sport);
    put_be16(udp + 2, dport);
    put_be16(udp + 4, (uint16_t)ulen);
    put_be16(udp + 6, 0);                   /* checksum 省略 */
    memcpy(udp + 8, data, (size_t)dlen);

    return build_ipv4(out, 17, src, dst, udp, ulen);
}

/* ==========================================================================
 * グループ 1: IPv4 ヘルパの正しさ
 * ========================================================================== */
static void test_ip_helpers(void)
{
    uint8_t pkt[128];
    int     tot = 0;
    uint16_t sum;
    char    desc[96];

    group("1) IPv4 チェックサム / ヘッダ検査");

    /*
     * RFC 1071 の教科書例。ヘッダ
     *   45 00 00 73 00 00 40 00 40 11 [b8 61] c0 a8 00 01 c0 a8 00 c7
     * のチェックサムは 0xB861 になることが知られている。
     */
    {
        static const uint8_t ref[20] = {
            0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
            0x40, 0x11, 0x00, 0x00, 0xC0, 0xA8, 0x00, 0x01,
            0xC0, 0xA8, 0x00, 0xC7
        };
        sum = vm_ip_checksum(ref, 20);
        check("RFC1071 の教科書例と一致する",
              sum == 0xB861u,
              "0x%04X (期待 0xB861)", (unsigned)sum);
    }

    /* 自己検算: 正しいヘッダは全域計算で 0 になる */
    tot = build_ipv4(pkt, 1, GUEST_IP, 0x08080808u, NULL, 0);
    check("正しいヘッダは自己検算で 0 になる",
          vm_ip_checksum(pkt, 20) == 0,
          "0x%04X", (unsigned)vm_ip_checksum(pkt, 20));

    check("vm_ip4_check が正常パケットを受理する",
          vm_ip4_check(pkt, tot, &tot) && tot == 20,
          "total_len=%d", tot);

    /* 各種の不正を弾くか */
    {
        uint8_t bad[128];

        memcpy(bad, pkt, 20);
        bad[0] = 0x65;                      /* version 6 に偽装 */
        check("version != 4 を拒否する", !vm_ip4_check(bad, 20, NULL), "");

        memcpy(bad, pkt, 20);
        bad[0] = 0x44;                      /* IHL=4 -> 16 バイト */
        check("IHL < 5 を拒否する", !vm_ip4_check(bad, 20, NULL), "");

        memcpy(bad, pkt, 20);
        put_be16(bad + 2, 19);              /* Total < IHL */
        check("Total Length < ヘッダ長を拒否する",
              !vm_ip4_check(bad, 20, NULL), "");

        memcpy(bad, pkt, 20);
        put_be16(bad + 2, 100);             /* Total > 実長 */
        check("Total Length > 実バッファ長を拒否する",
              !vm_ip4_check(bad, 20, NULL), "");

        memcpy(bad, pkt, 20);
        bad[10] ^= 0xFFu;                   /* チェックサム破壊 */
        check("チェックサム誤りを拒否する",
              !vm_ip4_check(bad, 20, NULL), "");

        check("20 バイト未満を拒否する",
              !vm_ip4_check(pkt, 19, NULL), "");
    }

    tot = build_icmp_echo(pkt, GUEST_IP, 0x08080808u, 1, 1, 32);
    check("describe が人間可読な要約を返す",
          strstr(vm_ip4_describe(pkt, tot, desc, sizeof(desc)),
                 "192.168.99.2 > 8.8.8.8 ICMP") != NULL,
          "%s", desc);
}

/* ==========================================================================
 * グループ 2: 難所 3 — MAC 生成
 * ========================================================================== */
static void test_mac(void)
{
    uint8_t m1[6], m2[6], m3[6];

    group("2) 難所3: 偽 MAC アドレスの生成規則");

    vm_eth_make_mac(GUEST_IP, m1);
    vm_eth_make_mac(GUEST_IP, m2);
    vm_eth_make_mac(HOST_IP,  m3);

    check("マルチキャストビット(bit0)が立っていない",
          (m1[0] & 0x01u) == 0u,
          "第1オクテット=0x%02X", m1[0]);

    check("ローカル管理ビット(bit1)が立っている",
          (m1[0] & 0x02u) != 0u,
          "第1オクテット=0x%02X (IEEE への OUI 登録が不要)", m1[0]);

    check("同じ IP からは常に同じ MAC が出る (決定的)",
          memcmp(m1, m2, 6) == 0,
          "%02X:%02X:%02X:%02X:%02X:%02X",
          m1[0], m1[1], m1[2], m1[3], m1[4], m1[5]);

    check("異なる IP からは異なる MAC が出る",
          memcmp(m1, m3, 6) != 0,
          "guest=..%02X:%02X host=..%02X:%02X",
          m1[4], m1[5], m3[4], m3[5]);

    check("'VM' シグネチャが埋まっている",
          m1[1] == 0x56u && m1[2] == 0x4Du,
          "0x%02X 0x%02X = 'V','M'", m1[1], m1[2]);

    check("全ゼロ MAC を生成しない",
          !(m1[3] == 0 && m1[4] == 0 && m1[5] == 0), "");

    /* seed=0 という退化ケース */
    vm_eth_make_mac(0, m1);
    check("seed=0 でも全ゼロにならない",
          !(m1[3] == 0 && m1[4] == 0 && m1[5] == 0),
          "%02X:%02X:%02X:%02X:%02X:%02X",
          m1[0], m1[1], m1[2], m1[3], m1[4], m1[5]);
}

/* ==========================================================================
 * グループ 3: 難所 1 / 2 — ARP
 * ========================================================================== */
static void test_arp(void)
{
    vm_eth_t e;
    uint8_t  frame[128], out[128];
    int      n, out_len = 0;
    vm_eth_result_t r;

    group("3) 難所1/2: 起動時 ARP と ARP レスポンダ");

    vm_eth_init(&e, GUEST_IP, HOST_IP, NULL);

    /* ---- 起動時 ARP Request の形 ---- */
    n = vm_eth_build_startup_arp(&e, frame, sizeof(frame));
    check("起動時 ARP は 42 バイト (14 + 28)",
          n == VM_ARP_FRAME_LEN, "%d バイト", n);

    check("宛先はブロードキャスト",
          frame[0] == 0xFF && frame[1] == 0xFF && frame[2] == 0xFF &&
          frame[3] == 0xFF && frame[4] == 0xFF && frame[5] == 0xFF, "");

    check("送信元は我々の guest_mac",
          memcmp(frame + 6, e.guest_mac, 6) == 0, "");

    check("EtherType は 0x0806 (ARP)",
          get_be16(frame + 12) == VM_ETH_P_ARP,
          "0x%04X", get_be16(frame + 12));

    check("opcode は Request (1) — slirp は Request で送信元を学習する",
          get_be16(frame + 14 + 6) == VM_ARP_OP_REQUEST,
          "op=%u", get_be16(frame + 14 + 6));

    /*
     * ★重要★ Target IP は **ゲートウェイ** であること。
     * 教科書的な Gratuitous ARP (Target = 自分) にすると
     * libslirp が Reply を返さないため、我々は相手の MAC を
     * 知らないままになる。詳細は vm_eth.h「難所 1 の続き」。
     */
    check("Sender IP は我々 (guest_ip)",
          get_be32(frame + 14 + 14) == GUEST_IP, "");

    check("★Target IP はゲートウェイ (自分自身だと Reply が返らない)",
          get_be32(frame + 14 + 24) == HOST_IP,
          "target=%08X (host_ip=%08X)",
          (unsigned)get_be32(frame + 14 + 24), (unsigned)HOST_IP);

    check("hrd/pro/hln/pln が Ethernet+IPv4",
          get_be16(frame + 14 + 0) == 1 &&
          get_be16(frame + 14 + 2) == 0x0800u &&
          frame[14 + 4] == 6 && frame[14 + 5] == 4, "");

    /* ---- ARP Request を受けて応答する ---- */
    {
        uint8_t gw_mac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };
        uint8_t *b;

        memset(frame, 0, sizeof(frame));
        memset(frame, 0xFF, 6);                     /* 宛先 = broadcast */
        memcpy(frame + 6, gw_mac, 6);
        put_be16(frame + 12, VM_ETH_P_ARP);
        b = frame + 14;
        put_be16(b + 0, 1);
        put_be16(b + 2, 0x0800u);
        b[4] = 6; b[5] = 4;
        put_be16(b + 6, VM_ARP_OP_REQUEST);
        memcpy(b + 8, gw_mac, 6);
        put_be32(b + 14, HOST_IP);
        memset(b + 18, 0, 6);
        put_be32(b + 24, GUEST_IP);                 /* 我々を尋ねている */

        r = vm_eth_decap(&e, frame, VM_ARP_FRAME_LEN,
                         out, sizeof(out), &out_len);

        check("★難所2★ ブロードキャスト宛の ARP を破棄しない",
              r == VM_ETH_REPLY,
              "result=%d out_len=%d", (int)r, out_len);

        check("応答は ARP Reply (opcode 2)",
              out_len == VM_ARP_FRAME_LEN &&
              get_be16(out + 14 + 6) == VM_ARP_OP_REPLY,
              "op=%u", get_be16(out + 14 + 6));

        check("Reply の宛先は尋ねてきた相手のユニキャスト",
              memcmp(out, gw_mac, 6) == 0, "");

        check("Reply の Sender は我々の MAC/IP",
              memcmp(out + 14 + 8, e.guest_mac, 6) == 0 &&
              get_be32(out + 14 + 14) == GUEST_IP, "");

        check("Request から相手の MAC を学習した",
              e.host_mac_known && memcmp(e.host_mac, gw_mac, 6) == 0,
              "%02X:%02X:%02X:%02X:%02X:%02X",
              e.host_mac[0], e.host_mac[1], e.host_mac[2],
              e.host_mac[3], e.host_mac[4], e.host_mac[5]);
    }

    /* ---- 自分以外を尋ねる ARP には答えない ---- */
    {
        put_be32(frame + 14 + 24, 0xC0A863FEu);     /* 192.168.99.254 */
        r = vm_eth_decap(&e, frame, VM_ARP_FRAME_LEN,
                         out, sizeof(out), &out_len);
        check("自分宛でない ARP Request には応答しない",
              r == VM_ETH_DROP, "result=%d", (int)r);
    }

    /* ---- 短すぎる ARP ---- */
    {
        r = vm_eth_decap(&e, frame, 20, out, sizeof(out), &out_len);
        check("短すぎる ARP フレームを安全に破棄する",
              r == VM_ETH_DROP, "");
    }

    check("ARP 統計が整合している",
          e.stats.arp_req_tx == 1 && e.stats.arp_req_rx == 2 &&
          e.stats.arp_rep_tx == 1,
          "req_tx=%llu req_rx=%llu rep_tx=%llu",
          (unsigned long long)e.stats.arp_req_tx,
          (unsigned long long)e.stats.arp_req_rx,
          (unsigned long long)e.stats.arp_rep_tx);
}

/* ==========================================================================
 * グループ 4: 難所 4 — パディング除去 / encap-decap 往復
 * ========================================================================== */
static void test_encap_decap(void)
{
    vm_eth_t e;
    uint8_t  ip[2048], frame[2048], out[2048];
    int      iplen, flen, out_len = 0;
    vm_eth_result_t r;

    group("4) 難所4: 末尾パディングの除去と encap/decap 往復");

    vm_eth_init(&e, GUEST_IP, HOST_IP, NULL);

    /* ---- encap の基本形 ---- */
    iplen = build_icmp_echo(ip, GUEST_IP, 0x08080808u, 0x1234, 1, 32);
    flen  = vm_eth_encap(&e, ip, iplen, frame, sizeof(frame));

    check("encap は 14 バイトのヘッダを前置する",
          flen == iplen + VM_ETH_HLEN,
          "ip=%d frame=%d", iplen, flen);

    check("未学習時の宛先はブロードキャスト (slirp は宛先を見ない)",
          frame[0] == 0xFF && frame[5] == 0xFF, "");

    check("EtherType は 0x0800 (IPv4)",
          get_be16(frame + 12) == VM_ETH_P_IP, "");

    /* ---- ★難所 4 の核心★ 60 バイト未満をパディングして戻す ---- */
    {
        int short_len, padded;

        /* 8 バイトペイロードの ICMP = IP 20 + ICMP 8 = 28 バイト */
        short_len = build_icmp_echo(ip, HOST_IP, GUEST_IP, 0x1234, 2, 0);

        memcpy(frame, e.guest_mac, 6);
        memcpy(frame + 6, e.host_mac, 6);
        put_be16(frame + 12, VM_ETH_P_IP);
        memcpy(frame + 14, ip, (size_t)short_len);

        padded = 14 + short_len;                 /* = 42 */
        /* イーサネット最小長までゼロパディング */
        memset(frame + padded, 0x00, (size_t)(VM_ETH_MIN_FRAME - padded));
        padded = VM_ETH_MIN_FRAME;               /* = 60 */

        r = vm_eth_decap(&e, frame, padded, out, sizeof(out), &out_len);

        check("パディング付きフレームから IP を取り出せる",
              r == VM_ETH_IP, "result=%d", (int)r);

        check("★難所4★ IP 長は Total Length を採用しパディングを含まない",
              out_len == short_len,
              "out_len=%d 期待=%d (frame_len-14=%d だと %d バイト過剰)",
              out_len, short_len, padded - 14, (padded - 14) - short_len);

        check("取り出した IP はバイト単位で一致する",
              memcmp(out, ip, (size_t)short_len) == 0, "");

        check("パディング除去が統計に記録される",
              e.stats.pad_stripped >= 1,
              "pad_stripped=%llu",
              (unsigned long long)e.stats.pad_stripped);
    }

    /* ---- 難所 2: 自分宛でないユニキャストは捨てる ---- */
    {
        uint8_t other[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
        uint64_t before = e.stats.drop_notme;

        iplen = build_icmp_echo(ip, HOST_IP, GUEST_IP, 1, 3, 16);
        memcpy(frame, other, 6);                 /* 宛先が他人 */
        memcpy(frame + 6, e.host_mac, 6);
        put_be16(frame + 12, VM_ETH_P_IP);
        memcpy(frame + 14, ip, (size_t)iplen);

        r = vm_eth_decap(&e, frame, 14 + iplen, out, sizeof(out), &out_len);
        check("自分宛でないユニキャストは破棄する",
              r == VM_ETH_DROP && e.stats.drop_notme == before + 1, "");
    }

    /* ---- マルチキャストは通す (難所 2) ---- */
    {
        memcpy(frame, "\x01\x00\x5E\x00\x00\x01", 6);  /* IPv4 マルチキャスト */
        r = vm_eth_decap(&e, frame, 14 + iplen, out, sizeof(out), &out_len);
        check("マルチキャスト宛は破棄しない",
              r == VM_ETH_IP, "result=%d", (int)r);
    }

    /* ---- 非対応 EtherType ---- */
    {
        memcpy(frame, e.guest_mac, 6);
        put_be16(frame + 12, VM_ETH_P_IPV6);
        r = vm_eth_decap(&e, frame, 14 + iplen, out, sizeof(out), &out_len);
        check("IPv6 (0x86DD) は破棄する (IPV6CP は Reject 済み)",
              r == VM_ETH_DROP && e.stats.drop_proto >= 1, "");
    }

    /* ---- 学習後はユニキャストで送る ---- */
    {
        uint8_t gw[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };
        memcpy(e.host_mac, gw, 6);
        e.host_mac_known = true;

        iplen = build_icmp_echo(ip, GUEST_IP, 0x08080808u, 1, 4, 32);
        flen  = vm_eth_encap(&e, ip, iplen, frame, sizeof(frame));
        check("host_mac 学習後の encap はユニキャスト宛になる",
              flen > 0 && memcmp(frame, gw, 6) == 0, "");
    }

    /* ---- MTU 級のパケットで往復 ---- */
    {
        iplen = build_icmp_echo(ip, GUEST_IP, 0x08080808u, 1, 5, 1472);
        check("1500 バイト (MTU 一杯) の IP を組み立てられる",
              iplen == 1500, "%d バイト", iplen);

        flen = vm_eth_encap(&e, ip, iplen, frame, sizeof(frame));
        /* 宛先を自分にして戻す */
        memcpy(frame, e.guest_mac, 6);
        r = vm_eth_decap(&e, frame, flen, out, sizeof(out), &out_len);

        check("MTU 級 (1500B) の往復でバイト誤りが無い",
              r == VM_ETH_IP && out_len == iplen &&
              memcmp(out, ip, (size_t)iplen) == 0,
              "out_len=%d", out_len);
    }

    /* ---- 出力バッファ不足 ---- */
    {
        uint8_t tiny[32];
        flen = vm_eth_encap(&e, ip, iplen, tiny, sizeof(tiny));
        check("encap は出力バッファ不足を検出する",
              flen == VM_ERR_NOMEM, "rc=%d", flen);
    }

    /* ---- 不正 IP は encap しない ---- */
    {
        ip[10] ^= 0xFFu;                        /* チェックサム破壊 */
        flen = vm_eth_encap(&e, ip, iplen, frame, sizeof(frame));
        check("不正な IP ヘッダは encap を拒否する",
              flen == VM_ERR_INVAL, "rc=%d", flen);
    }
}

/* ==========================================================================
 * グループ 5: NAT (loopback バックエンド)
 * ========================================================================== */
typedef struct {
    uint8_t  pkt[64][2048];
    int      len[64];
    int      n;
} rxq_t;

static void nat_ip_out(void *user, const uint8_t *pkt, int len)
{
    rxq_t *q = (rxq_t *)user;
    if (q->n < 64 && len <= 2048) {
        memcpy(q->pkt[q->n], pkt, (size_t)len);
        q->len[q->n] = len;
        q->n++;
    }
}

static void test_nat_loopback(void)
{
    vm_nat_cfg_t  cfg;
    vm_nat_t     *nat = NULL;
    rxq_t         q;
    uint8_t       ip[2048];
    int           iplen;
    vm_err_t      rc;
    vm_nat_stats_t st;
    char          buf[256];

    group("5) NAT 層 (loopback バックエンド)");

    memset(&q, 0, sizeof(q));
    vm_nat_cfg_defaults(&cfg);

    check("既定値は 192.168.99.0/24",
          cfg.network == 0xC0A86300u && cfg.netmask == 0xFFFFFF00u &&
          cfg.host_ip == HOST_IP && cfg.guest_ip == GUEST_IP,
          "gw=%08X guest=%08X", (unsigned)cfg.host_ip,
          (unsigned)cfg.guest_ip);

    check("既定バックエンドは slirp (仕様書の要求)",
          cfg.backend == VM_NAT_SLIRP, "%s",
          vm_nat_backend_name(cfg.backend));

    check("バックエンド名の解析が双方向で一致する",
          vm_nat_backend_from_string("loopback") == VM_NAT_LOOPBACK &&
          vm_nat_backend_from_string("SLIRP") == VM_NAT_SLIRP &&
          vm_nat_backend_from_string("none") == VM_NAT_NONE &&
          vm_nat_backend_from_string("") == VM_NAT_SLIRP, "");

    check("loopback / none は常に利用可能",
          vm_nat_backend_available(VM_NAT_LOOPBACK) &&
          vm_nat_backend_available(VM_NAT_NONE), "");

    printf("         (参考) slirp バックエンド: %s\n",
           vm_nat_backend_available(VM_NAT_SLIRP)
               ? "利用可能" : "未リンク (VMODEM_HAVE_LIBSLIRP 未定義)");

    /* ---- 生成 ---- */
    cfg.backend = VM_NAT_LOOPBACK;
    rc = vm_nat_create(&nat, &cfg, nat_ip_out, &q);
    check("loopback バックエンドで生成できる",
          rc == VM_OK && nat != NULL, "rc=%s", vm_strerror(rc));
    if (nat == NULL)
        return;

    /* ---- 単調クロック ---- */
    {
        int64_t t0 = vm_nat_now_ns();
        int64_t t1 = vm_nat_now_ns();
        check("★難所6★ クロックは単調増加する (巻き戻らない)",
              t1 >= t0 && t0 > 0,
              "t0=%lld t1=%lld", (long long)t0, (long long)t1);
    }

    /* ---- リンクアップで Gratuitous ARP が飛ぶ ---- */
    rc = vm_nat_link_up(nat);
    check("link_up が成功する", rc == VM_OK, "rc=%s", vm_strerror(rc));

    /*
     * loopback バックエンドは Gratuitous ARP に対して ARP Reply を返す
     * ように作ってある。その Reply は共通部で学習され、以降の encap が
     * ユニキャストになる。= 難所 1 の経路が実際に動いた証拠。
     */
    check("★難所1★ link_up の ARP 往復で相手 MAC を学習できた",
          nat->eth.host_mac_known,
          "host_mac=%02X:%02X:%02X:%02X:%02X:%02X",
          nat->eth.host_mac[0], nat->eth.host_mac[1], nat->eth.host_mac[2],
          nat->eth.host_mac[3], nat->eth.host_mac[4], nat->eth.host_mac[5]);

    check("学習した MAC は我々自身の MAC とは異なる",
          memcmp(nat->eth.host_mac, nat->eth.guest_mac, 6) != 0, "");

    /* ---- ICMP Echo が返ってくる ---- */
    q.n = 0;
    iplen = build_icmp_echo(ip, GUEST_IP, HOST_IP, 0xABCD, 7, 32);
    rc = vm_nat_input_ip(nat, ip, iplen);
    check("ICMP Echo Request を投入できる", rc == VM_OK,
          "rc=%s", vm_strerror(rc));

    check("ICMP Echo Reply が 1 個返ってくる (= ping が通る)",
          q.n == 1, "受信 %d 個", q.n);

    if (q.n == 1) {
        const uint8_t *r = q.pkt[0];
        int rl = q.len[0], rihl, tot = 0;
        bool ipok;

        /*
         * 注: check() の引数評価順序は未規定なので、
         * 条件式の中で tot を書き換えて同じ呼び出しで読むのは
         * バグの温床。先に評価しておく。
         */
        ipok = vm_ip4_check(r, rl, &tot);
        check("Reply は正しい IPv4 パケットである",
              ipok && tot == iplen, "total=%d 期待=%d", tot, iplen);

        rihl = (int)(r[0] & 0x0Fu) * 4;

        check("Reply の送信元/宛先が入れ替わっている",
              get_be32(r + 12) == HOST_IP && get_be32(r + 16) == GUEST_IP,
              "%08X -> %08X",
              (unsigned)get_be32(r + 12), (unsigned)get_be32(r + 16));

        check("ICMP Type が 0 (Echo Reply) になっている",
              r[rihl] == 0, "type=%u", r[rihl]);

        check("★難所8★ ICMP チェックサムが全計算で正しい",
              vm_ip_checksum(r + rihl, rl - rihl) == 0,
              "検算=0x%04X", vm_ip_checksum(r + rihl, rl - rihl));

        check("Identifier / Sequence が保存されている",
              get_be16(r + rihl + 4) == 0xABCD &&
              get_be16(r + rihl + 6) == 7,
              "id=0x%04X seq=%u",
              get_be16(r + rihl + 4), get_be16(r + rihl + 6));

        check("ペイロードがそのまま返る",
              memcmp(r + rihl + 8, ip + 20 + 8, 32) == 0, "");
    }

    /* ---- 短い ICMP (パディングが発生するケース) ---- */
    q.n = 0;
    iplen = build_icmp_echo(ip, GUEST_IP, HOST_IP, 1, 8, 0);
    check("28 バイトの最小 ICMP を組み立てた", iplen == 28,
          "%d バイト", iplen);

    (void)vm_nat_input_ip(nat, ip, iplen);
    check("★難所4★ 28 バイトの Reply もパディング混入なしで返る",
          q.n == 1 && q.len[0] == 28,
          "受信 %d 個 / %d バイト (60 になっていたらパディング混入)",
          q.n, q.n > 0 ? q.len[0] : -1);

    /* ---- UDP 折り返し ---- */
    q.n = 0;
    iplen = build_udp(ip, GUEST_IP, HOST_IP, 12345, 53, "vmodem-udp-test");
    (void)vm_nat_input_ip(nat, ip, iplen);
    check("UDP が折り返される", q.n == 1, "受信 %d 個", q.n);

    if (q.n == 1) {
        const uint8_t *r = q.pkt[0];
        int rihl = (int)(r[0] & 0x0Fu) * 4;
        check("UDP のポートが入れ替わっている",
              get_be16(r + rihl) == 53 && get_be16(r + rihl + 2) == 12345,
              "%u -> %u", get_be16(r + rihl), get_be16(r + rihl + 2));
        check("UDP ペイロードが保存されている",
              memcmp(r + rihl + 8, "vmodem-udp-test", 15) == 0, "");
    }

    /* ---- TCP は loopback では破棄 ---- */
    q.n = 0;
    {
        uint8_t tcp[20];
        memset(tcp, 0, sizeof(tcp));
        put_be16(tcp + 0, 1024);
        put_be16(tcp + 2, 80);
        tcp[12] = 0x50;
        tcp[13] = 0x02;                     /* SYN */
        iplen = build_ipv4(ip, 6, GUEST_IP, 0x08080808u, tcp, 20);
        (void)vm_nat_input_ip(nat, ip, iplen);
    }
    check("TCP は loopback では応答しない (libslirp の担当)",
          q.n == 0, "受信 %d 個", q.n);

    /* ---- poll が CPU を焼かない ---- */
    {
        int next = vm_nat_poll(nat, 50);
        check("★難所7★ poll は 0 を返さない (busy loop 防止)",
              next >= 1, "次回まで %d ms", next);
    }

    /* ---- 統計 ---- */
    vm_nat_get_stats(nat, &st);
    check("送受信統計が計上されている",
          st.tx_pkts == 4 && st.rx_pkts == 3,
          "tx=%llu rx=%llu (ICMP x2 + UDP + TCP / 応答 3)",
          (unsigned long long)st.tx_pkts, (unsigned long long)st.rx_pkts);

    check("ゲストエラーは発生していない",
          st.guest_errors == 0,
          "guest_errors=%llu", (unsigned long long)st.guest_errors);

    printf("         %s\n", vm_nat_status(nat, buf, sizeof(buf)));

    /* ---- リンクダウンで MAC 学習が破棄される ---- */
    vm_nat_link_down(nat);
    check("link_down で MAC 学習が破棄される (次回は再 ARP)",
          !nat->eth.host_mac_known, "");

    vm_nat_destroy(nat);

    /* ---- backend = none ---- */
    {
        vm_nat_t *n2 = NULL;
        cfg.backend = VM_NAT_NONE;
        rc = vm_nat_create(&n2, &cfg, nat_ip_out, &q);
        check("backend=none でも生成できる (PPP 単体試験用)",
              rc == VM_OK && n2 != NULL, "rc=%s", vm_strerror(rc));
        if (n2 != NULL) {
            q.n = 0;
            iplen = build_icmp_echo(ip, GUEST_IP, HOST_IP, 1, 1, 8);
            (void)vm_nat_input_ip(n2, ip, iplen);
            check("backend=none は IP を黙って破棄する", q.n == 0, "");
            vm_nat_destroy(n2);
        }
    }
}

/* ==========================================================================
 * グループ 6: PPP を通した端から端までの疎通
 * ==========================================================================
 * ここが本テストの山場。
 *
 *   [mock RAS] --HDLC--> [vm_ppp] --生IP--> [vm_nat/loopback]
 *                             ^                    |
 *                             +------- 生IP <------+
 *
 * を実際に組み、RAS 側から ICMP Echo Request を送って
 * Echo Reply が HDLC フレームとして戻ってくることを確認する。
 *
 * ★再入の注意★
 *   loopback の send_frame は同期的に recv_frame を呼び、そこから
 *   vm_ppp_send_ip → HDLC encode → write_cb と進む。
 *   write_cb は **キューに詰めるだけ** にしてある。もしここで
 *   vm_ppp_input を呼ぶと test_ppp.c で経験したスタックオーバフローが
 *   再発する。ドレインは必ず下の e2e_pump() から行う。
 * ========================================================================== */
#define E2E_QCAP 65536

typedef struct {
    /* vm_ppp -> RAS 方向のバイト列 */
    uint8_t  q[E2E_QCAP];
    int      q_len, q_pos;

    /* RAS -> vm_ppp 方向のバイト列 */
    uint8_t  tq[E2E_QCAP];
    int      tq_len, tq_pos;

    /* RAS 側の素朴なデフレーマ */
    uint8_t  fbuf[4096];
    int      flen;
    bool     in_frame, escaped;

    uint8_t  next_id;
    bool     lcp_open, ipcp_open;
    uint32_t my_ip;

    /* RAS が受け取った IP パケット */
    uint8_t  rx_ip[2048];
    int      rx_ip_len;
    int      n_ip_rx;

    vm_ppp_t *ppp;
    vm_nat_t *nat;
} e2e_t;

static int e2e_write_cb(void *user, const uint8_t *data, int len)
{
    e2e_t *s = (e2e_t *)user;
    if (s->q_len + len <= (int)sizeof(s->q)) {
        memcpy(s->q + s->q_len, data, (size_t)len);
        s->q_len += len;
    }
    return len;
}

/* vm_ppp が IP を受け取ったら NAT へ流す */
static void e2e_ppp_ip_cb(void *user, const uint8_t *pkt, int len)
{
    e2e_t *s = (e2e_t *)user;
    if (s->nat != NULL)
        (void)vm_nat_input_ip(s->nat, pkt, len);
}

/* NAT が IP を返してきたら PPP へ流す */
static void e2e_nat_ip_cb(void *user, const uint8_t *pkt, int len)
{
    e2e_t *s = (e2e_t *)user;
    if (s->ppp != NULL)
        (void)vm_ppp_send_ip(s->ppp, pkt, len);
}

/* RAS 側の送信: tq に詰めるだけ */
static void e2e_ras_send(e2e_t *s, uint16_t proto,
                         const uint8_t *payload, int len)
{
    uint8_t  f[4096];
    int      n = 0;
    uint16_t fcs = 0xFFFFu;
    int      i;
    uint8_t  hdr[4];
    int      hlen = 0;

    hdr[hlen++] = 0xFF;
    hdr[hlen++] = 0x03;
    hdr[hlen++] = (uint8_t)(proto >> 8);
    hdr[hlen++] = (uint8_t)(proto & 0xFF);

    f[n++] = 0x7E;

    fcs = vm_hdlc_fcs16(fcs, hdr, hlen);
    fcs = vm_hdlc_fcs16(fcs, payload, len);
    fcs = (uint16_t)(~fcs);

    for (i = 0; i < hlen; i++) {
        uint8_t c = hdr[i];
        if (c == 0x7E || c == 0x7D || c < 0x20) {
            f[n++] = 0x7D; f[n++] = (uint8_t)(c ^ 0x20);
        } else f[n++] = c;
    }
    for (i = 0; i < len; i++) {
        uint8_t c = payload[i];
        if (c == 0x7E || c == 0x7D || c < 0x20) {
            f[n++] = 0x7D; f[n++] = (uint8_t)(c ^ 0x20);
        } else f[n++] = c;
    }
    {
        uint8_t fb[2];
        fb[0] = (uint8_t)(fcs & 0xFF);
        fb[1] = (uint8_t)(fcs >> 8);
        for (i = 0; i < 2; i++) {
            uint8_t c = fb[i];
            if (c == 0x7E || c == 0x7D || c < 0x20) {
                f[n++] = 0x7D; f[n++] = (uint8_t)(c ^ 0x20);
            } else f[n++] = c;
        }
    }
    f[n++] = 0x7E;

    if (s->tq_len + n <= (int)sizeof(s->tq)) {
        memcpy(s->tq + s->tq_len, f, (size_t)n);
        s->tq_len += n;
    }
}

static void e2e_ras_ctrl(e2e_t *s, uint16_t proto, uint8_t code, uint8_t id,
                         const uint8_t *data, int dlen)
{
    uint8_t p[1024];
    p[0] = code;
    p[1] = id;
    p[2] = (uint8_t)((dlen + 4) >> 8);
    p[3] = (uint8_t)((dlen + 4) & 0xFF);
    if (dlen > 0)
        memcpy(p + 4, data, (size_t)dlen);
    e2e_ras_send(s, proto, p, dlen + 4);
}

/* RAS が受けたフレームの処理 */
static void e2e_ras_on_frame(e2e_t *s, const uint8_t *f, int n)
{
    uint16_t proto;
    const uint8_t *p = f;

    if (n < 4)
        return;
    if (p[0] == 0xFF && p[1] == 0x03) { p += 2; n -= 2; }
    if ((p[0] & 1) != 0) { proto = p[0]; p += 1; n -= 1; }
    else { proto = (uint16_t)((p[0] << 8) | p[1]); p += 2; n -= 2; }

    if (proto == VM_PPP_PROTO_IP) {
        if (n <= (int)sizeof(s->rx_ip)) {
            memcpy(s->rx_ip, p, (size_t)n);
            s->rx_ip_len = n;
            s->n_ip_rx++;
        }
        return;
    }

    if (n < 4)
        return;

    if (proto == VM_PPP_PROTO_LCP) {
        uint8_t code = p[0], id = p[1];
        if (code == VM_PPP_CONF_REQ) {
            /* 全部受け入れる */
            e2e_ras_ctrl(s, VM_PPP_PROTO_LCP, VM_PPP_CONF_ACK, id,
                         p + 4, n - 4);
            /* 我々の CR も出す (Magic のみ、罠を避けた最小形) */
            if (!s->lcp_open) {
                uint8_t o[6];
                o[0] = VM_LCP_OPT_MAGIC; o[1] = 6;
                put_be32(o + 2, 0xCAFEBABEu);
                e2e_ras_ctrl(s, VM_PPP_PROTO_LCP, VM_PPP_CONF_REQ,
                             s->next_id++, o, 6);
            }
        } else if (code == VM_PPP_CONF_ACK) {
            s->lcp_open = true;
            /* IPCP を開始。IP は 0.0.0.0 で要求 -> Nak を期待 (C-4) */
            {
                uint8_t o[6];
                o[0] = VM_IPCP_OPT_ADDR; o[1] = 6;
                put_be32(o + 2, 0);
                e2e_ras_ctrl(s, VM_PPP_PROTO_IPCP, VM_PPP_CONF_REQ,
                             s->next_id++, o, 6);
            }
        } else if (code == VM_PPP_ECHO_REQ) {
            e2e_ras_ctrl(s, VM_PPP_PROTO_LCP, VM_PPP_ECHO_REP, id,
                         p + 4, n - 4);
        }
        return;
    }

    if (proto == VM_PPP_PROTO_IPCP) {
        uint8_t code = p[0], id = p[1];
        if (code == VM_PPP_CONF_REQ) {
            e2e_ras_ctrl(s, VM_PPP_PROTO_IPCP, VM_PPP_CONF_ACK, id,
                         p + 4, n - 4);
        } else if (code == VM_PPP_CONF_NAK) {
            /* Nak された値を採用して再送 */
            int off = 4;
            while (off + 2 <= n) {
                int ol = p[off + 1];
                if (ol < 2 || off + ol > n) break;
                if (p[off] == VM_IPCP_OPT_ADDR && ol == 6)
                    s->my_ip = get_be32(p + off + 2);
                off += ol;
            }
            {
                uint8_t o[6];
                o[0] = VM_IPCP_OPT_ADDR; o[1] = 6;
                put_be32(o + 2, s->my_ip);
                e2e_ras_ctrl(s, VM_PPP_PROTO_IPCP, VM_PPP_CONF_REQ,
                             s->next_id++, o, 6);
            }
        } else if (code == VM_PPP_CONF_ACK) {
            s->ipcp_open = true;
        }
        return;
    }
}

/* RAS 側のデフレーム。pump からのみ呼ぶ。 */
static void e2e_ras_consume(e2e_t *s)
{
    while (s->q_pos < s->q_len) {
        uint8_t c = s->q[s->q_pos++];

        if (c == 0x7E) {
            if (s->in_frame && s->flen >= 4) {
                /* FCS 検算して本体を渡す */
                if (vm_hdlc_fcs16(0xFFFFu, s->fbuf, s->flen) == 0xF0B8u)
                    e2e_ras_on_frame(s, s->fbuf, s->flen - 2);
            }
            s->flen = 0; s->in_frame = true; s->escaped = false;
            continue;
        }
        if (!s->in_frame) continue;
        if (c == 0x7D) { s->escaped = true; continue; }
        if (s->escaped) { c ^= 0x20; s->escaped = false; }
        if (s->flen < (int)sizeof(s->fbuf)) s->fbuf[s->flen++] = c;
    }
    if (s->q_pos > 0 && s->q_pos == s->q_len) { s->q_pos = 0; s->q_len = 0; }
}

/*
 * トップレベルのポンプ。両方のキューが空になるまで交互にドレインする。
 * これがスタック深さを一定に保ち、実機の非同期性を再現する。
 */
static int e2e_pump(e2e_t *s, uint32_t now)
{
    int rounds = 0;

    while (rounds < 64) {
        bool did = false;

        if (s->tq_pos < s->tq_len) {
            static uint8_t tmp[E2E_QCAP];
            int n = s->tq_len - s->tq_pos;
            memcpy(tmp, s->tq + s->tq_pos, (size_t)n);
            s->tq_pos = 0; s->tq_len = 0;
            vm_ppp_input(s->ppp, tmp, n, now);
            did = true;
        }
        if (s->q_pos < s->q_len) { e2e_ras_consume(s); did = true; }

        if (!did) break;
        rounds++;
    }
    return rounds;
}

static void test_end_to_end(void)
{
    static e2e_t s;
    static vm_ppp_t ppp;
    vm_nat_cfg_t cfg;
    vm_nat_t    *nat = NULL;
    vm_ppp_cfg_t pcfg;
    uint8_t      ip[2048];
    int          iplen;
    uint32_t     now = 1000;
    int          rounds;
    char         buf[256];

    group("6) 端から端まで: RAS -> PPP -> NAT -> PPP -> RAS");

    memset(&s, 0, sizeof(s));
    s.next_id = 1;

    /* NAT を作る */
    vm_nat_cfg_defaults(&cfg);
    cfg.backend = VM_NAT_LOOPBACK;
    if (vm_nat_create(&nat, &cfg, e2e_nat_ip_cb, &s) != VM_OK) {
        check("NAT の生成", false, "");
        return;
    }
    s.nat = nat;

    /* PPP を作る */
    vm_ppp_cfg_defaults(&pcfg);
    pcfg.server_ip    = HOST_IP;
    pcfg.client_ip    = GUEST_IP;
    pcfg.dns1         = 0xC0A86303u;
    pcfg.dns2         = 0;
    pcfg.require_auth = false;

    if (vm_ppp_init(&ppp, &pcfg, e2e_write_cb, &s,
                    e2e_ppp_ip_cb, &s, now) != VM_OK) {
        check("PPP の初期化", false, "");
        vm_nat_destroy(nat);
        return;
    }
    s.ppp = &ppp;

    /* ---- 交渉 ---- */
    (void)vm_ppp_open(&ppp, now);
    rounds = e2e_pump(&s, now);

    check("LCP が収束した", s.lcp_open && ppp.lcp.state == VM_CP_OPENED,
          "ras_open=%d our_state=%d (%d ラウンド)",
          (int)s.lcp_open, (int)ppp.lcp.state, rounds);

    check("IPCP が収束した", s.ipcp_open && ppp.ipcp.state == VM_CP_OPENED,
          "ras_open=%d our_state=%d", (int)s.ipcp_open,
          (int)ppp.ipcp.state);

    check("RAS は Nak 経由で正しい IP を受け取った",
          s.my_ip == GUEST_IP,
          "%u.%u.%u.%u",
          (unsigned)((s.my_ip >> 24) & 0xFF), (unsigned)((s.my_ip >> 16) & 0xFF),
          (unsigned)((s.my_ip >> 8) & 0xFF), (unsigned)(s.my_ip & 0xFF));

    check("PPP が RUNNING になった", vm_ppp_is_running(&ppp),
          "state=%s", vm_ppp_state_name(ppp.state));

    /* ---- リンクアップ (Gratuitous ARP) ---- */
    (void)vm_nat_link_up(nat);
    check("NAT リンクアップで ARP 学習が完了した",
          nat->eth.host_mac_known, "");

    /* ---- ★本番★ RAS から ping を打つ ---- */
    s.n_ip_rx = 0;
    iplen = build_icmp_echo(ip, GUEST_IP, HOST_IP, 0x4321, 100, 32);
    e2e_ras_send(&s, VM_PPP_PROTO_IP, ip, iplen);
    rounds = e2e_pump(&s, now);

    check("★端から端まで★ RAS の ping に Echo Reply が返る",
          s.n_ip_rx == 1,
          "受信 %d 個 (%d ラウンドで収束)", s.n_ip_rx, rounds);

    if (s.n_ip_rx == 1) {
        const uint8_t *r = s.rx_ip;
        int rl = s.rx_ip_len, rihl, tot = 0;
        bool ipok;

        ipok = vm_ip4_check(r, rl, &tot);
        check("戻ってきた IP パケットが健全である",
              ipok && tot == iplen,
              "len=%d total=%d 期待=%d", rl, tot, iplen);

        rihl = (int)(r[0] & 0x0Fu) * 4;

        check("ICMP Echo Reply である",
              r[rihl] == 0 && get_be16(r + rihl + 4) == 0x4321 &&
              get_be16(r + rihl + 6) == 100,
              "type=%u id=0x%04X seq=%u",
              r[rihl], get_be16(r + rihl + 4), get_be16(r + rihl + 6));

        check("HDLC/PPP/Ethernet を 4 回通ってもペイロードが無傷",
              memcmp(r + rihl + 8, ip + 20 + 8, 32) == 0, "");
    }

    /* ---- 連続 ping ---- */
    {
        int i, ok = 0;
        for (i = 0; i < 200; i++) {
            s.n_ip_rx = 0;
            iplen = build_icmp_echo(ip, GUEST_IP, HOST_IP,
                                    0x1000, (uint16_t)i,
                                    32 + (i % 64));
            e2e_ras_send(&s, VM_PPP_PROTO_IP, ip, iplen);
            now += 10;
            (void)e2e_pump(&s, now);

            if (s.n_ip_rx == 1 && s.rx_ip_len == iplen) {
                int rihl = (int)(s.rx_ip[0] & 0x0Fu) * 4;
                if (s.rx_ip[rihl] == 0 &&
                    get_be16(s.rx_ip + rihl + 6) == (uint16_t)i &&
                    vm_ip_checksum(s.rx_ip + rihl,
                                   s.rx_ip_len - rihl) == 0 &&
                    memcmp(s.rx_ip + rihl + 8,
                           ip + 20 + 8, (size_t)(32 + (i % 64))) == 0)
                    ok++;
            }
        }
        check("可変長 200 連続 ping が全て正しく往復する",
              ok == 200, "%d / 200 成功", ok);
    }

    /* ---- 統計の整合 ---- */
    {
        vm_nat_stats_t nst;
        vm_nat_get_stats(nat, &nst);

        check("NAT の送受信数が一致する (取りこぼし無し)",
              nst.tx_pkts == 201 && nst.rx_pkts == 201,
              "tx=%llu rx=%llu",
              (unsigned long long)nst.tx_pkts,
              (unsigned long long)nst.rx_pkts);

        check("NAT の破棄・ゲストエラーが 0",
              nst.drops == 0 && nst.guest_errors == 0,
              "drops=%llu errors=%llu",
              (unsigned long long)nst.drops,
              (unsigned long long)nst.guest_errors);

        check("偽 Ethernet 層に IP 不正が記録されていない",
              nat->eth.stats.drop_badip == 0 &&
              nat->eth.stats.drop_space == 0,
              "badip=%llu space=%llu",
              (unsigned long long)nat->eth.stats.drop_badip,
              (unsigned long long)nat->eth.stats.drop_space);

        check("PPP の HDLC に FCS 誤りが無い",
              ppp.hdlc.stats.rx_fcs_err == 0 &&
              ppp.hdlc.stats.rx_too_long == 0,
              "fcs_err=%llu too_long=%llu frames=%llu",
              (unsigned long long)ppp.hdlc.stats.rx_fcs_err,
              (unsigned long long)ppp.hdlc.stats.rx_too_long,
              (unsigned long long)ppp.hdlc.stats.rx_frames);

        printf("         %s\n", vm_nat_status(nat, buf, sizeof(buf)));
        printf("         %s\n", vm_ppp_status(&ppp, buf, sizeof(buf)));
    }

    /* ---- 切断 ---- */
    vm_ppp_close(&ppp, true, now);
    (void)e2e_pump(&s, now);
    vm_nat_link_down(nat);
    vm_nat_destroy(nat);

    check("切断後も状態が破綻しない",
          !vm_ppp_is_running(&ppp), "state=%s",
          vm_ppp_state_name(ppp.state));
}

/* ==========================================================================
 * main
 * ========================================================================== */
int main(int argc, char **argv)
{
    bool verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    vm_log_init(verbose ? VM_LOG_TRACE : VM_LOG_ERROR, NULL);

    printf("=======================================================\n");
    printf(" VModem NAT / 偽イーサネット層 テスト\n");
    printf("=======================================================\n");

    test_ip_helpers();
    test_mac();
    test_arp();
    test_encap_decap();
    test_nat_loopback();
    test_end_to_end();

    printf("\n=======================================================\n");
    printf(" 結果: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
