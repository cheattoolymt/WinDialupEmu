/*
 * vm_sequence.c - 接続音響シーケンサ 実装
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 仕様書「音声シーケンス (重要)」の 1〜5 をリアルタイム合成で実行する。
 * 録音の再生は一切行わない。
 */
#include "vmodem/vm_sequence.h"
#include "vmodem/vm_log.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

/* ms -> サンプル数 */
static int ms_to_samples(int ms, int sr)
{
    if (ms <= 0) return 0;
    return (int)(((int64_t)ms * (int64_t)sr) / 1000);
}

/* ===========================================================================
 * 各段の長さの既定値
 * ===========================================================================
 * 日本の公衆網 (NTT) の実測値に合わせる。
 *
 *   発信音 (DT)   : 400Hz 連続。受話器を上げてから概ね即座に聞こえる。
 *                   実機モデムは S6 レジスタ (既定 2 秒) だけ待ってから
 *                   ダイアルを始めるので、2 秒鳴らす。
 *   DTMF          : ITU-T Q.24 は最小 40ms ON / 40ms OFF。
 *                   実際の交換機は 80〜100ms 程度で送る事が多い。
 *                   モデムの S11 レジスタ既定値も 95ms。
 *   呼出音 (RBT)  : 日本は 1 秒 ON / 2 秒 OFF (3 秒周期)。
 *                   北米は 2s ON / 4s OFF なので国によって違う。
 *                   400Hz を 15Hz で AM 変調した音。
 *   応答までの無音: 相手が受話器を上げてから応答トーンが出るまでの間。
 *                   実際は 0.2〜0.5 秒程度。
 * =========================================================================*/
#define DEF_OFFHOOK_MS       200
#define DEF_DIALTONE_MS     2000
#define DEF_DTMF_ON_MS        95     /* S11 既定 */
#define DEF_DTMF_OFF_MS       95
#define DEF_RINGBACK_ON_MS  1000     /* 日本: 1 秒 ON */
#define DEF_RINGBACK_OFF_MS 2000     /* 日本: 2 秒 OFF */
#define DEF_RINGBACK_COUNT     2
#define DEF_ANSWER_WAIT_MS   300
#define DEF_END_SILENCE_MS   200

/* ===========================================================================
 * フェーズ遷移
 * =========================================================================*/

/* 発信シーケンスにおける次フェーズ */
static vm_phase_t next_phase_dial(vm_phase_t p)
{
    switch (p) {
    case VM_PHASE_OFFHOOK:      return VM_PHASE_DIALTONE;
    case VM_PHASE_DIALTONE:     return VM_PHASE_DTMF;
    case VM_PHASE_DTMF:         return VM_PHASE_RINGBACK;
    case VM_PHASE_RINGBACK:     return VM_PHASE_ANSWER_TONE;
    case VM_PHASE_ANSWER_TONE:  return VM_PHASE_SILENCE;
    case VM_PHASE_SILENCE:      return VM_PHASE_CONNECTED;
    default:                    return VM_PHASE_CONNECTED;
    }
}

/*
 * 現フェーズに入る時の準備。
 * 各生成器はここで初期化する (位相を 0 から始めるため)。
 */
