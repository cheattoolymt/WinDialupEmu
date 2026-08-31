/*
 * vm_audio_null.c - WAV ファイル出力 / 無出力 バックエンド
 *
 * 目的:
 *   1. 音を出さない設定 (audio_enable=0) の受け皿
 *   2. 生成した PCM を WAV に落として解析するデバッグ手段
 *   3. Windows 以外 (本開発サンドボックスの Linux 等) でも
 *      DSP とシーケンサを実機同等の経路で検証できるようにする
 *
 * WASAPI バックエンドと同じ vm_audio_backend_render() を通すので、
 * リングバッファ / リサンプラ / 音量 / クリップの経路は完全に共通。
 * つまり WAV に出た波形は、Windows で耳に届く波形と同一である。
 *
 * タイミング:
 *   実デバイスが無いので、時間経過は自前で作る。
 *   WASAPI の 30ms バッファに相当する 20ms 刻みで「消費」し、
 *   実時間で sleep する事で、生成側 (モデム制御スレッド) から見た
 *   振る舞いを実機に近づける。
 *   WAV バックエンドでは sleep を省略して全力で書き出す事も可能だが、
 *   シーケンサのタイミング検証をしたいので実時間に合わせる。
 */
#include "vm_audio_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* WAV ヘッダ                                                            */
/* --------------------------------------------------------------------- */
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/*
 * 16bit PCM の WAV ヘッダを書く。
 * データ長は後で fseek して埋め直す (ストリーミング書き出しのため)。
 */
static void write_wav_header(FILE *fp, int rate, int channels,
                             uint32_t data_bytes)
{
    uint8_t h[44];
    int bits = 16;
    int block = channels * bits / 8;

    memcpy(h + 0, "RIFF", 4);
    put_u32le(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);

    memcpy(h + 12, "fmt ", 4);
    put_u32le(h + 16, 16);
    put_u16le(h + 20, 1);                       /* PCM */
    put_u16le(h + 22, (uint16_t)channels);
    put_u32le(h + 24, (uint32_t)rate);
    put_u32le(h + 28, (uint32_t)(rate * block));
    put_u16le(h + 32, (uint16_t)block);
    put_u16le(h + 34, (uint16_t)bits);

    memcpy(h + 36, "data", 4);
    put_u32le(h + 40, data_bytes);

    fwrite(h, 1, sizeof(h), fp);
}

/* --------------------------------------------------------------------- */
/* 共通ループ                                                            */
/*                                                                       */
/* write_wav が false なら PCM を捨てるだけ (null バックエンド)。        */
/* --------------------------------------------------------------------- */
static void run_common(vm_audio_t *a, bool write_wav)
{
    enum { BLOCK_MS = 20 };
    FILE *fp = NULL;
    float  *mix = NULL;
    int16_t *pcm = NULL;
    uint32_t frames_per_block;
    uint32_t total_frames = 0;
    vm_err_t rc;

    /*
     * デバイスレートの決定。
     * null/WAV では変換の必要が無いので DSP レートをそのまま使う。
     * こうするとリサンプラはパススルー相当になり、
     * WAV に「DSP が生成したそのままの波形」が残るので解析しやすい。
     */
    a->device_rate     = a->params.dsp_rate;
    a->device_channels = 1;
    snprintf(a->device_name, sizeof(a->device_name), "%s",
             write_wav ? "WAV file" : "null device");

    rc = vm_audio_backend_resampler_init(a);
    if (rc != VM_OK) {
        a->ready_result = (int)rc;
        vm_ev_set(a->ready_ev);
        return;
    }

    frames_per_block = (uint32_t)(a->device_rate * BLOCK_MS / 1000);
    if (frames_per_block == 0) frames_per_block = 160;

    /*
     * バッファはループ開始前に確保する (ループ内では malloc しない)。
     * これは WASAPI 実装と同じ規律を守るため。
     */
    mix = (float *)malloc((size_t)frames_per_block * sizeof(float));
    pcm = (int16_t *)malloc((size_t)frames_per_block * sizeof(int16_t));
    if (!mix || !pcm) {
        free(mix); free(pcm);
        a->ready_result = (int)VM_ERR_NOMEM;
        vm_ev_set(a->ready_ev);
        return;
    }

    if (write_wav) {
        const char *path = a->params.wav_path;
        if (!path || !path[0]) path = "vmodem_out.wav";
        fp = fopen(path, "wb");
        if (!fp) {
            VM_LOGE("WAV ファイルを開けません: %s", path);
            free(mix); free(pcm);
            a->ready_result = (int)VM_ERR_IO;
            vm_ev_set(a->ready_ev);
            return;
        }
        write_wav_header(fp, a->device_rate, 1, 0);
        a->wav_fp = fp;
        snprintf(a->device_name, sizeof(a->device_name), "%s", path);
    }

    a->ready_result = (int)VM_OK;
    vm_ev_set(a->ready_ev);

    while (!a->quit) {
        uint32_t i;

        vm_audio_backend_handle_flush(a);

        /* WASAPI と全く同じ経路で PCM を取り出す */
        vm_audio_backend_render(a, mix, frames_per_block);

        if (fp) {
            for (i = 0; i < frames_per_block; i++) {
                float s = mix[i];
                if (s >  1.0f) s =  1.0f;
                if (s < -1.0f) s = -1.0f;
                pcm[i] = (int16_t)(s * 32767.0f);
            }
            fwrite(pcm, sizeof(int16_t), frames_per_block, fp);
        }
        total_frames += frames_per_block;

        /* 実デバイスのクロックの代わりに実時間で待つ */
        vm_sleep_ms(BLOCK_MS);
    }

    if (fp) {
        /* データ長を埋め直す */
        uint32_t data_bytes = total_frames * 2u;
        fflush(fp);
        if (fseek(fp, 0, SEEK_SET) == 0)
            write_wav_header(fp, a->device_rate, 1, data_bytes);
        fclose(fp);
        a->wav_fp = NULL;
        VM_LOGI("WAV 書き出し完了: %u フレーム (%.2f 秒)",
                (unsigned)total_frames,
                (double)total_frames / a->device_rate);
    }

    a->wav_frames = total_frames;
    free(mix);
    free(pcm);
}

void vm_audio_run_wavfile(vm_audio_t *a) { run_common(a, true); }
void vm_audio_run_null(vm_audio_t *a)    { run_common(a, false); }
