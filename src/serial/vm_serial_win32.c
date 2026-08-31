/*
 * vm_serial_win32.c - Win32 COM ポート バックエンド (com0com の CNCB0 等)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * 実装上の難所 (1): ポート名の \\.\ 前置
 * ===========================================================================
 * "COM1".."COM9" は CreateFile("COM1", ...) で開けるが、
 * "COM10" 以上は DOS デバイス名の互換規則の都合で開けない。
 * また com0com のポート名 "CNCB0" は COM で始まらないので、
 * これも \\.\ 前置が必要。
 *
 *   OK : CreateFile("\\\\.\\COM10")
 *   OK : CreateFile("\\\\.\\CNCB0")
 *   NG : CreateFile("COM10")        <- ERROR_FILE_NOT_FOUND
 *
 * 従って「常に \\.\ を前置する」のが唯一安全な方法である。
 * ユーザが既に "\\.\CNCB0" と書いていた場合は二重付与を避ける。
 *
 * ===========================================================================
 * 実装上の難所 (2): ブロッキング ReadFile は使えない
 * ===========================================================================
 * 素朴に
 *     ReadFile(h, buf, len, &got, NULL);
 * と書くと、COMMTIMEOUTS の設定次第で「1 バイトも来ない間ずっと固まる」。
 * その間はハングアップ要求にもシャットダウンにも応答できない。
 *
 * VModem は次の構成にする:
 *
 *   1. CreateFile に FILE_FLAG_OVERLAPPED を付ける (非同期 I/O)
 *   2. COMMTIMEOUTS を「即座に戻る」に設定
 *   3. ReadFile を OVERLAPPED 付きで発行
 *      - 即完了すれば TRUE が返る
 *      - 保留なら FALSE + GetLastError()==ERROR_IO_PENDING
 *   4. 保留なら WaitForMultipleObjects で
 *          [ov_read_ev (I/O完了), cancel_ev (終了要求)]
 *      の両方を待つ
 *   5. cancel_ev が先に来たら CancelIo() で取り消して抜ける
 *
 * これで「読み込み待ち中でも即座に安全に止められる」。
 *
 * ★ 落とし穴 ★
 *   CancelIo() の後、OVERLAPPED 構造体とバッファは
 *   「I/O が本当に終わるまで」解放してはいけない。
 *   そのため CancelIo の後に GetOverlappedResult(..., TRUE) で
 *   完了を待ち切ってから抜ける。これを忘れるとスタック上の
 *   OVERLAPPED をカーネルが後で書き込み、スタック破壊を起こす。
 *   (デバッグが極めて困難なクラッシュになる典型例)
 *
 * ===========================================================================
 * 実装上の難所 (3): COMMTIMEOUTS の意味
 * ===========================================================================
 * この構造体は直感に反する挙動をする。
 *
 *   ReadIntervalTimeout        = MAXDWORD
 *   ReadTotalTimeoutMultiplier = 0
 *   ReadTotalTimeoutConstant   = 0
 *
 * この「魔法の組み合わせ」だけが「バッファにある分だけ読んで即座に戻る、
 * 何も無ければ 0 バイトで即座に戻る」を意味する。
 * MSDN に明記されている特別ケースで、他の値では実現できない。
 *
 * 我々は timeout_ms > 0 のときだけ WaitCommEvent 相当の待ちを
 * OVERLAPPED 側で行うので、ドライバのタイムアウトは常に「即戻り」で良い。
 *
 * ===========================================================================
 * 実装上の難所 (4): DCD を「出力」する側になる
 * ===========================================================================
 * 通常のシリアル通信では DCD は入力線 (モデムが出し、PC が読む)。
 * 我々はモデム役なので DCD を *出力* しなければならない。
 *
 * しかし Win32 API には「DCD を上げる」関数が存在しない
 * (EscapeCommFunction は DTR/RTS = PC 側が出す線しか制御できない)。
 *
 * com0com はこれを「ピン配線の設定」で解決している。
 * ドライバのポート設定で
 *      CNCB0 の DTR  ->  CNCA0 の DCD
 * とクロス配線しておけば、我々が CNCB0 の DTR を上げると
 * RAS 側 (CNCA0) の DCD が上がる。
 *
 * つまり:
 *   - 我々が呼ぶのは EscapeCommFunction(SETDTR)
 *   - RAS が見るのは DCD
 * という対応になる。setupc.exe での配線設定が必須で、
 * scripts/setup-com0com.ps1 がこれを自動でやる。
 *
 * 同様に「RAS の DTR が落ちた」= 我々の DCD 入力が落ちた事になるので、
 * GetCommModemStatus() の MS_RLSD_ON を見て検出する。
 */
