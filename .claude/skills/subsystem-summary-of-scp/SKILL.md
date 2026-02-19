---
name: subsystem-summary-of-scp
description: "read this skill for a token-efficient summary of the scp subsystem"
---

# SCP Subsystem — Technical Summary

## Overview

The SCP (Stellar Consensus Protocol) subsystem implements the federated Byzantine agreement protocol used by stellar-core to reach consensus on ledger values. It is a self-contained library with a clean driver interface (`SCPDriver`) that decouples protocol logic from networking, persistence, and application-specific validation. The protocol operates in two main stages: **nomination** (proposing and filtering candidate values) and **balloting** (converging on a single value through prepare/confirm/externalize phases).

## Key Files

- **SCP.h / SCP.cpp** — Top-level `SCP` class; entry point for receiving envelopes, nominating values, managing slots.
- **SCPDriver.h / SCPDriver.cpp** — Abstract driver interface that the application implements; handles signing, validation, timers, quorum set retrieval, hashing, and event callbacks.
- **Slot.h / Slot.cpp** — Per-slot state container; owns `NominationProtocol` and `BallotProtocol` instances.
- **NominationProtocol.h / NominationProtocol.cpp** — Nomination phase logic: voting for values, federated acceptance, candidate promotion.
- **BallotProtocol.h / BallotProtocol.cpp** — Ballot phase logic: prepare/confirm/externalize state machine, ballot bumping, timer management.
- **LocalNode.h / LocalNode.cpp** — Represents the local node; owns quorum set, provides quorum slice / v-blocking set checks.
- **QuorumSetUtils.h / QuorumSetUtils.cpp** — Quorum set sanity checking and normalization utilities.

---

## Key Classes and Data Structures

### `SCP`

The top-level protocol object. One instance per node.

**Members:**
- `mDriver` (`SCPDriver&`) — Reference to the application-provided driver.
- `mLocalNode` (`shared_ptr<LocalNode>`) — The local node descriptor with its quorum set.
- `mKnownSlots` (`map<uint64, shared_ptr<Slot>>`) — Map from slot index to `Slot` objects. Slots are created lazily on first access via `getSlot()`.

**Key Methods:**
- `receiveEnvelope(SCPEnvelopeWrapperPtr)` — Main entry point. Routes envelope to the appropriate `Slot::processEnvelope()`.
- `nominate(slotIndex, value, previousValue)` — Initiates nomination for a slot (must be validator).
- `stopNomination(slotIndex)` — Stops nomination for a slot.
- `purgeSlots(maxSlotIndex, slotToKeep)` — Removes old slots below `maxSlotIndex` except `slotToKeep`.
- `getSlot(slotIndex, create)` — Lazily creates and retrieves `Slot` from `mKnownSlots`.
- `setStateFromEnvelope(slotIndex, e)` — Restores state from a previously emitted envelope (crash recovery).
- `processCurrentState(slotIndex, f, forceSelf)` — Iterates over latest messages for a slot.
- `processSlotsAscendingFrom / processSlotsDescendingFrom` — Iteration helpers over known slots.
- `getExternalizingState(slotIndex)` — Returns envelopes that contributed to externalization.
- `getState(node, slotIndex)` — Computes `QuorumInfoNodeState` for a node, checking up to `NUM_SLOTS_TO_CHECK_FOR_REPORTING` (2) recent slots.
- `getJsonQuorumInfo(id, summary, fullKeys, index)` — JSON diagnostic info categorizing quorum nodes as AGREE/MISSING/DELAYED/DISAGREE.
- `envToStr(envelope/statement)` — Formats SCP envelopes/statements as human-readable strings for logging.

### `SCPDriver` (Abstract)

The application-facing interface. Stellar-core's `HerderSCPDriver` implements this.

