/*
 * vm_at.h - Hayes AT コマンド インタプリタ
 *
 * VModem - Windows 用 本物のダイアルアップモデムエミュレータ
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * 設計方針: I/O を一切知らない純粋な状態機械にする
 * ===========================================================================
 * この層は「バイト列を食わせると、返すべきバイト列とやるべきアクションが
 * 出てくる」だけの関数にしてある。理由:
 *
 *   - シリアル I/O・オーディオ・PPP と混ぜると単体テストが書けなくなる
 *   - 逆に分離しておけば、AT コマンドの網羅テストが数十行で書ける
 *     (tests/test_at.c がまさにそれ)
 *
 * 呼び出し側 (src/modem/vm_modem.c) は
 *
 *     n = vm_serial_read(...);
 *     vm_at_feed(&at, buf, n);            <- 応答は at のレスポンスキューに溜まる
 *     while (vm_at_pop_response(&at, ...)) vm_serial_write(...);
 *     if (at.action == VM_AT_ACTION_DIAL) { ... 音響シーケンス開始 ... }
 *
 * という形で使う。
 *
 * ===========================================================================
 * Hayes コマンドセットの落とし穴
 * ===========================================================================
 * 実機互換のために押さえるべき点:
 *
 * 1. 行編集
 *    S3 (CR) で行が確定し、S5 (BS) で 1 文字消える。Windows RAS は BS を
 *    送ってこないが、ユーザが手で TeraTerm から叩く事を考えると必要。
 *
 * 2. エコー (ATE1 が既定)
 *    受け取った 1 文字をそのまま返す。RAS は ATE0 を送ってエコーを切る事が
 *    多いが、切る *前* の "ATE0" 自体はエコーしなければならない。
 *    -> エコー判定は「行を実行する前の状態」で行う必要がある。
 *
 * 3. 応答フォーマットは V0/V1 で全く違う
 *      V1 (既定, verbose): "\r\nOK\r\n"          <- 前後に CRLF
 *      V0 (数字):          "0\r"                 <- 後ろに CR のみ
 *    ここを間違えると RAS が応答を認識できず「モデムが応答しません」になる。
 *    実際の CRLF は S3/S4 レジスタの値を使う。
 *
 * 4. "AT" プレフィクスは 1 行に 1 回、コマンドは連結できる
 *      "ATE0V1&C1&D2S0=0" は 6 個のコマンドの連結。
 *    RAS の既定初期化文字列はまさにこの形なので、連結解析は必須。
 *
 * 5. ATDT の引数は行末まで全部が電話番号
 *    "ATDT0120-000-0000" の後にコマンドは続かない。
 *    ダイアル修飾子 (W 待ち, , ポーズ, ; トーン後コマンドモード復帰,
 *    T/P トーン/パルス指定, ! フック フラッシュ, @ 無音待ち) は
 *    番号から除去する (vm_number_normalize が数字以外を落とすので自動的)。
 *
 * 6. A/ (直前コマンドの再実行) は AT プレフィクス無しで即実行
 *    CR も要らない。特殊扱いが必要。
 *
 * 7. オンライン中の "+++" エスケープ
 *    前後に 1 秒以上の無通信 (ガードタイム) があり、かつ 1 秒以内に 3 個の
 *    '+' が並んだ場合のみコマンドモードに戻る。
 *    これを実装しないと、PPP のデータ中に偶然現れた "+++" で切断してしまう。
 *    ガードタイム判定のために vm_at_tick() で時間を渡してもらう。
 */
#ifndef VMODEM_VM_AT_H
#define VMODEM_VM_AT_H

#include "vmodem/vm_types.h"
#include "vmodem/vm_config.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define VM_AT_MAX_LINE      256   /* コマンド行の最大長 */
#define VM_AT_RESP_BYTES    4096  /* 応答キュー         */

/*
 * ---------------------------------------------------------------------------
 * 応答コード (V0 の数字応答と V1 の文字列応答の対応表)
 * ---------------------------------------------------------------------------
 */
typedef enum {
    VM_AT_RESULT_OK           = 0,
    VM_AT_RESULT_CONNECT      = 1,
    VM_AT_RESULT_RING         = 2,
    VM_AT_RESULT_NO_CARRIER   = 3,
    VM_AT_RESULT_ERROR        = 4,
    VM_AT_RESULT_NO_DIALTONE  = 6,
    VM_AT_RESULT_BUSY         = 7,
    VM_AT_RESULT_NO_ANSWER    = 8
} vm_at_result_t;

const char *vm_at_result_text(vm_at_result_t r);

/*
 * ---------------------------------------------------------------------------
 * 上位層に依頼するアクション
 * ---------------------------------------------------------------------------
 * AT ハンドラ自身は音を出したり回線を繋いだりしない。
 * 「これをやってくれ」というリクエストだけを立てる。
 */
typedef enum {
    VM_AT_ACTION_NONE = 0,
    VM_AT_ACTION_DIAL,        /* ATDT: 音響シーケンス開始要求          */
    VM_AT_ACTION_ANSWER,      /* ATA : 着信応答 (被呼側ハンドシェイク)  */
    VM_AT_ACTION_HANGUP,      /* ATH / DTR 落ち / +++ATH               */
    VM_AT_ACTION_RESET,       /* ATZ : 全設定初期化                    */
    VM_AT_ACTION_ONLINE       /* ATO : オンラインモードへ復帰           */
} vm_at_action_t;