#if !defined(_WIN32)
/* 非 Windows では空翻訳単位にする (CMake が常にこのファイルを渡しても安全) */
typedef int vm_serial_win32_dummy_t;
#else

#include "vm_serial_internal.h"
#include "vmodem/vm_log.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * ポート名の正規化
 * -------------------------------------------------------------------------*/
static void build_device_path(const char *port, char *out, size_t out_size)
{
    if (!port || !*port) port = "CNCB0";

    /* 既に \\.\ が付いていればそのまま */
    if (port[0] == '\\' && port[1] == '\\' &&
        (port[2] == '.' || port[2] == '?') && port[3] == '\\') {
        snprintf(out, out_size, "%s", port);
        return;
    }
    snprintf(out, out_size, "\\\\.\\%s", port);
}

/* ---------------------------------------------------------------------------
 * オープン
 * -------------------------------------------------------------------------*/
vm_err_t vm_serial_open_win32(vm_serial_t *s, char *errbuf, size_t errbuf_size)
{
    char path[160];
    DCB dcb;
    COMMTIMEOUTS to;
    DWORD err;

    build_device_path(s->params.port, path, sizeof(path));

    /*
     * 共有不可 (dwShareMode = 0) で開く。シリアルポートは排他が必須。
     * FILE_FLAG_OVERLAPPED で非同期 I/O を有効化する。
     */
    s->h = CreateFileA(path,
                       GENERIC_READ | GENERIC_WRITE,
                       0,                  /* 共有しない            */
                       NULL,
                       OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED,
                       NULL);

    if (s->h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        s->h = NULL;
        if (err == ERROR_FILE_NOT_FOUND) {
            snprintf(errbuf, errbuf_size,
                     "COM port \"%s\" not found. "
                     "com0com is probably not installed, or the port name is "
                     "wrong. Run scripts\\setup-com0com.ps1 first, then check "
                     "the pair names with: setupc.exe list",
                     s->params.port ? s->params.port : "CNCB0");
        } else if (err == ERROR_ACCESS_DENIED) {
            snprintf(errbuf, errbuf_size,
                     "COM port \"%s\" is already in use by another process. "
                     "Close any terminal software (TeraTerm/PuTTY) and make "
                     "sure Windows RAS is assigned to the OTHER half of the "
                     "pair (CNCA0), not this one",
                     s->params.port ? s->params.port : "CNCB0");
        } else {
            snprintf(errbuf, errbuf_size,
                     "CreateFile(\"%s\") failed, GetLastError()=%lu",
                     path, (unsigned long)err);
        }
        return VM_ERR_IO;
    }

    /*
     * ----------------------------------------------------------------
     * DCB (通信パラメータ)
     * ----------------------------------------------------------------
     * ★ 必ず GetCommState で現在値を取ってから変更する ★
     * DCB はメンバが非常に多く、ゼロ埋めから作ると
     * 予約フィールドが不正な値になり SetCommState が失敗する。
     */
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(s->h, &dcb)) {
        snprintf(errbuf, errbuf_size, "GetCommState failed (%lu)",
                 (unsigned long)GetLastError());
        CloseHandle(s->h); s->h = NULL;
        return VM_ERR_IO;
    }

    dcb.BaudRate = (DWORD)s->params.baud;
    dcb.ByteSize = (BYTE)(s->params.data_bits > 0 ? s->params.data_bits : 8);
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;

    /*
     * フロー制御は全部切る。
     *
     * ★ 極めて重要 ★
     * PPP フレームは 0x00-0xFF の任意のバイトを含むため、
     * XON/XOFF (0x11/0x13) を有効にすると PPP が壊れる。
     * PPP 側の ACCM (Async Control Character Map) でエスケープする
     * 事もできるが、そもそもソフトウェアフロー制御を使わないのが正解。
     *
     * RTS/CTS も com0com では意味が無いので切る。代わりに
     * DTR は我々が「DCD 相当の信号」として手動制御するため
     * DTR_CONTROL_DISABLE (= 手動) にする。
     */
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX        = FALSE;
    dcb.fInX         = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fRtsControl  = RTS_CONTROL_ENABLE;
    dcb.fDtrControl  = DTR_CONTROL_DISABLE;  /* 手動制御 = DCD として使う */
    dcb.fBinary      = TRUE;                 /* Windows では常に TRUE 必須 */
    dcb.fParity      = FALSE;
    dcb.fNull        = FALSE;                /* 0x00 を捨てない (PPP に必須) */
    dcb.fAbortOnError= FALSE;

    if (!SetCommState(s->h, &dcb)) {
        snprintf(errbuf, errbuf_size, "SetCommState failed (%lu)",
                 (unsigned long)GetLastError());
        CloseHandle(s->h); s->h = NULL;
        return VM_ERR_IO;
    }

    /*
     * ----------------------------------------------------------------
     * COMMTIMEOUTS: 「ある分だけ読んで即戻る」の魔法の組み合わせ
     * ----------------------------------------------------------------
     */
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    /* 書き込みは 5 秒でタイムアウト (相手が読まない場合の保険) */
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 5000;
    if (!SetCommTimeouts(s->h, &to)) {
        snprintf(errbuf, errbuf_size, "SetCommTimeouts failed (%lu)",
                 (unsigned long)GetLastError());
        CloseHandle(s->h); s->h = NULL;
        return VM_ERR_IO;
    }

    /* ドライバ内部バッファを大きめに */
    SetupComm(s->h, (DWORD)s->params.rx_buf_bytes,
                    (DWORD)s->params.tx_buf_bytes);

    /* 残留データを破棄 */
    PurgeComm(s->h, PURGE_RXCLEAR | PURGE_TXCLEAR |
                    PURGE_RXABORT | PURGE_TXABORT);

    /*
     * ----------------------------------------------------------------
     * OVERLAPPED 用イベント
     * ----------------------------------------------------------------
     * 手動リセット (bManualReset=TRUE) にする事。
     * 自動リセットだと WaitForMultipleObjects が
     * イベントを消費してしまい、GetOverlappedResult が
     * 二重待ちして固まる事がある。
     */
    s->ov_read_ev  = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->ov_write_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->cancel_ev   = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!s->ov_read_ev || !s->ov_write_ev || !s->cancel_ev) {
        snprintf(errbuf, errbuf_size, "CreateEvent failed (%lu)",
                 (unsigned long)GetLastError());
        vm_serial_close_win32(s);
        return VM_ERR_IO;
    }

    memset(&s->ov_read,  0, sizeof(s->ov_read));
    memset(&s->ov_write, 0, sizeof(s->ov_write));
    s->ov_read.hEvent  = s->ov_read_ev;
    s->ov_write.hEvent = s->ov_write_ev;
    s->read_pending = false;

    snprintf(s->name, sizeof(s->name), "%s",
             s->params.port ? s->params.port : "CNCB0");

    /*
     * 初期状態: オンフック相当。
     * DTR (= RAS 側の DCD) は落としておく。
     */
    s->dcd = false;
    vm_serial_set_lines_win32(s);

    VM_LOGI("COM port opened: %s (%d bps, overlapped)", s->name, s->params.baud);
    return VM_OK;
}

