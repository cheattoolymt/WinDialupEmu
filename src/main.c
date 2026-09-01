/*
 * main.c - VModem エントリポイント
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * やることは 4 つだけ。
 *   1. config.ini を読む
 *   2. ログを初期化する
 *   3. Ctrl-C ハンドラを設置する
 *   4. vm_modem_run() を呼ぶ
 *
 * ★ Ctrl-C の扱いについて ★
 * Windows の SetConsoleCtrlHandler は **別スレッド** でハンドラを呼ぶ。
 * POSIX の signal はシグナルコンテキストで呼ぶ。どちらの場合も
 * 「そこで後片付けをする」のは危険 (COM を閉じている最中に
 *  イベントループが同じハンドルを触る)。
 * したがってハンドラでは vm_modem_request_stop() でフラグを立てるだけにし、
 * 実際の後片付けはイベントループが自分で抜けてから main が行う。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ★順序厳守★
 * vm_winsock.h は winsock2.h -> ws2tcpip.h -> windows.h の順を保証する。
 * windows.h を先に通すと古い winsock.h (Winsock 1.1) が入り、
 * 後から winsock2.h を読んだ翻訳単位と型が食い違う。
 * したがって **他のどのヘッダより先に** これを置く。
 */
#include "vmodem/vm_winsock.h"

#include "vmodem/vm_modem.h"
#include "vmodem/vm_config.h"
#include "vmodem/vm_log.h"
#include "vmodem/vm_nat.h"

#ifndef _WIN32
#  include <signal.h>
#endif

static vm_modem_t *g_modem = NULL;

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        fprintf(stderr, "\n終了要求を受けました。切断処理をします...\n");
        vm_modem_request_stop(g_modem);
        return TRUE;
    default:
        return FALSE;
    }
}
#else
static void on_signal(int sig)
{
    (void)sig;
    vm_modem_request_stop(g_modem);
}
#endif

static void usage(const char *argv0)
{
    printf(
"VModem - Windows 用 本物のダイアルアップモデムエミュレータ\n"
"\n"
"使い方: %s [オプション]\n"
"\n"
"  -c, --config <path>   設定ファイル (既定: config.ini)\n"
"  -p, --port <name>     COM ポート名を上書き (例: CNCB0)\n"
"  -v, --verbose         ログレベルを DEBUG に\n"
"      --trace           ログレベルを TRACE に (バイト単位のダンプ)\n"
"  -q, --quiet           音を出さない\n"
"      --wav <path>      合成音を WAV に書き出す (デバッグ用)\n"
"      --net <mode>      slirp | loopback | none\n"
"  -h, --help            このヘルプ\n"
"\n"
"設定例 (config.ini):\n"
"  [ISP_1]\n"
"  number   = 0120-000-0000\n"
"  speed    = 33600\n"
"  protocol = V34PLUS\n"
"\n", argv0);
}