/*
 * ---------------------------------------------------------------------------
 * S レジスタ
 * ---------------------------------------------------------------------------
 * 全 256 本持つ必要は無いが、RAS が S0/S7/S10 等を平然と書いてくるので
 * 配列で持ってしまうのが一番安全かつ簡単。
 */
#define VM_AT_NUM_SREG   256

typedef struct {
    /* --- 現在のモード --- */
    bool online;              /* true = データモード (PPP 中)         */
    bool echo;                /* ATE                                   */
    bool verbose;             /* ATV : true=文字列 false=数字          */
    bool quiet;               /* ATQ : true=応答を返さない             */
    bool speaker_on;          /* ATM : スピーカ (音を出すか)           */
    int  speaker_volume;      /* ATL : 0-3                             */

    /*
     * &C : DCD の扱い  0 = 常にオン / 1 = キャリアに追従 (既定)
     * &D : DTR の扱い  0 = 無視 / 2 = DTR 落ちでハングアップ (既定)
     * &K : フロー制御  0 = なし / 3 = RTS/CTS (既定)
     */
    int  dcd_mode;
    int  dtr_mode;
    int  flow_mode;

    uint8_t sreg[VM_AT_NUM_SREG];

    /* --- 接続要求 (config で選ばれた規格の上限) --- */
    vm_standard_t req_std;    /* AT+MS で指定された規格 */
    int           req_max_bps;
    int           req_min_bps;
    bool          req_automode;

    /* --- 行編集バッファ --- */
    char line[VM_AT_MAX_LINE];
    int  line_len;
    char last_line[VM_AT_MAX_LINE];   /* A/ 用 */

    /* --- 応答キュー (リングでなく単純な線形バッファ) --- */
    char resp[VM_AT_RESP_BYTES];
    int  resp_len;
    int  resp_pos;

    /* --- 上位層への依頼 --- */
    vm_at_action_t action;
    char           action_number[VM_MAX_NUMBER_LEN];  /* DIAL の番号 (原文) */

    /* --- +++ エスケープ検出 --- */
    int      plus_count;
    uint32_t last_rx_ms;      /* 最後にデータを受けた時刻       */
    uint32_t plus_first_ms;   /* 最初の '+' を受けた時刻         */
    bool     guard_ok;        /* 直前に 1 秒以上の無通信があった */
    uint32_t now_ms;          /* vm_at_tick() で更新             */

    /* --- 参照設定 (番号照合と AT+MS の応答生成に使う) --- */
    const vm_config_t *cfg;

    /*
     * オンライン中に受け取った「PPP へ渡すべき」データ。
     * vm_at_feed() は online のとき AT 解釈をせず、ここに素通しする。
     * (+++ 検出のためにスキャンだけはする)
     */
    const uint8_t *passthru;
    int            passthru_len;
} vm_at_t;

/* 初期化。cfg は寿命を borrow するだけ (コピーしない)。 */
void vm_at_init(vm_at_t *at, const vm_config_t *cfg);

/* ATZ 相当のソフトリセット (S レジスタと各モードを既定へ) */
void vm_at_reset(vm_at_t *at);

/*
 * 現在時刻 (ms 単位の単調増加値) を通知する。
 * +++ のガードタイム判定に必要。呼ばなくても動くが、
 * その場合 +++ エスケープは効かなくなる。
 */
void vm_at_tick(vm_at_t *at, uint32_t now_ms);

/*
 * DTE から受け取ったバイト列を投入する。
 *
 * コマンドモードなら:
 *   - エコーを resp に積む
 *   - CR で行を確定して実行し、応答を resp に積む
 *   - 必要なら at->action を立てる
 *
 * オンラインモードなら:
 *   - at->passthru / passthru_len に「PPP へ渡すべき範囲」を設定する
 *     (+++ を検出した場合はそこで打ち切り、action = HANGUP or NONE)
 *
 * 戻り値 = 消費したバイト数 (常に len)。
 */
int vm_at_feed(vm_at_t *at, const void *data, int len);

/*
 * 応答キューから取り出す。
 * 戻り値 = 取り出したバイト数 (0 = 空)。
 */
int vm_at_pop_response(vm_at_t *at, void *buf, int len);

/* 応答キューにデータがあるか */
bool vm_at_has_response(const vm_at_t *at);

/*
 * 上位層から応答を注入する。
 * 音響シーケンス完了後に "CONNECT 33600" を返す等に使う。
 */
void vm_at_emit_result(vm_at_t *at, vm_at_result_t r);
void vm_at_emit_connect(vm_at_t *at, int bps);
void vm_at_emit_raw(vm_at_t *at, const char *s);

/* action を消費 (取得してクリア) */
vm_at_action_t vm_at_take_action(vm_at_t *at);

/* オンライン/コマンドモードの切替 (上位層が接続完了/切断時に呼ぶ) */
void vm_at_set_online(vm_at_t *at, bool online);

/*
 * ---------------------------------------------------------------------------
 * ヘルパ: この AT 設定と ISP エントリから実際の接続速度を決める
 * ---------------------------------------------------------------------------
 * AT+MS で上限が指定されていればそれで頭打ちにする。
 * (実機同様「回線が許す最大速度」を返すのが自然な振る舞い)
 */
int vm_at_negotiated_bps(const vm_at_t *at, const vm_isp_entry_t *isp,
                         vm_standard_t *std_out);

#if defined(__cplusplus)
}
#endif
#endif /* VMODEM_VM_AT_H */