/* ---------------------------------------------------------------------------
 * クローズ
 * -------------------------------------------------------------------------*/
void vm_serial_close_win32(vm_serial_t *s)
{
    if (!s) return;

    /* 進行中の読み込みを起こす */
    if (s->cancel_ev) SetEvent(s->cancel_ev);

    if (s->h) {
        /*
         * 切断を RAS に伝えるため DTR (= DCD) を落とす。
         */
        EscapeCommFunction(s->h, CLRDTR);
        EscapeCommFunction(s->h, CLRRTS);

        /*
         * ★ 順序が重要 ★
         * CancelIoEx で保留 I/O を取り消し、
         * 完了を待ってから CloseHandle する。
         * 逆順にすると OVERLAPPED の書き込み先が消えた後に
         * カーネルが書き込む可能性がある。
         */
        CancelIoEx(s->h, NULL);

        if (s->read_pending) {
            DWORD got = 0;
            /* TRUE = 完了するまで待つ */
            GetOverlappedResult(s->h, &s->ov_read, &got, TRUE);
            s->read_pending = false;
        }

        PurgeComm(s->h, PURGE_RXCLEAR | PURGE_TXCLEAR |
                        PURGE_RXABORT | PURGE_TXABORT);
        CloseHandle(s->h);
        s->h = NULL;
    }

    if (s->ov_read_ev)  { CloseHandle(s->ov_read_ev);  s->ov_read_ev  = NULL; }
    if (s->ov_write_ev) { CloseHandle(s->ov_write_ev); s->ov_write_ev = NULL; }
    if (s->cancel_ev)   { CloseHandle(s->cancel_ev);   s->cancel_ev   = NULL; }

    VM_LOGI("COM port closed (rx=%llu tx=%llu bytes)",
            (unsigned long long)s->rx_bytes, (unsigned long long)s->tx_bytes);
}

