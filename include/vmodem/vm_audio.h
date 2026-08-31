/*
 * vm_audio.h - オーディオ出力バックエンド (WASAPI / WAV / null)
 *
 * =========================================================================
 * 実装の難所 (1): WASAPI 統合
 * =========================================================================
 *
 * ■ なぜ WASAPI か
 * ---------------------------------------------------------------------
 * Windows のオーディオ API には歴史的に waveOut / DirectSound / WASAPI が
 * あるが、Windows Vista 以降 waveOut / DirectSound は WASAPI 上の
 * エミュレーション層に過ぎない。エミュレーション層は
 *   - 余分なバッファリング (遅延 50-100ms)
 *   - 勝手なサンプルレート変換 (品質不定)
 * を挟むため、「プロトコルのタイミングに同期した音」を出す本用途には
 * 不適である。V.34 の 40ms 位相反転や V.8 の 200ms 反転周期を
 * 正しく聞かせるには、生成した PCM がそのままデバイスに届く必要がある。
 * 従って WASAPI を直接叩く。
 *
 * ■ 共有モード (shared) か排他モード (exclusive) か
 * ---------------------------------------------------------------------
 * 排他モードはデバイスを占有でき、8000Hz をそのまま渡せる可能性があり
 * 遅延も最小になる。しかし
 *   - 他アプリの音が全部止まる (ユーザ体験として許容し難い)
 *   - デバイスが 8000Hz を拒否する事が多く、失敗時の分岐が増える
 * 本エミュレータは「懐かしい音を聞かせる」用途であり 10-30ms の遅延は
 * 全く問題にならないので、共有モードを採用する。
 * 共有モードではデバイスのミックス フォーマット
 * (GetMixFormat が返す。通常 44100/48000Hz の 32bit float) に
 * 合わせる事が必須で、ここでリサンプラ (vm_resample) が必要になる。
 *
 * ■ イベント駆動 vs ポーリング
 * ---------------------------------------------------------------------
 * AUDCLNT_STREAMFLAGS_EVENTCALLBACK を付け、SetEventHandle() で
 * イベントを登録すると、デバイスが「バッファが空いた」タイミングで
 * イベントをシグナルしてくれる。専用スレッドが
 *     WaitForSingleObject(event) -> GetBuffer -> 書く -> ReleaseBuffer
 * を回すだけになり、Sleep() ポーリングに比べて
 *   - CPU 消費が下がる
 *   - タイミング精度が上がる (デバイスのクロックに追従)
 * ため、こちらを採用する。
 *
 * ■ イベント駆動での落とし穴
 * ---------------------------------------------------------------------
 * 共有モードでは GetCurrentPadding で「まだ再生されていない残量」を見て
 * 空き分だけ書く。空きが 0 の時に GetBuffer(0) を呼ぶと
 * AUDCLNT_E_BUFFER_TOO_LARGE 等のエラーになる実装があるため、
 * 空きが 0 なら何も書かず次のイベントを待つ事。
 * また Start() の前に 1 回バッファを埋めておかないと、
 * 開始直後に必ずグリッチが出る (プリロール)。
 *
 * ■ COM の初期化とスレッド モデル
 * ---------------------------------------------------------------------
 * WASAPI は COM ベースなので CoInitializeEx が必要。
 * MTA (COINIT_MULTITHREADED) で初期化する。重要なのは
 * 「CoInitializeEx を呼んだスレッドで CoUninitialize する」事、
 * および「COM オブジェクトを作ったスレッドと使うスレッドを揃える」事。
 * 本実装ではレンダースレッド内で COM を初期化し、そこで
 * デバイス列挙からストリーム開始・停止・解放まで全部行う。こうすると
 * アパートメント跨ぎのマーシャリングが一切発生しない。
 * (初期化の成否は専用イベントで呼び出し元に通知する)
 *
 * ■ グリッチ (音切れ) を出さないための鉄則
 * ---------------------------------------------------------------------
 * レンダースレッドのループ内では以下を【一切やらない】。
 *   - malloc / free      (ヒープロックで数 ms 止まる事がある)
 *   - ミューテックス取得 (プライオリティ インバージョン)
 *   - ファイル IO / printf
 * PCM は vm_frb (ロックフリー SPSC リングバッファ) 経由で受け取り、
 * 足りない時は無音を埋める (vm_frb_read_or_silence)。
 * さらに AvSetMmThreadCharacteristics("Audio") でスレッドを
 * MMCSS (Multimedia Class Scheduler Service) に登録し、
 * OS に「これはオーディオスレッドだ」と伝えて優先度を確保する。
 * (avrt.dll を動的ロードするので、無い環境でも動く)
 *
 * ■ 移植性
 * ---------------------------------------------------------------------
 * 本ヘッダは抽象インタフェースだけを公開する。
 *   - vm_audio_wasapi.c : Windows 実装 (_WIN32 のみ)
 *   - vm_audio_null.c   : WAV ファイル書き出し / 無出力 (全プラットフォーム)
 * これにより Linux サンドボックス上でも DSP とシーケンサを
 * そのままテストできる (本プロジェクトは実際にそうして検証している)。
 */
