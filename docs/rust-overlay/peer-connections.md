# Peer Connections and Discovery

The Rust overlay's peer-management strategy is intentionally simple:
**connect to every peer Core tells us about, accept any peer that dials
us**. There is no autonomous peer discovery — no Kademlia, no peer
exchange, no gossip. Core is the source of truth for peer membership,
delivered via the `SetPeerConfig` IPC message.

## High level

```
Core (C++)                                         Overlay (Rust)
──────────                                         ──────────────
config: KNOWN_PEERS, PREFERRED_PEERS
        │
        ▼
SetPeerConfig (JSON: known_peers + preferred_peers)
   ────────────────────────────────────────────▶
                                                   for each addr:
                                                     resolve DNS
                                                     dial libp2p QUIC

   ◀─────  PeerConnected events                    track in known_peers
                                                   + peer_hostnames

                     (later, if peer drops)
                                                   targeted reconnect
                                                   with exponential backoff
```

The overlay accepts and tracks two flavours of peer:

- **Configured peers** — addresses sent by Core via `SetPeerConfig`.
  The overlay reconnects to these on disconnect.
- **Unconfigured inbound peers** — peers that dialed us. The overlay
  serves traffic to them but does **not** reconnect if they drop.

## SetPeerConfig handler

`main.rs:1283-1382`. JSON schema:

```json
{
  "known_peers":     ["host1:port", "10.0.0.5:11625", ...],
  "preferred_peers": ["host2:port", ...],
  "listen_port":     11625
}
```

Steps performed:

1. **Replace** the configured-peer set with `known_peers ∪ preferred_peers`.
   No distinction is made between known and preferred at the overlay
   layer — both lists are dialed identically.
2. **Prune stale peers**: any peer whose hostname is no longer in the
   config is removed from `known_peers` and `peer_hostnames`. This
   prevents reconnection to peers that Core has dropped from its config.
3. **Resolve and dial** each address in a background task. See
   [DNS resolution](#dns-resolution).
4. **Schedule a retry task** for any addresses where DNS failed.

Note: `target_outbound_peers` and `max_inbound_peers` are defined in
`config.rs` but are **not enforced anywhere in the network code**. The
overlay attempts to connect to every address Core sends and accepts all
inbound connections.

## DNS resolution

`main.rs:198-298`.

`resolve_peer_addr` accepts:
- `IP:port` — parsed directly.
- `hostname` — resolved via `tokio::net::lookup_host`, port = `listen_port`
  from the SetPeerConfig payload.
- `hostname:port` — resolved via DNS with explicit port.

After resolution the address is rewritten to the **libp2p port**
(`port + 1000`) and turned into a multiaddr `/ip{4,6}/<ip>/udp/<port>/quic-v1`
for dialing.

If DNS resolution fails, the address is not given up — it goes into a
retry queue (`spawn_peer_retry_task`, `main.rs:303-362`):

- First retry after **2 s**.
- Exponential backoff to a max of **30 s** between retries.
- Loops until DNS succeeds. There is no give-up condition.

This was added because in containerized deployments (K8s headless
services), peer DNS records sometimes do not exist at startup; the
overlay must keep retrying until the other pods come up.

## Self-dial prevention

`main.rs:367+ (collect_local_addrs)`, used in `resolve_peer_to_libp2p` /
`resolve_and_dial`. The overlay snapshots its local interface addresses
plus loopback into a set, and silently skips dials that resolve to one
of those `(ip, libp2p_port)` pairs. Important in K8s pods where a service
hostname can resolve back to the same pod.

## Connection tracking

State maintained in the main task (`main.rs:444-465`):

| Field               | Type                            | Purpose                                          |
|---------------------|---------------------------------|--------------------------------------------------|
| `configured_peers`  | `RwLock<ConfiguredPeers>`       | Addresses from SetPeerConfig + resolved cache    |
| `known_peers`       | `RwLock<HashMap<PeerId, Multiaddr>>` | PeerId → last-known Multiaddr               |
| `peer_hostnames`    | `RwLock<HashMap<PeerId, String>>`     | PeerId → original hostname from config; absence here means "inbound, not reconnect-eligible" |

When `PeerConnected` fires, the overlay only records the peer in
`known_peers` / `peer_hostnames` if its endpoint matches a configured
address (`main.rs:851-870`). Random inbound peers are served traffic
but never tracked for reconnect.

## Reconnection

Two complementary mechanisms.

### Targeted reconnect on disconnect

`main.rs:872-949`. When a `PeerDisconnected` event fires for a peer that
has a hostname entry (i.e., a configured peer), a per-peer task is
spawned:

- **10 attempts** total.
- Delay starts at **1 s**, doubles up to a cap of **30 s**.
- Attempts 1–3 dial the cached `Multiaddr` directly (fast path).
- Attempts 4–10 re-resolve DNS first (handles K8s pod restarts where the
  IP changes).
- Each re-resolve updates `known_peers` and `configured_peers.resolved`.

### Safety-net reconnect timer

`main.rs:544-720` (event loop branch on `reconnect_interval.tick()`).
Every **30 s** the main loop:

1. Compares connected-peer count against the configured-peer count.
2. If under target, issues PeerId-based dials for every configured peer
   we have ever connected to (libp2p deduplicates if already connected).
3. For configured peers we have *never* learned a `PeerId` for, performs
   resolve-then-check-then-dial (so we don't accidentally double-dial
   the same address under a different name).

This catches edge cases the targeted task can miss, e.g. peers that have
never connected at all yet.

## Identify protocol

`/stellar/1.0.0` runs alongside the three Stellar streams
(`libp2p_overlay.rs:380`). On `IdentifyEvent::Received` the overlay logs
the peer's reported listen addresses but does **not** feed them into
peer selection (`libp2p_overlay.rs:643-646`). It is currently
informational only.

## What's not here

- **No Kademlia DHT.** Earlier versions of this branch wired up Kademlia;
  it was removed (commit `4e8e10af8`, "Remove Kademlia, fix DNS bugs")
  and is no longer in `Cargo.toml` or the source tree.
- **No peer-exchange / gossip.** Peers do not learn about other peers from
  each other.
- **No outbound connection limit.** `target_outbound_peers` is unused.
- **No inbound connection limit.** `max_inbound_peers` is unused.
- **No banlist or DoS scoring.** Misbehaving peers are not throttled or
  evicted by the overlay (this lives in Core for the legacy overlay; not
  yet ported here).
