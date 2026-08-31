/*
 * vm_eth.h - 偽イーサネット層 / ARP レスポンダ
 *
 * VModem - Windows 用 本物のダイアルアップモデムエミュレータ
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * なぜ「偽イーサネット層」が必要なのか  ★NAT 層 最大の難所★
 * ===========================================================================
 * libslirp は QEMU のユーザモードネットワークを切り出したものであり、
 * その入出力は **イーサネットフレーム** である。
 *
 *   void slirp_input(Slirp *, const uint8_t *pkt, int pkt_len);
 *        └─ pkt は 14 バイトの Ethernet ヘッダから始まる完全なフレーム。
 *           slirp_input() の冒頭は文字通り
 *               if (pkt_len < ETH_HLEN) return;
 *               proto = (pkt[12] << 8) | pkt[13];
 *           であり、L2 が無いバッファを渡すと **無言で捨てられる**。
 *
 *   ssize_t (*send_packet)(const void *buf, size_t len, void *opaque);
 *        └─ こちらも Ethernet フレームが返ってくる。
 *
 * 一方 PPP (RFC 1332 IPCP) が運ぶのは **生の IPv4 パケット** で、L2 ヘッダは
 * 存在しない。PPP は本来 point-to-point なのでアドレス解決の概念が無い。
 *
 * したがって、この 2 つを繋ぐには我々が「存在しないイーサネット LAN」を
 * 演じるほかない。
 *
 *      Windows RAS          我々                        libslirp
 *      ───────────          ────                        ────────
 *      生 IP  ──PPP──>  [+Eth ヘッダ]  ──slirp_input──>  Eth フレーム
 *      生 IP  <─PPP───  [-Eth ヘッダ]  <──send_packet──  Eth フレーム
 *                            │
 *                            └── ARP は我々が代理応答する
 *
 * ===========================================================================
 * 難所 1: ARP を答えないと「最初のパケットが必ず消える」
 * ===========================================================================
 * libslirp の送信経路 if_encap() は、IP パケットを Ethernet で包む前に
 * 宛先 MAC を ARP テーブルから引く:
 *
 *      if (!arp_table_search(slirp, iph->ip_dst.s_addr, ethaddr)) {
 *              ... ARP Request を送出 ...
 *              ... 元のパケットは m_free() で破棄 (or 1 発だけ保留) ...
 *              return false;
 *      }
 *
 * つまり ARP 応答を返さない限り、slirp からゲスト (= RAS 側) へ向かう
 * パケットは **一切届かない**。
 *
 * さらに厄介なのは失敗の見え方である。TCP の最初の SYN/ACK が落ちるので
 *   「PPP は繋がっている、ping も通らない、しかし 3 秒ほど待つと突然動く」
 * という症状になり、PPP 側のバグに見えてしまう。実際には ARP の問題。
 *
 * 対策は 2 段構えにする:
 *
 *   (a) リンクアップ直後に **ARP Request を 1 発送る**。
 *       libslirp の arp_input() は Request を受けた時点で
 *           arp_table_add(slirp, ar_sip, ar_sha);
 *       を実行して送信元を学習する。つまり「聞く」だけで登録される。
 *       これで最初のパケットから ARP 解決済みになり、SYN が落ちない。
 *
 *   (b) それでも slirp が ARP Request を投げてきた場合に備え、
 *       常時 ARP レスポンダを動かす (ARP テーブルには寿命がある)。
 *
 * ---------------------------------------------------------------------------
 * 難所 1 の続き: 「古典的 Gratuitous ARP」では片方向しか解決しない
 * ---------------------------------------------------------------------------
 * 教科書的な Gratuitous ARP は Sender IP == Target IP == 自分 とする。
 * これで相手は送信元を学習するので、上記 (a) の目的は達成される。
 *
 * しかし **こちらは相手の MAC を知らないまま**である。理由は libslirp の
 * arp_input() が Reply を返す条件で、要旨は
 *
 *      if (ar_tip が仮想ネットワーク内 && ar_tip != ゲスト自身) {
 *              ... ARP Reply を返す ...
 *      }
 *
 * となっており、Target IP が「自分自身」の Gratuitous ARP には
 * **応答が返ってこない**。結果として我々の host_mac は未知のままになる。
 *
 * 実害は小さい (libslirp は受信フレームの宛先 MAC を照合しないので
 * ブロードキャストで送り続けても通る) が、無駄が残る。
 *
 * ★より良い設計★
 *   Sender IP = 我々 (guest_ip)、Target IP = **ゲートウェイ (host_ip)**
 *   の普通の ARP Request を送る。すると:
 *
 *     - Sender を見て相手が我々を学習する    ← Gratuitous ARP と同じ効果
 *     - Target が相手自身なので Reply が返る  ← 我々も相手を学習できる
 *
 *   つまり **1 パケットで双方向が解決する**。実機の OS がリンクアップ後に
 *   最初に行うのもこれ (デフォルトゲートウェイの ARP 解決) であり、
 *   挙動として最も自然でもある。
 *
 *   本実装はこちらを採用し、関数名も vm_eth_build_startup_arp() とした。
 *
 * ===========================================================================
 * 難所 2: ブロードキャスト宛を「自分宛でない」と捨ててはいけない
 * ===========================================================================
 * ARP Request の宛先 MAC は ff:ff:ff:ff:ff:ff である。
 * 受信フィルタを素直に
 *      if (memcmp(dst_mac, my_mac, 6) != 0) return DROP;
 * と書くと ARP が全部消え、難所 1 の症状が再発する。
 * ブロードキャストとマルチキャスト (第 1 オクテットの LSB = 1) を
 * 明示的に通す必要がある。
 *
 * ===========================================================================
 * 難所 3: 偽 MAC アドレスの選び方
 * ===========================================================================
 * 適当な値ではいけない。第 1 オクテットの下位 2 ビットに意味がある:
 *
 *      bit0 (0x01) = 1 なら **マルチキャスト**。送信元 MAC に使うと不正。
 *      bit1 (0x02) = 1 なら **ローカル管理アドレス** (IEEE に登録不要)。
 *
 * よって送信元に使える安全な形は「下位 2 ビットが 10b」つまり
 * 第 1 オクテット & 0x03 == 0x02。VModem は 02:56:4D:xx:xx:xx を使う
 * ('V','M' = 0x56,0x4D)。
 *
 * ===========================================================================
 * 難所 4: 末尾パディングを IP パケットに含めてはいけない
 * ===========================================================================
 * イーサネットの最小フレーム長は 60 バイト (FCS 除く) なので、短い IP
 * パケット (例: 20 バイトの ICMP) は 0x00 でパディングされて届くことがある。
 *
 * 素直に「IP 長 = frame_len - 14」と計算すると、このパディングが IP
 * パケットの尻尾にくっついたまま PPP へ流れる。すると:
 *   - IP ヘッダの Total Length と実長が食い違う
 *   - Windows のスタックは寛容なので大抵動いてしまう ← これが最悪
 *   - しかし TCP チェックサム検証や PPP の MRU 計算がずれ、
 *     高負荷時にだけ再送が増えるという再現困難な劣化になる
 *
 * 正解は **IP ヘッダの Total Length を信じる** こと。frame_len は
 * 「それ以上の長さがあるか」の検算にのみ使う。
 * ===========================================================================
 */