int main(int argc, char **argv)
{
    const char *cfg_path = "config.ini";
    const char *port_override = NULL;
    const char *net_override  = NULL;
    const char *wav_override  = NULL;
    int  level_override = -1;
    int  quiet = 0;
    vm_config_t cfg;
    char errbuf[256];
    char status[256];
    vm_err_t rc;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i + 1 < argc)
            cfg_path = argv[++i];
        else if ((!strcmp(a, "-p") || !strcmp(a, "--port")) && i + 1 < argc)
            port_override = argv[++i];
        else if (!strcmp(a, "--net") && i + 1 < argc)
            net_override = argv[++i];
        else if (!strcmp(a, "--wav") && i + 1 < argc)
            wav_override = argv[++i];
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose"))
            level_override = VM_LOG_DEBUG;
        else if (!strcmp(a, "--trace"))
            level_override = VM_LOG_TRACE;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet"))
            quiet = 1;
        else {
            fprintf(stderr, "不明なオプション: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    /* --- 設定 --- */
    vm_config_defaults(&cfg);
    errbuf[0] = '\0';
    rc = vm_config_load(&cfg, cfg_path, errbuf, sizeof(errbuf));
    if (rc != VM_OK) {
        /*
         * 設定が読めなくても既定値で起動できるようにする。
         * 「まず動かして音を聞きたい」というのが最初の使い方なので、
         * ここで終了するとハードルが上がる。
         */
        fprintf(stderr,
                "警告: '%s' を読み込めませんでした (%s)。既定値で起動します。\n",
                cfg_path, errbuf[0] ? errbuf : "理由不明");
    }

    if (port_override != NULL)
        snprintf(cfg.com_port, sizeof(cfg.com_port), "%s", port_override);
    if (net_override != NULL)
        snprintf(cfg.net_mode, sizeof(cfg.net_mode), "%s", net_override);
    if (wav_override != NULL) {
        cfg.dump_wav = true;
        snprintf(cfg.dump_wav_path, sizeof(cfg.dump_wav_path), "%s", wav_override);
    }
    if (quiet) cfg.audio_enable = false;
    if (level_override >= 0) cfg.log_level = level_override;

    /* --- ログ --- */
    vm_log_init((vm_log_level_t)cfg.log_level,
                cfg.log_file[0] ? cfg.log_file : NULL);

    /* --- Winsock ---
     *
     * ★必ず vm_modem_create() より前★
     *
     * Windows では WSAStartup() を呼ぶまで socket() / sendto() /
     * WSAPoll() が WSANOTINITIALISED (10093) で全部失敗する。
     *
     * libslirp も内部で WSAStartup(MAKEWORD(2, 0)) を呼ぶが、
     *   - 要求バージョンが 2.0 で、WSAPoll は 2.2 の API
     *   - atexit() で WSACleanup() する
     * ため、我々が先に 2.2 で初期化し、自分の参照カウントを持つ。
     * 詳細は include/vmodem/vm_winsock.h 冒頭「罠 2」。
     *
     * ログ初期化より後に置いているのは、失敗理由をログに残すため。
     */
    errbuf[0] = '\0';
    if (vm_winsock_init(errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr,
                "エラー: Winsock を初期化できませんでした (%s)\n",
                errbuf[0] ? errbuf : "理由不明");
        VM_LOGE("winsock: 初期化失敗 (%s)", errbuf[0] ? errbuf : "理由不明");
        return 1;
    }

    VM_LOGI("VModem 起動 (config=%s port=%s net=%s isp=%d 件)",
            cfg_path, cfg.com_port, cfg.net_mode, cfg.isp_count);
    for (i = 0; i < cfg.isp_count; i++) {
        VM_LOGI("  [%s] %s (正規化 %s) %d bps %s",
                cfg.isp[i].section, cfg.isp[i].number_raw,
                cfg.isp[i].number_norm, cfg.isp[i].speed,
                vm_standard_name(cfg.isp[i].protocol));
    }
    if (cfg.isp_count == 0)
        VM_LOGW("ISP エントリが 0 件です。どの番号にダイアルしても "
                "NO CARRIER になります");

    /* --- モデム --- */
    rc = vm_modem_create(&g_modem, &cfg);
    if (rc != VM_OK) {
        fprintf(stderr,
                "エラー: モデムを初期化できませんでした (%s)\n"
                "COM ポート '%s' が存在し、他のプロセスが開いていないか\n"
                "確認してください (com0com のセットアップは\n"
                "scripts/setup-com0com.ps1 を参照)。\n",
                vm_strerror(rc), cfg.com_port);
        vm_winsock_cleanup();
        return 1;
    }

    /* --- Ctrl-C --- */
#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
#endif

    printf("VModem 待機中。Windows のダイアルアップ接続から発信してください。\n"
           "終了は Ctrl-C。\n");
    fflush(stdout);

    rc = vm_modem_run(g_modem);

    VM_LOGI("最終状態: %s", vm_modem_status(g_modem, status, sizeof(status)));
    vm_modem_destroy(g_modem);
    g_modem = NULL;

    vm_winsock_cleanup();

    return (rc == VM_OK) ? 0 : 1;
}
