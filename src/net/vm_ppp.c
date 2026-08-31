/*
 * vm_ppp.c - PPP サーバ実装 (LCP / PAP / IPCP)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 設計思想と難所の解説はすべて include/vmodem/vm_ppp.h に書いた。
 * ここは「その通りに書いた実装」なので、コメントは
 * ヘッダで説明した規則が *どの行に対応するか* を示す事に集中する。
 */
#include "vmodem/vm_ppp.h"
#include "vmodem/vm_log.h"

#include <string.h>
#include <stdio.h>

/* ===========================================================================
 * 小道具
 * =========================================================================== */

static const char *const state_names[VM_PPP__COUNT] = {
    "DEAD", "ESTABLISH", "AUTHENTICATE", "NETWORK", "RUNNING", "TERMINATE"
};

const char *vm_ppp_state_name(vm_ppp_state_t s)
{
    if ((int)s < 0 || (int)s >= VM_PPP__COUNT) return "?";
    return state_names[s];
}

static const char *cp_state_name(vm_cp_state_t s)
{
    switch (s) {
    case VM_CP_INITIAL:  return "INITIAL";
    case VM_CP_REQ_SENT: return "REQ_SENT";
    case VM_CP_ACK_RCVD: return "ACK_RCVD";
    case VM_CP_ACK_SENT: return "ACK_SENT";
    case VM_CP_OPENED:   return "OPENED";
    }
    return "?";
}

bool vm_ipv4_parse(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int oct = 0, digits = 0, cur = 0;

    if (!s || !out) return false;
    for (;;) {
        char c = *s++;
        if (c >= '0' && c <= '9') {
            if (++digits > 3) return false;
            cur = cur * 10 + (c - '0');
            if (cur > 255) return false;
        } else if (c == '.' || c == '\0') {
            if (digits == 0) return false;
            v = (v << 8) | (uint32_t)cur;
            oct++;
            cur = 0; digits = 0;
            if (c == '\0') break;
            if (oct >= 4) return false;
        } else {
            return false;
        }
    }
    if (oct != 4) return false;
    *out = v;
    return true;
}

const char *vm_ipv4_str(uint32_t ip, char *buf, size_t size)
{
    if (!buf || size < 8) return "?";
    snprintf(buf, size, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFFu), (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),  (unsigned)(ip & 0xFFu));
    return buf;
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFFu);
}
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v & 0xFFu);
}
static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

void vm_ppp_cfg_defaults(vm_ppp_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    vm_ipv4_parse("192.168.99.1", &cfg->server_ip);
    vm_ipv4_parse("192.168.99.2", &cfg->client_ip);
    vm_ipv4_parse("8.8.8.8",      &cfg->dns1);
    vm_ipv4_parse("8.8.4.4",      &cfg->dns2);
    cfg->require_auth = false;
    cfg->mru = VM_HDLC_DEFAULT_MRU;
}

/* ===========================================================================
 * 送信プリミティブ
 * =========================================================================== */

/*
 * PPP パケット (code/id/data) を HDLC でくるんで write_cb へ渡す。
 *
 * PPP 制御パケットの共通ヘッダ (RFC 1661 5):
 *   +------+------------+--------+----------+
 *   | Code | Identifier | Length | Data ... |
 *   |  1   |     1      |   2    |          |
 *   +------+------------+--------+----------+
 * Length は Code/Id/Length 自身を含む。ここを含めない実装ミスが定番。
 */
static int send_pkt(vm_ppp_t *p, uint16_t proto, uint8_t code, uint8_t id,
                    const uint8_t *data, int dlen)
{
    int n, total;

    if (dlen < 0 || dlen > (int)sizeof(p->pbuf) - 4) return VM_ERR_INVAL;

    total = 4 + dlen;
    p->pbuf[0] = code;
    p->pbuf[1] = id;
    put_be16(p->pbuf + 2, (uint16_t)total);   /* ← Length は自分を含む */
    if (dlen > 0 && data) memcpy(p->pbuf + 4, data, (size_t)dlen);

    n = vm_hdlc_encode(&p->hdlc, proto, p->pbuf, total,
                       p->wbuf, (int)sizeof(p->wbuf));
    if (n < 0) return n;

    switch (proto) {
    case VM_PPP_PROTO_LCP:  p->stats.lcp_tx++;  break;
    case VM_PPP_PROTO_IPCP: p->stats.ipcp_tx++; break;
    case VM_PPP_PROTO_PAP:  p->stats.pap_tx++;  break;
    default: break;
    }

    VM_LOGT("ppp tx: proto=%04X code=%u id=%u len=%d (%d raw)",
            (unsigned)proto, code, id, total, n);

    if (p->write_cb)
        return p->write_cb(p->write_user, p->wbuf, n);
    return n;
}

/* ===========================================================================
 * 我々の Configure-Request の組み立て
 * =========================================================================== */

/*
 * ★ 我々の CR に何を載せるか = 「我々が受信する時のパラメータ」★
 *
 * 載せるもの:
 *   MRU         我々が受け取れる最大長
 *   ACCM        相手が送信時にエスケープすべき文字マップ
 *   Magic       ループ検出用
 *   Auth        認証を要求する場合のみ (我々がサーバなので我々が要求する)
 *
 * 載せないもの:
 *   PFC / ACFC  「相手に圧縮させる」オプション。受信の実装は
 *               既に対応済み (deliver_frame の ACFC/PFC 判定) だが、
 *               敢えて要求しない。理由は下記。
 *
 * ★ なぜ PFC/ACFC を要求しないのか ★
 *   帯域の節約量は 1 フレームあたり 3 バイト。33.6kbps でも
 *   1500 バイトフレームに対して 0.2% で無意味。
 *   一方、圧縮を合意すると「LCP は圧縮しない」「Reject された時に
 *   途中から無圧縮に戻す」といった状態管理が増え、バグの温床になる。
 *   相手 (RAS) が要求してきたら Ack して *我々の送信側で* 使うのは
 *   問題ないが、我々から要求はしない。非対称でよい。
 *
 * ★ ACCM に何を入れるか ★
 *   com0com は完全に 8bit クリーン (fNull=FALSE, XON/XOFF off を
 *   vm_serial_win32.c で設定済み) なので 0x00000000 を要求できる。
 *   これで制御文字のエスケープが消え、PPP のスループットが上がる。
 *   もし RAS が Nak で 0xFFFFFFFF を返してきたら素直に受け入れる
 *   (受信側は ACCM を参照しないので実は何でも動く)。
 */
