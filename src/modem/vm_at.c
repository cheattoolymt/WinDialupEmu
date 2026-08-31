/*
 * vm_at.c - Hayes AT コマンド インタプリタ 実装
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_at.h"
#include "vmodem/vm_log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ===========================================================================
 * 応答文字列
 * =========================================================================*/
const char *vm_at_result_text(vm_at_result_t r)
{
    switch (r) {
    case VM_AT_RESULT_OK:          return "OK";
    case VM_AT_RESULT_CONNECT:     return "CONNECT";
    case VM_AT_RESULT_RING:        return "RING";
    case VM_AT_RESULT_NO_CARRIER:  return "NO CARRIER";
    case VM_AT_RESULT_ERROR:       return "ERROR";
    case VM_AT_RESULT_NO_DIALTONE: return "NO DIALTONE";
    case VM_AT_RESULT_BUSY:        return "BUSY";
    case VM_AT_RESULT_NO_ANSWER:   return "NO ANSWER";
    default:                       return "ERROR";
    }
}

/* ===========================================================================
 * 応答キュー
 * =========================================================================*/

/* キューを詰め直して空き容量を確保する */
static void resp_compact(vm_at_t *at)
{
    if (at->resp_pos == 0) return;
    if (at->resp_pos >= at->resp_len) {
        at->resp_len = at->resp_pos = 0;
        return;
    }
    memmove(at->resp, at->resp + at->resp_pos,
            (size_t)(at->resp_len - at->resp_pos));
    at->resp_len -= at->resp_pos;
    at->resp_pos = 0;
}

static void resp_put(vm_at_t *at, const char *s, int n)
{
    if (n < 0) n = (int)strlen(s);
    if (n <= 0) return;

    if (at->resp_len + n > VM_AT_RESP_BYTES) resp_compact(at);
    if (at->resp_len + n > VM_AT_RESP_BYTES) {
        /*
         * ここに来るのは上位層が応答を読み出していない場合のみ。
         * 実機は溢れたら捨てるので同じ挙動にする (無限に溜めない)。
         */
        n = VM_AT_RESP_BYTES - at->resp_len;
        if (n <= 0) return;
    }
    memcpy(at->resp + at->resp_len, s, (size_t)n);
    at->resp_len += n;
}

static void resp_putc(vm_at_t *at, char c) { resp_put(at, &c, 1); }

/*
 * 応答の CRLF は S3 (CR) / S4 (LF) レジスタの値を使う。
 * 実機はここを本当に参照するし、V0 では LF を付けない。
 */
static void resp_crlf(vm_at_t *at)
{
    resp_putc(at, (char)at->sreg[3]);
    resp_putc(at, (char)at->sreg[4]);
}

/*
 * 応答の送出。
 *   V1: <CR><LF>TEXT<CR><LF>
 *   V0: N<CR>
 * ATQ1 なら何も出さない。
 */
static void emit_result(vm_at_t *at, vm_at_result_t r)
{
    if (at->quiet) return;

    if (at->verbose) {
        resp_crlf(at);
        resp_put(at, vm_at_result_text(r), -1);
        resp_crlf(at);
    } else {
        char b[8];
        snprintf(b, sizeof(b), "%d", (int)r);
        resp_put(at, b, -1);
        resp_putc(at, (char)at->sreg[3]);
    }
}

void vm_at_emit_result(vm_at_t *at, vm_at_result_t r)
{
    if (!at) return;
    emit_result(at, r);
}

void vm_at_emit_connect(vm_at_t *at, int bps)
{
    if (!at) return;
    if (at->quiet) return;

    /*
     * "CONNECT 33600" 形式。
     * X0 (ATX0) の場合は速度を付けず単に "CONNECT" とするのが実機仕様だが、
     * RAS は速度表示を期待するので常に速度を付ける
     * (仕様書の「CONNECT xxxxx を返す」に合わせる)。
     */
    if (at->verbose) {
        char b[32];
        resp_crlf(at);
        snprintf(b, sizeof(b), "CONNECT %d", bps);
        resp_put(at, b, -1);
        resp_crlf(at);
    } else {
        resp_put(at, "1", -1);
        resp_putc(at, (char)at->sreg[3]);
    }
}

void vm_at_emit_raw(vm_at_t *at, const char *s)
{
    if (!at || !s) return;
    resp_crlf(at);
    resp_put(at, s, -1);
    resp_crlf(at);
}

