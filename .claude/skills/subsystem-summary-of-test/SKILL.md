---
name: subsystem-summary-of-test
description: "read this skill for a token-efficient summary of the test subsystem"
---

# Test Subsystem — Test Utilities and Infrastructure

The `src/test/` directory contains test infrastructure, helpers, and utilities used across the entire stellar-core test suite. It does NOT contain the actual test cases (those live alongside their subsystems). This document covers the test framework, helper classes, and conventions.

## Test Framework and Runner (`test.h` / `test.cpp`)

### Test Entry Point
- `runTest(CommandLineArgs const& args)` — Main test runner entry point. Configures Catch2 session, parses CLI args (log level, metrics, version selection, tx meta recording), seeds PRNGs, and runs all tests.
- Uses **Catch2** as the underlying test framework (wrapped via `Catch2.h`).

### Test Configuration
- `getTestConfig(int instanceNumber, Config::TestDbMode mode)` — Returns a lazily-created, cached `Config` for the given instance number. Configs are stored in `gTestCfg[mode]` arrays. Default mode is `TESTDB_BUCKET_DB_VOLATILE`. Key config settings:
  - `RUN_STANDALONE = true`, `FORCE_SCP = true`, `MANUAL_CLOSE = true`
  - `WORKER_THREADS = 3`, invariant checks enabled (except `EventsAreConsistentWithEntryDiffs`)
  - Test root directories created via `TmpDir` in `gTestRoots`
  - Node seed derived deterministically from instance number + command-line seed
  - Single-node quorum with `UNSAFE_QUORUM = true`
  - `NETWORK_PASSPHRASE = "(V) (;,,;) (V)"`
  - DB can be in-memory SQLite, file-backed SQLite, or PostgreSQL

### Version Testing Helpers
The framework supports running tests across multiple protocol versions:
- `for_all_versions(app, f)` — Run `f` for every protocol version (1 to current).
- `for_versions(from, to, app, f)` — Run `f` for versions in range `[from, to]`.
- `for_versions_from(from, app, f)` — Run `f` for versions `[from, current]`.
- `for_versions_to(to, app, f)` — Run `f` for versions `[1, to]`.
- `for_all_versions_except(versions, app, f)` — Run `f` for all versions except those listed.
- `TEST_CASE_VERSIONS(name, filters)` macro — Declares a test that iterates over all versions specified via `--version` or `--all-versions` CLI flags.
- `test_versions_wrapper(f)` — Internal: iterates `gVersionsToTest`, sets `gTestingVersion`, clears configs, and runs `f` inside a Catch2 `SECTION` for each version.
- Internally relies on `gMustUseTestVersionsWrapper` flag to enforce correct usage.

### PRNG and Determinism
- `ReseedPRNGListener` — Catch2 event listener that re-seeds all PRNGs at the start of every test case using `reinitializeAllGlobalStateWithSeed(sCommandLineSeed)`. The seed rotates every 24 hours by default.
- Node secret keys derived from `0xFFFF0000 + (instanceNumber ^ lastGlobalStateSeed)` to avoid inter-test collisions.

### Transaction Metadata Recording/Checking
- `recordOrCheckGlobalTestTxMetadata(TransactionMeta const& txMeta)` — Records or checks a SIPHash of normalized tx metadata against a persistent baseline.
- `TestTxMetaMode` enum: `META_TEST_IGNORE`, `META_TEST_RECORD`, `META_TEST_CHECK`.
- `TestContextListener` — Catch2 listener tracking current test case and sections for metadata context keying.
- Baselines stored as JSON files with base64-encoded hashes per test case/section. CLI flags: `--record-test-tx-meta DIR`, `--check-test-tx-meta DIR`, `--debug-test-tx-meta FILE`.
- `saveTestTxMeta()` / `loadTestTxMeta()` / `reportTestTxMeta()` manage persistence.

