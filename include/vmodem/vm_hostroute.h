/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef VMODEM_VM_HOSTROUTE_H
#define VMODEM_VM_HOSTROUTE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Select an active Ethernet/Wi-Fi IPv4 outside the emulated subnet before
 * RAS connects. Both the source address and IP_UNICAST_IF must be set on
 * libslirp sockets: source binding alone cannot bypass the PPP default route.
 * Returns a host-order address, or zero if selection failed. */
uint32_t vm_hostroute_pick_outbound_ip(uint32_t exclude_net,
                                      uint32_t exclude_mask,
                                      char *name_out, size_t name_size);
uint32_t vm_hostroute_interface_index(uint32_t ip);
#ifdef __cplusplus
}
#endif
#endif
