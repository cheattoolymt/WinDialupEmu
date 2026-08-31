/*
 * vm_ppp.h - PPP サーバ (RFC 1661 LCP / RFC 1334 PAP / RFC 1332 IPCP)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * なぜ pppd も lwIP も使わず自前実装なのか
 * ===========================================================================
 * 候補を検討した結果、自前実装が最良と判断した。
 *
 *   pppd (ppp-project)   GPL-2  → 汚染。しかも Linux の /dev/ppp カーネル
 *                                 インタフェース前提で、Windows 移植は
 *                                 実質不可能 (WSL2 でも COM が見えない)。
 *
 *   lwIP PPPoS           BSD-3  → サーバモードは確かに動く
 *                                 (pppos_create / ppp_set_ipcp_hisaddr /
 *                                  ppp_listen / pppos_input_tcpip)。
 *                                 だが lwIP には NAT が無い。RAS 側から
 *                                 出た IP を実インターネットへ出すには
 *                                 結局 libslirp 相当が別途必要になる。
 *                                 PPP のためだけに TCP/IP スタックを
 *                                 丸ごと抱えるのは構成上の負債。
 *
 *   自前 PPP + libslirp  BSD+LGPL → 採用。
 *                                 我々が必要な PPP は「HDLC フレーミング +
 *                                 LCP/PAP/IPCP のオプション交渉」だけで、
 *                                 データ段階に入ったら生 IP パケットを
 *                                 libslirp に投げるだけ。これなら
 *                                 2000 行程度で済み、全て BSD で書ける。
 *                                 libslirp は DLL 動的リンクなので LGPL は
 *                                 波及しない。
 *
 * ===========================================================================
 * PPP の 3 段階
 * ===========================================================================
 *
 *   DEAD ──(CONNECT)──> ESTABLISH ──(LCP 収束)──> AUTHENTICATE
 *                          ^                            │
 *                          │                       (PAP 成功 or 認証なし)
 *                          │                            v
 *                    (Term-Req / 回線切断) <── NETWORK <──┘
 *                                              (IPCP 収束)
 *                                                 │
 *                                                 v
 *                                            RUNNING = IP 転送中
 *
 * ===========================================================================
 * 難所 A: 交渉は「2 本の独立した交渉」である
 * ===========================================================================
 * これが PPP 実装で最も誤解される点。LCP/IPCP の Configure-Request は
 * 双方向に飛ぶが、それは 1 つの交渉の往復ではなく **完全に独立した 2 本**。
 *
 *   我々の CR ──> 相手     : 「我々が受信する時のパラメータ」を要求
 *   相手の CR ──> 我々     : 「相手が受信する時のパラメータ」を要求
 *
 * 状態機械は「自分の CR が Ack されたか (ackssent/acksrecv)」を
 * 別々のフラグで持ち、**両方揃って初めて Opened** になる。
 * RFC 1661 の状態遷移表で言えば ACK-SENT と ACK-RCVD を経て OPENED。
 *
 * つまり:
 *   - 相手の CR に Ack を返した           → ack_sent = true
 *   - 我々の CR に Ack が返ってきた       → ack_rcvd = true
 *   - ack_sent && ack_rcvd                → この Protocol は Opened
 *
 * 片方だけで Opened にすると、RAS 側は永遠に CR を再送し続ける。
 *
 * ===========================================================================
 * 難所 B: Nak と Reject の使い分け
 * ===========================================================================
 * 相手の Configure-Request に対する返答は 3 種類あり、意味が全く違う。
 *
 *   Configure-Ack     全オプション受け入れ。
 *   Configure-Nak     「そのオプションは理解できるが、値が不満。
 *                       代わりにこの値でどうか」
 *                       → Nak には *我々が希望する値* を入れる。
 *   Configure-Reject  「そのオプション自体を実装していない/使いたくない。
 *                       二度と送るな」
 *                       → Reject には *相手が送ってきた値をそのまま* 入れる。
 *
 * ★ 絶対に守るルール ★
 *   1 つの応答パケットには Nak と Reject を混ぜられない。
 *      Reject すべきオプションが 1 つでもあれば **Reject を先に返す**。
 *      Reject が全部片付いてから Nak、それも片付いてから Ack。
 *      混ぜたり順序を逆にすると RAS は同じ CR を再送し続け、
 *      Max-Configure (10回) を使い切って回線が落ちる。
 *
 *   Reject に値を書き換えて入れてはいけない。
 *      相手は「自分が送ったオプションと byte 単位で一致するか」で
 *      照合するため、書き換えると Reject を無視される。
 *
 * ===========================================================================
 * 難所 C: Windows RAS 固有の落とし穴 (実測で判明する類)
 * ===========================================================================
 *
 * C-1. RAS は LCP CR に Callback オプション (type 0x0D) を載せてくる
 *      これは MS の Callback Control Protocol 用。実装しないので
 *      **Configure-Reject** する。Nak を返すと RAS は値を変えて再送し、
 *      無限ループになる。
 *
 * C-2. RAS は CCP (0x80FD) で MPPC/MPPE 圧縮を要求してくる
 *      CCP を実装しないので **LCP Protocol-Reject (code 8)** を返す。
 *      無応答で放置すると RAS は CCP CR を 10 回再送し、
 *      その間 IPCP が進まないため「接続に時間がかかる」症状になる。
 *      Protocol-Reject を返せば RAS は即座に圧縮を諦めて先へ進む。
 *      同様に IPV6CP (0x8057) と CBCP (0xC029) も Protocol-Reject。
 *
 * C-3. RAS は IPCP CR で VJ 圧縮 (option 2, 0x002D) を要求してくる
 *      Van Jacobson TCP/IP ヘッダ圧縮は、TCP コネクション毎に
 *      ヘッダの差分状態を持つ本格的な実装が必要。
 *      **Configure-Reject** する。33.6kbps では VJ が効くと体感が
 *      変わるが、正しさを優先する。
 *
 * C-4. RAS は IPCP CR で自分の IP を 0.0.0.0 で送ってくる
 *      「サーバから割り当ててくれ」という意味。
 *      **Configure-Nak に我々が割り当てるアドレスを入れて返す**。
 *      Reject してはいけない (RAS はアドレス無しでは IPCP を上げられない)。
 *
 * C-5. RAS は MS 独自の IPCP オプションで DNS/NBNS を要求する
 *      129 = Primary DNS      131 = Secondary DNS
 *      130 = Primary NBNS     132 = Secondary NBNS
 *      DNS は 0.0.0.0 で来るので Nak で実値を返す。
 *      NBNS (WINS) は使わないので Reject。
 *      DNS を Reject すると Windows の名前解決が死んで
 *      「繋がっているのにブラウザが開かない」という最悪の症状になる。
 *
 * C-6. 認証を要求しない場合、我々は Authentication-Protocol を送らない
 *      Auth-Protocol オプションは *認証する側* が CR に載せる。
 *      サーバである我々が認証不要なら、単に載せなければ良い。
 *      RAS は認証をスキップして IPCP へ進む。
 *      なお RAS の既定は「CHAP を許可」だが、サーバが PAP を要求すれば
 *      RAS は (ユーザー設定で暗号化必須にしていない限り) PAP で応じる。
 *      MS-CHAPv2 は実装しない (DES+MD4 が必要で、しかも我々は
 *      仮想 ISP なので認証の意味が無い)。
 *
 * C-7. Magic-Number は必ず載せる
 *      載せないと RAS 側はループバック検出ができず、
 *      LCP Echo-Request への応答検証で警告を出す実装がある。
 *      値は 0 以外の乱数。相手の Magic と一致したら回線ループなので
 *      即座に Terminate。
 *
 * ===========================================================================
 * 難所 D: 再送タイマとカウンタ (RFC 1661 4.6)
 * ===========================================================================
 *   Restart Timer     3 秒 (低速回線なので長めでも良いが 3 が標準)
 *   Max-Configure     10 回
 *   Max-Terminate     2 回
 *   Max-Failure       5 回 (Nak を貰い続けた時の諦め回数)
 *
 * この層は自分では時間を測らない (テスト不能になるため)。
 * 呼び出し側が vm_ppp_tick(p, now_ms) を定期的に呼ぶ pull モデル。
 * シーケンサ (vm_sequence) と同じ設計方針。
 * ===========================================================================
 */