int vm_at_pop_response(vm_at_t *at, void *buf, int len)
{
    int avail;
    if (!at || !buf || len <= 0) return 0;
    avail = at->resp_len - at->resp_pos;
    if (avail <= 0) { at->resp_len = at->resp_pos = 0; return 0; }
    if (len < avail) avail = len;
    memcpy(buf, at->resp + at->resp_pos, (size_t)avail);
    at->resp_pos += avail;
    if (at->resp_pos >= at->resp_len) at->resp_len = at->resp_pos = 0;
    return avail;
}

bool vm_at_has_response(const vm_at_t *at)
{
    return at && (at->resp_len - at->resp_pos) > 0;
}

/* ===========================================================================
 * 初期化
 * =========================================================================*/
void vm_at_reset(vm_at_t *at)
{
    if (!at) return;

    at->online         = false;
    at->echo           = true;    /* ATE1 が既定 */
    at->verbose        = true;    /* ATV1 が既定 */
    at->quiet          = false;
    at->speaker_on     = true;    /* ATM1: 接続まで鳴らす */
    at->speaker_volume = 2;

    at->dcd_mode  = 1;   /* &C1 : キャリアに追従 */
    at->dtr_mode  = 2;   /* &D2 : DTR 落ちで切断 */
    at->flow_mode = 3;   /* &K3 : RTS/CTS        */

    memset(at->sreg, 0, sizeof(at->sreg));
    at->sreg[0]  = 0;    /* 自動応答しない          */
    at->sreg[1]  = 0;    /* リングカウンタ          */
    at->sreg[2]  = 43;   /* エスケープ文字 '+'      */
    at->sreg[3]  = 13;   /* CR                      */
    at->sreg[4]  = 10;   /* LF                      */
    at->sreg[5]  = 8;    /* BS                      */
    at->sreg[6]  = 2;    /* ダイアル前の待ち (秒)   */
    at->sreg[7]  = 60;   /* キャリア待ち (秒)       */
    at->sreg[8]  = 2;    /* ',' のポーズ長 (秒)     */
    at->sreg[9]  = 6;    /* キャリア検出時間 (0.1s) */
    at->sreg[10] = 14;   /* キャリア消失猶予 (0.1s) */
    at->sreg[11] = 95;   /* DTMF 長 (ms)            */
    at->sreg[12] = 50;   /* エスケープ ガードタイム (0.02s 単位 = 1.0s) */

    at->req_std      = VM_STD_UNKNOWN;
    at->req_max_bps  = 0;
    at->req_min_bps  = 0;
    at->req_automode = true;

    at->line_len = 0;
    at->line[0]  = '\0';

    at->action = VM_AT_ACTION_NONE;
    at->action_number[0] = '\0';

    at->plus_count    = 0;
    at->plus_first_ms = 0;
    at->guard_ok      = true;

    at->passthru     = NULL;
    at->passthru_len = 0;
}

void vm_at_init(vm_at_t *at, const vm_config_t *cfg)
{
    if (!at) return;
    memset(at, 0, sizeof(*at));
    at->cfg = cfg;
    vm_at_reset(at);
    at->last_line[0] = '\0';
    at->resp_len = at->resp_pos = 0;
    at->now_ms = 0;
    at->last_rx_ms = 0;
}

void vm_at_tick(vm_at_t *at, uint32_t now_ms)
{
    if (!at) return;
    at->now_ms = now_ms;

    /*
     * +++ の「前ガードタイム」判定。
     * S12 は 0.02 秒単位なので ms に直すと *20。
     */
    if (!at->guard_ok) {
        uint32_t guard = (uint32_t)at->sreg[12] * 20u;
        if (guard == 0) guard = 1000u;
        if (now_ms - at->last_rx_ms >= guard) at->guard_ok = true;
    }

    /*
     * '+' を数え始めてからガードタイムを超えたら、
     * それはエスケープでは無かった (単なるデータ) ので破棄する。
     */
    if (at->plus_count > 0) {
        uint32_t guard = (uint32_t)at->sreg[12] * 20u;
        if (guard == 0) guard = 1000u;
        if (now_ms - at->plus_first_ms > guard) at->plus_count = 0;
    }
}

void vm_at_set_online(vm_at_t *at, bool online)
{
    if (!at) return;
    at->online = online;
    at->plus_count = 0;
    at->guard_ok = false;
    at->last_rx_ms = at->now_ms;
    if (!online) { at->line_len = 0; at->line[0] = '\0'; }
}

vm_at_action_t vm_at_take_action(vm_at_t *at)
{
    vm_at_action_t a;
    if (!at) return VM_AT_ACTION_NONE;
    a = at->action;
    at->action = VM_AT_ACTION_NONE;
    return a;
}

