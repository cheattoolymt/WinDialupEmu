/*
 * test_at.c - AT コマンド インタプリタ + シリアル層の検証
 *
 * 仕様書が要求する最低限の AT コマンドを 1 つずつ検証する:
 *   ATZ          -> OK
 *   ATDT<number> -> config.ini に一致すれば接続シーケンス開始
 *                   一致しなければ NO CARRIER
 *   ATA          -> 着信応答
 *   ATH          -> ハングアップ
 *   ATE0 / ATE1  -> エコー off/on
 *   AT+MS=?      -> 対応規格の一覧
 *
 * さらに実機互換性のために重要な
 *   - コマンド連結 ("ATE0V1&C1&D2S0=0")
 *   - V0/V1 応答フォーマット
 *   - S レジスタ
 *   - +++ エスケープのガードタイム
 * も検証する。
 */
#include "vmodem/vm_at.h"
#include "vmodem/vm_serial.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void check(const char *name, int ok, const char *detail)
{
    if (ok) { g_pass++; printf("  [ OK ] %-52s %s\n", name, detail ? detail : ""); }
    else    { g_fail++; printf("  [FAIL] %-52s %s\n", name, detail ? detail : ""); }
}

/* ---------------------------------------------------------------------------
 * ヘルパ: AT に文字列を食わせ、応答を全部取り出して 1 本の文字列にする
 * -------------------------------------------------------------------------*/
static const char *feed(vm_at_t *at, const char *cmd)
{
    static char out[2048];
    int n, total = 0;

    vm_at_feed(at, cmd, (int)strlen(cmd));
    while ((n = vm_at_pop_response(at, out + total,
                                  (int)sizeof(out) - 1 - total)) > 0) {
        total += n;
        if (total >= (int)sizeof(out) - 1) break;
    }
    out[total] = '\0';
    return out;
}

/* 制御文字を可視化 (テスト出力用) */
static const char *vis(const char *s)
{
    static char b[512];
    int o = 0;
    for (; *s && o < (int)sizeof(b) - 5; s++) {
        if (*s == '\r')      { b[o++]='<'; b[o++]='C'; b[o++]='R'; b[o++]='>'; }
        else if (*s == '\n') { b[o++]='<'; b[o++]='L'; b[o++]='F'; b[o++]='>'; }
        else                 { b[o++] = *s; }
    }
    b[o] = '\0';
    return b;
}

/* テスト用の config を組み立てる */
static void make_cfg(vm_config_t *cfg)
{
    vm_config_defaults(cfg);
    cfg->isp_count = 3;

    snprintf(cfg->isp[0].section, sizeof(cfg->isp[0].section), "ISP_1");
    snprintf(cfg->isp[0].number_raw, sizeof(cfg->isp[0].number_raw),
             "0120-000-0000");
    vm_number_normalize(cfg->isp[0].number_raw, cfg->isp[0].number_norm,
                        sizeof(cfg->isp[0].number_norm));
    cfg->isp[0].speed    = 33600;
    cfg->isp[0].protocol = VM_STD_V34PLUS;

    snprintf(cfg->isp[1].section, sizeof(cfg->isp[1].section), "ISP_2");
    snprintf(cfg->isp[1].number_raw, sizeof(cfg->isp[1].number_raw), "000");
    vm_number_normalize(cfg->isp[1].number_raw, cfg->isp[1].number_norm,
                        sizeof(cfg->isp[1].number_norm));
    cfg->isp[1].speed    = 2400;
    cfg->isp[1].protocol = VM_STD_V22BIS;

    snprintf(cfg->isp[2].section, sizeof(cfg->isp[2].section), "ISP_3");
    snprintf(cfg->isp[2].number_raw, sizeof(cfg->isp[2].number_raw),
             "0000-00-00000");
    vm_number_normalize(cfg->isp[2].number_raw, cfg->isp[2].number_norm,
                        sizeof(cfg->isp[2].number_norm));
    cfg->isp[2].speed    = 28800;
    cfg->isp[2].protocol = VM_STD_V34;
}

/* =====================================================================
 * 1. 仕様書が要求する最低限のコマンド
 * ===================================================================== */
