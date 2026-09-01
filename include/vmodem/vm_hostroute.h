/*
 * vm_hostroute.h - ホスト側の「本物のインターネット経路」を突き止める
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ===========================================================================
 * ★★★ このファイルが存在する理由 = 「繋がらない」の真の原因 ★★★
 * ===========================================================================
 *
 * 【症状】
 *   nat tx: 192.168.99.2 > 8.8.8.8 UDP 69B   (大量に出ている)
 *   nat rx: 192.168.99.1 > 192.168.99.2 ICMP 97B (毎回これが返る)
 *   8.8.8.8 からの応答は皆無。TCP も同様。
 *
 * 【この 97 バイトが犯人を特定する】
 *   libslirp の icmp_forward_error() が作る ICMP エラーの大きさは
 *
 *       IP ヘッダ(20) + ICMP ヘッダ(8) + 元パケットの先頭 s_ip_len
 *
 *   で、s_ip_len = min(元の IP 全長, ICMP_MAXDATALEN=548)。
 *   元パケットが 69B なので 20 + 8 + 69 = **97**。ログと完全に一致する。
 *
 *   では誰がこの ICMP を作ったのか。libslirp で UDP に対して
 *   ICMP_UNREACH を投げる箇所は 2 つしかない:
 *
 *     (A) src/udp.c:233     sosendto() が -1 を返した時   ← 送信の失敗
 *     (B) src/socket.c:687  recvfrom() が -1 を返した時   ← 受信の失敗
 *
 *   (B) は「応答が返ってきたが読めなかった」場合に起きる。しかし
 *   8.8.8.8 からの応答は一切来ていないのだから、recvfrom が呼ばれる
 *   きっかけ (poll の POLLIN) 自体が発生しない。よって (B) ではない。
 *
 *   したがって **(A) = sendto() が同期的に失敗している**。
 *   ゲストが送るたびに即座に ICMP が返るという観測とも符合する
 *   (待ち時間ゼロで返っている)。
 *
 * 【なぜ sendto() が失敗するのか】
 *   Windows RAS はダイアルアップ接続が確立すると、既定で
 *   「リモート ネットワークでデフォルト ゲートウェイを使う」が有効になり、
 *
 *       0.0.0.0/0 -> 192.168.99.1 (我々の PPP インタフェース)
 *
 *   という **メトリックの低いデフォルト経路**をホストの経路表に追加する。
 *   これは VPN クライアントと全く同じ挙動である。
 *
 *   すると次の自己参照ループが完成する:
 *
 *       libslirp が sendto(8.8.8.8) を呼ぶ
 *            ↓  OS が経路検索 → 最良経路は PPP インタフェース
 *       パケットが RAS 経由で我々自身の PPP に戻ってくる
 *            ↓
 *       vm_ppp → vm_nat → libslirp → sendto(8.8.8.8) …(無限)
 *
 *   実際には Windows のスタックがこのループを検出し、
 *   sendto() に対して WSAENETUNREACH (10051) / WSAEHOSTUNREACH (10065) /
 *   WSAEADDRNOTAVAIL (10049) のいずれかを即座に返して打ち切る。
 *   libslirp の util.c はこれらを ENETUNREACH / EHOSTUNREACH へ翻訳し、
 *   udp.c:233 が ICMP_UNREACH を生成する。→ 観測された 97B の ICMP。
 *
 *   「nat tx は出ているのに応答が皆無」「ICMP だけが即座に返る」という
 *   一見矛盾した症状は、これで完全に説明がつく。
 *   パケットは一度も NIC から外に出ていない。
 *
 * 【なぜ WSAStartup や WSAPoll の修正だけでは直らないか】
 *   それらは確かにバグだが、症状が違う:
 *     - WSAStartup 未呼び出し → socket() 自体が失敗し nat tx すら出ない
 *     - WSAPoll の events 不正 → *受信* が止まる。送信は成功するので
 *       ICMP は返らず「無応答のまま沈黙」する
 *   今回は「送信が同期的に失敗して即 ICMP」なので、送信経路の問題である。
 *   (もちろん両方とも直す。直さないと次の壁になる)
 *
 * 【対策】
 *   libslirp が外向きに使うソケットを、**ホストの実 NIC のアドレスに
 *   bind** する。SlirpConfig.outbound_addr (version >= 2) がこのためにある。
 *   slirp4netns の --outbound-addr と同じ仕組みで、
 *   実装は src/misc.c の slirp_bind_outbound() → bind(so->s, addr)。
 *
 *   Windows は Vista 以降 IPv4 で **strong host model** が既定なので、
 *   送信元アドレスを bind で固定すると、そのアドレスを持つ
 *   インタフェースから送出される。結果 PPP 経路は選ばれず、
 *   ループが物理的に成立しなくなる。
 *
 *   バインドするアドレスは「PPP を除いた実 NIC」でなければならない。
 *   これを人間に設定させるのは酷なので、本モジュールが自動検出する。
 *
 * 【検出方法】
 *   GetBestRoute(8.8.8.8) で最良経路を引く…だけでは駄目である。
 *   RAS が既にデフォルト経路を奪った後に呼ぶと、PPP 自身が返ってきて
 *   自分の首を絞める。よって:
 *
 *     1. PPP 接続が確立する **前** (= vm_nat_create 時点) に検出する。
 *        この時点ではまだ RAS の経路は無いので、GetBestRoute は
 *        正しく実 NIC を返す。
 *     2. 加えて GetAdaptersAddresses で当該アドレスの種別を検査し、
 *        PPP / TUNNEL / LOOPBACK なら採用しない (二重の保険)。
 *
 *   検出できなければ 0 を返す。呼び出し側は outbound_addr を設定せず
 *   従来動作にフォールバックする (設定を強制して壊すより良い)。
 * ===========================================================================
 */
#ifndef VMODEM_VM_HOSTROUTE_H
#define VMODEM_VM_HOSTROUTE_H

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * インターネットへ出るために使うべきホスト側 IPv4 アドレスを返す。
 *
 * 戻り値: ホストオーダの IPv4 アドレス。検出できなければ 0。
 *
 * 引数:
 *   exclude_net  / exclude_mask
 *       仮想ネットワーク (例 192.168.99.0/24)。この範囲のアドレスは
 *       我々自身の PPP 側なので絶対に選ばない。
 *   name_out / name_size
 *       採用したアダプタの説明を書き込む (NULL 可)。ログ用。
 *
 * ★呼び出しタイミングが極めて重要★
 *   RAS がデフォルト経路を書き換える前 (= ダイアルアップ確立前) に
 *   呼ぶこと。確立後に呼ぶと PPP 自身を検出してしまう。
 */
uint32_t vm_hostroute_pick_outbound_ip(uint32_t exclude_net,
                                       uint32_t exclude_mask,
                                       char *name_out, size_t name_size);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* VMODEM_VM_HOSTROUTE_H */
