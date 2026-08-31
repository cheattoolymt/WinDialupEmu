/*
 * vm_handshake.c - ハンドシェイク シーケンサ
 *
 * 規格ごとに「どの信号を、どの順で、どれだけの長さ出すか」を
 * plan として組み立て、それを順に実行して PCM を生成する。
 *
 * 実機のモデム音を聴いた時の印象を決めるのは、
 *   (1) 各信号のスペクトラム (周波数構成)
 *   (2) 各信号の継続時間と順序
 * の 2 つである。本モジュールは (2) を、vm_v8.c / vm_v34.c が (1) を担う。
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vmodem/vm_handshake.h"

#include <math.h>
#include <string.h>

/* ========================================================================= */
/* ステージ名                                                                */
/* ========================================================================= */

const char *vm_hs_stage_name(vm_hs_stage_t s)
{
    switch (s) {
    case VM_HS_DONE:            return "DONE";
    case VM_HS_CT:              return "CT(1300Hz)";
    case VM_HS_ANSAM:           return "ANSam(2100Hz AM+PR)";
    case VM_HS_CM:              return "CM(V.21ch1)";
    case VM_HS_JM:              return "JM(V.21ch2)";
    case VM_HS_CJ:              return "CJ(V.21ch1)";
    case VM_HS_V8_END_SILENCE:  return "V8-silence(75ms)";
    case VM_HS_INFO0:           return "INFO0(600bps DPSK)";
    case VM_HS_TONE_B:          return "ToneB(1200Hz+PR)";
    case VM_HS_TONE_A:          return "ToneA(2400Hz+PR)";
    case VM_HS_L1:              return "L1(21-tone probe)";
    case VM_HS_L2:              return "L2(21-tone probe)";
    case VM_HS_INFO1:           return "INFO1(600bps DPSK)";
    case VM_HS_S:               return "S(128T)";
    case VM_HS_SBAR:            return "!S(16T)";
    case VM_HS_PP:              return "PP(288T)";
    case VM_HS_TRN:             return "TRN";
    case VM_HS_J:               return "J";
    case VM_HS_MP:              return "MP";
    case VM_HS_E:               return "E";
    case VM_HS_B1:              return "B1";
    case VM_HS_V32_AA:          return "V32-AA/AC";
    case VM_HS_V32_S:           return "V32-S/!S";
    case VM_HS_V32_TRN:         return "V32-TRN";
    case VM_HS_V32_R1R2R3:      return "V32-R1/R2/R3";
    case VM_HS_V22_USB1:        return "V22-carrier";
    case VM_HS_V22_S1:          return "V22-S1";
    case VM_HS_V22_SCR1:        return "V22-SCR1";
    case VM_HS_V21_MARK:        return "V21-mark";
    default:                    return "?";
    }
}

/* ========================================================================= */
/* plan の構築                                                               */
/* ========================================================================= */

static void plan_add(vm_handshake_t *h, vm_hs_stage_t st,
                     int duration_ms, int symbols)
{
    if (h->plan_len >= VM_HS_MAX_STAGES) return;
    h->plan[h->plan_len].stage       = st;
    h->plan[h->plan_len].duration_ms = duration_ms;
    h->plan[h->plan_len].symbols     = symbols;
    h->plan_len++;
}

/*
 * ---------------------------------------------------------------------------
 * V.34 / V.34+ / V.90 の plan
 * ---------------------------------------------------------------------------
 * 実機の 33.6k 接続にかかる時間は概ね 8〜12 秒。
 * その内訳 (ITU-T V.8/V.34 の規定と実測から):
 *
 *   ANSam            約 2000ms  (応答側が送り続ける)
 *   CM               約  400ms  (発呼側 V.21 ch1)
 *   JM               約  400ms  (応答側 V.21 ch2)
 *   CJ               約  100ms
 *   無音              75ms      (V.8/6.2 規定)
 *   INFO0            約  200ms  (600bps で約 100bit)
 *   Tone B / A       約  300ms  (位相反転を含むレンジング)
 *   L1               約  600ms
 *   L2               約  600ms
 *   INFO1            約  250ms
 *   S / !S           128T + 16T = 144 シンボル (3429baud で 42ms)
 *   PP               288 シンボル (84ms)
 *   TRN              約 1200ms  (等化器収束に時間がかかる)
 *   J                約   50ms
 *   MP               約  400ms
 *   E                約   50ms
 *   B1               約  200ms
 */
