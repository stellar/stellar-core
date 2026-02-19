---
name: subsystem-summary-of-overlay
description: "read this skill for a token-efficient summary of the overlay subsystem"
---

# Overlay Subsystem — Technical Summary

## Overview

The overlay subsystem implements stellar-core's peer-to-peer network layer. It manages TCP connections to other nodes, authenticates peers via ECDH+HMAC, floods broadcast messages (transactions, SCP messages) across the network, fetches missing data (tx sets, quorum sets) via anycast requests, and performs network surveys. The subsystem supports optional background thread processing for I/O-heavy operations (reads/writes on TCP sockets) to keep the main thread responsive.

## Key Files

- **OverlayManager.h / OverlayManagerImpl.h/.cpp** — Central manager; owns peer lists, Floodgate, TxDemandsManager, SurveyManager, PeerManager, PeerAuth, PeerDoor.
- **Peer.h / Peer.cpp** — Abstract base class for a connected peer; handles message dispatch, HMAC auth, flow control, pull-mode adverts.
- **TCPPeer.h / TCPPeer.cpp** — Concrete `Peer` subclass; async TCP read/write via Asio, framing with RFC5531 record marking.
- **FlowControl.h / FlowControl.cpp** — Per-peer flow control for flood traffic; outbound queuing with priority and load shedding.
- **FlowControlCapacity.h / FlowControlCapacity.cpp** — Tracks message-count and byte-count capacity for reading/writing flood data.
- **Floodgate.h / Floodgate.cpp** — Tracks which peers have seen which broadcast messages; ensures each message is sent/received at most once per peer.
- **ItemFetcher.h / ItemFetcher.cpp** — Manages anycast fetch requests for tx sets and quorum sets via Tracker instances.
- **Tracker.h / Tracker.cpp** — Tracks a single fetch request; tries peers sequentially with timeout-based retries.
- **TxAdverts.h / TxAdverts.cpp** — Per-peer incoming/outgoing transaction hash advertisement queues (pull mode).
- **TxDemandsManager.h / TxDemandsManager.cpp** — Global transaction demand scheduling; issues FLOOD_DEMAND messages based on received adverts.
- **PeerManager.h / PeerManager.cpp** — Persists peer records (address, type, failure count, next-attempt time) in the database.
- **PeerDoor.h / PeerDoor.cpp** — Listens on the configured TCP port; accepts incoming connections and hands them to OverlayManager.
- **PeerAuth.h / PeerAuth.cpp** — ECDH key exchange and HMAC key derivation for peer authentication.
- **Hmac.h / Hmac.cpp** — Per-peer HMAC state for message authentication (send/recv MAC keys, sequence numbers).
- **PeerBareAddress.h / PeerBareAddress.cpp** — Value type for an IPv4 address + port, with DNS resolution.
- **PeerSharedKeyId.h / PeerSharedKeyId.cpp** — Cache key type for shared ECDH keys (remote public key + role).
- **RandomPeerSource.h / RandomPeerSource.cpp** — Loads random peers from PeerManager matching a query, with local caching.
- **BanManager.h / BanManagerImpl.h/.cpp** — Manages a persistent ban list of NodeIDs in the database.
- **SurveyManager.h / SurveyManager.cpp** — Orchestrates time-sliced overlay network surveys.
- **SurveyDataManager.h / SurveyDataManager.cpp** — Collects and finalizes per-node and per-peer survey data.
- **SurveyMessageLimiter.h / SurveyMessageLimiter.cpp** — Rate-limits and deduplicates survey messages.
- **OverlayMetrics.h / OverlayMetrics.cpp** — Central cache of medida metrics for the overlay (meters, timers, counters).
- **OverlayUtils.h / OverlayUtils.cpp** — Utility: `logErrorOrThrow` for error handling in overlay code.
- **StellarXDR.h** — Convenience include aggregating all XDR headers used by overlay.

---