**Pure Virtual Methods (must be implemented):**
- `signEnvelope(SCPEnvelope&)` — Sign an outgoing envelope.
- `getQSet(Hash)` — Retrieve a quorum set by hash (return `nullptr` for unknown/invalid).
- `emitEnvelope(SCPEnvelope)` — Broadcast an envelope to the network.
- `getHashOf(vector<opaque_vec<>>)` — Compute a hash of serialized data.
- `combineCandidates(slotIndex, candidates)` — Produce a composite value from candidate set (used when transitioning from nomination to balloting).
- `hasUpgrades(Value)` — Check if a value contains protocol upgrades.
- `stripAllUpgrades(Value)` — Remove all upgrades from a value.
- `getUpgradeNominationTimeoutLimit()` — Max nomination timeouts before stripping upgrades.
- `setupTimer(slotIndex, timerID, timeout, cb)` / `stopTimer(slotIndex, timerID)` — Timer management.
- `computeTimeout(roundNumber, isNomination)` — Compute timeout for a round.

**Virtual Methods with Defaults:**
- `validateValue(slotIndex, value, nomination)` — Returns `kMaybeValidValue` by default. Three levels: `kInvalidValue`, `kMaybeValidValue`, `kFullyValidatedValue`.
- `extractValidValue(slotIndex, value)` — Extract a valid variant from an invalid value (returns `nullptr` by default).
- `wrapEnvelope(e)` / `wrapValue(v)` — Factory methods for `SCPEnvelopeWrapper` / `ValueWrapper` (allow subclasses to add metadata).
- `computeHashNode(slotIndex, prev, isPriority, roundNumber, nodeID)` — Hash for nomination leader election.
- `computeValueHash(slotIndex, prev, roundNumber, value)` — Hash for value ordering during nomination.
- `getNodeWeight(nodeID, qset, isLocalNode)` — Compute weight of a node within a quorum set (normalized 0–UINT64_MAX). Local node always gets `UINT64_MAX`. For other nodes, weight is `threshold/total * leafWeight` recursively through inner sets.
- Event callbacks: `valueExternalized`, `nominatingValue`, `updatedCandidateValue`, `startedBallotProtocol`, `acceptedBallotPrepared`, `confirmedBallotPrepared`, `acceptedCommit`, `ballotDidHearFromQuorum`.

### `Slot`

Per-slot state container. Each slot tracks one consensus round (one ledger sequence number).

**Members:**
- `mSlotIndex` (`uint64 const`) — The slot/ledger index.
- `mSCP` (`SCP&`) — Back-reference to the owning `SCP` instance.
- `mBallotProtocol` (`BallotProtocol`) — Owns ballot protocol state (value, not pointer).
- `mNominationProtocol` (`NominationProtocol`) — Owns nomination protocol state (value, not pointer).
- `mStatementsHistory` (`vector<HistoricalStatement>`) — Debug log of all statements seen.
- `mFullyValidated` (`bool`) — True if all values processed by this slot have been fully validated.
- `mGotVBlocking` (`bool`) — True once messages from a v-blocking set have been received.

**Key Methods:**
- `processEnvelope(envelope, self)` — Dispatches to `NominationProtocol` or `BallotProtocol` based on statement type (`SCP_ST_NOMINATE` vs ballot types).
- `nominate(value, previousValue, timedout)` — Delegates to `NominationProtocol::nominate()`.
- `bumpState(value, force)` — Delegates to `BallotProtocol::bumpState()`.
- `federatedAccept(voted, accepted, envs)` — Checks if a statement should be accepted: true if either (a) a v-blocking set accepted it, or (b) a quorum voted-or-accepted it.
- `federatedRatify(voted, envs)` — Checks if a statement is ratified: true if a quorum voted for it.
- `getQuorumSetFromStatement(st)` — Retrieves quorum set for a statement; for `EXTERNALIZE` statements, returns the singleton `{nodeID}` set.
- `createEnvelope(statement)` — Wraps a statement into a signed envelope.
- `getCompanionQuorumSetHashFromStatement(st)` — Static; extracts the quorum set hash from any statement type (note: `EXTERNALIZE` uses `commitQuorumSetHash`).
- `maybeSetGotVBlocking()` — Checks if messages from a v-blocking set have been received.

### `NominationProtocol`

Implements the nomination phase of SCP. Votes for values, promotes them through federated accept and ratify, and produces candidate values.

