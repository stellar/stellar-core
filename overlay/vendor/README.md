# QUIC stream priority bridge

The overlay needs to prioritize control messages, then tx-set responses, then
transaction flooding on each QUIC connection. Quinn already supports this, but
the pinned libp2p API erases the native stream type and does not expose priority.

These are copies of the exact releases already in the workspace lockfile:

| Crate | Version |
|-------|---------|
| libp2p-core | 0.42.0 |
| libp2p-swarm | 0.45.1 |
| libp2p-quic | 0.11.1 |
| multistream-select | 0.13.0 |

The workspace `[patch.crates-io]` selects these copies. No dependency versions
change. Published source, manifests, tests, and provenance are retained;
`.cargo-ok` is omitted. Each copy also includes the repository-wide license
from its recorded upstream revision. `UPSTREAM.json` records those revisions,
license URLs, and original registry archive checksums. The archives were
verified against those checksums when copied.

## What the patch does

`stream-priority.patch` contains the priority bridge, in nine files:

- `libp2p-quic` delegates priority set/get to `quinn::SendStream`.
- `libp2p-core` carries priority callbacks through muxer type erasure,
  including `Either` and nested boxed muxers. Other muxers return `Unsupported`;
  the overlay requires successful QUIC priority assignment before sending.
  Pinned read/write forwarding preserves vectored I/O, flush, and close.
- `multistream-select` exposes a shared reference to the underlying stream
  without consuming buffered negotiation data, including pending negotiation.
  An invalidated negotiation returns `None`.
- `libp2p-swarm::Stream` exposes the callbacks to the overlay.

The overlay sets priority before application writes on initial opening and
reopening. Failure to set priority is an opening error; it cannot silently run
at default priority. No scheduler, QUIC configuration, wire encoding, or
acknowledgement behavior changes inside these crates.

`udp-buffer.patch` separately requires a 4 MiB OS receive-buffer request on
every listener and dial-only socket. There are no buffer settings on the overlay
or vendored transport configuration. The transport verifies the OS result,
automatically tries Linux `SO_RCVBUFFORCE` if the ordinary request is clamped,
and fails socket creation if the requirement cannot be met. The privileged
operation requires an existing NET_ADMIN grant; it never grants capabilities
or changes sysctls. libc is a direct Linux dependency at the already-locked
version. Tests cover adequate ordinary buffering, successful forced increases,
permission failures, a still-clamped result, and real socket creation.

Quinn uses strict priority for eligible stream data. Higher values run first;
equal priorities retain Quinn's configured fairness. This does not reserve
bandwidth or connection flow-control capacity, preempt bytes already sent, or
guarantee progress for a lower-priority stream under continuous higher-priority
load. Routing and compatibility are described in
[`transport.md`](../../docs/rust-overlay/transport.md).

## Validation and maintenance

Run `cargo test --locked -p stellar-overlay` on a host that meets the same UDP
buffer requirement as production. Tests use real libp2p/QUIC peers
and read back native Quinn priorities through every wrapper. They cover route
priorities, reopening, fetches during blocked response writes, and legacy-only
protocol negotiation. They do not substitute for a saturated-network benchmark.

To reproduce the source patch, unpack the recorded crate archives into the
corresponding directories, then run `patch -p1 < stream-priority.patch` from
`overlay/vendor`, followed by `patch -p1 < udp-buffer.patch`. Restore each license from its recorded URL. To verify an
existing copy, `patch --dry-run -R -p1 < stream-priority.patch` checks that the
patch can be reversed (also reverse `udp-buffer.patch`); compare the resulting source to the verified archive
when auditing an update.

When upgrading libp2p, review and rebase this bridge explicitly, update the
provenance and patch, and rerun the overlay transport tests. Remove these
overrides once the selected upstream API supports the same capability. Keeping
the bridge local adds dependency maintenance work; the patch file separates
that work from the unchanged vendored source.