## Key Classes and Data Structures

### `OverlayManager` (abstract interface)

Defines the public API for managing the overlay network. Created via `OverlayManager::create(app)`. Key pure virtual methods:
- `broadcastMessage(msg, hash)` — Flood a message to all authenticated peers.
- `recvFloodedMsgID(peer, msgID)` — Record that a peer sent us a flooded message.
- `recvTransaction(tx, peer, index)` — Process incoming transaction, pass to Herder.
- `recvTxDemand(dmd, peer)` — Process incoming demand for a transaction.
- `connectTo(address)` — Initiate outbound connection.
- `acceptAuthenticatedPeer(peer)` — Promote peer from pending to authenticated.
- `removePeer(peer)` — Remove a peer in CLOSING state.
- `clearLedgersBelow(ledgerSeq, lclSeq)` — Purge old Floodgate/ItemFetcher data.
- `start()` / `shutdown()` — Lifecycle control.
- `checkScheduledAndCache(tracker)` — Deduplicate messages already scheduled for processing.
- `getOverlayThreadSnapshot()` — Get/create a bucket list snapshot for the overlay background thread.

Static helpers: `isFloodMessage(msg)`, `createTxBatch()`, `getFlowControlBytesBatch(cfg)`.

### `OverlayManagerImpl` (concrete implementation)

Owns all major overlay components:
- `mInboundPeers`, `mOutboundPeers` — `PeersList` structs holding pending (vector) and authenticated (map by NodeID) peer collections, plus a `mDropped` set to extend lifetime until background I/O completes.
- `mFloodGate` (`Floodgate`) — Broadcast deduplication.
- `mTxDemandsManager` (`TxDemandsManager`) — Pull-mode demand scheduling.
- `mSurveyManager` (`shared_ptr<SurveyManager>`) — Network survey orchestration.
- `mPeerManager` (`PeerManager`) — Persistent peer record storage.
- `mDoor` (`PeerDoor`) — TCP listener.
- `mAuth` (`PeerAuth`) — Authentication key management.
- `mOverlayMetrics` (`OverlayMetrics`) — Metrics cache.
- `mMessageCache` (`RandomEvictionCache<uint64_t, bool>`) — Deduplicates received messages for metrics.
- `mScheduledMessages` (`RandomEvictionCache<Hash, weak_ptr<CapacityTrackedMessage>>`) — Tracks messages currently scheduled for processing to avoid duplicates.
- `mPeerSources` (`map<PeerType, unique_ptr<RandomPeerSource>>`) — Peer sources for INBOUND, OUTBOUND, PREFERRED types.
- `mResolvedPeers` (`future<ResolvedPeers>`) — Async DNS resolution result.
- `mOverlayThreadSnapshot` — Bucket list snapshot for overlay thread use only.

Inner struct `PeersList`:
- `mPending` (`vector<Peer::pointer>`) — Peers that have connected but not yet authenticated.
- `mAuthenticated` (`map<NodeID, Peer::pointer>`) — Fully authenticated peers.
- `mDropped` (`unordered_set<Peer::pointer>`) — Dropped peers kept alive until background I/O finishes.
- Methods: `byAddress()`, `removePeer()`, `moveToAuthenticated()`, `acceptAuthenticatedPeer()`, `shutdown()`.

### `Peer` (abstract base class)

