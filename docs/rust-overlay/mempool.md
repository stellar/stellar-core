# Mempool

The overlay maintains an **in-process, fee-ordered mempool** of pending
transactions. Core queries it for nomination (`GetTopTxs`), and the
overlay services TX-flood requests from it (`tx_buffer` is *not* the
mempool — see [tx-propagation.md](tx-propagation.md)).

The mempool does no transaction validation: Core validates the
transactions it pulls at nomination, and pulls only as many as fit in the
next ledger (see [Querying](#querying)) so that work is not spent on
transactions that cannot make it into the block.

Source: `flood/mempool.rs`. Owned by `Overlay` in `integrated.rs`,
guarded by `RwLock`.

## Configuration

`integrated.rs:49`:

```rust
Mempool::new(100000, Duration::from_secs(300))
```

| Parameter   | Value      | Notes                                       |
|-------------|------------|---------------------------------------------|
| `max_size`  | 100,000    | Hardcoded; no config knob.                  |
| `max_age`   | 300 s      | Hardcoded; no config knob.                  |

## Indexes

`flood/mempool.rs:78-90`. Two indexes over the same `MempoolEntry` set:

| Index         | Type                                  | Purpose                                  |
|---------------|---------------------------------------|------------------------------------------|
| `by_hash`     | `HashMap<TxHash, MempoolEntry>`       | O(1) lookup, dedup                       |
| `by_fee`      | `[BTreeSet<FeePriority>; 2]`, one per `TxKind` | Ordered access per tx set phase (for `top_by_fee`) |

`MempoolEntry` (`flood/mempool.rs`) is an `Arc<ValidatedTx>` plus
`received_at` (for age-based eviction). The `ValidatedTx` (`wire.rs`)
carries the canonical envelope bytes, sha256 hash, fee, op count and
kind (classic or Soroban) — computed once at the trust boundary where
the transaction entered the process (or supplied by Core for `SubmitTx`),
and shared by reference through the rest of the pipeline.

Classic and Soroban transactions are indexed separately because Core
fills the two tx set phases independently, with independent limits: a
Soroban transaction's (much larger) fee must not crowd classic
transactions out of a pull, and vice versa.

## Fee ordering

`FeePriority::cmp` (`flood/mempool.rs:37-59`) orders by **fee-per-op**
without using division. Given two priorities `(fee1, ops1)` and
`(fee2, ops2)`:

```
fee1 / ops1 > fee2 / ops2  iff  fee1 * ops2 > fee2 * ops1
```

Tie-breakers in order: equal ratio → fewer ops wins → equal ops → hash
comparison (deterministic).

> **Caveat — fee overflow**: `fee * num_ops` is computed in `u64`. For
> pathological values (very large fee or ops counts) this can overflow.
> Not a near-term concern for real Stellar fees but worth noting.

## Insertion

`Mempool::insert` (`flood/mempool.rs`):

1. **Dedup**: if `by_hash` already contains this hash, return `false`.
2. **Capacity check**: while at `max_size`, call `evict_lowest_fee`.
3. Add to `by_hash` and to the `by_fee` index for the tx's kind.
4. Return `true`.


## Eviction

- **Capacity-based** (`evict_lowest_fee`, `flood/mempool.rs`): compares
  the lowest-priority entry of each kind's `by_fee` index and removes the
  lower of the two via `remove`. Called from `insert` on capacity
  overflow — happens *synchronously per insert*.
- **Age-based** (`evict_expired`, `flood/mempool.rs:152-166`): scans for
  entries with `now - received_at > max_age` and removes them. Called
  only from the `RemoveTxsFromMempool` handler (i.e. piggybacked on
  externalization) — there is no periodic timer that invokes it.

## Removal on externalization

When Core sends `TxSetExternalized`, `main.rs:1180` calls
`overlay_handle.remove_txs_sync(tx_hashes)` and **awaits** completion.
The synchronous wait is intentional: the next nomination cycle must not
see TXs that were just included in the closing ledger.

The `RemoveTxsFromMempool` handler (`integrated.rs:89-100`) removes each
hash from both indexes, then runs `evict_expired`.

## Insertion sources

Two paths into the mempool, one per trust boundary. Both produce the
same `Arc<ValidatedTx>` currency, so every entry carries its real fee,
op count and kind, and fee ordering is correct regardless of origin.

### `SubmitTx` from Core (`main.rs`)

Core submits `(data, fee, num_ops, flags)` over IPC; the handler mints the
entry with `ValidatedTx::from_core_trusted`, which trusts Core's metadata
(including the classic/Soroban kind in `flags`) and does not re-decode
(it only rejects fee-bumps via the envelope discriminant and hashes the
bytes).

### TX received over the network (`main.rs`)

`LibP2pOverlayEvent::TxReceived` already carries an `Arc<ValidatedTx>`:
the per-peer TX stream reader minted it with `ValidatedTx::from_network`
during its single strict decode of the inbound message
(`flood/inv_messages.rs`), reading fee/op metadata and the kind (from the
envelope's Soroban `ext`) off the decoded envelope. The handler just
forwards it to `submit_tx`.

## Querying

- `Mempool::top_by_fee(kind, budget)`: walks that kind's `by_fee` index
  in priority order and returns up to `budget.max_count` transactions
  whose envelope bytes total at most `budget.max_bytes`. A transaction
  that would overflow the byte budget is skipped and the walk continues
  with smaller ones (mirroring Core's surge pricing). Used by
  `GetTopTxs` for nomination.
- `Mempool::contains(hash)`: O(1) dedup check.
- `Mempool::get(hash)`: returns `Option<&Arc<ValidatedTx>>`.

`GetTopTxs` (`integrated.rs`) carries a `TopTxsRequest` — one
`(max_count, max_bytes)` budget for the classic phase and one for the
Soroban phase. Core deliberately asks for **more than fits**, by count
and by bytes alike (`HerderImpl::mempoolPullBudget`): each budget is
`(what fits + one transaction) × MEMPOOL_PULL_MULTIPLIER` (currently 2;
`HerderImpl.h`). For counts, "what fits" is `maxTxSetSizeOps` (classic)
or `ledgerMaxTxCount` (Soroban) and one transaction is 1. For bytes,
"what fits" is the classic byte allowance or the Soroban ledger tx byte
limit, and one transaction is the maximum transaction size of that
phase. The two dimensions get the same slack on purpose: whichever one
binds first decides how much surplus Core actually receives, and a byte
budget of just "one ledger plus one transaction" would let a few large
invalid transactions at the top of the fee order leave a byte-bound
ledger short. The reply lists classic transactions first, then Soroban,
each highest fee first; it is sized by these budgets, and the IPC frame
limit (256 MiB) sits far above them as a corruption guard.

Why over-pull: the mempool does no stateful validation, so its top
entries can be unusable (stale sequence number, drained account, bad
signature). An exact-fit pull would then produce a partially empty
ledger even with plenty of valid demand behind it. Over-pulling is cheap
because Core builds the nominated set with **lazy validation**
(`LazyTxValidator`, `TxSetFrame.cpp`; this is the only way tx sets are built): surge pricing walks the
candidates in fee order and validates one only when it is about to be
included, plus at most one non-fitting candidate per fee lane (that
probe is what tells surge pricing there was excess demand, and it has to
be a valid transaction, otherwise an invalid one could raise everyone's
base fee). Candidates that surge pricing never reaches are neither
validated nor reported, so Core leaves them in the mempool for the next
ledger. Only candidates that were validated and failed are removed from
the mempool (except those with a future sequence number, which are
chained behind a pending transaction). The metric
`herder.txset.candidates-validated` counts lazy validations; per ledger
it should stay close to the number of included transactions.

The "+1" matters on its own: surge pricing raises the base fee only when
it sees a valid transaction that does not fit, so a pull with no slack
would hide excess demand and keep fees at the minimum under overload.

## Known gaps

- Mempool size (100,000) and age limit (300 s) are hardcoded — there
  are no config knobs for them.
- Age-based eviction runs only on externalization, not on a timer — on
  a stalled network, old TXs sit in the mempool until pushed out by
  capacity-based eviction.
- Per-account grouping does not exist: the former `by_account` index
  (and the source-account/sequence metadata that fed it) was removed
  along with `TxEntry`, so per-account queries are not possible.
- No fee-bumping / replacement protocol. A higher-fee TX from the same
  account does not replace an existing one — both sit in the mempool
  until eviction.
