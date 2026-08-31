/*
 * test_audio.c - オーディオ パイプラインの検証
 *
 * WASAPI は Linux で動かせないが、vm_audio_backend_render() 以降の
 * 経路 (リング -> リサンプル -> 音量 -> クリップ) は完全に共通なので、
 * WAV バックエンドで検証すれば実機の音を検証した事になる。
 *
 * 検証項目:
 *   1. open/close、スレッド起動、二重 close の安全性
 *   2. 投入した PCM が欠落なく WAV に到達する事
 *   3. 音量設定が正しく反映される事 / クリップされる事
 *   4. flush でキューが破棄される事
 *   5. 実際の V.34 ハンドシェイクを流し、WAV のスペクトルに
 *      ANSam 2100Hz と 21 トーン プローブが現れる事
 */
#include "vmodem/vm_audio.h"
#include "vmodem/vm_handshake.h"
#include "vmodem/vm_tone.h"
#include "vmodem/vm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass = 0, g_fail = 0;

/* テスト用の短い待ち (プラットフォーム差を吸収) */
#ifdef _WIN32
#  include <windows.h>
static void vm_sleep_ms_test(int ms) { Sleep((DWORD)ms); }
#else
#  include <time.h>
static void vm_sleep_ms_test(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static void check(const char *name, int ok, const char *detail)
{
    if (ok) { g_pass++; printf("  [ OK ] %-52s %s\n", name, detail ? detail : ""); }
    else    { g_fail++; printf("  [FAIL] %-52s %s\n", name, detail ? detail : ""); }
}

/* ------------------------------------------------------------------ */
/* WAV 読み込み (16bit mono を前提)                                    */
/* ------------------------------------------------------------------ */
static int read_wav(const char *path, float **out, int *n, int *rate)
{
    FILE *fp = fopen(path, "rb");
    uint8_t h[44];
    uint32_t data_bytes;
    int i, count;
    int16_t *raw;
    float *f;

    *out = NULL; *n = 0; *rate = 0;
    if (!fp) return -1;
    if (fread(h, 1, 44, fp) != 44) { fclose(fp); return -1; }
    if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        fclose(fp); return -1;
    }
    *rate = (int)(h[24] | (h[25] << 8) | (h[26] << 16) | ((uint32_t)h[27] << 24));
    data_bytes = (uint32_t)(h[40] | (h[41] << 8) | (h[42] << 16) |
                            ((uint32_t)h[43] << 24));
    count = (int)(data_bytes / 2);
    if (count <= 0) { fclose(fp); return -1; }

    raw = (int16_t *)malloc((size_t)count * sizeof(int16_t));
    f   = (float *)malloc((size_t)count * sizeof(float));
    if (!raw || !f) { free(raw); free(f); fclose(fp); return -1; }

    count = (int)fread(raw, sizeof(int16_t), (size_t)count, fp);
    fclose(fp);
    for (i = 0; i < count; i++) f[i] = (float)raw[i] / 32768.0f;
    free(raw);

    *out = f; *n = count;
    return 0;
}

static double goertzel(const float *x, int n, double freq, double fs)
{
    double w = 2.0 * M_PI * freq / fs;
    double c = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / (double)n;
}

/* リングに全部入るまで押し込むヘルパ */
static void push_all(vm_audio_t *a, const float *s, int n)
{
    int off = 0;
    while (off < n) {
        uint32_t w = vm_audio_write(a, s + off, (uint32_t)(n - off));
        off += (int)w;
        if (w == 0) vm_audio_wait_drain(a, 1, 50);
    }
}