static void lcp_send_conf_req(vm_ppp_t *p)
{
    uint8_t o[64];
    int n = 0;

    /* MRU */
    o[n++] = VM_LCP_OPT_MRU; o[n++] = 4;
    put_be16(o + n, (uint16_t)p->cfg.mru); n += 2;

    /* ACCM: 8bit クリーンなのでエスケープ不要を要求 */
    o[n++] = VM_LCP_OPT_ACCM; o[n++] = 6;
    put_be32(o + n, 0x00000000u); n += 4;

    /* 認証要求 (PAP) */
    if (p->cfg.require_auth) {
        o[n++] = VM_LCP_OPT_AUTH; o[n++] = 4;
        put_be16(o + n, VM_PPP_PROTO_PAP); n += 2;
    }

    /* Magic-Number (難所 C-7) */
    o[n++] = VM_LCP_OPT_MAGIC; o[n++] = 6;
    put_be32(o + n, p->our_magic); n += 4;

    p->lcp.req_id = p->lcp.next_id++;
    p->lcp.timer_ms = p->now_ms + VM_PPP_RESTART_MS;
    send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_CONF_REQ, p->lcp.req_id, o, n);
}

/*
 * IPCP の我々の CR。
 * 我々が要求するのは自分の IP アドレスだけ。
 * DNS を我々が要求する必要はない (我々は DNS を使わない)。
 */
static void ipcp_send_conf_req(vm_ppp_t *p)
{
    uint8_t o[16];
    int n = 0;

    o[n++] = VM_IPCP_OPT_ADDR; o[n++] = 6;
    put_be32(o + n, p->cfg.server_ip); n += 4;

    p->ipcp.req_id = p->ipcp.next_id++;
    p->ipcp.timer_ms = p->now_ms + VM_PPP_RESTART_MS;
    send_pkt(p, VM_PPP_PROTO_IPCP, VM_PPP_CONF_REQ, p->ipcp.req_id, o, n);
}

/* ===========================================================================
 * 状態遷移
 * =========================================================================== */

static void cp_reset(vm_cp_t *cp)
{
    cp->state = VM_CP_INITIAL;
    cp->restart_count = VM_PPP_MAX_CONFIGURE;
    cp->failure_count = 0;
    cp->timer_ms = 0;
}

static void enter_network(vm_ppp_t *p)
{
    if (p->state == VM_PPP_NETWORK || p->state == VM_PPP_RUNNING) return;
    VM_LOGI("ppp: -> NETWORK (IPCP 交渉開始)");
    p->state = VM_PPP_NETWORK;
    cp_reset(&p->ipcp);
    ipcp_send_conf_req(p);
    p->ipcp.state = VM_CP_REQ_SENT;
}

/*
 * LCP が Opened になった直後の分岐。
 * 認証が必要なら AUTHENTICATE、不要なら即 NETWORK。
 */
static void lcp_opened(vm_ppp_t *p)
{
    /*
     * ★ 交渉結果を HDLC 層に反映する唯一のタイミング ★
     *
     * 難所: tx_accm は「相手の CR に載っていた値」から取る。
     * 我々の CR に載せた値ではない (ヘッダの難所 1 を参照)。
     * peer_accm は handle_conf_req() で相手の CR から拾ってある。
     */
    p->hdlc.tx_accm = p->peer_accm;
    p->hdlc.tx_pfc  = p->peer_wants_pfc;
    p->hdlc.tx_acfc = p->peer_wants_acfc;

    VM_LOGI("ppp: LCP opened (tx_accm=%08X pfc=%d acfc=%d peer_mru=%d)",
            (unsigned)p->hdlc.tx_accm, (int)p->hdlc.tx_pfc,
            (int)p->hdlc.tx_acfc, p->peer_mru);

    p->echo_next_ms = p->now_ms + VM_PPP_ECHO_INTERVAL_MS;
    p->echo_missed  = 0;

    if (p->cfg.require_auth && !p->auth_ok) {
        VM_LOGI("ppp: -> AUTHENTICATE (PAP 待ち)");
        p->state = VM_PPP_AUTHENTICATE;
        p->auth_deadline_ms = p->now_ms + VM_PPP_AUTH_TIMEOUT_MS;
    } else {
        enter_network(p);
    }
}

static void ipcp_opened(vm_ppp_t *p)
{
    char a[16], b[16];
    p->ipcp_up = true;
    p->state = VM_PPP_RUNNING;
    VM_LOGI("ppp: -> RUNNING  server=%s client=%s",
            vm_ipv4_str(p->cfg.server_ip, a, sizeof(a)),
            vm_ipv4_str(p->assigned_ip ? p->assigned_ip : p->cfg.client_ip,
                        b, sizeof(b)));
}

/*
 * 「両方揃って初めて Opened」の判定 (難所 A)。
 * cp_state の遷移を 1 か所に集約しておくと、REQ_SENT→ACK_SENT→OPENED と
 * REQ_SENT→ACK_RCVD→OPENED のどちらの順序でも正しく動く。
 */
static void cp_got_ack_sent(vm_ppp_t *p, vm_cp_t *cp, bool is_lcp)
{
    switch (cp->state) {
    case VM_CP_INITIAL:
    case VM_CP_REQ_SENT:
    case VM_CP_ACK_SENT:
        cp->state = VM_CP_ACK_SENT;
        break;
    case VM_CP_ACK_RCVD:
    case VM_CP_OPENED:
        cp->state = VM_CP_OPENED;
        cp->timer_ms = 0;
        if (is_lcp) lcp_opened(p); else ipcp_opened(p);
        break;
    }
}

static void cp_got_ack_rcvd(vm_ppp_t *p, vm_cp_t *cp, bool is_lcp)
{
    switch (cp->state) {
    case VM_CP_INITIAL:
    case VM_CP_REQ_SENT:
    case VM_CP_ACK_RCVD:
        cp->state = VM_CP_ACK_RCVD;
        cp->timer_ms = 0;   /* 我々の CR は通ったので再送不要 */
        break;
    case VM_CP_ACK_SENT:
    case VM_CP_OPENED:
        cp->state = VM_CP_OPENED;
        cp->timer_ms = 0;
        if (is_lcp) lcp_opened(p); else ipcp_opened(p);
        break;
    }
}

/* ===========================================================================
 * オプション走査ヘルパ
 * ===========================================================================
 * PPP のオプションは TLV: [type(1)][length(1)][value(length-2)]
 * length は type/length 自身を含む。length < 2 は不正で、
 * これを検査しないと無限ループする (定番の脆弱性)。
 */
typedef struct {
    const uint8_t *p;
    int            remain;
} opt_iter_t;