**Members:**
- `mSlot` (`Slot&`) — Back-reference.
- `mRoundNumber` (`int32`) — Current nomination round (incremented on each `nominate()` call).
- `mVotes` (`ValueWrapperPtrSet`, paper variable X) — Values this node has voted to nominate.
- `mAccepted` (`ValueWrapperPtrSet`, paper variable Y) — Values accepted as nominated.
- `mCandidates` (`ValueWrapperPtrSet`, paper variable Z) — Values confirmed nominated (candidates).
- `mLatestNominations` (`map<NodeID, SCPEnvelopeWrapperPtr>`, paper variable N) — Latest nomination envelope per node.
- `mLastEnvelope` (`SCPEnvelopeWrapperPtr`) — Last envelope emitted by this node.
- `mRoundLeaders` (`set<NodeID>`) — Nodes with highest priority this round.
- `mNominationStarted` (`bool`) — Whether `nominate()` has been called.
- `mLatestCompositeCandidate` (`ValueWrapperPtr`) — Latest composite candidate value (from `combineCandidates`).
- `mPreviousValue` (`Value`) — Value from the previous slot (used for leader hashing).
- `mTimerExpCount` (`uint32_t`) — Number of timer expirations (used for reporting and upgrade timeout logic).

**Key Methods:**
- `nominate(value, previousValue, timedout)` — Main entry. Increments round, updates leaders, adds votes from leaders, optionally adds own value (if self is leader). Strips upgrades after exceeding `getUpgradeNominationTimeoutLimit()`. Arms a timer to re-invoke itself on timeout. Stops nominating once candidates exist.
- `processEnvelope(envelope)` — Processes a nomination message from another node. For each voted value, checks `federatedAccept(voted, accepted)`; if accepted, adds to `mAccepted`. For each accepted value, checks `federatedRatify(accepted)`; if ratified, promotes to `mCandidates`. When candidates found, stops timer, calls `combineCandidates`, and triggers `bumpState` on the ballot protocol.
- `updateRoundLeaders()` — Computes which nodes have priority this round using `hashNode(priority)`. Includes self. Fast-forwards rounds if no node has priority.
- `getNewValueFromNomination(nom)` — Extracts the highest-hash value from a nomination that the local node doesn't already have, preferring accepted values. Validates or extracts valid values.
- `emitNomination()` — Creates and emits a `SCP_ST_NOMINATE` statement containing current votes and accepted values.
- `hashNode(isPriority, nodeID)` / `hashValue(value)` — Delegate to `SCPDriver::computeHashNode` / `computeValueHash`.
- `getNodePriority(nodeID, qset)` — Computes priority: if `hashNode(N, nodeID) <= weight`, returns `hashNode(P, nodeID)`, else 0.
- `stripUpgrades(value)` — Calls `SCPDriver::stripAllUpgrades` to remove upgrades from a value when timeouts exceed threshold.
- `stopNomination()` — Sets `mNominationStarted = false`.
- `getState(node, selfAlreadyMovedOn)` — Categorizes node as AGREE/DELAYED/DISAGREE/MISSING based on accepted value comparison.
- `isNewerStatement(old, new)` — A nomination statement is newer if its votes and accepted sets are (non-strict) supersets with at least one being strictly larger.

### `BallotProtocol`

Implements the ballot phase of SCP with three sub-phases: PREPARE, CONFIRM, EXTERNALIZE.

