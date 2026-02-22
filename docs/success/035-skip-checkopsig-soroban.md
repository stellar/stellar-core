# Experiment 035: Skip checkOperationSignatures for Soroban TXs

## Date
2026-02-22

## Hypothesis
In `processSignatures`, `checkOperationSignatures` creates a `LedgerSnapshot`
(heap allocation) and loads the operation's source account to verify per-op
authorization. For Soroban TXs with a single operation using the TX source
account, this is redundant because:

1. `commonValid` already called `checkAllTransactionSignatures` which verified
   the TX source's signature and marked it in the SignatureChecker.
2. The operation uses the same source account, so the same signers/signature
   would be checked again.
3. All matching signatures are already marked as "used", so
   `checkAllSignaturesUsed` still passes correctly.

Skipping `checkOperationSignatures` avoids ~1.2µs/TX of LedgerSnapshot
creation + account load.

## Change Summary
Added a guard in `processSignatures` that skips `checkOperationSignatures`
when `isSoroban() && mOperations.size() == 1 && !op.sourceAccount` (the
operation has no per-op source override, so it uses the TX source).

The guard explicitly checks the three conditions needed for correctness:
- `isSoroban()`: only applies to Soroban TXs
- Single operation: Soroban TXs always have exactly 1 operation
- No per-op source: operation uses TX source, already verified

## Results

### TPS
- Baseline (exp-034): 15,808 TPS [15,808-15,872]
- Post-change: 15,808 TPS [15,808-15,872]
- Delta: Within variance (0%)

### Tracy Analysis
| Zone | Exp-034 (ns/TX) | Exp-035 (ns/TX) | Delta |
|------|-----------------|-----------------|-------|
| processSignatures (inclusive) | 1,709 | 489 | -1,220 (-71%) |
| checkOperationSignatures | 1,228 | skipped | -1,228 (-100%) |

| Zone | Exp-034 (ms/ledger) | Exp-035 (ms/ledger) | Delta |
|------|---------------------|---------------------|-------|
| applyLedger | 1,186 | 1,178 | -8 (-0.7%) |

The per-TX improvement of ~1.2µs × 16K TXs = ~19ms/ledger is real in
Tracy but within benchmark TPS variance (~5-10%).

## Files Changed
- `src/transactions/TransactionFrame.cpp` — Added guard to skip
  `checkOperationSignatures` for Soroban TXs using TX source account

## Commit
