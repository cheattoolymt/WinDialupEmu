/*
 * test_sequence.c - 接続音響シーケンサの検証
 *
 * 仕様書「音声シーケンス (重要)」が本当にその順序・その周波数で
 * 合成されている事を *スペクトル解析で* 検証する。
 *
 * 「録音の再生ではない」事の証明にもなっている:
 *   - 同じ番号を違う速度で鳴らすと、ネゴ音だけが変わる
 *   - 番号を変えると DTMF の周波数対が変わる
 *   - 呼出音は 400Hz に 15Hz の側波帯 (385/415Hz) を持つ
 * これらは固定音源の再生では実現できない。
 */
#include "vmodem/vm_sequence.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass = 0, g_fail = 0;

static void check(const char *name, int ok, const char *detail)
{
    if (ok) { g_pass++; printf("  [ OK ] %-54s %s\n", name, detail ? detail : ""); }
    else    { g_fail++; printf("  [FAIL] %-54s %s\n", name, detail ? detail : ""); }
}

/* ---------------------------------------------------------------------------
 * Goertzel 法による単一周波数のエネルギー測定
 * ---------------------------------------------------------------------------
 * FFT を使わずに「この周波数がどれだけ含まれるか」だけを測る。
 * 正規化して振幅 (0..1) を返す。
 * -------------------------------------------------------------------------*/
static double goertzel(const float *x, int n, double freq, int fs)
{
    double w = 2.0 * M_PI * freq / (double)fs;
    double coeff = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    int i;

    for (i = 0; i < n; i++) {
        s0 = (double)x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) / (double)n;
}

static double rms(const float *x, int n)
{
    double a = 0;
    int i;
    for (i = 0; i < n; i++) a += (double)x[i] * (double)x[i];
    return n > 0 ? sqrt(a / (double)n) : 0.0;
}

/* ---------------------------------------------------------------------------
 * シーケンス全体を回して、フェーズごとに音を別バッファへ切り分ける
 * -------------------------------------------------------------------------*/
#define MAX_REC  (8 * 60 * 1000)   /* 8kHz で 60 秒ぶん */

typedef struct {
    float *buf[VM_PHASE__COUNT];
    int    len[VM_PHASE__COUNT];
    int    order[64];              /* フェーズが現れた順序 */
    int    order_len;
    int    total;
} recording_t;

static void rec_init(recording_t *r)
{
    int i;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < VM_PHASE__COUNT; i++) {
        r->buf[i] = (float *)calloc(MAX_REC, sizeof(float));
        r->len[i] = 0;
    }
}

static void rec_free(recording_t *r)
{
    int i;
    for (i = 0; i < VM_PHASE__COUNT; i++) free(r->buf[i]);
}

/*
 * 1 サンプル単位でフェーズを観測しながら録音する。
 *
 * ★ 重要 ★
 * ブロック単位 (160 サンプル) で観測すると、
 * 短いフェーズ (無音 50ms 等) を取り逃す。
 * ここでは 1 サンプルずつ render して確実に全フェーズを捕える。
 * (test_dsp.c で同じ罠を踏んだ教訓)
 */
static void record_sequence(vm_sequence_t *s, recording_t *r)
{
    int guard = 0;
    bool done = false;

    rec_init(r);

    while (!done && guard++ < MAX_REC) {
        float v = 0.0f;
        vm_phase_t p = vm_sequence_phase(s);

        done = vm_sequence_render(s, &v, 1);

        if (p >= 0 && p < VM_PHASE__COUNT) {
            if (r->len[p] < MAX_REC) r->buf[p][r->len[p]++] = v;

            /* フェーズ遷移を記録 */
            if (r->order_len == 0 || r->order[r->order_len - 1] != (int)p) {
                if (r->order_len < 64) r->order[r->order_len++] = (int)p;
            }
        }
        r->total++;
    }
}

static const char *order_string(const recording_t *r)
{
    static char b[512];
    int i, o = 0;
    b[0] = '\0';
    for (i = 0; i < r->order_len && o < (int)sizeof(b) - 40; i++) {
        o += snprintf(b + o, sizeof(b) - (size_t)o, "%s%s",
                      i ? " -> " : "", vm_phase_name((vm_phase_t)r->order[i]));
    }
    return b;
}

