# Starting a test network
By default stellar-core waits to hear from the network for a ledger close before it starts emmiting its own SCP messages. This works fine in the common case but when you want to start your own network you need to start SCP manually.
this is done by
```sh
$ stellar-core --forcescp
```
That will set state in the DB and then exit. The next time you start stellar-core SCP will start immediately rather than waiting.



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