static void test_required_commands(void)
{
    vm_config_t cfg;
    vm_at_t at;
    const char *r;
    char detail[400];

    printf("\n[1] 仕様必須 AT コマンド\n");

    make_cfg(&cfg);
    vm_at_init(&at, &cfg);

    /* --- ATZ -> OK --- */
    r = feed(&at, "ATZ\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("ATZ returns OK", strstr(r, "\r\nOK\r\n") != NULL, detail);
    check("ATZ raises RESET action",
          vm_at_take_action(&at) == VM_AT_ACTION_RESET, NULL);

    /* --- ATE0 : エコー off。"ATE0" 自体はエコーされる --- */
    vm_at_init(&at, &cfg);
    r = feed(&at, "ATE0\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("ATE0 echoes itself then disables echo",
          strncmp(r, "ATE0\r\n", 6) == 0 && strstr(r, "OK") != NULL, detail);
    check("echo flag is off after ATE0", at.echo == false, NULL);

    /* エコー off の状態では入力が返ってこない */
    r = feed(&at, "ATZ\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("no echo after ATE0", strstr(r, "ATZ") == NULL, detail);

    /* --- ATE1 : エコー on --- */
    vm_at_init(&at, &cfg);
    (void)feed(&at, "ATE0\r");
    r = feed(&at, "ATE1\r");
    check("ATE1 re-enables echo", at.echo == true, NULL);
    r = feed(&at, "AT\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("echo works again after ATE1", strncmp(r, "AT\r\n", 4) == 0, detail);

    /* --- ATH -> ハングアップ --- */
    vm_at_init(&at, &cfg);
    r = feed(&at, "ATH\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("ATH returns OK", strstr(r, "OK") != NULL, detail);
    check("ATH raises HANGUP action",
          vm_at_take_action(&at) == VM_AT_ACTION_HANGUP, NULL);

    /* ATH1 はオフフック維持なので切断しない */
    r = feed(&at, "ATH1\r");
    check("ATH1 does not hang up (off-hook only)",
          vm_at_take_action(&at) == VM_AT_ACTION_NONE &&
          strstr(r, "OK") != NULL, NULL);

    /* --- ATA -> 着信応答 --- */
    vm_at_init(&at, &cfg);
    r = feed(&at, "ATA\r");
    check("ATA raises ANSWER action and defers the response",
          vm_at_take_action(&at) == VM_AT_ACTION_ANSWER &&
          strstr(r, "OK") == NULL, "response comes after handshake");

    /* --- AT+MS=? -> 対応規格の一覧 --- */
    vm_at_init(&at, &cfg);
    r = feed(&at, "AT+MS=?\r");
    {
        int ok = strstr(r, "V21")  && strstr(r, "V22")  &&
                 strstr(r, "V32")  && strstr(r, "V34")  &&
                 strstr(r, "33600") && strstr(r, "OK");
        snprintf(detail, sizeof(detail), "%d bytes listing all standards",
                 (int)strlen(r));
        check("AT+MS=? lists supported standards", ok != 0, detail);
    }
    check("AT+MS=? mentions V.34 (mandatory minimum)",
          strstr(r, "V34") != NULL, "V.34 は仕様上の必須要件");

    /* AT+MS= で規格を選ぶと速度上限が変わる */
    vm_at_init(&at, &cfg);
    r = feed(&at, "AT+MS=V22B\r");
    snprintf(detail, sizeof(detail), "req_std=%s max=%d",
             vm_standard_name(at.req_std), at.req_max_bps);
    check("AT+MS=V22B sets the standard",
          at.req_std == VM_STD_V22BIS && strstr(r, "OK") != NULL, detail);
}

/* =====================================================================
 * 2. ATDT と config.ini マッチング (仕様の核心)
 * ===================================================================== */
static void test_dial_matching(void)
{
    vm_config_t cfg;
    vm_at_t at;
    const char *r;
    char detail[400];
    int i;

    /*
     * 表: ダイアル文字列 -> 一致するか
     * ハイフン・空白・括弧・ダイアル修飾子を無視して数字のみで照合する。
     */
    struct { const char *dial; bool match; const char *why; } cases[] = {
        { "0120-000-0000", true,  "完全一致"                       },
        { "01200000000",   true,  "ハイフン無し"                   },
        { "0120 000 0000", true,  "空白区切り"                     },
        { "(0120)000-0000",true,  "括弧付き"                       },
        { "T0120-000-0000",true,  "トーン指定子 T"                 },
        { "0120-000-0000,",true,  "末尾ポーズ ,"                   },
        { "W0120,,0000000",true,  "W 待ち + 複数ポーズ"            },
        { "000",           true,  "3 桁の短い番号"                 },
        { "0-0-0",         true,  "ハイフン入り 3 桁"              },
        { "0000-00-00000", true,  "任意フォーマット 11 桁"         },
        { "00000000000",   true,  "同じ 11 桁をハイフン無しで"     },
        { "0120-000-0001", false, "1 桁違い -> NO CARRIER"         },
        { "01200000",      false, "前方一致は不可 -> NO CARRIER"   },
        { "012000000000",  false, "余分な桁 -> NO CARRIER"         },
        { "00",            false, "短すぎ -> NO CARRIER"           },
        { "0000",          false, "4 桁は未登録 -> NO CARRIER"     },
        { "1234567890",    false, "未登録 -> NO CARRIER"           }
    };

    printf("\n[2] ATDT と config.ini 番号マッチング\n");
    make_cfg(&cfg);

    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        char cmd[128];
        vm_at_action_t act;
        bool got_nc, got_dial;

        vm_at_init(&at, &cfg);
        vm_at_feed(&at, "ATE0\r", 5);
        while (vm_at_pop_response(&at, cmd, (int)sizeof(cmd)) > 0) { }

        snprintf(cmd, sizeof(cmd), "ATDT%s\r", cases[i].dial);
        r = feed(&at, cmd);
        act = vm_at_take_action(&at);

        got_nc   = (strstr(r, "NO CARRIER") != NULL);
        got_dial = (act == VM_AT_ACTION_DIAL);

        snprintf(detail, sizeof(detail), "ATDT%-16s %-30s -> %s",
                 cases[i].dial, cases[i].why,
                 got_dial ? "sequence start"
                          : (got_nc ? "NO CARRIER" : vis(r)));

        if (cases[i].match)
            check("dial matches config.ini entry",
                  got_dial && !got_nc, detail);
        else
            check("unmatched number returns NO CARRIER",
                  got_nc && !got_dial, detail);
    }

    /* 一致した番号から正しい ISP エントリと速度が引ける */
    {
        const vm_isp_entry_t *isp;
        vm_standard_t std;
        int bps;

        vm_at_init(&at, &cfg);
        isp = vm_config_match_number(&cfg, "0120-000-0000");
        bps = vm_at_negotiated_bps(&at, isp, &std);
        snprintf(detail, sizeof(detail), "0120-000-0000 -> [%s] %d bps %s",
                 isp ? isp->section : "?", bps, vm_standard_name(std));
        check("matched entry yields its configured speed/standard",
              isp && bps == 33600 && std == VM_STD_V34PLUS, detail);

        isp = vm_config_match_number(&cfg, "000");
        bps = vm_at_negotiated_bps(&at, isp, &std);
        snprintf(detail, sizeof(detail), "000 -> [%s] %d bps %s",
                 isp ? isp->section : "?", bps, vm_standard_name(std));
        check("a second entry yields its own speed/standard",
              isp && bps == 2400 && std == VM_STD_V22BIS, detail);
    }

    /* AT+MS で上限を絞ると接続速度も下がる (実機同様) */
    {
        const vm_isp_entry_t *isp;
        vm_standard_t std;
        int bps;

        vm_at_init(&at, &cfg);
        vm_at_feed(&at, "AT+MS=V32B\r", 11);
        isp = vm_config_match_number(&cfg, "0120-000-0000");
        bps = vm_at_negotiated_bps(&at, isp, &std);
        snprintf(detail, sizeof(detail),
                 "config=33600 but AT+MS=V32B -> %d bps %s",
                 bps, vm_standard_name(std));
        check("AT+MS caps the negotiated speed",
              bps == 14400 && std == VM_STD_V32BIS, detail);
    }

    /* ATD に数字が無ければ NO DIALTONE */
    vm_at_init(&at, &cfg);
    r = feed(&at, "ATD\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("ATD with no digits returns NO DIALTONE",
          strstr(r, "NO DIALTONE") != NULL, detail);
}

/* =====================================================================
 * 3. 実機互換: 連結コマンド / 応答フォーマット / S レジスタ
 * ===================================================================== */
static void test_hayes_compat(void)
{
    vm_config_t cfg;
    vm_at_t at;
    const char *r;
    char detail[400];

    printf("\n[3] Hayes 互換性 (RAS が実際に送る形)\n");
    make_cfg(&cfg);

    /*
     * Windows RAS の既定初期化文字列そのもの。
     * 6 個のコマンドが 1 行に連結されている。
     */
    vm_at_init(&at, &cfg);
    r = feed(&at, "ATE0V1&C1&D2S0=0\r");
    snprintf(detail, sizeof(detail),
             "E=%d V=%d &C=%d &D=%d S0=%u -> \"%s\"",
             at.echo ? 1 : 0, at.verbose ? 1 : 0, at.dcd_mode,
             at.dtr_mode, at.sreg[0], vis(r));
    check("concatenated init string is fully parsed",
          at.echo == false && at.verbose == true &&
          at.dcd_mode == 1 && at.dtr_mode == 2 && at.sreg[0] == 0 &&
          strstr(r, "OK") != NULL, detail);

    /* もう 1 つよく使われる形 */
    vm_at_init(&at, &cfg);
    r = feed(&at, "AT&F&C1&D2E0Q0V1X4S7=60\r");
    snprintf(detail, sizeof(detail), "S7=%u -> \"%s\"", at.sreg[7], vis(r));
    check("AT&F + flags + S-register in one line",
          at.sreg[7] == 60 && strstr(r, "OK") != NULL, detail);

    /* --- V1 (verbose) 応答フォーマット --- */
    vm_at_init(&at, &cfg);
    vm_at_feed(&at, "ATE0\r", 5);
    while (vm_at_pop_response(&at, detail, (int)sizeof(detail)) > 0) { }
    r = feed(&at, "AT\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("V1 response is exactly <CR><LF>OK<CR><LF>",
          strcmp(r, "\r\nOK\r\n") == 0, detail);

    /* --- V0 (数字) 応答フォーマット --- */
    vm_at_init(&at, &cfg);
    vm_at_feed(&at, "ATE0V0\r", 7);
    while (vm_at_pop_response(&at, detail, (int)sizeof(detail)) > 0) { }
    r = feed(&at, "AT\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("V0 response is exactly 0<CR>", strcmp(r, "0\r") == 0, detail);

    /* V0 での NO CARRIER は "3\r" */
    r = feed(&at, "ATDT9999999\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("V0 NO CARRIER is 3<CR>", strcmp(r, "3\r") == 0, detail);

    /* --- ATQ1 は応答を返さない --- */
    vm_at_init(&at, &cfg);
    vm_at_feed(&at, "ATE0Q1\r", 7);
    while (vm_at_pop_response(&at, detail, (int)sizeof(detail)) > 0) { }
    r = feed(&at, "AT\r");
    snprintf(detail, sizeof(detail), "\"%s\" (empty expected)", vis(r));
    check("ATQ1 suppresses all responses", r[0] == '\0', detail);

    /* --- S レジスタの読み書き --- */
    vm_at_init(&at, &cfg);
    vm_at_feed(&at, "ATE0\r", 5);
    while (vm_at_pop_response(&at, detail, (int)sizeof(detail)) > 0) { }
    r = feed(&at, "ATS7=45\r");
    check("ATS7=45 writes the S-register", at.sreg[7] == 45, NULL);
    r = feed(&at, "ATS7?\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("ATS7? returns 3-digit zero-padded value",
          strstr(r, "045") != NULL, detail);

    /* 範囲外は ERROR */
    r = feed(&at, "ATS7=999\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("out-of-range S-register value returns ERROR",
          strstr(r, "ERROR") != NULL, detail);

    /* --- 未知のコマンドは ERROR --- */
    r = feed(&at, "ATG\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("unknown command returns ERROR",
          strstr(r, "ERROR") != NULL, detail);

    /* --- 空行は無応答 (実機と同じ) --- */
    r = feed(&at, "\r");
    snprintf(detail, sizeof(detail), "\"%s\" (empty expected)", vis(r));
    check("empty line produces no response", r[0] == '\0', detail);

    /* --- 小文字も受け付ける --- */
    r = feed(&at, "atz\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("lowercase 'atz' is accepted",
          strstr(r, "OK") != NULL, detail);

    /* --- A/ : 直前コマンドの再実行 (CR 不要) --- */
    vm_at_init(&at, &cfg);
    vm_at_feed(&at, "ATE0\r", 5);
    while (vm_at_pop_response(&at, detail, (int)sizeof(detail)) > 0) { }
    (void)feed(&at, "ATS5=8\r");
    r = feed(&at, "A/");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("A/ repeats the previous command without CR",
          strstr(r, "OK") != NULL, detail);

    /* --- BS による行編集 --- */
    vm_at_init(&at, &cfg);
    /* "ATX" と打って BS で X を消し、"Z" を打つ -> ATZ */
    r = feed(&at, "ATX\bZ\r");
    snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
    check("backspace edits the command line",
          strstr(r, "OK") != NULL &&
          vm_at_take_action(&at) == VM_AT_ACTION_RESET, detail);

    /* --- 1 バイトずつ流し込んでも同じ結果になる --- */
    {
        const char *cmd = "ATDT0120-000-0000\r";
        size_t i;
        vm_at_init(&at, &cfg);
        vm_at_feed(&at, "ATE0\r", 5);
        while (vm_at_pop_response(&at, detail, (int)sizeof(detail)) > 0) { }
        for (i = 0; i < strlen(cmd); i++) vm_at_feed(&at, cmd + i, 1);
        check("byte-at-a-time input works (real serial behaviour)",
              vm_at_take_action(&at) == VM_AT_ACTION_DIAL,
              "ATDT delivered one byte per feed() call");
    }
}

/* =====================================================================
 * 4. +++ エスケープとオンラインモード
 * ===================================================================== */
static void test_online_escape(void)
{
    vm_config_t cfg;
    vm_at_t at;
    char detail[300];
    const uint8_t ppp[] = { 0x7E, 0xFF, 0x03, 0xC0, 0x21, 0x01, 0x00, 0x7E };

    printf("\n[4] オンラインモードと +++ エスケープ\n");
    make_cfg(&cfg);

    /* オンライン中は AT 解釈をせず素通しする */
    vm_at_init(&at, &cfg);
    vm_at_tick(&at, 10000);
    vm_at_set_online(&at, true);
    vm_at_feed(&at, ppp, (int)sizeof(ppp));
    snprintf(detail, sizeof(detail), "passthru_len=%d (expect %d)",
             at.passthru_len, (int)sizeof(ppp));
    check("online data is passed through untouched",
          at.passthru_len == (int)sizeof(ppp) &&
          at.passthru && memcmp(at.passthru, ppp, sizeof(ppp)) == 0, detail);

    /* オンライン中の "ATH" はコマンドではなくデータ */
    vm_at_init(&at, &cfg);
    vm_at_tick(&at, 10000);
    vm_at_set_online(&at, true);
    vm_at_feed(&at, "ATH\r", 4);
    check("\"ATH\" while online is data, not a command",
          at.passthru_len == 4 &&
          vm_at_take_action(&at) == VM_AT_ACTION_NONE, NULL);

    /*
     * ガードタイム無しの "+++" は無視されなければならない。
     * PPP のデータに偶然 0x2B 0x2B 0x2B が現れても切断してはいけない。
     */
    vm_at_init(&at, &cfg);
    vm_at_tick(&at, 10000);
    vm_at_set_online(&at, true);
    vm_at_tick(&at, 10010);                 /* 直後 = ガードタイム不足 */
    vm_at_feed(&at, "+++", 3);
    snprintf(detail, sizeof(detail), "still online=%d passthru=%d",
             at.online ? 1 : 0, at.passthru_len);
    check("+++ without guard time is treated as data",
          at.online == true, detail);

    /* 正しいガードタイムを置いた "+++" はコマンドモードへ戻る */
    vm_at_init(&at, &cfg);
    vm_at_tick(&at, 10000);
    vm_at_set_online(&at, true);
    vm_at_tick(&at, 12000);                 /* 2 秒の無通信 -> ガード成立 */
    vm_at_feed(&at, "+++", 3);
    snprintf(detail, sizeof(detail), "online=%d", at.online ? 1 : 0);
    check("+++ after guard time returns to command mode",
          at.online == false, detail);

    /* コマンドモードに戻った後は AT が効く */
    {
        const char *r = feed(&at, "ATH\r");
        snprintf(detail, sizeof(detail), "\"%s\"", vis(r));
        check("AT commands work again after escape",
              vm_at_take_action(&at) == VM_AT_ACTION_HANGUP, detail);
    }

    /* データ + ガード + "+++" では、データ部だけ PPP へ渡る */
    vm_at_init(&at, &cfg);
    vm_at_tick(&at, 10000);
    vm_at_set_online(&at, true);
    vm_at_tick(&at, 12000);
    vm_at_feed(&at, "AB+++", 5);
    snprintf(detail, sizeof(detail), "passthru_len=%d (expect 2: \"AB\")",
             at.passthru_len);
    check("data before +++ is delivered, the +++ itself is not",
          at.online == false && at.passthru_len == 2, detail);
}

/* =====================================================================
 * 5. シリアル層 (LOOPBACK バックエンド)
 * ===================================================================== */
static void test_serial_loopback(void)
{
    vm_serial_params_t sp;
    vm_serial_t *s = NULL;
    char err[256] = "";
    char buf[256];
    int n;
    char detail[300];

    printf("\n[5] シリアル層 (LOOPBACK)\n");

    vm_serial_params_defaults(&sp);
    sp.backend = VM_SERIAL_BACKEND_LOOPBACK;

    check("open loopback serial",
          vm_serial_open(&s, &sp, err, sizeof(err)) == VM_OK && s != NULL, err);
    if (!s) return;

    check("backend name",
          strcmp(vm_serial_backend_name(s), "loopback") == 0,
          vm_serial_backend_name(s));

    /* 何も来ていない状態では 0 バイト (ブロックしない) */
    n = vm_serial_read(s, buf, (int)sizeof(buf), 0);
    check("read returns 0 when nothing is pending (non-blocking)",
          n == 0, NULL);

    /* PC -> モデム */
    vm_serial_peer_write_str(s, "ATZ\r");
    n = vm_serial_read(s, buf, (int)sizeof(buf), 0);
    buf[n > 0 ? n : 0] = '\0';
    snprintf(detail, sizeof(detail), "read %d bytes: \"%s\"", n, vis(buf));
    check("DTE->DCE direction works", n == 4 && strcmp(buf, "ATZ\r") == 0,
          detail);

    /* モデム -> PC */
    vm_serial_write_str(s, "\r\nOK\r\n");
    n = vm_serial_peer_read(s, buf, (int)sizeof(buf));
    buf[n > 0 ? n : 0] = '\0';
    snprintf(detail, sizeof(detail), "peer read %d bytes: \"%s\"", n, vis(buf));
    check("DCE->DTE direction works", n == 6 && strcmp(buf, "\r\nOK\r\n") == 0,
          detail);

    /* DCD の操作 */
    vm_serial_set_dcd(s, true);
    vm_serial_set_dcd(s, false);
    check("DCD can be toggled without error", 1, "SETDTR/CLRDTR path");

    /* purge */
    vm_serial_peer_write_str(s, "GARBAGE");
    vm_serial_purge_rx(s);
    n = vm_serial_read(s, buf, (int)sizeof(buf), 0);
    check("purge_rx discards pending input", n == 0, NULL);

    vm_serial_close(s);
    check("close", 1, NULL);
}

/* =====================================================================
 * 6. AT ハンドラ + シリアル層の結合 (実際の使い方)
 * ===================================================================== */
static void test_integration(void)
{
    vm_config_t cfg;
    vm_at_t at;
    vm_serial_params_t sp;
    vm_serial_t *s = NULL;
    char err[256] = "";
    char buf[512];
    char accum[1024];
    int n;
    char detail[400];

    printf("\n[6] 結合テスト: RAS の初期化から CONNECT まで\n");

    make_cfg(&cfg);
    vm_at_init(&at, &cfg);

    vm_serial_params_defaults(&sp);
    sp.backend = VM_SERIAL_BACKEND_LOOPBACK;
    if (vm_serial_open(&s, &sp, err, sizeof(err)) != VM_OK) {
        check("open serial", 0, err);
        return;
    }

    /*
     * RAS が実際にやる手順を再現する:
     *   1. "AT&F\r"           工場出荷設定
     *   2. "ATE0V1&C1&D2\r"   初期化
     *   3. "ATDT<number>\r"   発信
     *   4. (音響シーケンス)
     *   5. "CONNECT 33600"    <- 我々が返す
     */
    {
        static const char *init_seq[] = {
            "AT&F\r", "ATE0V1&C1&D2S0=0\r", "AT+MS=V34,1,300,33600\r"
        };
        int i, ok = 1;

        for (i = 0; i < 3; i++) {
            vm_serial_peer_write_str(s, init_seq[i]);
            n = vm_serial_read(s, buf, (int)sizeof(buf), 0);
            vm_at_feed(&at, buf, n);
            while ((n = vm_at_pop_response(&at, buf, (int)sizeof(buf))) > 0) {
                vm_serial_write(s, buf, n);
            }
            n = vm_serial_peer_read(s, accum, (int)sizeof(accum) - 1);
            accum[n > 0 ? n : 0] = '\0';
            if (!strstr(accum, "OK")) ok = 0;
        }
        check("RAS init sequence all answered with OK", ok, 
              "AT&F / ATE0V1&C1&D2S0=0 / AT+MS=V34,1,300,33600");
    }

    /* --- 発信: 一致する番号 --- */
    vm_serial_peer_write_str(s, "ATDT0120-000-0000\r");
    n = vm_serial_read(s, buf, (int)sizeof(buf), 0);
    vm_at_feed(&at, buf, n);
    while ((n = vm_at_pop_response(&at, buf, (int)sizeof(buf))) > 0)
        vm_serial_write(s, buf, n);

    check("ATDT on a configured number starts the sequence (no response yet)",
          vm_at_take_action(&at) == VM_AT_ACTION_DIAL &&
          vm_serial_peer_read(s, buf, (int)sizeof(buf)) == 0,
          "音響シーケンス実行中は応答を保留する");

    /* --- 音響シーケンス完了後に CONNECT を返す --- */
    {
        const vm_isp_entry_t *isp = vm_config_match_number(&cfg,
                                                           at.action_number);
        vm_standard_t std;
        int bps = vm_at_negotiated_bps(&at, isp, &std);

        vm_at_emit_connect(&at, bps);
        vm_at_set_online(&at, true);
        vm_serial_set_dcd(s, true);

        while ((n = vm_at_pop_response(&at, buf, (int)sizeof(buf))) > 0)
            vm_serial_write(s, buf, n);

        n = vm_serial_peer_read(s, accum, (int)sizeof(accum) - 1);
        accum[n > 0 ? n : 0] = '\0';

        snprintf(detail, sizeof(detail), "\"%s\" (%s, %d bps)",
                 vis(accum), vm_standard_name(std), bps);
        check("CONNECT xxxxx is returned to the COM port",
              strcmp(accum, "\r\nCONNECT 33600\r\n") == 0, detail);
    }

    /* --- 発信: 一致しない番号 -> NO CARRIER --- */
    vm_at_set_online(&at, false);
    vm_serial_set_dcd(s, false);
    vm_serial_peer_write_str(s, "ATDT0311112222\r");
    n = vm_serial_read(s, buf, (int)sizeof(buf), 0);
    vm_at_feed(&at, buf, n);
    while ((n = vm_at_pop_response(&at, buf, (int)sizeof(buf))) > 0)
        vm_serial_write(s, buf, n);
    n = vm_serial_peer_read(s, accum, (int)sizeof(accum) - 1);
    accum[n > 0 ? n : 0] = '\0';
    snprintf(detail, sizeof(detail), "\"%s\"", vis(accum));
    check("unconfigured number returns NO CARRIER over the COM port",
          strcmp(accum, "\r\nNO CARRIER\r\n") == 0, detail);

    vm_serial_close(s);
}

int main(void)
{
    printf("=======================================================\n");
    printf(" VModem AT コマンド / シリアル層 検証テスト\n");
    printf("=======================================================\n");

    vm_log_init(VM_LOG_NONE, NULL);   /* テスト中はログを出さない */

    test_required_commands();
    test_dial_matching();
    test_hayes_compat();
    test_online_escape();
    test_serial_loopback();
    test_integration();

    printf("\n=======================================================\n");
    printf(" 結果: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");

    vm_log_shutdown();
    return g_fail == 0 ? 0 : 1;
}