/* ===========================================================================
 * コマンド行の解析
 * ===========================================================================
 * 1 行は "AT" + 連結されたコマンド列。
 * 例: "ATE0V1&C1&D2S0=0"  ->  E0, V1, &C1, &D2, S0=0
 *
 * パーサは 1 文字ずつ進めながら、コマンド文字とその数値引数を取る。
 * 引数が省略された場合は 0 とみなす (実機と同じ)。
 * =========================================================================*/

/* 数値を読む。1 文字も読めなければ have=false */
static int parse_num(const char **pp, bool *have)
{
    const char *p = *pp;
    int v = 0;
    bool got = false;
    while (*p >= '0' && *p <= '9') {
        if (v < 1000000) v = v * 10 + (*p - '0');
        p++; got = true;
    }
    *pp = p;
    if (have) *have = got;
    return v;
}

/* AT+MS=? の応答を作る (対応規格の一覧) */
static void emit_ms_list(vm_at_t *at)
{
    /*
     * 実機の書式は
     *   +MS: (<mod>,...),(<automode>),(<min_rate>),(<max_rate>)
     * だが、可読性重視で「対応する変調方式の一覧」を返す。
     * 仕様書の要求は「対応規格の一覧を返す」なのでこれで満たす。
     */
    resp_crlf(at);
    resp_put(at,
        "+MS: (V21,V22,V22B,V32,V32B,V34,V34P,V90),(0,1),"
        "(300-33600),(300-56000)", -1);
    resp_crlf(at);

    /* 人間が読みやすい対応表も併せて出す (実機にも冗長な機種はある) */
    resp_put(at, "  V21   300 bps", -1);   resp_crlf(at);
    resp_put(at, "  V22  1200 bps", -1);   resp_crlf(at);
    resp_put(at, "  V22B 2400 bps", -1);   resp_crlf(at);
    resp_put(at, "  V32  9600 bps", -1);   resp_crlf(at);
    resp_put(at, "  V32B 14400 bps", -1);  resp_crlf(at);
    resp_put(at, "  V34  28800 bps", -1);  resp_crlf(at);
    resp_put(at, "  V34P 33600 bps", -1);  resp_crlf(at);
    resp_put(at, "  V90  56000 bps", -1);  resp_crlf(at);
}

/*
 * AT+MS=<mod>[,<automode>[,<min>[,<max>]]]
 * 例: AT+MS=V34,1,300,33600
 */
static bool parse_plus_ms(vm_at_t *at, const char **pp)
{
    const char *p = *pp;
    char tok[32];
    int  ti = 0;

    if (*p == '?') {                 /* AT+MS? : 現在値の問い合わせ */
        char b[64];
        p++;
        resp_crlf(at);
        snprintf(b, sizeof(b), "+MS: %s,%d,%d,%d",
                 vm_standard_name(at->req_std == VM_STD_UNKNOWN
                                  ? VM_STD_V34PLUS : at->req_std),
                 at->req_automode ? 1 : 0,
                 at->req_min_bps ? at->req_min_bps : 300,
                 at->req_max_bps ? at->req_max_bps : 33600);
        resp_put(at, b, -1);
        resp_crlf(at);
        *pp = p;
        return true;
    }

    if (*p != '=') return false;
    p++;

    if (*p == '?') {                 /* AT+MS=? : 対応一覧 */
        p++;
        emit_ms_list(at);
        *pp = p;
        return true;
    }

    /* <mod> */
    while (*p && *p != ',' && ti < (int)sizeof(tok) - 1) tok[ti++] = *p++;
    tok[ti] = '\0';
    if (ti > 0) {
        vm_standard_t s = vm_standard_from_string(tok);
        if (s == VM_STD_UNKNOWN) return false;
        at->req_std = s;
        /*
         * ★ ここで req_max_bps を変調方式の上限で埋めてはいけない ★
         *
         * "AT+MS=V34,1,300,33600" のように <max> が明示された場合、
         * DTE の意図は「33600 まで出して良い」である。
         * ここで req_max_bps = max_bitrate(V34) = 28800 と書いてしまうと
         * 後続の <max> パースで上書きされるとはいえ、<max> 省略時に
         * 「変調方式由来の上限」と「明示指定」の区別が付かなくなる。
         *
         * 区別が必要な理由:
         *   ITU-T V.34 は本来 33.6k まで規定している (1996 年改訂で
         *   31200/33600 が追加された)。VModem は仕様書の速度表に
         *   合わせて V34=28800 / V34+=33600 と列挙子を分けているので、
         *   "V34" と書かれただけで 28800 に丸めると
         *   "AT+MS=V34,1,300,33600" が 28800 になってしまう。
         *
         * 従って req_max_bps は「明示されたときのみ」設定し、
         * 変調方式による上限は req_std 経由で別に適用する
         * (vm_at_negotiated_bps() 参照)。
         */
        at->req_max_bps = 0;
    }

    /* <automode> */
    if (*p == ',') {
        bool have;
        int v;
        p++;
        v = parse_num(&p, &have);
        if (have) at->req_automode = (v != 0);
    }
    /* <min> */
    if (*p == ',') {
        bool have;
        int v;
        p++;
        v = parse_num(&p, &have);
        if (have) at->req_min_bps = v;
    }
    /* <max> */
    if (*p == ',') {
        bool have;
        int v;
        p++;
        v = parse_num(&p, &have);
        if (have) at->req_max_bps = v;
    }

    *pp = p;
    return true;
}

