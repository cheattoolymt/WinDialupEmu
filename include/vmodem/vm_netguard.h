/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef VMODEM_VM_NETGUARD_H
#define VMODEM_VM_NETGUARD_H
#include "vmodem/vm_types.h"
/* Dynamic WFP session: closing the handle, including process death, removes
 * every filter. No adapter, route, registry or firewall profile is modified.
 * Active from successful modem creation until modem destruction. */
typedef struct vm_netguard_s vm_netguard_t;
vm_err_t vm_netguard_open(vm_netguard_t **out);
void vm_netguard_close(vm_netguard_t *guard);
#endif