Represents a single connected peer. Inherits `enable_shared_from_this`. Key state:
- `mState` — `CONNECTING`, `CONNECTED`, `GOT_HELLO`, `GOT_AUTH`, `CLOSING`. Protected by `mStateMutex` (recursive mutex).
- `mRole` — `WE_CALLED_REMOTE` or `REMOTE_CALLED_US` (const after construction).
- `mFlowControl` (`shared_ptr<FlowControl>`) — Per-peer flow control instance.
- `mTxAdverts` (`shared_ptr<TxAdverts>`) — Per-peer transaction advertisement state.
- `mHmac` (`Hmac`) — Per-connection HMAC keys and sequence counters.
- `mPeerMetrics` (`PeerMetrics`) — Atomic counters for per-peer statistics.
- `mSendNonce` / `mRecvNonce` — Random nonces for key derivation.
- `mPeerID` (`NodeID`) — Remote node's public key (set during HELLO).
- `mAddress` (`PeerBareAddress`) — Remote address.
- `mRecurringTimer` — Fires every 5s for ping, idle timeout, straggler checks.
- `mDelayedExecutionTimer` — One-shot timer for delayed operations.

Key methods:
- `sendMessage(msg)` — Enqueue a message for sending. Flood messages go through FlowControl; non-flood messages are sent directly via `sendAuthenticatedMessage()`.
- `recvRawMessage(tracker)` — Entry point for processing a received message from the background thread. Posts to main thread.
- `recvMessage(tracker)` — Main-thread message dispatch (called from main). Dispatches to `recvHello`, `recvAuth`, `recvTransaction`, `recvSCPMessage`, `recvFloodAdvert`, `recvFloodDemand`, etc.
- `beginMessageProcessing(msg)` / `endMessageProcessing(msg)` — Bracket message processing to track flow control capacity. `endMessageProcessing` may send `SEND_MORE_EXTENDED` to request more data.
- `sendHello()` / `sendAuth()` — Handshake messages.
- `shutdownAndRemovePeer(reason, direction)` — Set state to CLOSING, remove from OverlayManager.
- `maybeExecuteInBackground(name, f)` — Post work to overlay thread if background processing is enabled.
- Pull mode facade: `sendAdvert(hash)`, `sendTxDemand(demands)`, `retryAdvert(hashes)`, `hasAdvert()`, `popAdvert()`.
- `recurrentTimerExpired()` — Checks idle timeout, straggler timeout, and no-outbound-capacity timeout.

### `CapacityTrackedMessage`

RAII wrapper for a received `StellarMessage`. On construction, calls `Peer::beginMessageProcessing` to lock flow control capacity. On destruction, calls `Peer::endMessageProcessing` to release capacity and potentially send `SEND_MORE`. Also pre-computes BLAKE2 hash for SCP/TX messages and optionally pre-populates signature cache on the overlay thread.

Members:
- `mWeakPeer` — Weak reference to the owning Peer.
- `mMsg` — The StellarMessage.
- `mMaybeHash` — Optional BLAKE2 hash (for SCP_MESSAGE and TRANSACTION types).
- `mTxsMap` — Map from hash to pre-constructed `TransactionFrameBasePtr` (with pre-cached hashes).

### `TCPPeer` (concrete Peer subclass)

Implements TCP socket I/O using Asio's `buffered_read_stream`. Key details:
- `mSocket` (`shared_ptr<SocketType>`) — The Asio TCP socket with 256KB buffer.
- `ThreadRestrictedVars` — Inner class ensuring write queue, write buffers, and incoming header/body vectors are only accessed from the correct thread (overlay thread when background processing is enabled).
- `mWriteQueue` (`deque<TimestampedMessage>`) — Outgoing message queue.
- `mDropStarted` (`atomic<bool>`) — Ensures drop is initiated only once across threads.
- `mLiveInboundPeersCounter` (`shared_ptr<int>`) — Shared counter tracking live inbound TCPPeers for load shedding.

Static factory methods:
- `initiate(app, address)` — Create outbound connection; resolves address, calls `async_connect`.
- `accept(app, socket)` — Create from an accepted inbound socket; starts reading immediately.