#ifndef VMODEM_VM_PPP_H
#define VMODEM_VM_PPP_H

#include "vmodem/vm_types.h"
#include "vmodem/vm_hdlc.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* --- PPP パケットのコード (LCP/IPCP 共通, RFC 1661 5) -------------------- */

#define VM_PPP_CONF_REQ      1
#define VM_PPP_CONF_ACK      2
#define VM_PPP_CONF_NAK      3
#define VM_PPP_CONF_REJ      4
#define VM_PPP_TERM_REQ      5
#define VM_PPP_TERM_ACK      6
#define VM_PPP_CODE_REJ      7
#define VM_PPP_PROTO_REJ     8   /* LCP のみ */
#define VM_PPP_ECHO_REQ      9   /* LCP のみ */
#define VM_PPP_ECHO_REP     10   /* LCP のみ */
#define VM_PPP_DISC_REQ     11   /* LCP のみ */

/* --- LCP オプション (RFC 1661 6) ----------------------------------------- */

#define VM_LCP_OPT_MRU       1
#define VM_LCP_OPT_ACCM      2
#define VM_LCP_OPT_AUTH      3
#define VM_LCP_OPT_QUALITY   4
#define VM_LCP_OPT_MAGIC     5
#define VM_LCP_OPT_PFC       7
#define VM_LCP_OPT_ACFC      8
#define VM_LCP_OPT_CALLBACK 13   /* MS 拡張 → Reject               */
#define VM_LCP_OPT_MRRU     17   /* Multilink → Reject             */

