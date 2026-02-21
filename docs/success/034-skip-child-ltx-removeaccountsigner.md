# Experiment 034: Skip Child LTX in removeAccountSigner via Peek

## Date
2026-02-21

## Hypothesis
`removeAccountSigner` creates a child `LedgerTxn` for every TX to provide
rollback semantics when removing pre-auth transaction signers. However, in
the common case (99.99%+), no matching pre-auth signer exists — the child
LTX is constructed and immediately destroyed without committing. By first
peeking at the account's signers via `getNewestVersion` (an O(1) map lookup),
we can skip the expensive child LTX construction/destruction entirely when
no matching signer is found.

## Change Summary
Restructured `removeAccountSigner` to:
1. Use `ltxOuter.getNewestVersion(accountKey(accountID))` to peek at the
   account's signers (cheap const lookup, no LTX allocation)
2. Search the signers list for the pre-auth key
3. Only create the child LTX if a matching signer is actually found (rare path)

This preserves the original semantics exactly — the child LTX is still
created when a signer needs to be removed — but avoids ~400ns of child
LTX construction/destruction overhead per TX in the common case.

## Results

### TPS
- Baseline (exp-032): 15,168 TPS [15,168-15,232]
- Post-change: 15,808 TPS [15,808-15,872]
- Delta: +640 TPS (+4.2%)

### Tracy Analysis
| Zone | Exp-032 (ns/TX) | Exp-034 (ns/TX) | Delta |
|------|-----------------|-----------------|-------|
| removeAccountSigner | 682 | 109 | -573 (-84%) |
| processSignatures | 2,383 | 1,709 | -674 (-28%) |
| checkOperationSignatures | 1,230 | 1,228 | ~same |

| Zone | Exp-032 (ms/ledger) | Exp-034 (ms/ledger) | Delta |
|------|---------------------|---------------------|-------|
| applyLedger | 1,215 | 1,186 | -29 (-2.4%) |

## Files Changed
- `src/transactions/TransactionFrame.cpp` — Restructured `removeAccountSigner`
  to peek at signers via `getNewestVersion` before creating child LTX

## Commit
