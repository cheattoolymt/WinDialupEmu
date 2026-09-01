/*
 * vm_hostroute.c - ホスト側の実インターネット経路の検出
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * 設計の背景 (なぜこれが「繋がらない」の本命なのか) は
 * include/vmodem/vm_hostroute.h 冒頭を熟読すること。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>     /* malloc / free                       */
#include <stdbool.h>    /* bool (C99)                          */

/* ★順序厳守★ winsock2.h を windows.h より先に通す */
#include "vmodem/vm_winsock.h"

#include "vmodem/vm_hostroute.h"
#include "vmodem/vm_log.h"

#if defined(_WIN32)

/*
 * iphlpapi.h は winsock2.h の後でなければならない。
 * (内部で IP_ADAPTER_ADDRESSES が sockaddr を使うため)
 */
#include <iphlpapi.h>

/* --------------------------------------------------------------------------
 * ヘルパ
 * -------------------------------------------------------------------------- */
static void ip_to_str(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),
             (unsigned)(ip & 0xFFu));
}

/*
 * 「外に出るのに使えないアドレス」を弾く。
 *   0.0.0.0        未設定
 *   127.0.0.0/8    ループバック
 *   169.254.0.0/16 APIPA (DHCP 失敗時の自動設定。経路が無い)
 *   224.0.0.0/4    マルチキャスト
 *   255.255.255.255 ブロードキャスト
 */
static bool addr_is_usable(uint32_t ip)
{
    if (ip == 0u || ip == 0xFFFFFFFFu)
        return false;
    if ((ip & 0xFF000000u) == 0x7F000000u)      /* 127/8   */
        return false;
    if ((ip & 0xFFFF0000u) == 0xA9FE0000u)      /* 169.254/16 */
        return false;
    if ((ip & 0xF0000000u) == 0xE0000000u)      /* 224/4   */
        return false;
    return true;
}

/*
 * このアダプタ種別は「本物の外向き NIC」か。
 *
 * ★ここで PPP を弾くのが肝★
 * RAS のダイアルアップは IF_TYPE_PPP として現れる。これを選んでしまうと
 * 自分自身に bind することになり、状況が改善しないどころか悪化する。
 * トンネル (VPN) も同様に避ける。
 */
static bool iftype_is_real_nic(DWORD iftype)
{
    switch (iftype) {
    case IF_TYPE_ETHERNET_CSMACD:   /* 有線 LAN            */
    case IF_TYPE_IEEE80211:         /* 無線 LAN            */
    case IF_TYPE_IEEE1394:          /* FireWire            */
        return true;
    case IF_TYPE_PPP:               /* ← まさに我々の RAS  */
    case IF_TYPE_SOFTWARE_LOOPBACK:
    case IF_TYPE_TUNNEL:            /* VPN                 */
    default:
        return false;
    }
}

/* --------------------------------------------------------------------------
 * GetAdaptersAddresses で「実 NIC のユニキャスト IPv4」を列挙し、
 * 条件に合うものを選ぶ。
 * --------------------------------------------------------------------------
 * want_ip != 0 の場合は「そのアドレスが実 NIC のものか検証する」用途、
 * want_ip == 0 の場合は「使えるものを 1 つ選ぶ」用途 (フォールバック)。
 */