#ifndef VMODEM_VM_ETH_H
#define VMODEM_VM_ETH_H

#include <stdint.h>
#include <stdbool.h>
#include "vmodem/vm_types.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * イーサネット定数
 * -------------------------------------------------------------------------- */
#define VM_ETH_ALEN         6
#define VM_ETH_HLEN         14
#define VM_ETH_MIN_FRAME    60      /* FCS を除く最小フレーム長 */

#define VM_ETH_P_IP         0x0800u
#define VM_ETH_P_ARP        0x0806u
#define VM_ETH_P_IPV6       0x86DDu

/* --------------------------------------------------------------------------
 * ARP 定数 (RFC 826)
 * -------------------------------------------------------------------------- */
#define VM_ARP_HRD_ETHER    1
#define VM_ARP_PRO_IP       0x0800u
#define VM_ARP_OP_REQUEST   1
#define VM_ARP_OP_REPLY     2
#define VM_ARP_BODY_LEN     28                              /* 固定長 */
#define VM_ARP_FRAME_LEN    (VM_ETH_HLEN + VM_ARP_BODY_LEN) /* = 42 */

/* IP ヘッダ最小長 */
#define VM_IP4_MIN_HLEN     20

/* --------------------------------------------------------------------------
 * decap の結果
 * -------------------------------------------------------------------------- */
typedef enum {
    VM_ETH_DROP = 0,        /* 破棄した (理由は stats を見る)             */
    VM_ETH_IP,              /* IPv4 パケットを取り出した -> PPP へ流す    */
    VM_ETH_REPLY,           /* 応答フレームを生成した -> NAT へ送り返す   */
    /*
     * 正常に処理したが転送も応答も不要。
     * 典型例は ARP Reply (送信元を学習して終わり)。
     *
     * これを VM_ETH_DROP と区別しないと、正常な ARP 学習が
     * 「破棄パケット」として統計に乗る。drops は障害切り分けの
     * 一次指標なので、正常挙動で増やしてはいけない。
     */
    VM_ETH_CONSUMED
} vm_eth_result_t;

/* --------------------------------------------------------------------------
 * 偽イーサネット層の状態
 * -------------------------------------------------------------------------- */
