/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vmodem/vm_winsock.h"
#include <iphlpapi.h>
#include "vmodem/vm_hostroute.h"
#include "vmodem/vm_log.h"
static MIB_IPINTERFACE_ROW current;
static DWORD read_error, write_error;
static int writes;
void InitializeIpInterfaceEntry(MIB_IPINTERFACE_ROW *r) { memset(r,0,sizeof(*r)); }
DWORD GetIpInterfaceEntry(MIB_IPINTERFACE_ROW *r) {
    assert(r->Family==AF_INET);
    assert(r->InterfaceIndex==18 || r->InterfaceLuid.Value==1234);
    if(read_error)return read_error;
    *r=current; return 0;
}
DWORD SetIpInterfaceEntry(MIB_IPINTERFACE_ROW *r) {
    assert(r->InterfaceLuid.Value==1234 && r->SitePrefixLength==0);
    assert(r->Metric==current.Metric && r->OtherPolicy==current.OtherPolicy);
    ++writes;
    if(write_error)return write_error;
    current=*r;return 0;
}
void vm_log_write(vm_log_level_t l,const char *f,int line,const char *fmt,...)
{(void)l;(void)f;(void)line;(void)fmt;}
int main(void) {
    vm_hostroute_guard_t *g=NULL;
    current.Family=AF_INET;current.InterfaceIndex=18;current.InterfaceLuid.Value=1234;
    current.Metric=25;current.OtherPolicy=999;current.SitePrefixLength=64;
    assert(vm_hostroute_guard_open(&g,18)==VM_OK);
    assert(vm_hostroute_guard_ensure(g)==VM_OK && writes==0);
    /* RAS leaves route present but disables its use; metric changes too. */
    current.DisableDefaultRoutes=1;current.Metric=4121;
    assert(vm_hostroute_guard_ensure(g)==VM_OK && writes==1);
    assert(!current.DisableDefaultRoutes && current.Metric==4121);
    current.DisableDefaultRoutes=1;write_error=5;
    assert(vm_hostroute_guard_ensure(g)==VM_ERR_IO && current.DisableDefaultRoutes);
    write_error=0;
    vm_hostroute_guard_close(g); /* Restores only the owned flag on exit. */
    assert(!current.DisableDefaultRoutes);
    current.DisableDefaultRoutes=1;
    assert(vm_hostroute_guard_open(&g,18)==VM_ERR_STATE && !g);
    read_error=2;
    assert(vm_hostroute_guard_open(&g,18)==VM_ERR_IO && !g);
    vm_hostroute_guard_close(NULL);
    puts("PASS: route guard baseline, RAS override, metric preservation, API errors, cleanup and pre-existing-policy refusal");
}
