---
name: subsystem-summary-of-transactions
description: "read this skill for a token-efficient summary of the transactions subsystem"
---

# Transactions Subsystem — Technical Summary

## Overview

The transactions subsystem implements the core transaction processing pipeline in stellar-core: parsing transaction envelopes from XDR, validating them, applying them to the ledger, and producing results and metadata. It encompasses the transaction frame hierarchy, all operation types (classic and Soroban), signature verification, offer exchange logic, sponsorship utilities, parallel apply infrastructure, and event/meta generation.

---

## Key Files

- **TransactionFrameBase.h/.cpp** — Abstract base class for all transaction types.
- **TransactionFrame.h/.cpp** — Concrete implementation for regular (non-fee-bump) transactions. ~2500 lines; contains the main `apply`, `checkValid`, `commonValid`, `processFeeSeqNum`, `parallelApply`, and `applyOperations` logic.
- **FeeBumpTransactionFrame.h/.cpp** — Wraps a `TransactionFrame` (inner tx) for fee bump support. Delegates most operations to the inner tx.
- **OperationFrame.h/.cpp** — Abstract base for all operations. Factory method `makeHelper` creates concrete subclasses from XDR `Operation`. Contains `apply`, `checkValid`, `parallelApply` dispatch.
- **MutableTransactionResult.h/.cpp** — Mutable result objects (`MutableTransactionResult`, `FeeBumpMutableTransactionResult`) that track transaction outcomes and fee refunds via `RefundableFeeTracker`.
- **TransactionMeta.h/.cpp** — `TransactionMetaBuilder` and `OperationMetaBuilder` for building `TransactionMeta` XDR (ledger changes, events, return values).
- **EventManager.h/.cpp** — `DiagnosticEventManager`, `OpEventManager`, `TxEventManager` for emitting contract/diagnostic/fee events during tx processing.
- **SignatureChecker.h/.cpp** — Validates decorated signatures against signers with weight thresholds, caching verification results.
- **SignatureUtils.h/.cpp** — Low-level signature creation/verification helpers (Ed25519, hash-x, signed payloads).
- **TransactionUtils.h/.cpp** — ~400 lines of helpers: ledger entry loading, balance/liability math, key construction, asset utilities, Soroban contract data helpers.
- **TransactionBridge.h/.cpp** — `txbridge` namespace: helpers for accessing/mutating `TransactionEnvelope` fields (signatures, operations, sequence numbers). Test-only mutation functions.
- **TransactionSQL.h/.cpp** — `populateCheckpointFilesFromDB`: serializes transaction results from DB into checkpoint files.
- **OfferExchange.h/.cpp** — Core DEX exchange logic: `exchangeV10`, `convertWithOffersAndPools`, price rounding, limit orders, liquidity pool interaction.
- **SponsorshipUtils.h/.cpp** — Sponsorship establishment/removal/transfer for entries and signers with reserve accounting.
- **ParallelApplyStage.h/.cpp** — Data structures for parallel tx application: `TxEffects`, `TxBundle`, `Cluster`, `ApplyStage`.
- **ParallelApplyUtils.h/.cpp** — `GlobalParallelApplyLedgerState`, `ThreadParallelApplyLedgerState`, `TxParallelApplyLedgerState`, `LedgerAccessHelper` hierarchy for parallel Soroban tx application.
- **LumenEventReconciler.h/.cpp** — Handles pre-protocol-8 XLM mint/burn reconciliation events.

---

## Key Classes and Data Structures

### Transaction Frame Hierarchy

```
TransactionFrameBase (abstract)
├── TransactionFrame         (regular V0/V1 transactions)
└── FeeBumpTransactionFrame  (fee bump wrapping an inner TransactionFrame)
```

