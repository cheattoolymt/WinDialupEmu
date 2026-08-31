/*
 * vm_eth.c - 偽イーサネット層 / ARP レスポンダ 実装
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 設計の背景と 4 つの難所は include/vmodem/vm_eth.h 冒頭を参照。
 */
#include <string.h>
#include <stdio.h>

#include "vmodem/vm_eth.h"
#include "vmodem/vm_log.h"

/* ==========================================================================
 * バイト列ヘルパ
 * ========================================================================== */
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static const uint8_t bcast_mac[VM_ETH_ALEN] =
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool mac_is_bcast(const uint8_t *m)
{
    return (m[0] & m[1] & m[2] & m[3] & m[4] & m[5]) == 0xFFu;
}

/*
 * 難所 3: 第 1 オクテットの LSB がマルチキャストビット。
 * ARP Request のような一斉通知はここが立っている。
 */
static bool mac_is_group(const uint8_t *m)
{
    return (m[0] & 0x01u) != 0u;
}

/* ==========================================================================
 * MAC 生成 (難所 3)
 * ==========================================================================
 * 第 1 オクテットを 0x02 に固定する。
 *   bit0 = 0 -> ユニキャスト  (1 だと送信元 MAC として不正)
 *   bit1 = 1 -> ローカル管理  (IEEE への OUI 登録が不要)
 * 続く 2 バイトは 'V','M' = 0x56,0x4D。下位 3 バイトに IP の下位 3 バイト
 * を入れることで、同じ払い出し IP なら常に同じ MAC になる (決定的)。
 */
void vm_eth_make_mac(uint32_t seed_ip, uint8_t mac[VM_ETH_ALEN])
{
    mac[0] = 0x02u;
    mac[1] = 0x56u;                                  /* 'V' */
    mac[2] = 0x4Du;                                  /* 'M' */
    mac[3] = (uint8_t)((seed_ip >> 16) & 0xFFu);
    mac[4] = (uint8_t)((seed_ip >> 8) & 0xFFu);
    mac[5] = (uint8_t)(seed_ip & 0xFFu);
    /* 万一 seed が 0 でも全ゼロ MAC にはしない */
    if (mac[3] == 0 && mac[4] == 0 && mac[5] == 0)
        mac[5] = 0x01u;
}

void vm_eth_init(vm_eth_t *e, uint32_t guest_ip, uint32_t host_ip,
                 const uint8_t *guest_mac)
{
    if (e == NULL)
        return;

    memset(e, 0, sizeof(*e));
    e->guest_ip = guest_ip;
    e->host_ip  = host_ip;

    if (guest_mac != NULL)
        memcpy(e->guest_mac, guest_mac, VM_ETH_ALEN);
    else
        vm_eth_make_mac(guest_ip, e->guest_mac);

    /*
     * host_mac (= libslirp 側 / ゲートウェイ) は未知。
     * libslirp のデフォルトは 52:55:0a:00:02:02 系だが、バージョンで
     * 変わりうるので決め打ちしない。学習するまではブロードキャストで送る。
     * libslirp の受信側は宛先 MAC を照合しないので、これで問題なく通る。
     */
    e->host_mac_known = false;
    memcpy(e->host_mac, bcast_mac, VM_ETH_ALEN);

    VM_LOGD("eth: guest_mac=%02X:%02X:%02X:%02X:%02X:%02X",
            e->guest_mac[0], e->guest_mac[1], e->guest_mac[2],
            e->guest_mac[3], e->guest_mac[4], e->guest_mac[5]);
}

/* ==========================================================================
 * IPv4 チェックサム (RFC 1071)
 * ==========================================================================
 * 16bit ワードの 1 の補数和の 1 の補数。
 * 桁上がりの畳み込みを 2 回行うのは、1 回目の加算でまた桁上がりが
 * 生じうるため (これを忘れると 65535 近傍でだけ 1 ずれる)。
 */
uint16_t vm_ip_checksum(const uint8_t *data, int len)
{
    uint32_t sum = 0;
    int      i;

    for (i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)get_be16(data + i);

    if (i < len)                                /* 奇数長なら 0 パディング */
        sum += (uint32_t)((uint32_t)data[i] << 8);

    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum = (sum & 0xFFFFu) + (sum >> 16);

    return (uint16_t)(~sum & 0xFFFFu);
}

/* ==========================================================================
 * IPv4 ヘッダ検査 (難所 4 の中核)
 * ========================================================================== */