static uint32_t scan_adapters(uint32_t want_ip,
                              uint32_t exclude_net, uint32_t exclude_mask,
                              char *name_out, size_t name_size)
{
    IP_ADAPTER_ADDRESSES *buf = NULL;
    IP_ADAPTER_ADDRESSES *ad;
    ULONG                 size = 16384;
    ULONG                 ret;
    uint32_t              found = 0;
    int                   tries;

    /*
     * GetAdaptersAddresses はバッファ不足だと ERROR_BUFFER_OVERFLOW を
     * 返し、必要サイズを size に書き戻す。アダプタは動的に増減しうるので
     * 数回リトライする (MSDN 推奨のパターン)。
     */
    for (tries = 0; tries < 3; tries++) {
        buf = (IP_ADAPTER_ADDRESSES *)malloc(size);
        if (buf == NULL)
            return 0;

        ret = GetAdaptersAddresses(AF_INET,
                                   GAA_FLAG_SKIP_ANYCAST |
                                   GAA_FLAG_SKIP_MULTICAST |
                                   GAA_FLAG_SKIP_DNS_SERVER,
                                   NULL, buf, &size);
        if (ret == ERROR_SUCCESS)
            break;

        free(buf);
        buf = NULL;

        if (ret != ERROR_BUFFER_OVERFLOW)
            return 0;
        /* size は書き換わっているので次のループで使う */
    }

    if (buf == NULL)
        return 0;

    for (ad = buf; ad != NULL && found == 0; ad = ad->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *ua;

        /* 稼働していないアダプタは論外 */
        if (ad->OperStatus != IfOperStatusUp)
            continue;

        if (!iftype_is_real_nic(ad->IfType))
            continue;

        for (ua = ad->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
            struct sockaddr_in *sin;
            uint32_t            ip;

            if (ua->Address.lpSockaddr == NULL ||
                ua->Address.lpSockaddr->sa_family != AF_INET)
                continue;

            sin = (struct sockaddr_in *)ua->Address.lpSockaddr;
            ip  = ntohl(sin->sin_addr.s_addr);

            if (!addr_is_usable(ip))
                continue;

            /* 仮想ネットワーク (192.168.99.0/24) は我々自身 */
            if (exclude_mask != 0u &&
                (ip & exclude_mask) == (exclude_net & exclude_mask))
                continue;

            if (want_ip != 0u && ip != want_ip)
                continue;

            found = ip;

            if (name_out != NULL && name_size > 0) {
                /*
                 * FriendlyName は WCHAR。%ls は MSVCRT でも動くが、
                 * ロケール依存で化けることがあるので WideCharToMultiByte で
                 * UTF-8 に明示変換する。
                 */
                if (ad->FriendlyName != NULL) {
                    int n = WideCharToMultiByte(CP_UTF8, 0, ad->FriendlyName,
                                                -1, name_out, (int)name_size,
                                                NULL, NULL);
                    if (n <= 0)
                        snprintf(name_out, name_size, "(名前取得失敗)");
                } else {
                    snprintf(name_out, name_size, "(名前なし)");
                }
            }
            break;
        }
    }

    free(buf);
    return found;
}

/* --------------------------------------------------------------------------
 * 本体
 * -------------------------------------------------------------------------- */
