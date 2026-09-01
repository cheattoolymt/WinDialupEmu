/*
 * vm_modem.c - モデム全体のオーケストレータ
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 設計の根拠 (難所 9/10/11) は include/vmodem/vm_modem.h に書いた。
 * ここはその設計を素直に実装したものであり、新しい判断はほとんど無い。
 * 「結線だけを担う」ことを徹底し、DSP・PPP・NAT の中身には触れない。
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "vmodem/vm_modem.h"
#include "vmodem/vm_serial.h"
#include "vmodem/vm_at.h"
#include "vmodem/vm_sequence.h"
#include "vmodem/vm_audio.h"
#include "vmodem/vm_ppp.h"
#include "vmodem/vm_nat.h"
#include "vmodem/vm_log.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

/* --------------------------------------------------------------------------
 * 定数
 * -------------------------------------------------------------------------- */

/* 1 周で音声リングに流し込む最大サンプル数 (20ms @ 8kHz = 160) */
#define VM_MODEM_RENDER_CHUNK   VM_DSP_BLOCK_SAMPLES

/* コマンド段で COM 読取をブロックする時間 (難所 10) */
#define VM_MODEM_COM_BLOCK_MS   200

/* データ段で NAT poll に許すブロック時間の上限 */
#define VM_MODEM_NAT_BLOCK_MS   20

/* COM から 1 周で読むバイト数。33.6kbps = 4.2KB/s なので 512 で十分 */
#define VM_MODEM_COM_CHUNK      512

/* ハングアップ時に PPP Terminate の完了を待つ上限 */
#define VM_MODEM_TERM_WAIT_MS   1500

/* --------------------------------------------------------------------------
 * 実体
 * -------------------------------------------------------------------------- */
struct vm_modem_s {
    const vm_config_t *cfg;         /* borrow (寿命は呼び出し側) */

    vm_serial_t   *ser;
    vm_audio_t    *aud;
    vm_nat_t      *nat;

    vm_at_t        at;
    vm_sequence_t  seq;
    vm_ppp_t       ppp;

    vm_modem_state_t state;

    /* 接続中の ISP と速度 */
    const vm_isp_entry_t *isp;
    vm_standard_t         std;
    int                   bps;

    /* 制御線の現在値 (無駄な再設定を避ける) */
    bool dcd;
    bool prev_dtr;

    /* HANGING_UP の期限 */
    uint32_t term_deadline_ms;

    /* 停止要求 (シグナルハンドラから触るので volatile) */
    volatile int stop;

    vm_modem_stats_t stats;

    /* 作業バッファ。スタックを食わないよう構造体に置く */
    uint8_t com_buf[VM_MODEM_COM_CHUNK];
    uint8_t resp_buf[512];
    float   pcm[VM_MODEM_RENDER_CHUNK];
    char    statbuf[256];
};

/* --------------------------------------------------------------------------
 * 単調増加ミリ秒クロック
 * -------------------------------------------------------------------------- */
uint32_t vm_modem_now_ms(void)
{
#ifdef _WIN32
    /*
     * GetTickCount() は 49.7 日で折り返すが、uint32_t の差分演算しか
     * 行わないので折り返しても正しく動く (符号なしの環状差分)。
     * とはいえ 64bit 版があるなら使わない理由が無い。
     */
    return (uint32_t)(GetTickCount64() & 0xFFFFFFFFull);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull +
                      (uint64_t)ts.tv_nsec / 1000000ull);
#endif
}

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    {
        struct timespec ts;
        ts.tv_sec  = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        (void)nanosleep(&ts, NULL);
    }
#endif
}

const char *vm_modem_state_name(vm_modem_state_t s)
{
    switch (s) {
    case VM_MODEM_COMMAND:     return "COMMAND";
    case VM_MODEM_DIALING:     return "DIALING";
    case VM_MODEM_ANSWERING:   return "ANSWERING";
    case VM_MODEM_DATA:        return "DATA";
    case VM_MODEM_HANGING_UP:  return "HANGING_UP";
    default:                   return "?";
    }
}