bool vm_ip4_check(const uint8_t *pkt, int len, int *total_len)
{
    int      ihl;
    int      tot;

    if (pkt == NULL || len < VM_IP4_MIN_HLEN)
        return false;

    if ((pkt[0] >> 4) != 4)                     /* version */
        return false;

    ihl = (int)(pkt[0] & 0x0Fu) * 4;
    if (ihl < VM_IP4_MIN_HLEN || ihl > len)
        return false;

    tot = (int)get_be16(pkt + 2);               /* Total Length */
    if (tot < ihl || tot > len)
        return false;

    /*
     * ヘッダチェックサム。チェックサムフィールドを含めて計算すると
     * 正しいヘッダでは結果が 0 になる (自己検算)。
     * IP ヘッダのチェックサムはヘッダのみを対象とする点に注意
     * (TCP/UDP と違いペイロードは含まない)。
     */
    if (vm_ip_checksum(pkt, ihl) != 0)
        return false;

    if (total_len != NULL)
        *total_len = tot;

    return true;
}

const char *vm_ip4_describe(const uint8_t *pkt, int len,
                            char *buf, size_t size)
{
    static const char *proto_name[] = { NULL };
    const char *pn = "IP";
    char        s[16], d[16];
    uint32_t    src, dst;
    int         tot = 0;

    (void)proto_name;

    if (buf == NULL || size == 0)
        return "";

    if (!vm_ip4_check(pkt, len, &tot)) {
        snprintf(buf, size, "<invalid IPv4, %d bytes>", len);
        return buf;
    }

    src = get_be32(pkt + 12);
    dst = get_be32(pkt + 16);

    switch (pkt[9]) {
    case 1:   pn = "ICMP"; break;
    case 6:   pn = "TCP";  break;
    case 17:  pn = "UDP";  break;
    case 47:  pn = "GRE";  break;
    case 58:  pn = "ICMP6";break;
    default:  pn = "IP";   break;
    }

    snprintf(s, sizeof(s), "%u.%u.%u.%u",
             (unsigned)((src >> 24) & 0xFFu), (unsigned)((src >> 16) & 0xFFu),
             (unsigned)((src >> 8) & 0xFFu), (unsigned)(src & 0xFFu));
    snprintf(d, sizeof(d), "%u.%u.%u.%u",
             (unsigned)((dst >> 24) & 0xFFu), (unsigned)((dst >> 16) & 0xFFu),
             (unsigned)((dst >> 8) & 0xFFu), (unsigned)(dst & 0xFFu));

    snprintf(buf, size, "%s > %s %s %dB", s, d, pn, tot);
    return buf;
}

/* ==========================================================================
 * カプセル化: 生 IP -> Ethernet フレーム
 * ========================================================================== */
int vm_eth_encap(vm_eth_t *e, const uint8_t *ip, int len,
                 uint8_t *out, int out_size)
{
    int tot = 0;

    if (e == NULL || ip == NULL || out == NULL)
        return VM_ERR_INVAL;

    if (!vm_ip4_check(ip, len, &tot)) {
        e->stats.drop_badip++;
        return VM_ERR_INVAL;
    }

    /*
     * 難所 4 の裏返し。PPP から来た IP パケットも、稀に末尾に余分な
     * バイトが付いていることがある (実装によっては PPP のパディング)。
     * ここでも Total Length を信じて切り詰める。
     */
    if (tot < len) {
        e->stats.pad_stripped++;
        len = tot;
    }

    if (out_size < VM_ETH_HLEN + len) {
        e->stats.drop_space++;
        return VM_ERR_NOMEM;
    }

    memcpy(out, e->host_mac, VM_ETH_ALEN);              /* 宛先 */
    memcpy(out + VM_ETH_ALEN, e->guest_mac, VM_ETH_ALEN); /* 送信元 */
    put_be16(out + 12, VM_ETH_P_IP);
    memcpy(out + VM_ETH_HLEN, ip, (size_t)len);

    e->stats.ip_out++;
    e->stats.frames_out++;

    return VM_ETH_HLEN + len;
}

/* ==========================================================================
 * ARP フレーム組み立て
 * ==========================================================================
 * ARP のボディは 28 バイト固定:
 *   [0:2]   Hardware type   = 1 (Ethernet)
 *   [2:4]   Protocol type   = 0x0800 (IPv4)
 *   [4]     Hardware size   = 6
 *   [5]     Protocol size   = 4
 *   [6:8]   Opcode          = 1 (Request) / 2 (Reply)
 *   [8:14]  Sender MAC
 *   [14:18] Sender IP
 *   [18:24] Target MAC
 *   [24:28] Target IP
 */