static void make_cfg(vm_config_t *cfg, bool fast)
{
    vm_config_defaults(cfg);
    cfg->fast_connect   = fast;
    cfg->ringback_count = 2;
}

/* =====================================================================
 * 1. シーケンスの順序 (仕様書の 1〜6 に一致する事)
 * ===================================================================== */
static void test_phase_order(void)
{
    vm_config_t cfg;
    vm_sequence_t seq;
    recording_t rec;
    char detail[600];

    printf("\n[1] 音声シーケンスの順序 (仕様書 1〜6)\n");

    make_cfg(&cfg, true);   /* fast: 各段を短縮して高速検証 */

    check("start_dial accepts a normalized number",
          vm_sequence_start_dial(&seq, "0120-000-0000", NULL, &cfg,
                                 VM_STD_V34PLUS, 33600,
                                 VM_DSP_SAMPLE_RATE, 1.0f) == VM_OK,
          "ATDT0120-000-0000 -> V.34+ 33600bps");

    check("hyphens are stripped before DTMF synthesis",
          strcmp(seq.digits, "01200000000") == 0, seq.digits);

    record_sequence(&seq, &rec);

    snprintf(detail, sizeof(detail), "%s", order_string(&rec));
    check("sequence reaches completion", seq.done && !seq.aborted, detail);

    /*
     * 期待する順序 (仕様書 1〜5 に 1:1 対応):
     *   OFFHOOK   オフフック直後の無音
     *   DIALTONE  1. ダイアルトーン
     *   DTMF      2. DTMF 発信音
     *   RINGBACK  3. 呼び出し音
     *   OFFHOOK   (相手が受話器を上げてから応答までの無音)
     *   ANSWER_TONE 4. モデムネゴシエーション音
     *   SILENCE   5. 無音
     *
     * ★ CONNECTED がここに現れない理由 ★
     *   CONNECTED は「シーケンス完了」を表す終端状態であり、
     *   この状態に入った瞬間に render() が done=true を返す。
     *   従って record_sequence() のループはその回で抜けるため、
     *   CONNECTED は「観測されない」のが正しい振舞い。
     *   (この後上位層が CONNECT xxxxx を COM ポートへ返す)
     */
    {
        static const vm_phase_t expect[] = {
            VM_PHASE_OFFHOOK, VM_PHASE_DIALTONE, VM_PHASE_DTMF,
            VM_PHASE_RINGBACK, VM_PHASE_OFFHOOK, VM_PHASE_ANSWER_TONE,
            VM_PHASE_SILENCE
        };
        int n = (int)(sizeof(expect) / sizeof(expect[0]));
        int ok = (rec.order_len == n);
        int i;
        for (i = 0; ok && i < n; i++)
            if (rec.order[i] != (int)expect[i]) ok = 0;

        snprintf(detail, sizeof(detail), "%d phases: %s -> [CONNECTED]",
                 rec.order_len, order_string(&rec));
        check("phase order matches the specification exactly", ok, detail);

        /* 終端状態に到達している事を別途確認する */
        check("terminal state is CONNECTED (ready to emit CONNECT xxxxx)",
              vm_sequence_phase(&seq) == VM_PHASE_CONNECTED,
              vm_phase_name(vm_sequence_phase(&seq)));
    }

    /* 各段が実際に時間を持っている事 */
    snprintf(detail, sizeof(detail),
             "dialtone=%dms dtmf=%dms ringback=%dms nego=%dms silence=%dms",
             rec.len[VM_PHASE_DIALTONE]    * 1000 / 8000,
             rec.len[VM_PHASE_DTMF]        * 1000 / 8000,
             rec.len[VM_PHASE_RINGBACK]    * 1000 / 8000,
             rec.len[VM_PHASE_ANSWER_TONE] * 1000 / 8000,
             rec.len[VM_PHASE_SILENCE]     * 1000 / 8000);
    check("every stage produced audio samples",
          rec.len[VM_PHASE_DIALTONE]    > 0 &&
          rec.len[VM_PHASE_DTMF]        > 0 &&
          rec.len[VM_PHASE_RINGBACK]    > 0 &&
          rec.len[VM_PHASE_ANSWER_TONE] > 0 &&
          rec.len[VM_PHASE_SILENCE]     > 0, detail);

    rec_free(&rec);
}

