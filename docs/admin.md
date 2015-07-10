---
id: admin
title: Admin
category: Guides
---
# Running an instance

# Hardware requirements
    (need more stress testing)

# Operational considerations
* You will need a PEER_SEED to recognize your node on the network;
* you will also need a VALIDATION_SEED if you plan on participating in SCP.

# Administrative commands
Interaction with stellar-core is done via an administrative HTTP endpoint.
The endpoint is typically accessed by a mid-tier application to submit 
transactions to the Stellar network, or by administrators.

# Notes
It can take up to 5 or 6 minutes to sync to the network when you start up.

# Supported commands

## /catchup?ledger=NNN[&mode=MODE]

Triggers the instance to catch up to ledger NNN from history;
mode is either 'minimal' (the default, if omitted) or 'complete'.

## /checkpoint

Triggers the instance to write an immediate history checkpoint.

## /connect?peer=NAME&port=NNN

Triggers the instance to connect to peer NAME at port NNN.

## /help

Give a list of currently supported commands

## /info

Returns information about the server in JSON format (sync
state, connected peers, etc).

## /ll?level=L[&partition=P]

Adjust the log level for partition P (or all if no partitionis specified).
level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE, TRACE

## /logrotate

Rotate (close and reopen) the log file.

## /manualclose

If MANUAL_CLOSE is set to true in the .cfg file. This is will cause the 
current ledger to close. Used only for testing.

## /metrics

Returns a snapshot of the metrics registry (for monitoring and
debugging purpose).

## /peers

Returns the list of known peers in JSON format.

## /scp

Returns a JSON object with the internal state of the SCP engine.

## /stop

Stops the instance.

## /tx?blob=HEX

```
submit a transaction to the network.
blob is a hex encoded XDR serialized 'TransactionEnvelope'
returns a JSON object with the following properties
status:
    * "PENDING" - transaction is being considered by consensus
    * "DUPLICATE" - transaction is already PENDING
    * "ERROR" - transaction rejected by transaction engine
        error: set when status is "ERROR".
            hex encoded, XDR serialized 'TransactionResult'
```


## Additional Documentation

This directory contains the following additional documentation:

* [testnet.md](/docs/testnet.md) is short tutorial demonstrating how to
  configure and run a short-lived, isolated test network.

* [architecture.md](/docs/architecture.md) describes how `stellar-core` is
  structured internally, how it is intended to be deployed and the collection of
  servers and services needed to get the full functionality and performance.