static void opt_iter_init(opt_iter_t *it, const uint8_t *data, int len)
{
    it->p = data; it->remain = len;
}

/* 次のオプションを返す。false で終端 or 不正。 */
static bool opt_next(opt_iter_t *it, uint8_t *type,
                     const uint8_t **val, int *vlen)
{
    int l;
    if (it->remain < 2) return false;
    l = it->p[1];
    if (l < 2 || l > it->remain) return false;   /* ← 無限ループ防止 */
    *type = it->p[0];
    *val  = it->p + 2;
    *vlen = l - 2;
    it->p      += l;
    it->remain -= l;
    return true;
}

/* Rej/Nak 応答バッファに 1 オプション丸ごとコピー */
static int opt_copy(uint8_t *dst, int dpos, int dsize,
                    uint8_t type, const uint8_t *val, int vlen)
{
    if (dpos + 2 + vlen > dsize) return dpos;   /* 溢れたら黙って落とす */
    dst[dpos++] = type;
    dst[dpos++] = (uint8_t)(2 + vlen);
    if (vlen > 0) memcpy(dst + dpos, val, (size_t)vlen);
    return dpos + vlen;
}

/* ===========================================================================
 * LCP: 相手の Configure-Request 処理
 * =========================================================================== */

static void lcp_handle_conf_req(vm_ppp_t *p, uint8_t id,
                                const uint8_t *data, int len)
{
    uint8_t  rej[256], nak[256];
    int      rejn = 0, nakn = 0;
    opt_iter_t it;
    uint8_t  t;
    const uint8_t *v;
    int      vl;

    /* 交渉結果の一時受け皿。Ack を返す時にだけ確定させる。 */
    uint32_t new_accm = 0xFFFFFFFFu;   /* 相手が ACCM を送ってこなければ既定 */
    int      new_mru  = VM_HDLC_DEFAULT_MRU;
    bool     new_pfc = false, new_acfc = false;
    uint32_t new_peer_magic = 0;
    bool     magic_seen = false;

    opt_iter_init(&it, data, len);
    while (opt_next(&it, &t, &v, &vl)) {
        switch (t) {

        case VM_LCP_OPT_MRU:
            if (vl != 2) { rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl); break; }
            new_mru = get_be16(v);
            /*
             * 相手の MRU が極端に小さい/大きい場合だけ Nak する。
             * RFC 1661 は最小 MRU を 1500 未満に下げる要求も許すが、
             * 実用上 128 未満は誤りとみなす。
             */
            if (new_mru < 128 || new_mru > VM_HDLC_MAX_MRU) {
                uint8_t fix[2];
                put_be16(fix, VM_HDLC_DEFAULT_MRU);
                nakn = opt_copy(nak, nakn, sizeof(nak), t, fix, 2);
                new_mru = VM_HDLC_DEFAULT_MRU;
            }
            break;

        case VM_LCP_OPT_ACCM:
            if (vl != 4) { rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl); break; }
            /*
             * ★ 相手が何を要求してきても Ack する ★
             * これは「我々が送信時にエスケープする文字」の指定なので、
             * 相手の言う通りにするのが常に可能かつ安全。
             * 交渉で揉める理由が無い。
             */
            new_accm = get_be32(v);
            break;

        case VM_LCP_OPT_MAGIC:
            if (vl != 4) { rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl); break; }
            new_peer_magic = get_be32(v);
            magic_seen = true;
            /*
             * ★ ループバック検出 (RFC 1661 6.4) ★
             * 相手の Magic が我々のものと一致 = 自分の送信が
             * そのまま返ってきている = 回線がループしている。
             * com0com のペア設定を間違えて CNCB0 を自分で開いた場合に
             * 実際に起きる。Nak で別の値を要求する。
             */
            if (magic_seen && new_peer_magic == p->our_magic) {
                uint8_t fix[4];
                p->our_magic = p->our_magic * 1103515245u + 12345u;
                if (p->our_magic == 0) p->our_magic = 0xA5A5A5A5u;
                put_be32(fix, p->our_magic);
                nakn = opt_copy(nak, nakn, sizeof(nak), t, fix, 4);
                VM_LOGW("ppp: LCP magic collision -> 回線ループの疑い");
            }
            break;

        case VM_LCP_OPT_PFC:
            /* 相手が「Protocol を圧縮して送ってよい」と言っている → 受ける */
            if (vl != 0) { rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl); break; }
            new_pfc = true;
            break;

        case VM_LCP_OPT_ACFC:
            if (vl != 0) { rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl); break; }
            new_acfc = true;
            break;

        case VM_LCP_OPT_AUTH:
            /*
             * ★ 相手 (RAS) が我々を認証しようとしてくるケース ★
             * ダイアルアップでは通常起きないが、相互認証設定だと来る。
             * 我々はクライアント側の認証情報を持たないので Reject。
             * Reject すれば RAS は片方向認証に落ちる。
             */
            rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl);
            break;

        case VM_LCP_OPT_CALLBACK:   /* 難所 C-1 */
        case VM_LCP_OPT_MRRU:       /* Multilink PPP: 実装しない */
        case VM_LCP_OPT_QUALITY:    /* LQR: 実装しない */
        default:
            /*
             * 知らないオプションは必ず Reject。
             * 無視すると相手は「Ack された」と誤解しない (Ack に
             * 載っていないので) が、CR を再送し続ける事になる。
             */
            rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl);
            break;
        }
    }

    /*
     * ★ 難所 B の順序ルール ★
     * Reject があれば Reject だけを返す。次に Nak。最後に Ack。
     * 混ぜない、順序を変えない。
     */
    if (rejn > 0) {
        VM_LOGD("ppp: LCP Conf-Rej (%d bytes of options)", rejn);
        p->lcp.failure_count++;
        send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_CONF_REJ, id, rej, rejn);
        return;
    }
    if (nakn > 0) {
        VM_LOGD("ppp: LCP Conf-Nak (%d bytes of options)", nakn);
        p->lcp.failure_count++;
        send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_CONF_NAK, id, nak, nakn);
        return;
    }

    /* --- 全部受け入れ: ここで初めて交渉結果を確定 --- */
    p->peer_accm       = new_accm;
    p->peer_mru        = new_mru;
    p->peer_wants_pfc  = new_pfc;
    p->peer_wants_acfc = new_acfc;
    p->peer_magic      = new_peer_magic;

    send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_CONF_ACK, id, data, len);
    cp_got_ack_sent(p, &p->lcp, true);
}

