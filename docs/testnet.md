# Starting a test network
First, make sure you have copied the example config to your current working directory.
From the TLD of the repo, run
`cp docs/stellar_core_example.cfg ./bin/stellar-core.cfg`

In order to come to a quorum and make progress when you're running your own network (with one node), change your configuration quorum threshold to 1 and your quorum set to just your validation seed's public key.

```
# what generates the peerID (used for peer connections) used by this node
PEER_SEED="s3BCUXncNvghHzKafx4gwYGaEG5rEeMUDdJPDsdjve3ojoFd5tK"
# what generates the nodeID (used in FBA)
VALIDATION_SEED="s3BCUXncNvghHzKafx4gwYGaEG5rEeMUDdJPDsdjve3ojoFd5tK"

QUORUM_THRESHOLD=1
QUORUM_SET=["gxoicA8D962NezYaa4AmrhXKGHYbrELu8rhyKE2vt8osLHL3T5"]
```

By default stellar-core waits to hear from the network for a ledger close before
it starts emmiting its own SCP messages. This works fine in the common case but
when you want to start your own network you need to start SCP manually.
this is done by:
```sh
$ stellar-core --forcescp
```
That will set state in the DB and then exit. The next time you start stellar-core
SCP will start immediately rather than waiting.



For each server in the cluster you need to do:
```sh
$ stellar-core --newdb --forcescp
$ stellar-core
```

This will start a new ledger on each server and cause them to move ahead with SCP. They will still wait to hear from a quorum of nodes before closing a ledger.

# Bringing a test network back up
If you need to restart the network after bringing it down. Do the following on all the nodes:
```sh
$ stellar-core --forcescp
$ stellar-core
```

This will start from the last saved state of each server. It is important that a quorum of them all have the same laste ledger as their saved state before they are restarted.
