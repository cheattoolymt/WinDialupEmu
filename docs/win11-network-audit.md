# Windows 11 network audit, 2026-11-12

Evidence: user's Windows 11 26200 route table, vmodem.log and net-debug.txt.
Private logs are not committed. No instructions from those logs were executed.

## Findings and changes

- PPP LCP/IPCP reaches RUNNING, client 192.168.99.2. The PPP default route
  metric 76 wins over physical default metric 4250. This rules out a simple
  missing PPP default route. Source-bound curl also fails.
- Of logged UDP send failures, 314 are MinGW errno 118 (ENETUNREACH), 12 are
  101 (EADDRNOTAVAIL), one is 22 (EINVAL). The TCP log shows ENOTCONN (126)
  and locally generated RST/ACK. Packet counts include local ICMP failures;
  they cannot certify Internet access. LAN replies are present.
- Windows supports DisableDefaultRoutes on an interface: the /0 entry can be
  present but excluded from lookup. The old program never checked this flag.
  RAS/VPN-induced disabling is consistent with the evidence, but the user's
  actual flag value has NOT been observed. This is a conditional fix, not an
  assertion that physical Windows Internet access has been proven.
- New vm_routeguard.c captures the selected uplink's IPv4 flag before dialing.
  It refuses to override a pre-existing disabled policy. Before each slirp
  input it restores only changes from the captured value, addressed by stable
  interface LUID. Every write uses a fresh row and SitePrefixLength=0, preserving
  current metric, DNS and unrelated fields. No routes are added/deleted.
  WFP is installed before the modem accepts calls; physical direct application
  traffic remains restricted. Cleanup retries restoration if necessary.
  An unavailable adapter/API/write error is reported and stops forwarding.
- The original immediate COM read mode bypassed the wait on successful empty
  reads. Ported the proven timeout-mode cache from the XP branch. This fixes
  idle CPU spin without importing XP audio/clock code. The wrapper still uses
  CancelIoEx and completes cancelled operations before releasing OVERLAPPED.
- Serial read errors were ignored by the modem; they now stop the event loop.
  WSAPoll errors now log WSAGetLastError and propagate to modem shutdown.

## Review scope

Reviewed PPP/HDLC negotiation and data forwarding; Ethernet ARP and IPv4
validation; NAT input/output buffers; slirp config ABI and pointer lifetime;
4.9.3 socket registration ordering before connect/send; IPv4 interface pinning;
Windows poll flags and error propagation; clock/timers; WFP dynamic-session
isolation and cleanup; physical NIC selection; serial read/write cancellation;
modem lifecycle and error paths; audio handoff and CLI/startup cleanup.
Upstream libslirp 4.9.3 sources were read for corroboration.

The local slirp/PPP tests verify packets and transport behavior, not Windows
WFP/RAS integration. No Windows execution environment is available here.

## Limits and remaining work

- IPv6 PPP is unsupported. Windows nslookup selected physical DNS fe80::1;
  that traffic is intentionally blocked by isolation. An explicit IPv4 DNS
  test is necessary to distinguish DNS server selection from NAT reachability.
  This build does NOT rewrite the system's DNS settings or disable IPv6.
- WFP cleanup is a dynamic session. Interface restoration only reestablishes
  the pre-start default-route eligibility; no full adapter snapshot is written
  back. Concurrent manual changes to this same flag cannot be distinguished
  from RAS changes. The log states when the flag is changed.
- Existing physical adapter selection fallback may pick the wrong NIC if
  launched after dialing with multiple virtual Ethernet adapters. Start before
  RAS; the provided user's log selects the correct physical Intel NIC, index 18.
- The new model tests cover state transitions and failures but do not prove
  the affected Windows machine's flag or successful TCP/DNS after the patch.

## Validation

Run scripts/test-baseline.sh --slirp with libslirp available, then
scripts/test-windows-models.sh. Cross-build Windows x64 with the README command.
The package includes runtime import audit and test metadata.

## Primary references

- https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-getipinterfaceentry
- https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-setipinterfaceentry
- https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-commtimeouts
- https://qemu.googlesource.com/libslirp/+/refs/tags/v4.9.3/src/tcp_subr.c
- https://qemu.googlesource.com/libslirp/+/refs/tags/v4.9.3/src/udp.c
