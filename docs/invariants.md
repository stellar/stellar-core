# Invariants in stellar-core

> ⚠️ **Warning:** Invariants are very expensive and may reduce performance. If a strict invariant is violated, it will crash your node.

Invariants are validation rules that ensure data correctness throughout stellar-core's operation. They are designed to detect inconsistencies or invalid states in the ledger and related data structures. When an invariant is violated, an `InvariantDoesNotHold` exception is thrown, halting the operation to prevent corrupt data from propagating.

## Invariant System Overview

### Architecture

The invariant system consists of:

- **`Invariant`** (base class): Defines the interface for all invariants with several check methods that can be overridden
- **`InvariantManager`**: Maintains a registry of available invariants and supports enabling them dynamically at configuration time
- **`InvariantDoesNotHold`**: Exception thrown when an invariant is violated

### Check Methods

Invariants can override the following check methods depending on when they should be validated:

| Method | When Called |
|--------|-------------|
| `checkOnOperationApply` | After each operation is applied |
| `checkOnBucketApply` | After bucket apply during catchup |
| `checkAfterAssumeState` | After assuming state after a restart |
| `checkOnLedgerCommit` | When a ledger is committed |
| `checkSnapshot` | Periodically against ledger state snapshots |

### Strict vs. Non-Strict Invariants

Invariants can be marked as "strict" or non-strict. Strict invariants are critical validations where failures should halt processing. While non-strict invariants are more lenient, they would only log an error and continue processing.

## Available Invariants

### AccountSubEntriesCountIsValid

**File:** `AccountSubEntriesCountIsValid.cpp`

**Purpose:** Validates that the `numSubEntries` field of an account is in sync with the number of subentries in the database.

**Details:**
- Tracks changes to sub-entries (trustlines, offers, data entries, signers)
- Verifies that the calculated sub-entry count matches the `numSubEntries` stored in the account
- Pool share trustlines count as 2 sub-entries instead of 1

**Check Method:** `checkOnOperationApply`

---

### ArchivedStateConsistency

**File:** `ArchivedStateConsistency.cpp`

**Purpose:** Ensures consistency between the Live BucketList and the Hot Archive BucketList for Soroban state management.

**Details:**
- Only applies to Soroban entry types (CONTRACT_CODE, CONTRACT_DATA) 
- Verifies that no entry exists in both the live state and the archived (Hot Archive) state simultaneously
- Checks eviction invariants: entries evicted from live state should not already exist in the hot archive
- Checks restoration invariants: entries restored from hot archive are not already present in live state

**Check Methods:** `checkOnLedgerCommit`, `checkSnapshot`

---

### BucketListIsConsistentWithDatabase

**File:** `BucketListIsConsistentWithDatabase.cpp`

**Purpose:** Validates OFFER entry types in the BucketList and Database are in a consistent state after bucket operations and node restarts.

**Details:**
- Checks that there is a one-to-one mapping of OFFER entry types between SQL DB and BucketList state.

**Check Methods:** `checkOnBucketApply`, `checkAfterAssumeState`

---

### ConservationOfLumens

**File:** `ConservationOfLumens.cpp`

**Purpose:** Validates that the total number of lumens (XLM) in the system remains constant except during inflation operations.

**Details:**
- Tracks lumen balances across accounts, claimable balances, and liquidity pools
- Also tracks lumens held in the Stellar Asset Contract (SAC) for wrapped XLM
- For non-inflation operations: `deltaBalances + deltaFeePool == 0`
- For inflation operations: validates that `totalCoins` and `feePool` match the sum of all balances
- Ensures no lumens are created or destroyed unexpectedly

**Check Method:** `checkOnOperationApply`

---

### ConstantProductInvariant

**File:** `ConstantProductInvariant.cpp`

**Purpose:** Ensures that liquidity pool operations maintain or increase the constant product formula (x * y).

**Details:**
- For AMM (Automated Market Maker) pools using constant product formula
- Validates: `currentReserveA * currentReserveB >= previousReserveA * previousReserveB`
- Excludes `LIQUIDITY_POOL_WITHDRAW`, `SET_TRUST_LINE_FLAGS`, and `ALLOW_TRUST` operations (which intentionally may decrease the product)
- Detects manipulation that would drain liquidity unfairly

**Check Method:** `checkOnOperationApply`

---

### EventsAreConsistentWithEntryDiffs

**File:** `EventsAreConsistentWithEntryDiffs.cpp`

**Purpose:** Validates that Soroban contract events are consistent with the actual ledger entry changes.

