/*
 * vm_audio_internal.h - オーディオ バックエンド共通の内部定義
 *
 * 共通部 (リングバッファ、リサンプラ、スレッド生成) を vm_audio.c に置き、
 * バックエンド固有部 (WASAPI / WAV / null) は run 関数だけを提供する。
 */
#ifndef VMODEM_VM_AUDIO_INTERNAL_H
#define VMODEM_VM_AUDIO_INTERNAL_H

#include "vmodem/vm_audio.h"
#include "vmodem/vm_resample.h"
#include "vmodem/vm_ringbuf.h"
#include "vmodem/vm_log.h"

#ifdef _WIN32
#  include <windows.h>
typedef HANDLE vm_thread_t;
typedef HANDLE vm_event_t;
#else
#  include <pthread.h>
typedef pthread_t vm_thread_t;
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             signaled;
    int             manual_reset;
} vm_event_impl_t;
typedef vm_event_impl_t *vm_event_t;
#endif

#define VM_AUDIO_NAME_MAX 128

struct vm_audio_s {
    vm_audio_params_t   params;
    vm_audio_backend_t  backend;

    /*
     * DSP -> レンダースレッド の受け渡し。
     * 生産者 = モデム制御スレッド、消費者 = レンダースレッド の SPSC。
     */
    vm_frb_t            ring;

    /*
     * リサンプラはレンダースレッド専用 (共有しない)。
     * デバイスレートが判明してから初期化するため、
     * WASAPI バックエンドではスレッド内で init する。
     */
    vm_resampler_t      rs;
    bool                rs_ready;

    int                 device_rate;
    int                 device_channels;
    char                device_name[VM_AUDIO_NAME_MAX];

    /* スレッド制御 */
    vm_thread_t         thread;
    bool                thread_started;
    volatile int        quit;          /* 1 で停止要求 */
    volatile int        flush_req;     /* 1 でリング破棄要求 */

    /* 初期化完了通知 (成否) */
    vm_event_t          ready_ev;
    volatile int        ready_result;  /* vm_err_t を int で保持 */

    /* 「リングに空きが出た」通知 (生成側の待機用) */
    vm_event_t          space_ev;

    /* WAV バックエンド用 */
    void               *wav_fp;
    uint32_t            wav_frames;

    /* WASAPI 用の不透明ポインタ (実装ファイル内でのみ解釈) */
    void               *impl;
};

/* --------------------------------------------------------------------- */
/* プラットフォーム薄ラッパ                                              */
/* --------------------------------------------------------------------- */
vm_err_t vm_ev_create(vm_event_t *ev, bool manual_reset);
void     vm_ev_destroy(vm_event_t ev);
void     vm_ev_set(vm_event_t ev);
void     vm_ev_reset(vm_event_t ev);
/* timeout_ms < 0 で無限待ち。true = シグナルされた */
bool     vm_ev_wait(vm_event_t ev, int timeout_ms);

void     vm_sleep_ms(int ms);

/* --------------------------------------------------------------------- */
/* バックエンド実装が提供する関数                                        */
/* --------------------------------------------------------------------- */

/*
 * レンダー ループ本体。
 * 各実装は
 *   1. 自前の初期化 (COM / デバイス / フォーマット決定)
 *   2. a->device_rate / device_channels / device_name を埋める
 *   3. vm_audio_backend_resampler_init(a) を呼ぶ
 *   4. a->ready_result に VM_OK を入れて vm_ev_set(a->ready_ev)
 *   5. a->quit が立つまでレンダー
 *   6. 後片付け
 * を行う。失敗時は a->ready_result にエラーを入れて ready_ev をセットし復帰。
 */
void vm_audio_run_wasapi(vm_audio_t *a);
void vm_audio_run_wavfile(vm_audio_t *a);
void vm_audio_run_null(vm_audio_t *a);

/* デバイスレート確定後に共通リサンプラを初期化する */
vm_err_t vm_audio_backend_resampler_init(vm_audio_t *a);

/*
 * リングから DSP サンプルを取り出し、デバイスレートに変換して
 * out (interleaved, device_channels ch) に n_frames 分書き込む。
 * 足りない分は無音。レンダースレッドから呼ぶ事を前提に
 * malloc / ロック / IO を行わない。
 */
void vm_audio_backend_render(vm_audio_t *a, float *out, uint32_t n_frames);

/* flush 要求の処理 (レンダースレッド内で呼ぶ) */
void vm_audio_backend_handle_flush(vm_audio_t *a);

#endif /* VMODEM_VM_AUDIO_INTERNAL_H */
