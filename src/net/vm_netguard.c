/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdlib.h>
#include "vmodem/vm_winsock.h"
#include "vmodem/vm_netguard.h"
#include "vmodem/vm_log.h"

#ifdef _WIN32
#include <fwpmu.h>
#include <ipifcons.h>
#include <objbase.h>
#include "vm_wfp_guids.h"
#ifndef FWPM_SESSION_FLAG_DYNAMIC
#define FWPM_SESSION_FLAG_DYNAMIC 0x00000001
#endif

struct vm_netguard_s { HANDLE engine; };

static DWORD add_filter(HANDLE engine, const GUID *sublayer, const GUID *layer,
                        FWP_BYTE_BLOB *app, UINT32 iftype, bool dhcp, bool ipv6)
{
    FWPM_FILTER0 filter = {0};
    FWPM_FILTER_CONDITION0 c[4] = {0};
    c[0].fieldKey = VM_FWPM_CONDITION_INTERFACE_TYPE;
    c[0].matchType = FWP_MATCH_EQUAL;
    c[0].conditionValue.type = FWP_UINT32;
    c[0].conditionValue.uint32 = iftype;
    if (dhcp) {
        /* Keep DHCP leases alive. This does not permit ordinary application
         * traffic. Higher weight wins within our own sublayer only. */
        c[1].fieldKey = VM_FWPM_CONDITION_IP_PROTOCOL;
        c[1].matchType = FWP_MATCH_EQUAL;
        c[1].conditionValue.type = FWP_UINT8;
        c[1].conditionValue.uint8 = IPPROTO_UDP;
        c[2].fieldKey = VM_FWPM_CONDITION_IP_LOCAL_PORT;
        c[2].matchType = FWP_MATCH_EQUAL;
        c[2].conditionValue.type = FWP_UINT16;
        c[2].conditionValue.uint16 = ipv6 ? 546 : 68;
        c[3].fieldKey = VM_FWPM_CONDITION_IP_REMOTE_PORT;
        c[3].matchType = FWP_MATCH_EQUAL;
        c[3].conditionValue.type = FWP_UINT16;
        c[3].conditionValue.uint16 = ipv6 ? 547 : 67;
        filter.numFilterConditions = 4;
    } else {
        c[1].fieldKey = VM_FWPM_CONDITION_ALE_APP_ID;
        c[1].matchType = FWP_MATCH_NOT_EQUAL;
        c[1].conditionValue.type = FWP_BYTE_BLOB_TYPE;
        c[1].conditionValue.byteBlob = app;
        /* Localhost must remain usable even when its address belongs to a NIC. */
        c[2].fieldKey = VM_FWPM_CONDITION_FLAGS;
        c[2].matchType = FWP_MATCH_FLAGS_NONE_SET;
        c[2].conditionValue.type = FWP_UINT32;
        c[2].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;
        filter.numFilterConditions = 3;
    }
    filter.displayData.name = dhcp ? L"VModem DHCP maintenance" : L"VModem dial-up only";
    filter.layerKey = *layer;
    filter.subLayerKey = *sublayer;
    filter.weight.type = FWP_UINT8;
    filter.weight.uint8 = dhcp ? 2 : 1;
    filter.action.type = dhcp ? FWP_ACTION_PERMIT : FWP_ACTION_BLOCK;
    filter.filterCondition = c;
    return FwpmFilterAdd0(engine, &filter, NULL, NULL);
}

vm_err_t vm_netguard_open(vm_netguard_t **out)
{
    static const GUID *const layers[] = {
        &VM_FWPM_LAYER_ALE_AUTH_CONNECT_V4, &VM_FWPM_LAYER_ALE_AUTH_CONNECT_V6,
        &VM_FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, &VM_FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6
    };
    static const UINT32 types[] = {IF_TYPE_ETHERNET_CSMACD, IF_TYPE_IEEE80211};
    FWPM_SESSION0 session = {0};
    FWPM_SUBLAYER0 sublayer = {0};
    FWP_BYTE_BLOB *app = NULL;
    WCHAR path[32768];
    vm_netguard_t *guard;
    DWORD rc, count;
    size_t i, j;
    bool transaction = false;

    if (!out) return VM_ERR_INVAL;
    *out = NULL;
    guard = calloc(1, sizeof(*guard));
    if (!guard) return VM_ERR_NOMEM;
    count = GetModuleFileNameW(NULL, path, 32768);
    if (!count || count >= 32768) {
        rc = ERROR_INSUFFICIENT_BUFFER;
        goto fail;
    }
    rc = FwpmGetAppIdFromFileName0(path, &app);
    if (rc != ERROR_SUCCESS) goto fail;
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    session.displayData.name = L"VModem temporary network isolation";
    rc = FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, &session, &guard->engine);
    if (rc != ERROR_SUCCESS) goto fail;
    rc = FwpmTransactionBegin0(guard->engine, 0);
    if (rc != ERROR_SUCCESS) goto fail;
    transaction = true;
    if (FAILED(CoCreateGuid(&sublayer.subLayerKey))) {
        rc = ERROR_GEN_FAILURE;
        goto fail;
    }
    sublayer.displayData.name = L"VModem temporary network isolation";
    sublayer.weight = 0x8000;
    rc = FwpmSubLayerAdd0(guard->engine, &sublayer, NULL);
    if (rc != ERROR_SUCCESS) goto fail;
    for (i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i) {
        for (j = 0; j < sizeof(types) / sizeof(types[0]); ++j) {
            rc = add_filter(guard->engine, &sublayer.subLayerKey, layers[i],
                            app, types[j], false, (i % 2) != 0);
            if (rc != ERROR_SUCCESS) goto fail;
            rc = add_filter(guard->engine, &sublayer.subLayerKey, layers[i],
                            app, types[j], true, (i % 2) != 0);
            if (rc != ERROR_SUCCESS) goto fail;
        }
    }
    rc = FwpmTransactionCommit0(guard->engine);
    if (rc != ERROR_SUCCESS) goto fail;
    FwpmFreeMemory0((void **)&app);
    *out = guard;
    VM_LOGI("netguard: LAN/Wi-Fi は VModem 専用。通常の通信には RAS 接続が必要です");
    return VM_OK;
fail:
    VM_LOGE("netguard: WFP 初期化失敗 (error=%lu)。管理者として起動し、"
            "Base Filtering Engine サービスを確認してください", (unsigned long)rc);
    if (transaction) FwpmTransactionAbort0(guard->engine);
    if (app) FwpmFreeMemory0((void **)&app);
    vm_netguard_close(guard);
    return VM_ERR_IO;
}

void vm_netguard_close(vm_netguard_t *guard)
{
    if (!guard) return;
    if (guard->engine) {
        DWORD rc = FwpmEngineClose0(guard->engine);
        if (rc != ERROR_SUCCESS)
            VM_LOGE("netguard: WFP 終了失敗 (error=%lu)", (unsigned long)rc);
        else
            VM_LOGI("netguard: LAN/Wi-Fi の通常利用を復元しました");
    }
    free(guard);
}
#else
struct vm_netguard_s { int unused; };
vm_err_t vm_netguard_open(vm_netguard_t **out)
{
    if (!out) return VM_ERR_INVAL;
    *out = NULL;
    return VM_ERR_UNSUPPORTED;
}
void vm_netguard_close(vm_netguard_t *guard) { free(guard); }
#endif