/* --- IPCP オプション (RFC 1332 + MS 拡張 RFC 1877) ----------------------- */

#define VM_IPCP_OPT_ADDRS    1   /* 廃止された旧形式 → Reject      */
#define VM_IPCP_OPT_VJ       2   /* VJ 圧縮 → Reject               */
#define VM_IPCP_OPT_ADDR     3   /* IP-Address                     */
#define VM_IPCP_OPT_DNS1   129
#define VM_IPCP_OPT_NBNS1  130   /* WINS → Reject                  */
#define VM_IPCP_OPT_DNS2   131
#define VM_IPCP_OPT_NBNS2  132   /* WINS → Reject                  */

/* --- PAP (RFC 1334) ------------------------------------------------------ */

#define VM_PAP_AUTH_REQ      1
#define VM_PAP_AUTH_ACK      2
#define VM_PAP_AUTH_NAK      3

/* --- タイマ定数 ---------------------------------------------------------- */

#define VM_PPP_RESTART_MS      3000
#define VM_PPP_MAX_CONFIGURE     10
#define VM_PPP_MAX_TERMINATE      2
#define VM_PPP_MAX_FAILURE        5
#define VM_PPP_AUTH_TIMEOUT_MS  20000
#define VM_PPP_ECHO_INTERVAL_MS 30000
#define VM_PPP_ECHO_MAX_MISS      4

/* --- リンク状態 ---------------------------------------------------------- */

