/*
 * vm_log.h - ログ出力
 *
 * 設計方針:
 *   - オーディオレンダースレッド (WASAPI) からは呼ばない事を前提とする。
 *     WASAPI のコールバック相当区間で fprintf / ロック取得を行うと
 *     グリッチ (音切れ) の原因になるため。デバッグ用に必要な場合は
 *     VM_LOGT (trace) を使い、既定のログレベルでは無効化する。
 *   - スレッド安全性: 出力の直列化のみクリティカルセクションで保護する。
 */
#ifndef VMODEM_VM_LOG_H
#define VMODEM_VM_LOG_H

#include "vmodem/vm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VM_LOG_NONE  = 0,
    VM_LOG_ERROR = 1,
    VM_LOG_WARN  = 2,
    VM_LOG_INFO  = 3,
    VM_LOG_DEBUG = 4,
    VM_LOG_TRACE = 5
} vm_log_level_t;

/* 初期化 / 終了。path==NULL または "" ならファイル出力なし (stderr のみ) */
vm_err_t vm_log_init(vm_log_level_t level, const char *path);
void     vm_log_shutdown(void);

void vm_log_set_level(vm_log_level_t level);
vm_log_level_t vm_log_get_level(void);

/* 実際の出力。通常はマクロ経由で呼ぶ */
void vm_log_write(vm_log_level_t level, const char *file, int line,
                  const char *fmt, ...);

/* 16 進ダンプ (AT コマンド / PPP フレームのデバッグ用) */
void vm_log_hexdump(vm_log_level_t level, const char *tag,
                    const void *data, size_t len);

#define VM_LOGE(...) vm_log_write(VM_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define VM_LOGW(...) vm_log_write(VM_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define VM_LOGI(...) vm_log_write(VM_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define VM_LOGD(...) vm_log_write(VM_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define VM_LOGT(...) vm_log_write(VM_LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* VMODEM_VM_LOG_H */
