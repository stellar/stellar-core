# stellar-core: leader schedule and leader tx-set push (experimental branch)

**This branch is an experiment. It is not production code and must not run on a real network.**
It has no feature flag, no protocol gate, and it is not compatible with nodes that nominate.

The latest experiment (the leader pushes its tx set) was measured on commit `8f7768be7` on
2026-11-11. "Before" is the previous experiment on this branch (the leader schedule alone), commit
`daa965f4c`, measured on 2026-11-10 on the same fixture. One run per setting on each side.

## What this branch is

It is the overlay-v2 stack up to [#5459](https://github.com/stellar/stellar-core/pull/5459)
(Rust overlay over QUIC, millisecond close times, compressed tx-set responses, leader-only tx-set
construction) plus two experiments:

1. **No nomination** ([#5495](https://github.com/stellar/stellar-core/pull/5495), the "before"
   below). The SCP nomination phase is deleted and replaced by a leader schedule that every node
   computes locally.
   - **Schedule.** For slot N, the nodes of the transitive quorum are sorted by
     `SHA256(hash of ledger header N-1 || node id)`. The leader of round r is entry `(r-1) mod n`.
   - **Rounds.** A round is a ballot counter. The round's leader builds its value and goes straight
     to the ballot protocol with `(r, value)`. The other nodes vote for the leader's ballot once it
     passes proposal validation. A timeout (the ballot timeout settings) hands the slot to the next
     leader.
2. **The leader pushes its tx set** ([#5497](https://github.com/stellar/stellar-core/pull/5497),
   the latest experiment). Before, a follower saw the leader's proposal, asked for the tx set and
   received it about two round trips later. Now:
   - **Push.** The round's leader sends the set it is about to propose to every connected peer as
     soon as the set is built, without waiting for a request. The proposal follows about 70 ms
     later.
   - **Use.** A follower's overlay verifies a pushed set and keeps it under the hash of its own
     content. Core only gets it when an SCP statement names that hash, so a pushed set that does not
     match the proposal is never used.
   - **Fallback.** The request path is unchanged, for a node that did not get the push.
   - **One flight.** Every QUIC connection starts with an 8 MiB congestion window, so that a set
     leaves in one flight instead of being paced over a round trip.
   - Details and limits: [docs/rust-overlay/txset-fetching.md](docs/rust-overlay/txset-fetching.md).

Unchanged by both: the ballot protocol's accept, confirm and commit rules, value validation, apply,
ledger state and meta.

## Performance results

Target block time 1,000 ms, 3,000 TPS, 30 validators in 4 regions.

| | Before (`daa965f4c`) | With the tx-set push (`8f7768be7`) |
|---|---|---|
| Block time windows | **missed** the first window; the load stops there | **on target in all four windows**, on all 30 nodes |
| p75, average over 30 nodes | 1,386 ms (limit 1,200) | 1,091 / 1,096 / 1,109 / 1,115 ms |
| p99, average over 30 nodes | 1,599 ms (limit 2,000) | 1,227 / 1,243 / 1,253 / 1,290 ms |
| Time per ledger | 1.33 s | 1.05-1.07 s |
| Tx-set fetch as seen by a follower's Core | 217 ms | 16 ms |

In short: before, the stages of one ledger added up to more than a second and the network backed
up. With the push the tx set is already at the follower when the proposal arrives, a ledger's
stages fit into about 0.9 s, and the network keeps up with the load at a 1 s target.

**One caveat on correctness checks.** Every node's ledgers held 60 transactions more than the
2,880,000 offered: about 60 transactions were included in two ledgers. This is an artifact of the
test mode, in which ledgers are not applied (see [The inclusion check](#the-inclusion-check)). The
run was judged with the exact check and reported FAIL for that check alone; the check has since
been relaxed to allow such duplicates
([performance-testing#4](https://github.com/stellar-experimental/performance-testing/pull/4)).
All other checks passed.

### 1,000 ms target, with the tx-set push

Load ran its full 900 s. Every node was on target in every window.

| Window | p75, average (range over nodes) | p99, average (range over nodes) |
|---|---|---|
| 1 | 1,091 ms (1,085-1,096) | 1,227 ms (1,205-1,273) |
| 2 | 1,096 ms (1,090-1,105) | 1,243 ms (1,227-1,272) |
| 3 | 1,109 ms (1,104-1,116) | 1,253 ms (1,231-1,360) |
| 4 (2 minutes) | 1,115 ms (1,109-1,125) | 1,290 ms (1,245-1,396) |

Limits: p75 below 1,200 ms, p99 at most 2,000 ms.

Checks:
- **No fork**, all 30 nodes in sync, still closing ledgers after the load: passed.
- **Error counters zero** (invalid signatures, invariant failures, publish failures, invalid values,
  lost sync), no node restarts: passed.
- **No wasted rounds:** every ledger was decided by the first leader. 0 leader timeouts, 0 ballot
  timeouts, 0 empty transaction sets.
- **Inclusion:** 2,880,060 transactions in every node's ledgers against 2,880,000 offered. See
  below.

### Before and after, stage by stage

5-minute means at the end of the first window, median over the 30 nodes.

| | Before (`daa965f4c`) | With the tx-set push (`8f7768be7`) |
|---|---:|---:|
| Ledgers per 5 minutes | 225 | 285 |
| Transactions per ledger | 3,750, the cap, on every ledger | 3,155 (what was offered; below the cap) |
| Leader tx-set build | 0.25 s | 0.15 s |
| Tx-set fetch (follower's Core, request to delivery) | 0.217 s | **0.016 s** |
| Tx sets a follower had to wait for from the network | every one | none in this window |
| Tx-set validation (follower) | 0.12 s | 0.11 s |
| Trigger to first ballot (includes the build, the fetch and validation) | 0.70 s | **0.42 s** |
| First ballot to externalize | 0.41 s | 0.33 s |
| Ledger close (simulated apply, 50 us per operation) | 0.20 s | 0.16 s |
| **Sum: trigger to close** | **1.30 s** | **0.91 s** |
| Leader timeouts, ballot timeouts, empty tx sets | 0, 0, 0 | 0, 0, 0 |

What the push did, over the whole load (totals of the 30 nodes):

| | |
|---|---:|
| Tx sets pushed (one per peer) | 28,507 |
| Deliveries that were already at the follower when its Core asked | 28,181 |
| Requests that crossed a push (the leader did not send a second copy) | 64 |
| Pushes dropped or evicted by a follower's limits, write errors | 0 |
| Requests Core had to repeat after its 1 s retry interval | 0 |

- **The tx-set round trips are gone.** On one follower whose logs were saved (us-west-2), all 817
  sets it received during the load arrived by push before its Core asked, a median 38 ms ahead of
  the request (minimum 18 ms). Measured from four nodes in four regions, a follower's Core held the
  set a median 126 ms after the leader finished building it (76-169 ms by region pair); before, that
  took about 480 ms on a 147 ms link.
- **The two runs are in different regimes.** Before, a ledger could not close in less than 1.33 s,
  so 3,000 TPS filled every set to the cap and the mempool backed up. Now ledgers hold what was
  offered. That also makes stages the push does not touch cheaper: the leader pulls 3,200
  candidates instead of 7,700, and sets are smaller.
- **Two changes were measured together:** the push and the 8 MiB congestion window. One run cannot
  say how much each contributes.
- **Block time is about 1.05 s although the stages sum to 0.91 s**, and the windows drifted up by
  about 25 ms over the 16 minutes. Neither was investigated.

### The inclusion check

The test compares the transactions in every node's ledgers with the number offered. All 30 nodes
held exactly 60 more (the same on every node, so no fork). The generators had submitted exactly
2,880,000 and every node had pulled each transaction once, so about 60 transactions (0.002%) were
included in two ledgers.

Why this happens in the test and not on a network that applies ledgers:

1. A leader that is not backed up proposes everything in its mempool, including a transaction
   that was submitted a moment ago and is still being flooded to the far nodes (up to about 0.5 s).
2. With the push, a follower closes the ledger about 0.5 s after the leader built its set, so now
   and then a far node closes the ledger before its own copy of that transaction has arrived.
3. The overlay's mempool removes a ledger's transactions when the ledger closes and keeps no memory
   of them, on purpose. The late copy is admitted as a new transaction.
4. On a real network Core rejects that copy when the node next builds a set, because its sequence
   number was already used. The test runs in "overlay-only" mode: ledgers are not applied, sequence
   numbers never advance, and that check is switched off. So the node includes the transaction a
   second time.

That this path produced the 60 is inferred from the counters; the logs carry no transaction hashes.
Before, a follower closed the ledger about a second after the leader's build, well after flooding
had finished, so the two never met: the completed "before" run at 2,000 ms held exactly 2,880,000.

It is accepted as an artifact of overlay-only mode. The check now allows up to 1 in 10,000
transactions more than offered and reports how many there were; fewer than offered, more than
that, and nodes that disagree still fail.

### Not rerun: the 2,000 ms target

The 2,000 ms target was measured for the "before" commit only: PASS in all four windows (p75
2,066-2,109 ms, p99 2,286-2,324 ms), with exactly 2,880,000 transactions on every node, and 0.19 s
less from trigger to externalize than with nomination (1.31 s against 1.50 s on #5459). The details
are in [#5495](https://github.com/stellar/stellar-core/pull/5495). It was not repeated with the
push.

### Where a ledger's time goes now

With the tx-set round trips gone, what is left of the 0.91 s from trigger to close at 1,000 ms:

1. **The ballot: 0.33 s.** Followers validate before they vote, and the ballot then needs four
   one-way trips.
2. **The leader's build: 0.15 s**, on the critical path. With the trigger mostly on time, the
   first-round leader also builds once before the trigger and discards that set unless it was
   capacity limited (562 builds for 285 ledgers).
3. **Validation: 0.11 s.**
4. **About 70 ms between the leader finishing its set and its proposal leaving.** SCP state,
   including the tx set, is written to the database before each broadcast. That this write accounts
   for most of the 70 ms is inferred, not timed. The push already leaves during this time.
5. **Ledger close: 0.16 s**, almost all of it the simulated apply.

## How it was measured

Both runs (before and after) use the `block_time_check` mission of perfnet, SDF's tooling for
performance networks on AWS. Each run launches a fresh network, brings it to the target settings,
runs the load, judges it, and terminates the network.

### Network topology

| | |
|---|---|
| Validators | 30, all validating, no watchers |
| Organizations | 10 home domains of 3 validators each, all `HIGH` quality |
| Quorum set | Generated by stellar-core from the home domains: 7 of 10 organizations, 2 of 3 validators within each |
| Regions | 4 AWS regions: eu-central-1 (8 nodes), ap-southeast-1 (8), us-east-1 (7), us-west-2 (7) |
| Placement | 9 organizations have their three validators in three different regions; one has two in us-east-1 and one in ap-southeast-1. The layout was identical in both runs |
| Connectivity | Full mesh: every node is configured with the other 29 as its only peers |
| Round-trip times seen from us-west-2 | us-east-1 about 60 ms, eu-central-1 about 147 ms, ap-southeast-1 about 180 ms |

### Hardware and software

| | |
|---|---|
| Instance type | c6id.2xlarge: 8 vCPU (Intel Ice Lake, with SHA extensions), 16 GiB RAM, local NVMe |
| Build, with the push | Commit `8f7768be7`, the perftests package `30.0.1-3675.8f7768be7a.noble~perf.leader.txset.push~perftests`. All 30 nodes reported that commit in their build label |
| Build, before | Commit `daa965f4c`, the perftests package `30.0.1-3674.daa965f4ce.noble~perf.leader.schedule.no.nomination~perftests` |
| Overlay | The Rust overlay of this stack: QUIC, one connection per peer. With the push, every connection starts with an 8 MiB congestion window (quinn's default before) |
| Ledger protocol | 30, upgraded from genesis during preparation |
| Database | SQLite |

### Node configuration

Beyond identity, peers and home domains, every node ran with:

```
ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING=true
UNSAFE_QUORUM=true
FAILURE_SAFETY=0
GENESIS_TEST_ACCOUNT_COUNT=1000000
TARGET_PEER_CONNECTIONS=29
MAX_ADDITIONAL_PEER_CONNECTIONS=87
PREFERRED_PEERS_ONLY=true
FLOOD_DEMAND_BACKOFF_DELAY_MS=1000
INVARIANT_CHECKS=[]
TESTING_IGNORE_LEDGER_TIME_UPGRADE_BOUNDS=true
TESTING_MAX_CLASSIC_BYTE_ALLOWANCE=1048576
TESTING_MAX_SOROBAN_BYTE_ALLOWANCE=9437184
LOADGEN_PREGENERATED_TRANSACTIONS_FILE="/var/lib/stellar-core/loadgen-txs.xdr"
OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING=[50]     # [25] in the 2,000 ms run
OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING=[100]
```

`EXPERIMENTAL_PARALLEL_TX_SET_DOWNLOAD` was off (the default). Catchup and automatic restart are
off, so a node that crashes or falls behind stays that way and fails the run.

### Network settings (applied by upgrade before the load)

| Setting | 1,000 ms runs (before and after) | 2,000 ms run (before only) |
|---|---:|---:|
| Target ledger close time | 1,000 ms | 2,000 ms |
| SCP timeouts (ballot and nomination, initial and increment) | 2,000 ms | 2,000 ms |
| Maximum transactions per ledger (125% of the offered load per block) | 3,750 | 7,500 |
| Maximum tx-set size in bytes | 10 MiB (1 MiB classic + 9 MiB Soroban) | same |
| Soroban limits | raised to fit the load | raised to fit the load |

The leader timer of this branch uses the ballot timeout, so a round's leader had 2 s (4 s with its
one extension) in every run.

### Load

| | |
|---|---|
| Transactions | Stellar Asset Contract (SAC) payments, pregenerated; no classic transactions |
| Rate | 3,000 TPS offered: every validator generates 100 TPS |
| Accounts | 1,000,000 genesis accounts, 33,333 per generator |
| Duration | 60 s warm-up, then 900 s measured |
| Apply | Skipped ("overlay-only" mode) and replaced by a sleep per operation, sized as a 150 ms budget per ledger at the offered load: 50 us per operation at 1,000 ms, 25 us at 2,000 ms |

Command (before and after differ only in the build):

```
block_time_check.py \
  --stellar-core-version 30.0.1-3675.8f7768be7a.noble~perf.leader.txset.push~perftests \
  --block-time-ms 1000 \
  --soroban-tps 3000 --soroban-type sac --classic-tps 0 \
  --simulate-apply-budget-ms 150 --load-duration-s 900 \
  --instance-type c6id.2xlarge --save-logs always
```

Defaults that matter: 10 organizations, validators as generators, `--scp-timeout-ms 2000`,
`--max-tx-set-bytes 10485760`, 1,000,000 genesis accounts, seed 0.

### How a run is judged

- Ledger close times are read per node from Prometheus (scraped every 10 s) and judged over
  5-minute windows after the warm-up.
- **PASS** needs, on every node in every window: p75 within [0.8 T, 1.2 T), p99 at most 2 T, and at
  least one close per 2 T on average, where T is the target block time. It also needs every node's
  ledgers to hold the offered transactions, zero error counters, all nodes in sync, no fork, and
  the network still closing ledgers afterwards.
- "Hold the offered transactions" was an exact comparison when these runs were judged. It now
  allows up to 1 in 10,000 more, for the duplicates overlay-only mode lets through (see
  [The inclusion check](#the-inclusion-check)).
- The load stops at the first window that misses, so a FAIL can rest on one window (as in the
  "before" run).

### Limits of these numbers

- One run on each side. The "before" numbers at 1,000 ms come from a single 5-minute window, and
  from a commit that differs from this branch's base in formatting only.
- The push and the 8 MiB congestion window were not measured separately.
- Apply is simulated, so these are consensus and overlay numbers, not end-to-end ledger close.
- `scp.timing.trigger-to-ballot` records 0 when a follower votes before its own trigger fires.
- The timings between a leader and its followers come from live logs of four nodes (one per
  region) and the saved logs of one node.

## Known limitations of the prototype

Tx-set push:
- The push is not relayed. A node that is not connected to the leader requests the set from the
  peer that relayed the proposal.
- A follower that drops a pushed set on arrival (its own limits for unrequested sets) only gets
  it through Core's 1 s retry. Honest leaders do not trigger those limits.
- The limits on unrequested sets can be stepped around by a peer that sends SCP envelopes naming
  unknown tx sets, and the 8 MiB congestion window applies to every stream of a connection. Both
  are prototype settings.
- Transactions can be included twice in overlay-only mode (above).

Leader schedule:
- The leader schedule's node set is whatever transitive quorum each node currently knows. It
  assumes a symmetric, static quorum.
- Upgrades: a value needs a quorum of nodes that armed the same upgrades; there is no stripping or
  merging. A follower follows a leader's empty-tx-set ballot under the weaker ballot validation.
- Leader grinding through the header hash and fairness between organizations are out of scope.
- The leader timer extension is broader than intended, and a proposal that arrives just after a
  timeout is wasted. Neither occurred in these runs.
- A build with `--disable-tests` does not compile at this commit (test headers reached from
  `herder/HerderImpl.cpp`).

## Building and the rest of the documentation

Unchanged from stellar-core: see [INSTALL.md](./INSTALL.md), [CONTRIBUTING.md](./CONTRIBUTING.md)
and [`./docs`](./docs). The full test suite passes on `8f7768be7`, also with `--all-versions`.