**`TransactionFrameBase`** — Pure virtual interface. Defines the contract for all transaction types:
- `apply()`, `checkValid()`, `parallelApply()`, `preParallelApply()` — core lifecycle methods
- `processFeeSeqNum()` — fee deduction and sequence number consumption
- `processPostApply()`, `processPostTxSetApply()` — post-application hooks (Soroban refunds)
- Accessors: `getFullHash()`, `getContentsHash()`, `getEnvelope()`, `getSeqNum()`, `getSourceID()`, `getFeeSourceID()`, `getFullFee()`, `getInclusionFee()`, `getNumOperations()`, `isSoroban()`, etc.
- `insertKeysForFeeProcessing()` / `insertKeysForTxApply()` — declare keys needed for prefetching
- `withInnerTx()` — visitor for fee bump inner tx access
- Type aliases: `TransactionFrameBasePtr = shared_ptr<TransactionFrameBase const>`, `MutableTxResultPtr = unique_ptr<MutableTransactionResultBase>`

**`TransactionFrame`** — The main transaction implementation.
- **Members**: `mEnvelope` (TransactionEnvelope), `mNetworkID` (Hash ref), `mContentsHash`/`mFullHash` (lazily computed), `mOperations` (vector of `shared_ptr<OperationFrame const>`, built in constructor via `OperationFrame::makeHelper`), `mCachedAccountPreProtocol8`
- **ValidationType enum**: `kInvalid`, `kInvalidUpdateSeqNum`, `kInvalidPostAuth`, `kMaybeValid` — used by `commonValid` to decide how to handle failures (e.g., whether to still update seq nums or remove one-time signers)
- **Key methods** (see Flows section below)

**`FeeBumpTransactionFrame`** — Holds an outer `mEnvelope` plus `mInnerTx` (`TransactionFramePtr`). Delegates most operations to the inner tx. Has its own `commonValid`/`commonValidPreSeqNum` for validating the fee bump wrapper's signatures and fee source. `ValidationType`: `kInvalid`, `kInvalidPostAuth`, `kFullyValid`.

### Operation Frame Hierarchy

```
OperationFrame (abstract base)
├── CreateAccountOpFrame
├── PaymentOpFrame
├── PathPaymentOpFrameBase (abstract)
│   ├── PathPaymentStrictReceiveOpFrame
│   └── PathPaymentStrictSendOpFrame
├── ManageOfferOpFrameBase (abstract)
│   ├── ManageSellOfferOpFrame
│   │   └── CreatePassiveSellOfferOpFrame (via ManageSellOfferOpHolder)
│   └── ManageBuyOfferOpFrame
├── SetOptionsOpFrame
├── ChangeTrustOpFrame
├── TrustFlagsOpFrameBase (abstract)
│   ├── AllowTrustOpFrame
│   └── SetTrustLineFlagsOpFrame
├── MergeOpFrame
├── InflationOpFrame
├── ManageDataOpFrame
├── BumpSequenceOpFrame
├── CreateClaimableBalanceOpFrame
├── ClaimClaimableBalanceOpFrame
├── BeginSponsoringFutureReservesOpFrame
├── EndSponsoringFutureReservesOpFrame
├── RevokeSponsorshipOpFrame
├── ClawbackOpFrame
├── ClawbackClaimableBalanceOpFrame
├── LiquidityPoolDepositOpFrame
├── LiquidityPoolWithdrawOpFrame
├── InvokeHostFunctionOpFrame       (Soroban)
├── ExtendFootprintTTLOpFrame       (Soroban)
└── RestoreFootprintOpFrame         (Soroban)
```

**`OperationFrame`** — Each operation holds a `const Operation&` reference and a `const TransactionFrame&` parent reference.
- **Virtual methods**: `doCheckValid(ledgerVersion, res)`, `doApply(app, ltx, res, opMeta)` — must be overridden by every concrete op.
- **Soroban overrides**: `doCheckValidForSoroban(...)`, `doApplyForSoroban(...)`, `doParallelApply(...)` — overridden by Soroban ops (`InvokeHostFunctionOpFrame`, `ExtendFootprintTTLOpFrame`, `RestoreFootprintOpFrame`).
- `getThresholdLevel()` — returns `LOW`, `MEDIUM`, or `HIGH`; most ops default to `MEDIUM`, but `MergeOpFrame`, `SetOptionsOpFrame`, `InflationOpFrame`, `BumpSequenceOpFrame`, `ClaimClaimableBalanceOpFrame`, `ExtendFootprintTTLOpFrame`, `RestoreFootprintOpFrame` override.
- `isOpSupported(header)` — gates ops by protocol version.
- `isDexOperation()` — true for offer ops and path payments.
- `isSoroban()` — true for Soroban ops.
- `insertLedgerKeysToPrefetch(keys)` — allows ops to declare keys for bulk loading.

