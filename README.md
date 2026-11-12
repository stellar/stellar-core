# stellar-core: leader schedule with terms, leader tx-set push, apply-time tx-set validation, transactions sent to the leaders only (experimental branch)

**This branch is an experiment. It is not production code and must not run on a real network.**
The leader schedule and the tx-set push have no feature flag and no protocol gate, and the branch
is not compatible with nodes that nominate.

The latest two experiments (leader terms with pre-validation, and transactions sent to the
upcoming leaders only) were measured on 2026-11-12 at an **800 ms target block time on 16-core
instances**: at 48 validators (16 organizations) and 8,000 TPS, and at the goal of this work,
**90 validators (30 organizations) and 10,000 TPS**. One run per build.

- **48 validators, 8,000 TPS:** with both experiments every window is on target (block time p75
  804-805 ms) and a node's overlay traffic is 6.2 MB/s. Without them the branch drifts and misses
  its second window (846, then 869 ms) at 21 MB/s.
- **90 validators, 10,000 TPS:** not reached yet. With both experiments the first window is at 980
  ms (limit 960), a ledger every 0.90 s, 15.7 MB/s per node; with leader terms and pre-validation
  alone it is at 1,032 ms, 1.02 s per ledger and 43 MB/s.

The earlier benchmark (48 validators, 5,000 TPS, 1,000 ms, 4-core instances) is kept below.

## What this branch is