### Utility Functions
- `getSrcTestDataPath(rel)` / `getBuildTestDataPath(rel)` — Resolve paths under `testdata/` relative to source or build dir.
- `cleanupTmpDirs()` — Clears `gTestRoots` (must be called manually if `getTestConfig` used outside Catch2).
- `gBaseInstance` — Global offset for test instance numbering (for parallel test execution).
- `force_sqlite` — Set via `STELLAR_FORCE_SQLITE` env var.

## TestAccount (`TestAccount.h` / `TestAccount.cpp`)

A high-level wrapper around a Stellar account for test convenience. Encapsulates `Application&`, `SecretKey`, and `SequenceNumber`.

### Construction
- `TestAccount(app, secretKey, seqNum=0)` — Wraps an existing secret key.

### Account Operations (all apply transactions and assert success)
- `create(secretKey, initialBalance)` / `create(name, initialBalance)` — Create a sub-account, returns new `TestAccount`.
- `createBatch(secretKeys, initialBalance)` — Batch create multiple accounts.
- `merge(into)` — Merge this account into another.
- `pay(destination, amount)` / `pay(destination, asset, amount)` — Send payment.
- `pay(destination, sendCur, sendMax, destCur, destAmount, path)` — Path payment strict receive.
- `pathPaymentStrictSend(...)` — Path payment strict send.
- `changeTrust(asset, limit)` — Establish/modify trustline.
- `allowTrust(asset, trustor, ...)` / `denyTrust(...)` / `allowMaintainLiabilities(...)` — Manage trust authorization.
- `setTrustLineFlags(asset, trustor, args)` — Set trust line flags.
- `setOptions(args)` — Set account options.
- `manageData(name, value)` — Set/delete data entries.
- `bumpSequence(to)` — Bump sequence number.
- `manageOffer(...)` / `manageBuyOffer(...)` / `createPassiveOffer(...)` — DEX operations, return offer ID.
- `createClaimableBalance(...)` / `claimClaimableBalance(...)` — Claimable balance ops.
- `clawback(...)` / `clawbackClaimableBalance(...)` — Clawback operations.
- `liquidityPoolDeposit(...)` / `liquidityPoolWithdraw(...)` — AMM operations.
- `inflation()` — Run inflation.

### Query Methods
- `getBalance()` / `getAvailableBalance()` — Native balance queries.
- `getTrustlineBalance(asset)` / `getTrustlineFlags(asset)` — Trustline queries.
- `loadTrustLine(asset)` / `hasTrustLine(asset)` — Load/check trustlines.
- `getNumSubEntries()` — Sub-entry count.
- `exists()` — Check if account exists on ledger.
- `loadSequenceNumber()` / `getLastSequenceNumber()` / `nextSequenceNumber()` — Sequence number management. Auto-loads from ledger if `mSn == 0`.

### Transaction Building
- `tx(ops, seqNum)` — Build a `TransactionTestFramePtr` from operations, auto-incrementing sequence.
- `op(operation)` — Set source account on an operation to this account.
- `applyOpsBatch(ops)` — Apply operations in batches of `MAX_OPS_PER_TX`, closing ledgers.

### Implicit Conversions
- Converts to `SecretKey` and `PublicKey` implicitly.

## TxTests (`TxTests.h` / `TxTests.cpp`)

The `stellar::txtest` namespace contains the bulk of transaction test utilities: operation builders, transaction constructors, apply helpers, and ledger close wrappers.

### Transaction Application
- `applyCheck(tx, app, checkSeqNum)` — The core test-apply function. Closes a ledger, then in a `LedgerTxn`: validates tx (`checkValidForTesting`), processes fees, applies tx, verifies results match between original and cloned tx, checks sequence number changes, verifies no unexpected ledger mutations on failure, commits, and records tx metadata. Returns success bool.
- `applyTx(tx, app, checkSeqNum)` — Applies a tx (via `applyCheck` for in-memory mode, or `closeLedger` for BucketListDB). Calls `throwIf` on failure, checks fee charged.
- `validateTxResults(tx, app, validationResult, applyResult)` — Validates that `checkValid` and `apply` produce expected results.