**`ManageOfferOpFrameBase`** — Shared base for sell/buy offer management. Contains the complete offer matching logic: validates offers, computes exchange parameters, calls `convertWithOffersAndPools`, manages offer creation/modification/deletion in the DEX. Uses sheep/wheat terminology.

**`PathPaymentOpFrameBase`** — Shared base for path payments. Provides `convert()` (calls `convertWithOffersAndPools` for each path hop), `updateSourceBalance`, `updateDestBalance`, `checkIssuer`.

**`TrustFlagsOpFrameBase`** — Shared base for `AllowTrustOpFrame` and `SetTrustLineFlagsOpFrame`. Contains common `doApply` logic for flag validation, authorization changes, and offer removal on deauthorization.

### Result Types

**`MutableTransactionResultBase`** — Abstract base for mutable results during tx processing.
- Holds `mTxResult` (TransactionResult XDR), optional `mRefundableFeeTracker`
- Methods: `setError()`, `setInsufficientFeeErrorWithFeeCharged()`, `getResultCode()`, `isSuccess()`, `getOpResultAt(index)`, `finalizeFeeRefund()`
- Subclasses: `MutableTransactionResult` (regular tx), `FeeBumpMutableTransactionResult` (fee bump, wraps inner result)

**`RefundableFeeTracker`** — Tracks consumed Soroban refundable resources (contract events size, rent fees) to compute fee refunds. `consumeRefundableSorobanResources()` returns false if the tx exceeds its refundable fee budget. `getFeeRefund()` returns unused portion.

### Meta and Events

**`TransactionMetaBuilder`** — Builds `TransactionMeta` XDR for a transaction. Creates `OperationMetaBuilder` instances for each operation. Methods: `pushTxChangesBefore()`, `pushTxChangesAfter()`, `setNonRefundableResourceFee()`, `finalize(success)`.

**`OperationMetaBuilder`** — Per-operation meta builder. `setLedgerChanges()` captures LedgerEntryChanges from operation's LedgerTxn. `setSorobanReturnValue()`, `getEventManager()`, `getDiagnosticEventManager()`.

**`DiagnosticEventManager`** — Buffers `DiagnosticEvent` entries. Created as enabled/disabled depending on context (apply vs. validation, meta enabled or not). `pushEvent()`, `pushError()`.

**`OpEventManager`** — Per-operation contract event buffer. Provides high-level event constructors: `newTransferEvent()`, `newMintEvent()`, `newBurnEvent()`, `newClawbackEvent()`, `newSetAuthorizedEvent()`, `eventsForClaimAtoms()`, `eventForTransferWithIssuerCheck()`.

**`TxEventManager`** — Transaction-level event buffer (fee events). `newFeeEvent()`.

### Signature Verification

**`SignatureChecker`** — Constructed with `(protocolVersion, contentsHash, signatures)`. `checkSignature(signers, neededWeight)` iterates decorated signatures, verifies each against provided signers, accumulates weight. Tracks which signatures have been used; `checkAllSignaturesUsed()` enforces no extra signatures. Maintains static counters for cache hit metrics.

### Offer Exchange

**`ExchangeResultV10`** — Result of a single offer crossing: `numWheatReceived`, `numSheepSend`, `wheatStays`.

**`exchangeV10(price, maxWheatSend, maxWheatReceive, maxSheepSend, maxSheepReceive, round)`** — Core exchange function. Computes amounts, applies rounding rules (NORMAL, PATH_PAYMENT_STRICT_SEND, PATH_PAYMENT_STRICT_RECEIVE), enforces 1% price error threshold.