**Members:**
- `mSlot` (`Slot&`) — Back-reference.
- `mHeardFromQuorum` (`bool`) — Whether a quorum at the current ballot counter has been heard.
- `mPhase` (`SCPPhase`) — Current phase: `SCP_PHASE_PREPARE`, `SCP_PHASE_CONFIRM`, or `SCP_PHASE_EXTERNALIZE`.
- `mCurrentBallot` (`SCPBallotWrapperUPtr`, paper variable b) — Current ballot.
- `mPrepared` (`SCPBallotWrapperUPtr`, paper variable p) — Highest accepted-prepared ballot.
- `mPreparedPrime` (`SCPBallotWrapperUPtr`, paper variable p') — Second-highest accepted-prepared ballot, incompatible with p.
- `mHighBallot` (`SCPBallotWrapperUPtr`, paper variable h) — Highest confirmed-prepared ballot.
- `mCommit` (`SCPBallotWrapperUPtr`, paper variable c) — Commit ballot.
- `mLatestEnvelopes` (`map<NodeID, SCPEnvelopeWrapperPtr>`, paper variable M) — Latest ballot envelope per node.
- `mValueOverride` (`ValueWrapperPtr`, paper variable z) — Value override set when h is confirmed prepared; ensures this value is used for subsequent ballots.
- `mCurrentMessageLevel` (`int`) — Recursion depth counter for `advanceSlot`, capped at `MAX_ADVANCE_SLOT_RECURSION` (50).
- `mTimerExpCount` (`uint32_t`) — Number of ballot timer expirations.
- `mLastEnvelope` / `mLastEnvelopeEmit` — Track last generated and last emitted envelopes.

**Inner Class: `SCPBallotWrapper`**
Pairs a `ValueWrapperPtr` with an `SCPBallot` to keep shared ownership of the value. Used via `unique_ptr<SCPBallotWrapper>` (`SCPBallotWrapperUPtr`).

**Key Methods — State Machine (`advanceSlot`):**
- `advanceSlot(hint)` — The core state machine driver. Called after each envelope is recorded. Sequentially attempts each progression step:
  1. `attemptAcceptPrepared(hint)` — Steps 1,5: Check if any ballot can be federatedAccept-ed as prepared.
  2. `attemptConfirmPrepared(hint)` — Steps 2,3,8: Check if any prepared ballot can be federatedRatify-ed (confirmed prepared). If so, sets h, c, and the value override.
  3. `attemptAcceptCommit(hint)` — Steps 4,6,8: Check if commit can be federatedAccept-ed. Transitions PREPARE→CONFIRM phase.
  4. `attemptConfirmCommit(hint)` — Steps 7,8: Check if commit can be federatedRatify-ed. Transitions to EXTERNALIZE phase, calls `valueExternalized`.
  5. `attemptBump()` — Step 9: If a v-blocking subset has higher counters, bump local counter to the minimum counter that eliminates this condition.
  After all attempts complete, calls `checkHeardFromQuorum()` and `sendLatestEnvelope()`.

**Key Methods — State Setters:**
- `setAcceptPrepared(ballot)` — Updates p/p', clears c if p/p' conflict with h.
- `setConfirmPrepared(newC, newH)` — Sets h, optionally c; sets `mValueOverride`; updates current ballot if needed.
- `setAcceptCommit(c, h)` — Sets c/h; transitions phase to CONFIRM if in PREPARE; updates current ballot.
- `setConfirmCommit(c, h)` — Transitions to EXTERNALIZE phase; calls `valueExternalized`; stops nomination.

**Key Methods — Ballot Management:**
- `bumpState(value, force)` — Creates a new ballot at counter+1 (or 1 if no current ballot). Uses `mValueOverride` if set.
- `bumpToBallot(ballot, check)` — Low-level ballot update; resets h/c if incompatible; resets `mHeardFromQuorum`.
- `updateCurrentValue(ballot)` — Updates current ballot with checks; calls `bumpToBallot`.
- `abandonBallot(n)` — Bumps to ballot counter n (or counter+1 if n=0) using latest composite candidate value.
- `ballotProtocolTimerExpired()` — Increments timer count, calls `abandonBallot(0)`.
- `startBallotProtocolTimer()` / `stopBallotProtocolTimer()` — Manage the ballot protocol timer.

**Key Methods — Predicates and Helpers:**
- `isNewerStatement(old, new)` — Total ordering: PREPARE < CONFIRM < EXTERNALIZE; within same type, lexicographic on (b, p, p', h).
- `isStatementSane(st, self)` — Validates structural invariants of each statement type (counter > 0, c ≤ h ≤ b, etc.).
- `hasPreparedBallot(ballot, st)` — Checks if a statement implies `ballot` is prepared.
- `commitPredicate(ballot, interval, st)` — Checks if a statement commits `ballot` within `[interval.first, interval.second]`.
- `compareBallots(b1, b2)` — Orders ballots by (counter, value). Returns -1/0/1.
- `areBallotsCompatible(b1, b2)` — True if `b1.value == b2.value`.
- `areBallotsLessAndCompatible/Incompatible` — Combined comparisons.
- `getPrepareCandidates(hint)` — Collects ballots from all known envelopes that might be prepared.
- `getCommitBoundariesFromStatements(ballot)` — Collects counter boundaries for commit interval search.
- `findExtendedInterval(candidate, boundaries, pred)` — Scans boundaries top-down to find the widest [low,high] interval satisfying the predicate.
- `validateValues(st)` — Validates all values in a statement; returns the minimum validation level.
- `checkHeardFromQuorum()` — Checks if a quorum at the current ballot counter has been heard; starts/stops timer accordingly; invokes `ballotDidHearFromQuorum` callback.
- `emitCurrentStateStatement()` — Creates statement for current phase, processes it self, emits if newer.
- `checkInvariants()` — Debug assertions: in CONFIRM/EXTERNALIZE, b/p/c/h must all be set; p' < p and incompatible; h ≤ b and compatible; c ≤ h.
- `createStatement(type)` — Constructs `SCPStatement` from local state for the given phase type.

### `LocalNode`

Represents the local node in the SCP network. Holds the node's identity, quorum set, and provides static methods for quorum/v-blocking checks.

**Members:**
- `mNodeID` (`NodeID const`) — This node's public key.
- `mIsValidator` (`bool const`) — Whether this node is a validator.
- `mQSet` (`SCPQuorumSet`) — This node's quorum set (normalized on construction).
- `mQSetHash` (`Hash`) — Hash of the quorum set.
- `mSingleQSet` (`shared_ptr<SCPQuorumSet>`) — Singleton quorum set `{{mNodeID}}`, used during EXTERNALIZE.
- `gSingleQSetHash` (`Hash`) — Hash of the singleton quorum set.
- `mDriver` (`SCPDriver&`) — Back-reference.

**Key Static Methods:**
- `forAllNodes(qset, proc)` — Recursively iterates all nodes in a quorum set; short-circuits on `proc` returning false.
- `isQuorumSlice(qSet, nodeSet)` — Tests if `nodeSet` contains a quorum slice for this quorum set (threshold validators + inner sets satisfied).
- `isVBlocking(qSet, nodeSet/map, filter)` — Tests if a set of nodes forms a v-blocking set. Condition: `nodeSet` size ≥ `total - threshold + 1` (enough to block every quorum slice).
- `isQuorum(qSet, map, qfun, filter)` — Iterative quorum check with transitivity: filters nodes, then repeatedly removes nodes whose quorum slice isn't satisfied until fixpoint, then checks if local node's slice is still satisfied.
- `findClosestVBlocking(qset, nodes, excluded)` — Finds the minimum set of nodes from `nodes` needed to form a v-blocking set (used for failure analysis in diagnostics).
- `getSingletonQSet(nodeID)` — Returns `{threshold:1, validators:[nodeID]}`.
- `toJson / fromJson` — Serialize/deserialize quorum sets to/from JSON.

### `QuorumSetUtils`

**Functions:**
- `isQuorumSetSane(qSet, extraChecks, errString)` — Validates a quorum set: nesting depth ≤ 4, threshold ≥ 1, threshold ≤ entries, no duplicate nodes, total nodes 1–1000. With `extraChecks`, also validates threshold ≥ v-blocking size (≥51% effective).
- `normalizeQSet(qSet, idToRemove)` — Normalizes a quorum set: removes `idToRemove` (adjusting threshold), merges singleton inner sets into parent's validators, simplifies `{t:1, {inner}}` to `inner`, then lexicographically sorts validators and inner sets.

### Wrapper Types

- `ValueWrapper` — Immutable wrapper around `Value` (XDR opaque byte vector). Non-copyable, non-movable. Shared via `ValueWrapperPtr` (`shared_ptr<ValueWrapper>`).
- `SCPEnvelopeWrapper` — Immutable wrapper around `SCPEnvelope`. Non-copyable, non-movable. Shared via `SCPEnvelopeWrapperPtr`.
- `ValueWrapperPtrSet` — `set<ValueWrapperPtr, WrappedValuePtrComparator>` ordered by underlying value bytes.

---

## SCP Protocol Phases

### 1. Nomination Phase

**Goal:** Agree on a set of candidate values to propose for balloting.

**Flow:**
1. `SCP::nominate()` is called by the application with a proposed value.
2. `NominationProtocol::nominate()` increments round, computes round leaders via `updateRoundLeaders()`, and adds values from leaders' latest nominations to votes (X).
3. If self is a leader, adds own proposed value to X (stripping upgrades if timeout limit exceeded).
4. A `NOMINATION_TIMER` is armed to re-invoke `nominate()` with `timedout=true` for the next round.
5. When messages arrive via `processEnvelope()`:
   - For each voted value: if `federatedAccept(voted_for, accepted)` holds, move to Y (accepted).
   - For each accepted value: if `federatedRatify(accepted)` holds, move to Z (candidates). Stops the nomination timer.
6. When new candidates appear, `combineCandidates()` produces a composite value and `bumpState()` initiates the ballot protocol.

**Leader Election:** Uses `hashNode(priority, nodeID)` with node weight from quorum set. The node(s) with highest priority hash value are leaders. Self is always included. Rounds fast-forward if no node has priority.

### 2. Ballot Phase — PREPARE

**Goal:** Converge on a prepared ballot.

**State:** `mPhase = SCP_PHASE_PREPARE`. Working variables: b (current ballot), p (highest accepted-prepared), p' (second highest, incompatible with p).

**Transitions:**
- **Accept Prepared** (`attemptAcceptPrepared`): If `federatedAccept(vote_to_prepare(ballot), accept_prepared(ballot))`, update p (and p' if needed). If p/p' conflict with h, clear c.
- **Confirm Prepared** (`attemptConfirmPrepared`): If `federatedRatify(prepared(ballot))`, set h (highest confirmed-prepared) and optionally c (lowest confirmed-prepared). Set `mValueOverride` to lock in h's value. Transition to CONFIRM if commit is also accepted.

### 3. Ballot Phase — CONFIRM

**Goal:** Converge on a committed ballot.

**State:** `mPhase = SCP_PHASE_CONFIRM`. Requires b, p, c, h all set. p' is cleared.

**Transitions:**
- **Accept Commit** (`attemptAcceptCommit`): If `federatedAccept(vote_to_commit, accept_commit)` for an interval [c, h], set c/h, transition PREPARE→CONFIRM.
- **Confirm Commit** (`attemptConfirmCommit`): If `federatedRatify(commit(ballot, [c,h]))`, transition to EXTERNALIZE.
- **Bump** (`attemptBump`): If v-blocking set has higher ballot counters, bump to lowest counter that eliminates this condition.
- Accept-prepared can still update p (for reporting purposes) in CONFIRM phase.

### 4. Ballot Phase — EXTERNALIZE

**Goal:** Finalize — consensus is reached.

**State:** `mPhase = SCP_PHASE_EXTERNALIZE`. Set by `setConfirmCommit()`.

**Actions:**
- Emits `EXTERNALIZE` statement.
- Stops nomination via `Slot::stopNomination()`.
- Invokes `SCPDriver::valueExternalized(slotIndex, value)`.
- Incoming envelopes are still processed (recorded) if their value matches the committed value, but no further state transitions occur.

---

## Key Control Flow and Timers

### Nomination Timer (`NOMINATION_TIMER = 0`)
- Armed in `NominationProtocol::nominate()` after each round.
- Timeout computed by `SCPDriver::computeTimeout(roundNumber, isNomination=true)`.
- On expiry: re-invokes `nominate()` with `timedout=true`, advancing to next round with new leaders.
- Stopped when candidates are found (federated ratify of an accepted value).

### Ballot Protocol Timer (`BALLOT_PROTOCOL_TIMER = 1`)
- Armed in `checkHeardFromQuorum()` when a quorum is first heard at the current ballot counter.
- Timeout computed by `SCPDriver::computeTimeout(ballotCounter, isNomination=false)`.
- On expiry: `ballotProtocolTimerExpired()` increments `mTimerExpCount` and calls `abandonBallot(0)`, which bumps to the next ballot counter.
- Stopped when quorum is no longer heard at current counter, or when externalized.

### `advanceSlot` Recursion
- Processing an envelope triggers `advanceSlot(hint)`, which may emit new statements that re-enter `advanceSlot`.
- Recursion is capped at `MAX_ADVANCE_SLOT_RECURSION = 50` to prevent infinite loops.
- Envelope emission (`sendLatestEnvelope`) is deferred to the outermost `advanceSlot` call (`mCurrentMessageLevel == 0`).

---

## Ownership Relationships

```
SCP
 ├── mLocalNode: shared_ptr<LocalNode>
 └── mKnownSlots: map<uint64, shared_ptr<Slot>>
       └── Slot
            ├── mBallotProtocol: BallotProtocol (value member)
            │    ├── mCurrentBallot: unique_ptr<SCPBallotWrapper>
            │    ├── mPrepared: unique_ptr<SCPBallotWrapper>
            │    ├── mPreparedPrime: unique_ptr<SCPBallotWrapper>
            │    ├── mHighBallot: unique_ptr<SCPBallotWrapper>
            │    ├── mCommit: unique_ptr<SCPBallotWrapper>
            │    ├── mLatestEnvelopes: map<NodeID, SCPEnvelopeWrapperPtr>
            │    └── mValueOverride: ValueWrapperPtr
            └── mNominationProtocol: NominationProtocol (value member)
                 ├── mVotes: ValueWrapperPtrSet (X)
                 ├── mAccepted: ValueWrapperPtrSet (Y)
                 ├── mCandidates: ValueWrapperPtrSet (Z)
                 ├── mLatestNominations: map<NodeID, SCPEnvelopeWrapperPtr>
                 └── mLatestCompositeCandidate: ValueWrapperPtr
```

`SCPDriver` is referenced (not owned) by `SCP` and `LocalNode`. The application owns the `SCPDriver` and `SCP` instances.

---

## Key Data Flows

### Inbound Envelope Processing
1. `SCP::receiveEnvelope(envelope)` → `Slot::processEnvelope(envelope, self=false)`
2. Slot dispatches based on statement type:
   - `SCP_ST_NOMINATE` → `NominationProtocol::processEnvelope()`
   - `SCP_ST_PREPARE/CONFIRM/EXTERNALIZE` → `BallotProtocol::processEnvelope()`
3. Protocol validates sanity, checks newness, validates values via `SCPDriver::validateValue()`.
4. Records envelope, then:
   - Nomination: checks federated accept/ratify on values
   - Ballot: calls `advanceSlot()` which attempts all state transitions

### Outbound Envelope Emission
1. Protocol state change triggers `emitCurrentStateStatement()` (ballot) or `emitNomination()` (nomination).
2. Statement is created from current local state, wrapped in an envelope, signed via `SCPDriver::signEnvelope()`.
3. The envelope is self-processed (fed back through `processEnvelope` with `self=true`) to ensure consistency.
4. If valid and newer than last emitted, `SCPDriver::emitEnvelope()` is called to broadcast.

### Nomination → Ballot Transition
1. When `NominationProtocol` confirms candidates (Z non-empty), it calls `combineCandidates()` to produce a composite value.
2. It then calls `Slot::bumpState(compositeValue, force=false)` which delegates to `BallotProtocol::bumpState()`.
3. This creates the first ballot `(1, compositeValue)` and emits a PREPARE statement.

### Federated Agreement Primitives
Both protocols use two primitives provided by `Slot`:
- `federatedAccept(voted, accepted, envs)` — True if: (a) a v-blocking set of nodes accepted the statement, OR (b) a quorum of nodes voted-or-accepted it.
- `federatedRatify(voted, envs)` — True if a quorum of nodes voted for the statement.

These delegate to `LocalNode::isVBlocking()` and `LocalNode::isQuorum()` with the local node's quorum set and the relevant envelope maps.