uint32_t vm_hostroute_pick_outbound_ip(uint32_t exclude_net,
                                       uint32_t exclude_mask,
                                       char *name_out, size_t name_size)
{
    MIB_IPFORWARDROW route;
    uint32_t         best = 0;
    uint32_t         verified = 0;
    char             a[16];

    if (name_out != NULL && name_size > 0)
        name_out[0] = '\0';

    /*
     * --- 手段 1: GetBestRoute で「8.8.8.8 に行くならどの経路か」を聞く ---
     *
     * 8.8.8.8 を代表アドレスに使う。実際にそこへ通信する訳ではなく、
     * 「グローバルなアドレスへ出る経路」を OS に問い合わせるだけ。
     *
     * ★この関数は RAS 接続 *前* に呼ばれる前提★
     *   接続後だと RAS のデフォルト経路が勝ち、PPP 自身が返ってくる。
     *   その保険として下で必ずアダプタ種別を検証する。
     */
    memset(&route, 0, sizeof(route));
    if (GetBestRoute(htonl(0x08080808u), 0, &route) == NO_ERROR) {
        best = ntohl(route.dwForwardNextHop);

        /*
         * NextHop はゲートウェイのアドレスであって我々の送信元ではない。
         * 欲しいのは「その経路を持つインタフェースに割り当てられた
         * ローカルアドレス」なので、素直に使ってはいけない。
         * ここでは経路の存在確認だけに使い、実アドレスは下で探す。
         */
        ip_to_str(best, a, sizeof(a));
        VM_LOGD("hostroute: GetBestRoute(8.8.8.8) -> nexthop=%s ifindex=%lu",
                a, (unsigned long)route.dwForwardIfIndex);

        /*
         * 経路のインタフェース index に一致する実 NIC のアドレスを探す。
         * GetAdaptersAddresses の IfIndex と dwForwardIfIndex は
         * 同じ名前空間なので突き合わせできる。
         */
        {
            IP_ADAPTER_ADDRESSES *buf = NULL;
            IP_ADAPTER_ADDRESSES *ad;
            ULONG                 size = 16384;
            ULONG                 ret;
            int                   tries;

            for (tries = 0; tries < 3; tries++) {
                buf = (IP_ADAPTER_ADDRESSES *)malloc(size);
                if (buf == NULL)
                    break;
                ret = GetAdaptersAddresses(AF_INET,
                                           GAA_FLAG_SKIP_ANYCAST |
                                           GAA_FLAG_SKIP_MULTICAST |
                                           GAA_FLAG_SKIP_DNS_SERVER,
                                           NULL, buf, &size);
                if (ret == ERROR_SUCCESS)
                    break;
                free(buf);
                buf = NULL;
                if (ret != ERROR_BUFFER_OVERFLOW)
                    break;
            }

            for (ad = buf; ad != NULL && verified == 0; ad = ad->Next) {
                IP_ADAPTER_UNICAST_ADDRESS *ua;

                if (ad->IfIndex != route.dwForwardIfIndex)
                    continue;
                if (ad->OperStatus != IfOperStatusUp)
                    continue;

                /*
                 * ★二重の保険★
                 * 経路が指すのが PPP / トンネルなら、それは RAS が既に
                 * デフォルト経路を奪った後という事。採用してはいけない。
                 */
                if (!iftype_is_real_nic(ad->IfType)) {
                    VM_LOGW("hostroute: 最良経路が実 NIC ではない "
                            "(IfType=%lu)。RAS が既にデフォルト経路を "
                            "奪っている可能性があるため採用しない",
                            (unsigned long)ad->IfType);
                    continue;
                }

                for (ua = ad->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
                    struct sockaddr_in *sin;
                    uint32_t            ip;

                    if (ua->Address.lpSockaddr == NULL ||
                        ua->Address.lpSockaddr->sa_family != AF_INET)
                        continue;

                    sin = (struct sockaddr_in *)ua->Address.lpSockaddr;
                    ip  = ntohl(sin->sin_addr.s_addr);

                    if (!addr_is_usable(ip))
                        continue;
                    if (exclude_mask != 0u &&
                        (ip & exclude_mask) == (exclude_net & exclude_mask))
                        continue;

                    verified = ip;
                    if (name_out != NULL && name_size > 0 &&
                        ad->FriendlyName != NULL) {
                        int n = WideCharToMultiByte(CP_UTF8, 0,
                                                    ad->FriendlyName, -1,
                                                    name_out, (int)name_size,
                                                    NULL, NULL);
                        if (n <= 0)
                            snprintf(name_out, name_size, "(名前取得失敗)");
                    }
                    break;
                }
            }

            if (buf != NULL)
                free(buf);
        }
    } else {
        VM_LOGD("hostroute: GetBestRoute が失敗 (経路表を読めない)");
    }

    if (verified != 0u) {
        ip_to_str(verified, a, sizeof(a));
        VM_LOGI("hostroute: 外向きアドレスに %s (%s) を採用", a,
                (name_out != NULL && name_out[0]) ? name_out : "?");
        return verified;
    }

    /*
     * --- 手段 2: 経路から特定できなかった場合の総当たり ---
     * 稼働中の実 NIC で、仮想ネットワーク外の使えるアドレスを 1 つ選ぶ。
     */
    verified = scan_adapters(0, exclude_net, exclude_mask,
                             name_out, name_size);
    if (verified != 0u) {
        ip_to_str(verified, a, sizeof(a));
        VM_LOGI("hostroute: 外向きアドレスに %s (%s) を採用 "
                "(経路表からは決められなかったのでアダプタ列挙で選択)",
                a, (name_out != NULL && name_out[0]) ? name_out : "?");
        return verified;
    }

    VM_LOGW("hostroute: 外向きに使えるホストアドレスを特定できなかった。"
            "outbound_addr を設定せずに続行する "
            "(RAS がデフォルト経路を奪うと通信できない恐れがある)");
    return 0;
}

#else /* !_WIN32 */

uint32_t vm_hostroute_pick_outbound_ip(uint32_t exclude_net,
                                       uint32_t exclude_mask,
                                       char *name_out, size_t name_size)
{
    /*
     * POSIX ではこの問題は起きない。
     * Linux は weak host model で、かつ本プロジェクトの想定環境は
     * Windows なので、何もせず 0 (= outbound_addr を使わない) を返す。
     */
    (void)exclude_net; (void)exclude_mask;
    if (name_out != NULL && name_size > 0)
        name_out[0] = '\0';
    return 0;
}

#endif /* _WIN32 */
