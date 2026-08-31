/*
 * vm_hdlc.h - RFC 1662 非同期 HDLC ライクフレーミング (PPP over Serial の下層)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * これは何か
 * ===========================================================================
 * Windows RAS のダイアルアップ接続は、CONNECT 後に COM ポートへ PPP フレームを
 * 流し始める。PPP そのものは「パケットの中身」の規約 (RFC 1661) であって、
 * それを 1 本のバイトストリーム (= シリアル回線) に載せる方法は別の規約
 * RFC 1662 "PPP in HDLC-like Framing" で決まっている。本ファイルはその下層。
 *
 * 上層 (vm_ppp.c) から見ると、この層は次の 2 つだけを提供する:
 *
 *     受信: COM から来た生バイト列 → 完全な PPP パケット (protocol + payload)
 *     送信: PPP パケット (protocol + payload) → COM へ流す生バイト列
 *
 * ===========================================================================
 * フレーム構造
 * ===========================================================================
 *
 *   +------+---------+---------+----------+---------+------+
 *   | 7E   | FF      | 03      | Protocol | Info    | FCS  | 7E
 *   | Flag | Address | Control | 16bit    | 0..MRU  | 16bit| Flag
 *   +------+---------+---------+----------+---------+------+
 *          <--------------- FCS の計算対象 --------->
 *
 *   Flag     = 0x7E 固定。フレームの境界。
 *   Address  = 0xFF (All-Stations)。PPP は点対点なので意味は無く常に 0xFF。
 *   Control  = 0x03 (UI frame, Unnumbered Information)。常に 0x03。
 *   Protocol = 0xC021 LCP / 0xC023 PAP / 0xC223 CHAP / 0x8021 IPCP / 0x0021 IPv4
 *   FCS      = CRC-16 (後述)
 *
 * Address/Control は常に FF 03 なので、LCP で ACFC
 * (Address-and-Control-Field-Compression) を合意すると省略できる。
 * Protocol も上位バイトが 0 の場合 (0x0021 → 0x21) は PFC
 * (Protocol-Field-Compression) で 1 バイトに縮められる。
 *
 * ===========================================================================
 * 難所 1: バイトスタッフィング と ACCM
 * ===========================================================================
 * 0x7E がフレーム境界なので、データ中に 0x7E が出たらそのままでは送れない。
 * そこでエスケープ文字 0x7D を使う:
 *
 *     送信: X をエスケープする必要がある → 0x7D, (X XOR 0x20) の 2 バイトに
 *     受信: 0x7D を見たら次の 1 バイトを XOR 0x20 して戻す
 *
 * 必ずエスケープするのは 0x7E と 0x7D。
 * 加えて「制御文字 0x00〜0x1F のうちどれをエスケープするか」は
 * LCP の ACCM (Async-Control-Character-Map, オプション番号 2) で交渉する
 * 32bit のビットマップで決まる。bit N が 1 なら文字 N をエスケープする。
 *
 *   既定値 0xFFFFFFFF = 0x00〜0x1F を全部エスケープ (交渉前は必ずこれ)
 *   交渉後  0x00000000 = 制御文字はエスケープ不要 (8bit クリーンな回線)
 *
 * ★ ここが一番よく間違える所 ★
 *
 *   ACCM は「相手が送信するときに何をエスケープすべきか」を伝えるオプション。
 *   つまり:
 *
 *     我々の Configure-Request に載せた ACCM
 *         → 「そちらが我々へ送るとき、このマップの文字はエスケープしろ」
 *         → これは我々の *受信* に関する要求
 *
 *     相手 (RAS) の Configure-Request に載っていた ACCM を我々が ACK した
 *         → 「我々が相手へ送るとき、このマップの文字をエスケープする義務」
 *         → これが我々の *送信* マップ (tx_accm)
 *
 *   したがって tx_accm は「相手のリクエストから」取る。逆ではない。
 *
 *   受信側 (rx) は ACCM をまったく参照しない。0x7D を見たら常に外すだけで、
 *   相手が余分にエスケープしてきても正しく復元できる。「受信は常に寛容に」。
 *
 * ===========================================================================
 * 難所 2: FCS-16 (CRC-16/X-25) の細かい約束
 * ===========================================================================
 * 生成多項式 x^16 + x^12 + x^5 + 1 (反転表現 0x8408)、初期値 0xFFFF。
 * PPP の FCS には 3 つの罠がある:
 *
 *   (a) FCS は「エスケープを外した後」のバイト列に対して計算する。
 *       0x7D や XOR 0x20 は FCS の対象外。
 *   (b) 送信時は計算結果を **1 の補数** にしてから、**下位バイト先** で送る。
 *   (c) 受信時は Info + FCS の全体を通して計算した結果が
 *       **0xF0B8 (GOOD_FCS)** になれば OK。これはマジックナンバーで、
 *       (b) の補数と LSB-first がちょうど打ち消し合う結果。
 *       わざわざ FCS を切り出して比較する必要はない。
 *
 * ===========================================================================
 * 難所 3: 受信側の耐障害性
 * ===========================================================================
 * モデム回線はノイズが乗る前提で設計されているので、受信は次を守る:
 *
 *   - 連続する 0x7E (フラグ間が空) は無視する。フラグは複数連続してよい。
 *   - フレーム長が 4 バイト未満 (Addr+Ctrl+FCS を満たさない) なら黙って捨てる。
 *   - 0x7D の直後に 0x7E が来たら "abort" 扱いで、そのフレームを捨てる。
 *   - FCS 不一致は黙って捨てる (エラー応答を返してはいけない)。
 *   - MRU を超えたら捨てる。ただしフラグまで読み飛ばして復帰する。
 *
 * 「黙って捨てる」のが重要で、壊れたフレームに対して何か返すと
 * ノイズでリンクが落ちる。
 * ===========================================================================
 */