/*
 * ATI / AT&V : 識別情報 / 現在設定の表示。
 * RAS は使わないが、ユーザが手で確認する時に有用。
 */
static void emit_info(vm_at_t *at, int n)
{
    char b[128];
    switch (n) {
    case 0:
        resp_crlf(at); resp_put(at, "VModem 33600", -1); resp_crlf(at);
        break;
    case 1:
        resp_crlf(at); resp_put(at, "VModem virtual dial-up modem", -1);
        resp_crlf(at);
        break;
    case 3:
        resp_crlf(at);
        resp_put(at, "VModem 1.0 (ITU-T V.21/V.22/V.22bis/V.32/V.32bis/"
                     "V.34/V.34+ realtime PCM synthesis)", -1);
        resp_crlf(at);
        break;
    case 4:
        snprintf(b, sizeof(b), "OK (%d ISP entries configured)",
                 at->cfg ? at->cfg->isp_count : 0);
        resp_crlf(at); resp_put(at, b, -1); resp_crlf(at);
        break;
    default:
        resp_crlf(at); resp_put(at, "0", -1); resp_crlf(at);
        break;
    }
}

static void emit_settings(vm_at_t *at)
{
    char b[160];
    resp_crlf(at);
    snprintf(b, sizeof(b),
             "ACTIVE PROFILE:\r\nE%d Q%d V%d M%d L%d &C%d &D%d &K%d",
             at->echo ? 1 : 0, at->quiet ? 1 : 0, at->verbose ? 1 : 0,
             at->speaker_on ? 1 : 0, at->speaker_volume,
             at->dcd_mode, at->dtr_mode, at->flow_mode);
    resp_put(at, b, -1);
    resp_crlf(at);
    snprintf(b, sizeof(b), "S0=%u S2=%u S3=%u S4=%u S5=%u S7=%u S12=%u",
             at->sreg[0], at->sreg[2], at->sreg[3], at->sreg[4],
             at->sreg[5], at->sreg[7], at->sreg[12]);
    resp_put(at, b, -1);
    resp_crlf(at);
}

/*
 * ---------------------------------------------------------------------------
 * 1 行を実行する
 * ---------------------------------------------------------------------------
 * 戻り値 = 送出すべき応答。ただし ATDT/ATA のように「今は応答しない」
 * ケースがあるので、応答するか否かは *emit_now で返す。
 */