typedef enum {
    VM_PPP_DEAD = 0,      /* 回線が無い                            */
    VM_PPP_ESTABLISH,     /* LCP 交渉中                            */
    VM_PPP_AUTHENTICATE,  /* PAP 待ち                              */
    VM_PPP_NETWORK,       /* IPCP 交渉中                           */
    VM_PPP_RUNNING,       /* IP 転送中                             */
    VM_PPP_TERMINATE,     /* Terminate-Req 送信済み、Ack 待ち      */
    VM_PPP__COUNT
} vm_ppp_state_t;

const char *vm_ppp_state_name(vm_ppp_state_t s);

/*
 * 1 つの制御プロトコル (LCP または IPCP) の交渉状態。
 * 難所 A の「2 本の独立した交渉」を素直に表現した構造。
 */
typedef enum {
    VM_CP_INITIAL = 0,    /* 何も送っていない                      */
    VM_CP_REQ_SENT,       /* CR 送信済み、Ack 未受信、Ack 未送信   */
    VM_CP_ACK_RCVD,       /* 我々の CR が Ack された               */
    VM_CP_ACK_SENT,       /* 相手の CR に Ack を返した             */
    VM_CP_OPENED          /* 両方揃った                            */
} vm_cp_state_t;

typedef struct {
    vm_cp_state_t state;
    uint8_t  next_id;      /* 次に送る Identifier                  */
    uint8_t  req_id;       /* 最後に送った CR の Identifier        */
    int      restart_count;/* 残り再送回数                         */
    int      failure_count;/* Nak を貰った回数                     */
    uint32_t timer_ms;     /* 次の再送時刻 (0 = タイマ停止)        */
} vm_cp_t;

/* --- 送信コールバック ---------------------------------------------------- */

/*
 * PPP 層が生成した HDLC フレームを COM へ書き出すためのフック。
 * ここを差し替えるだけでテスト時にキャプチャできる。
 */
typedef int (*vm_ppp_write_cb)(void *user, const uint8_t *data, int len);

/*
 * IPCP が収束した後、RAS から届いた IP パケットを上位 (NAT) へ渡すフック。
 */
typedef void (*vm_ppp_ip_cb)(void *user, const uint8_t *pkt, int len);

/* --- 設定 ---------------------------------------------------------------- */

typedef struct {
    uint32_t server_ip;    /* 我々 (モデム側) のアドレス、ホスト順   */
    uint32_t client_ip;    /* RAS に割り当てるアドレス、ホスト順     */
    uint32_t dns1, dns2;   /* 0 なら DNS オプションを Reject         */
    bool     require_auth; /* true: PAP を要求する                   */
    char     username[64];
    char     password[64];
    int      mru;
} vm_ppp_cfg_t;

/* --- 統計 ---------------------------------------------------------------- */

typedef struct {
    uint64_t ip_rx_pkts, ip_rx_bytes;   /* RAS → インターネット      */
    uint64_t ip_tx_pkts, ip_tx_bytes;   /* インターネット → RAS      */
    uint64_t lcp_rx, lcp_tx;
    uint64_t ipcp_rx, ipcp_tx;
    uint64_t pap_rx, pap_tx;
    uint64_t proto_rej_tx;
    uint64_t echo_rx, echo_tx;
    uint64_t unknown_proto;
} vm_ppp_stats_t;

/* --- 本体 ---------------------------------------------------------------- */

