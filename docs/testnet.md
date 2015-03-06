# Starting a test network
By default stellard waits to hear from the network for a ledger close before it starts emmiting its own SCP messages. This works fine in the common case but when you want to start your own network you need to start SCP manually.
this is done by
```sh
$ stellard --forcescp 
```
That will set state in the DB and then exit. The next time you start stellard SCP will start immediately rather than waiting.



For each server in the cluster you need to do:
```sh
$ stellard --newdb
$ stellard --forcescp
$ stellard
```

This will start a new ledger on each server and cause them to move ahead with SCP. They will still wait to hear from a quorum of nodes before closing a ledger.

# Bringing a test network backup
If you need to bring this netowrk down and restart it. Do:
```sh
$ stellard --forcescp
$ stellard
```

This will start from the last saved state of each server. It is important that a quorum of them all stopped while on the same ledger.
