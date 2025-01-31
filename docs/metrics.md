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

### Buckets (`NewBuckets`)
Tracks multiple timers organized into disjoint buckets.

Metric name                               | Type      | Description
---------------------------------------   | --------  | --------------------
app.post-on-background-thread.delay       | timer     | time to start task posted to background thread
app.post-on-main-thread.delay             | timer     | time to start task posted to current crank of main thread
app.post-on-overlay-thread.delay          | timer     | time to start task posted to overlay thread
bucket.batch.addtime                      | timer     | time to add a live batch
bucket.batch.objectsadded                 | meter     | number of objects added per live batch
bucket.batch-archive.addtime              | timer     | time to add a hot archive batch
bucket.batch-archive.objectsadded         | meter     | number of objects added per hot archive batch
bucket.memory.shared                      | counter   | number of buckets referenced (excluding publish queue)
bucket.merge-time.level-<X>               | timer     | time to merge two buckets on level <X>
bucket.snap.merge                         | timer     | time to merge two buckets
bucketlist-archive.size.bytes             | counter   | total size of the hot archive BucketList in bytes
bucketlist.size.bytes                     | counter   | total size of the BucketList in bytes
bucketlist.entryCounts.-<X>               | counter   | number of entries of type <X> in the BucketList
bucketlist.entrySizes.-<X>                | counter   | size of entries of type <X> in the BucketList
bucketlistDB-<X>.bloom.lookups              | meter     | number of bloom filter lookups on BucketList <X> (live/hotArchive)
bucketlistDB-<X>.bloom.misses               | meter     | number of bloom filter false positives on BucketList <X> (live/hotArchive)
bucketlistDB-<X>.bulk.loads                 | meter     | number of entries BucketListDB queried to prefetch on BucketList <X> (live/hot-archive)
bucketlistDB-live.bulk.inflationWinners     | timer     | time to load inflation winners
bucketlistDB-live.bulk.poolshareTrustlines  | timer     | time to load poolshare trustlines by accountID and assetID
bucketlistDB-live.bulk.prefetch             | timer     | time to prefetch
bucketlistDB-<X>.point.<y>                | timer     | time to load single entry of type <Y> on BucketList <X> (live/hotArchive)
crypto.verify.hit                         | meter     | number of signature cache hits
crypto.verify.miss                        | meter     | number of signature cache misses
crypto.verify.total                       | meter     | sum of both hits and misses
herder.pending[-soroban]-txs.age0         | counter   | number of gen0 pending transactions
herder.pending[-soroban]-txs.age1         | counter   | number of gen1 pending transactions
herder.pending[-soroban]-txs.age2         | counter   | number of gen2 pending transactions
herder.pending[-soroban]-txs.age3         | counter   | number of gen3 pending transactions
herder.pending[-soroban]-txs.banned       | counter   | number of transactions that got banned
herder.pending[-soroban]-txs.delay        | timer     | time for transactions to be included in a ledger
herder.pending[-soroban]-txs.self-delay   | timer     | time for transactions submitted from this node to be included in a ledger
history.check.failure                     | meter     | history archive status checks failed
history.check.success                     | meter     | history archive status checks succeeded
history.publish.failure                   | meter     | published failed
history.publish.success                   | meter     | published completed successfully
history.publish.time                      | timer     | time to successfully publish history
history.get.throughput                    | meter     | bytes per second of history archive retrieval
history.get.failure                       | meter     | history archive downloads failed
ledger.age.closed                         | bucket    | time between ledgers
ledger.age.current-seconds                | counter   | gap between last close ledger time and current time
ledger.apply.success                      | counter   | count of successfully applied transactions
ledger.apply.failure                      | counter   | count of failed applied transactions
ledger.apply-soroban.success              | counter   | count of successfully applied soroban transactions
ledger.apply-soroban.failure              | counter   | count of failed applied soroban transactions
ledger.catchup.duration                   | timer     | time between entering LM_CATCHING_UP_STATE and entering LM_SYNCED_STATE
ledger.invariant.failure                  | counter   | number of times invariants failed
ledger.ledger.close                       | timer     | time to close a ledger (excluding consensus)
ledger.memory.queued-ledgers              | counter   | number of ledgers queued in memory for replay
ledger.metastream.bytes                   | meter     | number of bytes written per ledger into meta-stream
ledger.metastream.write                   | timer     | time spent writing data into meta-stream
ledger.operation.apply                    | timer     | time applying an operation
ledger.operation.count                    | histogram | number of operations per ledger
ledger.transaction.apply                  | timer     | time to apply one transaction
ledger.transaction.count                  | histogram | number of transactions per ledger
ledger.transaction.internal-error         | counter   | number of internal errors since start
loadgen.account.created                   | meter     | loadgenerator: account created
loadgen.payment.native                    | meter     | loadgenerator: native payment submitted
loadgen.pretend.submitted                 | meter     | loadgenerator: pretend ops submitted
loadgen.run.complete                      | meter     | loadgenerator: run complete
loadgen.soroban.create_upgrade            | meter     | loadgenerator: soroban create upgrade TXs submitted
loadgen.soroban.invoke                    | meter     | loadgenerator: soroban invoke TXs submitted
loadgen.soroban.setup_invoke              | meter     | loadgenerator: soroban setup invoke TXs submitted
loadgen.soroban.setup_upgrade             | meter     | loadgenerator: soroban setup upgrades TXs submitted
loadgen.soroban.upload                    | meter     | loadgenerator: soroban upload TXs submitted
loadgen.step.count                        | meter     | loadgenerator: generated some transactions
loadgen.step.submit                       | timer     | loadgenerator: time spent submitting transactions per step
loadgen.txn.attempted                     | meter     | loadgenerator: transaction submitted
loadgen.txn.bytes                         | meter     | loadgenerator: size of transactions submitted
loadgen.txn.rejected                      | meter     | loadgenerator: transaction rejected
overlay.byte.read                         | meter     | number of bytes received
overlay.byte.write                        | meter     | number of bytes sent
overlay.async.read                        | meter     | number of async read requests issued
overlay.async.write                       | meter     | number of async write requests issued
overlay.connection.authenticated          | counter   | number of authenticated peers
overlay.connection.latency                | timer     | estimated latency between peers
overlay.connection.pending                | counter   | number of pending connections
overlay.connection.read-throttle          | timer     | throttle time for reading incoming traffic from peers
overlay.connection.flood-throttle         | timer     | throttle time for sending flood traffic to peers
overlay.delay.async-write                 | timer     | time between each message's async write issue and completion
overlay.delay.write-queue                 | timer     | time between each message's entry and exit from peer write queue
overlay.error.read                        | meter     | error while receiving a message
overlay.error.write                       | meter     | error while sending a message
overlay.fetch.txset                       | timer     | time to complete fetching of a txset
overlay.fetch.qset                        | timer     | time to complete fetching of a qset
overlay.flood.advertised                  | meter     | transactions advertised through pull mode
overlay.flood.demanded                    | meter     | transactions demanded through pull mode
overlay.flood.fulfilled                   | meter     | demanded transactions fulfilled through pull mode
overlay.flood.unfulfilled-banned          | meter     | transactions we failed to fulfilled since they are banned
overlay.flood.unfulfilled-unknown         | meter     | transactions we failed to fulfilled since they are unknown
overlay.flood.tx-pull-latency             | timer     | time between the first demand and the first time we receive the txn
overlay.flood.peer-tx-pull-latency        | timer     | time to pull a transaction from a peer
overlay.demand.timeout                    | meter     | pull mode timeouts
overlay.flood.relevant-txs                | meter     | relevant transactions pulled from peers
overlay.flood.irrelevant-txs              | meter     | irrelevant transactions pulled from peers
overlay.flood.advert-delay                | timer     | time each advert sits in the inbound queue
overlay.flood.abandoned-demands           | meter     | tx hash pull demands that no peers responded
overlay.flood.broadcast                   | meter     | message sent as broadcast per peer
overlay.flood.duplicate_recv              | meter     | number of bytes of flooded messages that have already been received
overlay.flood.unique_recv                 | meter     | number of bytes of flooded messages that have not yet been received
overlay.inbound.attempt                   | meter     | inbound connection attempted (accepted on socket)
overlay.inbound.drop                      | meter     | inbound connection dropped
overlay.inbound.establish                 | meter     | inbound connection established (added to pending)
overlay.inbound.reject                    | meter     | inbound connection rejected
overlay.outbound-queue.<X>                | timer     | time <X> traffic sits in flow-controlled queues
overlay.outbound-queue.drop-<X>           | meter     | number of <X> messages dropped from flow-controlled queues
overlay.item-fetcher.next-peer            | meter     | ask for item past the first one
overlay.memory.flood-known                | counter   | number of known flooded entries
overlay.message.broadcast                 | meter     | message broadcasted
overlay.message.read                      | meter     | message received
overlay.message.write                     | meter     | message sent
overlay.message.drop                      | meter     | message dropped due to load-shedding
overlay.outbound.attempt                  | meter     | outbound connection attempted (socket opened)
overlay.outbound.cancel                   | meter     | outbound connection cancelled
overlay.outbound.drop                     | meter     | outbound connection dropped
overlay.outbound.establish                | meter     | outbound connection established (added to pending)
overlay.recv.<X>                          | timer     | received message <X>
overlay.send.<X>                          | meter     | sent message <X>
overlay.timeout.idle                      | meter     | idle peer timeout
overlay.recv.start-survey-collecting      | timer     | time spent in processing request to start survey collecting phase
overlay.recv.stop-survey-collecting       | timer     | time spent in processing request to stop survey collecting phase
overlay.recv.survey-request               | timer     | time spent in processing survey request
overlay.recv.survey-response              | timer     | time spent in processing survey response
overlay.send.start-survey-collecting      | timer     | sent request to start survey collecting phase
overlay.send.stop-survey-collecting       | timer     | sent request to stop survey collecting phase
overlay.send.survey-request               | meter     | sent survey request
overlay.send.survey-response              | meter     | sent survey response
process.action.queue                      | counter   | number of items waiting in internal action-queue
process.action.overloaded                 | counter   | 0-or-1 value indicating action-queue overloading
process.file.handles                      | counter   | number of open file handles
process.memory.handles                    | counter   | number of running processes in process manager
scp.envelope.emit                         | meter     | SCP message sent
scp.envelope.invalidsig                   | meter     | envelope failed signature verification
scp.envelope.receive                      | meter     | SCP message received
scp.envelope.sign                         | meter     | envelope signed
scp.envelope.validsig                     | meter     | envelope signature verified
scp.fetch.envelope                        | timer     | time to complete fetching of an envelope
scp.memory.cumulative-statements          | counter   | number of known SCP statements known
scp.nomination.combinecandidates          | meter     | number of candidates per call
scp.pending.discarded                     | counter   | number of discarded envelopes
scp.pending.fetching                      | counter   | number of incomplete envelopes
scp.pending.processed                     | counter   | number of already processed envelopes
scp.pending.ready                         | counter   | number of envelopes ready to process
scp.sync.lost                             | meter     | validator lost sync
scp.timeout.nominate                      | meter     | timeouts in nomination
scp.timeout.prepare                       | meter     | timeouts in ballot protocol
scp.timing.nominated                      | timer     | time spent in nomination
scp.timing.externalized                   | timer     | time spent in ballot protocol
scp.timing.first-to-self-externalize-lag  | timer     | delay between first externalize message and local node externalizing
scp.timing.self-to-others-externalize-lag | timer     | delay between local node externalizing and later externalize messages from other nodes
scp.value.invalid                         | meter     | SCP value is invalid
scp.value.valid                           | meter     | SCP value is valid
scp.slot.values-referenced                | histogram | number of values referenced per consensus round
state-archival.eviction.age               | counter   | the average of the delta between an entry's liveUntilLedger and the ledger when it is evicted
state-archival.eviction.bytes-scanned     | counter   | number of bytes that eviction scan has read
state-archival.eviction.entries-evicted   | counter   | number of entries that have been evicted
state-archival.eviction.incomplete-scan   | counter   | number of buckets that were too large to be fully scanned for eviction
state-archival.eviction.period            | counter   | number of ledgers to complete an eviction scan
soroban.host-fn-op.read-entry                | meter     | number of entries accessed (read or modified) during the `InvokeHostFunctionOp`
soroban.host-fn-op.write-entry               | meter     | number of entries modified during the `InvokeHostFunctionOp`
soroban.host-fn-op.read-key-byte             | meter     | number of `LedgerKey` bytes in entries accessed (read or modified) during the `InvokeHostFunctionOp`
soroban.host-fn-op.write-key-byte            | meter     | number of `LedgerKey` bytes in entries modified during the `InvokeHostFunctionOp`
soroban.host-fn-op.read-ledger-byte          | meter     | number of `LedgerEntry` bytes accessed (read or modified) during the `InvokeHostFunctionOp`
soroban.host-fn-op.read-data-byte            | meter     | number of `ContractDataEntry` bytes accessed (read or modified) during the `InvokeHostFunctionOp`
soroban.host-fn-op.read-code-byte            | meter     | number of `ContractCodeEntry` bytes accessed (read or modified) during the `InvokeHostFunctionOp`
soroban.host-fn-op.write-ledger-byte         | meter     | number of `LedgerEntry` bytes modified during the `InvokeHostFunctionOp`
soroban.host-fn-op.write-data-byte           | meter     | number of `ContractDataEntry` bytes modified during the `InvokeHostFunctionOp`
soroban.host-fn-op.write-code-byte           | meter     | number of `ContractCodeEntry` bytes modified during the `InvokeHostFunctionOp`
soroban.host-fn-op.emit-event                | meter     | number of events emitted during the `InvokeHostFunctionOp`
soroban.host-fn-op.emit-event-byte           | meter     | number of event bytes emitted during the `InvokeHostFunctionOp`
soroban.host-fn-op.cpu-insn                  | meter     | number of metered cpu instructions during the `InvokeHostFunctionOp`
soroban.host-fn-op.mem-byte                  | meter     | number of metered memory bytes during the `InvokeHostFunctionOp`
soroban.host-fn-op.invoke-time-nsecs         | timer     | time spent on the soroban host invocation. Note: this is **not** the total time of the operation, which is tracked under "soroban.host-fn-op.exec".
soroban.host-fn-op.cpu-insn-excl-vm          | meter     | number of metered cpu instructions excluding VM instantiation during the `InvokeHostFunctionOp`
soroban.host-fn-op.invoke-time-nsecs-excl-vm | timer     | time spent in soroban host invocation excluding VM instantiation
soroban.host-fn-op.invoke-time-fsecs-cpu-insn-ratio         | histogram | ratio between soroban host invocation time (femto-seconds) and metered cpu instructions
soroban.host-fn-op.invoke-time-fsecs-cpu-insn-ratio-excl-vm | histogram | ratio between soroban host invocation time (femto-seconds) and metered cpu instructions, excluding VM instantiation
soroban.host-fn-op.max-rw-key-byte           | meter     | size of the largest `LedgerKey` (in bytes) among all entires accessed (read or modified) during the `InvokeHostFunctionOp`
soroban.host-fn-op.max-rw-data-byte          | meter     | size of the largest `ContractDataEntry` (in bytes) among all entires accessed (read or modified) during the `InvokeHostFunctionOp`
soroban.host-fn-op.max-rw-code-byte          | meter     | size of the largest `ContractCodeEntry` (in bytes) among all entires accessed (read or modified) during the `InvokeHostFunctionOp`
soroban.host-fn-op.max-emit-event-byte       | meter     | size of the largest event emitted during the `InvokeHostFunctionOp`
soroban.host-fn-op.success                   | meter     | number of successful `InvokeHostFunctionOp` operations
soroban.host-fn-op.failure                   | meter     | number of failed `InvokeHostFunctionOp` operations
soroban.host-fn-op.exec                      | timer     | total time spent during the `InvokeHostFunctionOp`
soroban.restore-fprint-op.read-ledger-byte   | meter     | number of `LedgerEntry` bytes accessed (read or modified) during the `RestoreFootprintOp`
soroban.restore-fprint-op.write-ledger-byte  | meter     | number of `LedgerEntry` bytes modified during the `RestoreFootprintOp`
soroban.restore-fprint-op.exec               | timer     | total time spent during the `RestoreFootprintOp`
soroban.ext-fprint-ttl-op.read-ledger-byte   | meter     | number of `LedgerEntry` bytes accessed (read or modified) during the `ExtendFootprintTTLOp`
soroban.ext-fprint-ttl-op.exec               | timer     | total time spent during the `ExtendFootprintTTLOp`
soroban.ledger.tx-count                      | histogram | number of soroban transactions per ledger
soroban.ledger.cpu-insn                      | histogram | total cpu instructions declared by soroban transactions per ledger
soroban.ledger.txs-size-byte                 | histogram | total size (in bytes) of soroban transactions per ledger
soroban.ledger.read-entry                    | histogram | number of accessed (read or modified) entries declared by soroban transactions per ledger
soroban.ledger.read-ledger-byte              | histogram | number of accessed (read or modified) `LedgerEntry` bytes declared by soroban transactions per ledger
soroban.ledger.write-entry                   | histogram | number of modified entries declared by soroban transactions per ledger
soroban.ledger.write-ledger-byte             | histogram | number of modified `LedgerEntry` bytes declared by soroban transactions per ledger
soroban.tx.size-byte                         | histogram | size (in bytes) of a soroban transaction
soroban.config.contract-max-rw-key-byte      | counter   | soroban config setting `contract_data_key_size_bytes`
soroban.config.contract-max-rw-data-byte     | counter   | soroban config setting `contract_data_entry_size_bytes`
soroban.config.contract-max-rw-code-byte     | counter   | soroban config setting `contract_max_size_bytes`
soroban.config.tx-max-size-byte              | counter   | soroban config setting `tx_max_size_bytes`
soroban.config.tx-max-cpu-insn               | counter   | soroban config setting `tx_max_instructions`
soroban.config.tx-max-mem-byte               | counter   | soroban config setting `tx_memory_limit`
soroban.config.tx-max-read-entry             | counter   | soroban config setting `tx_max_read_ledger_entries`
soroban.config.tx-max-read-ledger-byte       | counter   | soroban config setting `tx_max_read_bytes`
soroban.config.tx-max-write-entry            | counter   | soroban config setting `tx_max_write_ledger_entries`
soroban.config.tx-max-write-ledger-byte      | counter   | soroban config setting `tx_max_write_bytes`
soroban.config.tx-max-emit-event-byte        | counter   | soroban config setting `tx_max_contract_events_size_bytes`
soroban.config.ledger-max-tx-count           | counter   | soroban config setting `ledger_max_tx_count`
soroban.config.ledger-max-cpu-insn           | counter   | soroban config setting `ledger_max_instructions`
soroban.config.ledger-max-txs-size-byte      | counter   | soroban config setting `ledger_max_txs_size_bytes`
soroban.config.ledger-max-read-entry         | counter   | soroban config setting `ledger_max_read_ledger_entries`
soroban.config.ledger-max-read-ledger-byte   | counter   | soroban config setting `ledger_max_read_bytes`
soroban.config.ledger-max-write-entry        | counter   | soroban config setting `ledger_max_write_ledger_entries`
soroban.config.ledger-max-write-ledger-byte  | counter   | soroban config setting `ledger_max_write_bytes`
soroban.config.bucket-list-target-size-byte  | counter   | soroban config setting `bucket_list_target_size_bytes`