Key methods:
- `sendMessage(xdrBytes, msg)` — Enqueues XDR bytes into `mWriteQueue`, calls `messageSender()`.
- `messageSender()` — Batches queued messages into `mWriteBuffers`, calls `async_write`.
- `scheduleRead()` / `startRead()` — Initiates async read of 4-byte header, then body.
- `readHeaderHandler()` / `readBodyHandler()` — Process received data; construct `CapacityTrackedMessage`, call `recvRawMessage`.
- `writeHandler()` — Completes write, processes sent messages via `FlowControl::processSentMessages`, sends next batch.
- `drop(reason, direction)` — Atomic drop initiation; shuts down socket, posts cleanup to main thread.

### `FlowControl` (thread-safe)

Per-peer flow control managing both inbound capacity tracking and outbound message queuing. Protected by `mFlowControlMutex`.

Key state:
- `mFlowControlCapacity` (`FlowControlMessageCapacity`) — Tracks message-count capacity.
- `mFlowControlBytesCapacity` (`FlowControlByteCapacity`) — Tracks byte-count capacity.
- `mOutboundQueues` (`FloodQueues<QueuedOutboundMessage>`, array of 4 deques) — Priority-ordered: [0] SCP, [1] transactions, [2] demands, [3] adverts.
- `mTxQueueByteCount`, `mAdvertQueueTxHashCount`, `mDemandQueueTxHashCount` — Size trackers for load shedding.
- `mFloodDataProcessed` / `mFloodDataProcessedBytes` — Counters since last SEND_MORE.
- `mLastThrottle` — Timestamp when reading was last throttled.
- `mNoOutboundCapacity` — Timestamp when outbound capacity was last exhausted.

Key methods:
- `addMsgAndMaybeTrimQueue(msg)` — Add flood message to appropriate priority queue; shed oldest transactions if byte limit exceeded; shed excess adverts/demands.
- `getNextBatchToSend()` — Dequeue messages across all priorities while outbound capacity is available; lock capacity for each sent message.
- `beginMessageProcessing(msg)` — Lock local reading capacity for an incoming message.
- `endMessageProcessing(msg)` — Release local capacity; return `SendMoreCapacity` indicating how much to request from the peer.
- `maybeThrottleRead()` — If local capacity is exhausted, mark peer as throttled.
- `stopThrottling()` — Resume reading from a throttled peer.
- `processSentMessages(sentMessages)` — After async_write completes, remove sent messages from front of queues and update size trackers.
- `isSendMoreValid(msg, errorMsg)` — Validate a received SEND_MORE message.

### `FlowControlCapacity` (abstract base)

Base class for capacity tracking. Two subclasses:
- `FlowControlMessageCapacity` — Tracks by message count. Capacity limits come from config (`PEER_FLOOD_READING_CAPACITY`).
- `FlowControlByteCapacity` — Tracks by byte count. Limits come from `OverlayManager::getFlowControlBytesTotal()`. Supports `handleTxSizeIncrease()` for protocol upgrades that increase max tx size.

Both track:
- `mCapacity.mFloodCapacity` — Local reading capacity for flood messages.
- `mCapacity.mTotalCapacity` — Optional total capacity (flood + non-flood).
- `mOutboundCapacity` — How much the remote peer has allowed us to send.

### `Floodgate`

Ensures each broadcast message is sent/received at most once per peer. Uses a `FloodRecord` per message hash.

Key state:
- `mFloodMap` (`map<Hash, FloodRecord::pointer>`) — Hash of `StellarMessage` → record of which peers have been told.

Key methods:
- `addRecord(peer, msgID)` — Record that `peer` sent us message with hash `msgID`. Returns true if new.
- `broadcast(msg, hash)` — Send message to all authenticated peers that haven't been told. For transactions (pull mode), sends adverts instead of the full message.
- `clearBelow(maxLedger)` — Remove records for ledgers older than `maxLedger`.
- `getPeersKnows(msgID)` — Return set of peers that have seen a given message.
- `forgetRecord(msgID)` — Remove a record (e.g., when tx is rejected).

### `ItemFetcher`