static void plan_v34(vm_handshake_t *h, bool fast)
{
    int scale_num = fast ? 1 : 1;
    int scale_den = fast ? 4 : 1;

#define MS(x)  ((x) * scale_num / scale_den)
#define SYM(x) (fast ? ((x) / 4 > 0 ? (x) / 4 : 1) : (x))

    /* ---- Phase 1 : V.8 ---- */
    plan_add(h, VM_HS_CT,             MS(500),  0);
    plan_add(h, VM_HS_ANSAM,          MS(2000), 0);
    plan_add(h, VM_HS_CM,             MS(400),  0);
    plan_add(h, VM_HS_JM,             MS(400),  0);
    plan_add(h, VM_HS_CJ,             MS(120),  0);
    plan_add(h, VM_HS_V8_END_SILENCE, MS(75),   0);

    /* ---- Phase 2 : ライン プロービング / レンジング ---- */
    plan_add(h, VM_HS_INFO0,   MS(220), 0);
    plan_add(h, VM_HS_TONE_B,  MS(300), 0);   /* 発呼側 1200Hz + 位相反転 */
    plan_add(h, VM_HS_TONE_A,  MS(300), 0);   /* 応答側 2400Hz + 位相反転 */
    plan_add(h, VM_HS_L1,      MS(600), 0);
    plan_add(h, VM_HS_L2,      MS(600), 0);
    plan_add(h, VM_HS_INFO1,   MS(260), 0);

    /* ---- Phase 3 : 等化器 / エコーキャンセラ訓練 ---- */
    plan_add(h, VM_HS_S,    0, SYM(128));    /* V.34/11.3.1 : 128T */
    plan_add(h, VM_HS_SBAR, 0, SYM(16));     /* V.34/11.3.1 :  16T */
    plan_add(h, VM_HS_PP,   0, SYM(288));    /* V.34/11.3.2 : 288T */
    plan_add(h, VM_HS_TRN,  MS(1200), 0);
    plan_add(h, VM_HS_J,    MS(50),   0);

    /* ---- Phase 4 : 最終訓練 / パラメータ確定 ---- */
    plan_add(h, VM_HS_MP, MS(400), 0);
    plan_add(h, VM_HS_E,  MS(60),  0);
    plan_add(h, VM_HS_B1, MS(200), 0);

#undef MS
#undef SYM
}

/*
 * ---------------------------------------------------------------------------
 * V.32 / V.32bis の plan (V.32/6.4)
 * ---------------------------------------------------------------------------
 * V.32 は V.8 を使わない (V.32bis は使う場合もある)。
 * 応答側が 2100Hz の応答トーン (ANS) を出し、その後
 * AA/AC の交替パターン -> S/!S -> TRN -> R1/R2/R3 と進む。
 *
 * 特徴的なのは「ヒュラララ」と聞こえる AA/AC 交替と、
 * ザーッというTRN (擬似ランダム QAM)。
 */
static void plan_v32(vm_handshake_t *h, bool fast)
{
    int d = fast ? 4 : 1;

    plan_add(h, VM_HS_CT,        500 / d,  0);
    /* V.32 の応答トーンは位相反転付き 2100Hz (ANS with PR) */
    plan_add(h, VM_HS_ANSAM,     2200 / d, 0);
    plan_add(h, VM_HS_V32_AA,    900 / d,  0);   /* AA/AC 交替  */
    plan_add(h, VM_HS_V32_S,     300 / d,  0);   /* S / !S      */
    plan_add(h, VM_HS_V32_TRN,   1400 / d, 0);   /* TRN 訓練    */
    plan_add(h, VM_HS_V32_R1R2R3, 400 / d, 0);   /* レート交渉  */
    plan_add(h, VM_HS_B1,        250 / d,  0);
}

