/*
 * vm_nat_internal.h - NAT バックエンド共通の内部定義
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 共通部 (偽 Ethernet 層、統計、バックエンド振り分け) を vm_nat.c に置き、
 * バックエンド固有部は以下のファイルが vtable を埋める形で提供する。
 *
 *   vm_nat_slirp.c     libslirp (VMODEM_HAVE_LIBSLIRP 時のみ実体を持つ)
 *   vm_nat_loopback.c  内蔵の最小応答器 (ICMP Echo / UDP Echo)
 */
#ifndef VMODEM_VM_NAT_INTERNAL_H
#define VMODEM_VM_NAT_INTERNAL_H

#include "vmodem/vm_nat.h"
#include "vmodem/vm_eth.h"
#include "vmodem/vm_log.h"

/* PPP MRU 1500 + Ethernet 14 + 余裕 */
#define VM_NAT_FRAME_MAX 2048

/* --------------------------------------------------------------------------
 * バックエンド vtable
 * --------------------------------------------------------------------------
 * open/close は必須。send_frame も必須。poll は NULL 可 (何もしない)。
 * link_up / link_down は NULL 可。
 */
typedef struct vm_nat_backend_ops_s {
    const char *name;

    /* 生成。失敗時は vm_err_t を返す。成功時 n->impl を埋める。 */
    vm_err_t (*open)(vm_nat_t *n);

    /* 破棄。open が成功していた場合のみ呼ばれる。 */
    void (*close)(vm_nat_t *n);

    /*
     * ゲストから来た Ethernet フレームをバックエンドへ渡す。
     * (libslirp なら slirp_input())
     */
    vm_err_t (*send_frame)(vm_nat_t *n, const uint8_t *frame, int len);

    /*
     * ソケットのポーリング。戻り値は次回までの推奨待ち時間 (ms)。
     * バックエンドからゲストへ届くフレームは vm_nat_backend_recv_frame()
     * を呼んで通知する。
     */
    int (*poll)(vm_nat_t *n, int max_block_ms);

    void (*link_up)(vm_nat_t *n);
    void (*link_down)(vm_nat_t *n);
} vm_nat_backend_ops_t;

/* --------------------------------------------------------------------------
 * NAT インスタンス
 * -------------------------------------------------------------------------- */
struct vm_nat_s {
    vm_nat_cfg_t                cfg;
    vm_nat_backend_t            backend;    /* 実際に開けたバックエンド */
    const vm_nat_backend_ops_t *ops;
    void                       *impl;       /* バックエンド固有状態 */

    vm_eth_t                    eth;        /* 偽イーサネット層 */
    bool                        link_up;

    vm_nat_ip_cb                ip_cb;
    void                       *ip_user;

    vm_nat_stats_t              stats;

    /* 作業バッファ。単一スレッド前提なのでインスタンスに持たせる。 */
    uint8_t                     fbuf[VM_NAT_FRAME_MAX];  /* encap 用 */
    uint8_t                     pbuf[VM_NAT_FRAME_MAX];  /* decap 用 */
};

/* --------------------------------------------------------------------------
 * バックエンドから共通部へ: フレームを受け取った
 * --------------------------------------------------------------------------
 * 偽 Ethernet 層で剥がし、IPv4 なら ip_cb へ、ARP 応答が必要なら
 * バックエンドへ送り返す。バックエンド側でこの処理を書く必要はない。
 */
void vm_nat_backend_recv_frame(vm_nat_t *n, const uint8_t *frame, int len);

/* バックエンドからのゲストエラー報告 */
void vm_nat_backend_guest_error(vm_nat_t *n, const char *msg);

/* --------------------------------------------------------------------------
 * 各バックエンドの ops 取得子。
 * 利用不可なら NULL を返す。
 * -------------------------------------------------------------------------- */
const vm_nat_backend_ops_t *vm_nat_ops_slirp(void);
const vm_nat_backend_ops_t *vm_nat_ops_loopback(void);

#endif /* VMODEM_VM_NAT_INTERNAL_H */