/*
 * 我々の CR に対する Nak / Rej の処理。
 *
 * 我々が載せているのは MRU / ACCM / Auth / Magic の 4 つだけなので、
 * 対応は単純にできる:
 *
 *   MRU を Nak     → 相手の言う値を採用 (ただし範囲チェック)
 *   ACCM を Nak    → 相手の言う値を採用 (保守的な方へ倒す)
 *   Magic を Nak   → 別の値を生成
 *   Auth を Nak    → 相手が PAP を嫌がっている。CHAP を提案されても
 *                     実装しないので認証を諦める (require_auth を落とす)
 *   Auth を Rej    → 認証そのものを拒否 → 認証を諦める
 *   ACCM/MRU を Rej→ 既定値に戻して再送
 *
 * 「認証を諦める」を許容するのは、我々が仮想 ISP であり
 * 認証がセキュリティ目的ではないため。実 ISP ならここで切断する。
 */
static void lcp_handle_conf_nak_rej(vm_ppp_t *p, bool is_rej,
                                    const uint8_t *data, int len)
{
    opt_iter_t it;
    uint8_t t;
    const uint8_t *v;
    int vl;

    p->lcp.failure_count++;
    if (p->lcp.failure_count > VM_PPP_MAX_FAILURE) {
        VM_LOGE("ppp: LCP Max-Failure 到達 → 切断");
        vm_ppp_close(p, true, p->now_ms);
        return;
    }

    opt_iter_init(&it, data, len);
    while (opt_next(&it, &t, &v, &vl)) {
        switch (t) {
        case VM_LCP_OPT_MRU:
            if (!is_rej && vl == 2) {
                int m = get_be16(v);
                if (m >= 128 && m <= VM_HDLC_MAX_MRU) p->cfg.mru = m;
            } else {
                p->cfg.mru = VM_HDLC_DEFAULT_MRU;
            }
            break;
        case VM_LCP_OPT_ACCM:
            /*
             * Rej された = 相手は ACCM オプションを扱えない
             *            = 既定 (全エスケープ) のまま行くしかない。
             * Nak された = 相手が指定した値を我々が受信側で期待する。
             * どちらにせよ我々の受信は寛容なので実害は無い。
             */
            break;
        case VM_LCP_OPT_MAGIC:
            p->our_magic = p->our_magic * 1103515245u + 12345u;
            if (p->our_magic == 0) p->our_magic = 0x5A5A5A5Au;
            break;
        case VM_LCP_OPT_AUTH:
            VM_LOGW("ppp: 相手が PAP を %s → 認証を省略する",
                    is_rej ? "Reject" : "Nak");
            p->cfg.require_auth = false;
            p->auth_ok = true;
            break;
        default:
            break;
        }
    }

    /* 修正した内容で CR を再送 */
    lcp_send_conf_req(p);
    if (p->lcp.state == VM_CP_INITIAL) p->lcp.state = VM_CP_REQ_SENT;
}

/* ===========================================================================
 * LCP: Echo / Terminate / Protocol-Reject
 * =========================================================================== */

static void lcp_send_echo_reply(vm_ppp_t *p, uint8_t id,
                                const uint8_t *data, int len)
{
    uint8_t o[64];
    int n = 0;
    /*
     * Echo-Reply の Data フィールドは
     *   [Magic-Number(4)][相手が送ってきた任意データ]
     * Magic は *我々の* もので上書きする (相手のものを返してはいけない)。
     */
    put_be32(o, p->our_magic); n = 4;
    if (len > 4) {
        int copy = len - 4;
        if (copy > (int)sizeof(o) - 4) copy = (int)sizeof(o) - 4;
        memcpy(o + 4, data + 4, (size_t)copy);
        n += copy;
    }
    p->stats.echo_tx++;
    send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_ECHO_REP, id, o, n);
}

/*
 * 難所 C-2: 実装しないネットワーク層プロトコルへの応答。
 *
 * Protocol-Reject の Data は
 *   [Rejected-Protocol(2)][元パケットの中身]
 * 元パケットは peer_mru に収まるよう切り詰める。
 */
static void lcp_send_proto_rej(vm_ppp_t *p, uint16_t rejected,
                               const uint8_t *pkt, int len)
{
    uint8_t o[VM_HDLC_DEFAULT_MRU];
    int n = 0, copy;

    put_be16(o, rejected); n = 2;
    copy = len;
    if (copy > (int)sizeof(o) - 2) copy = (int)sizeof(o) - 2;
    if (copy > 0) { memcpy(o + 2, pkt, (size_t)copy); n += copy; }

    p->stats.proto_rej_tx++;
    VM_LOGD("ppp: Protocol-Reject %04X (未実装プロトコル)", (unsigned)rejected);
    send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_PROTO_REJ, p->lcp.next_id++, o, n);
}

/* ===========================================================================
 * LCP ディスパッチ
 * =========================================================================== */

static void handle_lcp(vm_ppp_t *p, const uint8_t *pkt, int len)
{
    uint8_t code, id;
    int     plen, dlen;
    const uint8_t *data;

    if (len < 4) return;
    code = pkt[0];
    id   = pkt[1];
    plen = get_be16(pkt + 2);

    /*
     * Length フィールドの検証。
     * plen > len は明らかに壊れている (捨てる)。
     * plen < len は「HDLC のパディング」で正常に起こり得るので、
     * plen を信じて切り詰める。
     */
    if (plen < 4 || plen > len) {
        VM_LOGD("ppp: LCP length 不正 (len=%d plen=%d)", len, plen);
        return;
    }
    data = pkt + 4;
    dlen = plen - 4;

    p->stats.lcp_rx++;
    VM_LOGT("ppp rx: LCP code=%u id=%u len=%d", code, id, plen);

    switch (code) {
    case VM_PPP_CONF_REQ:
        lcp_handle_conf_req(p, id, data, dlen);
        break;

    case VM_PPP_CONF_ACK:
        /*
         * ★ Identifier の照合を必ず行う ★
         * 再送で古い CR への Ack が遅れて届く事があり、
         * 照合しないと交渉結果が古い値に巻き戻る。
         */
        if (id != p->lcp.req_id) {
            VM_LOGD("ppp: LCP Ack id 不一致 (got %u want %u) 無視",
                    id, p->lcp.req_id);
            break;
        }
        cp_got_ack_rcvd(p, &p->lcp, true);
        break;

    case VM_PPP_CONF_NAK:
    case VM_PPP_CONF_REJ:
        if (id != p->lcp.req_id) break;
        lcp_handle_conf_nak_rej(p, code == VM_PPP_CONF_REJ, data, dlen);
        break;

    case VM_PPP_TERM_REQ:
        VM_LOGI("ppp: LCP Terminate-Request 受信 → 切断");
        send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_TERM_ACK, id, data, dlen);
        p->state = VM_PPP_DEAD;
        p->ipcp_up = false;
        break;

    case VM_PPP_TERM_ACK:
        if (p->state == VM_PPP_TERMINATE) {
            VM_LOGI("ppp: Terminate-Ack 受信 → DEAD");
            p->state = VM_PPP_DEAD;
            p->ipcp_up = false;
        }
        break;

    case VM_PPP_ECHO_REQ:
        p->stats.echo_rx++;
        lcp_send_echo_reply(p, id, data, dlen);
        break;

    case VM_PPP_ECHO_REP:
        p->stats.echo_rx++;
        p->echo_missed = 0;   /* 生きている */
        break;

    case VM_PPP_DISC_REQ:
        /* Discard-Request: 何もしないのが仕様通りの正しい動作 */
        break;

    case VM_PPP_CODE_REJ:
    case VM_PPP_PROTO_REJ:
        VM_LOGW("ppp: 相手から Code/Protocol-Reject を受信 (code=%u)", code);
        break;

    default:
        /*
         * 知らない Code には Code-Reject を返す。
         * Data には受信したパケット全体を入れる。
         */
        send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_CODE_REJ,
                 p->lcp.next_id++, pkt, plen);
        break;
    }
}