/*
 * ---------------------------------------------------------------------------
 * V.22 / V.22bis の plan (V.22bis/6.3)
 * ---------------------------------------------------------------------------
 * 応答側が 2225Hz の無変調搬送波を約 1.5 秒出す。
 * 発呼側は 1200Hz 搬送波でこれに応じる。
 * V.22bis ではさらに S1 (00/11 の交替) を 100ms 送って
 * 「2400bps でいけるか」を確認する。
 */
static void plan_v22(vm_handshake_t *h, bool bis, bool fast)
{
    int d = fast ? 4 : 1;

    plan_add(h, VM_HS_CT,         500 / d,  0);
    plan_add(h, VM_HS_V22_USB1,   1800 / d, 0);   /* 2225Hz 無変調 */
    if (bis)
        plan_add(h, VM_HS_V22_S1, 250 / d,  0);   /* S1 パターン   */
    plan_add(h, VM_HS_V22_SCR1,   900 / d,  0);   /* スクランブル1 */
}

/*
 * ---------------------------------------------------------------------------
 * V.21 の plan
 * ---------------------------------------------------------------------------
 * V.21 は最も単純。応答側が 2100Hz 応答トーン (または直接 ch2 mark) を
 * 出し、双方が mark 周波数を保持したら接続完了。
 * 「ピー…ヒョロヒョロ」ではなく「ピーーー」だけで繋がる。
 */
static void plan_v21(vm_handshake_t *h, bool fast)
{
    int d = fast ? 4 : 1;
    plan_add(h, VM_HS_CT,        400 / d,  0);
    plan_add(h, VM_HS_ANSAM,     1600 / d, 0);
    plan_add(h, VM_HS_V21_MARK,  1200 / d, 0);
}

/* ========================================================================= */
/* 初期化                                                                    */
/* ========================================================================= */

vm_err_t vm_handshake_init(vm_handshake_t *h, vm_standard_t std, int bps,
                           int sample_rate, float gain, bool fast)
{
    int  symbol_rate = 3429;
    bool high_carrier = false;

    if (!h) return VM_ERR_INVAL;
    memset(h, 0, sizeof(*h));

    h->std         = std;
    h->bps         = bps;
    h->sample_rate = sample_rate > 0 ? sample_rate : VM_DSP_SAMPLE_RATE;
    h->gain        = gain;

    /* --- plan を構築 --- */
    switch (std) {
    case VM_STD_V90:
    case VM_STD_V34PLUS:
    case VM_STD_V34:
        plan_v34(h, fast);
        break;
    case VM_STD_V32BIS:
    case VM_STD_V32:
        plan_v32(h, fast);
        break;
    case VM_STD_V22BIS:
        plan_v22(h, true, fast);
        break;
    case VM_STD_V22:
        plan_v22(h, false, fast);
        break;
    case VM_STD_V21:
        plan_v21(h, fast);
        break;
    default:
        return VM_ERR_UNSUPPORTED;
    }

    /* --- 各ジェネレータを準備 --- */

    /*
     * ANSam (V.8/5.2.1):
     *   搬送波 2100Hz、変調 15Hz、変調度 0.2 (±20%)
     *   200ms 毎に 180 度位相反転
     *
     * V.32 の ANS (位相反転付き) も同じ 2100Hz なので共用する。
     * ただし V.32 の ANS は AM 変調が無いので変調度を 0 にする。
     */
    {
        float depth = (std == VM_STD_V32 || std == VM_STD_V32BIS ||
                       std == VM_STD_V21) ? 0.0f : 0.20f;
        vm_amgen_init_sr(&h->ansam, 2100.0, 15.0, depth, 0.5f,
                         h->sample_rate);
    }
    h->ansam_pr_count = 0;

    /*
     * CT (Calling Tone) : V.25 の 1300Hz。
     * 発呼側モデムが「私はモデムです」と知らせる短いトーン。
     * (FAX の CNG 1100Hz と同じ役割)
     */
    vm_tonegen_init(&h->ct);
    vm_tonegen_add_sr(&h->ct, 1300.0, 0.45f, 0.0, h->sample_rate);

    /* L1/L2 用 21 トーン プロービング信号 */
    vm_v34_probe_init(&h->probe, h->sample_rate, 0.55f);

    /* 純音ジェネレータ (Tone A/B, V.22 搬送波) */
    vm_tonegen_init(&h->puretone);

    /* V.21 FSK (CM/JM/CJ) */
    vm_fsk_tx_init(&h->fsk, true, h->sample_rate, 0.42f);

    /* INFO0/INFO1 用 DPSK */
    vm_dpsk_tx_init(&h->dpsk, h->sample_rate, 0.45f);

    /* V.34 QAM */
    vm_v34_pick_rate(bps, &symbol_rate, &high_carrier);
    vm_v34_mod_init(&h->qam, symbol_rate, high_carrier, true,
                    h->sample_rate, 0.40f);

    h->plan_pos      = 0;
    h->stage_started = false;
    return VM_OK;
}

