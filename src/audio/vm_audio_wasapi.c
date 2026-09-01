/*
 * vm_audio_wasapi.c - WASAPI 共有モード / イベント駆動レンダラ
 *
 * =========================================================================
 * このファイルの読み方
 * =========================================================================
 * 処理は次の順で進む。すべて【1 本のレンダースレッド内】で行う事が重要。
 *
 *   1.  CoInitializeEx(MTA)
 *   2.  MMDeviceEnumerator を生成
 *   3.  デバイスを選ぶ (名前部分一致 or 既定デバイス)
 *   4.  IAudioClient を Activate
 *   5.  GetMixFormat でデバイスのミックス フォーマットを取得
 *   6.  フォーマットを解釈 (float32 か PCM16 か、何チャネルか)
 *   7.  Initialize(SHARED, EVENTCALLBACK, ...)
 *   8.  SetEventHandle
 *   9.  GetService(IAudioRenderClient)
 *   10. MMCSS にスレッド登録 (avrt.dll を動的ロード)
 *   11. 無音をプリロールしてから Start
 *   12. [ループ] WaitForSingleObject -> GetCurrentPadding
 *              -> GetBuffer -> 変換して書く -> ReleaseBuffer
 *   13. Stop / 解放 / CoUninitialize
 *
 * =========================================================================
 * MinGW-w64 での注意
 * =========================================================================
 * MinGW の古い環境では mmdeviceapi.h / audioclient.h の一部の GUID が
 * ライブラリに含まれていない事がある。そのため本ファイルでは
 * 必要な IID / CLSID を自前で定義し (DEFINE_GUID 相当)、
 * ライブラリ リンクに依存しないようにしている。
 * また C から COM を叩くので、vtable 経由の呼び出しマクロを使う。
 *   C++  : pClient->GetMixFormat(&fmt)
 *   C    : pClient->lpVtbl->GetMixFormat(pClient, &fmt)
 * 見づらいので HELPER マクロを定義してある。
 */

#ifdef _WIN32

#include "vm_audio_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COBJMACROS          /* C から IFoo_Method(p, ...) 形式を使う */
#define INITGUID            /* GUID の実体をこのファイルに置く */

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

/*
 * ===========================================================================
 * 大文字小文字を無視した部分一致 (StrStrIA の置き換え)
 * ===========================================================================
 * 旧実装は shlwapi.h の StrStrIA() を使っていたが、以下の問題があった:
 *
 *   1. <shlwapi.h> を include しておらず、MinGW-w64 では
 *          error: implicit declaration of function 'StrStrIA'
 *      (C99 以降は暗黙宣言がエラー) になる。
 *   2. include しても -lshlwapi が必要で、リンクライブラリが増える。
 *   3. StrStrIA の「大文字小文字無視」は現在のロケールに依存するため、
 *      日本語ロケールのデバイス名で意図しない一致をする可能性がある。
 *
 * やっている事は ASCII の部分一致だけなので、自前で書くのが最も確実。
 * ロケールに依存しないよう tolower() ではなく手動で 'A'..'Z' を畳む
 * (tolower() は locale 依存で、トルコ語ロケールの 'I' 問題などがある)。
 * ===========================================================================
 */
static char vm_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static const char *vm_stristr(const char *hay, const char *needle)
{
    size_t i;

    if (hay == NULL || needle == NULL)
        return NULL;
    if (needle[0] == '\0')
        return hay;

    for (; *hay != '\0'; hay++) {
        for (i = 0; needle[i] != '\0'; i++) {
            if (vm_ascii_lower(hay[i]) != vm_ascii_lower(needle[i]))
                break;
            if (hay[i] == '\0')
                break;
        }
        if (needle[i] == '\0')
            return hay;
    }

    return NULL;
}

/*
 * ---------------------------------------------------------------------
 * GUID の自前定義
 * ---------------------------------------------------------------------
 * INITGUID を定義した上で mmdeviceapi.h / audioclient.h を include すれば
 * 通常は実体が生成されるが、環境差を吸収するため明示的に用意しておく。
 * (重複定義になる環境では INITGUID 側が優先され問題ない)
 */
