# Mempool

The overlay maintains an **in-process, fee-ordered mempool** of pending
transactions. Core queries it for nomination (`GetTopTxs`), and the
overlay services TX-flood requests from it (`tx_buffer` is *not* the
mempool — see [tx-propagation.md](tx-propagation.md)).

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
| `by_fee`      | `BTreeSet<FeePriority>`               | Ordered access (for `top_by_fee`)        |

`MempoolEntry` (`flood/mempool.rs:20-24`) is an
`Arc<ValidatedTx>` plus `received_at` (for age-based eviction). The
`ValidatedTx` (`wire.rs`) carries the canonical envelope bytes, sha256
hash, fee, and op count — computed once at the trust boundary where the
transaction entered the process, and shared by reference through the
rest of the pipeline.

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

`Mempool::insert` (`flood/mempool.rs:106-127`):

1. **Dedup**: if `by_hash` already contains this hash, return `false`.
2. **Capacity check**: while at `max_size`, call `evict_lowest_fee`.
3. Add to both indexes.
4. Return `true`.

## Eviction

- **Capacity-based** (`evict_lowest_fee`, `flood/mempool.rs:179-184`):
  takes the last entry of `by_fee` (lowest priority) and removes it via
  `remove`. Called from `insert` on capacity overflow — happens
  *synchronously per insert*.
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
same `Arc<ValidatedTx>` currency, so every entry carries its real fee
and op count and fee ordering is correct regardless of origin.

### `SubmitTx` from Core (`main.rs:1095`)

Core submits `(data, fee, num_ops)` over IPC; the handler mints the
entry with `ValidatedTx::from_core_trusted`, which trusts Core's
metadata and does not re-decode (it only rejects fee-bumps via the
envelope discriminant and hashes the bytes).

### TX received over the network (`main.rs:727-734`)

`LibP2pOverlayEvent::TxReceived` already carries an `Arc<ValidatedTx>`:
the per-peer TX stream reader minted it with `ValidatedTx::from_network`
during its single strict decode of the inbound message
(`flood/inv_messages.rs`), reading fee/op metadata off the decoded
envelope. The handler just forwards it to `submit_tx`.

## Querying

- `Mempool::top_by_fee(count)`: returns up to `count` hashes from
  `by_fee` in priority order. Used by `GetTopTxs` for nomination.
- `Mempool::contains(hash)`: O(1) dedup check.
- `Mempool::get(hash)`: returns `Option<&Arc<ValidatedTx>>`.

`GetTopTxs` (`integrated.rs:79-87`) returns `(hash, data)` pairs to
Core for inclusion in a TX set.

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