/* ========================================================================= */
/* ステージ開始処理                                                          */
/* ========================================================================= */

static void stage_begin(vm_handshake_t *h)
{
    const vm_hs_step_t *st = &h->plan[h->plan_pos];
    int nbits;

    h->stage_remain_samples = (int)((int64_t)st->duration_ms
                                    * h->sample_rate / 1000);
    h->stage_remain_symbols = st->symbols;
    h->pr_timer = 0;
    h->pr_state = 0;

    switch (st->stage) {

    case VM_HS_CM:
        /* CM を V.21 チャネル 1 (発呼側) で送る */
        vm_fsk_tx_init(&h->fsk, true, h->sample_rate, 0.42f);
        nbits = vm_v8_build_cm(h->bitbuf, (int)sizeof(h->bitbuf), h->std);
        vm_fsk_tx_set_bits(&h->fsk, h->bitbuf, nbits);
        break;

    case VM_HS_JM:
        /* JM を V.21 チャネル 2 (応答側) で送る */
        vm_fsk_tx_init(&h->fsk, false, h->sample_rate, 0.42f);
        nbits = vm_v8_build_jm(h->bitbuf, (int)sizeof(h->bitbuf), h->std);
        vm_fsk_tx_set_bits(&h->fsk, h->bitbuf, nbits);
        break;

    case VM_HS_CJ:
        /* CJ は発呼側 = チャネル 1 */
        vm_fsk_tx_init(&h->fsk, true, h->sample_rate, 0.42f);
        nbits = vm_v8_build_cj(h->bitbuf, (int)sizeof(h->bitbuf));
        vm_fsk_tx_set_bits(&h->fsk, h->bitbuf, nbits);
        break;

    case VM_HS_INFO0:
        nbits = vm_v34_build_info0(h->bitbuf, (int)sizeof(h->bitbuf), true);
        vm_dpsk_tx_init(&h->dpsk, h->sample_rate, 0.45f);
        vm_dpsk_tx_set_bits(&h->dpsk, h->bitbuf, nbits);
        break;

    case VM_HS_INFO1:
        {
            int  sr; bool hc;
            vm_v34_pick_rate(h->bps, &sr, &hc);
            nbits = vm_v34_build_info1(h->bitbuf, (int)sizeof(h->bitbuf),
                                       true, sr, h->bps);
        }
        vm_dpsk_tx_init(&h->dpsk, h->sample_rate, 0.45f);
        vm_dpsk_tx_set_bits(&h->dpsk, h->bitbuf, nbits);
        break;

    case VM_HS_TONE_B:
        /*
         * Tone B : 1200Hz 純音。発呼側が送る (V.34/11.2.1.1)。
         * 40±1ms 毎に位相反転させて往復遅延を測る。
         */
        vm_tonegen_init(&h->puretone);
        vm_tonegen_add_sr(&h->puretone, 1200.0, 0.5f, 0.0, h->sample_rate);
        break;

    case VM_HS_TONE_A:
        /*
         * Tone A : 2400Hz 純音。応答側が送る (V.34/11.2.1.2)。
         */
        vm_tonegen_init(&h->puretone);
        vm_tonegen_add_sr(&h->puretone, 2400.0, 0.5f, 0.0, h->sample_rate);
        break;

    case VM_HS_V22_USB1:
        /*
         * V.22 応答側の無変調搬送波は 2225Hz。
         * (発呼側は 1200Hz)
         */
        vm_tonegen_init(&h->puretone);
        vm_tonegen_add_sr(&h->puretone, 2225.0, 0.45f, 0.0, h->sample_rate);
        vm_tonegen_add_sr(&h->puretone, 1200.0, 0.30f, 0.0, h->sample_rate);
        break;

    case VM_HS_V21_MARK:
        /*
         * V.21 のマーク保持。
         * ch1 mark = 980Hz、ch2 mark = 1650Hz を同時に鳴らす
         * (両方向の搬送波が回線上で重畳するため)。
         */
        vm_tonegen_init(&h->puretone);
        vm_tonegen_add_sr(&h->puretone,  980.0, 0.40f, 0.0, h->sample_rate);
        vm_tonegen_add_sr(&h->puretone, 1650.0, 0.40f, 0.0, h->sample_rate);
        break;

    case VM_HS_V32_AA:
        /*
         * V.32 の AA/AC 交替 (V.32/6.4.1)。
         * AA は 1800Hz 搬送波の +/-90 度交替 = 900Hz と 2700Hz の
         * 側波帯が立つ。実機では「ヒュラララ」と聞こえる。
         *
         * これを 2 トーン合成で近似する。
         */
        vm_tonegen_init(&h->puretone);
        vm_tonegen_add_sr(&h->puretone,  900.0, 0.35f, 0.0,  h->sample_rate);
        vm_tonegen_add_sr(&h->puretone, 2700.0, 0.35f, 0.25, h->sample_rate);
        break;

    case VM_HS_V32_S:
        /*
         * V.32 の S/!S。1800Hz 搬送波で 2 点交替 -> 1800±1200Hz。
         */
        vm_tonegen_init(&h->puretone);
        vm_tonegen_add_sr(&h->puretone,  600.0, 0.32f, 0.0,  h->sample_rate);
        vm_tonegen_add_sr(&h->puretone, 3000.0, 0.32f, 0.5,  h->sample_rate);
        break;

    case VM_HS_V32_R1R2R3:
        /*
         * R1/R2/R3 のレート信号。1800Hz 付近の 4 点 QAM。
         * V.32 の QAM 変調器 (2400baud) を流用する。
         */
        vm_v34_mod_init(&h->qam, 2400, false, true, h->sample_rate, 0.38f);
        break;

    case VM_HS_V32_TRN:
        /* V.32 TRN は 2400baud, 搬送波 1800Hz */
        vm_v34_mod_init(&h->qam, 2400, false, true, h->sample_rate, 0.40f);
        break;

    case VM_HS_V22_S1:
    case VM_HS_V22_SCR1:
        /* V.22bis は 600baud, 搬送波 1200/2400Hz */
        vm_v34_mod_init(&h->qam, 2400, false, true, h->sample_rate, 0.38f);
        break;

    case VM_HS_S:
    case VM_HS_SBAR:
    case VM_HS_PP:
    case VM_HS_TRN:
    case VM_HS_J:
    case VM_HS_MP:
    case VM_HS_E:
    case VM_HS_B1:
        /* V.34 QAM は init 済み。pattern_pos だけリセット */
        h->qam.pattern_pos = 0;
        break;

    default:
        break;
    }
    h->stage_started = true;
}