**`convertWithOffersAndPools(...)`** — Buys wheat with sheep by crossing offers from the order book and/or using liquidity pools. Returns `ConvertResult` (eOK, ePartial, eFilterStopBadPrice, etc.). Takes a filter callback for price bounds and self-crossing prevention.

### Parallel Apply Infrastructure

**`TxEffects`** — Container holding `TransactionMetaBuilder` and `LedgerTxnDelta` for a single transaction during parallel apply.

**`TxBundle`** — Groups a transaction pointer, its result payload reference, tx number, and `TxEffects`.

**`Cluster`** — `vector<TxBundle>` — a group of transactions that must be applied sequentially (they share footprint overlap).

**`ApplyStage`** — `vector<Cluster>` with iteration support. Contains non-overlapping clusters that can be applied in parallel.

**Parallel Ledger State Hierarchy** (scoped entry ownership for safety):
- **`GlobalParallelApplyLedgerState`** — Owns the global entry map, hot archive snapshot, live snapshot, in-memory Soroban state, and restored entries. Splits state into per-thread maps before parallel execution, merges back after.
- **`ThreadParallelApplyLedgerState`** — Per-thread state copied from global. Owns `mThreadEntryMap`, `mThreadRestoredEntries`, RO TTL bumps buffer. Commits changes from successful txs.
- **`TxParallelApplyLedgerState`** — Per-transaction state within a thread. Owns `mTxEntryMap` (modified entries) and `mTxRestoredEntries`. Provides `takeSuccess()`/`takeFailure()` to produce `ParallelTxReturnVal`.

**`LedgerAccessHelper`** — Abstract interface (`getLedgerEntryOpt`, `upsertLedgerEntry`, `eraseLedgerEntryIfExists`) with two implementations:
- `PreV23LedgerAccessHelper` — wraps `AbstractLedgerTxn` for sequential apply
- `ParallelLedgerAccessHelper` — wraps `TxParallelApplyLedgerState` for parallel apply

**`ParallelTxReturnVal`** — Returned by each parallel tx: contains success flag, `TxModifiedEntryMap`, and `RestoredEntries`.

---

## Key Data Flows

### Transaction Lifecycle: Submission to Application

1. **Deserialization**: `TransactionFrameBase::makeTransactionFromWire(networkID, envelope)` constructs either a `TransactionFrame` or `FeeBumpTransactionFrame` based on envelope type. `TransactionFrame` constructor invokes `OperationFrame::makeHelper` for each operation.

2. **Validation (`checkValid`)**: Called during flood/herder acceptance.
   - `TransactionFrame::checkValid()` → checks XDR depth, validates fee XDR, creates `MutableTransactionResult`, calls `checkValidWithOptionallyChargedFee()`.
   - `checkValidWithOptionallyChargedFee()` → constructs `SignatureChecker`, computes Soroban resource fee if applicable, calls `commonValid()`.
   - `commonValid()` → calls `commonValidPreSeqNum()` (protocol version checks, time bounds, fee sufficiency, Soroban resource validation, footprint dedup), then validates sequence number (`isBadSeq`), checks account balance, verifies signatures via `checkAllTransactionSignatures()`.
   - For each operation: `op->checkValid()` → `doCheckValid()` or `doCheckValidForSoroban()`.
   - Final check: `signatureChecker.checkAllSignaturesUsed()`.
   - For fee bumps: `FeeBumpTransactionFrame::checkValid()` validates outer envelope, then calls inner tx's `checkValidWithOptionallyChargedFee(chargeFee=false)`.

3. **Fee Processing (`processFeeSeqNum`)**: Called at ledger close before applying.
   - Loads source account, computes fee (capped by balance), deducts from account, adds to fee pool.
   - Pre-v10: also updates sequence number here.
   - Returns `MutableTransactionResult::createSuccess(tx, feeCharged)`.