typedef struct vm_ppp_s {
    vm_ppp_state_t state;
    vm_ppp_cfg_t   cfg;

    vm_hdlc_t      hdlc;

    vm_cp_t        lcp;
    vm_cp_t        ipcp;

    /* --- LCP 交渉結果 --- */
    uint32_t our_magic;      /* 我々の Magic-Number                  */
    uint32_t peer_magic;     /* 相手の Magic-Number                  */
    int      peer_mru;       /* 相手が受け取れる最大長               */
    uint32_t peer_accm;      /* 相手が要求した ACCM (= 我々の送信用) */
    bool     peer_wants_pfc;
    bool     peer_wants_acfc;

    /* --- IPCP 交渉結果 --- */
    uint32_t assigned_ip;    /* RAS に実際に割り当てたアドレス       */
    bool     ipcp_up;

    /* --- 認証 --- */
    bool     auth_ok;
    uint32_t auth_deadline_ms;

    /* --- Echo (死活監視) --- */
    uint32_t echo_next_ms;
    int      echo_missed;
    uint32_t echo_id;

    /* --- Terminate --- */
    int      term_count;
    uint32_t term_timer_ms;

    uint32_t now_ms;

    /* --- フック --- */
    vm_ppp_write_cb write_cb;
    void           *write_user;
    vm_ppp_ip_cb    ip_cb;
    void           *ip_user;

    vm_ppp_stats_t stats;

    /* 作業バッファ (スタックを食わないようここに置く) */
    uint8_t        wbuf[VM_HDLC_TXBUF_SIZE];
    uint8_t        pbuf[VM_HDLC_MAX_MRU + 64];
} vm_ppp_t;

/* --- API ----------------------------------------------------------------- */

/* 既定設定 (192.168.99.1/2, DNS 8.8.8.8/8.8.4.4, 認証なし) */
void vm_ppp_cfg_defaults(vm_ppp_cfg_t *cfg);

/*
 * 初期化。state は DEAD。
 * now_ms は単調増加ミリ秒 (基準は任意)。
 */
vm_err_t vm_ppp_init(vm_ppp_t *p, const vm_ppp_cfg_t *cfg,
                     vm_ppp_write_cb wcb, void *wuser,
                     vm_ppp_ip_cb icb, void *iuser,
                     uint32_t now_ms);

/*
 * 回線確立 (CONNECT 直後) に呼ぶ。
 * DEAD → ESTABLISH に遷移し、LCP Configure-Request を送出する。
 *
 * ★ ここで我々から先に CR を送るのが重要 ★
 * 「相手が先に送ってくるのを待つ」実装にすると、RAS 側も
 * サーバの CR を待つ設定の場合に双方が黙ってタイムアウトする。
 * PPP は対称プロトコルなので、両者が即座に CR を送るのが正しい。
 */
vm_err_t vm_ppp_open(vm_ppp_t *p, uint32_t now_ms);

/*
 * COM から読んだ生バイト列を投入する。
 * HDLC デフレーミング → PPP 処理 → 必要なら応答送信 (write_cb) まで
 * この呼び出しの中で完結する。
 */
void vm_ppp_input(vm_ppp_t *p, const uint8_t *data, int len, uint32_t now_ms);

/*
 * インターネット側から来た IP パケットを RAS へ送る。
 * state が RUNNING でなければ捨てる。
 */
vm_err_t vm_ppp_send_ip(vm_ppp_t *p, const uint8_t *pkt, int len);

/* タイマ処理。10〜100ms 間隔で呼ぶ。 */
void vm_ppp_tick(vm_ppp_t *p, uint32_t now_ms);

/*
 * 正常切断 (ATH 相当)。Terminate-Request を送って TERMINATE へ。
 * graceful=false なら即 DEAD (回線が既に落ちている場合)。
 */
void vm_ppp_close(vm_ppp_t *p, bool graceful, uint32_t now_ms);

bool vm_ppp_is_running(const vm_ppp_t *p);

/* デバッグ用: "RUNNING lcp=OPENED ipcp=OPENED ip=192.168.99.2" 等 */
const char *vm_ppp_status(const vm_ppp_t *p, char *buf, size_t size);

/* --- ユーティリティ (テストからも使う) ----------------------------------- */

/* "192.168.99.1" → ホスト順 uint32。失敗で false。 */
bool vm_ipv4_parse(const char *s, uint32_t *out);
/* ホスト順 uint32 → "192.168.99.1"。buf は 16 バイト以上。 */
const char *vm_ipv4_str(uint32_t ip, char *buf, size_t size);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_PPP_H */