/* ===========================================================================
 * PAP (RFC 1334)
 * ===========================================================================
 * Authenticate-Request の Data:
 *   [Peer-ID-Len(1)][Peer-ID][Passwd-Len(1)][Passwd]
 *
 * ★ 平文である ★
 * PAP はパスワードを平文で送る。実 ISP では論外だが、
 * 我々は「ローカルの仮想 ISP」なので問題にならない。
 * むしろ CHAP/MS-CHAPv2 を実装すると MD4/MD5/DES が必要になり、
 * 暗号ライブラリ依存が増えてビルドが面倒になる。
 *
 * ★ 認証を通す条件 ★
 * config.ini の [ISP_n] に username/password が無い場合は
 * 「どんな値でも通す」。ダイアルアップの体験を再現するのが目的で、
 * 認証で弾く事に意味が無いため。設定があれば厳密に一致を要求する。
 */
static void handle_pap(vm_ppp_t *p, const uint8_t *pkt, int len)
{
    uint8_t code, id;
    int     plen;
    const uint8_t *d;
    int     ulen, plen2, pos;
    char    user[64], pass[64];
    bool    ok;
    uint8_t msg[64];
    int     mn;

    if (len < 4) return;
    code = pkt[0]; id = pkt[1]; plen = get_be16(pkt + 2);
    if (plen < 4 || plen > len) return;

    p->stats.pap_rx++;

    if (code != VM_PAP_AUTH_REQ) return;   /* Ack/Nak はサーバには来ない */

    if (p->state != VM_PPP_AUTHENTICATE) {
        /*
         * まだ LCP が終わっていない or もう認証済み。
         * RFC 1334 的には「認証済みなら Ack を返してよい」。
         * RAS は再送してくる事があるので Ack を返す方が安全。
         */
        if (p->auth_ok) {
            mn = 0; msg[mn++] = 0;
            send_pkt(p, VM_PPP_PROTO_PAP, VM_PAP_AUTH_ACK, id, msg, mn);
        }
        return;
    }

    d   = pkt + 4;
    pos = 0;
    memset(user, 0, sizeof(user));
    memset(pass, 0, sizeof(pass));

    if (plen - 4 < 1) return;
    ulen = d[pos++];
    if (pos + ulen > plen - 4) return;
    if (ulen > 0) {
        int c = ulen < (int)sizeof(user) - 1 ? ulen : (int)sizeof(user) - 1;
        memcpy(user, d + pos, (size_t)c);
    }
    pos += ulen;

    if (pos + 1 > plen - 4) return;
    plen2 = d[pos++];
    if (pos + plen2 > plen - 4) return;
    if (plen2 > 0) {
        int c = plen2 < (int)sizeof(pass) - 1 ? plen2 : (int)sizeof(pass) - 1;
        memcpy(pass, d + pos, (size_t)c);
    }

    if (p->cfg.username[0] == '\0') {
        ok = true;   /* 設定が無ければ何でも通す */
    } else {
        ok = (strcmp(user, p->cfg.username) == 0 &&
              strcmp(pass, p->cfg.password) == 0);
    }

    VM_LOGI("ppp: PAP user=\"%s\" -> %s", user, ok ? "ACK" : "NAK");

    /* Ack/Nak の Data: [Msg-Length(1)][Message] */
    if (ok) {
        const char *m = "Welcome";
        mn = 0;
        msg[mn++] = (uint8_t)strlen(m);
        memcpy(msg + mn, m, strlen(m)); mn += (int)strlen(m);
        send_pkt(p, VM_PPP_PROTO_PAP, VM_PAP_AUTH_ACK, id, msg, mn);
        p->auth_ok = true;
        enter_network(p);
    } else {
        const char *m = "Auth failed";
        mn = 0;
        msg[mn++] = (uint8_t)strlen(m);
        memcpy(msg + mn, m, strlen(m)); mn += (int)strlen(m);
        send_pkt(p, VM_PPP_PROTO_PAP, VM_PAP_AUTH_NAK, id, msg, mn);
        /*
         * RFC 1334: Nak を返したら LCP Terminate する。
         * ただし RAS は再試行のために別の資格情報を送ってくる事があるので、
         * 即断せず Max-Failure まで待つ。
         */
        if (++p->lcp.failure_count > VM_PPP_MAX_FAILURE)
            vm_ppp_close(p, true, p->now_ms);
    }
}

/* ===========================================================================
 * IPCP (RFC 1332 + RFC 1877)
 * =========================================================================== */