static int arp_build(uint8_t *out, int out_size,
                     const uint8_t *eth_dst, const uint8_t *eth_src,
                     uint16_t op,
                     const uint8_t *sha, uint32_t sip,
                     const uint8_t *tha, uint32_t tip)
{
    uint8_t *b;

    if (out_size < VM_ARP_FRAME_LEN)
        return VM_ERR_NOMEM;

    memcpy(out, eth_dst, VM_ETH_ALEN);
    memcpy(out + VM_ETH_ALEN, eth_src, VM_ETH_ALEN);
    put_be16(out + 12, VM_ETH_P_ARP);

    b = out + VM_ETH_HLEN;
    put_be16(b + 0, VM_ARP_HRD_ETHER);
    put_be16(b + 2, VM_ARP_PRO_IP);
    b[4] = VM_ETH_ALEN;
    b[5] = 4;
    put_be16(b + 6, op);
    memcpy(b + 8, sha, VM_ETH_ALEN);
    put_be32(b + 14, sip);
    memcpy(b + 18, tha, VM_ETH_ALEN);
    put_be32(b + 24, tip);

    return VM_ARP_FRAME_LEN;
}

/* ==========================================================================
 * 起動時 ARP Request (難所 1 対策 (a))
 * ==========================================================================
 * libslirp の arp_input() は **Request を受けた時点で送信元を学習する**:
 *
 *      case ARPOP_REQUEST:
 *          ...
 *          arp_table_add(slirp, ar_sip, ah->ar_sha);
 *
 * つまり「Reply を待たせる」のではなく「こちらから Request を投げる」だけで
 * slirp の ARP テーブルに guest_ip -> guest_mac が入る。
 * これによりリンクアップ直後の最初のパケットから ARP 解決済みになり、
 * 「最初の TCP SYN が消える」問題が原理的に発生しなくなる。
 *
 * ★ここで Target IP の選び方が効く★
 *
 * 教科書的な Gratuitous ARP は Target IP = Sender IP = 自分とする。
 * これでも上記の「相手に学習させる」目的は達成される。
 * しかし libslirp が Reply を返す条件は概ね
 *
 *      Target IP が仮想ネットワーク内 かつ Target IP != ゲスト自身
 *
 * であり、Target = 自分自身の Gratuitous ARP には **応答が返らない**。
 * よって我々の側は相手の MAC を知らないままになる。
 *
 * そこで Target IP を **ゲートウェイ (host_ip)** にした通常の ARP Request
 * を送る。すると 1 パケットで
 *
 *      Sender を見て相手が我々を学習   (Gratuitous ARP と同じ効果)
 *      Target が相手自身なので Reply が返る (我々も相手を学習)
 *
 * の双方が成立する。実機の OS がリンクアップ後に最初に行う動作
 * (デフォルトゲートウェイの ARP 解決) と同一であり、最も自然でもある。
 */
int vm_eth_build_startup_arp(vm_eth_t *e, uint8_t *out, int out_size)
{
    static const uint8_t zero_mac[VM_ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };
    int n;

    if (e == NULL || out == NULL)
        return VM_ERR_INVAL;

    n = arp_build(out, out_size,
                  bcast_mac, e->guest_mac,
                  VM_ARP_OP_REQUEST,
                  e->guest_mac, e->guest_ip,   /* Sender = 我々           */
                  zero_mac, e->host_ip);       /* Target = ゲートウェイ   */
    if (n < 0) {
        e->stats.drop_space++;
        return n;
    }

    e->stats.arp_req_tx++;
    e->stats.frames_out++;
    VM_LOGD("eth: 起動時 ARP Request を送出 "
            "(相手に我々を学習させ、同時に相手の MAC を得る)");

    return n;
}

/* ==========================================================================
 * ARP 受信処理
 * ========================================================================== */
