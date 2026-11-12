# Negotiated bandwidth enforcement

Before this fix the negotiated bps was used for CONNECT, status and sounds;
IPv4 packets passed directly between PPP and NAT without pacing. A virtual
COM port does not impose the advertised carrier rate. High throughput while
PPP counters increase does not by itself indicate bypass of network isolation.

Each call now has independent upload and download FIFO serializers using the
negotiated rate (2400, 14400, 28800, 33600, etc). Each IPv4 packet must wait
length * 8 / bps seconds before release. At 33600 bit/s the IPv4 budget is at
most 4200 bytes/s in each direction; TCP/UDP payload is lower due to headers.
PPP/HDLC overhead is not charged separately. Control protocols (LCP/IPCP,
termination and echoes) stay responsive and are not queued behind data.

Memory is bounded to eight packets of up to 1600 bytes per direction. A full
queue drops new packets instead of allocating unlimited memory. TCP retransmits
and slows; UDP loss is expected under saturation. No idle-time credits or
catch-up bursts are accumulated. Reconnect and hangup clear both queues.
IPv4 with the none backend is discarded as before. WFP isolation is unchanged.

The Ethernet status continues to show the hardware link's 1 Gbit/s; it must
remain enabled to carry the emulator's translated traffic. Bandwidth should be
assessed at the application's receive rate or PPP byte-count delta, not the
physical adapter's advertised link speed. Its counters can also include queued
upstream socket data and unrelated traffic from before the call.

Tests:
- Simulated 60-second saturation in both directions at 2400/14400/33600/56000.
  No elapsed-time bit budget is exceeded; throughput approaches the configured
  rate with a 1ms polling step. Validate exact packet serialization boundaries,
  no credit after idle, bounded overflow, no catch-up after stalls and reset.
- Integration uses real modem callbacks, NAT loopback echo, PPP encoding and
  serial loopback. A 1500-byte upload and reply each wait at least 357.143ms
  at 33600 bit/s; no output escapes before both delays expire. Hangup discards
  queued data. Only the clock is replaced; no external Internet is required.
- Existing regression suites, real libslirp UDP/TCP and Windows x64 cross-build.

Actual Windows/fast.com measurements require the affected machine. Very slow
pages may load slowly by design. The existing Internet-connectivity and route
eligibility fixes are retained.