Manages fetching of tx sets and quorum sets via anycast. One `ItemFetcher` per item type, each with a configurable `AskPeer` delegate (e.g., `sendGetTxSet` or `sendGetQuorumSet`).

Key state:
- `mTrackers` (`map<Hash, shared_ptr<Tracker>>`) — One Tracker per item hash being fetched.

Key methods:
- `fetch(hash, envelope)` — Start or join fetching of item `hash` needed by SCP `envelope`.
- `stopFetch(hash, envelope)` — Remove interest from a specific envelope.
- `recv(hash, timer)` — Item received; cancel tracker, re-process waiting envelopes via Herder.
- `doesntHave(hash, peer)` — Peer reported DONT_HAVE; try next peer.
- `stopFetchingBelow(slotIndex, slotToKeep)` — Cleanup old trackers.

### `Tracker`

Tracks a single item fetch across multiple peers. Tries peers sequentially with 1.5s timeout per attempt.

Key state:
- `mPeersAsked` (`map<Peer::pointer, bool>`) — Which peers have been tried.
- `mWaitingEnvelopes` (`vector<pair<Hash, SCPEnvelope>>`) — Envelopes waiting for this data.
- `mTimer` — Timeout timer for current fetch attempt.
- `mNumListRebuild` — Number of times the peer list has been rebuilt (max 20 tries).

Key methods:
- `tryNextPeer()` — Pick an authenticated peer that hasn't been tried (or rebuild list), send request via `mAskPeer` delegate, start timeout timer.
- `doesntHave(peer)` — Mark peer and try next.
- `listen(env)` / `discard(env)` — Add/remove envelopes from wait list.
- `cancel()` — Stop timer and fetching.

### `TxAdverts`

Per-peer transaction advertisement management. Handles both incoming adverts (hashes to demand) and outgoing adverts (hashes to advertise).

Key state:
- `mIncomingTxHashes` (`deque<Hash>`) — FIFO queue of hashes received from this peer.
- `mTxHashesToRetry` (`list<Hash>`) — Hashes to retry demanding.
- `mAdvertHistory` (`RandomEvictionCache<Hash, uint32_t>`) — Seen hash cache (50k entries).
- `mOutgoingTxHashes` (`TxAdvertVector`) — Batch of hashes to advertise to this peer.
- `mAdvertTimer` — Periodic flush timer.
- `mSendCb` — Callback to send the advert message.

Key methods:
- `queueOutgoingAdvert(hash)` — Add hash to outgoing batch; flush if batch is full or timer fires.
- `queueIncomingAdvert(hashes, seq)` — Deduplicate and enqueue incoming hashes.
- `popIncomingAdvert()` — Pop next hash (retries first, then incoming queue).
- `retryIncomingAdvert(list)` — Re-queue hashes for retry after failed demand.
- `seenAdvert(hash)` — Check if hash was already seen.
- `clearBelow(ledgerSeq)` — Remove stale advert history entries.

### `TxDemandsManager`

Global demand scheduling for pull-mode transactions. Runs on a periodic timer (`FLOOD_DEMAND_PERIOD_MS`, default 200ms).

Key state:
- `mDemandHistoryMap` (`UnorderedMap<Hash, DemandHistory>`) — Tracks per-hash demand history (peers tried, timestamps, retry count).
- `mPendingDemands` (`queue<Hash>`) — FIFO of all demanded hashes for cleanup.
- `mDemandTimer` — Periodic demand timer.

Key methods:
- `demand()` — Main demand loop: iterates over authenticated peers with pending adverts, determines demand status per hash (DEMAND / RETRY_LATER / DISCARD), batches demands, sends `FLOOD_DEMAND` messages. Uses linear backoff up to 2s between retries, max 15 retry attempts.
- `recvTxDemand(dmd, peer)` — Process incoming demand: look up transactions in Herder, send back if available; track metrics for fulfilled/unfulfilled demands.
- `recordTxPullLatency(hash, peer)` — Record latency from first demand to receipt.