#ifndef VM_WASAPI_GUIDS_DEFINED
#define VM_WASAPI_GUIDS_DEFINED
static const CLSID VM_CLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}
};
static const IID VM_IID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}
};
static const IID VM_IID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}
};
static const IID VM_IID_IAudioRenderClient = {
    0xF294ACFC, 0x3146, 0x4483, {0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}
};
#endif

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / PCM (ksmedia.h が無い環境向け) */
static const GUID VM_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {
    0x00000003, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}
};
static const GUID VM_KSDATAFORMAT_SUBTYPE_PCM = {
    0x00000001, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}
};

/* 100ns 単位。共有モードのバッファ長要求 (30ms) */
#define VM_WASAPI_BUFFER_DURATION  300000LL

typedef enum {
    VM_SAMPFMT_UNKNOWN = 0,
    VM_SAMPFMT_FLOAT32,
    VM_SAMPFMT_PCM16,
    VM_SAMPFMT_PCM32,
    VM_SAMPFMT_PCM24
} vm_sampfmt_t;

typedef struct {
    IMMDeviceEnumerator *enumr;
    IMMDevice           *dev;
    IAudioClient        *client;
    IAudioRenderClient  *render;
    HANDLE               ev;
    WAVEFORMATEX        *mixfmt;
    UINT32               buffer_frames;
    vm_sampfmt_t         sampfmt;
    int                  frame_bytes;
    HANDLE               mmcss;
} wasapi_impl_t;