### Ledger Close Helpers
- `closeLedger(app, txs, strictOrder, upgrades)` — Close the next ledger with the given transactions and upgrades.
- `closeLedgerOn(app, day, month, year, txs)` — Close ledger with a specific date.
- `closeLedgerOn(app, ledgerSeq, closeTime, txs, strictOrder, upgrades, parallelSorobanOrder)` — Full-control ledger close. Builds a `TxSetXDRFrame`, externalizes via Herder, cranks until the ledger closes.
- `closeLedger(app, txSet)` — Close with a pre-built tx set.
- When `strictOrder = true`, transactions are applied in exact order (allows intentionally invalid txs). Otherwise, `checkValid` is asserted.

### Transaction Constructors
- `transactionFromOperations(app, from, seq, ops, fee, memo)` — Creates V0 or V1 envelope based on protocol version.
- `transactionFromOperationsV0(...)` / `transactionFromOperationsV1(...)` — Explicit version constructors.
- `paddedTransactionFromOperations(...)` / `paddedTransactionFromOperationsV1(...)` — Create transactions padded to a desired byte size (V23+).
- `transactionWithV2Precondition(app, account, seqDelta, fee, cond)` — Transaction with V2 preconditions.
- `feeBump(app, feeSource, tx, inclusion, useInclusionAsFullFee)` — Create a fee bump transaction.
- `transactionFrameFromOps(networkID, source, ops, opKeys, cond)` — Direct envelope construction with explicit signers.
- `sorobanTransactionFrameFromOps(...)` / `sorobanTransactionFrameFromOpsWithTotalFee(...)` — Soroban transaction construction with resources, fees.

### Operation Builders (all return `Operation`)
- **Account**: `createAccount(dest, amount)`, `accountMerge(dest)`
- **Payments**: `payment(to, amount)`, `payment(to, asset, amount)`, `pathPayment(...)`, `pathPaymentStrictSend(...)`
- **Trust**: `changeTrust(asset, limit)`, `allowTrust(trustor, asset, authorize)`, `setTrustLineFlags(trustor, asset, args)`
- **DEX**: `manageOffer(...)`, `manageBuyOffer(...)`, `createPassiveOffer(...)`
- **Data**: `manageData(name, value)`, `bumpSequence(to)`
- **Claimable Balance**: `createClaimableBalance(...)`, `claimClaimableBalance(...)`
- **Sponsorship**: `beginSponsoringFutureReserves(...)`, `endSponsoringFutureReserves()`, `revokeSponsorship(...)`
- **Clawback**: `clawback(from, asset, amount)`, `clawbackClaimableBalance(...)`
- **Liquidity Pool**: `liquidityPoolDeposit(...)`, `liquidityPoolWithdraw(...)`
- **Inflation**: `inflation()`
- **Soroban**: `createUploadWasmOperation(generatedWasmSize, wasmSeed)`, `createUploadWasmTx(...)`
- **SetOptions builders**: `setMasterWeight(w)`, `setLowThreshold(t)`, `setMedThreshold(t)`, `setHighThreshold(t)`, `setSigner(s)`, `setFlags(f)`, `clearFlags(f)`, `setInflationDestination(id)`, `setHomeDomain(d)` — return `SetOptionsArguments`, composable with `operator|`.

### Apply Helpers
- `applyManageOffer(...)` / `applyManageBuyOffer(...)` / `applyCreatePassiveOffer(...)` — Apply offer operations and verify ledger state, return offer ID.

### Asset Builders
- `makeNativeAsset()`, `makeInvalidAsset()`, `makeAsset(issuer, code)`, `makeAssetAlphanum12(issuer, code)`, `makeChangeTrustAssetPoolShare(assetA, assetB, fee)`