### `PeerManager`

Persists peer records in the database. Each peer record stores: address (IP:port), number of failures, next attempt time, type (inbound/outbound/preferred).

Key methods:
- `ensureExists(address)` — Insert if not present.
- `update(address, type, preferredTypeKnown, backOff)` — Update type and/or backoff. Type transitions: outbound→preferred (upgrade), preferred→outbound (downgrade only if definitely not preferred).
- `loadRandomPeers(query, size)` — Load random peers matching criteria from DB.
- `removePeersWithManyFailures(minFailures)` — Purge dead peers.
- `getPeersToSend(size, address)` — Select peers to recommend to a requesting peer.

### `PeerDoor`

TCP listener using Asio acceptor. Calls `TCPPeer::accept()` to create inbound peers, then `OverlayManager::maybeAddInboundConnection()` to register them.

### `PeerAuth`

Handles per-connection authentication key derivation:
- Generates ephemeral Curve25519 keypair on startup.
- Creates `AuthCert` (signed ephemeral public key with expiration).
- Derives shared HMAC keys via: `HKDF(ECDH(local_secret, remote_public) || local_pub || remote_pub)`, then per-session send/recv keys via `HKDF_expand` with nonces.
- Uses `RandomEvictionCache` for shared key caching.

### `Hmac`

Per-connection HMAC state (thread-safe via mutex):
- `mSendMacKey` / `mRecvMacKey` — HMAC-SHA256 keys.
- `mSendMacSeq` / `mRecvMacSeq` — Monotonic sequence numbers preventing replay.
- `checkAuthenticatedMessage()` — Verify incoming message MAC and sequence.
- `setAuthenticatedMessageBody()` — Compute and set MAC on outgoing message.

### `SurveyManager`

Orchestrates time-sliced network surveys. Supports two phases: Collecting (gathering data) and Reporting (answering queries).

Key state:
- `mSurveyDataManager` (`SurveyDataManager`) — Manages collected data.
- `mMessageLimiter` (`SurveyMessageLimiter`) — Rate-limits survey messages.
- `mPeersToSurveyQueue` — Queue of nodes to survey.
- `mRunningSurveyReportingPhase` — Whether in reporting phase.
- `mCurve25519SecretKey/PublicKey` — Keys for encrypting survey responses.

Key methods:
- `broadcastStartSurveyCollecting(nonce)` / `broadcastStopSurveyCollecting()` — Start/stop collecting phase.
- `startSurveyReporting()` / `stopSurveyReporting()` — Start/stop reporting phase.
- `addNodeToRunningSurveyBacklog(node, inIdx, outIdx)` — Queue a node for surveying.
- `relayOrProcessRequest/Response(msg, peer)` — Route survey messages.
- `updateSurveyPhase(...)` — Called from OverlayManager tick to check phase transitions/timeouts.

### `SurveyDataManager`

Thread-safe data collection for time-sliced surveys.

Key state:
- `mCollectingNodeData` (`optional<CollectingNodeData>`) — Node-level stats during collecting.
- `mCollectingInboundPeerData` / `mCollectingOutboundPeerData` (`unordered_map<NodeID, CollectingPeerData>`) — Per-peer stats during collecting.
- `mFinalNodeData`, `mFinalInboundPeerData`, `mFinalOutboundPeerData` — Finalized data for reporting.
- `mPhase` — `COLLECTING`, `REPORTING`, or `INACTIVE`.

### `RandomPeerSource`

Loads random peers from PeerManager matching a query. Maintains a local cache that is refreshed from the database when exhausted.

### `BanManager` / `BanManagerImpl`

Persistent ban list stored in the database. Methods: `banNode(id)`, `unbanNode(id)`, `isBanned(id)`, `getBans()`.

### `OverlayMetrics`