/* ========================================================================= */
/* ステージ実行                                                              */
/* ========================================================================= */

/*
 * 位相反転付きトーンを n サンプル出す。
 *
 * V.34 の Tone A/B は 40±1ms 毎、V.8 の ANSam は 200ms 毎に
 * 180 度位相反転する。この「プツッ」という不連続が実機の音の
 * 特徴になっている。
 */
static void render_pr_tone(vm_handshake_t *h, vm_tonegen_t *g,
                           float *out, int n, int period_ms)
{
    int done = 0;
    int period = (int)((int64_t)period_ms * h->sample_rate / 1000);
    if (period <= 0) period = 1;

    while (done < n) {
        int avail = period - h->pr_timer;
        int chunk = n - done;
        if (chunk > avail) chunk = avail;
        if (chunk <= 0) chunk = 1;

        vm_tonegen_render_add(g, out + done, chunk);

        done        += chunk;
        h->pr_timer += chunk;
        if (h->pr_timer >= period) {
            int k;
            h->pr_timer = 0;
            /* 全パーシャルの位相を 180 度反転 */
            for (k = 0; k < g->count; k++)
                vm_dds_invert(&g->dds[k]);
        }
    }
}

/* ANSam の位相反転版 */
static void render_ansam(vm_handshake_t *h, float *out, int n)
{
    int done = 0;
    /* V.8/5.2.1 : 200ms 毎に位相反転 */
    int period = (int)((int64_t)200 * h->sample_rate / 1000);

    while (done < n) {
        int avail = period - h->ansam_pr_count;
        int chunk = n - done;
        if (chunk > avail) chunk = avail;
        if (chunk <= 0) chunk = 1;

        vm_amgen_render_add(&h->ansam, out + done, chunk);

        done              += chunk;
        h->ansam_pr_count += chunk;
        if (h->ansam_pr_count >= period) {
            h->ansam_pr_count = 0;
            vm_dds_invert(&h->ansam.carrier);
        }
    }
}