static vm_at_result_t exec_line(vm_at_t *at, const char *line, bool *emit_now)
{
    const char *p = line;
    vm_at_result_t res = VM_AT_RESULT_OK;

    *emit_now = true;

    /* 先頭の空白を飛ばす */
    while (*p == ' ' || *p == '\t') p++;

    /* 空行は無視 (応答も返さない = 実機と同じ) */
    if (*p == '\0') { *emit_now = false; return VM_AT_RESULT_OK; }

    /* "A/" : 直前の行を再実行 */
    if ((p[0] == 'A' || p[0] == 'a') && p[1] == '/') {
        if (at->last_line[0] == '\0') return VM_AT_RESULT_ERROR;
        return exec_line(at, at->last_line, emit_now);
    }

    /* "AT" プレフィクス必須 */
    if (!((p[0] == 'A' || p[0] == 'a') && (p[1] == 'T' || p[1] == 't')))
        return VM_AT_RESULT_ERROR;
    p += 2;

    /* 直前行として保存 (A/ 用) */
    if (line != at->last_line) {
        size_t n = strlen(line);
        if (n >= sizeof(at->last_line)) n = sizeof(at->last_line) - 1;
        memcpy(at->last_line, line, n);
        at->last_line[n] = '\0';
    }

    /* "AT" のみ -> OK */
    if (*p == '\0') return VM_AT_RESULT_OK;

    while (*p) {
        char c = *p;

        /* 空白は無視 (RAS が入れてくる事がある) */
        if (c == ' ' || c == '\t') { p++; continue; }

        c = (char)toupper((unsigned char)c);
        p++;

        switch (c) {

        /* ------------------------------------------------------------ *
         * ATD : ダイアル
         *
         * D の後は行末まで全部が番号 (+修飾子)。
         * 修飾子 T/P/W/,/;/!/@ や括弧・空白は
         * vm_number_normalize() が数字以外を落とすので保持したまま渡す。
         * ------------------------------------------------------------ */
        case 'D': {
            const vm_isp_entry_t *isp;
            char norm[VM_MAX_NUMBER_LEN];
            size_t n;

            n = strlen(p);
            if (n >= sizeof(at->action_number)) n = sizeof(at->action_number) - 1;
            memcpy(at->action_number, p, n);
            at->action_number[n] = '\0';
            p += strlen(p);           /* 行末まで消費 */

            vm_number_normalize(at->action_number, norm, sizeof(norm));

            if (norm[0] == '\0') {
                /*
                 * "ATD" だけ / 番号に数字が無い。
                 * 実機は発信音待ちのまま NO DIALTONE になる。
                 */
                VM_LOGW("ATD with no digits: \"%s\"", at->action_number);
                return VM_AT_RESULT_NO_DIALTONE;
            }

            isp = at->cfg ? vm_config_match_number(at->cfg, at->action_number)
                          : NULL;
            if (!isp) {
                /*
                 * ★ 仕様の核心 ★
                 * config.ini に一致しない番号は接続シーケンスを開始せず
                 * 即座に NO CARRIER を返す。
                 */
                VM_LOGI("dial \"%s\" (digits \"%s\") -> no match, NO CARRIER",
                        at->action_number, norm);
                return VM_AT_RESULT_NO_CARRIER;
            }

            VM_LOGI("dial \"%s\" -> [%s] %d bps %s",
                    at->action_number, isp->section, isp->speed,
                    vm_standard_name(isp->protocol));

            /*
             * ここでは応答しない。
             * 上位層が音響シーケンス (ダイアルトーン -> DTMF -> 呼出音 ->
             * ネゴシエーション音 -> 無音) を実行し、完了後に
             * vm_at_emit_connect() で "CONNECT xxxxx" を返す。
             */
            at->action = VM_AT_ACTION_DIAL;
            *emit_now = false;
            return VM_AT_RESULT_OK;
        }

        /* ATA : 着信応答 (被呼側としてハンドシェイク) */
        case 'A':
            at->action = VM_AT_ACTION_ANSWER;
            *emit_now = false;
            return VM_AT_RESULT_OK;

        /* ATH : ハングアップ (ATH0 = 切断, ATH1 = オフフック維持) */
        case 'H': {
            bool have;
            int v = parse_num(&p, &have);
            if (have && v == 1) return VM_AT_RESULT_OK;  /* オフフックのみ */
            at->action = VM_AT_ACTION_HANGUP;
            return VM_AT_RESULT_OK;
        }

        /* ATZ : リセット (Z0/Z1 のプロファイル指定は無視) */
        case 'Z': {
            bool have;
            (void)parse_num(&p, &have);
            at->action = VM_AT_ACTION_RESET;
            /*
             * リセットしても応答フォーマットは「リセット後の設定」で返す。
             * 既定は V1 なので "\r\nOK\r\n" になる。
             */
            vm_at_reset(at);
            at->action = VM_AT_ACTION_RESET;
            return VM_AT_RESULT_OK;
        }

        /* ATO : オンラインへ復帰 */
        case 'O': {
            bool have;
            (void)parse_num(&p, &have);
            at->action = VM_AT_ACTION_ONLINE;
            *emit_now = false;
            return VM_AT_RESULT_OK;
        }

        /* ATE : エコー */
        case 'E': {
            bool have;
            int v = parse_num(&p, &have);
            at->echo = have ? (v != 0) : false;   /* "ATE" = ATE0 */
            break;
        }

        /* ATQ : 応答抑止 */
        case 'Q': {
            bool have;
            int v = parse_num(&p, &have);
            at->quiet = have ? (v != 0) : false;
            break;
        }

        /* ATV : 応答形式 */
        case 'V': {
            bool have;
            int v = parse_num(&p, &have);
            at->verbose = have ? (v != 0) : false;
            break;
        }

        /* ATM : スピーカ制御 (0=常時オフ 1=接続まで 2=常時 3=ダイアル後) */
        case 'M': {
            bool have;
            int v = parse_num(&p, &have);
            if (!have) v = 0;
            if (v < 0 || v > 3) return VM_AT_RESULT_ERROR;
            at->speaker_on = (v != 0);
            break;
        }

        /* ATL : 音量 */
        case 'L': {
            bool have;
            int v = parse_num(&p, &have);
            if (!have) v = 0;
            if (v < 0 || v > 3) return VM_AT_RESULT_ERROR;
            at->speaker_volume = v;
            break;
        }

        /* ATX : 応答セットの選択。受け付けるだけ */
        case 'X': {
            bool have;
            int v = parse_num(&p, &have);
            if (have && (v < 0 || v > 7)) return VM_AT_RESULT_ERROR;
            break;
        }

        /* ATI : 識別情報 */
        case 'I': {
            bool have;
            int v = parse_num(&p, &have);
            emit_info(at, have ? v : 0);
            break;
        }

        /* ATS<n>=<v> / ATS<n>? : S レジスタ */
        case 'S': {
            bool have;
            int reg = parse_num(&p, &have);
            if (!have || reg < 0 || reg >= VM_AT_NUM_SREG)
                return VM_AT_RESULT_ERROR;

            if (*p == '=') {
                bool hv;
                int v;
                p++;
                v = parse_num(&p, &hv);
                if (!hv) return VM_AT_RESULT_ERROR;
                if (v < 0 || v > 255) return VM_AT_RESULT_ERROR;
                at->sreg[reg] = (uint8_t)v;
            } else if (*p == '?') {
                char b[8];
                p++;
                /* 実機は 3 桁ゼロ詰めで返す */
                snprintf(b, sizeof(b), "%03u", at->sreg[reg]);
                resp_crlf(at);
                resp_put(at, b, -1);
                resp_crlf(at);
            } else {
                return VM_AT_RESULT_ERROR;
            }
            break;
        }

        /* ------------------------------------------------------------ *
         * AT& 系
         * ------------------------------------------------------------ */
        case '&': {
            char c2 = (char)toupper((unsigned char)*p);
            bool have;
            int v;
            if (c2 == '\0') return VM_AT_RESULT_ERROR;
            p++;
            switch (c2) {
            case 'C':
                v = parse_num(&p, &have);
                at->dcd_mode = have ? v : 0;
                if (at->dcd_mode < 0 || at->dcd_mode > 2)
                    return VM_AT_RESULT_ERROR;
                break;
            case 'D':
                v = parse_num(&p, &have);
                at->dtr_mode = have ? v : 0;
                if (at->dtr_mode < 0 || at->dtr_mode > 3)
                    return VM_AT_RESULT_ERROR;
                break;
            case 'K':
                v = parse_num(&p, &have);
                at->flow_mode = have ? v : 0;
                break;
            case 'F':                 /* 工場出荷設定 */
                (void)parse_num(&p, &have);
                vm_at_reset(at);
                break;
            case 'V':                 /* 現在設定表示 */
                (void)parse_num(&p, &have);
                emit_settings(at);
                break;
            case 'W':                 /* プロファイル保存: 受け付けるだけ */
            case 'Y':
                (void)parse_num(&p, &have);
                break;
            default:
                return VM_AT_RESULT_ERROR;
            }
            break;
        }

        /* ------------------------------------------------------------ *
         * AT+ 系 (+MS など)
         * ------------------------------------------------------------ */
        case '+': {
            /* コマンド名を読む (英数字) */
            char name[16];
            int  ni = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')
                   && ni < (int)sizeof(name) - 1)
                name[ni++] = (char)toupper((unsigned char)*p++);
            name[ni] = '\0';

            if (strcmp(name, "MS") == 0) {
                if (!parse_plus_ms(at, &p)) return VM_AT_RESULT_ERROR;
            } else if (strcmp(name, "GMI") == 0) {
                resp_crlf(at); resp_put(at, "VModem", -1); resp_crlf(at);
            } else if (strcmp(name, "GMM") == 0) {
                resp_crlf(at); resp_put(at, "VModem Virtual Modem", -1);
                resp_crlf(at);
            } else if (strcmp(name, "GMR") == 0) {
                resp_crlf(at); resp_put(at, "1.0", -1); resp_crlf(at);
            } else if (strcmp(name, "FCLASS") == 0) {
                /* FAX クラス: データのみ対応 */
                if (*p == '=' && p[1] == '?') {
                    p += 2;
                    resp_crlf(at); resp_put(at, "0", -1); resp_crlf(at);
                } else if (*p == '?') {
                    p++;
                    resp_crlf(at); resp_put(at, "0", -1); resp_crlf(at);
                } else if (*p == '=') {
                    bool have;
                    int v;
                    p++;
                    v = parse_num(&p, &have);
                    /* FAX は非対応: クラス 0 以外は ERROR */
                    if (have && v != 0) return VM_AT_RESULT_ERROR;
                }
            } else {
                return VM_AT_RESULT_ERROR;
            }
            break;
        }

        /* ATB / ATN / ATW / ATY / AT% 等: 受け付けて無視 */
        case 'B':
        case 'N':
        case 'W':
        case 'Y':
        case 'C':
        case 'P':
        case 'T': {
            bool have;
            (void)parse_num(&p, &have);
            break;
        }

        case '%':
        case '\\':
        case '-': {
            /* AT%C1 / AT\N3 / AT-K1 等: 圧縮/エラー訂正設定。受け付ける */
            bool have;
            if (*p) p++;              /* サブコマンド文字 */
            (void)parse_num(&p, &have);
            break;
        }

        default:
            VM_LOGD("unknown AT command char '%c' in \"%s\"", c, line);
            return VM_AT_RESULT_ERROR;
        }
    }

    return res;
}