Centralized cache of medida metrics for the overlay. Thread-safe (medida is thread-safe). Groups meters/timers/counters for: message read/write, byte read/write, async I/O, per-message-type recv/send timers, connection latency, flow control throttle, outbound queue delays/drops, flood bytes (unique/duplicate), demand/pull metrics.

---

## Key Control Loops, Threads, and Tasks

### Main Thread (`tick()` loop)

`OverlayManagerImpl::tick()` runs every `PEER_AUTHENTICATION_TIMEOUT + 1` seconds (default 3s):
1. Cleans up unreferenced dropped peers (use_count == 1).
2. Checks if DNS resolution future is ready; stores resolved peers, schedules next resolution.
3. Updates survey phase via `SurveyManager::updateSurveyPhase()`.
4. Connects to preferred peers (highest priority).
5. If out of sync, may randomly drop a non-preferred outbound peer.
6. Connects to outbound peers (from DB).
7. Attempts to promote inbound peers to outbound.

### Overlay Background Thread

When `BACKGROUND_OVERLAY_PROCESSING` is enabled, TCP socket I/O (async_read, async_write) runs on a dedicated overlay thread (`Application::getOverlayIOContext()`). Key operations on overlay thread:
- `TCPPeer::readHeaderHandler()` / `readBodyHandler()` — Read messages from socket.
- `TCPPeer::writeHandler()` — Process write completions, call `FlowControl::processSentMessages()`.
- `TCPPeer::messageSender()` — Batch and send queued messages.
- `CapacityTrackedMessage` constructor — Pre-parses transactions, optionally verifies signatures in background.
- `Peer::recvRawMessage()` — Posts received message to main thread for processing.

### Peer Recurrent Timer

Each Peer runs a 5-second recurring timer (`startRecurrentTimer()`) checking:
- Idle timeout: no read/write for `PEER_TIMEOUT` seconds (authenticated) or `PEER_AUTHENTICATION_TIMEOUT` (pending).
- Straggler timeout: last write enqueue too old (`PEER_STRAGGLER_TIMEOUT`).
- Flow control timeout: no outbound capacity for `PEER_SEND_MODE_IDLE_TIMEOUT` (60s).

### Demand Timer

`TxDemandsManager::demand()` fires every `FLOOD_DEMAND_PERIOD_MS` (default 200ms):
1. Purges obsolete demand history entries.
2. Iterates over authenticated peers with pending adverts.
3. For each hash, checks demand status (demand/retry/discard).
4. Batches demands up to `getMaxDemandSize()`, sends `FLOOD_DEMAND` to peer.
5. Handles retry failures by requeueing hashes via `peer->retryAdvert()`.

### Advert Timer

Per-peer `TxAdverts::flushAdvert()` fires after `FLOOD_ADVERT_PERIOD_MS` or when batch is full. Sends accumulated outgoing adverts as a single `FLOOD_ADVERT` message.

### DNS Resolution

`triggerPeerResolution()` resolves `KNOWN_PEERS` and `PREFERRED_PEERS` on a background thread. Results are picked up in `tick()` and stored via `storePeerList()`. Retry with backoff on failure; resolves again every 600s on success.

---

## Ownership Relationships

```
Application
 └─ OverlayManagerImpl (unique_ptr)
     ├─ PeerDoor (value) — TCP acceptor
     ├─ PeerAuth (value) — authentication key manager
     ├─ PeerManager (value) — database peer records
     ├─ Floodgate (value) — broadcast deduplication
     ├─ TxDemandsManager (value) — demand scheduling
     ├─ SurveyManager (shared_ptr)
     │   ├─ SurveyDataManager (value)
     │   └─ SurveyMessageLimiter (value)
     ├─ OverlayMetrics (value)
     ├─ PeersList mInboundPeers (value)
     │   ├─ mPending: vector<Peer::pointer>
     │   ├─ mAuthenticated: map<NodeID, Peer::pointer>
     │   └─ mDropped: unordered_set<Peer::pointer>
     ├─ PeersList mOutboundPeers (value) — same structure
     └─ RandomPeerSource[3] (unique_ptr per PeerType)

Peer (shared_ptr, TCPPeer concrete)
 ├─ FlowControl (shared_ptr)
 │   ├─ FlowControlMessageCapacity (value)
 │   └─ FlowControlByteCapacity (value)
 ├─ TxAdverts (shared_ptr)
 ├─ Hmac (value)
 ├─ PeerMetrics (value)
 └─ TCPPeer::SocketType (shared_ptr) — Asio socket
```