It is the overlay-v2 stack up to [#5459](https://github.com/stellar/stellar-core/pull/5459)
(Rust overlay over QUIC, millisecond close times, compressed tx-set responses, leader-only tx-set
construction) plus five experiments:

1. **No nomination** ([#5495](https://github.com/stellar/stellar-core/pull/5495)). The SCP
   nomination phase is deleted and replaced by a leader schedule that every node computes locally.
   - **Schedule.** For slot N, the nodes of the transitive quorum are sorted by
     `SHA256(hash of ledger header N-1 || node id)`. The leader of round r is entry `(r-1) mod n`.
   - **Rounds.** A round is a ballot counter. The round's leader builds its value and goes straight
     to the ballot protocol with `(r, value)`. The other nodes vote for the leader's ballot once it
     passes proposal validation. A timeout (the ballot timeout settings) hands the slot to the next
     leader.
2. **The leader pushes its tx set** ([#5497](https://github.com/stellar/stellar-core/pull/5497),
   the "before" below). Before it, a follower saw the leader's proposal, asked for the tx set and
   received it about two round trips later. With it:
   - **Push.** The round's leader sends the set it is about to propose to every connected peer as
     soon as the set is built, without waiting for a request.
   - **Use.** A follower's overlay verifies a pushed set and keeps it under the hash of its own
     content. Core only gets it when an SCP statement names that hash, so a pushed set that does not
     match the proposal is never used.
   - **Fallback.** The request path is unchanged, for a node that did not get the push.
   - **One flight.** Every QUIC connection starts with an 8 MiB congestion window, so that a set
     leaves in one flight instead of being paced over a round trip.
   - Details and limits: [docs/rust-overlay/txset-fetching.md](docs/rust-overlay/txset-fetching.md).
3. **Vote without validating the tx set; validate and execute at apply**
   ([#5498](https://github.com/stellar/stellar-core/pull/5498), the latest experiment). Gated on
   protocol 30; protocol 29 and older behave as before.
   - **Vote.** A node votes for the leader's value as soon as it holds the tx set, without running
     the set's validity check. Still checked before the vote: the value itself (signature, close
     time, upgrades) and that the set was built on the last closed ledger.
   - **Validate at apply.** The validity check runs when the ledger is applied. An invalid set
     closes an **empty ledger**: no fee, no sequence bump, no result. The header keeps the
     externalized value, upgrades in the value still apply, and history keeps the set so that a
     replaying node reaches the same verdict.
   - **Speculative execution.** When a node emits a PREPARE or CONFIRM for a value for the next
     ledger, its apply thread validates and executes that ledger while the ballot is still running.
     When the value externalizes, the executed ledger is committed; if another value externalizes,
     it is dropped and the ledger is applied from scratch. `EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY`
     (default true) turns it off.
   - **Durable tx set, written by the overlay.** The Rust overlay writes the compressed (zstd) set
     to `<BUCKET_DIR_PATH>/txsets` before it hands a set to Core, and before it sends anything that
     refers to a set Core built. Core no longer writes the set to SQLite in front of its first
     vote, and gets its sets back from the overlay after a restart.
   - Details, safety argument and review findings:
     [#5498](https://github.com/stellar/stellar-core/pull/5498).

4. **Leader terms, pre-validation during the previous ballot, one build before the trigger**
   ([#5502](https://github.com/stellar/stellar-core/pull/5502)). No protocol gate, like the
   schedule it changes.
   - **Terms.** The node whose value closed a ledger leads the next one, for up to 5 ledgers; a
     term ends on a ledger number divisible by 5. The order of a slot is seeded with the hash of
     the ledger before the last closed one, so it is known one ledger earlier. A leader loses its
     term when its round times out or its ledger applies nothing.
   - **Pre-validation.** The nodes that expect to lead the next ledger check the validity of their
     mempool's top candidates against the state the speculative execution left, on background
     threads, while the executed ledger is still in its ballot. The results are used only if the
     ledger closes with the value that was executed.
   - **One build.** The first-round leader builds once, a lead time before its trigger, pushes
     the set as soon as it exists and proposes it unchanged at the trigger.
5. **Transactions go to the upcoming leaders only**
   ([#5501](https://github.com/stellar/stellar-core/pull/5501), the latest experiment). No
   protocol gate and no version negotiation.
   - **Targets.** A node that accepts a transaction no longer advertises it to every peer. Its
     overlay sends the body straight to the leaders of the node's current round and of the next
     one for the next ledger and, one ledger before a new leader term starts, to that term's
     first leader. One function in Core decides the list (`HerderImpl::computeTxFloodTargets`).
   - **Keep and follow.** The accepting node keeps the transaction until it is externalized. When
     the targets change it goes to a new target only while it is fresh (under 200 ms) or once
     three ledgers closed without it. If a target is not a directly connected, identified peer,
     the node floods as before.
   - **Admission check.** A submitted transaction is checked against the last closed ledger
     before it is sent on, with one pending transaction per source account. Before, a node
     flooded whatever it was given.
   - **Identity.** An overlay proves which validator it belongs to with a binding of its
     connection key, signed by the node key.
   - **Cost.** Followers no longer hold the transactions before the leader's set arrives, so
     they verify signatures when they check the set at apply instead of in the background.

Unchanged by all five: the ballot protocol's accept, confirm and commit rules. For a valid tx
set, ledger state and meta are also unchanged; experiment 3 changes when a set is validated and
executed, and what an invalid set does to the ledger; experiment 4 changes who leads; experiment
5 changes which transactions reach a leader, and which submissions a node refuses.

## Performance results

### 800 ms target on 16-core instances: 48 validators, 8,000 TPS

c6id.8xlarge (16 cores), 4 regions, SAC payments, 150 ms simulated apply, 900 s of load; one run
per build, all on 2026-11-12. "Base" is this branch up to experiment 3.

| Windows 1 / 2 / 3 / 4 | Base (`0c9a02e56`) | + leader terms and pre-validation (#5502, `deec2d87c`) | + transactions to leaders only (#5501, `6b3c7ae0e`) |
|---|---:|---:|---:|
| Block time windows on target | window 1 only; window 2 missed, the load stops there | **all four**, on all 48 nodes | **all four**, on all 48 nodes |
| p75, average over 48 nodes (limit 960 ms) | 846 / 869 ms, rising | 807 / 806 / 805 / 805 ms | **805 / 804 / 804 / 804 ms** |
| p99, average over 48 nodes (limit 1,600 ms) | 1,368 / 1,480 ms | 1,153 / 1,188 / 1,100 / 1,249 ms | 1,170 / 1,209 / 1,039 / 1,172 ms |
| Time per ledger | 0.84-0.85 s | 0.81 s | 0.81 s |
| Leader builds per ledger; build time | 1.9; 0.16 s | 1.0; 0.16 s, before the trigger | 1.0; 0.15 s, before the trigger |
| Trigger to first ballot | 0.34-0.36 s | 0.11 s | 0.10-0.12 s |
| First ballot to externalize | 0.29 s | 0.29 s | 0.29 s |
| Externalize to ledger closed | 0.04 s | 0.05 s | 0.04 s |
| **Trigger to ledger closed** | 0.67-0.69 s | **0.44-0.45 s** | **0.43-0.44 s** |
| Tx-set validation at apply | 0.08 s | 0.08 s | 0.10 s (signatures verified inline) |
| **Overlay traffic per node, each way** | 21.0 MB/s | 21.2 MB/s | **6.2 MB/s** |
| Overlay messages received per node | 20,000 /s | 20,500 /s | 12,550 /s |
| Transaction adverts sent per node | 7,848 /s | 7,848 /s | 0 |
| Leader and ballot timeouts, empty tx sets | 0 | 0 | 0 |
| Run verdict | FAIL (block time) | FAIL (inclusion check only) | FAIL (inclusion check only) |

- **Leader terms and the single early build take the build off the critical path**: trigger to
  ledger closed falls from 0.68 to 0.44 s, and the branch holds 800 ms at 8,000 TPS for the full
  15 minutes instead of drifting.
- **Sending transactions to the leaders only removes 71% of the overlay traffic** at the same
  block time. Both runs sit at the floor of this fixture (the stages fill 0.44 s of the 0.8 s
  cycle), so this fixture cannot show a block-time difference between them.
- What is left of the traffic is SCP (12,100 messages per second per node), the leader's pushed
  set (about 5 MB per ledger) and 3.3 transaction bodies per transaction. The build measured
  here re-sent every pending transaction when the targets changed; the head of #5501 no longer
  does (2.5 bodies per transaction in the next table's run).
- **Inclusion.** The check is an exact comparison (see [The inclusion check](#the-inclusion-check)).
  #5502 alone held 80 transactions more than the 7,680,000 offered, the run with #5501's first
  version 6,610 more (0.086%): copies of transactions that reach a node after their ledger
  closed, included a second time, which only the test's "overlay-only" mode permits. With the
  head's rule for re-sends the next run had 81 on 3,870,249.

### 800 ms target at the goal: 90 validators, 10,000 TPS

30 organizations, 22-23 validators per region, the same instances and load shape, 10,080 TPS
offered. Both runs missed their first window and the load was stopped after it.

| Window 1 | Leader terms and pre-validation (#5502, `8aa7f6bf0`) | + transactions to leaders only (#5501, `6ab837c27`) |
|---|---:|---:|
| p75, average over 90 nodes (limit 960 ms) | 1,032 ms | **980 ms** |
| p99, average over 90 nodes (limit 1,600 ms) | 1,887 ms (over) | **1,441 ms** (inside) |
| Time per ledger | 1.02 s | **0.90 s** (median 0.88 s) |
| The first minute of the load | 802 ms for 30 s, then a backlog | 805 ms over the whole minute |
| Tx sets at the 10,000 cap | 93% | 39% |
| Leader builds finished before the trigger | 0 of 40 sampled | 40 of 55 sampled |
| Leader build; candidates in the mempool | 0.33 s; 20,200 | 0.24 s; 8,350 (median) |
| Trigger to first ballot | 0.58 s | **0.39 s** |
| First ballot to externalize | 0.32 s | 0.32 s |
| Externalize to ledger closed | 0.07 s | 0.07 s |
| **Trigger to ledger closed** | 0.97 s | **0.77 s** |
| Tx set at a follower after the proposal | 0.23 s | 0.17 s |
| Tx-set validation at apply | (not listed) | 0.14 s |
| **Overlay traffic per node, each way** | 43 MB/s | **15.7 MB/s** |
| Leader and ballot timeouts, empty tx sets | 0 | 0 |
| Submitted against included at the end | 49,855 short (backlog) | 81 more (the backlog drained) |
| Run verdict | FAIL (block time: p75 and p99) | FAIL (block time: p75) |

- **Neither build holds 800 ms at 90 validators and 10,000 TPS.** With transactions sent to the
  leaders only the miss is 20 ms on the window's p75 instead of 72 ms, p99 stays inside its
  limit, and the network runs 12% faster (0.90 s against 1.02 s per ledger).
- The gain is in trigger to first ballot: the leader's mempool holds about one ledger of
  candidates instead of a backlog of 20,200, so its build is shorter and three of four builds
  finish before the trigger again.
- **Overlay traffic is 15.7 MB/s per node instead of 43.** At 90 validators most of what is left
  is SCP: 35,700 messages per second per node.
- There is no run of the base (up to experiment 3) at this fixture. In the #5502 column, the
  set's arrival, the build and the traffic are from that PR's first run at this fixture
  (`deec2d87c`, 1,046 ms); its rerun with `8aa7f6bf0` reported the same stage table.

### Where a ledger's time goes at 90 validators and 10,000 TPS

From the logs of twelve nodes (three per region) in the run with all five experiments, 56
ledgers led by one of them:

| Step on the leader | Mean | |
|---|---:|---|
| Previous ledger closed to proposal | 0.29 s | The build starts when the previous ledger closes (it needs that ledger) and takes 0.24 s for 8,500 transactions; the set is pushed while the build finishes |
| Proposal to consensus | 0.53 s | Of which, on a follower: 0.17 s until the set is in hand, then 0.34-0.36 s of ballot |
| Consensus to ledger closed | 0.07 s | The commit of the speculatively executed ledger |
| **Sum** | **0.89 s** | Target 0.80 s. 41 of the 56 cycles took longer than 0.8 s |

1. **The build is on the critical path again: 0.29 s.** It cannot start before the previous
   ledger has closed, and the cycle has no slack left for it, so the trigger is already due when
   it ends (the proposal went out within 20 ms of the build's end on half of the ledgers that
   had an early build).
2. **The leader's set takes 0.19 s to reach a follower's overlay**, 0.13 s of it even within the
   leader's own region: the leader uploads the full set (5.7 MB on average) to each of its 89
   peers, 506 MB per ledger. A follower cannot vote before it has the set.
3. **The ballot is 0.34-0.36 s** from the set in hand to consensus. It is message delay between
   four regions: proposal to consensus is 0.43 s when the leader is in us-east-1, 0.58 s from
   us-west-2 and 0.60 s from ap-southeast-1, and a ledger led from ap-southeast-1 takes 0.96 s
   against 0.83 s from us-east-1.
4. **A follower's execution ends just before consensus**: 0.15 s of validation (signatures
   verified inline) plus the simulated apply, 0.31 s after the set arrived.
5. The main thread is not saturated (a posted task waits 14 ms on average).

### Earlier benchmark: 1,000 ms target, 5,000 TPS, 48 validators, 4-core instances

The measurement of experiment 3 (vote without validating the tx set, validate and execute at
apply), on commit `0c9a02e56` on 2026-11-11. "Before" is the previous experiment on this branch
(the leader's tx-set push), commit `8f7768be7`, measured on the same day on the same fixture.
One run per build.

| | Before: tx-set push (`8f7768be7`) | With apply-time validation (`0c9a02e56`) |
|---|---|---|
| Block time windows | **missed** the first window on all 48 nodes; the load stops there | **on target in all four windows**, on all 48 nodes |
| p75, average over 48 nodes | 1,461 ms (limit 1,200) | 1,081 / 1,132 / 1,118 / 1,142 ms |
| p99, average over 48 nodes | 1,638 ms (limit 2,000) | 1,292 / 1,413 / 1,598 / 1,595 ms |
| Time per ledger | 1.42 s | 1.03-1.08 s |
| Trigger to ledger closed (sum of the stages) | 1.37 s | 0.76-0.84 s |
| Run verdict | FAIL (block time) | FAIL (inclusion check only, see the caveat) |

In short: before, the stages of one ledger added up to 1.37 s, every tx set was full and the
network backed up. With validation moved out of the vote and the ledger executed while the ballot
runs, a ledger's stages fit into about 0.8 s and the network keeps up with 5,000 TPS at a 1 s
target.

**Caveat: the run with the change is a FAIL on the inclusion check, and on that check alone.**
Every node's ledgers held 4,800,060 transactions against 4,800,000 offered: 60 more (0.00125%),
the same on all 48 nodes. The check is an exact comparison. All other checks passed. The 60 are
not explained (see [The inclusion check](#the-inclusion-check)).

**An earlier run of the same change found a bug.** The change was first measured without its last
commit (`a33da62c4`). The block times were about the same (p75 1,094 / 1,116 / 1,130 / 1,137 ms, all
windows on target), but one node fell out of consensus 15 s before the end of the load. The cause
was in the Core-overlay IPC code and is fixed in `0c9a02e56`; in the run reported here no node was
affected (see [The IPC bug](#the-ipc-bug-found-by-the-first-run)).

### 1,000 ms target, with apply-time validation

Load ran its full 900 s. Every node was on target in every window.

| Window | p75, average (range over nodes) | p99, average (range over nodes) |
|---|---|---|
| 1 | 1,081 ms (1,078-1,087) | 1,292 ms (1,223-1,358) |
| 2 | 1,132 ms (1,118-1,150) | 1,413 ms (1,355-1,511) |
| 3 | 1,118 ms (1,109-1,126) | 1,598 ms (1,433-1,710) |
| 4 (2.2 minutes) | 1,142 ms (1,127-1,170) | 1,595 ms (1,413-1,712) |

Limits: p75 below 1,200 ms, p99 at most 2,000 ms.

Checks:
- **No fork**, all 48 nodes in sync, still closing ledgers after the load: passed.
- **Error counters zero** (invalid signatures, invariant failures, publish failures), no node lost
  sync: passed.
- **No wasted rounds:** every ledger was decided by the first leader. 0 leader timeouts, 0 ballot
  timeouts, 0 empty transaction sets. No tx set was found invalid at apply.
- **Inclusion: failed.** 4,800,060 transactions in every node's ledgers against 4,800,000 offered.
  See below.

### Before and after, stage by stage

5-minute means at the end of each run's first window, median over the 48 nodes.

| | Before (`8f7768be7`) | With apply-time validation (`0c9a02e56`) |
|---|---:|---:|
| Ledgers per 5 minutes | 211-212 | 291-292 |
| Transactions per ledger | 6,250, the cap, on every ledger | 5,152 (what was offered; below the cap) |
| Leader tx-set build | 0.41 s | 0.24 s |
| Tx-set validation | 0.195 s, before the vote | 0.179 s, at apply, while the ballot runs |
| Trigger to first ballot (includes the build and the fetch; before, also validation) | 0.80 s | **0.41 s** |
| First ballot to externalize | 0.37 s | 0.30 s |
| Externalize to ledger closed (from node logs) | 0.21 s | **0.05-0.07 s** |
| **Sum: trigger to ledger closed** | **1.37 s** | **0.76-0.78 s** |
| Tx-set fetch (follower's Core, request to delivery) | 0.033 s | 0.051 s |
| Leader timeouts, ballot timeouts, empty tx sets | 0, 0, 0 | 0, 0, 0 |

In the later windows of the run with the change, trigger to first ballot is 0.47 s and the sum
0.82-0.84 s. Externalize to ledger closed is the range of the per-node medians of eight nodes over
the whole load.

What the new parts did, over the whole load of the run with the change:

| | |
|---|---:|
| Ledgers executed speculatively (total of the 48 nodes) | 46,194 |
| Speculatively executed ledgers that were committed | 46,206 (46,204 ledgers closed; the counters are scraped at slightly different times) |
| Executions dropped because another value externalized | 0 |
| Tx sets found invalid at apply (empty ledger) | 0 |
| Execution (validation plus simulated apply), mean | 0.33-0.35 s |
| Commit after externalize, mean | 0.037-0.041 s |
| Overlay's write of a tx set to disk (8 nodes, 6,941 writes) | median 4.3 ms, p99 10.8 ms, maximum 356 ms |
| Core's write of SCP state before a vote, now without the tx set | 2.2-2.6 ms mean |

- **Every ledger was executed before it externalized or while it did.** No execution was wasted:
  with one leader per round and no timeouts, the value a node votes for is the value that
  externalizes.
- **The execution is slightly longer than the ballot.** It takes 0.33-0.35 s (0.18-0.19 s of
  validation plus the simulated apply), the ballot 0.30 s. In the logs of eight nodes, the ledger
  closed more than 50 ms after externalize on 60% of the ledgers and more than 100 ms after on 21%
  (a commit alone takes about 40 ms). Before, it was about 207 ms on every ledger.
- **The gain from speculation is an upper bound.** Apply is simulated by a sleep that uses no CPU,
  so running it beside the ballot is free here. Real execution would compete with SCP processing
  and signature verification.
- **The two builds do not simulate the same work per ledger.** Before, the sleep starts with the
  apply and absorbs the commit. With the change it sits in the execution, and validation and the
  commit come on top of it.
- **The two runs are in different regimes**, as in the earlier comparison. Before, a ledger could
  not close in less than 1.42 s, so 5,000 TPS filled every set to the cap and the mempool backed
  up. Now ledgers hold what was offered. That alone shortens the leader's build and the sets. One
  run on each side cannot separate it from the effect of the change.
- **The push mostly loses its race now.** The leader's proposal leaves earlier relative to the
  push, so a follower's request usually reaches the leader while the push is still in flight
  (38,883 such requests against 6,240 sets that were already in place; before: 17 against 13,914).
  The leader does not send a second copy, and the fetch costs about 0.02 s more per ledger. Why
  the proposal overtakes the push is inferred from the removed database write, not timed.
- **Block time is 1.03-1.08 s although the stages sum to about 0.8 s.** The windows drifted up
  over the 16 minutes: p75 by about 60 ms, p99 from 1,292 to about 1,600 ms. Neither was
  investigated.
- **The two runs of the change agree.** Without the IPC fix (`a33da62c4`): p75 1,094-1,137 ms,
  288-290 ledgers in the first 5 minutes, trigger to first ballot 0.42 s, first ballot to
  externalize 0.30 s, every ledger executed speculatively and committed.

### The inclusion check

The test compares the transactions in every node's ledgers with the number offered. In the run
with the change all 48 nodes held exactly 60 more (the same on every node, so no fork). The
generators had submitted exactly 4,800,000 and none was rejected.

- **The same excess, exactly 60, appeared on the earlier fixture** in the run with the tx-set push
  (30 validators, 3,000 TPS: 2,880,060 against 2,880,000).
- **The explanation given then was an inference**: the test runs in "overlay-only" mode, where
  ledgers are not applied and sequence numbers never advance, so a copy of a transaction that
  reaches a far node after its ledger closed is admitted again and included a second time. The
  logs carry no transaction hashes, so this was never shown.
- **It does not predict the same number** at a different rate and network size. Which
  transactions are counted twice, or whether something else adds 60, is open.
- The "before" run and the first run of the change did not get to this check (the load ended
  early or a generator failed). In the first run of the change the 47 healthy nodes held
  4,798,690, fewer than offered; the node that dropped out kept generating for 15 s while nothing
  left it.
- A change that lets the check allow up to 1 in 10,000 more than offered is proposed
  ([performance-testing#4](https://github.com/stellar-experimental/performance-testing/pull/4)),
  not merged. Under it this run would pass.

### The IPC bug found by the first run

- **What happened.** In the first run of the change (`a33da62c4`), 15 s before the end of the
  load, Core on one node logged "Overlay IPC connection closed" and stopped reading from its
  overlay process. Both processes kept running and the socket between them was still open, with
  unread data on Core's side. The node heard nothing further from the network and lost sync. Two
  more nodes did the same two minutes after the load.
- **When.** All three times within 26 ms of a step of a history checkpoint publish.
- **Cause (inferred; the logs carry no error code).** Core's IPC reader treated any failed `recv`
  as a closed connection, including one interrupted by a signal (`EINTR`). stellar-core handles
  `SIGCHLD`, and a checkpoint publish runs child processes.
- **Fix.** `0c9a02e56`: an interrupted read or write on the IPC socket is retried. A unit test
  interrupts a blocked read with a signal and fails without the fix.
- **In the run with the fix** all 48 nodes were checked fourteen times, from before the load until
  17 s before the network was terminated: none logged the warning and none had a stuck socket.
  Every node published 44 checkpoints in that time (about 2,100 in the network); the three events
  of the first run fell within its first 19 checkpoints per node (about 900).
- **This is not a proof.** The fix logs nothing when it retries, so there is no count of
  interrupted reads it survived, and the "before" build, with the unfixed reader, had no such
  event in its 30 minutes either. Whether the change makes the event more likely is not known.

### Where a ledger's time went at 1,000 ms and 5,000 TPS

What was left of the 0.76-0.84 s from trigger to ledger closed after experiment 3:

1. **Trigger to first ballot: 0.41-0.47 s.** Most of it is the leader's build (0.24-0.25 s), on
   the critical path. The first-round leader also builds once before the trigger and discards
   that set unless it was capacity limited (540-568 builds for 279-292 ledgers). The tx-set fetch
   adds 0.05 s.
2. **The ballot: 0.30 s.** Nothing is validated in it any more; it is message delays between four
   regions.
3. **Externalize to ledger closed: 0.05-0.07 s.** The commit (0.04 s), plus the end of the
   execution on more than half of the ledgers.

The execution itself (0.33-0.35 s) runs beside the ballot.

### Earlier fixture: 30 validators, 3,000 TPS

The first two experiments were measured on a smaller fixture (10 organizations, 30 validators,
3,000 TPS, 1,000 ms target, 2026-11-10 and 2026-11-11). It is no longer the benchmark.

| | Leader schedule alone (`daa965f4c`) | With the tx-set push (`8f7768be7`) |
|---|---|---|
| Block time windows | missed the first window | on target in all four windows, on all 30 nodes |
| p75, average over 30 nodes | 1,386 ms | 1,091 / 1,096 / 1,109 / 1,115 ms |
| Time per ledger | 1.33 s | 1.05-1.07 s |
| Tx-set fetch as seen by a follower's Core | 217 ms | 16 ms |
| Trigger to ledger closed (sum of the stages) | 1.30 s | 0.91 s |

- With the push, every node's ledgers held 60 transactions more than the 2,880,000 offered. The
  run was judged with an exact comparison and reported FAIL for that check alone (see
  [The inclusion check](#the-inclusion-check)).
- A 2,000 ms target was measured for the leader schedule alone: PASS in all four windows (p75
  2,066-2,109 ms).
- The full results, stage tables and the measurement setup of that fixture are in
  [the README as of #5497](https://github.com/stellar/stellar-core/blob/6d5fc9e814abee2b7e93553b3903b56929bedf91/README.md)
  and in [#5495](https://github.com/stellar/stellar-core/pull/5495).

## How it was measured

The sections below describe the earlier benchmark (1,000 ms, 5,000 TPS). The 800 ms runs of
2026-11-12 differ from it in these points only:

| | 48 validators, 8,000 TPS | 90 validators, 10,000 TPS |
|---|---|---|
| Instance type | c6id.8xlarge: 32 vCPU, 16 cores, the same processor | the same |
| Organizations; validators per region | 16; 12 | 30; 22 or 23 |
| Connectivity | full mesh, 47 peers | full mesh, 89 peers |
| Target ledger close time | 800 ms | 800 ms |
| Offered load | 8,000 TPS, about 167 per validator | 10,080 TPS, 112 per validator |
| Maximum transactions per ledger | 8,000 | 10,000 |
| Simulated apply, per operation | 23 us (150 ms per ledger at the offered load) | 18 us |
| Window limits | p75 in [640, 960) ms, p99 at most 1,600 ms | the same |
| Validation tasks (one per core but one) | 15 | 15 |
| Logs | not saved; twelve nodes were read while the network was up | the same |
| Builds | `0c9a02e56` (base), `deec2d87c` (#5502), `6b3c7ae0e` (#5501 on #5502) | `8aa7f6bf0` (#5502), `6ab837c27` (#5501 on #5502) |

`--block-time-ms 800 --soroban-tps 8000` (or `--org-count 30 --soroban-tps 10000`),
`--instance-type c6id.8xlarge` and no `--save-logs`; everything else as in the command below.

All runs use the `block_time_check` mission of perfnet, SDF's tooling for
performance networks on AWS. Each run launches a fresh network, brings it to the target settings,
runs the load, judges it, and terminates the network.

### Network topology

| | |
|---|---|
| Validators | 48, all validating, no watchers |
| Organizations | 16 home domains of 3 validators each, all `HIGH` quality |
| Quorum set | Generated by stellar-core from the home domains. Its rule gives 11 of 16 organizations, 2 of 3 validators within each (computed from the rule; not read back from a node) |
| Regions | 4 AWS regions, 12 nodes each: eu-central-1, us-west-2, ap-southeast-1, us-east-1 |
| Placement | 14 organizations have their three validators in three different regions; 2 have them in two regions. All runs used the same layout seed |
| Connectivity | Full mesh: every node is configured with the other 47 as its only peers |
| Round-trip times seen from us-west-2 (measured on the earlier fixture, same regions) | us-east-1 about 60 ms, eu-central-1 about 147 ms, ap-southeast-1 about 180 ms |

### Hardware and software

| | |
|---|---|
| Instance type | c6id.2xlarge: 8 vCPU (Intel Ice Lake, with SHA extensions), 16 GiB RAM, local NVMe |
| Build, with apply-time validation | Commit `0c9a02e56`, the perftests package `30.0.1-3678.0c9a02e566.noble~perf.apply.time.txset.validation~perftests` (the first run of the change: `a33da62c4`, `30.0.1-3677.a33da62c49.noble~perf.apply.time.txset.validation~perftests`). All 48 nodes reported the commit in their build label |
| Build, before | Commit `8f7768be7`, the perftests package `30.0.1-3675.8f7768be7a.noble~perf.leader.txset.push~perftests`. All 48 nodes reported that commit in their build label |
| Overlay | The Rust overlay of this stack: QUIC, one connection per peer, 8 MiB initial congestion window. With the change it also stores tx sets on the local NVMe |
| Ledger protocol | 30, upgraded from genesis during preparation |
| Database | SQLite |

### Node configuration

Beyond identity, peers and home domains, every node ran with:

```
ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING=true
UNSAFE_QUORUM=true
FAILURE_SAFETY=0
GENESIS_TEST_ACCOUNT_COUNT=1000000
TARGET_PEER_CONNECTIONS=47
MAX_ADDITIONAL_PEER_CONNECTIONS=141
PREFERRED_PEERS_ONLY=true
FLOOD_DEMAND_BACKOFF_DELAY_MS=1000
INVARIANT_CHECKS=[]
TESTING_IGNORE_LEDGER_TIME_UPGRADE_BOUNDS=true
TESTING_MAX_CLASSIC_BYTE_ALLOWANCE=1048576
TESTING_MAX_SOROBAN_BYTE_ALLOWANCE=9437184
LOADGEN_PREGENERATED_TRANSACTIONS_FILE="/var/lib/stellar-core/loadgen-txs.xdr"
OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING=[30]
OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING=[100]
```

`EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY` was on (the default) in the runs with the change.
`EXPERIMENTAL_PARALLEL_TX_SET_DOWNLOAD` was off (the default). Catchup and automatic restart are
off, so a node that crashes or falls behind stays that way and fails the run.

### Network settings (applied by upgrade before the load)

| Setting | All runs |
|---|---:|
| Target ledger close time | 1,000 ms |
| SCP timeouts (ballot and nomination, initial and increment) | 2,000 ms |
| Maximum transactions per ledger (125% of the offered load per block) | 6,250 |
| Maximum tx-set size in bytes | 10 MiB (1 MiB classic + 9 MiB Soroban) |
| Soroban limits | raised to fit the load |

The leader timer of this branch uses the ballot timeout, so a round's leader had 2 s (4 s with its
one extension).

### Load

| | |
|---|---|
| Transactions | Stellar Asset Contract (SAC) payments, pregenerated; no classic transactions |
| Rate | 5,000 TPS offered: every validator generates about 104 TPS |
| Accounts | 1,000,000 genesis accounts, 20,833 per generator |
| Duration | 60 s warm-up, then 900 s measured |
| Apply | Skipped ("overlay-only" mode) and replaced by a sleep per operation, sized as a 150 ms budget per ledger at the offered load: 30 us per operation |

Command (the runs differ only in the build):

```
block_time_check.py \
  --stellar-core-version 30.0.1-3678.0c9a02e566.noble~perf.apply.time.txset.validation~perftests \
  --org-count 16 --block-time-ms 1000 \
  --soroban-tps 5000 --soroban-type sac --classic-tps 0 \
  --simulate-apply-budget-ms 150 --load-duration-s 900 \
  --instance-type c6id.2xlarge --save-logs always
```

Defaults that matter: validators as generators, `--scp-timeout-ms 2000`,
`--max-tx-set-bytes 10485760`, 1,000,000 genesis accounts, seed 0.

### How a run is judged

- Ledger close times are read per node from Prometheus (scraped every 10 s) and judged over
  5-minute windows after the warm-up.
- **PASS** needs, on every node in every window: p75 within [0.8 T, 1.2 T), p99 at most 2 T, and at
  least one close per 2 T on average, where T is the target block time. It also needs every node's
  ledgers to hold exactly the offered transactions, zero error counters, all nodes in sync, no
  fork, and the network still closing ledgers afterwards.
- The load stops at the first window that misses, so a FAIL can rest on one window (as in the
  "before" run). The inclusion check is skipped when the load ends early or a generator fails
  (the "before" run and the first run of the change).

### Limits of these numbers

- One run on each side, plus the first run of the change without the IPC fix. The "before"
  numbers come from a single 5-minute window.
- The run with the change is a FAIL on the inclusion check, and the 60 extra transactions are not
  explained.
- Apply is simulated by a sleep, so these are consensus and overlay numbers, not end-to-end ledger
  close. The gain from speculative execution is an upper bound, and the two builds do not sleep
  for the same part of the apply (above).
- `herder.txset.validate` times the pre-vote check on the main thread in the "before" build and the
  apply-time check on the apply thread in the build with the change.
- `scp.timing.trigger-to-ballot` records 0 when a follower votes before its own trigger fires.
- Externalize-to-close and the tx-set store timings come from the logs of eight nodes (two per
  region) in the run with the change, and of four nodes in the "before" run.
- No run has measured an invalid tx set, a dropped speculative execution or a restart from the
  tx-set store on a network; those paths are covered by unit tests only.

## Known limitations of the prototype

Transactions to the leaders only:
- No protocol gate and no version negotiation: a node without the change ignores the identity
  stream, and nodes with the change flood toward it.
- A node whose targets are not all directly connected, identified peers floods its transactions
  as before, so a sparse topology gains nothing.
- The first leader of a new term is predicted one ledger ahead. If the ledger in between costs
  that node its turn, another node leads and gets the pending transactions only at the close.
- After a leader failure the next round's leader lacks the transactions that are older than
  200 ms until that ledger closes; they reach it after it.
- A restarted node forgets the transactions it accepted and had not seen included. Before, every
  node held them.
- A peer can fill a leader's mempool with invalid transactions that outbid valid ones; the leader
  drops them when it builds. A refused submission returns an error without its result code.
- Followers verify the set's signatures at apply (0.10 s at 6,400 transactions, 0.14 s at 8,500,
  on 15 tasks). On 4-core instances that cost made the change miss a 1,000 ms target at 5,000 TPS
  unless validation also used the hyper-threads (`BATCH_EXECUTOR_TASKS`).
- Copies of a transaction that reach a node after its ledger closed are counted
  (`overlay.flood.tx-late-copy`), not dropped.

Leader terms and pre-validation: see [#5502](https://github.com/stellar/stellar-core/pull/5502).
The schedule change has no protocol gate, and a term gives one node five ledgers in a row.

Apply-time validation and speculative execution:
- A nondeterministic validity check used to cost liveness at most. It now forks the ledger. The
  check's node-local inputs are the binary's protocol version, the network passphrase and, in test
  builds, the load generation flags; validators that differ in those would disagree.
- A leader can get an invalid set externalized. The ledger is empty, and the set's transactions
  are still removed from every mempool, so a faulty leader can drop pending transactions at no
  cost whenever it leads.
- Downstream consumers see new shapes: a LedgerCloseMeta with a non-empty tx set and no
  transaction meta, and a history `transactions` entry without a `results` entry.
- If the overlay drops a set Core built (hash mismatch or encode error, both Core bugs), Core
  still believes the set is stored.
- A value whose execution was dropped is not executed speculatively again. `ledger.ledger.close`
  leaves out the time a finished execution waits for externalize.
- A parked execution keeps a SQL transaction open for the length of the ballot. It is not
  confirmed that no other writer on the main database can make its commit fail.
- Speculative execution was never measured with real transaction execution.
- There is no compatibility work beyond the protocol gate.

Tx-set push:
- The push is not relayed. A node that is not connected to the leader requests the set from the
  peer that relayed the proposal.
- A follower that drops a pushed set on arrival (its own limits for unrequested sets) only gets
  it through Core's 1 s retry. Honest leaders do not trigger those limits.
- The limits on unrequested sets can be stepped around by a peer that sends SCP envelopes naming
  unknown tx sets, and the 8 MiB congestion window applies to every stream of a connection. Both
  are prototype settings.
- Both runs that reached the inclusion check with the push held 60 transactions more than offered
  (see [The inclusion check](#the-inclusion-check)). Not explained.

Leader schedule:
- The leader schedule's node set is whatever transitive quorum each node currently knows. It
  assumes a symmetric, static quorum.
- Upgrades: a value needs a quorum of nodes that armed the same upgrades; there is no stripping or
  merging. A follower follows a leader's empty-tx-set ballot under the weaker ballot validation.
- Leader grinding through the header hash and fairness between organizations are out of scope.
- The leader timer extension is broader than intended, and a proposal that arrives just after a
  timeout is wasted. Neither occurred in these runs.
- A build with `--disable-tests` did not compile with the leader schedule (test headers reached
  from `herder/HerderImpl.cpp`) and was not tried since.

## Building and the rest of the documentation

Unchanged from stellar-core: see [INSTALL.md](./INSTALL.md), [CONTRIBUTING.md](./CONTRIBUTING.md)
and [`./docs`](./docs).

Test status with experiments 4 and 5 (`6ab837c27`, #5501 on top of #5502): the full test suite
passes, also with `--all-versions`, including the Rust overlay's tests.

Test status of the branch up to experiment 3 (`0c9a02e56`, the same tree as the merge of #5498):
- The full test suite passes (615 test cases).
- With `--all-versions` the suite passes on `a33da62c4`, the commit before the IPC fix. On
  `0c9a02e56` one test case of the leader-schedule change failed ("PendingEnvelopes
  recvSCPEnvelope", at protocol version 4): its helper tries 100 random peers and needs one that
  does not lead round 1, and for that day's test seed and that partition's node key the local
  node sorts last in the schedule, so every peer leads. The binary of #5495 alone fails the same
  way with the same seed and instance number, and the test passes with instance number 0. It is
  a flaw of that test helper, not of the later changes. Not fixed.