typedef struct {
    uint64_t frames_in;     /* NAT から受け取ったフレーム数     */
    uint64_t frames_out;    /* NAT へ渡したフレーム数           */
    uint64_t ip_in;         /* 取り出せた IPv4 パケット数       */
    uint64_t ip_out;        /* カプセル化した IPv4 パケット数   */
    uint64_t arp_req_rx;    /* 受けた ARP Request               */
    uint64_t arp_rep_tx;    /* 返した ARP Reply                 */
    uint64_t arp_rep_rx;    /* 受けた ARP Reply (学習のみ)      */
    uint64_t arp_req_tx;    /* 送った起動時 ARP Request         */
    uint64_t drop_short;    /* 長さ不足                         */
    uint64_t drop_proto;    /* 非対応 EtherType (IPv6 など)     */
    uint64_t drop_notme;    /* 自分宛でないユニキャスト         */
    uint64_t drop_badip;    /* IP ヘッダ不正                    */
    uint64_t drop_space;    /* 出力バッファ不足                 */
    uint64_t pad_stripped;  /* パディングを剥がした回数         */
} vm_eth_stats_t;

typedef struct {
    uint8_t  guest_mac[VM_ETH_ALEN]; /* PPP クライアント側を名乗る偽 MAC */
    uint8_t  host_mac[VM_ETH_ALEN];  /* libslirp (ゲートウェイ) 側の MAC */
    bool     host_mac_known;         /* 上を学習済みか                   */
    uint32_t guest_ip;               /* RAS へ払い出した IP (ホストオーダ) */
    uint32_t host_ip;                /* ゲートウェイ IP  (ホストオーダ)   */
    vm_eth_stats_t stats;
} vm_eth_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/*
 * 初期化。
 *   guest_ip  : IPCP で RAS に払い出した IP (ホストバイトオーダ)
 *   host_ip   : ゲートウェイ (我々/slirp) の IP (ホストバイトオーダ)
 *   guest_mac : NULL なら guest_ip から決定的に 02:56:4D:xx:xx:xx を生成
 */
void vm_eth_init(vm_eth_t *e, uint32_t guest_ip, uint32_t host_ip,
                 const uint8_t *guest_mac);

/*
 * ローカル管理 MAC を IP から決定的に生成する。
 * 第 1 オクテットは必ず 0x02 (ユニキャスト かつ ローカル管理)。
 */
void vm_eth_make_mac(uint32_t seed_ip, uint8_t mac[VM_ETH_ALEN]);

/*
 * 生 IPv4 パケット -> イーサネットフレーム。
 * 戻り値: 書き込んだ長さ (= VM_ETH_HLEN + len)、負値はエラー。
 *
 * 宛先 MAC は host_mac_known なら学習値、未学習ならブロードキャスト。
 * (libslirp は自分宛かどうかを見ないので、ブロードキャストでも通る)
 */
int vm_eth_encap(vm_eth_t *e, const uint8_t *ip, int len,
                 uint8_t *out, int out_size);

/*
 * イーサネットフレーム -> 生 IPv4 パケット / ARP 応答。
 *
 * 戻り値 VM_ETH_IP    : out に IPv4 パケット、*out_len に長さ。PPP へ。
 * 戻り値 VM_ETH_REPLY : out に応答フレーム、*out_len に長さ。NAT へ返送。
 * 戻り値 VM_ETH_DROP  : 何もしない。
 */
vm_eth_result_t vm_eth_decap(vm_eth_t *e, const uint8_t *frame, int len,
                             uint8_t *out, int out_size, int *out_len);

/*
 * リンクアップ時の ARP Request を組み立てる (難所 1 の対策 (a))。
 *
 * Sender = 我々 (guest_ip / guest_mac)、Target = ゲートウェイ (host_ip)。
 * これにより 1 パケットで
 *   - 相手が我々を学習する (Gratuitous ARP と同じ効果)
 *   - 相手が Reply を返すので我々も相手を学習できる
 * の双方が達成される。詳細はファイル冒頭「難所 1 の続き」を参照。
 *
 * リンクアップ直後に 1 回、これを slirp_input() に流し込む。
 * 戻り値: 長さ (= VM_ARP_FRAME_LEN)、負値はエラー。
 */
int vm_eth_build_startup_arp(vm_eth_t *e, uint8_t *out, int out_size);

/* --------------------------------------------------------------------------
 * IPv4 ヘルパ
 * -------------------------------------------------------------------------- */

/* RFC 1071 の 16bit 1 の補数和 (ネットワークオーダで返す) */
uint16_t vm_ip_checksum(const uint8_t *data, int len);

/*
 * IPv4 ヘッダの健全性検査。
 *   - version == 4
 *   - IHL >= 5 かつ IHL*4 <= total_length
 *   - total_length >= 20 かつ total_length <= len
 *   - ヘッダチェックサムが正しい
 * 成功時 *total_len に IP ヘッダの Total Length を返す。
 */
bool vm_ip4_check(const uint8_t *pkt, int len, int *total_len);

/* デバッグ用: IPv4 パケットを "192.168.99.2 > 8.8.8.8 ICMP 84B" 形式で表現 */
const char *vm_ip4_describe(const uint8_t *pkt, int len,
                            char *buf, size_t size);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_ETH_H */