/* --------------------------------------------------------------------------
 * コールバック: PPP → COM (HDLC フレームの送出)
 * -------------------------------------------------------------------------- */
static int cb_ppp_write(void *user, const uint8_t *data, int len)
{
    vm_modem_t *m = (vm_modem_t *)user;
    int n = vm_serial_write(m->ser, data, len);
    if (n > 0) m->stats.com_tx_bytes += (uint64_t)n;
    return n;
}

/* --------------------------------------------------------------------------
 * コールバック: PPP → NAT (RAS が出した IP パケット)
 * -------------------------------------------------------------------------- */
static void cb_ppp_ip(void *user, const uint8_t *pkt, int len)
{
    vm_modem_t *m = (vm_modem_t *)user;
    if (m->nat == NULL) return;
    (void)vm_nat_input_ip(m->nat, pkt, len);
}

/* --------------------------------------------------------------------------
 * コールバック: NAT → PPP (インターネットから来た IP パケット)
 * -------------------------------------------------------------------------- */
static void cb_nat_ip(void *user, const uint8_t *pkt, int len)
{
    vm_modem_t *m = (vm_modem_t *)user;
    if (m->state != VM_MODEM_DATA) return;   /* 回線が無い間は捨てる */
    (void)vm_ppp_send_ip(&m->ppp, pkt, len);
}

/* --------------------------------------------------------------------------
 * DCD 操作 (難所 11)
 * -------------------------------------------------------------------------- */
static void set_dcd(vm_modem_t *m, bool on)
{
    if (m->dcd == on) return;
    m->dcd = on;
    /*
     * &C0 は「DCD 常時オン」。RAS はこの設定を使わないが、
     * TeraTerm 等で手動確認する時に邪魔にならないよう尊重する。
     */
    if (m->at.dcd_mode == 0) on = true;
    vm_serial_set_dcd(m->ser, on);
    VM_LOGD("modem: DCD %s", on ? "ON" : "OFF");
}

/* --------------------------------------------------------------------------
 * AT 応答を COM へ吐き出す
 * -------------------------------------------------------------------------- */
static void flush_at_responses(vm_modem_t *m)
{
    for (;;) {
        int n = vm_at_pop_response(&m->at, m->resp_buf, (int)sizeof(m->resp_buf));
        int w;
        if (n <= 0) break;
        w = vm_serial_write(m->ser, m->resp_buf, n);
        if (w > 0) m->stats.com_tx_bytes += (uint64_t)w;
    }
}

/* --------------------------------------------------------------------------
 * PPP / NAT 設定の組み立て
 * -------------------------------------------------------------------------- */