**Details:**
- Compares balance changes recorded in events against actual ledger entry modifications
- Validates Stellar Asset Contract (SAC) events for transfers, mints, burns, and authorization changes
- Ensures that event amounts match entry diffs for:
  - Account balances (XLM)
  - Trustline balances (issued assets)
  - SAC balance contract data entries
- Checks authorization events match trustline authorization state changes

**Check Method:** `checkOnOperationApply`

---

### LedgerEntryIsValid

**File:** `LedgerEntryIsValid.cpp`

**Purpose:** Validates that ledger entries meet structural and semantic requirements.

**Details:**
- Performs bounds checking on various fields
- Validates that `lastModifiedLedgerSeq` equals the current ledger sequence
- Checks entry-specific validity rules:
  - **Accounts:** Non-negative balance, valid flags, valid signers (sorted, proper weights)
  - **Trustlines:** Valid limits, authorization flags, non-negative balances
  - **Offers:** Valid prices, non-negative amounts
  - **Data entries:** Valid name format
  - **Claimable balances:** Valid claimants, proper sponsorship
  - **Liquidity pools:** Non-negative reserves, valid pool parameters
  - **Contract data/code:** Structure validation
  - **TTL entries:** Valid time bounds

**Check Method:** `checkOnOperationApply`

---

### LiabilitiesMatchOffers

**File:** `LiabilitiesMatchOffers.cpp`

**Purpose:** Ensures that liabilities remain in sync with the offer book, and the balance of accounts & trustlines respect the liabilities & reserve.

**Details:**
- Validates that buying and selling liabilities are consistent with open offers
- Checks that account/trustline balances respect liability reservations
- Only validates entries where balance decreased or liabilities increased
- Ensures unauthorized trustlines don't have increased liabilities
- Prevents scenarios where offers could execute but insufficient balance exists

**Check Method:** `checkOnOperationApply`

---

### OrderBookIsNotCrossed

**File:** `OrderBookIsNotCrossed.cpp`

**Purpose:** Validates that the order book is not in a crossed state (where buy and sell orders could match but haven't).

**Details:**
- **Note:** This invariant is only available in test builds (`BUILD_TESTS`) and is only used in fuzzing
- Maintains internal order book state across operations
- Detects if lowest ask price ≤ highest bid price (crossed state)
- Handles passive offers correctly (passive offers at the same price don't cross)
- Used primarily for fuzzing and testing

**Check Method:** `checkOnOperationApply`

---

### SponsorshipCountIsValid

**File:** `SponsorshipCountIsValid.cpp`

**Purpose:** Validates that sponsorship accounting is correct across the ledger

**Details:**
- Checks that each account's `numSponsoring` and `numSponsored` counts are accurate
- Validates the global sponsorship invariant:
  ```
  totalNumSponsoring = totalNumSponsored + totalClaimableBalanceReserve
  ```
- Tracks sponsorship changes for all sponsorable entry types
- Accounts for signer sponsorship in account extensions
- Only applies from protocol version 14 onwards

**Check Method:** `checkOnOperationApply`

---

## Configuration

Invariants can be enabled in the stellar-core configuration file using the `INVARIANT_CHECKS` option. Strings are matched as regex against the list of invariants. Example:

```ini
INVARIANT_CHECKS = ["AccountSubEntriesCountIsValid", "ConservationOfLumens"]
```

To enable all invariants:

```ini
INVARIANT_CHECKS = [".*"]
```

### State Snapshot Invariants

Some expensive invariants (like `ArchivedStateConsistency.checkSnapshot`) run periodically on a background thread against ledger state snapshots, as they require scanning the entire BucketList and can't run in a blocking fashion during normal operation. This invariant consumes a lot of memory which is why its not allowed on validator nodes.

The frequency of these checks is controlled by `STATE_SNAPSHOT_INVARIANT_LEDGER_FREQUENCY` (default: 300 seconds).

## Metrics

The invariant system exposes metrics for monitoring:

- `ledger.invariant.failure`: Counter of invariant failures
- `ledger.invariant.state-snapshot-skipped`: Counter of skipped snapshot invariant checks

## Best Practices for Adding New Invariants

1. **Consider performance**: Expensive checks may use `checkSnapshot` with background execution
2. **Provide descriptive error messages**: Include relevant data to help diagnose the failure
3. **Mark strictness appropriately**: Use strict invariants for critical validations
4. **Add tests**: Include unit tests for both passing and failing cases