static void ipcp_handle_conf_req(vm_ppp_t *p, uint8_t id,
                                 const uint8_t *data, int len)
{
    uint8_t  rej[256], nak[256];
    int      rejn = 0, nakn = 0;
    opt_iter_t it;
    uint8_t  t;
    const uint8_t *v;
    int      vl;
    uint32_t want_ip = 0;
    bool     ip_seen = false;
    char     abuf[16];

    opt_iter_init(&it, data, len);
    while (opt_next(&it, &t, &v, &vl)) {
        switch (t) {

        case VM_IPCP_OPT_ADDR:
            if (vl != 4) { rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl); break; }
            want_ip = get_be32(v);
            ip_seen = true;
            /*
             * ★ 難所 C-4 ★
             * RAS は 0.0.0.0 で「割り当ててくれ」と言ってくる。
             * また、前回の接続で覚えたアドレスを提案してくる事もある。
             * どちらも我々の割り当てと違えば Nak で正しい値を返す。
             * Reject してはいけない (RAS はアドレスが無いと上がれない)。
             */
            if (want_ip != p->cfg.client_ip) {
                uint8_t fix[4];
                put_be32(fix, p->cfg.client_ip);
                nakn = opt_copy(nak, nakn, sizeof(nak), t, fix, 4);
                VM_LOGD("ppp: IPCP 相手希望 %s → %s を Nak で提示",
                        vm_ipv4_str(want_ip, abuf, sizeof(abuf)),
                        vm_ipv4_str(p->cfg.client_ip, abuf, sizeof(abuf)));
            }
            break;

        case VM_IPCP_OPT_DNS1:
        case VM_IPCP_OPT_DNS2: {
            /*
             * ★ 難所 C-5 ★
             * DNS を Reject すると「繋がっているのに名前解決できない」
             * という最悪の症状になる。必ず Nak で実値を返す。
             */
            uint32_t want = (t == VM_IPCP_OPT_DNS1) ? p->cfg.dns1 : p->cfg.dns2;
            if (vl != 4) { rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl); break; }
            if (want == 0) {
                /* DNS を配れないなら Reject するしかない */
                rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl);
            } else if (get_be32(v) != want) {
                uint8_t fix[4];
                put_be32(fix, want);
                nakn = opt_copy(nak, nakn, sizeof(nak), t, fix, 4);
            }
            break;
        }

        case VM_IPCP_OPT_VJ:      /* 難所 C-3: VJ 圧縮は実装しない       */
        case VM_IPCP_OPT_ADDRS:   /* 廃止された旧 IP-Addresses オプション */
        case VM_IPCP_OPT_NBNS1:   /* WINS は使わない                    */
        case VM_IPCP_OPT_NBNS2:
        default:
            rejn = opt_copy(rej, rejn, sizeof(rej), t, v, vl);
            break;
        }
    }

    /* 難所 B: Rej → Nak → Ack の順序 */
    if (rejn > 0) {
        p->ipcp.failure_count++;
        send_pkt(p, VM_PPP_PROTO_IPCP, VM_PPP_CONF_REJ, id, rej, rejn);
        return;
    }
    if (nakn > 0) {
        p->ipcp.failure_count++;
        if (p->ipcp.failure_count > VM_PPP_MAX_FAILURE * 2) {
            VM_LOGE("ppp: IPCP Max-Failure → 切断");
            vm_ppp_close(p, true, p->now_ms);
            return;
        }
        send_pkt(p, VM_PPP_PROTO_IPCP, VM_PPP_CONF_NAK, id, nak, nakn);
        return;
    }

    if (ip_seen) p->assigned_ip = want_ip;
    send_pkt(p, VM_PPP_PROTO_IPCP, VM_PPP_CONF_ACK, id, data, len);
    cp_got_ack_sent(p, &p->ipcp, false);
}

static void ipcp_handle_conf_nak_rej(vm_ppp_t *p, bool is_rej,
                                     const uint8_t *data, int len)
{
    opt_iter_t it;
    uint8_t t;
    const uint8_t *v;
    int vl;

    p->ipcp.failure_count++;
    if (p->ipcp.failure_count > VM_PPP_MAX_FAILURE) {
        VM_LOGE("ppp: IPCP Max-Failure → 切断");
        vm_ppp_close(p, true, p->now_ms);
        return;
    }

    opt_iter_init(&it, data, len);
    while (opt_next(&it, &t, &v, &vl)) {
        if (t == VM_IPCP_OPT_ADDR && !is_rej && vl == 4) {
            /*
             * 相手が我々のアドレスに文句を付けてきた。
             * 我々は仮想 ISP なので相手の希望を受け入れて構わない。
             * ただし 0.0.0.0 を提案されたら無視する (自分のアドレスが
             * 無くなると IP 転送ができない)。
             */
            uint32_t sug = get_be32(v);
            if (sug != 0) {
                char a[16];
                VM_LOGI("ppp: IPCP 我々のアドレスを %s に変更",
                        vm_ipv4_str(sug, a, sizeof(a)));
                p->cfg.server_ip = sug;
            }
        }
        /* IP-Address を Reject された場合はどうしようもないので無視 */
    }
    ipcp_send_conf_req(p);
}

static void handle_ipcp(vm_ppp_t *p, const uint8_t *pkt, int len)
{
    uint8_t code, id;
    int     plen, dlen;
    const uint8_t *data;

    if (len < 4) return;
    code = pkt[0]; id = pkt[1]; plen = get_be16(pkt + 2);
    if (plen < 4 || plen > len) return;
    data = pkt + 4; dlen = plen - 4;

    p->stats.ipcp_rx++;

    /*
     * ★ LCP が Opened になる前に IPCP が来る事がある ★
     * RAS は LCP の Ack を受け取った直後 (我々が Ack を返す前) に
     * IPCP を投げてくる実装がある。ここで捨てると RAS は再送するので
     * 動作はするが遅くなる。NETWORK でなくても Conf-Req は処理する。
     */
    switch (code) {
    case VM_PPP_CONF_REQ:
        if (p->state == VM_PPP_DEAD || p->state == VM_PPP_TERMINATE) break;
        if (p->cfg.require_auth && !p->auth_ok) {
            /*
             * 認証前の IPCP は無視するのが正しい (RFC 1661 3.5)。
             * Protocol-Reject を返してはいけない。認証後に再送される。
             */
            VM_LOGD("ppp: 認証前の IPCP Conf-Req を無視");
            break;
        }
        if (p->state == VM_PPP_ESTABLISH) enter_network(p);
        ipcp_handle_conf_req(p, id, data, dlen);
        break;

    case VM_PPP_CONF_ACK:
        if (id != p->ipcp.req_id) break;
        cp_got_ack_rcvd(p, &p->ipcp, false);
        break;

    case VM_PPP_CONF_NAK:
    case VM_PPP_CONF_REJ:
        if (id != p->ipcp.req_id) break;
        ipcp_handle_conf_nak_rej(p, code == VM_PPP_CONF_REJ, data, dlen);
        break;

    case VM_PPP_TERM_REQ:
        send_pkt(p, VM_PPP_PROTO_IPCP, VM_PPP_TERM_ACK, id, data, dlen);
        p->ipcp_up = false;
        if (p->state == VM_PPP_RUNNING) p->state = VM_PPP_NETWORK;
        break;

    default:
        send_pkt(p, VM_PPP_PROTO_IPCP, VM_PPP_CODE_REJ,
                 p->ipcp.next_id++, pkt, plen);
        break;
    }
}

