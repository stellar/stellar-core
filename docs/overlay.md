---
title: Overlay
---

- **Peer**
    - A peer is another node in the Stellar network with which Stellar Core can communicate.
- **Overlay**
    - The overlay is a peer-to-peer network responsible for connecting to other Stellar cores (peers) and propagating transactions, blocks, and consensus votes to them.


- **Overlay Network**
    * Discovering and connecting to other peers on the network: the overlay has a built-in peer discovery mechanism, where peers exchange known peers with each other over time. The initial set of connections to try is seeded via a node's quorum set as well as `KNOWN_PEERS` and `PREFERRED_PEERS`. Connectivity targets are configured via `TARGET_PEER_CONNECTIONS` (max outbound) and `MAX_ADDITIONAL_PEER_CONNECTIONS` (max inbound). Core can expect up to `TARGET_PEER_CONNECTIONS`+`MAX_ADDITIONAL_PEER_CONNECTIONS` total authenticated connections.

    * Connecting to peers: by default, the overlay will select random peers it knows about to connect to. Connection preferences can be tailored via the `KNOWN_PEERS` and `PREFERRED_PEERS` config options. The former is a list of known peers core will try to connect to without discovering them from others first. The latter is a list of peers to prioritize connecting to. If `PREFERRED_PEERS` or `PREFERRED_PEERS_KEYS` is configured, core will constantly try to connect to those peers.

    * Propagating consensus votes and transactions on the network: the overlay is responsible for ensuring that messages are propagated (flooded) to all nodes on the network. There are several optimizations that improve flooding:
        - block and transaction propagation are pull-based, where peers only request data they don't have 
        - consensus propagation is push-based, as it's sensitive to latency, and consensus messages are small in size
        - peers request new data when they're ready to process it. This is done to prevent network congestion. Applying back-pressure on the receiver side also allows the sender to prioritize accumulated messages in the queue, and shed load that becomes obsolete.

    * Versioning: The overlay subsystem has a version number, which is the latest version of the protocol that the node supports. It also maintains a minimum supported overlay version. Any connection that doesn't support the minimum version is rejected.

## Hashes versus payloads

The overlay does not flood full objects by default. Peers first exchange
**identifiers** (32-byte hashes). A node only **pulls** the matching payload if
it does not already have it. Flooding is keyed by `Hash` in `FloodGate`
(`src/overlay/Floodgate.h`).

`StellarMessage` (see `Stellar-overlay.x`) splits into three kinds, documented
in `src/overlay/OverlayManager.h`:

- **Peer-directed:** `HELLO`, `PEERS`, `DONT_HAVE`, `ERROR_MSG`.
- **Broadcast:** `TRANSACTION`, `SCP_MESSAGE`. Consensus votes are **pushed**
  because they are small and latency-sensitive.
- **Anycast by hash:** `GET_TX_SET` / `TX_SET`, `GET_SCP_QUORUMSET` /
  `SCP_QUORUMSET`, `GET_SCP_STATE`. `ItemFetcher` asks connected peers, in
  sequence, for the body of a hash. These messages are not flooded.

Transaction dissemination in pull mode uses the same split:

- `FLOOD_ADVERT` carries a `FloodAdvert` of `txHashes` (up to
  `TX_ADVERT_VECTOR_MAX_SIZE`).
- `FLOOD_DEMAND` carries a `FloodDemand` of hashes this node is missing.
- The transaction body is sent only in response to a demand.

So a peer that already holds a given hash never downloads the envelope again.
That is the same rule as `broadcastMessage(..., std::optional<Hash>)`: when a
transaction is flooded, its envelope hash is what overlay uses to decide
whether the message is new.

History catchup and long-term ledger archives stay **off** the overlay (see
`docs/architecture.md` and `docs/history.md`). Overlay is for live consensus
and mempool, not for shipping the full payload of the network's past.