#ifndef VMODEM_VM_HDLC_H
#define VMODEM_VM_HDLC_H

#include "vmodem/vm_types.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* --- 定数 ---------------------------------------------------------------- */

#define VM_HDLC_FLAG        0x7Eu   /* フレーム境界                        */
#define VM_HDLC_ESC         0x7Du   /* エスケープ文字                      */
#define VM_HDLC_ESC_XOR     0x20u   /* エスケープ時の XOR 値               */
#define VM_HDLC_ADDR        0xFFu   /* All-Stations                        */
#define VM_HDLC_CTRL        0x03u   /* UI frame                            */

#define VM_HDLC_FCS_INIT    0xFFFFu
#define VM_HDLC_FCS_GOOD    0xF0B8u /* 受信検算のマジックナンバー          */

/* PPP プロトコル番号 (RFC 1700 / IANA) */
#define VM_PPP_PROTO_IP     0x0021u /* IPv4 データ                         */
#define VM_PPP_PROTO_IPV6   0x0057u
#define VM_PPP_PROTO_VJCOMP 0x002Du /* Van Jacobson 圧縮 TCP/IP            */
#define VM_PPP_PROTO_IPCP   0x8021u /* IP Control Protocol                 */
#define VM_PPP_PROTO_IPV6CP 0x8057u
#define VM_PPP_PROTO_CCP    0x80FDu /* Compression Control Protocol        */
#define VM_PPP_PROTO_LCP    0xC021u /* Link Control Protocol               */
#define VM_PPP_PROTO_PAP    0xC023u /* Password Authentication Protocol    */
#define VM_PPP_PROTO_LQR    0xC025u
#define VM_PPP_PROTO_CHAP   0xC223u /* Challenge Handshake Auth Protocol   */
#define VM_PPP_PROTO_CBCP   0xC029u /* MS Callback Control Protocol        */

/* MRU/MTU: PPP の既定は 1500。RAS も 1500 を要求してくる。 */
#define VM_HDLC_DEFAULT_MRU 1500
#define VM_HDLC_MAX_MRU     2048

/*
 * 受信作業バッファ:
 *   Addr(1) + Ctrl(1) + Proto(2) + Info(MRU) + FCS(2) = MRU + 6
 * 余裕を見て +16。
 */
#define VM_HDLC_RXBUF_SIZE  (VM_HDLC_MAX_MRU + 16)

/*
 * 送信作業バッファ:
 *   最悪ケースは全バイトがエスケープ対象 → 2 倍。
 *   さらに前後のフラグで +2。
 */
