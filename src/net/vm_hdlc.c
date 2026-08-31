/*
 * vm_hdlc.c - RFC 1662 非同期 HDLC ライクフレーミング 実装
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_hdlc.h"
#include "vmodem/vm_log.h"

#include <string.h>

/* ===========================================================================
 * FCS-16 テーブル (CRC-16/X-25, 反転多項式 0x8408)
 * ===========================================================================
 * RFC 1662 Appendix C に掲載されている表とビット単位で同一。
 * 実行時生成でも良いが、値が仕様書と一致している事を目で確認できる方が
 * デバッグしやすいので静的テーブルで持つ。
 */
static const uint16_t fcstab[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

uint16_t vm_hdlc_fcs16(uint16_t fcs, const uint8_t *data, int len)
{
    int i;
    for (i = 0; i < len; i++)
        fcs = (uint16_t)((fcs >> 8) ^ fcstab[(fcs ^ data[i]) & 0xFFu]);
    return fcs;
}

bool vm_hdlc_needs_escape(uint32_t accm, uint8_t c)
{
    /*
     * 0x7E (Flag) と 0x7D (Escape) は ACCM に関係なく必ずエスケープする。
     * RFC 1662 4.2: "the transmitter MUST escape 0x7E and 0x7D".
     */
    if (c == VM_HDLC_FLAG || c == VM_HDLC_ESC)
        return true;
    /*
     * 制御文字 0x00-0x1F は ACCM の bit で決まる。
     * 0x20 以上は (0x7E/0x7D を除き) エスケープしない。
     *
     * 注: 実装によっては 0x91/0x93 (XON/XOFF に 8bit 目を立てた物) も
     * エスケープするが、これは ACCM のビット 0x11/0x13 が立っている時に
     * 「上位ビットを無視する回線」向けの追加配慮。com0com は完全に
     * 8bit クリーンなので不要。
     */
    if (c < 0x20u)
        return (accm & (1u << c)) != 0u;
    return false;
}

/* ===========================================================================
 * 初期化
 * =========================================================================== */

void vm_hdlc_init(vm_hdlc_t *h, vm_hdlc_rx_cb cb, void *user)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    /*
     * 交渉前の既定値。RFC 1662 の要求どおり、
     * ACCM は「全制御文字をエスケープ」から始める。
     * LCP 交渉が終わるまで相手が何をエスケープするか分からないため、
     * 保守的に全部エスケープしておくのが唯一安全な初期値。
     */
    h->tx_accm  = 0xFFFFFFFFu;
    h->mru      = VM_HDLC_DEFAULT_MRU;
    h->tx_acfc  = false;
    h->tx_pfc   = false;
    h->cb       = cb;
    h->cb_user  = user;
    vm_hdlc_rx_reset(h);
}

void vm_hdlc_rx_reset(vm_hdlc_t *h)
{
    if (!h) return;
    h->len      = 0;
    h->fcs      = VM_HDLC_FCS_INIT;
    h->in_frame = false;
    h->escaped  = false;
    h->overflow = false;
}

/* ===========================================================================
 * 受信
 * =========================================================================== */

/*
 * 完成したフレーム (h->buf, h->len) を検査して cb に渡す。
 * h->len には FCS 2 バイトが含まれている。
 */