static void build_ppp_cfg(const vm_modem_t *m, const vm_isp_entry_t *isp,
                          vm_ppp_cfg_t *pc)
{
    const vm_config_t *c = m->cfg;
    uint32_t ip;

    vm_ppp_cfg_defaults(pc);

    if (c->ppp_server_ip[0] && vm_ipv4_parse(c->ppp_server_ip, &ip))
        pc->server_ip = ip;
    if (c->ppp_client_ip[0] && vm_ipv4_parse(c->ppp_client_ip, &ip))
        pc->client_ip = ip;
    if (c->ppp_dns1[0] && vm_ipv4_parse(c->ppp_dns1, &ip)) pc->dns1 = ip;
    if (c->ppp_dns2[0] && vm_ipv4_parse(c->ppp_dns2, &ip)) pc->dns2 = ip;

    /*
     * ★ ここが「PPP は繋がるのに名前解決できない」原因のひとつ ★
     *
     * slirp バックエンドの DNS 代理は、宛先が vnameserver
     * (既定 192.168.99.3) と **完全一致**した時だけ働く。
     * 8.8.8.8 を配ると、DNS クエリは代理されずただの外部 UDP として
     * NAT され、53/udp を塞ぐ環境や VPN 環境で解決できなくなる。
     * (詳細な根拠は vm_nat_dns_ip() の実装コメント)
     *
     * よって slirp を使っている時は config の dns1 を無視し、
     * NAT が持つ代理アドレスを配る。
     *
     * dns2 にも同じ代理アドレスを入れる。一見冗長だが理由がある:
     *
     *   - 代理は 1 つしか無いので、2 番目に別のアドレスを教えられない。
     *   - かといって dns2 = 0 にすると vm_ppp は DNS2 オプションを
     *     Config-Reject する。RAS は Reject を受けると設定を作り直して
     *     もう 1 往復するため、接続完了が目に見えて遅くなる
     *     (33.6kbps では 1 往復が数百 ms に効いてくる)。
     *   - 同じアドレスを 2 つ配っても Windows は同一サーバを 2 回引くだけで、
     *     動作上の不利益は無い。
     *
     * loopback / none バックエンドでは代理が無いので config の値を使う
     * (どうせ外に出られないが、設定が効かないより分かりやすい)。
     */
    if (m->nat != NULL && vm_nat_active_backend(m->nat) == VM_NAT_SLIRP) {
        uint32_t proxy = vm_nat_dns_ip(m->nat);
        if (proxy != 0u) {
            if (proxy != pc->dns1) {
                char abuf[16];
                VM_LOGI("ppp: DNS を slirp の代理 %s に差し替える "
                        "(config の dns1/dns2 は slirp では代理されないため)",
                        vm_ipv4_str(proxy, abuf, sizeof(abuf)));
            }
            pc->dns1 = proxy;
            pc->dns2 = proxy;
        }
    }

    /*
     * 認証は「config で要求されている」かつ「ISP エントリに
     * ユーザ名がある」場合のみ有効にする。
     * 片方だけ設定されている状態で PAP を要求すると、
     * RAS が資格情報ダイアログを出して延々失敗する。
     */
    pc->require_auth = false;
    if (c->ppp_require_auth && isp != NULL && isp->username[0] != '\0') {
        pc->require_auth = true;
        snprintf(pc->username, sizeof(pc->username), "%s", isp->username);
        snprintf(pc->password, sizeof(pc->password), "%s", isp->password);
    }
}

static void build_nat_cfg(const vm_modem_t *m, vm_nat_cfg_t *nc)
{
    const vm_config_t *c = m->cfg;
    uint32_t ip;

    vm_nat_cfg_defaults(nc);
    nc->backend = vm_nat_backend_from_string(c->net_mode);

    /*
     * ★ ここを間違えると「PPP は繋がるが通信できない」になる ★
     * PPP が RAS に配る client_ip と、NAT が想定する guest_ip は
     * 同一でなければならない。config で PPP 側だけ書き換えた場合に
     * 黙って不整合になるのを防ぐため、PPP 側を正とする。
     */
    if (c->ppp_client_ip[0] && vm_ipv4_parse(c->ppp_client_ip, &ip))
        nc->guest_ip = ip;
    if (c->ppp_server_ip[0] && vm_ipv4_parse(c->ppp_server_ip, &ip))
        nc->host_ip = ip;
    nc->network = nc->host_ip & nc->netmask;
    nc->dns_ip  = (nc->network & nc->netmask) | 3u;
}

/* --------------------------------------------------------------------------
 * 生成 / 破棄
 * -------------------------------------------------------------------------- */
