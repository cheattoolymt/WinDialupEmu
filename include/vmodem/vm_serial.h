/*
 * vm_serial.h - DTE 側シリアルポート抽象 (仮想 COM ポート)
 *
 * VModem - Windows 用 本物のダイアルアップモデムエミュレータ
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * この層の役割
 * ===========================================================================
 * Windows のダイアルアップ (RAS) は「COM ポートの向こうに Hayes 互換モデムが
 * 居る」という前提で動く。従って我々がやるべき事は
 *
 *     [Windows RAS] --- COMポート --- [VModem]
 *
 * の右側を担当する事、つまり
 *   1. COM ポートを開いて AT コマンド文字列を受け取る
 *   2. "OK" / "CONNECT 33600" / "NO CARRIER" 等を返す
 *   3. CONNECT 後は透過的なバイトストリーム (= PPP フレーム) を中継する
 * である。
 *
 * ---------------------------------------------------------------------------
 * なぜ「仮想 COM ポートのペア」が必要なのか
 * ---------------------------------------------------------------------------
 * 1 本の COM ポートは 1 プロセスしか開けない。Windows RAS が COM5 を開いたら
 * 我々は COM5 を開けない。そこで com0com (LGPL/GPL のカーネルドライバ) で
 *
 *     CNCA0  <==== 内部で直結 ====>  CNCB0
 *
 * というヌルモデムケーブル相当のペアを作り、
 *   - Windows RAS のモデム定義には CNCA0 を割り当てる
 *   - VModem は CNCB0 を開く
 * とする。片方に書いたバイトがもう片方から読める。
 *
 * ★ ライセンス上の重要点 ★
 *   com0com はカーネルモードドライバであり、我々は「OS の CreateFile API で
 *   デバイスを開く」だけである。ドライバとのリンクは一切発生しないので
 *   GPL の伝播は起こらない (プロセス境界どころかカーネル/ユーザ境界)。
 *   詳細は README.md の「OSS ライセンス整理」を参照。
 *
 * ---------------------------------------------------------------------------
 * 実装上の難所: Win32 のシリアル I/O
 * ---------------------------------------------------------------------------
 * 素朴に ReadFile() をブロッキングで呼ぶと、
 *   - 相手が何も送ってこない間スレッドが固まる
 *   - ハングアップやシャットダウンで中断できない
 * という致命的な問題が起きる。VModem では
 *
 *   (a) FILE_FLAG_OVERLAPPED で開く
 *   (b) COMMTIMEOUTS を「読めるだけ読んで即戻る」に設定
 *   (c) ReadFile が ERROR_IO_PENDING を返したら WaitForMultipleObjects で
 *       [I/O完了イベント, 終了要求イベント] の両方を待つ
 *   (d) 終了要求が先に来たら CancelIo() で読み込みを取り消す
 *
 * という構成にする。これで「いつでも即座に安全に止められる」読み込みが
 * 実現できる。詳細は src/serial/vm_serial_win32.c のコメントを参照。
 *
 * ---------------------------------------------------------------------------
 * 移植性とテスト
 * ---------------------------------------------------------------------------
 * サンドボックス/CI (Linux) では COM ポートが存在しないので、
 * 同じインタフェースを実装した 2 つの代替バックエンドを用意する:
 *
 *   VM_SERIAL_BACKEND_LOOPBACK : メモリ内リングによる完全なループバック。
 *                                AT コマンドハンドラの単体テストに使う。
 *   VM_SERIAL_BACKEND_PTY      : POSIX の openpty()。実際に /dev/pts/N が
 *                                生えるので minicom 等で手動確認できる。
 *
 * AT ハンドラ本体 (vm_at.c) はこの層より上にあり、I/O を一切知らない
 * 純粋な状態機械なので、どのバックエンドでも同一に動作する。
 */
#ifndef VMODEM_VM_SERIAL_H
#define VMODEM_VM_SERIAL_H

#include "vmodem/vm_types.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum {
    VM_SERIAL_BACKEND_AUTO = 0,  /* Windows なら WIN32COM, それ以外は PTY  */
    VM_SERIAL_BACKEND_WIN32COM,  /* 実 COM ポート (com0com の CNCB0 等)    */
    VM_SERIAL_BACKEND_PTY,       /* POSIX 疑似端末 (手動テスト用)          */
    VM_SERIAL_BACKEND_LOOPBACK   /* メモリ内 (単体テスト用)                */
} vm_serial_backend_t;

