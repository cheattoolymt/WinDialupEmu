/*
 * vm_config.h - config.ini パーサと仮想電話番号マッチング
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef VMODEM_VM_CONFIG_H
#define VMODEM_VM_CONFIG_H

#include "vmodem/vm_types.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define VM_MAX_ISP_ENTRIES     32
#define VM_MAX_NUMBER_LEN      64
#define VM_MAX_NAME_LEN        64
#define VM_MAX_PATH_LEN        512

/*
 * 1 つの仮想 ISP エントリ ([ISP_1] セクション相当)
 */
typedef struct {
    char          section[VM_MAX_NAME_LEN];   /* "ISP_1"                       */
    char          number_raw[VM_MAX_NUMBER_LEN];  /* "0120-000-0000" 原文     */
    char          number_norm[VM_MAX_NUMBER_LEN]; /* "01200000000" 数字のみ   */
    int           speed;                     /* 33600                         */
    vm_standard_t protocol;                  /* VM_STD_V34PLUS                */

    /* PPP 認証 (省略可: 空なら認証なし) */
    char          username[VM_MAX_NAME_LEN];
    char          password[VM_MAX_NAME_LEN];
} vm_isp_entry_t;

/*
 * グローバル設定
 */
typedef struct {
    /* --- [general] --- */
    char  com_port[32];        /* "CNCB0" : 我々が開く側のポート */
    bool  audio_enable;
    float audio_volume;        /* 0.0 - 1.0 */
    char  audio_device[VM_MAX_NAME_LEN];  /* "" = 既定デバイス */
    int   log_level;           /* 0=none 1=err 2=warn 3=info 4=debug 5=trace */
    char  log_file[VM_MAX_PATH_LEN];

    /* WAV ダンプ (デバッグ用: 合成した音を検証するため) */
    bool  dump_wav;
    char  dump_wav_path[VM_MAX_PATH_LEN];

    /* --- [timing] --- 各フェーズの長さ(ms)。0 は既定値 --- */
    int   dialtone_ms;         /* 既定 2000 */
    int   dtmf_on_ms;          /* 既定 80   */
    int   dtmf_off_ms;         /* 既定 80   */
    int   ringback_count;      /* 既定 2 回 */
    bool  fast_connect;        /* true: ハンドシェイク音を短縮（テスト用） */

    /* --- [ppp] --- */
    char  ppp_server_ip[32];   /* "192.168.99.1"  我々(モデム側) */
    char  ppp_client_ip[32];   /* "192.168.99.2"  Windows RAS 側 */
    char  ppp_dns1[32];
    char  ppp_dns2[32];
    bool  ppp_require_auth;

    /* --- [network] --- */
    /* "slirp" = libslirp ユーザモード NAT / "none" = PPP のみ */
    char  net_mode[32];

    /* --- ISP エントリ --- */
    vm_isp_entry_t isp[VM_MAX_ISP_ENTRIES];
    int            isp_count;
} vm_config_t;

/* 既定値を設定 */
void vm_config_defaults(vm_config_t *cfg);

/*
 * config.ini を読み込む。
 * 戻り値 VM_OK で成功。パースエラーは errbuf に格納。
 */
vm_err_t vm_config_load(vm_config_t *cfg, const char *path,
                        char *errbuf, size_t errbuf_size);

/*
 * ---------------------------------------------------------------------------
 * 番号正規化とマッチング
 * ---------------------------------------------------------------------------
 * 仕様:
 *   - ハイフンを無視して数字のみで照合する
 *   - 桁数・フォーマットは自由 ("000" / "0120-000-0000" / "0000-00-00000")
 *
 * vm_number_normalize() は数字以外 (ハイフン・空白・括弧・
 * ATDT のダイアル修飾子 W , ; ! @ T P など) をすべて除去する。
 */
void vm_number_normalize(const char *in, char *out, size_t out_size);

/*
 * ダイアルされた番号に一致する ISP エントリを返す。
 * 見つからなければ NULL (=> NO CARRIER を返すべき)。
 */
const vm_isp_entry_t *vm_config_match_number(const vm_config_t *cfg,
                                             const char *dialed);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_CONFIG_H */