#ifndef VMODEM_VM_AUDIO_H
#define VMODEM_VM_AUDIO_H

#include "vmodem/vm_types.h"
#include "vmodem/vm_ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VM_AUDIO_BACKEND_AUTO = 0,   /* Windows なら WASAPI、他は null */
    VM_AUDIO_BACKEND_WASAPI,
    VM_AUDIO_BACKEND_WAVFILE,    /* WAV に書くだけ (デバッグ / CI 用) */
    VM_AUDIO_BACKEND_NULL        /* 破棄するだけ (音を出さない設定) */
} vm_audio_backend_t;

typedef struct vm_audio_s vm_audio_t;

typedef struct {
    vm_audio_backend_t backend;

    /* DSP 側のサンプルレート (通常 VM_DSP_SAMPLE_RATE = 8000) */
    int   dsp_rate;

    /* 音量 0.0 - 1.0 */
    float volume;

    /*
     * 使用デバイス名の部分一致文字列。NULL または "" なら既定デバイス。
     * 例: "Speakers" / "ヘッドホン"
     */
    const char *device_match;

    /* WAVFILE バックエンド時の出力先 */
    const char *wav_path;

    /*
     * 内部リングバッファの長さ (DSP サンプル単位)。
     * 大きいほどグリッチに強いが、停止要求への追従が遅れる。
     * 0 なら既定値 (dsp_rate / 2 = 0.5 秒) を使う。
     */
    int   ring_samples;
} vm_audio_params_t;

void vm_audio_params_defaults(vm_audio_params_t *p);

/*
 * 生成 / 破棄。
 * open に成功するとレンダースレッドが動き出し、
 * リングバッファが空の間は無音を出し続ける。
 */
vm_err_t vm_audio_open(vm_audio_t **out, const vm_audio_params_t *params);
void     vm_audio_close(vm_audio_t *a);

/*
 * DSP サンプル (dsp_rate, モノラル, -1.0..1.0) を投入する。
 * 戻り値 = 実際に受け付けたサンプル数。
 * リングが満杯なら受け付けられる分だけ書く (呼び出し側でリトライする)。
 * 【ブロックしない】
 */
uint32_t vm_audio_write(vm_audio_t *a, const float *samples, uint32_t n);

/* リングの空き / 溜まり (DSP サンプル単位) */
uint32_t vm_audio_space(const vm_audio_t *a);
uint32_t vm_audio_queued(const vm_audio_t *a);

/*
 * リングが溜まっている間、生成側を待たせるためのヘルパ。
 * 「溜まりが high_water 未満になるまで」最大 timeout_ms 待つ。
 * オーディオスレッドではなく生成側 (モデム制御スレッド) から呼ぶ。
 */
void vm_audio_wait_drain(vm_audio_t *a, uint32_t high_water, int timeout_ms);

/* 溜まっている音を全部鳴らし切るまで待つ (CONNECT 前の無音確定等に使う) */
void vm_audio_drain(vm_audio_t *a, int timeout_ms);

/* キューを捨てる (ATH による即時切断など) */
void vm_audio_flush(vm_audio_t *a);

/* 実際に採用されたデバイス レート / バックエンド名 (ログ表示用) */
int         vm_audio_device_rate(const vm_audio_t *a);
const char *vm_audio_backend_name(const vm_audio_t *a);
const char *vm_audio_device_name(const vm_audio_t *a);

/*
 * 真のアンダーラン累計 (再生途中でデータが尽きた = 耳に聞こえるグリッチ)。
 * 正常動作なら 0 のままでなければならない。
 */
uint32_t vm_audio_underruns(const vm_audio_t *a);

/*
 * アイドル (リングが空で無音を出した) フレーム数。
 * オンフック中や無音ステージでは当然増えるので、これは異常ではない。
 * underruns と混同しない事。
 */
uint32_t vm_audio_idle_frames(const vm_audio_t *a);

#ifdef __cplusplus
}
#endif

#endif /* VMODEM_VM_AUDIO_H */
