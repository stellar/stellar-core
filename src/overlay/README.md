# Overlay

The overlay subsystem manages a virtual "broadcast network" composed of a set of
peer-to-peer TCP connections, as well as mechanisms for managing distribution of
broadcast messages, anycast request/reply message pairs, and peer-to-peer control
messages to and from those peers.

Within the local process, the overlay subsystem primarily delivers messages to,
and accepts them from, the [Herder](../herder), as well as propagating through
the network any transactions injected from public API servers.

Good reading entry points are [`OverlayManager.h`](./OverlayManager.h), as well as the implementation of
`OverlayManagerImpl::tick`, and `OverlayManagerImpl::broadcastMessage`.

Flooding is hash-keyed (`Floodgate`). Transaction envelopes are advertised and
requested with `FLOOD_ADVERT` / `FLOOD_DEMAND`; transaction sets and quorum
sets are fetched by hash through `ItemFetcher` using `GET_TX_SET` and
`GET_SCP_QUORUMSET`. See [`docs/overlay.md`](../../docs/overlay.md).