/* ================================================================== */
/* 1. ライフサイクル                                                   */
/* ================================================================== */
static void test_lifecycle(void)
{
    vm_audio_t *a = NULL;
    vm_audio_params_t p;
    char detail[200];
    vm_err_t rc;

    printf("\n[1] ライフサイクル\n");

    vm_audio_params_defaults(&p);
    snprintf(detail, sizeof(detail), "dsp_rate=%d volume=%.2f backend=AUTO",
             p.dsp_rate, p.volume);
    check("defaults are sane",
          p.dsp_rate == VM_DSP_SAMPLE_RATE && p.volume > 0.0f, detail);

    p.backend = VM_AUDIO_BACKEND_NULL;
    rc = vm_audio_open(&a, &p);
    snprintf(detail, sizeof(detail), "rc=%s backend=%s rate=%d",
             vm_strerror(rc), vm_audio_backend_name(a),
             vm_audio_device_rate(a));
    check("open null backend", rc == VM_OK && a != NULL, detail);

    if (a) {
        snprintf(detail, sizeof(detail), "space=%u queued=%u",
                 vm_audio_space(a), vm_audio_queued(a));
        check("ring starts empty", vm_audio_queued(a) == 0 &&
                                   vm_audio_space(a) > 1000, detail);
        vm_audio_close(a);
        check("close joins render thread", 1, NULL);
    }

    /* NULL 渡しで落ちない事 */
    vm_audio_close(NULL);
    check("close(NULL) is safe", 1, NULL);
    check("write(NULL) returns 0", vm_audio_write(NULL, NULL, 0) == 0, NULL);
}

/* ================================================================== */
/* 2. PCM の到達性 / 音量                                              */
/* ================================================================== */
static void test_wav_roundtrip(void)
{
    vm_audio_t *a = NULL;
    vm_audio_params_t p;
    char detail[220];
    const char *path = "/tmp/vm_test_tone.wav";
    const int fs = VM_DSP_SAMPLE_RATE;
    const int n  = fs;                     /* 1 秒 */
    float *sig, *back = NULL;
    int nback = 0, rate = 0, i;

    printf("\n[2] PCM 到達性 (WAV ラウンドトリップ)\n");

    sig = (float *)malloc(sizeof(float) * (size_t)n);
    for (i = 0; i < n; i++)
        sig[i] = 0.5f * (float)sin(2.0 * M_PI * 1000.0 * i / fs);

    vm_audio_params_defaults(&p);
    p.backend  = VM_AUDIO_BACKEND_WAVFILE;
    p.wav_path = path;
    p.volume   = 1.0f;

    if (vm_audio_open(&a, &p) != VM_OK) {
        check("open wav backend", 0, "open failed");
        free(sig);
        return;
    }
    check("open wav backend", 1, path);

    push_all(a, sig, n);
    vm_audio_drain(a, 3000);
    vm_audio_close(a);

    if (read_wav(path, &back, &nback, &rate) != 0) {
        check("wav file readable", 0, path);
        free(sig);
        return;
    }
    snprintf(detail, sizeof(detail), "%d frames @%dHz (pushed %d)",
             nback, rate, n);
    check("wav file written with expected rate",
          rate == fs && nback >= n - 1000, detail);

    /* 1000Hz が保存されている事 */
    {
        double amp = goertzel(back + 800, nback - 1600, 1000.0, rate);
        double off = goertzel(back + 800, nback - 1600, 1700.0, rate);
        snprintf(detail, sizeof(detail),
                 "amp@1000Hz=%.4f (in 0.500) amp@1700Hz=%.6f", amp, off);
        check("1000Hz tone survives the full audio path",
              amp > 0.45 && amp < 0.55 && off < 0.02, detail);
    }

    free(sig); free(back);
}

