# Mempool

The overlay maintains an **in-process, fee-ordered mempool** of pending
transactions. Core queries it for nomination (`GetTopTxs`), and the
overlay services TX-flood requests from it (`tx_buffer` is *not* the
mempool — see [tx-propagation.md](tx-propagation.md)).

Source: `flood/mempool.rs`. Owned by `Overlay` in `integrated.rs`,
guarded by `RwLock`.

## Configuration

`integrated.rs:100`:

```rust
Mempool::new(100_000, Duration::from_secs(300))
```

| Parameter   | Value      | Notes                                       |
|-------------|------------|---------------------------------------------|
| `max_size`  | 100,000    | Hardcoded. `config.max_mempool_size` is **not** read here. |
| `max_age`   | 300 s      | Hardcoded.                                  |

## Indexes

`flood/mempool.rs:91-105`. Three concurrent indexes over the same
`TxEntry` set:

| Index         | Type                                  | Purpose                                  |
|---------------|---------------------------------------|------------------------------------------|
| `by_hash`     | `HashMap<TxHash, TxEntry>`            | O(1) lookup, dedup                       |
| `by_fee`      | `BTreeSet<FeePriority>`               | Ordered access (for `top_by_fee`)        |
| `by_account`  | `HashMap<AccountId, Vec<TxHash>>`     | Per-account grouping                     |

`TxEntry` (`flood/mempool.rs:21-39`) carries: data bytes, hash,
source_account, sequence, fee, num_ops, received_at, from_peer.

## Fee ordering

`FeePriority::cmp` (`flood/mempool.rs:60-82`) orders by **fee-per-op**
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

`Mempool::insert` (`flood/mempool.rs:123-153`):

1. **Dedup**: if `by_hash` already contains this hash, return `false`.
2. **Capacity check**: while at `max_size`, call `evict_lowest_fee`.
3. Add to all three indexes.
4. Return `true`.

## Eviction

- **Capacity-based** (`evict_lowest_fee`, `flood/mempool.rs:233-238`):
  takes the last entry of `by_fee` (lowest priority) and removes it via
  `remove`. Called from `insert` on capacity overflow — happens
  *synchronously per insert*.
- **Age-based** (`evict_expired`, `flood/mempool.rs:208-220`): scans for
  entries with `now - received_at > max_age` and removes them. **Not
  currently called automatically** — there is no periodic timer that
  invokes it.

## Removal on externalization

When Core sends `TxSetExternalized`, `main.rs:1192-1200` calls
`overlay_handle.remove_txs_sync(tx_hashes)` and **awaits** completion.
The synchronous wait is intentional: the next nomination cycle must not
see TXs that were just included in the closing ledger.

`integrated.rs:248` (`remove_txs_sync`) iterates `tx_hashes` and calls
`mempool.remove(&hash)`, which removes from all three indexes.

## Insertion sources

Two paths into the mempool:

### `SubmitTx` from Core (`integrated.rs:124-152`)

Core gives us `(data, fee, num_ops)` directly. Mempool gets the correct
fee and we use the right priority for ordering.

### TX received over the network (`main.rs:745-755`)

```rust
LibP2pOverlayEvent::TxReceived { tx, from } => {
    // TODO: Parse XDR to extract fee and ops instead of hardcoding fee=0, ops=1
    self.overlay_handle.submit_tx(tx, 0, 1);
}
```

This is a **known issue**: TXs that arrive from the network (i.e.
everything not locally submitted) are inserted with **fee = 0,
num_ops = 1**. Consequences:

- Fee-based ordering in `by_fee` is wrong for these entries.
- They become the first eviction targets when the mempool fills up,
  ahead of locally-submitted TXs at the same true fee level.
- `inv_messages` already encodes `fee_per_op` in INV entries, but
  because the receiver records the announced TX in the mempool with
  `fee=0`, that signal is lost downstream.

The fix is to parse `TransactionEnvelope` XDR to extract fee, op count,
source account, and sequence — there's a TODO in the code at the same
spot.

## Account grouping

`by_account` exists but is **also broken for network TXs** because
`source_account` is hardcoded to `[0u8; 32]` for network-received TXs
(`integrated.rs:144`, same TODO). All such TXs land in the same
"zero-account" bucket, defeating per-account queries
(`top_by_account`).

Locally-submitted TXs also have this problem until the XDR-parsing
TODO is resolved — see `integrated.rs:144-145`.

## Querying

- `Mempool::top_by_fee(count)`: returns up to `count` hashes from
  `by_fee` in priority order. Used by `GetTopTxs` for nomination.
- `Mempool::contains(hash)`: O(1) dedup check.
- `Mempool::get(hash)`: returns `&TxEntry`.

`GetTopTxs` (`integrated.rs:154-162`) returns `(hash, data)` pairs to
Core for inclusion in a TX set.

## Known gaps

- `config.max_mempool_size` and `config.tx_push_peer_count` are defined
  but **not** read in the runtime — the mempool size and TX flood
  fanout are hardcoded.
- `evict_expired` is implemented but **not scheduled** — old TXs sit
  in the mempool until pushed out by capacity-based eviction.
- Network TXs land with **fee=0, account=0, sequence=0** (XDR parsing
  not implemented).
- No fee-bumping / replacement protocol. A higher-fee TX from the same
  account does not replace an existing one — both sit in the mempool
  until eviction.
