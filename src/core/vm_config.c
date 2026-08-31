/*
 * vm_config.c - config.ini パーサと仮想電話番号マッチング
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------------- */
/* 文字列ユーティリティ                                                      */
/* ------------------------------------------------------------------------- */

static void str_copy(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst || dst_size == 0) return;
    if (src) {
        for (; src[i] && i + 1 < dst_size; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 前後の空白と引用符を除去 (in-place) */
static char *str_trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';

    /* 値が "..." または '...' で囲まれていれば剥がす */
    if (end - s >= 2 &&
        ((s[0] == '"'  && end[-1] == '"') ||
         (s[0] == '\'' && end[-1] == '\''))) {
        s[end - s - 1] = '\0';
        s++;
    }
    return s;
}

static bool str_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool parse_bool(const char *v, bool def)
{
    if (!v || !*v) return def;
    if (str_ieq(v, "1") || str_ieq(v, "true") || str_ieq(v, "yes") ||
        str_ieq(v, "on") || str_ieq(v, "enable") || str_ieq(v, "enabled"))
        return true;
    if (str_ieq(v, "0") || str_ieq(v, "false") || str_ieq(v, "no") ||
        str_ieq(v, "off") || str_ieq(v, "disable") || str_ieq(v, "disabled"))
        return false;
    return def;
}

/* ------------------------------------------------------------------------- */
/* 番号正規化                                                                */
/* ------------------------------------------------------------------------- */
/*
 * 仕様: 「番号比較時はハイフンを無視して数字のみで照合する」
 *
 * ATDT の引数には以下のようなダイアル修飾子が入り得るため、
 * 数字 (0-9) 以外はすべて捨てる方針とする:
 *
 *   ATDT0120-000-0000       ハイフン
 *   ATDT 0120 000 0000      空白
 *   ATDT(0120)000-0000      括弧
 *   ATDTW0120000000         W = ダイアルトーン待ち
 *   ATDT0120000000,,,123    , = ポーズ
 *   ATDT0120000000;         ; = ダイアル後コマンドモードへ復帰
 *   ATDT*70,0120000000      * = キャッチホン解除 (これも数字以外なので除去)
 *
 * なお '*' と '#' は DTMF 送出時には必要なので、
 * 正規化後の照合用文字列からは除くが、DTMF 送出には原文を使う。
 */
void vm_number_normalize(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    if (!out || out_size == 0) return;
    if (in) {
        for (; *in && o + 1 < out_size; in++) {
            if (*in >= '0' && *in <= '9')
                out[o++] = *in;
        }
    }
    out[o] = '\0';
}

const vm_isp_entry_t *vm_config_match_number(const vm_config_t *cfg,
                                             const char *dialed)
{
    char norm[VM_MAX_NUMBER_LEN];
    int i;

    if (!cfg || !dialed) return NULL;

    vm_number_normalize(dialed, norm, sizeof(norm));
    if (!*norm)
        return NULL;   /* 数字が 1 つも無い -> マッチ不可 */

    /*
     * 完全一致のみを採用する。
     * 部分一致を許すと "000" が "0120000000" にもマッチしてしまい、
     * config.ini の意図と異なる ISP に繋がるため。
     */
    for (i = 0; i < cfg->isp_count; i++) {
        if (strcmp(cfg->isp[i].number_norm, norm) == 0)
            return &cfg->isp[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* 既定値                                                                    */
/* ------------------------------------------------------------------------- */

void vm_config_defaults(vm_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    str_copy(cfg->com_port, sizeof(cfg->com_port), "CNCB0");
    cfg->audio_enable = true;
    cfg->audio_volume = 0.7f;
    cfg->audio_device[0] = '\0';
    cfg->log_level = 3;    /* info */
    str_copy(cfg->log_file, sizeof(cfg->log_file), "vmodem.log");

    cfg->dump_wav = false;
    str_copy(cfg->dump_wav_path, sizeof(cfg->dump_wav_path), "handshake.wav");

    cfg->dialtone_ms    = 2000;
    cfg->dtmf_on_ms     = 80;
    cfg->dtmf_off_ms    = 80;
    cfg->ringback_count = 2;
    cfg->fast_connect   = false;

    str_copy(cfg->ppp_server_ip, sizeof(cfg->ppp_server_ip), "192.168.99.1");
    str_copy(cfg->ppp_client_ip, sizeof(cfg->ppp_client_ip), "192.168.99.2");
    str_copy(cfg->ppp_dns1, sizeof(cfg->ppp_dns1), "8.8.8.8");
    str_copy(cfg->ppp_dns2, sizeof(cfg->ppp_dns2), "1.1.1.1");
    cfg->ppp_require_auth = false;

    str_copy(cfg->net_mode, sizeof(cfg->net_mode), "slirp");

    cfg->isp_count = 0;
}

/* ------------------------------------------------------------------------- */
/* INI パーサ                                                                */
/* ------------------------------------------------------------------------- */

typedef enum {
    SEC_NONE,
    SEC_GENERAL,
    SEC_TIMING,
    SEC_PPP,
    SEC_NETWORK,
    SEC_ISP
} sec_kind_t;

static sec_kind_t classify_section(const char *name)
{
    if (str_ieq(name, "general") || str_ieq(name, "modem"))
        return SEC_GENERAL;
    if (str_ieq(name, "timing"))
        return SEC_TIMING;
    if (str_ieq(name, "ppp"))
        return SEC_PPP;
    if (str_ieq(name, "network") || str_ieq(name, "net"))
        return SEC_NETWORK;
    /*
     * それ以外はすべて ISP エントリ扱い。
     * セクション名は自由 ([ISP_1] でも [MyProvider] でもよい)。
     */
    return SEC_ISP;
}

static void apply_general(vm_config_t *cfg, const char *k, const char *v)
{
    if      (str_ieq(k, "com_port") || str_ieq(k, "port"))
        str_copy(cfg->com_port, sizeof(cfg->com_port), v);
    else if (str_ieq(k, "audio") || str_ieq(k, "audio_enable"))
        cfg->audio_enable = parse_bool(v, cfg->audio_enable);
    else if (str_ieq(k, "audio_volume") || str_ieq(k, "volume"))
        cfg->audio_volume = (float)atof(v);
    else if (str_ieq(k, "audio_device"))
        str_copy(cfg->audio_device, sizeof(cfg->audio_device), v);
    else if (str_ieq(k, "log_level"))
        cfg->log_level = atoi(v);
    else if (str_ieq(k, "log_file"))
        str_copy(cfg->log_file, sizeof(cfg->log_file), v);
    else if (str_ieq(k, "dump_wav"))
        cfg->dump_wav = parse_bool(v, cfg->dump_wav);
    else if (str_ieq(k, "dump_wav_path"))
        str_copy(cfg->dump_wav_path, sizeof(cfg->dump_wav_path), v);
}

static void apply_timing(vm_config_t *cfg, const char *k, const char *v)
{
    if      (str_ieq(k, "dialtone_ms"))    cfg->dialtone_ms    = atoi(v);
    else if (str_ieq(k, "dtmf_on_ms"))     cfg->dtmf_on_ms     = atoi(v);
    else if (str_ieq(k, "dtmf_off_ms"))    cfg->dtmf_off_ms    = atoi(v);
    else if (str_ieq(k, "ringback_count")) cfg->ringback_count = atoi(v);
    else if (str_ieq(k, "fast_connect"))   cfg->fast_connect   =
                                              parse_bool(v, cfg->fast_connect);
}

static void apply_ppp(vm_config_t *cfg, const char *k, const char *v)
{
    if      (str_ieq(k, "server_ip") || str_ieq(k, "local_ip"))
        str_copy(cfg->ppp_server_ip, sizeof(cfg->ppp_server_ip), v);
    else if (str_ieq(k, "client_ip") || str_ieq(k, "remote_ip"))
        str_copy(cfg->ppp_client_ip, sizeof(cfg->ppp_client_ip), v);
    else if (str_ieq(k, "dns1"))
        str_copy(cfg->ppp_dns1, sizeof(cfg->ppp_dns1), v);
    else if (str_ieq(k, "dns2"))
        str_copy(cfg->ppp_dns2, sizeof(cfg->ppp_dns2), v);
    else if (str_ieq(k, "require_auth"))
        cfg->ppp_require_auth = parse_bool(v, cfg->ppp_require_auth);
}

static void apply_network(vm_config_t *cfg, const char *k, const char *v)
{
    if (str_ieq(k, "mode"))
        str_copy(cfg->net_mode, sizeof(cfg->net_mode), v);
}

static void apply_isp(vm_isp_entry_t *e, const char *k, const char *v)
{
    if (str_ieq(k, "number") || str_ieq(k, "phone")) {
        str_copy(e->number_raw, sizeof(e->number_raw), v);
        vm_number_normalize(v, e->number_norm, sizeof(e->number_norm));
    }
    else if (str_ieq(k, "speed") || str_ieq(k, "bps")) {
        e->speed = atoi(v);
    }
    else if (str_ieq(k, "protocol") || str_ieq(k, "standard")) {
        e->protocol = vm_standard_from_string(v);
    }
    else if (str_ieq(k, "username") || str_ieq(k, "user")) {
        str_copy(e->username, sizeof(e->username), v);
    }
    else if (str_ieq(k, "password") || str_ieq(k, "pass")) {
        str_copy(e->password, sizeof(e->password), v);
    }
}

vm_err_t vm_config_load(vm_config_t *cfg, const char *path,
                        char *errbuf, size_t errbuf_size)
{
    FILE       *fp;
    char        line[1024];
    sec_kind_t  sec = SEC_NONE;
    vm_isp_entry_t *cur_isp = NULL;
    int         lineno = 0;

    if (!cfg || !path) return VM_ERR_INVAL;
    if (errbuf && errbuf_size) errbuf[0] = '\0';

    vm_config_defaults(cfg);

    fp = fopen(path, "r");
    if (!fp) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "cannot open '%s'", path);
        return VM_ERR_NOTFOUND;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *s, *eq, *key, *val;
        lineno++;

        /* 改行と CR を除去 */
        s = line;
        {
            char *nl = strpbrk(s, "\r\n");
            if (nl) *nl = '\0';
        }

        /* コメント除去 (; と #)。
         * ただし値の中の # は残したいので、行頭側のみを見る簡易方式。 */
        s = str_trim(s);
        if (!*s || *s == ';' || *s == '#')
            continue;

        /* セクション行 */
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) {
                if (errbuf && errbuf_size)
                    snprintf(errbuf, errbuf_size,
                             "line %d: unterminated section header", lineno);
                fclose(fp);
                return VM_ERR_INVAL;
            }
            *close = '\0';
            {
                char *name = str_trim(s + 1);
                sec = classify_section(name);
                cur_isp = NULL;

                if (sec == SEC_ISP) {
                    if (cfg->isp_count >= VM_MAX_ISP_ENTRIES) {
                        if (errbuf && errbuf_size)
                            snprintf(errbuf, errbuf_size,
                                     "line %d: too many ISP entries (max %d)",
                                     lineno, VM_MAX_ISP_ENTRIES);
                        fclose(fp);
                        return VM_ERR_NOMEM;
                    }
                    cur_isp = &cfg->isp[cfg->isp_count];
                    memset(cur_isp, 0, sizeof(*cur_isp));
                    str_copy(cur_isp->section, sizeof(cur_isp->section), name);
                    cur_isp->protocol = VM_STD_UNKNOWN;
                    cur_isp->speed    = 0;
                    cfg->isp_count++;
                }
            }
            continue;
        }

        /* key = value 行 */
        eq = strchr(s, '=');
        if (!eq) {
            /* '=' が無い行は無視 (寛容にパースする) */
            continue;
        }
        *eq = '\0';
        key = str_trim(s);
        val = str_trim(eq + 1);

        switch (sec) {
        case SEC_GENERAL: apply_general(cfg, key, val); break;
        case SEC_TIMING:  apply_timing(cfg, key, val);  break;
        case SEC_PPP:     apply_ppp(cfg, key, val);     break;
        case SEC_NETWORK: apply_network(cfg, key, val); break;
        case SEC_ISP:     if (cur_isp) apply_isp(cur_isp, key, val); break;
        default: break;   /* セクション外の key=value は無視 */
        }
    }
    fclose(fp);

    /* --- 後処理: 速度と規格の整合を取る --- */
    {
        int i, w = 0;
        for (i = 0; i < cfg->isp_count; i++) {
            vm_isp_entry_t *e = &cfg->isp[i];

            /* 番号が空のエントリは捨てる */
            if (!e->number_norm[0])
                continue;

            /* protocol 省略時は speed から推定 */
            if (e->protocol == VM_STD_UNKNOWN) {
                if (e->speed <= 0) e->speed = 33600;
                e->protocol = vm_standard_from_bitrate(e->speed);
            }
            /* speed 省略時は protocol の上限を採用 */
            if (e->speed <= 0)
                e->speed = vm_standard_max_bitrate(e->protocol);

            /*
             * speed が protocol の上限を超えていたら丸める。
             * 例: protocol=V.32bis, speed=33600 -> 14400
             */
            {
                int cap = vm_standard_max_bitrate(e->protocol);
                if (cap > 0 && e->speed > cap)
                    e->speed = cap;
            }

            if (w != i) cfg->isp[w] = *e;
            w++;
        }
        cfg->isp_count = w;
    }

    if (cfg->audio_volume < 0.0f) cfg->audio_volume = 0.0f;
    if (cfg->audio_volume > 1.0f) cfg->audio_volume = 1.0f;

    if (cfg->isp_count == 0 && errbuf && errbuf_size) {
        snprintf(errbuf, errbuf_size,
                 "warning: no valid ISP entries found in '%s'", path);
    }
    return VM_OK;
}