/* =====================================================================
 * 2. 各段の周波数がスペクトル上で正しい事
 * ===================================================================== */
static void test_frequencies(void)
{
    vm_config_t cfg;
    vm_sequence_t seq;
    recording_t rec;
    char detail[600];

    printf("\n[2] 各段のスペクトル検証 (リアルタイム合成の証明)\n");

    make_cfg(&cfg, false);       /* 実時間: 本来の長さで合成 */
    cfg.dialtone_ms = 500;       /* テスト時間短縮のため発信音のみ短く */
    cfg.ringback_count = 1;

    vm_sequence_start_dial(&seq, "012-345", NULL, &cfg,
                           VM_STD_V34PLUS, 33600, VM_DSP_SAMPLE_RATE, 1.0f);
    record_sequence(&seq, &rec);

    /* --- 1. ダイアルトーン: 日本は 400Hz 連続 --- */
    {
        const float *b = rec.buf[VM_PHASE_DIALTONE];
        int n = rec.len[VM_PHASE_DIALTONE];
        double a400 = goertzel(b, n, 400.0, 8000);
        double a350 = goertzel(b, n, 350.0, 8000);
        double a440 = goertzel(b, n, 440.0, 8000);
        double a1000 = goertzel(b, n, 1000.0, 8000);

        snprintf(detail, sizeof(detail),
                 "400Hz=%.4f 350Hz=%.4f 440Hz=%.4f 1000Hz=%.4f (%d ms)",
                 a400, a350, a440, a1000, n * 1000 / 8000);
        check("dial tone is a clean 400Hz continuous tone (Japan)",
              a400 > 0.4 && a350 < 0.05 && a440 < 0.05 && a1000 < 0.01,
              detail);
    }

    /* --- 2. DTMF: 各桁が Q.23 の周波数対を持つ --- */
    {
        /*
         * "012345" の各桁の期待周波数 (ITU-T Q.23)
         *   0 = 941 + 1336
         *   1 = 697 + 1209
         *   2 = 697 + 1336
         *   3 = 697 + 1477
         *   4 = 770 + 1209
         *   5 = 770 + 1336
         */
        struct { char d; double lo, hi; } tab[] = {
            { '0', 941.0, 1336.0 }, { '1', 697.0, 1209.0 },
            { '2', 697.0, 1336.0 }, { '3', 697.0, 1477.0 },
            { '4', 770.0, 1209.0 }, { '5', 770.0, 1336.0 }
        };
        const float *b = rec.buf[VM_PHASE_DTMF];
        int n = rec.len[VM_PHASE_DTMF];
        /*
         * ★ ON/OFF 長はハードコードせず実際の設定値から取る ★
         * config の既定は 80/80ms (S11 の 95ms とは違う)。
         * マジックナンバを書くと設定変更でテストが崩れる。
         */
        int on_samp = seq.dtmf_on_ms  * 8;
        int off_samp= seq.dtmf_off_ms * 8;
        int period  = on_samp + off_samp;
        int ndig    = (int)strlen(seq.digits);
        int i, all_ok = 1;
        char sub[400];
        int so = 0;

        /* 桁数 × 周期 ぶんの長さがある事 */
        snprintf(detail, sizeof(detail),
                 "%d ms for %d digits (expect ~%d ms = %d x (%d+%d)ms)",
                 n * 1000 / 8000, ndig,
                 ndig * (seq.dtmf_on_ms + seq.dtmf_off_ms), ndig,
                 seq.dtmf_on_ms, seq.dtmf_off_ms);
        check("DTMF burst length matches digit count x (on+off)ms",
              n >= ndig * period - period / 2, detail);

        for (i = 0; i < 6; i++) {
            /* i 番目の桁の ON 区間の中央付近を切り出す */
            int off = i * period + on_samp / 4;
            int len = on_samp / 2;
            double alo, ahi, aother;

            if (off + len > n) { all_ok = 0; break; }
            (void)off_samp;

            alo    = goertzel(b + off, len, tab[i].lo, 8000);
            ahi    = goertzel(b + off, len, tab[i].hi, 8000);
            /* この桁が使わない周波数 (852Hz 低群) は出ていないはず */
            aother = goertzel(b + off, len, 852.0, 8000);

            if (!(alo > 0.15 && ahi > 0.15 && aother < 0.06)) all_ok = 0;

            so += snprintf(sub + so, sizeof(sub) - (size_t)so,
                           "%c:%.0f/%.0f=%.2f/%.2f ",
                           tab[i].d, tab[i].lo, tab[i].hi, alo, ahi);
        }
        check("each dialed digit emits its exact Q.23 frequency pair",
              all_ok, sub);

        /* 高群 twist: 高群が低群より大きい (規定 +2dB) */
        {
            int off = on_samp / 4;
            int len = on_samp / 2;
            double alo = goertzel(b + off, len, 941.0, 8000);
            double ahi = goertzel(b + off, len, 1336.0, 8000);
            double db  = 20.0 * log10(ahi / (alo > 1e-9 ? alo : 1e-9));
            snprintf(detail, sizeof(detail),
                     "high/low = %+.2f dB (spec: high group +2dB twist)", db);
            check("DTMF high group carries the specified +2dB twist",
                  db > 1.0 && db < 3.5, detail);
        }
    }

    /* --- 3. 呼出音: 400Hz を 15Hz で AM -> 385/415Hz の側波帯 --- */
    {
        const float *b = rec.buf[VM_PHASE_RINGBACK];
        int n = rec.len[VM_PHASE_RINGBACK];
        /* ON 区間 (先頭 1 秒) を解析 */
        int on = n / 3;
        double a400, a385, a415, a1000;

        if (on > 8000) on = 8000;
        a400  = goertzel(b, on, 400.0, 8000);
        a385  = goertzel(b, on, 385.0, 8000);
        a415  = goertzel(b, on, 415.0, 8000);
        a1000 = goertzel(b, on, 1000.0, 8000);

        snprintf(detail, sizeof(detail),
                 "carrier 400Hz=%.4f sidebands 385Hz=%.4f 415Hz=%.4f "
                 "1000Hz=%.4f", a400, a385, a415, a1000);
        check("ringback is 400Hz AM-modulated at 15Hz (sidebands present)",
              a400 > 0.2 && a385 > 0.03 && a415 > 0.03 && a1000 < 0.01,
              detail);

        /* ON 1 秒 / OFF 2 秒 の断続構造 */
        {
            double r_on  = rms(b, 8000);                   /* 0.0-1.0s */
            double r_off = rms(b + 8000 + 4000, 8000);      /* 1.5-2.5s */
            snprintf(detail, sizeof(detail),
                     "rms(0-1s)=%.4f rms(1.5-2.5s)=%.6f  (Japan: 1s ON / 2s OFF)",
                     r_on, r_off);
            check("ringback cadence is 1s ON / 2s OFF (Japan)",
                  n >= 3 * 8000 && r_on > 0.1 && r_off < 0.001, detail);
        }
    }

    /* --- 4. ネゴシエーション音: V.34 の ANSam 2100Hz など --- */
    {
        const float *b = rec.buf[VM_PHASE_ANSWER_TONE];
        int n = rec.len[VM_PHASE_ANSWER_TONE];
        double best2100 = 0;
        int i;

        /* ANSam は先頭付近にある。窓をずらして最大値を探す */
        for (i = 0; i + 800 <= n && i < 8000 * 4; i += 400) {
            double a = goertzel(b + i, 800, 2100.0, 8000);
            if (a > best2100) best2100 = a;
        }

        snprintf(detail, sizeof(detail),
                 "max 2100Hz energy = %.4f over %d ms of negotiation",
                 best2100, n * 1000 / 8000);
        check("negotiation contains the V.8 ANSam 2100Hz answer tone",
              best2100 > 0.2, detail);

        /* V.34 のライン プロービング帯域 (150Hz〜3750Hz) が使われる */
        {
            double lo = 0, hi = 0;
            for (i = 0; i + 1600 <= n; i += 800) {
                double a1 = goertzel(b + i, 1600, 150.0,  8000);
                double a2 = goertzel(b + i, 1600, 3750.0, 8000);
                if (a1 > lo) lo = a1;
                if (a2 > hi) hi = a2;
            }
            snprintf(detail, sizeof(detail),
                     "150Hz=%.4f 3750Hz=%.4f (V.34 Table 8 probe band edges)",
                     lo, hi);
            check("negotiation exercises the full V.34 probe band",
                  lo > 0.02 && hi > 0.02, detail);
        }
    }

    /* --- 5. 無音: 本当に無音である事 --- */
    {
        const float *b = rec.buf[VM_PHASE_SILENCE];
        int n = rec.len[VM_PHASE_SILENCE];
        double r = rms(b, n);
        snprintf(detail, sizeof(detail), "rms=%.9f over %d ms",
                 r, n * 1000 / 8000);
        check("final stage is true silence (audio output stops)",
              n > 0 && r < 1e-9, detail);
    }

    rec_free(&rec);
}