static vm_eth_result_t arp_handle(vm_eth_t *e, const uint8_t *frame, int len,
                                  uint8_t *out, int out_size, int *out_len)
{
    const uint8_t *b;
    uint16_t       op;
    uint32_t       sip, tip;
    int            n;

    if (len < VM_ARP_FRAME_LEN) {
        e->stats.drop_short++;
        return VM_ETH_DROP;
    }

    b = frame + VM_ETH_HLEN;

    /* Ethernet/IPv4 以外の ARP は扱わない */
    if (get_be16(b + 0) != VM_ARP_HRD_ETHER ||
        get_be16(b + 2) != VM_ARP_PRO_IP ||
        b[4] != VM_ETH_ALEN || b[5] != 4) {
        e->stats.drop_proto++;
        return VM_ETH_DROP;
    }

    op  = get_be16(b + 6);
    sip = get_be32(b + 14);
    tip = get_be32(b + 24);

    /*
     * 送信元を学習する。これで以降の IP 送信でブロードキャストではなく
     * 正しいユニキャスト宛先を使えるようになる。
     */
    if (sip == e->host_ip || !e->host_mac_known) {
        memcpy(e->host_mac, b + 8, VM_ETH_ALEN);
        e->host_mac_known = true;
        VM_LOGT("eth: host_mac を学習 %02X:%02X:%02X:%02X:%02X:%02X",
                e->host_mac[0], e->host_mac[1], e->host_mac[2],
                e->host_mac[3], e->host_mac[4], e->host_mac[5]);
    }

    if (op == VM_ARP_OP_REPLY) {
        e->stats.arp_rep_rx++;
        /*
         * 学習だけして終わり。これは正常処理なので VM_ETH_DROP ではなく
         * VM_ETH_CONSUMED を返す。DROP にすると上位の drops 統計が
         * 増えてしまい、障害切り分けの一次指標が汚れる。
         */
        return VM_ETH_CONSUMED;
    }

    if (op != VM_ARP_OP_REQUEST) {
        e->stats.drop_proto++;
        return VM_ETH_DROP;
    }

    e->stats.arp_req_rx++;

    /*
     * 自分 (guest_ip) を尋ねられた時だけ答える。
     * ここで答えないと slirp からのパケットが永久に届かない (難所 1)。
     */
    if (tip != e->guest_ip) {
        VM_LOGT("eth: ARP Request 対象が我々でない (target=%08X)",
                (unsigned)tip);
        return VM_ETH_DROP;
    }

    n = arp_build(out, out_size,
                  b + 8,            /* 宛先 Ethernet = 相手の Sender MAC */
                  e->guest_mac,
                  VM_ARP_OP_REPLY,
                  e->guest_mac, e->guest_ip,   /* Sender = 我々           */
                  b + 8, sip);                 /* Target = 尋ねてきた相手 */
    if (n < 0) {
        e->stats.drop_space++;
        return VM_ETH_DROP;
    }

    *out_len = n;
    e->stats.arp_rep_tx++;
    e->stats.frames_out++;
    VM_LOGT("eth: ARP Reply を返した");

    return VM_ETH_REPLY;
}

/* ==========================================================================
 * デカプセル化: Ethernet フレーム -> 生 IP / ARP 応答
 * ========================================================================== */
vm_eth_result_t vm_eth_decap(vm_eth_t *e, const uint8_t *frame, int len,
                             uint8_t *out, int out_size, int *out_len)
{
    uint16_t proto;
    int      tot = 0;
    int      payload_len;

    if (e == NULL || frame == NULL || out == NULL || out_len == NULL)
        return VM_ETH_DROP;

    *out_len = 0;

    if (len < VM_ETH_HLEN) {
        e->stats.drop_short++;
        return VM_ETH_DROP;
    }

    e->stats.frames_in++;

    /*
     * 難所 2: 宛先 MAC フィルタ。
     * ブロードキャスト / マルチキャストは必ず通す。ARP Request が
     * ff:ff:ff:ff:ff:ff で来るので、ここで捨てると難所 1 が再発する。
     */
    if (!mac_is_bcast(frame) && !mac_is_group(frame) &&
        memcmp(frame, e->guest_mac, VM_ETH_ALEN) != 0) {
        e->stats.drop_notme++;
        VM_LOGT("eth: 自分宛でないユニキャストを破棄");
        return VM_ETH_DROP;
    }

    proto       = get_be16(frame + 12);
    payload_len = len - VM_ETH_HLEN;

    if (proto == VM_ETH_P_ARP)
        return arp_handle(e, frame, len, out, out_size, out_len);

    if (proto != VM_ETH_P_IP) {
        /* IPv6 などは PPP の IPV6CP を Reject しているので来ないはず */
        e->stats.drop_proto++;
        VM_LOGT("eth: 非対応 EtherType 0x%04X を破棄", (unsigned)proto);
        return VM_ETH_DROP;
    }

    if (!vm_ip4_check(frame + VM_ETH_HLEN, payload_len, &tot)) {
        e->stats.drop_badip++;
        return VM_ETH_DROP;
    }

    /*
     * ★ 難所 4 ★
     * payload_len ではなく **IP ヘッダの Total Length** を採用する。
     * 60 バイト未満のフレームには末尾に 0x00 パディングが付くため、
     * payload_len を使うと余分なバイトが PPP へ流れてしまう。
     */
    if (tot < payload_len) {
        e->stats.pad_stripped++;
        VM_LOGT("eth: 末尾パディング %d バイトを除去", payload_len - tot);
    }

    if (out_size < tot) {
        e->stats.drop_space++;
        return VM_ETH_DROP;
    }

    memcpy(out, frame + VM_ETH_HLEN, (size_t)tot);
    *out_len = tot;
    e->stats.ip_in++;

    return VM_ETH_IP;
}