### Upgrade Helpers
- `executeUpgrade(app, lupgrade)` / `executeUpgrades(app, upgrades)` — Apply ledger upgrades and return resulting header.
- `makeConfigUpgradeSet(ltx, configUpgradeSet)` — Create a config upgrade set entry in the ledger.
- `makeConfigUpgrade(configUpgradeSet)` — Create a `LedgerUpgrade` from a config upgrade set.
- `makeBaseReserveUpgrade(baseReserve)` — Create a base reserve upgrade.

### Query Helpers
- `getRoot(networkID)` / `getAccount(name)` — Get root or named test account keys.
- `loadAccount(ltx, k)` / `doesAccountExist(app, k)` — Account existence checks.
- `getAccountSigners(k, app)` — Get signers for an account.
- `checkLiquidityPool(app, poolID, ...)` — Assert liquidity pool state.
- `getBalance(app, accountID, asset)` — Get balance for any asset type.
- `getLclProtocolVersion(app)` — Get last closed ledger protocol version.
- `isSuccessResult(res)` — Check if result is success (including fee bump inner success).
- `getGenesisAccount(app, accountIndex)` — Get a genesis test account.
- `sorobanResourceFee(app, resources, txSize, eventsSize, ...)` — Compute Soroban resource fee.

### Result Inspection
- `getFirstResult(tx)` / `getFirstResultCode(tx)` — Get first operation result.
- `checkTx(index, resultSet, expected)` — Assert transaction result code at index.
- `expectedResult(fee, opsCount, code, ops)` — Build an expected `TransactionResult`.
- `sign(networkID, key, env)` — Sign a V1 envelope.

### Structs
- `ExpectedOpResult` — Wraps `OperationResult` with constructors for various result codes.
- `ValidationResult` — Pair of `{fee, TransactionResultCode}`.
- `SetOptionsArguments` — Optional fields for set_options, composable via `operator|`.
- `SetTrustLineFlagsArguments` — `{setFlags, clearFlags}`, composable via `operator|`.

## TestMarket (`TestMarket.h` / `TestMarket.cpp`)

Tracks DEX offer state for verification in tests.

### Key Types
- `OfferKey` — `{sellerID, offerID}`, ordered by seller then ID.
- `OfferState` — `{selling, buying, price, amount, type}`. Sentinel values: `OfferState::SAME` (no change expected), `OfferState::DELETED` (offer removed, amount=0).
- `TestMarketOffer` — `{OfferKey, OfferState}`, with `exchanged(ledgerVersion, sold, bought)` returning a `ClaimAtom`.
- `TestMarketBalance` / `TestMarketBalances` — For balance verification.

### TestMarket Class
- `TestMarket(app)` — Owns `mOffers` map and `mLastAddedID`.
- `addOffer(account, state, finishedState)` — Create an offer, verify ID assignment.
- `updateOffer(account, id, state, finishedState)` — Update existing offer.
- `requireChanges(changes, f)` — Execute `f`, then verify offer state changes match expectations. On exception, verifies no unintended changes.
- `requireChangesWithOffer(changes, f)` — Like `requireChanges` but `f` returns the new offer.
- `requireBalances(balances)` — Assert account balances match expectations.
- `checkCurrentOffers()` — Verify all tracked offers match ledger state.
- `checkState(offers, deletedOffers)` — Internal: verify offers exist/deleted in ledger.

## TestUtils (`TestUtils.h` / `TestUtils.cpp`)

### Clock/Crank Helpers (`testutil` namespace)
- `crankSome(clock)` — Crank up to 100 times or 1 second.
- `crankFor(clock, duration)` — Crank until duration elapsed.
- `crankUntil(app, predicate, timeout)` — Crank until predicate is true or timeout.
- `shutdownWorkScheduler(app)` — Shut down work scheduler and crank until aborted.