/* ===========================================================================
 * バイト投入
 * =========================================================================*/

/*
 * オンラインモードでの +++ エスケープ検出。
 *
 * 条件 (実機の TIES/Hayes 方式):
 *   1. 直前に S12 ガードタイム以上の無通信がある
 *   2. その後 S2 の文字 ('+') が 3 個連続
 *   3. 3 個目の後にも S12 以上の無通信がある
 *
 * 3 の判定は次の tick で行うので、ここでは 2 まで検出して
 * plus_count == 3 の状態にし、tick 側で確定させる…という実装も可能だが、
 * 実用上は「3 個揃った時点でコマンドモードへ」で問題ない
 * (PPP のデータに "+++" が現れても、その前後に 1 秒の無通信は
 *  まず有り得ないため 1 の条件で弾ける)。
 */
static int scan_escape(vm_at_t *at, const uint8_t *d, int len)
{
    char esc = (char)at->sreg[2];
    int  i;

    for (i = 0; i < len; i++) {
        if ((char)d[i] == esc) {
            if (at->plus_count == 0) {
                if (!at->guard_ok) continue;      /* 前ガード不足 */
                at->plus_first_ms = at->now_ms;
            }
            at->plus_count++;
            if (at->plus_count >= 3) {
                at->plus_count = 0;
                /* i+1 バイト目まで消費 ('+' 3 個は PPP へ渡さない) */
                return i - 2;                     /* '+' 3 個の直前まで */
            }
        } else {
            at->plus_count = 0;
        }
    }
    return -1;
}