#define VM_HDLC_TXBUF_SIZE  (VM_HDLC_RXBUF_SIZE * 2 + 8)

/* --- 統計 ---------------------------------------------------------------- */

typedef struct {
    uint64_t rx_bytes;        /* COM から読んだ生バイト数               */
    uint64_t rx_frames;       /* 正常に取り出せたフレーム数             */
    uint64_t rx_fcs_err;      /* FCS 不一致で捨てた数                   */
    uint64_t rx_too_short;    /* 4 バイト未満で捨てた数                 */
    uint64_t rx_too_long;     /* MRU 超過で捨てた数                     */
    uint64_t rx_abort;        /* 0x7D 0x7E の abort シーケンス           */
    uint64_t tx_bytes;        /* COM へ書いた生バイト数 (エスケープ後)  */
    uint64_t tx_frames;       /* 送出フレーム数                         */
} vm_hdlc_stats_t;

/* --- 受信器 -------------------------------------------------------------- */

/*
 * 受信コールバック。
 *   proto   : PPP プロトコル番号 (PFC/ACFC 展開済み)
 *   payload : Info フィールド先頭
 *   len     : Info フィールド長 (FCS は除去済み)
 * 戻り値は無視。payload はコールバック内でのみ有効 (コピーせよ)。
 */
typedef void (*vm_hdlc_rx_cb)(void *user, uint16_t proto,
                              const uint8_t *payload, int len);

typedef struct {
    /* --- 交渉パラメータ --- */
    uint32_t tx_accm;   /* 送信時にエスケープする制御文字マップ            */
    int      mru;       /* 受け入れる最大 Info 長                          */
    bool     tx_acfc;   /* 送信で Address/Control を省略するか             */
    bool     tx_pfc;    /* 送信で Protocol を 1 バイトに縮めるか           */

    /* --- 受信状態機械 --- */
    uint8_t  buf[VM_HDLC_RXBUF_SIZE];
    int      len;
    uint16_t fcs;
    bool     in_frame;  /* 最初のフラグを見たか                            */
    bool     escaped;   /* 直前が 0x7D だったか                            */
    bool     overflow;  /* このフレームは MRU 超過で捨てる                 */

    vm_hdlc_rx_cb cb;
    void         *cb_user;

    vm_hdlc_stats_t stats;
} vm_hdlc_t;

/* 初期化。交渉前の既定値 (ACCM=0xFFFFFFFF, MRU=1500, 圧縮なし) になる。 */
void vm_hdlc_init(vm_hdlc_t *h, vm_hdlc_rx_cb cb, void *user);

/* 受信状態機械だけをリセット (交渉パラメータと統計は保持)。 */
void vm_hdlc_rx_reset(vm_hdlc_t *h);

/*
 * COM から読んだ生バイト列を投入する。
 * 完全なフレームが得られる度に cb が呼ばれる (1 回の呼び出しで複数回あり)。
 * 戻り値: 取り出せたフレーム数。
 */
int vm_hdlc_input(vm_hdlc_t *h, const uint8_t *data, int len);

/* --- 送信器 -------------------------------------------------------------- */

/*
 * PPP パケットを HDLC フレームにエンコードする。
 *
 *   out/out_size : 出力先。VM_HDLC_TXBUF_SIZE あれば絶対に足りる。
 *   戻り値       : 書いたバイト数。out_size 不足なら負値 (VM_ERR_NOMEM)。
 *
 * この関数は COM への書き込みを行わない (純関数)。呼び出し側が
 * vm_serial_write() する。テストしやすくするための分離。
 */
int vm_hdlc_encode(vm_hdlc_t *h, uint16_t proto,
                   const uint8_t *payload, int len,
                   uint8_t *out, int out_size);

/* --- FCS ヘルパ (テストからも使う) --------------------------------------- */

uint16_t vm_hdlc_fcs16(uint16_t fcs, const uint8_t *data, int len);

/*
 * ACCM のビットを見て「この文字はエスケープが必要か」を返す。
 * 0x7E / 0x7D は accm に関わらず常に true。
 */
bool vm_hdlc_needs_escape(uint32_t accm, uint8_t c);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_HDLC_H */
