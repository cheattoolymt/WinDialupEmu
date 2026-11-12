/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdlib.h>
#include "vmodem/vm_winsock.h"
#include "vmodem/vm_hostroute.h"
#include "vmodem/vm_log.h"
#ifdef _WIN32
#include <iphlpapi.h>
/* RAS can leave the physical /0 route visible but mark it unusable.
 * Preserve the pre-dial value and use the stable LUID when writing back.
 * Never change metrics, DNS, weak-host settings or other adapters. */
struct vm_hostroute_guard_s {
    NET_LUID luid;
    BOOLEAN original_disabled;
    bool changed;
};
static DWORD guard_read(vm_hostroute_guard_t *g, MIB_IPINTERFACE_ROW *row)
{
    InitializeIpInterfaceEntry(row);
    row->Family = AF_INET;
    row->InterfaceLuid = g->luid;
    return GetIpInterfaceEntry(row);
}
vm_err_t vm_hostroute_guard_open(vm_hostroute_guard_t **out, uint32_t index)
{
    MIB_IPINTERFACE_ROW row;
    vm_hostroute_guard_t *g;
    DWORD rc;
    *out = NULL;
    InitializeIpInterfaceEntry(&row);
    row.Family = AF_INET;
    row.InterfaceIndex = index;
    rc = GetIpInterfaceEntry(&row);
    if (rc != NO_ERROR) {
        VM_LOGE("hostroute: initial interface query failed (%lu)", (unsigned long)rc);
        return VM_ERR_IO;
    }
    VM_LOGI("hostroute: initial DisableDefaultRoutes=%u ifindex=%lu",
            (unsigned)row.DisableDefaultRoutes, (unsigned long)index);
    if (row.DisableDefaultRoutes) {
        VM_LOGE("hostroute: selected uplink already ignores default routes; refusing to override pre-existing policy");
        return VM_ERR_STATE;
    }
    g = calloc(1, sizeof(*g));
    if (!g) return VM_ERR_NOMEM;
    g->luid = row.InterfaceLuid;
    g->original_disabled = row.DisableDefaultRoutes;
    *out = g;
    return VM_OK;
}
vm_err_t vm_hostroute_guard_ensure(vm_hostroute_guard_t *g)
{
    MIB_IPINTERFACE_ROW row;
    DWORD rc;
    if (!g) return VM_ERR_INVAL;
    rc = guard_read(g, &row);
    if (rc == NO_ERROR && row.DisableDefaultRoutes != g->original_disabled) {
        row.DisableDefaultRoutes = g->original_disabled;
        row.SitePrefixLength = 0; /* Required by SetIpInterfaceEntry for IPv4. */
        rc = SetIpInterfaceEntry(&row);
        if (rc == NO_ERROR) {
            g->changed = true;
            VM_LOGI("hostroute: restored physical default-route eligibility after RAS change (ifindex=%lu)",
                    (unsigned long)row.InterfaceIndex);
        }
    }
    if (rc != NO_ERROR) {
        VM_LOGE("hostroute: restoring physical route eligibility failed (%lu)", (unsigned long)rc);
        return VM_ERR_IO;
    }
    return VM_OK;
}
void vm_hostroute_guard_close(vm_hostroute_guard_t *g)
{
    if (!g) return;
    if (g->changed && vm_hostroute_guard_ensure(g) != VM_OK)
        VM_LOGE("hostroute: cleanup failed; disconnect RAS and check IgnoreDefaultRoutes on the uplink");
    free(g);
}

#else
struct vm_hostroute_guard_s { int unused; };
vm_err_t vm_hostroute_guard_open(vm_hostroute_guard_t **out, uint32_t index)
{ (void)index; *out = NULL; return VM_ERR_UNSUPPORTED; }
vm_err_t vm_hostroute_guard_ensure(vm_hostroute_guard_t *g)
{ (void)g; return VM_ERR_UNSUPPORTED; }
void vm_hostroute_guard_close(vm_hostroute_guard_t *g) { free(g); }

#endif