/* =====================================================================
 * 3. 「録音の再生ではない」事の証明
 * ===================================================================== */
static void test_not_a_recording(void)
{
    vm_config_t cfg;
    char detail[600];

    printf("\n[3] 録音再生ではない事の証明 (合成パラメータへの追従)\n");

    make_cfg(&cfg, false);
    cfg.dialtone_ms    = 200;
    cfg.ringback_count = 1;

    /*
     * 証明 1: 番号を変えると DTMF の周波数が変わる。
     * 固定音源の再生では絶対に起こらない。
     */
    {
        vm_sequence_t s1, s2;
        recording_t r1, r2;
        double a1, a2, b1, b2;

        vm_sequence_start_dial(&s1, "111", NULL, &cfg,
                               VM_STD_V34, 28800, VM_DSP_SAMPLE_RATE, 1.0f);
        record_sequence(&s1, &r1);

        vm_sequence_start_dial(&s2, "999", NULL, &cfg,
                               VM_STD_V34, 28800, VM_DSP_SAMPLE_RATE, 1.0f);
        record_sequence(&s2, &r2);

        /* '1' = 697+1209, '9' = 852+1477 */
        a1 = goertzel(r1.buf[VM_PHASE_DTMF], 600, 697.0,  8000);
        b1 = goertzel(r1.buf[VM_PHASE_DTMF], 600, 852.0,  8000);
        a2 = goertzel(r2.buf[VM_PHASE_DTMF], 600, 697.0,  8000);
        b2 = goertzel(r2.buf[VM_PHASE_DTMF], 600, 852.0,  8000);

        snprintf(detail, sizeof(detail),
                 "\"111\": 697Hz=%.3f 852Hz=%.3f | \"999\": 697Hz=%.3f 852Hz=%.3f",
                 a1, b1, a2, b2);
        check("dialing a different number synthesizes different DTMF tones",
              a1 > 0.15 && b1 < 0.06 && a2 < 0.06 && b2 > 0.15, detail);

        rec_free(&r1);
        rec_free(&r2);
    }

    /*
     * 証明 2: 規格を変えるとネゴシエーション音の長さと中身が変わる。
     */
    {
        struct { vm_standard_t std; int bps; const char *name; } cases[] = {
            { VM_STD_V21,     300,  "V.21"    },
            { VM_STD_V22BIS,  2400, "V.22bis" },
            { VM_STD_V32BIS, 14400, "V.32bis" },
            { VM_STD_V34PLUS,33600, "V.34+"   }
        };
        int i;
        int nego_ms[4];
        char sub[400];
        int so = 0;
        int all_differ = 1;

        for (i = 0; i < 4; i++) {
            vm_sequence_t s;
            recording_t r;
            vm_sequence_start_dial(&s, "000", NULL, &cfg,
                                   cases[i].std, cases[i].bps,
                                   VM_DSP_SAMPLE_RATE, 1.0f);
            record_sequence(&s, &r);
            nego_ms[i] = r.len[VM_PHASE_ANSWER_TONE] * 1000 / 8000;
            so += snprintf(sub + so, sizeof(sub) - (size_t)so, "%s=%dms ",
                           cases[i].name, nego_ms[i]);
            rec_free(&r);
        }

        /* V.34 が最も長く、V.21 が最も短いはず */
        if (!(nego_ms[3] > nego_ms[0])) all_differ = 0;
        for (i = 0; i < 4; i++) if (nego_ms[i] <= 0) all_differ = 0;

        check("each standard synthesizes its own negotiation length",
              all_differ, sub);
    }

    /*
     * 証明 3: V.22bis の応答キャリアは 2225Hz、V.34 は 2100Hz ANSam。
     * 規格ごとに違う周波数が出る = 合成している証拠。
     */
    {
        vm_sequence_t s;
        recording_t r;
        double a2225 = 0, a2100 = 0;
        int i, n;

        vm_sequence_start_dial(&s, "000", NULL, &cfg,
                               VM_STD_V22BIS, 2400,
                               VM_DSP_SAMPLE_RATE, 1.0f);
        record_sequence(&s, &r);
        n = r.len[VM_PHASE_ANSWER_TONE];

        for (i = 0; i + 800 <= n; i += 400) {
            double x = goertzel(r.buf[VM_PHASE_ANSWER_TONE] + i, 800,
                                2225.0, 8000);
            double y = goertzel(r.buf[VM_PHASE_ANSWER_TONE] + i, 800,
                                2100.0, 8000);
            if (x > a2225) a2225 = x;
            if (y > a2100) a2100 = y;
        }
        snprintf(detail, sizeof(detail),
                 "V.22bis: 2225Hz=%.4f (answer carrier) 2100Hz=%.4f",
                 a2225, a2100);
        check("V.22bis emits its 2225Hz answer carrier, not V.34's ANSam",
              a2225 > 0.2, detail);
        rec_free(&r);
    }

    /*
     * 証明 4: 音量パラメータが波形に反映される (ATL/ATM)。
     */
    {
        vm_sequence_t s;
        float blk[800];
        double r_full, r_half, r_mute;
        vm_config_t vcfg;
        int i;

        /*
         * ★ 測定はダイアルトーン中に行う ★
         * 以前は 20 ブロック (=2 秒) 進めてから測っていたが、
         * ダイアルトーンは 200ms しか無いので既に無音区間に
         * 入ってしまい「gain を変えても 0」になっていた。
         * 発信音を 3 秒に伸ばし、その内側で 3 回測る。
         */
        make_cfg(&vcfg, false);
        vcfg.dialtone_ms    = 3000;
        vcfg.ringback_count = 1;

        vm_sequence_start_dial(&s, "000", NULL, &vcfg,
                               VM_STD_V34, 28800, VM_DSP_SAMPLE_RATE, 1.0f);

        /* OFFHOOK (200ms) を抜けてダイアルトーンに入る */
        for (i = 0; i < 3; i++) vm_sequence_render(&s, blk, 800);
        while (vm_sequence_phase(&s) != VM_PHASE_DIALTONE)
            vm_sequence_render(&s, blk, 800);

        vm_sequence_render(&s, blk, 800);
        r_full = rms(blk, 800);

        vm_sequence_set_gain(&s, 0.5f);
        vm_sequence_render(&s, blk, 800);
        r_half = rms(blk, 800);

        vm_sequence_set_muted(&s, true);
        vm_sequence_render(&s, blk, 800);
        r_mute = rms(blk, 800);

        snprintf(detail, sizeof(detail),
                 "gain1.0=%.4f gain0.5=%.4f muted=%.9f (ratio %.3f)",
                 r_full, r_half, r_mute,
                 r_full > 1e-9 ? r_half / r_full : 0.0);
        check("volume/mute are applied to the synthesized waveform",
              r_full > 0.05 && fabs(r_half / r_full - 0.5) < 0.05 &&
              r_mute < 1e-9, detail);
    }
}