/*
 * 現ステージを最大 n サンプル分実行する。
 * 実際に生成したサンプル数を返す。
 * ステージが終わったら *finished = true。
 */
static int stage_render(vm_handshake_t *h, float *out, int n, bool *finished)
{
    const vm_hs_step_t *st = &h->plan[h->plan_pos];
    int produced = n;

    *finished = false;

    /* 時間指定のステージは残りサンプル数で切る */
    if (st->symbols == 0) {
        if (produced > h->stage_remain_samples)
            produced = h->stage_remain_samples;
        if (produced <= 0) {
            *finished = true;
            return 0;
        }
    }

    switch (st->stage) {

    case VM_HS_CT:
        vm_tonegen_render_add(&h->ct, out, produced);
        break;

    case VM_HS_ANSAM:
        render_ansam(h, out, produced);
        break;

    case VM_HS_CM:
    case VM_HS_JM:
    case VM_HS_CJ:
        /*
         * FSK が先に終わった場合、残り時間は無音にする。
         * (実機でも CM 送出後に少し間がある)
         */
        vm_fsk_tx_render_add(&h->fsk, out, produced);
        break;

    case VM_HS_V8_END_SILENCE:
    case VM_HS_DONE:
        /* 無音: 何も加算しない */
        break;

    case VM_HS_INFO0:
    case VM_HS_INFO1:
        vm_dpsk_tx_render_add(&h->dpsk, out, produced);
        break;

    case VM_HS_TONE_B:
    case VM_HS_TONE_A:
        /* V.34/11.2.1 : 40ms 毎の位相反転 */
        render_pr_tone(h, &h->puretone, out, produced, 40);
        break;

    case VM_HS_L1:
        vm_tonegen_render_add(&h->probe, out, produced);
        break;

    case VM_HS_L2:
        /*
         * L2 は L1 と同じ 21 トーンだが送出レベルが低い
         * (V.34/11.2.3 : L1 より 6dB 低い)。
         * 6dB = 振幅 0.5 倍。
         */
        {
            float saved = h->probe.gain;
            h->probe.gain = saved * 0.5f;
            vm_tonegen_render_add(&h->probe, out, produced);
            h->probe.gain = saved;
        }
        break;

    /* ---- V.34 Phase 3/4 : シンボル数指定 ---- */
    case VM_HS_S:
    case VM_HS_SBAR:
    case VM_HS_PP:
        {
            vm_v34_signal_t sig =
                (st->stage == VM_HS_S)    ? VM_V34SIG_S :
                (st->stage == VM_HS_SBAR) ? VM_V34SIG_SBAR : VM_V34SIG_PP;
            bool end = vm_v34_mod_render_add(&h->qam, sig, out, produced,
                                             &h->stage_remain_symbols);
            if (end) {
                *finished = true;
                return produced;
            }
        }
        break;

    case VM_HS_TRN:
    case VM_HS_J:
        {
            int dummy = 1 << 28;   /* 時間で切るのでシンボル数は無制限 */
            vm_v34_mod_render_add(&h->qam, VM_V34SIG_TRN, out, produced,
                                  &dummy);
        }
        break;

    case VM_HS_MP:
        {
            int dummy = 1 << 28;
            vm_v34_mod_render_add(&h->qam, VM_V34SIG_MP, out, produced,
                                  &dummy);
        }
        break;

    case VM_HS_E:
        {
            int dummy = 1 << 28;
            vm_v34_mod_render_add(&h->qam, VM_V34SIG_E, out, produced,
                                  &dummy);
        }
        break;

    case VM_HS_B1:
    case VM_HS_V32_TRN:
    case VM_HS_V32_R1R2R3:
    case VM_HS_V22_S1:
    case VM_HS_V22_SCR1:
        {
            int dummy = 1 << 28;
            vm_v34_signal_t sig =
                (st->stage == VM_HS_V22_S1) ? VM_V34SIG_S : VM_V34SIG_TRN;
            vm_v34_mod_render_add(&h->qam, sig, out, produced, &dummy);
        }
        break;

    /* ---- V.32 / V.22 / V.21 の純音系 ---- */
    case VM_HS_V32_AA:
    case VM_HS_V32_S:
    case VM_HS_V22_USB1:
    case VM_HS_V21_MARK:
        vm_tonegen_render_add(&h->puretone, out, produced);
        break;

    default:
        break;
    }

    /* 時間指定ステージの残量を減らす */
    if (st->symbols == 0) {
        h->stage_remain_samples -= produced;
        if (h->stage_remain_samples <= 0)
            *finished = true;
    }
    return produced;
}