/* ---------------------------------------------------------------------------
 * 読み込み
 * -------------------------------------------------------------------------*/
int vm_serial_read_win32(vm_serial_t *s, void *buf, int len, int timeout_ms)
{
    DWORD got = 0;
    DWORD err;

    if (!s->h) return VM_ERR_STATE;

    ResetEvent(s->ov_read_ev);
    memset(&s->ov_read, 0, sizeof(s->ov_read));
    s->ov_read.hEvent = s->ov_read_ev;

    if (ReadFile(s->h, buf, (DWORD)len, &got, &s->ov_read)) {
        /* 即完了 */
        return (int)got;
    }

    err = GetLastError();
    if (err != ERROR_IO_PENDING) {
        if (err == ERROR_OPERATION_ABORTED) {
            /* CancelIo された: エラーではない */
            ClearCommError(s->h, NULL, NULL);
            return 0;
        }
        VM_LOGW("ReadFile failed (%lu)", (unsigned long)err);
        ClearCommError(s->h, NULL, NULL);
        return VM_ERR_IO;
    }

    /* ------------------------------------------------------------------ *
     * 保留中: I/O 完了と中断要求の両方を待つ
     * ------------------------------------------------------------------ */
    s->read_pending = true;
    {
        HANDLE hs[2];
        DWORD  wait_ms = (timeout_ms > 0) ? (DWORD)timeout_ms
                       : (timeout_ms == 0 ? 0 : INFINITE);
        DWORD  r;

        hs[0] = s->ov_read_ev;
        hs[1] = s->cancel_ev;

        r = WaitForMultipleObjects(2, hs, FALSE, wait_ms);

        if (r == WAIT_OBJECT_0) {
            /* I/O 完了 */
            if (GetOverlappedResult(s->h, &s->ov_read, &got, FALSE)) {
                s->read_pending = false;
                return (int)got;
            }
            err = GetLastError();
            s->read_pending = false;
            if (err == ERROR_OPERATION_ABORTED) {
                ClearCommError(s->h, NULL, NULL);
                return 0;
            }
            VM_LOGW("GetOverlappedResult(read) failed (%lu)",
                    (unsigned long)err);
            return VM_ERR_IO;
        }

        /*
         * 中断要求 or タイムアウト。
         * どちらの場合も保留 I/O を取り消し、
         * 「本当に終わるまで」待ってから戻る (スタック破壊防止)。
         */
        CancelIoEx(s->h, &s->ov_read);
        GetOverlappedResult(s->h, &s->ov_read, &got, TRUE);
        s->read_pending = false;

        if (r == WAIT_OBJECT_0 + 1) {
            VM_LOGD("serial read cancelled by shutdown request");
            return 0;
        }
        /* タイムアウトでも、取り消し前に届いていた分は返す */
        return (int)got;
    }
}