static void enter_phase(vm_sequence_t *s, vm_phase_t p)
{
    s->phase         = p;
    s->phase_samples = 0;
    s->phase_len     = 0;

    switch (p) {
    case VM_PHASE_OFFHOOK:
        s->phase_len = s->len_offhook;
        break;

    /* ----------------------------------------------------------------- *
     * 1. ダイアルトーン
     * -----------------------------------------------------------------
     * 日本の発信音は 400Hz ± 20Hz の連続音 (単一周波数)。
     * 北米は 350Hz + 440Hz の 2 周波なので、そちらを模したい場合は
     * partial を 2 本足せば良い。
     * ----------------------------------------------------------------- */
    case VM_PHASE_DIALTONE:
        vm_tonegen_init(&s->dialtone);
        vm_tonegen_add_sr(&s->dialtone, 400.0, 0.5f, 0.0, s->sample_rate);
        vm_tonegen_set_gain(&s->dialtone, 1.0f);
        s->phase_len = s->len_dialtone;
        break;

    /* ----------------------------------------------------------------- *
     * 2. DTMF 発信音
     * -----------------------------------------------------------------
     * 長さは桁数から自動的に決まるので phase_len は使わず、
     * vm_dtmf_tx_render_add() の戻り値で完了を判定する。
     * ----------------------------------------------------------------- */
    case VM_PHASE_DTMF:
        vm_dtmf_tx_init(&s->dtmf, s->digits,
                        s->dtmf_on_ms, s->dtmf_off_ms,
                        s->sample_rate);
        break;

    /* ----------------------------------------------------------------- *
     * 3. 呼び出し音
     * -----------------------------------------------------------------
     * 400Hz を 15Hz で振幅変調する。
     * vm_amgen_t は連続音を作るので、ON/OFF の断続は
     * phase_samples を見て自前で作る。
     * ----------------------------------------------------------------- */
    case VM_PHASE_RINGBACK:
        vm_amgen_init_sr(&s->ringback, 400.0, 15.0, 0.5f, 0.45f,
                         s->sample_rate);
        s->ringback_done = 0;
        s->phase_len     = s->len_ringback * s->ringback_cycles;
        break;

    /* ----------------------------------------------------------------- *
     * 4. モデムネゴシエーション音
     * -----------------------------------------------------------------
     * ここが本体。vm_handshake_t が選択規格に応じた
     * ITU-T 手順 (V.34 なら ANSam/CM/JM/CJ/INFO0/Tone A/B/L1/L2/
     * INFO1/S/!S/PP/TRN/J/MP/E/B1) を順に合成する。
     * ----------------------------------------------------------------- */
    case VM_PHASE_ANSWER_TONE:
        if (vm_handshake_init(&s->hs, s->std, s->bps, s->sample_rate,
                              1.0f, s->fast) == VM_OK) {
            s->hs_ready = true;
            VM_LOGI("handshake: %s %d bps, %d stages, ~%d ms",
                    vm_standard_name(s->std), s->bps,
                    s->hs.plan_len, vm_handshake_total_ms(&s->hs));
        } else {
            /* 規格が未対応: 無音で通過させる (接続自体は成立させる) */
            s->hs_ready = false;
            s->phase_len = ms_to_samples(500, s->sample_rate);
            VM_LOGW("handshake init failed for %s, using silence",
                    vm_standard_name(s->std));
        }
        break;

    /* ----------------------------------------------------------------- *
     * 5. 無音
     * -----------------------------------------------------------------
     * ネゴシエーション完了 -> 音声出力停止。
     * この後、上位層が CONNECT xxxxx を COM ポートへ返す。
     * ----------------------------------------------------------------- */
    case VM_PHASE_SILENCE:
        s->phase_len = s->len_end_silence;
        break;

    case VM_PHASE_CONNECTED:
        s->done = true;
        break;

    default:
        break;
    }

    VM_LOGD("sequence phase -> %s (%d samples)",
            vm_phase_name(p), s->phase_len);
}

/* ===========================================================================
 * 開始
 * =========================================================================*/