vm_err_t vm_modem_create(vm_modem_t **out, const vm_config_t *cfg)
{
    vm_modem_t *m;
    vm_serial_params_t sp;
    vm_audio_params_t  ap;
    vm_nat_cfg_t       nc;
    char err[256];
    vm_err_t rc;

    if (out == NULL || cfg == NULL) return VM_ERR_INVAL;
    *out = NULL;

    m = (vm_modem_t *)calloc(1, sizeof(*m));
    if (m == NULL) return VM_ERR_NOMEM;

    m->cfg   = cfg;
    m->state = VM_MODEM_COMMAND;
    m->std   = VM_STD_UNKNOWN;

    /* --- COM ポート (これだけは必須。開けなければ起動できない) --- */
    vm_serial_params_defaults(&sp);
    sp.port = cfg->com_port;
    err[0] = '\0';
    rc = vm_serial_open(&m->ser, &sp, err, sizeof(err));
    if (rc != VM_OK) {
        VM_LOGE("modem: COM ポート '%s' を開けない: %s",
                cfg->com_port, err[0] ? err : "(理由不明)");
        free(m);
        return rc;
    }
    VM_LOGI("modem: シリアル %s (%s) を開いた",
            vm_serial_name(m->ser), vm_serial_backend_name(m->ser));

    /* --- 音声 (失敗しても続行。音が出なくてもダイアルは成立させる) --- */
    if (cfg->audio_enable) {
        vm_audio_params_defaults(&ap);
        ap.dsp_rate = VM_DSP_SAMPLE_RATE;
        ap.volume   = cfg->audio_volume;
        ap.device_match = (cfg->audio_device[0] != '\0')
                          ? cfg->audio_device : NULL;
        if (cfg->dump_wav) {
            ap.backend  = VM_AUDIO_BACKEND_WAVFILE;
            ap.wav_path = cfg->dump_wav_path;
        }
        if (vm_audio_open(&m->aud, &ap) != VM_OK) {
            VM_LOGW("modem: 音声デバイスを開けなかった。無音で続行する");
            m->aud = NULL;
        } else {
            VM_LOGI("modem: 音声 %s / %s / %d Hz",
                    vm_audio_backend_name(m->aud),
                    vm_audio_device_name(m->aud),
                    vm_audio_device_rate(m->aud));
        }
    } else {
        VM_LOGI("modem: audio_enable=false のため音声を開かない");
    }

    /* --- NAT (失敗したら LOOPBACK に落とす) --- */
    build_nat_cfg(m, &nc);
    if (nc.backend != VM_NAT_NONE) {
        if (vm_nat_create(&m->nat, &nc, cb_nat_ip, m) != VM_OK) {
            VM_LOGW("modem: NAT バックエンド '%s' が使えない。"
                    "内蔵ループバックに切り替える",
                    vm_nat_backend_name(nc.backend));
            nc.backend = VM_NAT_LOOPBACK;
            if (vm_nat_create(&m->nat, &nc, cb_nat_ip, m) != VM_OK)
                m->nat = NULL;
        }
        if (m->nat != NULL)
            VM_LOGI("modem: NAT = %s",
                    vm_nat_backend_name(vm_nat_active_backend(m->nat)));
    }

    /* --- AT 解釈器 --- */
    vm_at_init(&m->at, cfg);

    /*
     * 初期の制御線。
     * DSR/CTS は常時オン (実機のモデムも電源が入っていれば上がっている)。
     * DCD だけがキャリアに追従する。
     */
    vm_serial_set_dsr(m->ser, true);
    vm_serial_set_cts(m->ser, true);
    m->dcd = true;               /* set_dcd(false) を必ず通すため反転させておく */
    set_dcd(m, false);
    m->prev_dtr = vm_serial_get_dtr(m->ser);

    *out = m;
    return VM_OK;
}

void vm_modem_destroy(vm_modem_t *m)
{
    if (m == NULL) return;
    if (m->state == VM_MODEM_DATA)
        vm_ppp_close(&m->ppp, false, vm_modem_now_ms());
    if (m->nat != NULL) vm_nat_destroy(m->nat);
    if (m->aud != NULL) vm_audio_close(m->aud);
    if (m->ser != NULL) {
        set_dcd(m, false);
        vm_serial_close(m->ser);
    }
    free(m);
}

/* --------------------------------------------------------------------------
 * 状態遷移
 * -------------------------------------------------------------------------- */

/*
 * ハングアップ。難所 11 (3) の通り DCD を下げてから NO CARRIER を書く。
 */