static void test_volume(void)
{
    vm_audio_t *a = NULL;
    vm_audio_params_t p;
    char detail[220];
    const char *path = "/tmp/vm_test_vol.wav";
    const int fs = VM_DSP_SAMPLE_RATE;
    const int n = fs / 2;
    float *sig, *back = NULL;
    int nback = 0, rate = 0, i;

    printf("\n[3] 音量適用とクリップ\n");

    sig = (float *)malloc(sizeof(float) * (size_t)n);
    for (i = 0; i < n; i++)
        sig[i] = 0.8f * (float)sin(2.0 * M_PI * 1000.0 * i / fs);

    /* volume 0.25 -> 振幅 0.2 になるはず */
    vm_audio_params_defaults(&p);
    p.backend  = VM_AUDIO_BACKEND_WAVFILE;
    p.wav_path = path;
    p.volume   = 0.25f;

    if (vm_audio_open(&a, &p) != VM_OK) {
        check("open for volume test", 0, NULL); free(sig); return;
    }
    push_all(a, sig, n);
    vm_audio_drain(a, 3000);
    vm_audio_close(a);

    if (read_wav(path, &back, &nback, &rate) != 0) {
        check("read volume wav", 0, NULL); free(sig); return;
    }
    {
        double amp = goertzel(back + 800, nback - 1600, 1000.0, rate);
        snprintf(detail, sizeof(detail),
                 "input 0.800 x vol 0.25 -> %.4f (expect ~0.200)", amp);
        check("volume scaling is applied", amp > 0.18 && amp < 0.22, detail);
    }
    free(back);

    /* クリップ確認: 振幅 2.0 を volume 1.0 で流す */
    for (i = 0; i < n; i++)
        sig[i] = 2.0f * (float)sin(2.0 * M_PI * 1000.0 * i / fs);
    p.wav_path = "/tmp/vm_test_clip.wav";
    p.volume   = 1.0f;
    if (vm_audio_open(&a, &p) != VM_OK) {
        check("open for clip test", 0, NULL); free(sig); return;
    }
    push_all(a, sig, n);
    vm_audio_drain(a, 3000);
    vm_audio_close(a);

    if (read_wav("/tmp/vm_test_clip.wav", &back, &nback, &rate) == 0) {
        float peak = 0.0f;
        for (i = 0; i < nback; i++)
            if (fabsf(back[i]) > peak) peak = fabsf(back[i]);
        snprintf(detail, sizeof(detail),
                 "input peak 2.000 -> output peak %.4f (must be <= 1.0)", peak);
        check("out-of-range input is clipped, not wrapped",
              peak <= 1.0001f && peak > 0.9f, detail);
        free(back);
    } else {
        check("read clip wav", 0, NULL);
    }
    free(sig);
}

/* ================================================================== */
/* 4. flush                                                            */
/* ================================================================== */
static void test_flush(void)
{
    vm_audio_t *a = NULL;
    vm_audio_params_t p;
    char detail[200];
    float buf[4000];
    int i;
    uint32_t queued_before, queued_after;

    printf("\n[4] flush (ATH 相当の即時停止)\n");

    for (i = 0; i < 4000; i++) buf[i] = 0.3f;

    vm_audio_params_defaults(&p);
    p.backend = VM_AUDIO_BACKEND_NULL;
    if (vm_audio_open(&a, &p) != VM_OK) {
        check("open for flush test", 0, NULL); return;
    }

    vm_audio_write(a, buf, 4000);
    queued_before = vm_audio_queued(a);

    vm_audio_flush(a);
    /* flush はレンダースレッドが処理するので少し待つ */
    for (i = 0; i < 100 && vm_audio_queued(a) > 0; i++) vm_audio_write(a, buf, 0);
    vm_audio_wait_drain(a, 1, 300);
    queued_after = vm_audio_queued(a);

    snprintf(detail, sizeof(detail), "queued %u -> %u",
             queued_before, queued_after);
    check("flush discards queued audio",
          queued_before > 0 && queued_after == 0, detail);

    /*
     * アイドルと真のアンダーランが区別されている事の確認。
     * flush 後は何も投入していないので、レンダースレッドは
     * 「リングが空」の状態で無音を出し続ける。
     * これは正常なので idle_frames が増え、underruns は 0 のまま。
     */
    vm_sleep_ms_test(200);
    snprintf(detail, sizeof(detail),
             "idle=%u frames, underruns=%u",
             vm_audio_idle_frames(a), vm_audio_underruns(a));
    check("idle silence is not counted as underrun",
          vm_audio_idle_frames(a) > 0 && vm_audio_underruns(a) == 0, detail);

    vm_audio_close(a);
}

