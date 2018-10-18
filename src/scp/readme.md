# SCP (Stellar Consensus Protocol)

The SCP subsystem is an abstract implementation of SCP, a protocol for federated
byzantine agreement, intended to drive a distributed system built around the
"replicated state machine" formalism. SCP is defined without reference to any
particular interpretation of the concepts of "slot" or "value", nor any
particular network communication system or replicated state machine.

This separation from the rest of the system is intended to make the
implementation of SCP easier to model, compare to the paper describing the
protocol, audit for correctness, and extract for reuse in different programs at
a later date.

The [SCPDriver class](SCPDriver.h) should be subclassed by any module wishing to
implement consensus using the SCP protocol, implementing the necessary abstract
methods for handling SCP-generated events, and calling methods from the central
[SCP base-class](SCP.h) methods to receive incoming messages.
The messages making up the protocol are defined in XDR,
in the file [Stellar-SCP.x](../xdr/Stellar-SCP.x)

The `stellar-core` program has a single subclass of SCPDriver called
[Herder](../herder), which gives a specific interpretation to "slot" and
"value", and connects SCP up with a specific broadcast communication medium
([Overlay](../overlay)) and specific replicated state machine
([LedgerManager](../ledger)).

For details of the protocol itself, see the [paper on SCP](https://www.stellar.org/papers/stellar-consensus-protocol.pdf).