---

## Key Data Flows

### Connection Handshake
1. Initiator calls `TCPPeer::initiate()` → `async_connect` → `connectHandler` → `sendHello()`.
2. Responder: `PeerDoor::acceptNextPeer()` → `TCPPeer::accept()` → `maybeAddInboundConnection()` → `startRead()`.
3. Both sides: `recvHello()` validates version, network ID, addresses → `recvAuth()` sets up HMAC keys via `PeerAuth`, calls `acceptAuthenticatedPeer()` → `moveToAuthenticated()`.
4. After auth: peers send `SEND_MORE_EXTENDED` to indicate initial reading capacity, exchange peer lists, start adverts.

### Transaction Flooding (Pull Mode)
1. Herder calls `OverlayManager::broadcastMessage(tx_msg, hash)`.
2. `Floodgate::broadcast()` sends `FLOOD_ADVERT` (hash only) to each peer not already told.
3. Per-peer `TxAdverts::queueOutgoingAdvert()` batches hashes; flushed on timer or batch full.
4. Remote peer receives advert → `Peer::recvFloodAdvert()` → `TxAdverts::queueIncomingAdvert()`.
5. `TxDemandsManager::demand()` timer fires → picks hashes from peers → sends `FLOOD_DEMAND`.
6. Remote peer receives demand → `TxDemandsManager::recvTxDemand()` → looks up tx in Herder → sends full `TRANSACTION` message back.
7. `Peer::recvTransaction()` → `OverlayManager::recvTransaction()` → `Herder::recvTransaction()`.

### SCP Message Flooding (Push Mode)
1. Herder calls `broadcastMessage(scp_msg)`.
2. `Floodgate::broadcast()` sends full `SCP_MESSAGE` to all peers not yet told.
3. Messages go through `FlowControl::addMsgAndMaybeTrimQueue()` with priority 0 (highest).
4. Remote peer receives → `recvSCPMessage()` → posted to main thread → dispatched to Herder.

### Anycast Fetch (TX Sets / Quorum Sets)
1. Herder needs a tx set or quorum set → calls `ItemFetcher::fetch(hash, envelope)`.
2. `ItemFetcher` creates/reuses a `Tracker` for the hash.
3. `Tracker::tryNextPeer()` picks a peer, sends `GET_TX_SET` or `GET_SCP_QUORUMSET`.
4. Remote peer responds with the data, or `DONT_HAVE`.
5. On `DONT_HAVE`: `Tracker::doesntHave()` → try next peer.
6. On receipt: `ItemFetcher::recv()` → cancel tracker → re-submit waiting envelopes to Herder.

### Flow Control
1. On connection, both sides start with initial flood reading capacity (messages + bytes).
2. When peer sends flood data, it consumes outbound capacity (`lockOutboundCapacity`).
3. Receiver processes message, releasing local capacity via `endMessageProcessing()`.
4. When enough capacity freed (batch threshold), receiver sends `SEND_MORE_EXTENDED(numMessages, numBytes)`.
5. Sender receives `SEND_MORE_EXTENDED` → `FlowControl::maybeReleaseCapacity()` → unlocks outbound capacity → can send more.
6. If outbound capacity exhausted, messages queue up. Oldest tx messages are shed if byte limit exceeded.
7. If reading capacity exhausted, `maybeThrottleRead()` stops scheduling reads until capacity is freed.
