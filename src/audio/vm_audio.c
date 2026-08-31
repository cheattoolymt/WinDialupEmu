/*
 * vm_audio.c - オーディオ バックエンド共通部
 *
 * ここには「どのバックエンドでも同じ」処理だけを置く。
 *   - リングバッファ管理
 *   - リサンプル + 音量適用 + チャネル複製
 *   - スレッド / イベントのプラットフォーム抽象
 * WASAPI 固有のコードは vm_audio_wasapi.c にある。
 */
/*
 * -std=c99 厳密モードでは clock_gettime / nanosleep / pthread の一部が
 * 隠れるため、POSIX 機能を明示的に要求する
 * (Windows ビルドでは _WIN32 分岐により未使用)。
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "vm_audio_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ===================================================================== */
/* プラットフォーム薄ラッパ                                              */
/* ===================================================================== */

#ifdef _WIN32

vm_err_t vm_ev_create(vm_event_t *ev, bool manual_reset)
{
    if (!ev) return VM_ERR_INVAL;
    *ev = CreateEventW(NULL, manual_reset ? TRUE : FALSE, FALSE, NULL);
    return *ev ? VM_OK : VM_ERR_IO;
}
void vm_ev_destroy(vm_event_t ev) { if (ev) CloseHandle(ev); }
void vm_ev_set(vm_event_t ev)     { if (ev) SetEvent(ev); }
void vm_ev_reset(vm_event_t ev)   { if (ev) ResetEvent(ev); }

bool vm_ev_wait(vm_event_t ev, int timeout_ms)
{
    DWORD r;
    if (!ev) return false;
    r = WaitForSingleObject(ev, (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms);
    return (r == WAIT_OBJECT_0);
}

void vm_sleep_ms(int ms) { Sleep((DWORD)(ms < 0 ? 0 : ms)); }

static unsigned __stdcall thread_entry(void *arg);   /* 前方宣言 */

#else  /* POSIX */

#include <errno.h>
#include <time.h>

vm_err_t vm_ev_create(vm_event_t *ev, bool manual_reset)
{
    vm_event_impl_t *e;
    if (!ev) return VM_ERR_INVAL;
    e = (vm_event_impl_t *)calloc(1, sizeof(*e));
    if (!e) return VM_ERR_NOMEM;
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->manual_reset = manual_reset ? 1 : 0;
    e->signaled = 0;
    *ev = e;
    return VM_OK;
}

void vm_ev_destroy(vm_event_t ev)
{
    if (!ev) return;
    pthread_cond_destroy(&ev->c);
    pthread_mutex_destroy(&ev->m);
    free(ev);
}

void vm_ev_set(vm_event_t ev)
{
    if (!ev) return;
    pthread_mutex_lock(&ev->m);
    ev->signaled = 1;
    /* manual reset は全員起こす、auto reset は 1 人だけ */
    if (ev->manual_reset) pthread_cond_broadcast(&ev->c);
    else                  pthread_cond_signal(&ev->c);
    pthread_mutex_unlock(&ev->m);
}

void vm_ev_reset(vm_event_t ev)
{
    if (!ev) return;
    pthread_mutex_lock(&ev->m);
    ev->signaled = 0;
    pthread_mutex_unlock(&ev->m);
}

bool vm_ev_wait(vm_event_t ev, int timeout_ms)
{
    bool got = false;
    if (!ev) return false;

    pthread_mutex_lock(&ev->m);
    if (timeout_ms < 0) {
        while (!ev->signaled)
            pthread_cond_wait(&ev->c, &ev->m);
        got = true;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!ev->signaled) {
            if (pthread_cond_timedwait(&ev->c, &ev->m, &ts) == ETIMEDOUT) break;
        }
        got = ev->signaled ? true : false;
    }
    if (got && !ev->manual_reset) ev->signaled = 0;
    pthread_mutex_unlock(&ev->m);
    return got;
}