typedef struct {
    vm_serial_backend_t backend;

    /*
     * ポート名。
     *   Windows : "CNCB0" / "COM5" / "\\\\.\\CNCB0"
     *             ("COM10" 以上は \\.\ 前置が必須。内部で自動補完する)
     *   PTY     : 無視 (割り当てられた名前は vm_serial_name() で取得)
     */
    const char *port;

    /*
     * ボーレート等。
     *
     * ★ 注意 ★
     *   これは「PC と仮想モデムの間 (DTE 速度)」の設定であって、
     *   電話線側の速度 (33600bps 等) とは無関係である。
     *   実機のモデムでも DTE 速度は 115200bps 固定で、
     *   回線速度は CONNECT メッセージで通知するのが普通。
     *   com0com は仮想ドライバなのでボーレート設定は実質無視されるが、
     *   RAS が SetCommState を呼んでも失敗しないよう受け付ける。
     */
    int  baud;          /* 既定 115200 */
    int  data_bits;     /* 既定 8      */
    int  stop_bits;     /* 既定 1      */
    int  parity;        /* 0 = none    */

    int  rx_buf_bytes;  /* 内部受信バッファ 既定 8192 */
    int  tx_buf_bytes;  /* 内部送信バッファ 既定 8192 */
} vm_serial_params_t;

void vm_serial_params_defaults(vm_serial_params_t *p);

typedef struct vm_serial_s vm_serial_t;

/*
 * オープン / クローズ。
 * 失敗時 *out は NULL のまま。errbuf に人間可読な理由を入れる
 * (「ポートが見つからない」「既に他プロセスが開いている」の区別が
 *  ユーザには決定的に重要なので、必ずメッセージを渡す)。
 */
vm_err_t vm_serial_open(vm_serial_t **out, const vm_serial_params_t *params,
                        char *errbuf, size_t errbuf_size);
void     vm_serial_close(vm_serial_t *s);

/*
 * ノンブロッキング読み込み。
 * 戻り値 = 読めたバイト数 (0 = 今は何も来ていない, 負 = エラー)。
 * timeout_ms > 0 の場合、最初の 1 バイトが来るまで最大その時間待つ。
 * timeout_ms == 0 なら即座に戻る。
 */
int vm_serial_read(vm_serial_t *s, void *buf, int len, int timeout_ms);

/*
 * 書き込み。len 全部書けるまで試みる (短絡書き込みは内部で再試行)。
 * 戻り値 = 書けたバイト数 (負 = エラー)。
 */
int vm_serial_write(vm_serial_t *s, const void *buf, int len);

/* 文字列送信のショートカット (AT 応答用) */
int vm_serial_write_str(vm_serial_t *s, const char *str);

/* 送信バッファを吐き切るまで待つ */
void vm_serial_flush(vm_serial_t *s);

/* 受信バッファを破棄 (ハングアップ時に残留 AT を捨てる) */
void vm_serial_purge_rx(vm_serial_t *s);

/*
 * ---------------------------------------------------------------------------
 * モデム制御線
 * ---------------------------------------------------------------------------
 * RAS は DCD (Data Carrier Detect) を見て「回線が繋がっているか」を判断する。
 * 実機モデムでは CONNECT 時に DCD を上げ、切断時に下げる。
 * com0com は DTR/RTS/DCD/DSR/CTS のクロス配線をエミュレートできるので、
 * ここで DCD を操作すると RAS 側が正しく切断を検出できる。
 *
 * DTR が落ちた事を検出したら (RAS がポートを閉じた = ユーザが切断した)
 * 我々もハングアップしなければならない。
 */
void vm_serial_set_dcd(vm_serial_t *s, bool on);
void vm_serial_set_dsr(vm_serial_t *s, bool on);
void vm_serial_set_cts(vm_serial_t *s, bool on);
bool vm_serial_get_dtr(const vm_serial_t *s);
bool vm_serial_get_rts(const vm_serial_t *s);

/* 実際に開いたポート名 (PTY の場合は /dev/pts/N) */
const char *vm_serial_name(const vm_serial_t *s);
const char *vm_serial_backend_name(const vm_serial_t *s);

/*
 * ---------------------------------------------------------------------------
 * LOOPBACK バックエンド専用: テスト用の「相手側」API
 * ---------------------------------------------------------------------------
 * peer_write は「PC (RAS) がモデムに送った」事を意味し、
 * vm_serial_read で読めるようになる。
 * peer_read は「モデムが PC に返した」ものを取り出す。
 */
int vm_serial_peer_write(vm_serial_t *s, const void *buf, int len);
int vm_serial_peer_read(vm_serial_t *s, void *buf, int len);
int vm_serial_peer_write_str(vm_serial_t *s, const char *str);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_SERIAL_H */
