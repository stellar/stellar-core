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