/* ---------------------------------------------------------------------------
 * 書き込み
 * -------------------------------------------------------------------------*/
int vm_serial_write_win32(vm_serial_t *s, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int done = 0;

    if (!s->h) return VM_ERR_STATE;

    while (done < len) {
        DWORD wrote = 0;
        DWORD err;

        ResetEvent(s->ov_write_ev);
        memset(&s->ov_write, 0, sizeof(s->ov_write));
        s->ov_write.hEvent = s->ov_write_ev;

        if (WriteFile(s->h, p + done, (DWORD)(len - done), &wrote,
                      &s->ov_write)) {
            done += (int)wrote;
            continue;
        }

        err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            VM_LOGW("WriteFile failed (%lu)", (unsigned long)err);
            ClearCommError(s->h, NULL, NULL);
            return done > 0 ? done : VM_ERR_IO;
        }

        {
            HANDLE hs[2];
            DWORD  r;
            hs[0] = s->ov_write_ev;
            hs[1] = s->cancel_ev;

            /* 書き込みは 5 秒で諦める (COMMTIMEOUTS と揃える) */
            r = WaitForMultipleObjects(2, hs, FALSE, 5000);

            if (r == WAIT_OBJECT_0 &&
                GetOverlappedResult(s->h, &s->ov_write, &wrote, FALSE)) {
                done += (int)wrote;
                continue;
            }

            CancelIoEx(s->h, &s->ov_write);
            GetOverlappedResult(s->h, &s->ov_write, &wrote, TRUE);
            done += (int)wrote;

            if (r == WAIT_OBJECT_0 + 1) return done;   /* 中断 */
            VM_LOGW("serial write timed out (%d/%d bytes)", done, len);
            return done;
        }
    }
    return done;
}

void vm_serial_flush_win32(vm_serial_t *s)
{
    if (s && s->h) FlushFileBuffers(s->h);
}

void vm_serial_purge_rx_win32(vm_serial_t *s)
{
    if (s && s->h) PurgeComm(s->h, PURGE_RXCLEAR | PURGE_RXABORT);
}

/* ---------------------------------------------------------------------------
 * モデム制御線
 * ---------------------------------------------------------------------------
 * 前述の通り、我々の DTR が com0com のクロス配線で
 * RAS 側の DCD になる。
 * -------------------------------------------------------------------------*/
void vm_serial_set_lines_win32(vm_serial_t *s)
{
    if (!s || !s->h) return;

    /* DCD 相当 = 我々の DTR */
    EscapeCommFunction(s->h, s->dcd ? SETDTR : CLRDTR);

    /* CTS 相当 = 我々の RTS */
    EscapeCommFunction(s->h, s->cts ? SETRTS : CLRRTS);
}

/*
 * RAS 側の DTR を読む。
 * com0com のクロス配線では「RAS の DTR」が「我々の DCD 入力」に来るので
 * MS_RLSD_ON (RLSD = Received Line Signal Detect = DCD) を見る。
 */
bool vm_serial_get_dtr_win32(vm_serial_t *s)
{
    DWORD st = 0;
    if (!s || !s->h) return false;
    if (!GetCommModemStatus(s->h, &st)) return false;
    return (st & MS_RLSD_ON) != 0;
}

#endif /* _WIN32 */