static void do_hangup(vm_modem_t *m, bool emit_no_carrier)
{
    uint32_t now = vm_modem_now_ms();

    if (m->state == VM_MODEM_DATA || m->state == VM_MODEM_HANGING_UP) {
        vm_ppp_close(&m->ppp, false, now);
        if (m->nat != NULL) vm_nat_link_down(m->nat);
    }
    vm_sequence_abort(&m->seq);
    if (m->aud != NULL) vm_audio_flush(m->aud);

    /* ★順序★ 先に DCD を下げる */
    set_dcd(m, false);

    vm_at_set_online(&m->at, false);
    if (emit_no_carrier) vm_at_emit_result(&m->at, VM_AT_RESULT_NO_CARRIER);
    flush_at_responses(m);

    m->state = VM_MODEM_COMMAND;
    m->isp   = NULL;
    m->bps   = 0;
    m->stats.hangups++;
    VM_LOGI("modem: ハングアップ (COMMAND へ)");
}

/*
 * 音響シーケンス完了 → CONNECT を返して PPP を開始する。
 * 難所 11 (1): CONNECT を書き、flush し、その後で DCD を上げる。
 */
static void enter_data_mode(vm_modem_t *m)
{
    vm_ppp_cfg_t pc;
    uint32_t now = vm_modem_now_ms();

    if (m->aud != NULL) {
        /* 最後の無音まで鳴らし切る。ここを飛ばすとネゴ音が途中で切れる */
        vm_audio_drain(m->aud, 500);
    }

    vm_at_emit_connect(&m->at, m->bps);
    flush_at_responses(m);
    vm_serial_flush(m->ser);      /* ★ CONNECT を確実に相手へ届ける ★ */

    build_ppp_cfg(m, m->isp, &pc);
    if (vm_ppp_init(&m->ppp, &pc, cb_ppp_write, m, cb_ppp_ip, m, now) != VM_OK) {
        VM_LOGE("modem: PPP を初期化できない");
        do_hangup(m, true);
        return;
    }
    if (m->nat != NULL) (void)vm_nat_link_up(m->nat);

    vm_at_set_online(&m->at, true);
    m->state = VM_MODEM_DATA;
    m->stats.connects++;
    m->stats.last_connect_bps = (uint32_t)m->bps;

    /* ★順序★ CONNECT を書き切った後で DCD を上げる */
    set_dcd(m, true);

    /*
     * ここで我々から LCP Configure-Request を送る。
     * 「相手が先に送るのを待つ」実装にすると双方が黙る (vm_ppp.h 参照)。
     */
    (void)vm_ppp_open(&m->ppp, now);

    VM_LOGI("modem: CONNECT %d (%s) — データ段へ",
            m->bps, vm_standard_name(m->std));
}

/*
 * ATDT の処理。番号が config.ini に無ければ NO CARRIER (仕様)。
 */
static void start_dial(vm_modem_t *m, const char *number)
{
    const vm_isp_entry_t *isp;
    vm_standard_t std = VM_STD_UNKNOWN;
    int bps;

    m->stats.dials++;

    isp = vm_config_match_number(m->cfg, number);
    if (isp == NULL) {
        char norm[VM_MAX_NUMBER_LEN];
        vm_number_normalize(number, norm, sizeof(norm));
        VM_LOGI("modem: 番号 '%s' (正規化 '%s') は config.ini に無い "
                "→ NO CARRIER", number, norm);
        m->stats.dials_rejected++;
        /*
         * 実機と同じく、少しだけ待ってから NO CARRIER を返す。
         * 即座に返すとダイアラが「ポートが壊れている」と判断する事がある。
         */
        vm_at_emit_result(&m->at, VM_AT_RESULT_NO_CARRIER);
        flush_at_responses(m);
        return;
    }

    bps = vm_at_negotiated_bps(&m->at, isp, &std);
    if (bps <= 0) { bps = isp->speed; std = isp->protocol; }

    m->isp = isp;
    m->std = std;
    m->bps = bps;

    if (vm_sequence_start_dial(&m->seq, number, isp, m->cfg, std, bps,
                               VM_DSP_SAMPLE_RATE,
                               m->at.speaker_on ? m->cfg->audio_volume : 0.0f)
        != VM_OK) {
        VM_LOGE("modem: 音響シーケンスを組めない");
        vm_at_emit_result(&m->at, VM_AT_RESULT_ERROR);
        flush_at_responses(m);
        m->isp = NULL;
        return;
    }
    vm_sequence_set_muted(&m->seq, !m->at.speaker_on);

    m->state = VM_MODEM_DIALING;
    VM_LOGI("modem: ダイアル開始 '%s' → %s %d bps (%s, 全体 %d ms)",
            number, isp->section, bps, vm_standard_name(std),
            vm_sequence_total_ms(&m->seq));
}