/* --------------------------------------------------------------------- */
/* avrt.dll (MMCSS) の動的ロード                                         */
/*                                                                       */
/* AvSetMmThreadCharacteristicsW を静的リンクすると avrt.lib が必要に     */
/* なり、環境によってはリンクできない。動的ロードにすれば avrt が無い    */
/* 環境でも「優先度を上げないだけ」で動作する。                          */
/* --------------------------------------------------------------------- */
typedef HANDLE (WINAPI *pfn_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
typedef BOOL   (WINAPI *pfn_AvRevertMmThreadCharacteristics)(HANDLE);

static HANDLE mmcss_enter(void)
{
    HMODULE h = LoadLibraryW(L"avrt.dll");
    pfn_AvSetMmThreadCharacteristicsW fn;
    DWORD task_index = 0;
    HANDLE r;

    if (!h) return NULL;
    fn = (pfn_AvSetMmThreadCharacteristicsW)(void *)
         GetProcAddress(h, "AvSetMmThreadCharacteristicsW");
    if (!fn) { FreeLibrary(h); return NULL; }

    r = fn(L"Audio", &task_index);
    /*
     * ライブラリはあえて解放しない (プロセス寿命中保持)。
     * ハンドル r を返した後に FreeLibrary すると
     * AvRevertMmThreadCharacteristics が引けなくなるため。
     */
    return r;
}

static void mmcss_leave(HANDLE h)
{
    HMODULE mod;
    pfn_AvRevertMmThreadCharacteristics fn;
    if (!h) return;
    mod = GetModuleHandleW(L"avrt.dll");
    if (!mod) return;
    fn = (pfn_AvRevertMmThreadCharacteristics)(void *)
         GetProcAddress(mod, "AvRevertMmThreadCharacteristics");
    if (fn) fn(h);
}

/* --------------------------------------------------------------------- */
/* フォーマット解釈                                                      */
/*                                                                       */
/* GetMixFormat はほぼ必ず WAVE_FORMAT_EXTENSIBLE を返す。               */
/* その場合 SubFormat を見て float / PCM を判定する必要がある。           */
/* --------------------------------------------------------------------- */
static vm_sampfmt_t detect_format(const WAVEFORMATEX *f)
{
    if (!f) return VM_SAMPFMT_UNKNOWN;

    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return (f->wBitsPerSample == 32) ? VM_SAMPFMT_FLOAT32 : VM_SAMPFMT_UNKNOWN;

    if (f->wFormatTag == WAVE_FORMAT_PCM) {
        if (f->wBitsPerSample == 16) return VM_SAMPFMT_PCM16;
        if (f->wBitsPerSample == 32) return VM_SAMPFMT_PCM32;
        if (f->wBitsPerSample == 24) return VM_SAMPFMT_PCM24;
        return VM_SAMPFMT_UNKNOWN;
    }

    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        f->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const WAVEFORMATEXTENSIBLE *ex = (const WAVEFORMATEXTENSIBLE *)f;
        if (IsEqualGUID(&ex->SubFormat, &VM_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            return (f->wBitsPerSample == 32)
                 ? VM_SAMPFMT_FLOAT32 : VM_SAMPFMT_UNKNOWN;
        if (IsEqualGUID(&ex->SubFormat, &VM_KSDATAFORMAT_SUBTYPE_PCM)) {
            if (f->wBitsPerSample == 16) return VM_SAMPFMT_PCM16;
            if (f->wBitsPerSample == 32) return VM_SAMPFMT_PCM32;
            if (f->wBitsPerSample == 24) return VM_SAMPFMT_PCM24;
        }
    }
    return VM_SAMPFMT_UNKNOWN;
}

static const char *sampfmt_name(vm_sampfmt_t f)
{
    switch (f) {
    case VM_SAMPFMT_FLOAT32: return "float32";
    case VM_SAMPFMT_PCM16:   return "PCM16";
    case VM_SAMPFMT_PCM24:   return "PCM24";
    case VM_SAMPFMT_PCM32:   return "PCM32";
    default:                 return "unknown";
    }
}

/*
 * float (-1..1) をデバイス フォーマットへ変換して書き込む。
 * 共有モードでは float32 がほぼ確実だが、PCM16 等も一応対応しておく。
 */
static void store_samples(void *dst, const float *src, uint32_t count,
                          vm_sampfmt_t fmt)
{
    uint32_t i;
    switch (fmt) {
    case VM_SAMPFMT_FLOAT32:
        memcpy(dst, src, (size_t)count * sizeof(float));
        break;
    case VM_SAMPFMT_PCM16: {
        int16_t *d = (int16_t *)dst;
        for (i = 0; i < count; i++) {
            float s = src[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            d[i] = (int16_t)(s * 32767.0f);
        }
        break;
    }
    case VM_SAMPFMT_PCM32: {
        int32_t *d = (int32_t *)dst;
        for (i = 0; i < count; i++) {
            float s = src[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            d[i] = (int32_t)(s * 2147483520.0f);
        }
        break;
    }
    case VM_SAMPFMT_PCM24: {
        uint8_t *d = (uint8_t *)dst;
        for (i = 0; i < count; i++) {
            float s = src[i];
            int32_t v;
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            v = (int32_t)(s * 8388607.0f);
            d[i * 3 + 0] = (uint8_t)(v & 0xFF);
            d[i * 3 + 1] = (uint8_t)((v >> 8) & 0xFF);
            d[i * 3 + 2] = (uint8_t)((v >> 16) & 0xFF);
        }
        break;
    }
    default:
        memset(dst, 0, (size_t)count * 4);
        break;
    }
}

/* --------------------------------------------------------------------- */
/* デバイス選択                                                          */
/* --------------------------------------------------------------------- */
static void wide_to_utf8(const WCHAR *w, char *out, int out_size)
{
    if (!w) { if (out_size > 0) out[0] = '\0'; return; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_size, NULL, NULL);
}

/* デバイスのフレンドリ名を取得 */
static void get_device_name(IMMDevice *dev, char *out, int out_size)
{
    IPropertyStore *props = NULL;
    PROPVARIANT pv;

    if (out_size > 0) snprintf(out, (size_t)out_size, "%s", "(unnamed)");
    if (!dev) return;

    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props)) || !props)
        return;

    PropVariantInit(&pv);
    if (SUCCEEDED(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &pv))
        && pv.vt == VT_LPWSTR) {
        wide_to_utf8(pv.pwszVal, out, out_size);
    }
    PropVariantClear(&pv);
    IPropertyStore_Release(props);
}