static void seq_common_init(vm_sequence_t *s, const vm_config_t *cfg,
                            vm_standard_t std, int bps,
                            int sample_rate, float gain)
{
    int rb_on, rb_off;

    memset(s, 0, sizeof(*s));

    s->sample_rate = sample_rate > 0 ? sample_rate : VM_DSP_SAMPLE_RATE;
    s->std   = std;
    s->bps   = bps;
    s->gain  = gain > 0.0f ? gain : 1.0f;
    s->muted = false;
    s->fast  = cfg ? cfg->fast_connect : false;

    /* [timing] セクションの値。0 なら既定値 */
    s->len_offhook = ms_to_samples(DEF_OFFHOOK_MS, s->sample_rate);

    {
        int dt = (cfg && cfg->dialtone_ms > 0) ? cfg->dialtone_ms
                                              : DEF_DIALTONE_MS;
        if (s->fast && dt > 200) dt = 200;
        s->len_dialtone = ms_to_samples(dt, s->sample_rate);
    }

    /*
     * DTMF の ON/OFF 長。
     * ITU-T Q.24 の下限は 40ms/40ms なので、設定値がそれを
     * 下回っていたら規格下限に丸める
     * (短すぎる DTMF は実際の交換機でも検出されない)。
     * fast モードはテスト専用なので下限を無視する。
     */
    if (s->fast) {
        s->dtmf_on_ms  = 20;
        s->dtmf_off_ms = 20;
    } else {
        s->dtmf_on_ms  = (cfg && cfg->dtmf_on_ms  > 0) ? cfg->dtmf_on_ms
                                                       : DEF_DTMF_ON_MS;
        s->dtmf_off_ms = (cfg && cfg->dtmf_off_ms > 0) ? cfg->dtmf_off_ms
                                                       : DEF_DTMF_OFF_MS;
        if (s->dtmf_on_ms  < 40) s->dtmf_on_ms  = 40;   /* Q.24 下限 */
        if (s->dtmf_off_ms < 40) s->dtmf_off_ms = 40;
    }

    rb_on  = DEF_RINGBACK_ON_MS;
    rb_off = DEF_RINGBACK_OFF_MS;
    if (s->fast) { rb_on = 100; rb_off = 100; }
    s->len_ringback = ms_to_samples(rb_on + rb_off, s->sample_rate);
    /* ON 区間の長さは render 側で再計算するので比率を保持しておく */
    s->ringback_cycles = (cfg && cfg->ringback_count > 0)
                       ? cfg->ringback_count : DEF_RINGBACK_COUNT;

    s->len_answer_wait = ms_to_samples(s->fast ? 50 : DEF_ANSWER_WAIT_MS,
                                       s->sample_rate);
    s->len_end_silence = ms_to_samples(s->fast ? 50 : DEF_END_SILENCE_MS,
                                       s->sample_rate);

    s->done    = false;
    s->aborted = false;
    s->hs_ready = false;
}

vm_err_t vm_sequence_start_dial(vm_sequence_t *s,
                                const char *digits,
                                const vm_isp_entry_t *isp,
                                const vm_config_t *cfg,
                                vm_standard_t std, int bps,
                                int sample_rate, float gain)
{
    if (!s) return VM_ERR_INVAL;
    (void)isp;   /* 速度/規格は呼び出し側が既に解決して渡す */

    seq_common_init(s, cfg, std, bps, sample_rate, gain);

    /*
     * DTMF に渡す番号を「数字のみ」に正規化する。
     * ハイフンや修飾子 (W , ; T P) は音にしない。
     *
     * なお # と * は本来 DTMF に存在するが、
     * vm_number_normalize() が数字以外を落とすため除去される。
     * 仕様の照合規則 (数字のみ) と一貫させるためこれで良い。
     */
    vm_number_normalize(digits, s->digits, sizeof(s->digits));

    if (s->digits[0] == '\0') {
        VM_LOGW("sequence_start_dial: no digits to dial");
        return VM_ERR_INVAL;
    }

    VM_LOGI("dial sequence: \"%s\" -> %s %d bps (%s)",
            s->digits, vm_standard_name(std), bps,
            s->fast ? "fast" : "realtime");

    enter_phase(s, VM_PHASE_OFFHOOK);
    return VM_OK;
}

vm_err_t vm_sequence_start_answer(vm_sequence_t *s,
                                  const vm_config_t *cfg,
                                  vm_standard_t std, int bps,
                                  int sample_rate, float gain)
{
    if (!s) return VM_ERR_INVAL;

    seq_common_init(s, cfg, std, bps, sample_rate, gain);
    s->digits[0] = '\0';

    VM_LOGI("answer sequence: %s %d bps", vm_standard_name(std), bps);

    /*
     * 着信側はダイアルトーン・DTMF・呼出音を鳴らさない。
     * 即座に応答トーン (ANSam) から始める。
     */
    enter_phase(s, VM_PHASE_ANSWER_TONE);
    return VM_OK;
}