static void start_answer(vm_modem_t *m)
{
    const vm_isp_entry_t *isp =
        (m->cfg->isp_count > 0) ? &m->cfg->isp[0] : NULL;
    vm_standard_t std = (isp != NULL) ? isp->protocol : VM_STD_V34PLUS;
    int bps = (isp != NULL) ? isp->speed : 33600;

    if (isp != NULL) {
        int n = vm_at_negotiated_bps(&m->at, isp, &std);
        if (n > 0) bps = n;
    }
    m->isp = isp;
    m->std = std;
    m->bps = bps;

    if (vm_sequence_start_answer(&m->seq, m->cfg, std, bps,
                                 VM_DSP_SAMPLE_RATE,
                                 m->at.speaker_on ? m->cfg->audio_volume : 0.0f)
        != VM_OK) {
        vm_at_emit_result(&m->at, VM_AT_RESULT_ERROR);
        flush_at_responses(m);
        return;
    }
    vm_sequence_set_muted(&m->seq, !m->at.speaker_on);
    m->state = VM_MODEM_ANSWERING;
    VM_LOGI("modem: 着信応答シーケンス開始 (%s %d bps)",
            vm_standard_name(std), bps);
}

/* --------------------------------------------------------------------------
 * AT アクションの処理
 * -------------------------------------------------------------------------- */
static void handle_at_action(vm_modem_t *m)
{
    vm_at_action_t act = vm_at_take_action(&m->at);
    switch (act) {
    case VM_AT_ACTION_NONE:
        break;

    case VM_AT_ACTION_DIAL:
        if (m->state != VM_MODEM_COMMAND) {
            vm_at_emit_result(&m->at, VM_AT_RESULT_ERROR);
            flush_at_responses(m);
        } else {
            start_dial(m, m->at.action_number);
        }
        break;

    case VM_AT_ACTION_ANSWER:
        if (m->state != VM_MODEM_COMMAND) {
            vm_at_emit_result(&m->at, VM_AT_RESULT_ERROR);
            flush_at_responses(m);
        } else {
            start_answer(m);
        }
        break;

    case VM_AT_ACTION_HANGUP:
        if (m->state == VM_MODEM_COMMAND) {
            /* 既にオンフック。ATH は OK を返すだけ (vm_at が積んでいる) */
            flush_at_responses(m);
        } else if (m->state == VM_MODEM_DATA) {
            /*
             * データ段からの ATH (+++ATH)。
             * PPP を礼儀正しく落とす: Terminate-Request を送り、
             * Ack を待ってから NO CARRIER を返す。
             */
            vm_ppp_close(&m->ppp, true, vm_modem_now_ms());
            m->state = VM_MODEM_HANGING_UP;
            m->term_deadline_ms = vm_modem_now_ms() + VM_MODEM_TERM_WAIT_MS;
            VM_LOGI("modem: PPP Terminate 送信、Ack を待つ");
        } else {
            do_hangup(m, true);
        }
        break;

    case VM_AT_ACTION_RESET:
        if (m->state != VM_MODEM_COMMAND) do_hangup(m, false);
        flush_at_responses(m);
        break;

    case VM_AT_ACTION_ONLINE:
        if (m->state == VM_MODEM_DATA) {
            vm_at_set_online(&m->at, true);
            vm_at_emit_connect(&m->at, m->bps);
        } else {
            vm_at_emit_result(&m->at, VM_AT_RESULT_ERROR);
        }
        flush_at_responses(m);
        break;
    }
}