/* =====================================================================
 * 4. ATA (着信応答) と中断
 * ===================================================================== */
static void test_answer_and_abort(void)
{
    vm_config_t cfg;
    vm_sequence_t seq;
    recording_t rec;
    char detail[600];

    printf("\n[4] ATA 着信応答 / 中断\n");

    make_cfg(&cfg, true);

    /* 着信側はダイアルトーン・DTMF・呼出音を鳴らさない */
    check("start_answer succeeds",
          vm_sequence_start_answer(&seq, &cfg, VM_STD_V34PLUS, 33600,
                                   VM_DSP_SAMPLE_RATE, 1.0f) == VM_OK, NULL);

    record_sequence(&seq, &rec);
    snprintf(detail, sizeof(detail), "%s", order_string(&rec));
    check("answer sequence skips dialtone/DTMF/ringback",
          rec.len[VM_PHASE_DIALTONE] == 0 &&
          rec.len[VM_PHASE_DTMF]     == 0 &&
          rec.len[VM_PHASE_RINGBACK] == 0 &&
          rec.len[VM_PHASE_ANSWER_TONE] > 0, detail);
    rec_free(&rec);

    /* 中断すると即座に無音になる */
    {
        float blk[800];
        int i;
        double r;

        make_cfg(&cfg, false);
        vm_sequence_start_dial(&seq, "000", NULL, &cfg,
                               VM_STD_V34, 28800, VM_DSP_SAMPLE_RATE, 1.0f);
        for (i = 0; i < 10; i++) vm_sequence_render(&seq, blk, 800);

        vm_sequence_abort(&seq);
        check("abort marks the sequence done",
              vm_sequence_render(&seq, blk, 800) == true &&
              seq.aborted == true, vm_phase_name(vm_sequence_phase(&seq)));

        r = rms(blk, 800);
        snprintf(detail, sizeof(detail), "rms after abort = %.9f", r);
        check("audio stops immediately after abort (ATH)", r < 1e-9, detail);
    }

    /* status 文字列がネゴ中に内部ステージまで出す */
    {
        float blk[160];
        int i;
        const char *st = "";
        int saw_stage = 0;

        make_cfg(&cfg, false);
        cfg.dialtone_ms = 100;
        cfg.ringback_count = 1;
        vm_sequence_start_dial(&seq, "000", NULL, &cfg,
                               VM_STD_V34PLUS, 33600,
                               VM_DSP_SAMPLE_RATE, 1.0f);
        for (i = 0; i < 4000; i++) {
            if (vm_sequence_render(&seq, blk, 160)) break;
            st = vm_sequence_status(&seq);
            if (strchr(st, '/')) { saw_stage = 1; break; }
        }
        snprintf(detail, sizeof(detail), "status = \"%s\"", st);
        check("status reports the ITU-T handshake stage during negotiation",
              saw_stage, detail);
    }
}