int vm_at_feed(vm_at_t *at, const void *data, int len)
{
    const uint8_t *d = (const uint8_t *)data;
    int i;

    if (!at || !d || len <= 0) {
        if (at) { at->passthru = NULL; at->passthru_len = 0; }
        return 0;
    }

    at->passthru = NULL;
    at->passthru_len = 0;

    /* ------------------------------------------------------------------ *
     * オンライン (データ) モード
     * ------------------------------------------------------------------ */
    if (at->online) {
        int cut = scan_escape(at, d, len);

        at->last_rx_ms = at->now_ms;
        at->guard_ok = false;

        if (cut >= 0) {
            /*
             * エスケープ成立。'+' より前のデータは PPP へ渡し、
             * コマンドモードへ戻る。
             * 実機は "\r\nOK\r\n" を返す。
             */
            at->passthru     = d;
            at->passthru_len = cut;
            at->online       = false;
            at->line_len     = 0;
            at->line[0]      = '\0';
            emit_result(at, VM_AT_RESULT_OK);
            VM_LOGI("+++ escape detected -> command mode");
            return len;
        }

        /*
         * '+' を数え途中の分は、まだエスケープか判らないので
         * PPP へ渡さず保留したい。しかし保留バッファを持つと複雑になるので、
         * 「途中の '+' も渡してしまう」方針にする。
         *
         * PPP (HDLC) は 0x7E フレーム境界で同期するので、
         * 余分な '+' (0x2B) が混ざってもフレームの FCS 検査で捨てられ、
         * 再送で復旧する。エスケープ成立時のみ厳密に切り落とせば十分。
         */
        at->passthru     = d;
        at->passthru_len = len;
        return len;
    }

    /* ------------------------------------------------------------------ *
     * コマンドモード
     * ------------------------------------------------------------------ */
    at->last_rx_ms = at->now_ms;

    for (i = 0; i < len; i++) {
        char c = (char)d[i];

        /* --- BS (S5): 行編集 --- */
        if (c == (char)at->sreg[5]) {
            if (at->line_len > 0) {
                at->line_len--;
                /*
                 * エコーが有効なら「BS SP BS」で端末上の文字を消す。
                 * これをやらないと TeraTerm 等で見た目が壊れる。
                 */
                if (at->echo) {
                    resp_putc(at, c);
                    resp_putc(at, ' ');
                    resp_putc(at, c);
                }
            }
            continue;
        }

        /* --- CR (S3): 行確定 --- */
        if (c == (char)at->sreg[3]) {
            bool emit_now = true;
            vm_at_result_t res;

            /*
             * ★ エコー順序の注意 ★
             * CR のエコーは「コマンド実行前」に積む必要がある。
             * ATE0 を実行してからエコーすると CR が消えてしまい、
             * 端末の表示が崩れる。
             */
            if (at->echo) { resp_putc(at, c); resp_putc(at, (char)at->sreg[4]); }

            at->line[at->line_len] = '\0';

            if (at->line_len == 0) {
                /* 空行: 応答なし (実機同様) */
                continue;
            }

            VM_LOGD("AT command: \"%s\"", at->line);
            res = exec_line(at, at->line, &emit_now);
            at->line_len = 0;
            at->line[0]  = '\0';

            if (emit_now) emit_result(at, res);
            continue;
        }

        /* --- LF: CR の直後なら無視 (CRLF 対応) --- */
        if (c == (char)at->sreg[4]) continue;

        /* --- "A/" : CR 無しで即実行される特殊形 --- */
        if (at->line_len == 1 && c == '/' &&
            (at->line[0] == 'A' || at->line[0] == 'a')) {
            bool emit_now = true;
            vm_at_result_t res;
            if (at->echo) resp_putc(at, c);
            at->line_len = 0;
            at->line[0]  = '\0';
            res = exec_line(at, "A/", &emit_now);
            if (emit_now) emit_result(at, res);
            continue;
        }

        /* --- 通常文字 --- */
        if (at->echo) resp_putc(at, c);

        if (at->line_len < VM_AT_MAX_LINE - 1) {
            at->line[at->line_len++] = c;
        } else {
            /* 行が長すぎる: 捨てて ERROR */
            at->line_len = 0;
            at->line[0] = '\0';
            emit_result(at, VM_AT_RESULT_ERROR);
        }
    }

    return len;
}