void vm_sleep_ms(int ms)
{
    struct timespec ts;
    if (ms < 0) ms = 0;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void *thread_entry(void *arg);   /* 前方宣言 */

#endif

/* ===================================================================== */
/* レンダースレッド エントリ                                             */
/* ===================================================================== */

static void dispatch_backend(vm_audio_t *a)
{
    switch (a->backend) {
#ifdef _WIN32
    case VM_AUDIO_BACKEND_WASAPI:  vm_audio_run_wasapi(a);  break;
#endif
    case VM_AUDIO_BACKEND_WAVFILE: vm_audio_run_wavfile(a); break;
    case VM_AUDIO_BACKEND_NULL:    vm_audio_run_null(a);    break;
    default:
        a->ready_result = (int)VM_ERR_UNSUPPORTED;
        vm_ev_set(a->ready_ev);
        break;
    }
}

#ifdef _WIN32
static unsigned __stdcall thread_entry(void *arg)
{
    dispatch_backend((vm_audio_t *)arg);
    return 0;
}
#else
static void *thread_entry(void *arg)
{
    dispatch_backend((vm_audio_t *)arg);
    return NULL;
}
#endif

/* ===================================================================== */
/* 公開 API                                                              */
/* ===================================================================== */

void vm_audio_params_defaults(vm_audio_params_t *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->backend      = VM_AUDIO_BACKEND_AUTO;
    p->dsp_rate     = VM_DSP_SAMPLE_RATE;
    p->volume       = 0.7f;
    p->device_match = NULL;
    p->wav_path     = NULL;
    p->ring_samples = 0;
}

vm_err_t vm_audio_open(vm_audio_t **out, const vm_audio_params_t *params)
{
    vm_audio_t *a;
    vm_audio_params_t p;
    int ring_n;
    vm_err_t rc;

    if (!out) return VM_ERR_INVAL;
    *out = NULL;

    if (params) p = *params;
    else        vm_audio_params_defaults(&p);

    if (p.dsp_rate <= 0) p.dsp_rate = VM_DSP_SAMPLE_RATE;
    if (p.volume < 0.0f) p.volume = 0.0f;
    if (p.volume > 1.0f) p.volume = 1.0f;

    /* AUTO の解決 */
    if (p.backend == VM_AUDIO_BACKEND_AUTO) {
#ifdef _WIN32
        p.backend = VM_AUDIO_BACKEND_WASAPI;
#else
        p.backend = p.wav_path && p.wav_path[0]
                  ? VM_AUDIO_BACKEND_WAVFILE : VM_AUDIO_BACKEND_NULL;
#endif
    }

    a = (vm_audio_t *)calloc(1, sizeof(*a));
    if (!a) return VM_ERR_NOMEM;

    a->params  = p;
    a->backend = p.backend;
    a->device_rate     = p.dsp_rate;   /* バックエンドが上書きする */
    a->device_channels = 1;
    snprintf(a->device_name, sizeof(a->device_name), "%s", "(unknown)");

    /* 既定 0.5 秒。20ms ブロック生成に対して十分な余裕 */
    ring_n = (p.ring_samples > 0) ? p.ring_samples : (p.dsp_rate / 2);

    rc = vm_frb_init(&a->ring, (uint32_t)ring_n);
    if (rc != VM_OK) { free(a); return rc; }

    if (vm_ev_create(&a->ready_ev, false) != VM_OK ||
        vm_ev_create(&a->space_ev, false) != VM_OK) {
        vm_ev_destroy(a->ready_ev);
        vm_ev_destroy(a->space_ev);
        vm_frb_free(&a->ring);
        free(a);
        return VM_ERR_IO;
    }

    a->ready_result = (int)VM_ERR_TIMEOUT;

    /* レンダースレッド起動 */
#ifdef _WIN32
    {
        uintptr_t h;
        /*
         * _beginthreadex を使うと CRT のスレッド局所状態が正しく
         * 初期化される (CreateThread 直呼びは避ける)。
         */
        extern uintptr_t __cdecl _beginthreadex(void *, unsigned,
                unsigned (__stdcall *)(void *), void *, unsigned, unsigned *);
        h = _beginthreadex(NULL, 0, thread_entry, a, 0, NULL);
        if (!h) {
            vm_ev_destroy(a->ready_ev);
            vm_ev_destroy(a->space_ev);
            vm_frb_free(&a->ring);
            free(a);
            return VM_ERR_IO;
        }
        a->thread = (HANDLE)h;
    }
#else
    if (pthread_create(&a->thread, NULL, thread_entry, a) != 0) {
        vm_ev_destroy(a->ready_ev);
        vm_ev_destroy(a->space_ev);
        vm_frb_free(&a->ring);
        free(a);
        return VM_ERR_IO;
    }
#endif
    a->thread_started = true;

    /* 初期化完了を待つ (デバイス オープンに失敗したら即座に判る) */
    if (!vm_ev_wait(a->ready_ev, 5000)) {
        VM_LOGE("オーディオ バックエンドの初期化がタイムアウトしました");
        vm_audio_close(a);
        return VM_ERR_TIMEOUT;
    }

    if ((vm_err_t)a->ready_result != VM_OK) {
        vm_err_t err = (vm_err_t)a->ready_result;
        VM_LOGE("オーディオ バックエンドの初期化に失敗: %s",
                vm_strerror(err));
        vm_audio_close(a);
        return err;
    }

    VM_LOGI("オーディオ出力: %s / %s / %dHz %dch (DSP %dHz)",
            vm_audio_backend_name(a), a->device_name,
            a->device_rate, a->device_channels, p.dsp_rate);

    *out = a;
    return VM_OK;
}

void vm_audio_close(vm_audio_t *a)
{
    if (!a) return;

    a->quit = 1;
    /* レンダースレッドが space_ev / ring 待ちで寝ていても起こす */
    vm_ev_set(a->space_ev);

    if (a->thread_started) {
#ifdef _WIN32
        WaitForSingleObject(a->thread, 3000);
        CloseHandle(a->thread);
#else
        pthread_join(a->thread, NULL);
#endif
        a->thread_started = false;
    }

    vm_ev_destroy(a->ready_ev);
    vm_ev_destroy(a->space_ev);
    vm_resampler_free(&a->rs);
    vm_frb_free(&a->ring);
    free(a);
}

uint32_t vm_audio_write(vm_audio_t *a, const float *samples, uint32_t n)
{
    if (!a || !samples || n == 0) return 0;
    return vm_frb_write(&a->ring, samples, n);
}

uint32_t vm_audio_space(const vm_audio_t *a)
{
    if (!a) return 0;
    return vm_frb_space(&a->ring);
}

uint32_t vm_audio_queued(const vm_audio_t *a)
{
    if (!a) return 0;
    return vm_frb_used(&a->ring);
}

void vm_audio_wait_drain(vm_audio_t *a, uint32_t high_water, int timeout_ms)
{
    int waited = 0;
    if (!a) return;

    while (vm_frb_used(&a->ring) >= high_water) {
        if (a->quit) return;
        if (timeout_ms >= 0 && waited >= timeout_ms) return;
        /*
         * space_ev はレンダースレッドが消費した時にセットされる。
         * 取り逃しても 5ms で起きるので、必ず短いタイムアウトを付ける。
         */
        vm_ev_wait(a->space_ev, 5);
        waited += 5;
    }
}

void vm_audio_drain(vm_audio_t *a, int timeout_ms)
{
    int waited = 0;
    if (!a) return;

    /* リングを空にする */
    while (vm_frb_used(&a->ring) > 0) {
        if (a->quit) return;
        if (timeout_ms >= 0 && waited >= timeout_ms) return;
        vm_ev_wait(a->space_ev, 5);
        waited += 5;
    }

    /*
     * リングが空になっても、デバイス側のバッファにまだ音が残っている。
     * 共有モードの典型的なバッファ長 (10-30ms) 分だけ余分に待つ。
     */
    vm_sleep_ms(40);
}

void vm_audio_flush(vm_audio_t *a)
{
    if (!a) return;
    /*
     * リング破棄はレンダースレッドに依頼する。
     * 生成側が直接 tail を動かすと SPSC の前提 (tail は消費者のみ) が
     * 壊れるため、フラグ経由にするのが重要。
     */
    a->flush_req = 1;
}

int vm_audio_device_rate(const vm_audio_t *a)
{
    return a ? a->device_rate : 0;
}

const char *vm_audio_backend_name(const vm_audio_t *a)
{
    if (!a) return "(none)";
    switch (a->backend) {
    case VM_AUDIO_BACKEND_WASAPI:  return "WASAPI";
    case VM_AUDIO_BACKEND_WAVFILE: return "WAV file";
    case VM_AUDIO_BACKEND_NULL:    return "null";
    default:                       return "(unknown)";
    }
}

const char *vm_audio_device_name(const vm_audio_t *a)
{
    return a ? a->device_name : "(none)";
}

uint32_t vm_audio_underruns(const vm_audio_t *a)
{
    if (!a) return 0;
    return a->ring.underruns;
}

uint32_t vm_audio_idle_frames(const vm_audio_t *a)
{
    if (!a) return 0;
    return a->ring.idle_frames;
}

/* ===================================================================== */
/* バックエンド共通ヘルパ (レンダースレッドから呼ばれる)                 */
/* ===================================================================== */

vm_err_t vm_audio_backend_resampler_init(vm_audio_t *a)
{
    vm_err_t rc;
    if (!a) return VM_ERR_INVAL;

    rc = vm_resampler_init(&a->rs, a->params.dsp_rate, a->device_rate);
    if (rc != VM_OK) return rc;
    a->rs_ready = true;
    return VM_OK;
}

void vm_audio_backend_handle_flush(vm_audio_t *a)
{
    if (!a || !a->flush_req) return;
    a->flush_req = 0;
    /* 消費者スレッドなので tail を動かして良い */
    vm_frb_reset(&a->ring);
    if (a->rs_ready) vm_resampler_reset(&a->rs);
}

/*
 * リング -> リサンプル -> 音量 -> チャネル複製 の一気通貫。
 *
 * レンダースレッドから呼ばれるので malloc / ロック / IO は禁止。
 * 中間バッファは静的サイズのスタック配列で済ませる。
 */
void vm_audio_backend_render(vm_audio_t *a, float *out, uint32_t n_frames)
{
    /*
     * 1 回の呼び出しで扱う最大デバイス フレーム数。
     * 共有モードのバッファは通常 10-30ms (48kHz で 480-1440 フレーム)。
     * 余裕を見て 4096 とする。これを超える要求は分割して処理する。
     */
    enum { MAX_OUT = 4096 };
    /* dsp_rate <= device_rate が前提だが、逆でも安全に足りる大きさ */
    enum { MAX_IN  = 4096 };

    float dsp_buf[MAX_IN];
    float dev_buf[MAX_OUT];
    uint32_t done = 0;
    int ch, i, c;

    if (!a || !out || n_frames == 0) return;

    ch = a->device_channels > 0 ? a->device_channels : 1;

    if (!a->rs_ready) {
        memset(out, 0, (size_t)n_frames * (size_t)ch * sizeof(float));
        return;
    }

    while (done < n_frames) {
        uint32_t want = n_frames - done;
        int need_in, got_in, produced, consumed;

        if (want > MAX_OUT) want = MAX_OUT;

        /*
         * want フレーム出すのに必要な DSP サンプル数。
         *
         * ここで安全マージンを足してはいけない。
         * 生産側が 20ms ブロック単位でちょうど供給している定常状態では、
         * 余分に要求すると【毎回必ず】部分読み出しになり、
         * 真のアンダーランとして誤計上される (実測で 2 秒間に 38 件)。
         * 必要数だけ要求し、出力が足りなければループが次の反復で
         * 追加の入力を要求する。リサンプラは内部に履歴を持つので
         * 分割供給でも波形は連続する。
         */
        need_in = (int)((uint64_t)want * (uint64_t)a->params.dsp_rate
                        / (uint64_t)a->device_rate);
        if (need_in < 1) need_in = 1;
        if (need_in > MAX_IN) need_in = MAX_IN;

        /*
         * リングが完全に空なら「アイドル」であり、部分的にしか無い場合が
         * 「真のアンダーラン」。この判定は vm_frb_read_or_silence が
         * idle_frames / underruns に分けて計上する。
         */
        got_in = (int)vm_frb_read_or_silence(&a->ring, dsp_buf,
                                            (uint32_t)need_in);
        (void)got_in;

        produced = vm_resampler_process(&a->rs, dsp_buf, need_in,
                                        dev_buf, (int)want, &consumed);

        /* 音量適用 + チャネル複製 (モノラル -> 全ch 同一) */
        {
            float vol = a->params.volume;
            float *dst = out + (size_t)done * (size_t)ch;
            for (i = 0; i < produced; i++) {
                float s = dev_buf[i] * vol;
                /* クリップ (デバイスに範囲外を渡さない) */
                if (s >  1.0f) s =  1.0f;
                if (s < -1.0f) s = -1.0f;
                for (c = 0; c < ch; c++) dst[(size_t)i * (size_t)ch + c] = s;
            }
        }

        done += (uint32_t)produced;

        /* 進捗しなかった場合は残りを無音で埋めて抜ける (無限ループ防止) */
        if (produced == 0) {
            memset(out + (size_t)done * (size_t)ch, 0,
                   (size_t)(n_frames - done) * (size_t)ch * sizeof(float));
            done = n_frames;
            break;
        }
    }

    /* 生成側が空き待ちで寝ている場合に起こす */
    vm_ev_set(a->space_ev);
}
