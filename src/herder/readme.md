# Herder

The [HerderSCPDriver](HerderSCPDriver.h) is a concrete implementation of the [SCP
protocol](../scp), operating in terms of the "transaction sets" and "ledger
numbers" that constitute the Stellar vocabulary. It is implemented as a subclass
of the [SCPDriver class](../scp/SCPDriver.h), and so is most easily understood after
reading that class and understanding where and how a subclass would make the abstract
SCP protocol concrete.

# key implementation details

The Herder considers a ledger number to be a "slot" in the SCP
protocol, and transaction set hashes (along with close-time and base-fee) to be
the sort of "value" that it is attempting to agree on for each "slot".

Herder acts as the glue between SCP and LedgerManager.

## LedgerManager interaction
Herder serializes "slot externalize" events as much as possible so that
LedgerManager only sees strictly monotonic ledger closing events (and deal with
 any potential gaps using catchup).

## SCP interaction
Herder has two main modes of operation.

### "Tracking" state
Herder knows which slot got externalized last and only processes SCP messages
 for the next slot.

SCP messages received for future slots are stored for later use: receiving
 future messages is not necessarily an indication of a problem.
Messages for the current slot may have been delayed by the network while
 other peers moved on.

#### Timeout
Herder places a timeout to make progress on the expected next slot, if it
 reaches this timeout, it changes its state to "Not tracking".

#### Picking the initial position
When a ledger is closed and LedgerManager is in sync, herder is responsible
 for picking a starting position to send a PREPARING message.

### "Not Tracking" state
Herder does not know which slot got externalized last, its goal is to go back
 to the tracking state.
In order to do this, it starts processing all SCP messages starting with the
 smallest slot number, maximizing the chances that one of the slots actually
 externalizes.

Note that moving to this state does not necessarily mean that the
 LedgerManager would move out of sync: it could just be that it takes an
 abnormal time (network outage of some sort, partitioning, etc) for nodes to
 reach consensus.