4. **Application (`apply`)**: Called for each tx in the tx set.
   - `TransactionFrame::apply(chargeFee, app, ltx, meta, txResult, sorobanConfig, prngSeed)`:
     - Calls `commonPreApply()`: builds `SignatureChecker`, calls `commonValid(applying=true)`, processes sequence number (`processSeqNum`), processes signatures (`processSignatures` — removes one-time signers, validates op signatures). Returns the checker on success, nullptr on failure.
     - Calls `applyOperations()`: iterates operations, for each op calls `op->apply()` which does `checkValid(forApply=true)` then `doApply()` or `doApplyForSoroban()`. Commits or rolls back per-op LedgerTxn based on success.
     - Returns success/failure.
   - Post-apply: `processPostApply()` handles pre-v23 Soroban refunds. `processPostTxSetApply()` handles v23+ refunds (after all txs applied).

5. **Parallel Apply (Soroban txs only, v23+)**:
   - `preParallelApply()` — runs in sequential phase: validates signatures, processes seq num, builds signature checker. Called per-tx before parallel execution begins.
   - `parallelApply()` — runs in parallel threads: asserts single-op Soroban tx, calls `op->parallelApply()` which dispatches to `doParallelApply()`. Uses `TxParallelApplyLedgerState` for ledger access. On success, `ThreadParallelApplyLedgerState::setEffectsDeltaFromSuccessfulTx()` records changes. Returns `ParallelTxReturnVal`.

### Parallel Apply Architecture