void vm_sequence_abort(vm_sequence_t *s)
{
    if (!s) return;
    VM_LOGI("sequence aborted at %s", vm_phase_name(s->phase));
    s->aborted = true;
    s->done    = true;
    s->phase   = VM_PHASE_DISCONNECTING;
}

/* ===========================================================================
 * レンダリング
 * =========================================================================*/

/*
 * 呼出音の ON/OFF 断続を作る。
 * 1 周期 = len_ringback サンプル。前半 1/3 が ON、後半 2/3 が OFF
 * (日本の 1s ON / 2s OFF に対応)。
 *
 * fast モードでは 100ms/100ms なので比率が変わるが、
 * 「鳴る -> 止まる」の構造は同じなので聴感上の意味は保たれる。
 */
static void render_ringback(vm_sequence_t *s, float *out, int n)
{
    int on_len;
    int i = 0;

    if (s->fast) on_len = s->len_ringback / 2;
    else         on_len = s->len_ringback / 3;   /* 1s / (1s+2s) */

    while (i < n) {
        int pos_in_cycle = s->phase_samples % s->len_ringback;
        int chunk;

        if (pos_in_cycle < on_len) {
            /* ON 区間 */
            chunk = on_len - pos_in_cycle;
            if (chunk > n - i) chunk = n - i;
            vm_amgen_render_add(&s->ringback, out + i, chunk);
        } else {
            /* OFF 区間: 何も足さない (呼び出し側が 0 クリア済み) */
            chunk = s->len_ringback - pos_in_cycle;
            if (chunk > n - i) chunk = n - i;
        }

        i                += chunk;
        s->phase_samples += chunk;
    }
}

bool vm_sequence_render(vm_sequence_t *s, float *out, int n)
{
    int done_samples = 0;

    if (!s || !out || n <= 0) return s ? s->done : true;

    /* 常に 0 クリアしてから加算する (各生成器は render_add 形式) */
    memset(out, 0, (size_t)n * sizeof(float));

    if (s->aborted) return true;

    while (done_samples < n) {
        int  want = n - done_samples;
        int  chunk;
        bool phase_finished = false;
        float *dst = out + done_samples;

        if (s->phase == VM_PHASE_CONNECTED) {
            /* 完了後は無音 (既に 0 クリア済み) */
            s->done = true;
            s->total_samples += (uint64_t)want;
            break;
        }

        /* 残りサンプル数を決める */
        if (s->phase_len > 0) {
            chunk = s->phase_len - s->phase_samples;
            if (chunk > want) chunk = want;
            if (chunk <= 0) { phase_finished = true; chunk = 0; }
        } else {
            chunk = want;
        }

        if (chunk > 0) {
            switch (s->phase) {

            case VM_PHASE_OFFHOOK:
                /* 無音 */
                s->phase_samples += chunk;
                break;

            /* --- 1. ダイアルトーン --- */
            case VM_PHASE_DIALTONE:
                vm_tonegen_render_add(&s->dialtone, dst, chunk);
                s->phase_samples += chunk;
                break;

            /* --- 2. DTMF --- */
            case VM_PHASE_DTMF:
                if (vm_dtmf_tx_render_add(&s->dtmf, dst, chunk))
                    phase_finished = true;
                s->phase_samples += chunk;
                break;

            /* --- 3. 呼出音 --- */
            case VM_PHASE_RINGBACK:
                render_ringback(s, dst, chunk);
                /* phase_samples は render_ringback が進める */
                if (s->phase_samples >= s->phase_len) phase_finished = true;
                break;

            /* --- 4. ネゴシエーション音 --- */
            case VM_PHASE_ANSWER_TONE:
                if (s->hs_ready) {
                    if (vm_handshake_render_add(&s->hs, dst, chunk))
                        phase_finished = true;
                }
                s->phase_samples += chunk;
                break;

            /* --- 5. 無音 --- */
            case VM_PHASE_SILENCE:
                s->phase_samples += chunk;
                break;

            default:
                s->phase_samples += chunk;
                break;
            }

            done_samples     += chunk;
            s->total_samples += (uint64_t)chunk;
        }

        /* 長さ制限に達したか */
        if (s->phase_len > 0 && s->phase_samples >= s->phase_len)
            phase_finished = true;

        if (phase_finished) {
            vm_phase_t nx = next_phase_dial(s->phase);

            /*
             * 呼出音の後は「応答までの無音」を挟む。
             * OFFHOOK フェーズを再利用せず、ANSWER_TONE の直前に
             * 短い無音を作るため phase_len を上書きする形にする。
             */
            if (s->phase == VM_PHASE_RINGBACK && s->len_answer_wait > 0) {
                s->phase         = VM_PHASE_OFFHOOK;   /* 無音を出す状態 */
                s->phase_samples = 0;
                s->phase_len     = s->len_answer_wait;
                /*
                 * 次に OFFHOOK が終わったら ANSWER_TONE に行きたいが、
                 * next_phase_dial(OFFHOOK) は DIALTONE を返してしまう。
                 * そこで「呼出音を鳴らし終えた」印として
                 * ringback_done を使い、下で分岐する。
                 */
                s->ringback_done = 1;
                VM_LOGD("sequence phase -> answer wait (%d samples)",
                        s->phase_len);
                continue;
            }
            if (s->phase == VM_PHASE_OFFHOOK && s->ringback_done) {
                s->ringback_done = 0;
                enter_phase(s, VM_PHASE_ANSWER_TONE);
                continue;
            }

            enter_phase(s, nx);
        }

        /* 進捗が無い場合の無限ループ防止 */
        if (chunk == 0 && !phase_finished) {
            VM_LOGW("sequence stalled at %s", vm_phase_name(s->phase));
            s->done = true;
            break;
        }
    }

    /* 音量とミュートの適用 */
    if (s->muted) {
        memset(out, 0, (size_t)n * sizeof(float));
    } else if (s->gain != 1.0f) {
        int i;
        for (i = 0; i < n; i++) out[i] *= s->gain;
    }

    /* クリップ (合成音が加算で 1.0 を超える可能性がある) */
    {
        int i;
        for (i = 0; i < n; i++) {
            if (out[i] >  1.0f) out[i] =  1.0f;
            else if (out[i] < -1.0f) out[i] = -1.0f;
        }
    }

    return s->done;
}