/* ===========================================================================
 * HDLC からのコールバック = プロトコル振り分け
 * =========================================================================== */

static void on_hdlc_frame(void *user, uint16_t proto,
                          const uint8_t *payload, int len)
{
    vm_ppp_t *p = (vm_ppp_t *)user;

    switch (proto) {
    case VM_PPP_PROTO_LCP:
        handle_lcp(p, payload, len);
        break;

    case VM_PPP_PROTO_PAP:
        handle_pap(p, payload, len);
        break;

    case VM_PPP_PROTO_IPCP:
        handle_ipcp(p, payload, len);
        break;

    case VM_PPP_PROTO_IP:
        /*
         * ★ データ段階 ★
         * ここに来るのが最終目的。RAS が送出した生 IPv4 パケットを
         * そのまま上位 (NAT) へ渡す。
         *
         * IPCP が上がる前の IP パケットは捨てる。RAS はまず送ってこないが、
         * 捨てないと NAT 側が未設定の状態で叩かれる。
         */
        if (!p->ipcp_up || p->state != VM_PPP_RUNNING) {
            VM_LOGD("ppp: IPCP 未確立の IP パケットを破棄 (%d bytes)", len);
            break;
        }
        p->stats.ip_rx_pkts++;
        p->stats.ip_rx_bytes += (uint64_t)len;
        if (p->ip_cb) p->ip_cb(p->ip_user, payload, len);
        break;

    /*
     * ★ 難所 C-2 の実装 ★
     * これらは「RAS が要求してくるが我々は実装しない」プロトコル。
     * Protocol-Reject を返すと RAS は即座に諦めて先へ進む。
     * 無応答だと 10 回の再送を待たされる (体感で 30 秒の遅延)。
     */
    case VM_PPP_PROTO_CCP:      /* MPPC/MPPE 圧縮   */
    case VM_PPP_PROTO_IPV6CP:   /* IPv6             */
    case VM_PPP_PROTO_CBCP:     /* MS Callback      */
    case VM_PPP_PROTO_CHAP:     /* CHAP/MS-CHAP     */
    case VM_PPP_PROTO_LQR:      /* Link Quality     */
    case VM_PPP_PROTO_VJCOMP:   /* VJ 圧縮 TCP/IP   */
    case VM_PPP_PROTO_IPV6:
    default:
        p->stats.unknown_proto++;
        /*
         * Protocol-Reject を返せるのは LCP が Opened の時のみ
         * (RFC 1661 5.7)。それ以前は黙って捨てる。
         */
        if (p->lcp.state == VM_CP_OPENED)
            lcp_send_proto_rej(p, proto, payload, len);
        break;
    }
}

/* ===========================================================================
 * 公開 API
 * =========================================================================== */

vm_err_t vm_ppp_init(vm_ppp_t *p, const vm_ppp_cfg_t *cfg,
                     vm_ppp_write_cb wcb, void *wuser,
                     vm_ppp_ip_cb icb, void *iuser,
                     uint32_t now_ms)
{
    if (!p) return VM_ERR_INVAL;

    memset(p, 0, sizeof(*p));
    if (cfg) p->cfg = *cfg;
    else     vm_ppp_cfg_defaults(&p->cfg);

    if (p->cfg.mru <= 0 || p->cfg.mru > VM_HDLC_MAX_MRU)
        p->cfg.mru = VM_HDLC_DEFAULT_MRU;

    p->write_cb   = wcb;
    p->write_user = wuser;
    p->ip_cb      = icb;
    p->ip_user    = iuser;
    p->now_ms     = now_ms;
    p->state      = VM_PPP_DEAD;

    /*
     * Magic-Number の生成。
     * 暗号強度は要らないが 0 は禁止で、かつ毎回変わる必要がある
     * (同じ値だと再接続時にループ誤検出される)。
     * now_ms とポインタ値を混ぜれば十分。
     */
    p->our_magic = (uint32_t)(now_ms * 2654435761u) ^
                   (uint32_t)((uintptr_t)p >> 4) ^ 0x5DEECE66u;
    if (p->our_magic == 0) p->our_magic = 0x1234ABCDu;

    p->peer_mru  = VM_HDLC_DEFAULT_MRU;
    p->peer_accm = 0xFFFFFFFFu;

    vm_hdlc_init(&p->hdlc, on_hdlc_frame, p);
    cp_reset(&p->lcp);
    cp_reset(&p->ipcp);
    p->auth_ok = !p->cfg.require_auth;

    return VM_OK;
}

vm_err_t vm_ppp_open(vm_ppp_t *p, uint32_t now_ms)
{
    if (!p) return VM_ERR_INVAL;

    p->now_ms = now_ms;
    p->state  = VM_PPP_ESTABLISH;

    /*
     * HDLC の送信パラメータを交渉前の既定値に戻す。
     * 再接続時に前回の交渉結果 (ACCM=0 等) が残っていると、
     * 相手がまだ全エスケープを期待している段階で
     * 生の 0x11 (XON) を送ってしまう危険がある。
     */
    p->hdlc.tx_accm = 0xFFFFFFFFu;
    p->hdlc.tx_pfc  = false;
    p->hdlc.tx_acfc = false;
    vm_hdlc_rx_reset(&p->hdlc);

    p->peer_accm = 0xFFFFFFFFu;
    p->peer_mru  = VM_HDLC_DEFAULT_MRU;
    p->peer_wants_pfc = p->peer_wants_acfc = false;
    p->ipcp_up   = false;
    p->assigned_ip = 0;
    p->auth_ok   = !p->cfg.require_auth;
    p->echo_missed = 0;
    p->term_count  = 0;

    cp_reset(&p->lcp);
    cp_reset(&p->ipcp);

    VM_LOGI("ppp: open -> ESTABLISH (LCP Configure-Request 送出)");

    /* 我々から先に CR を送る (ヘッダ vm_ppp_open の説明を参照) */
    lcp_send_conf_req(p);
    p->lcp.state = VM_CP_REQ_SENT;
    return VM_OK;
}

void vm_ppp_input(vm_ppp_t *p, const uint8_t *data, int len, uint32_t now_ms)
{
    if (!p || !data || len <= 0) return;
    p->now_ms = now_ms;
    vm_hdlc_input(&p->hdlc, data, len);
}