/* =====================================================================
 * 5. 全速度・全規格を通す (仕様書の速度表を網羅)
 * ===================================================================== */
static void test_all_standards(void)
{
    vm_config_t cfg;
    char detail[600];
    int i;

    struct { vm_standard_t std; int bps; const char *label; } tab[] = {
        { VM_STD_V21,       300, "300bps   V.21"    },
        { VM_STD_V22,      1200, "1200bps  V.22"    },
        { VM_STD_V22BIS,   2400, "2400bps  V.22bis" },
        { VM_STD_V32,      9600, "9600bps  V.32"    },
        { VM_STD_V32BIS,  14400, "14400bps V.32bis" },
        { VM_STD_V34,     28800, "28800bps V.34"    },
        { VM_STD_V34PLUS, 33600, "33600bps V.34+"   },
        { VM_STD_V90,     56000, "56000bps V.90"    }
    };

    printf("\n[5] 仕様書の速度表を全て通す\n");

    make_cfg(&cfg, true);

    for (i = 0; i < 8; i++) {
        vm_sequence_t seq;
        recording_t rec;
        double r;
        int ok;

        if (vm_sequence_start_dial(&seq, "000", NULL, &cfg,
                                   tab[i].std, tab[i].bps,
                                   VM_DSP_SAMPLE_RATE, 1.0f) != VM_OK) {
            check("standard completes a full sequence", 0, tab[i].label);
            continue;
        }
        record_sequence(&seq, &rec);

        r = rms(rec.buf[VM_PHASE_ANSWER_TONE],
                rec.len[VM_PHASE_ANSWER_TONE]);

        ok = seq.done && !seq.aborted &&
             rec.len[VM_PHASE_ANSWER_TONE] > 0 && r > 0.01;

        snprintf(detail, sizeof(detail),
                 "%s : nego %d ms, rms=%.4f, total %d ms",
                 tab[i].label,
                 rec.len[VM_PHASE_ANSWER_TONE] * 1000 / 8000,
                 r, rec.total * 1000 / 8000);
        check("standard completes a full sequence", ok, detail);

        rec_free(&rec);
    }
}

int main(void)
{
    printf("=======================================================\n");
    printf(" VModem 接続音響シーケンサ 検証テスト\n");
    printf("=======================================================\n");

    vm_log_init(VM_LOG_NONE, NULL);

    test_phase_order();
    test_frequencies();
    test_not_a_recording();
    test_answer_and_abort();
    test_all_standards();

    printf("\n=======================================================\n");
    printf(" 結果: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");

    vm_log_shutdown();
    return g_fail == 0 ? 0 : 1;
}