/* --------------------------------------------------------------------------
 * COM 読取 → AT / PPP
 * -------------------------------------------------------------------------- */
static void pump_com(vm_modem_t *m, int timeout_ms, uint32_t now)
{
    int n = vm_serial_read(m->ser, m->com_buf, (int)sizeof(m->com_buf),
                           timeout_ms);
    if (n <= 0) return;

    m->stats.com_rx_bytes += (uint64_t)n;

    vm_at_tick(&m->at, now);
    m->at.passthru = NULL;
    m->at.passthru_len = 0;

    (void)vm_at_feed(&m->at, m->com_buf, n);

    /*
     * オンライン中なら passthru に「PPP へ渡すべき範囲」が入る。
     * ★ vm_at_feed が返したポインタは com_buf を指しているので、
     *   vm_ppp_input を呼ぶ前に別の読み取りをしてはいけない ★
     */
    if (m->at.passthru != NULL && m->at.passthru_len > 0 &&
        m->state == VM_MODEM_DATA) {
        vm_ppp_input(&m->ppp, m->at.passthru, m->at.passthru_len, now);
    }

    flush_at_responses(m);
    handle_at_action(m);
}

/* --------------------------------------------------------------------------
 * DTR 落ちの検出 (難所 11 (2))
 * -------------------------------------------------------------------------- */
static void check_dtr(vm_modem_t *m)
{
    bool dtr = vm_serial_get_dtr(m->ser);
    if (m->prev_dtr && !dtr) {
        VM_LOGI("modem: DTR が落ちた (&D%d)", m->at.dtr_mode);
        if (m->at.dtr_mode != 0 && m->state != VM_MODEM_COMMAND) {
            /*
             * RAS がポートを閉じた。NO CARRIER を書いても読む相手が
             * いないので、文字列は出さずに静かに切る。
             */
            do_hangup(m, false);
            vm_serial_purge_rx(m->ser);
        }
    }
    m->prev_dtr = dtr;
}

/* --------------------------------------------------------------------------
 * 音響シーケンスの描画
 * -------------------------------------------------------------------------- */
static bool render_sequence(vm_modem_t *m)
{
    bool done = false;

    if (m->aud == NULL) {
        /*
         * 音声デバイスが無い場合も「シーケンスの時間は消費する」。
         * 即完了させると実機と時間感覚が変わり、RAS のタイムアウト
         * 挙動の検証ができなくなる。
         * 20ms 分描画して 20ms 眠る、という素朴な方法で足りる。
         */
        done = vm_sequence_render(&m->seq, m->pcm, VM_MODEM_RENDER_CHUNK);
        sleep_ms((VM_MODEM_RENDER_CHUNK * 1000) / VM_DSP_SAMPLE_RATE);
        return done;
    }

    /* リングに空きがある限り詰める */
    while (vm_audio_space(m->aud) >= (uint32_t)VM_MODEM_RENDER_CHUNK) {
        done = vm_sequence_render(&m->seq, m->pcm, VM_MODEM_RENDER_CHUNK);
        (void)vm_audio_write(m->aud, m->pcm, (uint32_t)VM_MODEM_RENDER_CHUNK);
        if (done) break;
    }

    /*
     * ★ 難所 10 のブロック点 ★
     * リングが半分以下になるまで待つ。ここで待つので COM は覗くだけ。
     */
    vm_audio_wait_drain(m->aud,
                        (uint32_t)(VM_DSP_SAMPLE_RATE / 8),  /* 125ms 分 */
                        50);
    return done;
}

/* --------------------------------------------------------------------------
 * イベントループ 1 周
 * -------------------------------------------------------------------------- */
