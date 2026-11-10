# stellar-core: leader schedule without SCP nomination (experimental branch)

**This branch is an experiment. It is not production code and must not run on a real network.**
It has no feature flag, no protocol gate, and it is not compatible with nodes that nominate.

Results below were measured on commit `daa965f4c` on 2026-11-10 and are one run per setting.

## What this branch is

It is the overlay-v2 stack up to [#5459](https://github.com/stellar/stellar-core/pull/5459)
(Rust overlay over QUIC, millisecond close times, compressed tx-set responses, leader-only tx-set
construction) plus one change: **the SCP nomination phase is deleted and replaced by a leader
schedule that every node computes locally.** Draft PR:
[#5495](https://github.com/stellar/stellar-core/pull/5495).

- **Schedule.** For slot N, the nodes of the transitive quorum are sorted by
  `SHA256(hash of ledger header N-1 || node id)`. The leader of round r is entry `(r-1) mod n`.
- **Rounds.** A round is a ballot counter. The round's leader builds its value and goes straight to
  the ballot protocol with `(r, value)`. The other nodes vote for the leader's ballot once it passes
  proposal validation. A timeout (the ballot timeout settings) hands the slot to the next leader.
- **Unchanged.** The ballot protocol's accept, confirm and commit rules, value validation, apply,
  ledger state and meta.

The question it answers: how much of a ledger's time is nomination, and what limits block time once
nomination is gone?

## Performance results

| Target block time | Load | Verdict | Block time measured (average over 30 nodes) |
|---|---|---|---|
| **2,000 ms** | 3,000 TPS | **PASS**, all four windows | p75 2,066-2,109 ms, p99 2,286-2,324 ms |
| **1,000 ms** | 3,000 TPS | **FAIL**, first window | p75 1,386 ms (limit 1,200), p99 1,599 ms |

In short: at 2 s the network keeps up and has more slack per ledger than with nomination. At 1 s it
settles at about 1.33 s per ledger. Nothing breaks; the stages of one ledger simply add up to more
than a second.

### 2,000 ms target: PASS

Load ran its full 900 s. Every node passed every window.

| Window | p75, average (range over nodes) | p99, average (range over nodes) |
|---|---|---|
| 1 | 2,109 ms (2,086-2,128) | 2,324 ms (2,282-2,359) |
| 2 | 2,066 ms (2,058-2,076) | 2,286 ms (2,239-2,328) |
| 3 | 2,082 ms (2,067-2,091) | 2,296 ms (2,251-2,333) |
| 4 | 2,094 ms (2,080-2,105) | 2,303 ms (2,262-2,344) |

Checks, all passed:
- **Inclusion:** every node's ledgers hold exactly the 2,880,000 offered transactions.
- **No fork**, all 30 nodes in sync, still closing ledgers after the load.
- **Error counters zero** (invalid signatures, invariant failures, publish failures, invalid values,
  lost sync), no node restarts.
- **No wasted rounds:** every ledger was decided by the first leader. 0 leader timeouts, 0 ballot
  timeouts, 0 empty transaction sets.

Against the same fixture on #5459's head `ebdd73635` (with nomination), run the same day on the same
instance type:

| 5-minute means, median node | #5459 `ebdd73635` | This branch `daa965f4c` |
|---|---:|---:|
| Verdict | PASS | PASS |
| p75 per window | 2,112 / 2,099 / 2,101 / 2,099 ms | 2,109 / 2,066 / 2,082 / 2,094 ms |
| p99 per window | 2,334 / 2,391 / 2,324 / 2,354 ms | 2,324 / 2,286 / 2,296 / 2,303 ms |
| Ledgers per 5 minutes | 147-148 | 149-150 |
| Transactions per ledger | 6,092 | 6,003 |
| Trigger to first ballot | 1.21 s | 0.88 s |
| First ballot to externalize | 0.29 s | 0.43 s |
| **Trigger to externalize** | **1.50 s** | **1.31 s** |
| Leader tx-set build | 0.28 s | 0.27 s |
| Tx-set fetch (follower) | 0.24 s | 0.26 s |
| Tx-set validation (follower) | 0.20 s | 0.19 s |
| Ledger close | 0.164 s | 0.162 s |

- Block time is the same on both, as expected: when the network keeps up, the 2 s trigger sets the
  cadence.
- The time from trigger to externalize is 0.19 s shorter. The part before the first ballot that is
  not build, fetch or validation fell from about 0.48 s (nomination rounds) to about 0.15 s.
- Some of that came back in the ballot phase. Nodes now start their ballot at different times, as
  each finishes validating the leader's set, so the early ones wait for a quorum of the others.

### 1,000 ms target: FAIL

The first 5-minute window missed on all 30 nodes, and the test stops the load at the first miss.

| Window 1 | Value |
|---|---|
| p75, average (range over nodes) | 1,386 ms (1,375-1,399); the limit is below 1,200 ms |
| p99, average (range over nodes) | 1,599 ms (1,582-1,628); the limit is 2,000 ms |
| Ledgers per 5 minutes | 225 (1.33 s each) |
| Transactions per ledger | 3,750, the cap, on every ledger |
| Leader timeouts, ballot timeouts, empty tx sets | 0, 0, 0 |
| Error counters, in sync, no fork, still closing | all passed |

The network was over capacity at this block time: 3,000 TPS x 1.33 s is 4,000 transactions offered
per ledger against a cap of 3,750, so the mempool backed up and every ledger started the instant
the previous one closed. Inclusion is not checked after an early stop. No baseline was run at this
setting.

### Where a ledger's time goes

| Stage (5-minute means, median node) | 2,000 ms target (6,000 txs) | 1,000 ms target (3,750 txs) |
|---|---:|---:|
| Leader builds the tx set | 0.27 s | 0.25 s |
| Trigger to first ballot (includes the build, the fetch and validation) | 0.88 s | 0.70 s |
| First ballot to externalize | 0.43 s | 0.41 s |
| Ledger close (150 ms / 187 ms of it is simulated apply) | 0.16 s | 0.20 s |
| **Sum** | **1.47 s** | **1.30 s** |

Halving the transaction set took only 0.17 s off the cycle. What remains is mostly cross-region
round trips, not CPU:

1. **Tx-set dissemination takes about 2.5 round trips.** A follower receives the proposal, requests
   the set from the proposer, and the response takes two flights. Seen from us-west-2, the fetch
   took 27 ms from the same region, 150 ms from us-east-1, 301 ms from eu-central-1 and 362 ms from
   ap-southeast-1. Nothing is pushed, and transactions are sent in full although followers already
   hold nearly all of them.
2. **Followers validate before they vote, and the ballot then needs four one-way trips.**
3. **About 70 ms pass between the leader finishing its set and the proposal reaching a peer in the
   same region.** SCP state, including the tx set, is written to the database before each
   broadcast. That this write accounts for most of the 70 ms is inferred, not timed.
4. **The leader's build is on the critical path.** At 1 s nothing can be built ahead of the trigger.

## How it was measured

Both runs use the `block_time_check` mission of perfnet, SDF's tooling for performance networks on
AWS. Each run launches a fresh network, brings it to the target settings, runs the load, judges
it, and terminates the network.

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
| Build | Commit `daa965f4c`, the perftests package `30.0.1-3674.daa965f4ce.noble~perf.leader.schedule.no.nomination~perftests`. All 30 nodes reported that commit in their build label |
| Overlay | The Rust overlay of this stack: QUIC, one connection per peer |
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
OP_APPLY_SLEEP_TIME_DURATION_FOR_TESTING=[25]     # [50] in the 1,000 ms run
OP_APPLY_SLEEP_TIME_WEIGHT_FOR_TESTING=[100]
```

`EXPERIMENTAL_PARALLEL_TX_SET_DOWNLOAD` was off (the default). Catchup and automatic restart are
off, so a node that crashes or falls behind stays that way and fails the run.

### Network settings (applied by upgrade before the load)

| Setting | 2,000 ms run | 1,000 ms run |
|---|---:|---:|
| Target ledger close time | 2,000 ms | 1,000 ms |
| SCP timeouts (ballot and nomination, initial and increment) | 2,000 ms | 2,000 ms |
| Maximum transactions per ledger (125% of the offered load per block) | 7,500 | 3,750 |
| Maximum tx-set size in bytes | 10 MiB (1 MiB classic + 9 MiB Soroban) | same |
| Soroban limits | raised to fit the load | raised to fit the load |

The leader timer of this branch uses the ballot timeout, so a round's leader had 2 s (4 s with its
one extension) in both runs.

### Load

| | |
|---|---|
| Transactions | Stellar Asset Contract (SAC) payments, pregenerated; no classic transactions |
| Rate | 3,000 TPS offered: every validator generates 100 TPS |
| Accounts | 1,000,000 genesis accounts, 33,333 per generator |
| Duration | 60 s warm-up, then 900 s measured |
| Apply | Skipped ("overlay-only" mode) and replaced by a sleep per operation, sized as a 150 ms budget per ledger at the offered load: 25 us per operation at 2,000 ms, 50 us at 1,000 ms |

Commands (the two runs differ only in the block time):

```
block_time_check.py \
  --stellar-core-version 30.0.1-3674.daa965f4ce.noble~perf.leader.schedule.no.nomination~perftests \
  --block-time-ms 2000 \          # 1000 for the second run
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
  ledgers to hold exactly the offered transactions, zero error counters, all nodes in sync, no
  fork, and the network still closing ledgers afterwards.
- The load stops at the first window that misses, so a FAIL can rest on one window.

### Limits of these numbers

- One run per setting, and one baseline run (at 2,000 ms only).
- Apply is simulated, so these are consensus and overlay numbers, not end-to-end ledger close.
- `scp.timing.trigger-to-ballot` records 0 when a follower votes before its own trigger fires, so it
  is not strictly comparable with the nomination build. Triggers were on time in the 2,000 ms run.
- The per-stage timeline and the fetch times by region come from one node's logs at 2,000 ms and
  four nodes' logs at 1,000 ms.

## Known limitations of the prototype

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
and [`./docs`](./docs). The full test suite passes on `daa965f4c`, also with `--all-versions`.