/*
 * device_match が指定されていれば、名前に部分一致する
 * 出力デバイスを探す。見つからなければ既定デバイスを返す。
 */
static IMMDevice *pick_device(IMMDeviceEnumerator *en, const char *match,
                              char *name_out, int name_size)
{
    IMMDevice *result = NULL;

    if (match && match[0]) {
        IMMDeviceCollection *coll = NULL;
        UINT count = 0, i;

        if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(
                en, eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
            if (SUCCEEDED(IMMDeviceCollection_GetCount(coll, &count))) {
                for (i = 0; i < count; i++) {
                    IMMDevice *d = NULL;
                    char nm[VM_AUDIO_NAME_MAX];
                    if (FAILED(IMMDeviceCollection_Item(coll, i, &d)) || !d)
                        continue;
                    get_device_name(d, nm, (int)sizeof(nm));
                    /* 大文字小文字を無視した部分一致 (自前実装) */
                    if (vm_stristr(nm, match) != NULL) {
                        result = d;
                        snprintf(name_out, (size_t)name_size, "%s", nm);
                        break;
                    }
                    IMMDevice_Release(d);
                }
            }
            IMMDeviceCollection_Release(coll);
        }
        if (!result)
            VM_LOGW("デバイス '%s' が見つかりません。既定デバイスを使います",
                    match);
    }

    if (!result) {
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(
                en, eRender, eConsole, &result)))
            return NULL;
        get_device_name(result, name_out, name_size);
    }
    return result;
}

/* --------------------------------------------------------------------- */
/* 後片付け                                                              */
/* --------------------------------------------------------------------- */
static void wasapi_cleanup(wasapi_impl_t *w)
{
    if (!w) return;
    if (w->mmcss)  { mmcss_leave(w->mmcss); w->mmcss = NULL; }
    if (w->render) { IAudioRenderClient_Release(w->render); w->render = NULL; }
    if (w->client) { IAudioClient_Release(w->client); w->client = NULL; }
    if (w->mixfmt) { CoTaskMemFree(w->mixfmt); w->mixfmt = NULL; }
    if (w->dev)    { IMMDevice_Release(w->dev); w->dev = NULL; }
    if (w->enumr)  { IMMDeviceEnumerator_Release(w->enumr); w->enumr = NULL; }
    if (w->ev)     { CloseHandle(w->ev); w->ev = NULL; }
}

