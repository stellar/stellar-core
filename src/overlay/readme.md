# Overlay

The overlay subsystem manages a virtual "broadcast network" composed of a set of
peer-to-peer TCP connections, as well as mechanisms for managing distribution of
broadcast messages, anycast request/reply message pairs, and peer-to-peer control
messages to and from those peers.

Within the local process, the overlay subsystem primarily delivers messages to,
and accepts them from, the [Herder](../herder), as well as propagating through
the network any transactions injected from public API servers.

Good reading entry points are `OverlayManager.h`, as well as the implementation of
`OverlayManagerImpl::tick`, and `OverlayManagerImpl::broadcastMessage`.
