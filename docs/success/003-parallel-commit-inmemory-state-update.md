# Experiment 003: Parallel Commit — addLiveBatch + InMemorySorobanState Update

## Date
2026-02-19

## Hypothesis
The ledger commit path in `finalizeLedgerTxnChanges` runs `addLiveBatch` and
`updateInMemorySorobanState` sequentially. These operate on independent data
structures (LiveBucketList vs InMemorySorobanState) and share only const
references to the entry vectors. Running them in parallel should reduce
commit wall time by overlapping the in-memory state update with addLiveBatch.

## Change Summary
- `LedgerManagerImpl::finalizeLedgerTxnChanges()`: After `getAllEntries` seals
  the LTX, launch `updateInMemorySorobanState` on an async worker thread while
  the main thread runs `addLiveBatch`. Both share const references to
  `initEntries`, `liveEntries`, `deadEntries`.
- Added `ApplyState::updateInMemorySorobanStateFromCommitWorker()` — variant
  that checks phase without the thread invariant assertion (needed because the
  worker thread is neither main nor APPLY type).
- Added `<future>` include for `std::async`.
- Added `ZoneScoped` to `InMemorySorobanState::updateState` for Tracy visibility.

## Results

### TPS
- Baseline: 9408 TPS
- Post-change: 9408 TPS
- Delta: 0% (within binary search granularity)

### Tracy Analysis (30s capture, 6 ledger commits)
| Zone | Avg/call | Notes |
|------|----------|-------|
| finalizeLedgerTxnChanges | 164ms | Down from ~220ms sequential |
| addLiveBatch | 119ms | Main thread (critical path) |
| updateState (InMemory) | 56ms | Async worker — fully overlapped |
| getAllEntries | 11ms | Seals LTX |
| waitForInMemoryUpdate | 1.8µs | Worker always finishes before addLiveBatch |
| addHotArchiveBatch | 1.5ms | Negligible (empty batch in benchmark) |

The parallelization saves ~56ms per ledger commit by fully overlapping
the in-memory state update with addLiveBatch.

## Files Changed
- `src/ledger/LedgerManagerImpl.cpp` — parallel commit in finalizeLedgerTxnChanges, new FromCommitWorker method
- `src/ledger/LedgerManagerImpl.h` — added updateInMemorySorobanStateFromCommitWorker declaration
- `src/ledger/InMemorySorobanState.cpp` — added Tracy zone to updateState

## Commit
