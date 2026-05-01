# Rust Overlay — Subsystem Docs

This directory documents the Rust overlay process, one subsystem per
file. Start with [the high-level design doc](../RUST_OVERLAY_DESIGN.md)
for the overall architecture and rationale.

| Doc                                       | Covers                                                          |
|-------------------------------------------|-----------------------------------------------------------------|
| [transport.md](transport.md)              | QUIC over UDP, libp2p, stream protocols, frame formats          |
| [peer-connections.md](peer-connections.md)| Peer membership, dialing, DNS, reconnect logic                  |
| [scp-flooding.md](scp-flooding.md)        | Push-based SCP propagation and dedup                            |
| [tx-propagation.md](tx-propagation.md)    | Pull-based INV/GETDATA TX flooding                              |
| [txset-fetching.md](txset-fetching.md)    | Fetching nominated TX sets, cache lifecycle                     |
| [mempool.md](mempool.md)                  | Fee-ordered pending-TX store                                    |
| [ipc.md](ipc.md)                          | Core ↔ Overlay Unix-socket protocol                             |

All file:line references use paths relative to the repo root (e.g.
`overlay/src/libp2p_overlay.rs:672`). When the implementation changes,
update both the relevant subsystem doc and any high-level claims in
`RUST_OVERLAY_DESIGN.md`.