### Test Application
- `TestApplication` — Subclass of `ApplicationImpl` that overrides `createInvariantManager()` to return a `TestInvariantManager`.
- `TestInvariantManager` — Subclass of `InvariantManagerImpl` that throws `InvariantDoesNotHold` on invariant failure instead of aborting (enables testing invariant violations).
- `createTestApplication<T>(clock, cfg, newDB, startApp)` — Template factory. Creates, adjusts config, optionally starts the application.

### BucketList Helpers
- `BucketListDepthModifier<BucketT>` — RAII class that temporarily modifies `BucketListBase<BucketT>::kNumLevels`, restores on destruction. Instantiated for `LiveBucket` and `HotArchiveBucket`.
- `testBucketMetadata(protocolVersion)` — Create `BucketMetadata` with appropriate version/type fields.

### Date/Time Helpers
- `getTestDate(day, month, year)` — Returns `TimePoint`.
- `getTestDateTime(day, month, year, hour, minute, second)` — Returns `std::tm`.
- `genesis(minute, second)` — Returns a system_time_point at July 1, 2014.

### Soroban Network Config Helpers
- `setSorobanNetworkConfigForTest(cfg, ledgerVersion)` — Sets generous defaults for Soroban config (large limits for instructions, data sizes, etc.).
- `overrideSorobanNetworkConfigForTest(app)` — Apply test Soroban config via full upgrade process.
- `upgradeSorobanNetworkConfig(modifyFn, simulation, applyUpgrade)` — Run loadgen-based config upgrade across a simulation.
- `prepareSorobanNetworkConfigUpgrade(app, modifyFn)` — Full 4-step upgrade preparation: deploy wasm, create instance, create upgrade entry, arm.
- `modifySorobanNetworkConfig(app, modifyFn)` — Full 5-step upgrade including closing the armed ledger.

### Other Utilities
- `getInvalidAssets(issuer)` — Generate a vector of invalid assets for negative testing.
- `computeMultiplier(le)` — Compute reserve multiplier for a ledger entry.
- `appProtocolVersionStartsFrom(app, fromVersion)` — Check if app's ledger version is >= a given version.
- `DEFAULT_TEST_RESOURCE_FEE = 1'000'000` — Constant for Soroban test fees.
- `generateTransactions(app, outputFile, numTransactions, accounts, offset)` — Generate payment transactions to a file using `TxGenerator`.

## TestExceptions (`TestExceptions.h` / `TestExceptions.cpp`)

Exception-based error reporting for transaction test results.

- `throwIf(TransactionResult const& result)` — Examines a transaction result and throws a typed exception for each possible error code across all operation types.
- `ex_txException` — Base class for all test exceptions.
- `TEST_EXCEPTION(M)` macro — Generates exception classes like `ex_CREATE_ACCOUNT_MALFORMED`, `ex_PAYMENT_UNDERFUNDED`, etc.
- Covers all operation types: CreateAccount, Payment, PathPayment (strict receive/send), ManageSellOffer, ManageBuyOffer, SetOptions, ChangeTrust, AllowTrust, AccountMerge, Inflation, ManageData, BumpSequence, CreateClaimableBalance, ClaimClaimableBalance, Clawback, SetTrustLineFlags, LiquidityPoolDeposit, LiquidityPoolWithdraw, InvokeHostFunction, ExtendFootprintTTL, RestoreFootprint.
- Also transaction-level exceptions: `ex_txBAD_SEQ`, `ex_txNO_ACCOUNT`, `ex_txINTERNAL_ERROR`, `ex_txINSUFFICIENT_BALANCE`, `ex_txBAD_AUTH`.

## Catch2 Integration (`Catch2.h` / `Catch2.cpp`)

- `Catch2.h` — Central include point for Catch2 (`lib/catch.hpp`). Defines `StringMaker` specializations for XDR types (using `xdr_to_string`), `OfferState`, `CatchupRange`, and `CatchupPerformedWork` for pretty-printing in test output.
- `Catch2.cpp` — Implements the `StringMaker::convert` methods.