/* --------------------------------------------------------------------- */
/* レンダー ループ本体                                                    */
/* --------------------------------------------------------------------- */
void vm_audio_run_wasapi(vm_audio_t *a)
{
    wasapi_impl_t w;
    HRESULT hr;
    bool com_ok = false;
    bool started = false;
    vm_err_t rc = VM_ERR_IO;
    /*
     * 中間バッファ。レンダースレッド内で malloc しないため静的サイズ。
     * 48kHz / 30ms / 8ch でも 11520 フレームには届かないが、
     * 余裕をみて 8192 フレーム × 8ch = 65536 サンプル分確保する。
     */
    static float mixbuf[8192 * 8];

    memset(&w, 0, sizeof(w));

    /* ---- 1. COM 初期化 (このスレッドで行う事が重要) ---- */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        /* 既に STA で初期化済み: そのまま使う (Uninitialize しない) */
        VM_LOGW("COM は既に STA で初期化されています");
    } else if (FAILED(hr)) {
        VM_LOGE("CoInitializeEx 失敗: 0x%08lX", (unsigned long)hr);
        goto fail;
    } else {
        com_ok = true;
    }

    /* ---- 2. デバイス列挙子 ---- */
    hr = CoCreateInstance(&VM_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &VM_IID_IMMDeviceEnumerator, (void **)&w.enumr);
    if (FAILED(hr) || !w.enumr) {
        VM_LOGE("MMDeviceEnumerator の生成に失敗: 0x%08lX", (unsigned long)hr);
        goto fail;
    }

    /* ---- 3. デバイス選択 ---- */
    w.dev = pick_device(w.enumr, a->params.device_match,
                        a->device_name, (int)sizeof(a->device_name));
    if (!w.dev) {
        VM_LOGE("再生デバイスが見つかりません");
        rc = VM_ERR_NOTFOUND;
        goto fail;
    }

    /* ---- 4. IAudioClient ---- */
    hr = IMMDevice_Activate(w.dev, &VM_IID_IAudioClient, CLSCTX_ALL,
                            NULL, (void **)&w.client);
    if (FAILED(hr) || !w.client) {
        VM_LOGE("IAudioClient の Activate に失敗: 0x%08lX", (unsigned long)hr);
        goto fail;
    }

    /* ---- 5/6. ミックス フォーマット取得と解釈 ---- */
    hr = IAudioClient_GetMixFormat(w.client, &w.mixfmt);
    if (FAILED(hr) || !w.mixfmt) {
        VM_LOGE("GetMixFormat に失敗: 0x%08lX", (unsigned long)hr);
        goto fail;
    }

    w.sampfmt = detect_format(w.mixfmt);
    if (w.sampfmt == VM_SAMPFMT_UNKNOWN) {
        VM_LOGE("未対応のミックス フォーマット (tag=%u bits=%u)",
                (unsigned)w.mixfmt->wFormatTag,
                (unsigned)w.mixfmt->wBitsPerSample);
        rc = VM_ERR_UNSUPPORTED;
        goto fail;
    }

    a->device_rate     = (int)w.mixfmt->nSamplesPerSec;
    a->device_channels = (int)w.mixfmt->nChannels;
    w.frame_bytes      = (int)w.mixfmt->nBlockAlign;

    VM_LOGD("WASAPI ミックス フォーマット: %dHz %dch %s (block %d bytes)",
            a->device_rate, a->device_channels,
            sampfmt_name(w.sampfmt), w.frame_bytes);

    /* チャネル数が中間バッファを超える異常系を弾く */
    if (a->device_channels <= 0 || a->device_channels > 8) {
        VM_LOGE("対応外のチャネル数: %d", a->device_channels);
        rc = VM_ERR_UNSUPPORTED;
        goto fail;
    }

    /* ---- 7. Initialize (共有モード + イベント駆動) ---- */
    hr = IAudioClient_Initialize(w.client,
                                 AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 VM_WASAPI_BUFFER_DURATION,
                                 0,             /* 共有モードでは 0 必須 */
                                 w.mixfmt,
                                 NULL);
    if (FAILED(hr)) {
        VM_LOGE("IAudioClient_Initialize に失敗: 0x%08lX", (unsigned long)hr);
        goto fail;
    }

    hr = IAudioClient_GetBufferSize(w.client, &w.buffer_frames);
    if (FAILED(hr)) {
        VM_LOGE("GetBufferSize に失敗: 0x%08lX", (unsigned long)hr);
        goto fail;
    }
    VM_LOGD("WASAPI バッファ: %u フレーム (%.1f ms)",
            (unsigned)w.buffer_frames,
            1000.0 * w.buffer_frames / a->device_rate);

    if ((size_t)w.buffer_frames * (size_t)a->device_channels
            > sizeof(mixbuf) / sizeof(mixbuf[0])) {
        VM_LOGE("WASAPI バッファが中間バッファを超えています (%u frames x %dch)",
                (unsigned)w.buffer_frames, a->device_channels);
        rc = VM_ERR_UNSUPPORTED;
        goto fail;
    }

    /* ---- 8. イベント登録 ---- */
    w.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!w.ev) { VM_LOGE("イベント生成に失敗"); goto fail; }

    hr = IAudioClient_SetEventHandle(w.client, w.ev);
    if (FAILED(hr)) {
        VM_LOGE("SetEventHandle に失敗: 0x%08lX", (unsigned long)hr);
        goto fail;
    }

    /* ---- 9. IAudioRenderClient ---- */
    hr = IAudioClient_GetService(w.client, &VM_IID_IAudioRenderClient,
                                 (void **)&w.render);
    if (FAILED(hr) || !w.render) {
        VM_LOGE("IAudioRenderClient の取得に失敗: 0x%08lX", (unsigned long)hr);
        goto fail;
    }

    /* ---- リサンプラ初期化 (デバイスレート確定後) ---- */
    rc = vm_audio_backend_resampler_init(a);
    if (rc != VM_OK) {
        VM_LOGE("リサンプラ初期化に失敗");
        goto fail;
    }

    /* ---- 10. MMCSS 登録 ---- */
    w.mmcss = mmcss_enter();
    if (!w.mmcss)
        VM_LOGW("MMCSS への登録に失敗 (優先度が上がりません)");

    /*
     * ---- 11. プリロール ----
     * Start() 前にバッファ全体を無音で埋める。
     * これをやらないと開始直後に必ずグリッチが出る。
     */
    {
        BYTE *p = NULL;
        hr = IAudioRenderClient_GetBuffer(w.render, w.buffer_frames, &p);
        if (SUCCEEDED(hr) && p) {
            IAudioRenderClient_ReleaseBuffer(w.render, w.buffer_frames,
                                             AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    hr = IAudioClient_Start(w.client);
    if (FAILED(hr)) {
        VM_LOGE("IAudioClient_Start に失敗: 0x%08lX", (unsigned long)hr);
        rc = VM_ERR_IO;
        goto fail;
    }
    started = true;

    /* ---- 初期化成功を通知 ---- */
    a->ready_result = (int)VM_OK;
    vm_ev_set(a->ready_ev);

    /* ================================================================= */
    /* 12. レンダー ループ                                                */
    /* ================================================================= */
    while (!a->quit) {
        UINT32 padding = 0, avail;
        BYTE  *dst = NULL;

        /*
         * デバイスが「バッファに空きが出た」と教えてくれるのを待つ。
         * タイムアウトを付けるのは、デバイス取り外し等で
         * イベントが来なくなった場合に quit を確認するため。
         */
        if (WaitForSingleObject(w.ev, 200) == WAIT_TIMEOUT)
            continue;

        if (a->quit) break;

        /* flush 要求の処理 (消費者スレッドなので安全に reset できる) */
        vm_audio_backend_handle_flush(a);

        hr = IAudioClient_GetCurrentPadding(w.client, &padding);
        if (FAILED(hr)) {
            /*
             * デバイスが無効化された場合 AUDCLNT_E_DEVICE_INVALIDATED が
             * 返る。本エミュレータでは音が出なくてもモデム動作は
             * 継続させたいので、ループを抜けるだけにする。
             */
            VM_LOGW("GetCurrentPadding 失敗 (0x%08lX): レンダーを停止します",
                    (unsigned long)hr);
            break;
        }

        if (padding >= w.buffer_frames)
            continue;                     /* 空きが無い: 次のイベントを待つ */

        avail = w.buffer_frames - padding;

        hr = IAudioRenderClient_GetBuffer(w.render, avail, &dst);
        if (FAILED(hr) || !dst) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                VM_LOGW("オーディオ デバイスが無効化されました");
                break;
            }
            continue;
        }

        /* リング -> リサンプル -> 音量 -> チャネル複製 */
        vm_audio_backend_render(a, mixbuf, avail);

        /* デバイス フォーマットへ変換して書き込む */
        store_samples(dst, mixbuf,
                      avail * (uint32_t)a->device_channels, w.sampfmt);

        IAudioRenderClient_ReleaseBuffer(w.render, avail, 0);
    }

    /* ---- 13. 停止 ---- */
    if (started) {
        IAudioClient_Stop(w.client);
        IAudioClient_Reset(w.client);
    }
    wasapi_cleanup(&w);
    if (com_ok) CoUninitialize();
    return;

fail:
    wasapi_cleanup(&w);
    if (com_ok) CoUninitialize();
    a->ready_result = (int)rc;
    vm_ev_set(a->ready_ev);
}

#endif /* _WIN32 */
