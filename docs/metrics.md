---
title: List of metrics exposed by stellar-core
---

stellar-core uses libmedida for computing metrics, a detailed description can
be found at http://dln.github.io/medida/

### Counters (`NewCounter`)
Tracks a value in absolute terms of a base unit.

### Histograms (`NewHistogram`)
Tracks aggregates (count, min, max, mean, percentiles, etc) for samples
expressed in arbitrary base unit.

### Timers (`NewTimer`)
Tracks aggregates (count, min, max, mean, percentiles, etc) of samples expressed in units of time.

### Meters (`NewMeter`)
Tracks aggregates (count, min, max, mean, etc),  rate (1m, 5m, 15m) for samples
expressed in base unit.


Metric name                              | Type      | Description
---------------------------------------  | --------  | --------------------
bucket.snap.merge                        | timer     | time to merge two buckets
bucket.batch.objectsadded                | meter     | number of objects added per batch
bucket.batch.addtime                     | timer     | time to add a batch
bucket.memory.shared                     | counter   | number of buckets referenced (excluding publish queue)
scp.sync.lost                            | meter     | validator lost sync
scp.envelope.emit                        | meter     | SCP message sent
scp.envelope.receive                     | meter     | SCP message received
scp.memory.cumulative-statements         | counter   | number of known SCP statements known
herder.pending-txs.age0                  | counter   | number of gen0 pending transactions
herder.pending-txs.age1                  | counter   | number of gen1 pending transactions
herder.pending-txs.age2                  | counter   | number of gen2 pending transactions
herder.pending-txs.age3                  | counter   | number of gen3 pending transactions
scp.envelope.sign                        | meter     | envelope signed
scp.envelope.validsig                    | meter     | envelope signature verified
scp.envelope.invalidsig                  | meter     | envelope failed signature verification
scp.value.valid                          | meter     | SCP value is valid
scp.value.invalid                        | meter     | SCP value is invalid
scp.nomination.combinecandidates         | meter     | number of candidates per call
scp.timing.nominated                     | timer     | time spent in nomination
scp.timing.externalized                  | timer     | time spent in ballot protocol
scp.pending.processed                    | counter   | number of already processed envelopes
scp.pending.discarded                    | counter   | number of discarded envelopes
scp.pending.fetching                     | counter   | number of incomplete envelopes
scp.pending.ready                        | counter   | number of envelopes ready to process
history.apply-ledger-chain.success       | meter     | apply ledger chain completed successfuly
history.apply-ledger-chain.failure       | meter     | apply ledger chain failed
history.publish.success                  | meter     | published completed successfuly
history.publish.failure                  | meter     | published failed
history.download-<X>.success             | meter     | download of <X> completed successfuly
history.download-<X>.failure             | meter     | download of <X> failed
history.verify-<X>.success               | meter     | verification of <X> succeeded
history.verify-<X>.failure               | meter     | verification of <X> failed
history-archive.<X>.success              | meter     | accessing history archive <X> succeeded
history-archive.<X>.failure              | meter     | accessing history archive <X> failed
ledger.invariant.failure                 | counter   | number of times invariants failed
ledger.transaction.apply                 | timer     | time to apply one transaction
ledger.transaction.count                 | histogram | number of transactions per ledger
ledger.transaction.internal-error        | counter   | number of internal errors since start
ledger.operation.count                   | histogram | number of operations per ledger
ledger.operation.apply                   | timer     | time applying an operation
ledger.ledger.close                      | timer     | time to close a ledger (excluding consensus)
ledger.age.closed                        | timer     | time between ledgers
ledger.age.current-seconds               | counter   | gap between last close ledger time and current time
ledger.memory.queued-ledgers             | counter   | number of ledgers queued in memory for replay
app.state.current                        | counter   | state (BOOTING=0, JOIN_SCP=1, LEDGER_SYNC=2, CATCHING_UP=3, SYNCED=4, STOPPING=5)
app.post-on-main-thread.delay            | timer     | time to start task posted to current crank of main thread
app.post-on-main-thread-with-delay.delay | timer     | time to start task posted to next crank of main thread
app.post-on-background-thread.delay      | timer     | time to start task posted to background threadoverlay.memory.flood-known        | counter   | number of known flooded entries
overlay.flood.broadcast                  | meter     | message sent as broadcast per peer
overlay.message.broadcast                | meter     | message broadcasted
overlay.connection.outbound-start        | meter     | outbound connection initiated
overlay.connection.establish             | meter     | connection established (pending inbound/outbound)
overlay.connection.drop                  | meter     | connection dropped
overlay.connection.reject                | meter     | connection rejected
overlay.connection.pending               | counter   | number of pending connections
overlay.connection.authenticated         | counter   | number of authenticated peers
overlay.byte.read                        | meter     | number of bytes received
overlay.byte.write                       | meter     | number of bytes sent
overlay.message.read                     | meter     | message received
overlay.message.write                    | meter     | message sent
overlay.error.read                       | meter     | error while receiving a message
overlay.error.write                      | meter     | error while sending a message
overlay.timeout.idle                     | meter     | idle peer timeout
overlay.recv.<X>                         | timer     | received message <X>
overlay.send.<X>                         | meter     | sent message <X>
overlay.item-fetcher.next-peer           | meter     | ask for item past the first one
loadgen.step.count                       | meter     | loadgenerator: generated some transactions
loadgen.step.submit                      | timer     | loadgenerator: time spent submiting transactions per step
loadgen.run.complete                     | meter     | loadgenerator: run complete
loadgen.account.created                  | meter     | loadgenerator: account created
loadgen.payment.native                   | meter     | loadgenerator: native payment submited
loadgen.txn.attempted                    | meter     | loadgenerator: transaction submitted
loadgen.txn.rejected                     | meter     | loadgenerator: transaction rejected
loadgen.txn.bytes                        | meter     | loadgenerator: size of transactions submitted
