/*
 * vm_types.c - 規格名テーブルと変換関数
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_types.h"
#include <string.h>
#include <ctype.h>

typedef struct {
    vm_standard_t std;
    const char   *name;
    const char   *aliases;   /* '|' 区切りの別名 */
    int           max_bps;
} std_info_t;

static const std_info_t g_std_table[] = {
    { VM_STD_V21,     "V.21",     "v21|21|300",                       300   },
    { VM_STD_V22,     "V.22",     "v22|22|1200",                      1200  },
    { VM_STD_V22BIS,  "V.22bis",  "v22bis|22bis|v.22b|2400",          2400  },
    { VM_STD_V32,     "V.32",     "v32|32|9600",                      9600  },
    { VM_STD_V32BIS,  "V.32bis",  "v32bis|32bis|v.32b|14400|14.4",    14400 },
    { VM_STD_V34,     "V.34",     "v34|34|28800|28.8",                28800 },
    { VM_STD_V34PLUS, "V.34+",    "v34+|v34plus|v.34plus|v.34+|"
                                  "33600|33.6|v34bis|v.34bis",        33600 },
    { VM_STD_V90,     "V.90",     "v90|90|56000|56k|57600",           56000 },
};

static const size_t g_std_count = sizeof(g_std_table)/sizeof(g_std_table[0]);

const char *vm_standard_name(vm_standard_t std)
{
    size_t i;
    for (i = 0; i < g_std_count; i++) {
        if (g_std_table[i].std == std)
            return g_std_table[i].name;
    }
    return "UNKNOWN";
}

int vm_standard_max_bitrate(vm_standard_t std)
{
    size_t i;
    for (i = 0; i < g_std_count; i++) {
        if (g_std_table[i].std == std)
            return g_std_table[i].max_bps;
    }
    return 0;
}

/* 大文字小文字とドット/ハイフン/アンダースコアを無視した比較用に正規化 */
static void norm_token(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (; *in && o + 1 < out_size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '.' || c == '-' || c == '_' || c == ' ' || c == '\t')
            continue;
        out[o++] = (char)tolower(c);
    }
    out[o] = '\0';
}

vm_standard_t vm_standard_from_string(const char *s)
{
    char want[64];
    size_t i;

    if (!s || !*s)
        return VM_STD_UNKNOWN;

    norm_token(s, want, sizeof(want));
    if (!*want)
        return VM_STD_UNKNOWN;

    for (i = 0; i < g_std_count; i++) {
        char cand[64];
        const char *p = g_std_table[i].aliases;

        /* 正式名との比較 */
        norm_token(g_std_table[i].name, cand, sizeof(cand));
        if (strcmp(cand, want) == 0)
            return g_std_table[i].std;

        /* 別名リストとの比較 */
        while (p && *p) {
            const char *end = strchr(p, '|');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            char tok[64];
            if (len < sizeof(tok)) {
                memcpy(tok, p, len);
                tok[len] = '\0';
                norm_token(tok, cand, sizeof(cand));
                if (strcmp(cand, want) == 0)
                    return g_std_table[i].std;
            }
            if (!end) break;
            p = end + 1;
        }
    }
    return VM_STD_UNKNOWN;
}

vm_standard_t vm_standard_from_bitrate(int bps)
{
    /*
     * 速度からもっとも自然な規格を選ぶ。
     * 33600 は V.34+ (V.34 annex 12) を、28800 は素の V.34 を返す。
     */
    if (bps >= 56000) return VM_STD_V90;
    if (bps >= 33600) return VM_STD_V34PLUS;
    if (bps >= 28800) return VM_STD_V34;
    if (bps >= 14400) return VM_STD_V32BIS;
    if (bps >=  9600) return VM_STD_V32;
    if (bps >=  2400) return VM_STD_V22BIS;
    if (bps >=  1200) return VM_STD_V22;
    return VM_STD_V21;
}

const char *vm_phase_name(vm_phase_t p)
{
    switch (p) {
    case VM_PHASE_IDLE:            return "IDLE";
    case VM_PHASE_OFFHOOK:         return "OFFHOOK";
    case VM_PHASE_DIALTONE:        return "DIALTONE";
    case VM_PHASE_DTMF:            return "DTMF";
    case VM_PHASE_RINGBACK:        return "RINGBACK";
    case VM_PHASE_ANSWER_TONE:     return "ANSWER_TONE";
    case VM_PHASE_V8_CM_JM:        return "V8_CM_JM";
    case VM_PHASE_PROBE:           return "PROBE";
    case VM_PHASE_TRAIN:           return "TRAIN";
    case VM_PHASE_PARAM_EXCHANGE:  return "PARAM_EXCHANGE";
    case VM_PHASE_SILENCE:         return "SILENCE";
    case VM_PHASE_CONNECTED:       return "CONNECTED";
    case VM_PHASE_DISCONNECTING:   return "DISCONNECTING";
    default:                       return "?";
    }
}

const char *vm_strerror(vm_err_t e)
{
    switch (e) {
    case VM_OK:              return "OK";
    case VM_ERR_INVAL:       return "invalid argument";
    case VM_ERR_NOMEM:       return "out of memory";
    case VM_ERR_IO:          return "I/O error";
    case VM_ERR_NOTFOUND:    return "not found";
    case VM_ERR_TIMEOUT:     return "timeout";
    case VM_ERR_UNSUPPORTED: return "unsupported";
    case VM_ERR_STATE:       return "invalid state";
    default:                 return "unknown error";
    }
}