vm_err_t vm_ppp_send_ip(vm_ppp_t *p, const uint8_t *pkt, int len)
{
    int n;

    if (!p || !pkt || len <= 0) return VM_ERR_INVAL;
    if (p->state != VM_PPP_RUNNING || !p->ipcp_up) return VM_ERR_STATE;

    /*
     * ★ peer_mru によるチェック ★
     * 相手が「1500 までしか受け取れない」と言っているのに
     * それを超えて送ると、相手は黙って捨てる (エラーも返らない)。
     * 症状は「大きい転送だけが止まる」で極めて分かりにくい。
     *
     * 本来は IP フラグメンテーションすべきだが、libslirp 側が
     * MTU を尊重して 1500 以下で渡してくるので、ここでは
     * 超過を捨ててログに残すだけにする。
     */
    if (len > p->peer_mru) {
        VM_LOGW("ppp: IP パケット %d bytes > peer MRU %d → 破棄",
                len, p->peer_mru);
        return VM_ERR_INVAL;
    }

    n = vm_hdlc_encode(&p->hdlc, VM_PPP_PROTO_IP, pkt, len,
                       p->wbuf, (int)sizeof(p->wbuf));
    if (n < 0) return (vm_err_t)n;

    p->stats.ip_tx_pkts++;
    p->stats.ip_tx_bytes += (uint64_t)len;

    if (p->write_cb) {
        int w = p->write_cb(p->write_user, p->wbuf, n);
        if (w < 0) return VM_ERR_IO;
    }
    return VM_OK;
}

void vm_ppp_tick(vm_ppp_t *p, uint32_t now_ms)
{
    if (!p) return;
    p->now_ms = now_ms;

    if (p->state == VM_PPP_DEAD) return;

    /* --- LCP 再送 --- */
    if (p->lcp.timer_ms != 0 && (int32_t)(now_ms - p->lcp.timer_ms) >= 0) {
        if (p->lcp.state == VM_CP_OPENED) {
            p->lcp.timer_ms = 0;
        } else if (--p->lcp.restart_count <= 0) {
            VM_LOGE("ppp: LCP Max-Configure (%d) 到達 → DEAD",
                    VM_PPP_MAX_CONFIGURE);
            p->state = VM_PPP_DEAD;
            p->ipcp_up = false;
            return;
        } else {
            VM_LOGD("ppp: LCP Conf-Req 再送 (残り %d)", p->lcp.restart_count);
            lcp_send_conf_req(p);
        }
    }

    /* --- IPCP 再送 --- */
    if (p->ipcp.timer_ms != 0 && (int32_t)(now_ms - p->ipcp.timer_ms) >= 0) {
        if (p->ipcp.state == VM_CP_OPENED) {
            p->ipcp.timer_ms = 0;
        } else if (--p->ipcp.restart_count <= 0) {
            VM_LOGE("ppp: IPCP Max-Configure 到達 → 切断");
            vm_ppp_close(p, true, now_ms);
            return;
        } else {
            VM_LOGD("ppp: IPCP Conf-Req 再送 (残り %d)", p->ipcp.restart_count);
            ipcp_send_conf_req(p);
        }
    }

    /* --- 認証タイムアウト --- */
    if (p->state == VM_PPP_AUTHENTICATE &&
        (int32_t)(now_ms - p->auth_deadline_ms) >= 0) {
        VM_LOGE("ppp: PAP タイムアウト → 切断");
        vm_ppp_close(p, true, now_ms);
        return;
    }

    /* --- Terminate 再送 --- */
    if (p->state == VM_PPP_TERMINATE &&
        (int32_t)(now_ms - p->term_timer_ms) >= 0) {
        if (++p->term_count >= VM_PPP_MAX_TERMINATE) {
            VM_LOGI("ppp: Terminate-Ack が来ないので強制 DEAD");
            p->state = VM_PPP_DEAD;
            p->ipcp_up = false;
        } else {
            send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_TERM_REQ,
                     p->lcp.next_id++, NULL, 0);
            p->term_timer_ms = now_ms + VM_PPP_RESTART_MS;
        }
    }

    /*
     * --- LCP Echo による死活監視 ---
     *
     * ★ なぜ必要か ★
     * ダイアルアップでは「相手が黙って消える」事が普通に起きる
     * (Windows 側がスリープした、RAS がクラッシュした等)。
     * DCD を落とすのは我々の側なので、我々が気付かないと
     * 回線が上がったままになり、次の ATDT が失敗する。
     * Echo-Request に 4 回続けて応答が無ければ回線を落とす。
     */
    if (p->state == VM_PPP_RUNNING &&
        (int32_t)(now_ms - p->echo_next_ms) >= 0) {
        uint8_t o[4];
        if (p->echo_missed >= VM_PPP_ECHO_MAX_MISS) {
            VM_LOGE("ppp: LCP Echo 無応答 %d 回 → 回線断とみなす",
                    p->echo_missed);
            p->state = VM_PPP_DEAD;
            p->ipcp_up = false;
            return;
        }
        put_be32(o, p->our_magic);
        p->echo_missed++;
        p->stats.echo_tx++;
        send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_ECHO_REQ,
                 (uint8_t)(p->echo_id++), o, 4);
        p->echo_next_ms = now_ms + VM_PPP_ECHO_INTERVAL_MS;
    }
}

void vm_ppp_close(vm_ppp_t *p, bool graceful, uint32_t now_ms)
{
    if (!p) return;
    p->now_ms = now_ms;

    if (p->state == VM_PPP_DEAD) return;

    p->ipcp_up = false;
    p->lcp.timer_ms  = 0;
    p->ipcp.timer_ms = 0;

    if (!graceful) {
        VM_LOGI("ppp: 強制切断 → DEAD");
        p->state = VM_PPP_DEAD;
        return;
    }

    VM_LOGI("ppp: Terminate-Request 送出");
    p->term_count    = 0;
    p->term_timer_ms = now_ms + VM_PPP_RESTART_MS;
    p->state         = VM_PPP_TERMINATE;
    send_pkt(p, VM_PPP_PROTO_LCP, VM_PPP_TERM_REQ, p->lcp.next_id++, NULL, 0);
}

bool vm_ppp_is_running(const vm_ppp_t *p)
{
    return p && p->state == VM_PPP_RUNNING && p->ipcp_up;
}

const char *vm_ppp_status(const vm_ppp_t *p, char *buf, size_t size)
{
    char a[16];
    if (!buf || size == 0) return "?";
    if (!p) { snprintf(buf, size, "(null)"); return buf; }
    snprintf(buf, size, "%s lcp=%s ipcp=%s client=%s",
             vm_ppp_state_name(p->state),
             cp_state_name(p->lcp.state),
             cp_state_name(p->ipcp.state),
             vm_ipv4_str(p->assigned_ip ? p->assigned_ip : p->cfg.client_ip,
                         a, sizeof(a)));
    return buf;
}