/* ===========================================================================
 * 情報取得
 * =========================================================================*/
vm_phase_t vm_sequence_phase(const vm_sequence_t *s)
{
    return s ? s->phase : VM_PHASE_IDLE;
}

const char *vm_sequence_status(const vm_sequence_t *s)
{
    static char buf[128];

    if (!s) return "idle";

    if (s->phase == VM_PHASE_ANSWER_TONE && s->hs_ready) {
        snprintf(buf, sizeof(buf), "%s/%s",
                 vm_phase_name(s->phase),
                 vm_hs_stage_name(vm_handshake_current_stage(&s->hs)));
        return buf;
    }
    return vm_phase_name(s->phase);
}

int vm_sequence_total_ms(const vm_sequence_t *s)
{
    int ms = 0;
    int sr;

    if (!s) return 0;
    sr = s->sample_rate > 0 ? s->sample_rate : VM_DSP_SAMPLE_RATE;

    ms += (s->len_offhook     * 1000) / sr;
    ms += (s->len_dialtone    * 1000) / sr;
    /* DTMF: 桁数 × (ON+OFF) */
    ms += (int)strlen(s->digits) * (s->dtmf_on_ms + s->dtmf_off_ms);
    ms += ((s->len_ringback * s->ringback_cycles) * 1000) / sr;
    ms += (s->len_answer_wait * 1000) / sr;
    if (s->hs_ready) ms += vm_handshake_total_ms(&s->hs);
    else             ms += s->fast ? 100 : 2000;   /* 推定 */
    ms += (s->len_end_silence * 1000) / sr;

    return ms;
}

void vm_sequence_set_gain(vm_sequence_t *s, float gain)
{
    if (!s) return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    s->gain = gain;
}

void vm_sequence_set_muted(vm_sequence_t *s, bool muted)
{
    if (!s) return;
    s->muted = muted;
}