Soroban transactions are organized into stages of non-overlapping clusters:
1. `GlobalParallelApplyLedgerState` is constructed, collecting modified classic entries and setting up snapshots.
2. For each `ApplyStage`: clusters are distributed across threads.
3. Each thread gets a `ThreadParallelApplyLedgerState` (split from global state for the cluster's footprint).
4. Within a thread, txs in a cluster are applied sequentially. Each tx gets a `TxParallelApplyLedgerState`.
5. Successful tx changes are committed from tx state → thread state.
6. After all threads complete, thread states are merged back → global state via `commitChangesFromThreads()`.
7. After all stages, `commitChangesToLedgerTxn()` writes final state to the main LedgerTxn.

### Offer Exchange Flow

For DEX operations (ManageSell/BuyOffer, PathPayment):
1. `ManageOfferOpFrameBase::doApply()` validates the offer, computes exchange parameters.
2. Calls `convertWithOffersAndPools()` which iterates matching offers in the order book.
3. For each crossed offer: `crossOfferV10()` → `exchangeV10()` computes exact amounts.
4. Liquidity pools are checked for each asset pair (if available) and can be crossed atomically.
5. Results accumulated in `offerTrail` (vector of `ClaimAtom`).
6. Residual offer amount (if any) is written into the order book or deleted.
7. Path payments chain multiple conversions through intermediate assets.

---

## Key Utility Modules

### TransactionUtils

Provides an extensive set of helpers used throughout the subsystem:
- **Key constructors**: `accountKey()`, `trustlineKey()`, `offerKey()`, `dataKey()`, `claimableBalanceKey()`, `liquidityPoolKey()`, `contractDataKey()`, `contractCodeKey()`
- **Entry loaders**: `loadAccount()`, `loadTrustLine()`, `loadOffer()`, `loadClaimableBalance()`, `loadLiquidityPool()`, `loadData()`, `loadContractData()`, `loadContractCode()`
- **Balance/liability math**: `addBalance()`, `getAvailableBalance()`, `getMaxAmountReceive()`, `addBuyingLiabilities()`, `addSellingLiabilities()`, `getMinBalance()`
- **Authorization checks**: `isAuthorized()`, `isAuthRequired()`, `isClawbackEnabledOnTrustline()`, `isClawbackEnabledOnAccount()`
- **Fee computation**: `getMinInclusionFee()`, `TransactionFrame::computeSorobanResourceFee()`
- **Soroban helpers**: `validateContractLedgerEntry()`, `getAssetContractInfo()`, `makeSymbolSCVal()`, `makeAddressSCVal()`, `toCxxBuf()`
- **Constants**: `FIRST_PROTOCOL_SUPPORTING_OPERATION_LIMITS` (v11), `EXPECTED_CLOSE_TIME_MULT` (2), `getAccountSubEntryLimit()`, `getMaxOffersToCross()`

### SponsorshipUtils

Manages entry and signer sponsorship:
- `canEstablishEntrySponsorship()` / `establishEntrySponsorship()` — check reserve constraints and set sponsor
- `canRemoveEntrySponsorship()` / `removeEntrySponsorship()` — undo sponsoring
- `canTransferEntrySponsorship()` / `transferEntrySponsorship()` — change sponsor
- Parallel signer-sponsorship functions
- `createEntryWithPossibleSponsorship()` / `removeEntryWithPossibleSponsorship()` — convenient wrappers used by operations

### TransactionBridge (txbridge namespace)

Utility namespace for accessing TransactionEnvelope internals:
- `getSignatures()` / `getSignaturesInner()` — access signature vectors
- `getOperations()` — access operation vector
- `convertForV13()` — convert V0 envelopes to V1
- Test-only: `setSeqNum()`, `setFullFee()`, `setSorobanFees()`, `setMemo()`, `setMinTime()`, `setMaxTime()`

---

## Threading Model

- **Sequential apply** (classic transactions and pre-v23 Soroban): All transactions applied on the main thread using `AbstractLedgerTxn` for atomic state management. Each operation runs in a nested LedgerTxn that can be committed or rolled back.

- **Parallel apply** (Soroban transactions, v23+): Transactions are grouped into `ApplyStage`s containing `Cluster`s. Non-overlapping clusters run on separate threads. Within each cluster, transactions are applied sequentially. The `LedgerEntryScope` template system enforces ownership discipline across global/thread/tx scopes, preventing accidental cross-scope reads via compile-time scope tagging (`GlobalParApply`, `ThreadParApply`, `TxParApply`).

- **Signature verification**: `SignatureChecker` uses `PubKeyUtils::VerifySigCacheLookupResult` for caching. Static mutex-protected counters track cache metrics across threads. Background signature validation (for flooding) uses `disableCacheMetricsTracking()`.

---

## Protocol Version Sensitive Logic

The transactions subsystem has many protocol-version-gated code paths:
- **V8**: Cached account for pre-protocol-8 bug compatibility
- **V10**: Sequence number processing moved to apply time; signature processing changed
- **V13**: Envelope type V0 deprecated; one-time signer removal on invalid txs
- **V19**: `PreconditionsV2` support (minSeqNum, minSeqAge, minSeqLedgerGap, extraSigners)
- **V20 (SOROBAN_PROTOCOL_VERSION)**: Soroban transaction support (InvokeHostFunction, ExtendFootprintTTL, RestoreFootprint)
- **V21**: Classic tx extension field must be v=0
- **V23**: Fee bump inner tx fee relaxation; parallel apply; Soroban refunds moved to post-tx-set stage; `OperationMetaV2`
- **V25**: Soroban transactions disallowed from using memo or muxed source accounts

---

## Ownership Summary

- `TransactionFrame` **owns** its `mEnvelope` and `mOperations` vector (shared_ptr to const OperationFrame).
- `FeeBumpTransactionFrame` **owns** its outer `mEnvelope` and a `TransactionFramePtr` to the inner tx.
- `OperationFrame` holds **const references** to its `Operation` and parent `TransactionFrame`.
- `MutableTransactionResultBase` **owns** the `TransactionResult` XDR and optional `RefundableFeeTracker`.
- `TransactionMetaBuilder` **owns** the `TransactionMeta` XDR, `OperationMetaBuilder` vector, and event managers.
- `GlobalParallelApplyLedgerState` **owns** the global entry map and restored entries; **references** snapshots and config.
- `ThreadParallelApplyLedgerState` **owns** per-thread entry map, restored entries, RO TTL bumps; **references** global config/snapshots.
- `TxParallelApplyLedgerState` **owns** per-tx entry map and restored entries; **references** parent thread state.
- `TxBundle` **owns** `TxEffects` (via unique_ptr); holds shared_ptr to tx and reference to result payload.