static void deliver_frame(vm_hdlc_t *h)
{
    const uint8_t *p = h->buf;
    int      n = h->len;
    uint16_t proto;

    /*
     * (1) 最小長チェック
     *
     * ACFC/PFC を両方使った最小フレームは Proto(1) + FCS(2) = 3 バイト。
     * 圧縮なしなら Addr(1)+Ctrl(1)+Proto(2)+FCS(2) = 6 バイト。
     * 判定を単純にするため下限は 4 (Proto(1)+Info(1)+FCS(2)) とする。
     */
    if (n < 4) {
        h->stats.rx_too_short++;
        return;
    }

    /*
     * (2) FCS 検算
     *
     * ★ ここが FCS の 3 番目の罠 ★
     * Info と FCS を通して連続で計算すると、正しいフレームなら
     * 結果が必ず 0xF0B8 になる。FCS を切り出して比較する必要はない。
     * 逆に「切り出して比較」を実装すると、送信側の 1 の補数 +
     * LSB-first の扱いを間違えて延々と嵌る。
     */
    if (vm_hdlc_fcs16(VM_HDLC_FCS_INIT, p, n) != VM_HDLC_FCS_GOOD) {
        h->stats.rx_fcs_err++;
        VM_LOGT("hdlc: FCS error, %d bytes dropped", n);
        return;
    }
    n -= 2;   /* FCS を切り落とす */

    /*
     * (3) Address / Control の除去 (ACFC 対応)
     *
     * 相手が ACFC を使ってきたら FF 03 が無い。判定は簡単で、
     * 先頭が FF 03 ならそれは Address/Control。
     *
     * 曖昧にならない根拠: PPP の Protocol フィールドは
     * 「最下位ビットが 1 の奇数バイトで終わり、先頭バイトは偶数」
     * と決まっている (RFC 1661 6.5)。0xFF は奇数なので
     * Protocol の第 1 バイトには成り得ない。よって先頭 FF は必ず Address。
     */
    if (n >= 2 && p[0] == VM_HDLC_ADDR && p[1] == VM_HDLC_CTRL) {
        p += 2;
        n -= 2;
    }

    if (n < 1) {
        h->stats.rx_too_short++;
        return;
    }

    /*
     * (4) Protocol フィールドの取り出し (PFC 対応)
     *
     * 上と同じ規則を使う: Protocol の第 1 バイトが偶数なら 2 バイト形式、
     * 奇数なら PFC で圧縮された 1 バイト形式。
     */
    if ((p[0] & 0x01u) != 0u) {
        proto = p[0];            /* PFC 圧縮: 0x21 → 0x0021 */
        p += 1;
        n -= 1;
    } else {
        if (n < 2) { h->stats.rx_too_short++; return; }
        proto = (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        n -= 2;
    }

    h->stats.rx_frames++;
    if (h->cb)
        h->cb(h->cb_user, proto, p, n);
}

int vm_hdlc_input(vm_hdlc_t *h, const uint8_t *data, int len)
{
    int i;
    int delivered = 0;
    uint64_t frames_before;

    if (!h || !data || len <= 0) return 0;

    h->stats.rx_bytes += (uint64_t)len;
    frames_before = h->stats.rx_frames;

    for (i = 0; i < len; i++) {
        uint8_t c = data[i];

        if (c == VM_HDLC_FLAG) {
            /*
             * フラグ到着。3 つのケースがある:
             *
             *  (a) 直前が 0x7D  → abort シーケンス。フレームを破棄。
             *  (b) len == 0     → 連続フラグ or フレーム開始。何もしない。
             *  (c) len > 0      → フレーム完成。
             *
             * (b) が重要で、PPP 実装は「前後にフラグを付ける」ため
             * 連続フレームでは 7E 7E が並ぶ事も 7E 1 個で共有する事もある。
             * 空フレームを黙って無視すれば両方に対応できる。
             */
            if (h->escaped) {
                h->stats.rx_abort++;
            } else if (h->len > 0) {
                if (h->overflow) {
                    h->stats.rx_too_long++;
                } else {
                    deliver_frame(h);
                }
            }
            h->len      = 0;
            h->escaped  = false;
            h->overflow = false;
            h->in_frame = true;
            continue;
        }

        /*
         * 最初のフラグを見るまでのバイトは全部捨てる。
         *
         * これは実運用で本質的に重要。CONNECT 直後の COM ポートには
         * ハンドシェイクの残骸や RAS が出す "CLIENT" 文字列 (Direct
         * Serial 接続の名残) が残っている事があり、フラグ同期を
         * 取らずに読むとゴミフレームを量産する。
         */
        if (!h->in_frame)
            continue;

        if (h->escaped) {
            h->escaped = false;
            c = (uint8_t)(c ^ VM_HDLC_ESC_XOR);
        } else if (c == VM_HDLC_ESC) {
            h->escaped = true;
            continue;
        }

        /*
         * MRU 超過。ここで in_frame を落とさない事に注意。
         * overflow フラグだけ立てて、次のフラグまで読み飛ばして復帰する。
         * (in_frame を落とすと、次のフラグを「開始」と解釈できず
         *  さらに 1 フレーム損をする)
         */
        if (h->len >= (int)sizeof(h->buf)) {
            h->overflow = true;
            continue;
        }
        if (!h->overflow) {
            h->buf[h->len++] = c;
        }
    }

    delivered = (int)(h->stats.rx_frames - frames_before);
    return delivered;
}

/* ===========================================================================
 * 送信
 * =========================================================================== */

/* 1 バイトを (必要ならエスケープして) 出力バッファに書く */
static int put_esc(uint8_t *out, int out_size, int pos, uint32_t accm, uint8_t c)
{
    if (vm_hdlc_needs_escape(accm, c)) {
        if (pos + 2 > out_size) return -1;
        out[pos++] = VM_HDLC_ESC;
        out[pos++] = (uint8_t)(c ^ VM_HDLC_ESC_XOR);
    } else {
        if (pos + 1 > out_size) return -1;
        out[pos++] = c;
    }
    return pos;
}

int vm_hdlc_encode(vm_hdlc_t *h, uint16_t proto,
                   const uint8_t *payload, int len,
                   uint8_t *out, int out_size)
{
    uint8_t  hdr[4];
    int      hdr_len = 0;
    uint16_t fcs = VM_HDLC_FCS_INIT;
    int      pos = 0;
    int      i;
    uint32_t accm;
    uint8_t  f0, f1;

    if (!h || !out || out_size < 8) return VM_ERR_INVAL;
    if (len < 0 || (payload == NULL && len > 0)) return VM_ERR_INVAL;

    accm = h->tx_accm;

    /*
     * ヘッダ組み立て。
     *
     * ★ ACFC / PFC を使うのは LCP で合意した場合だけ ★
     * しかも重要な例外がある: LCP パケット自身は絶対に圧縮してはいけない
     * (RFC 1661 6.5 / 6.6)。LCP は「圧縮を合意する為のプロトコル」なので、
     * 圧縮された LCP を送ると相手が復号できない可能性がある。
     * ここでは proto == LCP の時に強制的に無圧縮にする。
     */
    if (!(h->tx_acfc && proto != VM_PPP_PROTO_LCP)) {
        hdr[hdr_len++] = VM_HDLC_ADDR;
        hdr[hdr_len++] = VM_HDLC_CTRL;
    }
    if (h->tx_pfc && proto != VM_PPP_PROTO_LCP && (proto & 0xFF00u) == 0u) {
        hdr[hdr_len++] = (uint8_t)(proto & 0xFFu);
    } else {
        hdr[hdr_len++] = (uint8_t)(proto >> 8);
        hdr[hdr_len++] = (uint8_t)(proto & 0xFFu);
    }

    /*
     * FCS は「エスケープ前」の生バイト列に対して計算する。
     * ヘッダ → ペイロードの順。
     */
    fcs = vm_hdlc_fcs16(fcs, hdr, hdr_len);
    if (len > 0)
        fcs = vm_hdlc_fcs16(fcs, payload, len);

    /*
     * ★ 送信 FCS の 2 つの約束 ★
     *   (1) 1 の補数を取る
     *   (2) 下位バイトを先に送る (little-endian)
     * この 2 つが揃うと、受信側で通して計算した時に 0xF0B8 になる。
     */
    fcs = (uint16_t)(~fcs);
    f0 = (uint8_t)(fcs & 0xFFu);
    f1 = (uint8_t)((fcs >> 8) & 0xFFu);

    /* --- 実際のバイト列を組む --- */

    /*
     * 先頭フラグ。
     * RFC 1662 は「送信側は各フレームの前後にフラグを付ける」と規定するが、
     * 連続送信時に前フレームの終端フラグを次フレームの開始フラグとして
     * 共有してよい。ここでは常に前後に付ける (受信側は空フレームを
     * 無視するので相互運用上の問題は無い)。
     */
    out[pos++] = VM_HDLC_FLAG;

    for (i = 0; i < hdr_len; i++) {
        pos = put_esc(out, out_size, pos, accm, hdr[i]);
        if (pos < 0) return VM_ERR_NOMEM;
    }
    for (i = 0; i < len; i++) {
        pos = put_esc(out, out_size, pos, accm, payload[i]);
        if (pos < 0) return VM_ERR_NOMEM;
    }
    pos = put_esc(out, out_size, pos, accm, f0);
    if (pos < 0) return VM_ERR_NOMEM;
    pos = put_esc(out, out_size, pos, accm, f1);
    if (pos < 0) return VM_ERR_NOMEM;

    if (pos + 1 > out_size) return VM_ERR_NOMEM;
    out[pos++] = VM_HDLC_FLAG;

    h->stats.tx_frames++;
    h->stats.tx_bytes += (uint64_t)pos;
    return pos;
}