## SimpleTestReporter (`SimpleTestReporter.h`)

Custom Catch2 reporter for minimal output:
- Prints test case name and source location on start.
- Prints dots for section progress (controllable via `gDisableDots`).
- Only reports assertion details on failure.
- Registered as `"simple"` reporter.

## Fuzzing Infrastructure (`Fuzzer.h`, `FuzzerImpl.h`, `FuzzerImpl.cpp`, `fuzz.h`, `fuzz.cpp`)

### Fuzzer Base Class
- `Fuzzer` — Abstract interface with `inject(filename)`, `initialize()`, `shutdown()`, `genFuzz(filename)`, `xdrSizeLimit()`.

### TransactionFuzzer
- Sets up a minimal ledger state with pregened accounts, trustlines, offers, claimable balances, and liquidity pools.
- `inject()` reads fuzzed XDR operations from a file, builds a transaction, and applies it.
- Initialization: creates `NUMBER_OF_PREGENERATED_ACCOUNTS` (5) accounts with trustlines, assets, offers, etc.
- Uses compact key encoding (`getShortKey`/`setShortKey`) to map fuzzed bytes to valid account/asset/ledger key references.
- `FuzzUtils` namespace constants: `NUM_STORED_LEDGER_KEYS = 0x100`, `NUM_UNVALIDATED_LEDGER_KEYS = 0x40`, `NUM_STORED_POOL_IDS = 0x7`.

### OverlayFuzzer
- Creates a 2-node `Simulation` (acceptor + initiator).
- `inject()` reads a `StellarMessage` from file and delivers it to the acceptor's peer connection.

### Fuzz Entry Point
- `fuzz(filename, metrics, processID, fuzzerMode)` — Creates fuzzer, initializes, runs inject loop (with `__AFL_LOOP` for AFL persistent mode).
- `FuzzUtils::createFuzzer(processID, mode)` — Factory for fuzzer instances.

## How Tests Are Structured and Run

1. **Test cases** use Catch2 macros (`TEST_CASE`, `SECTION`) and live alongside their subsystem code (not in `src/test/`).
2. **Test setup** typically: get config via `getTestConfig()`, create `VirtualClock`, create `TestApplication` via `createTestApplication()`.
3. **Account creation**: Use root `TestAccount` from `getRoot(networkID)`, then `root.create(...)` to make sub-accounts.
4. **Transaction testing**: Build operations (e.g., `payment()`, `createAccount()`), wrap in `transactionFromOperations()`, apply with `applyTx()` or `closeLedger()`.
5. **Version iteration**: Use `for_versions(...)` or `TEST_CASE_VERSIONS` to test across protocol versions.
6. **Error testing**: Operations that should fail throw typed exceptions from `TestExceptions.h`; use `REQUIRE_THROWS_AS(...)` to catch them.
7. **DEX testing**: Use `TestMarket` to track and verify offer state changes.
8. **Soroban testing**: Use `sorobanTransactionFrameFromOps()` with `SorobanResources`, apply via `closeLedger()`. Use `overrideSorobanNetworkConfigForTest()` for generous limits.

## Ownership and Relationships

- `TestAccount` holds a reference to `Application&` and owns `SecretKey` + sequence number.
- `TestMarket` holds a reference to `Application&` and owns a `map<OfferKey, OfferState>`.
- `TestApplication` inherits from `ApplicationImpl`, owns a `TestInvariantManager`.
- `getTestConfig()` returns references to globally cached `Config` objects in `gTestCfg[]`.
- `TransactionTestFramePtr` (from `transactions/test/TransactionTestFrame.h`) wraps `TransactionFrameBase` with test-specific methods like `overrideResult()` and `addSignature()`.
- Test temp directories are managed via `gTestRoots` vector of `TmpDir` objects.