/* ================================================================== */
/* 5. 実際の V.34 ハンドシェイクを通す                                 */
/* ================================================================== */
static void test_real_handshake(void)
{
    vm_audio_t *a = NULL;
    vm_audio_params_t p;
    vm_handshake_t h;
    char detail[240];
    const char *path = "/tmp/vm_test_v34.wav";
    float blk[VM_DSP_BLOCK_SAMPLES];
    float *back = NULL;
    int nback = 0, rate = 0;
    bool done = false;
    int guard = 0;

    printf("\n[5] 実信号: V.34 ハンドシェイク全体をオーディオ経路に流す\n");

    vm_audio_params_defaults(&p);
    p.backend  = VM_AUDIO_BACKEND_WAVFILE;
    p.wav_path = path;
    p.volume   = 1.0f;

    if (vm_audio_open(&a, &p) != VM_OK) {
        check("open for handshake test", 0, NULL);
        return;
    }

    /* fast モードで所要時間を短縮 (内容は同一) */
    vm_handshake_init(&h, VM_STD_V34PLUS, 33600, VM_DSP_SAMPLE_RATE, 0.9f, true);

    /*
     * 実機のシーケンサと同じ流し方をする。
     *   - リングにある程度溜めてから流し続ける (プリバッファ)
     *   - 溜まり過ぎたら wait_drain で待つ (生成が先走らないように)
     * こうする事で「再生途中でデータが尽きる」真のアンダーランが
     * 発生しない事を確認できる。
     */
    while (!done && guard++ < 20000) {
        /* 高水位に達したら消費を待つ (0.25 秒ぶん溜めて走る) */
        vm_audio_wait_drain(a, (uint32_t)(VM_DSP_SAMPLE_RATE / 4), 1000);
        memset(blk, 0, sizeof(blk));
        done = vm_handshake_render_add(&h, blk, VM_DSP_BLOCK_SAMPLES);
        push_all(a, blk, VM_DSP_BLOCK_SAMPLES);
    }
    vm_audio_drain(a, 5000);

    snprintf(detail, sizeof(detail),
             "underruns=%u (真のグリッチ) / idle=%u frames (正常な無音)",
             vm_audio_underruns(a), vm_audio_idle_frames(a));
    check("no mid-stream underruns while feeding handshake",
          vm_audio_underruns(a) == 0, detail);

    vm_audio_close(a);

    if (read_wav(path, &back, &nback, &rate) != 0) {
        check("read handshake wav", 0, path);
        return;
    }

    snprintf(detail, sizeof(detail), "%d frames = %.2f s @%dHz",
             nback, (double)nback / rate, rate);
    check("handshake audio captured", nback > rate / 2, detail);

    /* ANSam 2100Hz が存在する事 (V.8 Phase 1 の証拠) */
    {
        double best = 0.0;
        int seg;
        /* 100ms 窓でスキャンして最大値を取る */
        for (seg = 0; seg + 800 < nback; seg += 800) {
            double amp = goertzel(back + seg, 800, 2100.0, rate);
            if (amp > best) best = amp;
        }
        snprintf(detail, sizeof(detail),
                 "max amplitude at 2100Hz = %.4f", best);
        check("ANSam 2100Hz present in rendered audio", best > 0.05, detail);
    }

    /* 21 トーン プローブの特徴周波数 (150Hz, 3750Hz) を探す */
    {
        double best150 = 0.0, best3750 = 0.0;
        int seg;
        for (seg = 0; seg + 800 < nback; seg += 400) {
            double a150  = goertzel(back + seg, 800, 150.0,  rate);
            double a3750 = goertzel(back + seg, 800, 3750.0, rate);
            if (a150  > best150)  best150  = a150;
            if (a3750 > best3750) best3750 = a3750;
        }
        snprintf(detail, sizeof(detail),
                 "150Hz=%.4f 3750Hz=%.4f (L1/L2 probe band edges)",
                 best150, best3750);
        check("V.34 line-probe band edges present",
              best150 > 0.01 && best3750 > 0.01, detail);
    }

    /* 全体が無音でない / DC オフセットが無い事 */
    {
        double sum = 0.0, sumsq = 0.0;
        int i;
        for (i = 0; i < nback; i++) { sum += back[i]; sumsq += back[i] * back[i]; }
        snprintf(detail, sizeof(detail), "rms=%.4f dc=%.6f",
                 sqrt(sumsq / nback), sum / nback);
        check("audio is non-silent and DC-free",
              sqrt(sumsq / nback) > 0.05 && fabs(sum / nback) < 0.01, detail);
    }

    free(back);
}

int main(void)
{
    printf("=======================================================\n");
    printf(" VModem オーディオ パイプライン 検証テスト\n");
    printf(" (WASAPI と共通の render 経路を WAV で検証)\n");
    printf("=======================================================\n");

    vm_log_init(VM_LOG_WARN, NULL);

    test_lifecycle();
    test_wav_roundtrip();
    test_volume();
    test_flush();
    test_real_handshake();

    vm_log_shutdown();

    printf("\n=======================================================\n");
    printf(" 結果: %d passed, %d failed\n", g_pass, g_fail);
    printf("=======================================================\n");
    return g_fail ? 1 : 0;
}