/* ===========================================================================
 * 速度決定
 * =========================================================================*/
int vm_at_negotiated_bps(const vm_at_t *at, const vm_isp_entry_t *isp,
                         vm_standard_t *std_out)
{
    int bps;
    vm_standard_t std;

    if (!isp) {
        if (std_out) *std_out = VM_STD_UNKNOWN;
        return 0;
    }

    std = isp->protocol;
    bps = isp->speed;

    /*
     * config の速度が規格上限を超えていたら規格側で頭打ちにする。
     * (例: protocol=V22bis speed=33600 という矛盾した設定)
     */
    if (std != VM_STD_UNKNOWN) {
        int cap = vm_standard_max_bitrate(std);
        if (cap > 0 && bps > cap) bps = cap;
    }

    /*
     * AT+MS による頭打ち。
     * 実機は「両者が対応する最大速度」でリンクするので、
     * DTE が V.22bis までと言えば 2400bps で繋がる。
     *
     * 優先順位:
     *   1. <max> が明示されていれば、それが最終的な上限 (DTE の明示意図)
     *   2. <max> 省略時は変調方式 (<mod>) の上限を使う
     *
     * 1 を 2 より優先する理由は parse_plus_ms() のコメントを参照
     * ("AT+MS=V34,1,300,33600" を 28800 に丸めてしまう問題)。
     */
    if (at) {
        if (at->req_max_bps > 0) {
            /* <max> 明示: これが上限 */
            if (bps > at->req_max_bps) {
                bps = at->req_max_bps;
                std = vm_standard_from_bitrate(bps);
            }
        } else if (at->req_std != VM_STD_UNKNOWN) {
            /* <max> 省略: 変調方式の上限 */
            int cap = vm_standard_max_bitrate(at->req_std);
            if (cap > 0 && bps > cap) {
                bps = cap;
                std = at->req_std;
            }
        }

        /* <min> より下は張らない (回線品質が良い前提のエミュレータ) */
        if (at->req_min_bps > 0 && bps < at->req_min_bps)
            bps = at->req_min_bps;
    }

    if (bps <= 0) bps = 33600;
    if (std == VM_STD_UNKNOWN) std = vm_standard_from_bitrate(bps);

    if (std_out) *std_out = std;
    return bps;
}