vm_err_t vm_modem_step(vm_modem_t *m)
{
    uint32_t now;

    if (m == NULL) return VM_ERR_INVAL;
    if (m->stop) return VM_ERR_STATE;

    m->stats.loops++;
    now = vm_modem_now_ms();

    check_dtr(m);

    switch (m->state) {

    /* ---------------------------------------------------------------- */
    case VM_MODEM_COMMAND:
        /*
         * 唯一のブロック点 = COM 読取。
         * ユーザのキー入力待ちであり、他に急ぐ仕事は無い。
         */
        pump_com(m, VM_MODEM_COM_BLOCK_MS, now);
        break;

    /* ---------------------------------------------------------------- */
    case VM_MODEM_DIALING:
    case VM_MODEM_ANSWERING:
        /* COM は覗くだけ (ATH での中断を拾うため) */
        pump_com(m, 0, now);
        if (m->state != VM_MODEM_DIALING && m->state != VM_MODEM_ANSWERING)
            break;                        /* pump_com 内で中断された */

        if (m->seq.aborted) { do_hangup(m, true); break; }

        /* 唯一のブロック点 = 音声リングの空き待ち */
        if (render_sequence(m)) {
            enter_data_mode(m);
        }
        break;

    /* ---------------------------------------------------------------- */
    case VM_MODEM_DATA:
        /* COM は覗くだけ (難所 10: 「COM は覗く、待つのは NAT」) */
        pump_com(m, 0, now);
        if (m->state != VM_MODEM_DATA) break;

        vm_ppp_tick(&m->ppp, now);

        if (m->ppp.state == VM_PPP_DEAD) {
            VM_LOGI("modem: PPP が DEAD になった");
            do_hangup(m, true);
            break;
        }

        /* 唯一のブロック点 = NAT の poll */
        if (m->nat != NULL) {
            (void)vm_nat_poll(m->nat, VM_MODEM_NAT_BLOCK_MS);
        } else {
            sleep_ms(5);
        }
        break;

    /* ---------------------------------------------------------------- */
    case VM_MODEM_HANGING_UP:
        pump_com(m, 0, now);
        vm_ppp_tick(&m->ppp, now);
        if (m->ppp.state == VM_PPP_DEAD ||
            (int32_t)(now - m->term_deadline_ms) >= 0) {
            do_hangup(m, true);
        } else {
            sleep_ms(5);
        }
        break;

    default:
        return VM_ERR_STATE;
    }

    return VM_OK;
}

vm_err_t vm_modem_run(vm_modem_t *m)
{
    if (m == NULL) return VM_ERR_INVAL;
    VM_LOGI("modem: イベントループ開始 (単一スレッド)");
    while (!m->stop) {
        vm_err_t rc = vm_modem_step(m);
        if (rc != VM_OK && !m->stop) return rc;
    }
    VM_LOGI("modem: 停止要求を受けた");
    if (m->state != VM_MODEM_COMMAND) do_hangup(m, false);
    return VM_OK;
}

void vm_modem_request_stop(vm_modem_t *m)
{
    if (m != NULL) m->stop = 1;
}

vm_modem_state_t vm_modem_state(const vm_modem_t *m)
{
    return (m != NULL) ? m->state : VM_MODEM_COMMAND;
}

void vm_modem_get_stats(const vm_modem_t *m, vm_modem_stats_t *st)
{
    if (m == NULL || st == NULL) return;
    *st = m->stats;
}

const char *vm_modem_status(const vm_modem_t *m, char *buf, size_t size)
{
    if (buf == NULL || size == 0) return "";
    if (m == NULL) { buf[0] = '\0'; return buf; }
    snprintf(buf, size,
             "%s dcd=%d ppp=%s bps=%d loops=%llu rx=%llu tx=%llu "
             "dial=%llu/%llu conn=%llu",
             vm_modem_state_name(m->state), m->dcd ? 1 : 0,
             (m->state == VM_MODEM_DATA || m->state == VM_MODEM_HANGING_UP)
                 ? vm_ppp_state_name(m->ppp.state) : "-",
             m->bps,
             (unsigned long long)m->stats.loops,
             (unsigned long long)m->stats.com_rx_bytes,
             (unsigned long long)m->stats.com_tx_bytes,
             (unsigned long long)(m->stats.dials - m->stats.dials_rejected),
             (unsigned long long)m->stats.dials,
             (unsigned long long)m->stats.connects);
    return buf;
}