/* ========================================================================= */
/* 公開 API                                                                  */
/* ========================================================================= */

bool vm_handshake_render_add(vm_handshake_t *h, float *out, int n)
{
    int done = 0;

    if (!h || !out || n <= 0) return true;

    while (done < n) {
        bool finished = false;
        int  got;

        if (h->plan_pos >= h->plan_len)
            return true;    /* 全ステージ完了 */

        if (!h->stage_started)
            stage_begin(h);

        got = stage_render(h, out + done, n - done, &finished);

        if (got <= 0 && !finished) {
            /* 進捗が無い -> 安全のためステージを進める */
            finished = true;
        }
        done += got;

        if (finished) {
            h->plan_pos++;
            h->stage_started = false;
        }
    }

    h->total_samples += (uint64_t)n;
    return (h->plan_pos >= h->plan_len);
}

vm_hs_stage_t vm_handshake_current_stage(const vm_handshake_t *h)
{
    if (!h || h->plan_pos >= h->plan_len) return VM_HS_DONE;
    return h->plan[h->plan_pos].stage;
}

int vm_handshake_total_ms(const vm_handshake_t *h)
{
    int i, total = 0;
    if (!h) return 0;

    for (i = 0; i < h->plan_len; i++) {
        if (h->plan[i].duration_ms > 0) {
            total += h->plan[i].duration_ms;
        } else if (h->plan[i].symbols > 0) {
            /* シンボル数 -> ms */
            int sr = h->qam.symbol_rate > 0 ? h->qam.symbol_rate : 3429;
            total += h->plan[i].symbols * 1000 / sr;
        }
    }
    return total;
}
