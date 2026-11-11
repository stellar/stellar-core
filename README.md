# stellar-core: leader schedule, leader tx-set push and apply-time tx-set validation (experimental branch)

**This branch is an experiment. It is not production code and must not run on a real network.**
The leader schedule and the tx-set push have no feature flag and no protocol gate, and the branch
is not compatible with nodes that nominate.

The benchmark is now **48 validators (16 organizations), 5,000 TPS, 1,000 ms target block time**.
The latest experiment (vote without validating the tx set, validate and execute at apply) was
measured there on commit `a33da62c4` on 2026-11-11. "Before" is the previous experiment on this
branch (the leader's tx-set push), commit `8f7768be7`, measured on the same day on the same
fixture. One run per build.

## What this branch is

It is the overlay-v2 stack up to [#5459](https://github.com/stellar/stellar-core/pull/5459)
(Rust overlay over QUIC, millisecond close times, compressed tx-set responses, leader-only tx-set
construction) plus three experiments:

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

Unchanged by all three: the ballot protocol's accept, confirm and commit rules. For a valid tx
set, ledger state and meta are also unchanged; experiment 3 changes when a set is validated and
executed, and what an invalid set does to the ledger.

## Performance results

Target block time 1,000 ms, 5,000 TPS, 48 validators in 4 regions.

| | Before: tx-set push (`8f7768be7`) | With apply-time validation (`a33da62c4`) |
|---|---|---|
| Block time windows | **missed** the first window on all 48 nodes; the load stops there | **on target in all four windows**, on all 48 nodes |
| p75, average over 48 nodes | 1,461 ms (limit 1,200) | 1,094 / 1,116 / 1,130 / 1,137 ms |
| p99, average over 48 nodes | 1,638 ms (limit 2,000) | 1,313 / 1,453 / 1,408 / 1,435 ms |
| Time per ledger | 1.42 s | 1.04-1.07 s |
| Trigger to ledger closed (sum of the stages) | 1.37 s | 0.79-0.84 s |
| Run verdict | FAIL (block time) | FAIL (one node dropped out of consensus, see the caveat) |

In short: before, the stages of one ledger added up to 1.37 s, every tx set was full and the
network backed up. With validation moved out of the vote and the ledger executed while the ballot
runs, a ledger's stages fit into about 0.8 s and the network keeps up with 5,000 TPS at a 1 s
target.

**Caveat: the run with the change is a FAIL, although every block time window was on target.**
15 s before the load ended, one node (of 48) stopped reading from its overlay process and fell out
of consensus; two more did the same two minutes after the load. The cause is a bug in the
Core-overlay IPC socket code that this experiment did not introduce (see
[The node that dropped out](#the-node-that-dropped-out)). The fix is on this branch (`0c9a02e56`);
it has not been measured on a network yet. Because the load counted as ended early, the inclusion
check was not judged.

### 1,000 ms target, with apply-time validation

Load ran its full 900 s. Every node was on target in every window.

| Window | p75, average (range over nodes) | p99, average (range over nodes) |
|---|---|---|
| 1 | 1,094 ms (1,083-1,110) | 1,313 ms (1,260-1,394) |
| 2 | 1,116 ms (1,107-1,126) | 1,453 ms (1,393-1,522) |
| 3 | 1,130 ms (1,116-1,145) | 1,408 ms (1,357-1,499) |
| 4 (2.4 minutes) | 1,137 ms (1,121-1,160) | 1,435 ms (1,381-1,486) |

Limits: p75 below 1,200 ms, p99 at most 2,000 ms.

Checks:
- **No fork**, still closing ledgers after the load: passed.
- **Error counters zero** (invalid signatures, invariant failures, publish failures, invalid
  values), no node restarts: passed.
- **All nodes in sync: failed.** One node was out of sync at the end of the load.
- **No wasted rounds during the load:** every ledger was decided by the first leader. 0 leader
  timeouts, 0 ballot timeouts, 0 empty transaction sets. No tx set was found invalid at apply.
- **Inclusion: not judged.** Every one of the 47 healthy nodes holds 4,798,690 transactions
  against 4,800,000 offered, 1,310 fewer (0.027%). The node that dropped out kept generating for
  15 s (about 104 transactions per second) while nothing left it, which accounts for a shortfall
  of that size. That these are the missing ones is inferred, not shown.

### Before and after, stage by stage

5-minute means at the end of each run's first window, median over the 48 nodes.

| | Before (`8f7768be7`) | With apply-time validation (`a33da62c4`) |
|---|---:|---:|
| Ledgers per 5 minutes | 211-212 | 288-290 |
| Transactions per ledger | 6,250, the cap, on every ledger | 5,189 (what was offered; over the whole load 5% of the sampled ledgers were at the cap) |
| Leader tx-set build | 0.41 s | 0.23 s |
| Tx-set validation | 0.195 s, before the vote | 0.180 s, at apply, while the ballot runs |
| Trigger to first ballot (includes the build and the fetch; before, also validation) | 0.80 s | **0.42 s** |
| First ballot to externalize | 0.37 s | 0.30 s |
| Externalize to ledger closed (from node logs) | 0.21 s | **0.07 s** |
| **Sum: trigger to ledger closed** | **1.37 s** | **0.79 s** |
| Tx-set fetch (follower's Core, request to delivery) | 0.033 s | 0.052 s |
| Leader timeouts, ballot timeouts, empty tx sets | 0, 0, 0 | 0, 0, 0 |

Over the later windows of the run with the change, the sum is 0.81-0.84 s.

What the new parts did, over the whole load of the run with the change:

| | |
|---|---:|
| Ledgers executed speculatively (total of the 48 nodes) | 46,847 |
| Speculatively executed ledgers that were committed | 46,853 (46,852 ledgers closed; the counters are scraped at slightly different times) |
| Executions dropped because another value externalized | 0 |
| Tx sets found invalid at apply (empty ledger) | 0 |
| Execution (validation plus simulated apply), mean | 0.33-0.35 s |
| Commit after externalize, mean | 0.037-0.041 s |
| Overlay's write of a tx set to disk (8 nodes, 6,957 writes) | median 3.6 ms, p99 8.9 ms, maximum 237 ms |
| Core's write of SCP state before a vote, now without the tx set | 2.2-2.6 ms mean |

- **Every ledger was executed before it externalized or while it did.** No execution was wasted:
  with one leader per round and no timeouts, the value a node votes for is the value that
  externalizes.
- **The execution is slightly longer than the ballot.** It takes 0.34-0.35 s (0.18-0.19 s of
  validation plus the simulated apply), the ballot 0.30 s. In the logs of eight nodes, the ledger
  closed more than 50 ms after externalize on 58% of the ledgers and more than 100 ms after on 21%
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
  (38,668 such requests against 7,032 sets that were already in place; before: 17 against 13,914).
  The leader does not send a second copy, and the fetch costs about 0.02 s more per ledger. Why
  the proposal overtakes the push is inferred from the removed database write, not timed.
- **Block time is 1.04-1.07 s although the stages sum to about 0.8 s**, and the windows drifted up
  by about 45 ms over the 16 minutes. Neither was investigated.

### The node that dropped out

- **What happened.** 15 s before the end of the load, Core on one node logged "Overlay IPC
  connection closed" and stopped reading from its overlay process. Both processes kept running and
  the socket between them was still open, with unread data on Core's side. The node heard nothing
  further from the network and lost sync. Two more nodes did the same two minutes after the load.
- **When.** All three times within 26 ms of a step of a history checkpoint publish.
- **Cause (inferred; the logs carry no error code).** Core's IPC reader treated any failed `recv`
  as a closed connection, including one interrupted by a signal (`EINTR`). stellar-core handles
  `SIGCHLD`, and a checkpoint publish runs child processes.
- **Fix.** `0c9a02e56` on this branch: an interrupted read or write on the IPC socket is retried.
  A unit test interrupts a blocked read with a signal and fails without the fix. The fix has not
  been run on a network yet.
- **Not caused by this experiment, as far as the code shows.** The reader is the same code in the
  "before" build. That run had no such event in 30 minutes and this one had three in 40; one run
  each cannot say whether the change makes it more likely.

### Where a ledger's time goes now

What is left of the 0.79-0.84 s from trigger to ledger closed:

1. **Trigger to first ballot: 0.42-0.47 s.** Most of it is the leader's build (0.23-0.25 s), on
   the critical path. The first-round leader also builds once before the trigger and discards
   that set unless it was capacity limited (542-577 builds for 281-290 ledgers). The tx-set fetch adds 0.05 s.
2. **The ballot: 0.30 s.** Nothing is validated in it any more; it is message delays between four
   regions.
3. **Externalize to ledger closed: 0.07 s.** The commit (0.04 s), plus the end of the execution
   on about half of the ledgers.

The execution itself (0.34-0.35 s) runs beside the ballot.

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

- With the push, every node's ledgers held 60 transactions more than the 2,880,000 offered: about
  60 transactions were included in two ledgers. This is an artifact of the test mode. Ledgers are
  not applied, so sequence numbers never advance, and a copy of a transaction that reaches a far
  node after its ledger closed is admitted and included again. The run was judged with an exact
  comparison and reported FAIL for that check alone. A change that lets the check allow such
  duplicates is proposed
  ([performance-testing#4](https://github.com/stellar-experimental/performance-testing/pull/4)),
  not merged.
- A 2,000 ms target was measured for the leader schedule alone: PASS in all four windows (p75
  2,066-2,109 ms).
- The full results, stage tables and the measurement setup of that fixture are in
  [the README as of #5497](https://github.com/stellar/stellar-core/blob/6d5fc9e814abee2b7e93553b3903b56929bedf91/README.md)
  and in [#5495](https://github.com/stellar/stellar-core/pull/5495).

## How it was measured

Both runs (before and after) use the `block_time_check` mission of perfnet, SDF's tooling for
performance networks on AWS. Each run launches a fresh network, brings it to the target settings,
runs the load, judges it, and terminates the network.

### Network topology

| | |
|---|---|
| Validators | 48, all validating, no watchers |
| Organizations | 16 home domains of 3 validators each, all `HIGH` quality |
| Quorum set | Generated by stellar-core from the home domains. Its rule gives 11 of 16 organizations, 2 of 3 validators within each (computed from the rule; not read back from a node) |
| Regions | 4 AWS regions, 12 nodes each: eu-central-1, us-west-2, ap-southeast-1, us-east-1 |
| Placement | 14 organizations have their three validators in three different regions; 2 have them in two regions. Both runs used the same layout seed |
| Connectivity | Full mesh: every node is configured with the other 47 as its only peers |
| Round-trip times seen from us-west-2 (measured on the earlier fixture, same regions) | us-east-1 about 60 ms, eu-central-1 about 147 ms, ap-southeast-1 about 180 ms |

### Hardware and software

| | |
|---|---|
| Instance type | c6id.2xlarge: 8 vCPU (Intel Ice Lake, with SHA extensions), 16 GiB RAM, local NVMe |
| Build, with apply-time validation | Commit `a33da62c4`, the perftests package `30.0.1-3677.a33da62c49.noble~perf.apply.time.txset.validation~perftests`. All 48 nodes reported that commit in their build label |
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

`EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY` was on (the default) in the run with the change.
`EXPERIMENTAL_PARALLEL_TX_SET_DOWNLOAD` was off (the default). Catchup and automatic restart are
off, so a node that crashes or falls behind stays that way and fails the run.

### Network settings (applied by upgrade before the load)

| Setting | Both runs |
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

Command (before and after differ only in the build):

```
block_time_check.py \
  --stellar-core-version 30.0.1-3677.a33da62c49.noble~perf.apply.time.txset.validation~perftests \
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
  (both runs).

### Limits of these numbers

- One run on each side. The "before" numbers come from a single 5-minute window.
- The run with the change is a FAIL for a node that lost its IPC reader. The block time windows
  include that node up to the moment it stopped; the stage numbers are medians over the 48 nodes.
  The fix has not been measured.
- Apply is simulated by a sleep, so these are consensus and overlay numbers, not end-to-end ledger
  close. The gain from speculative execution is an upper bound, and the two builds do not sleep
  for the same part of the apply (above).
- The inclusion check was judged in neither run.
- `herder.txset.validate` times the pre-vote check on the main thread in the "before" build and the
  apply-time check on the apply thread in the build with the change.
- `scp.timing.trigger-to-ballot` records 0 when a follower votes before its own trigger fires.
- Externalize-to-close and the tx-set store timings come from the logs of eight nodes (two per
  region) in the run with the change, and of four nodes in the "before" run.
- No run has measured an invalid tx set, a dropped speculative execution or a restart from the
  tx-set store on a network; those paths are covered by unit tests only.

## Known limitations of the prototype

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
- Transactions can be included twice in overlay-only mode (see the earlier fixture).

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

Test status of this branch's code (`0c9a02e56`, the same tree as the merge of #5498):
- The full test suite passes (615 test cases).
- With `--all-versions` the suite passes on `a33da62c4`, the commit before the IPC fix. On
  `0c9a02e56` one test case of the leader-schedule change failed ("PendingEnvelopes
  recvSCPEnvelope", at protocol version 4): its helper tries 100 random peers and needs one that
  does not lead round 1, and for that day's test seed and that partition's node key the local
  node sorts last in the schedule, so every peer leads. The binary of #5495 alone fails the same
  way with the same seed and instance number, and the test passes with any other instance number
  tried. It is a flaw of that test helper, not of the later changes. Not fixed.
