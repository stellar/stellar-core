---
name: subsystem-summary-of-protocol-curr
description: "read this skill for a token-efficient summary of the protocol-curr subsystem"
---

# Subsystem: protocol-curr (Current-Protocol XDR Type Definitions)

## Overview

The `src/protocol-curr/xdr/` directory contains the canonical XDR (External Data Representation) type definitions for the current protocol version of stellar-core. These `.x` files define all on-wire and on-disk data structures used by the Stellar network. Corresponding `.h` files are auto-generated C++ headers from these XDR definitions via the xdrpp code generator. **Never edit `.h` files directly; always modify the `.x` source files.**

The directory is a git submodule pointing to the official Stellar XDR repository.

## File Organization and Dependency Graph

The XDR files form a dependency DAG via `%#include` directives:

```
Stellar-types.x                  (base types, no dependencies)
├── Stellar-SCP.x                (consensus protocol types)
├── Stellar-contract.x           (smart contract value types)
│   └── Stellar-contract-config-setting.x  (Soroban config settings)
├── Stellar-contract-env-meta.x  (contract environment metadata)
├── Stellar-contract-meta.x      (contract metadata)
├── Stellar-contract-spec.x      (contract specification/ABI)
├── Stellar-ledger-entries.x     (ledger state entries; depends on contract.x, contract-config-setting.x)
│   └── Stellar-transaction.x   (transaction types; depends on ledger-entries.x, contract.x)
│       └── Stellar-ledger.x    (ledger structure, meta; depends on transaction.x, SCP.x)
│           ├── Stellar-overlay.x    (peer-to-peer network messages)
│           ├── Stellar-internal.x   (internal-only persistence types)
│           └── Stellar-exporter.x   (ledger export batch types)
```

## Module Summaries

### Stellar-types.x — Foundational Types

Defines primitive and shared types used throughout all other XDR files:

- **Primitive typedefs**: `Hash` (32-byte opaque), `uint256` (32-byte opaque), `uint32`, `int32`, `uint64`, `int64`, `TimePoint` (uint64), `Duration` (uint64)
- **`ExtensionPoint`**: A union (always case 0/void) used as a placeholder in structs for future extensibility.
- **Cryptographic key types**:
  - `CryptoKeyType` enum: `KEY_TYPE_ED25519 (0)`, `KEY_TYPE_PRE_AUTH_TX (1)`, `KEY_TYPE_HASH_X (2)`, `KEY_TYPE_ED25519_SIGNED_PAYLOAD (3)`, `KEY_TYPE_MUXED_ED25519 (0x100)`.
  - `PublicKey` union (discriminant `PublicKeyType`): wraps ed25519 key.
  - `SignerKey` union (discriminant `SignerKeyType`): supports ed25519, pre-auth tx hash, hash-x, ed25519+signed-payload.
- **Identity typedefs**: `NodeID = PublicKey`, `AccountID = PublicKey`, `ContractID = Hash`, `PoolID = Hash`.
- **Signature types**: `Signature` (opaque<64>), `SignatureHint` (opaque[4]).
- **Crypto primitives**: `Curve25519Secret`, `Curve25519Public`, `HmacSha256Key`, `HmacSha256Mac`, `ShortHashSeed`.
- **`SerializedBinaryFuseFilter`**: Probabilistic filter with configurable bit-width (8/16/32-bit), used by bucket list.
- **`ClaimableBalanceID`**: Union keyed by `ClaimableBalanceIDType`, currently only `V0` wrapping a `Hash`.

### Stellar-SCP.x — Stellar Consensus Protocol

Types for the SCP (Federated Byzantine Agreement) consensus mechanism:

- **`SCPBallot`**: `{counter, value}` — a ballot in the SCP protocol.
- **`SCPStatementType`** enum: `PREPARE (0)`, `CONFIRM (1)`, `EXTERNALIZE (2)`, `NOMINATE (3)`.
- **`SCPStatement`**: Contains `nodeID`, `slotIndex`, and a `pledges` union discriminated by `SCPStatementType`:
  - `PREPARE`: `{quorumSetHash, ballot, prepared*, preparedPrime*, nC, nH}`
  - `CONFIRM`: `{ballot, nPrepared, nCommit, nH, quorumSetHash}`
  - `EXTERNALIZE`: `{commit, nH, commitQuorumSetHash}`
  - `NOMINATE`: `SCPNomination {quorumSetHash, votes<>, accepted<>}`
- **`SCPEnvelope`**: `{statement, signature}` — signed SCP message.
- **`SCPQuorumSet`**: `{threshold, validators<>, innerSets<>}` — recursive quorum slice definition (max 4 nesting levels).

### Stellar-contract.x — Smart Contract (Soroban) Value Types

Core types for the Soroban smart contract system:

- **`SCValType`** enum (22 variants): `BOOL, VOID, ERROR, U32, I32, U64, I64, TIMEPOINT, DURATION, U128, I128, U256, I256, BYTES, STRING, SYMBOL, VEC, MAP, ADDRESS, CONTRACT_INSTANCE, LEDGER_KEY_CONTRACT_INSTANCE, LEDGER_KEY_NONCE`.
- **`SCVal`** union: The universal polymorphic value type for Soroban, discriminated by `SCValType`.
- **`SCError`** union: Discriminated by `SCErrorType` (10 types: `CONTRACT, WASM_VM, CONTEXT, STORAGE, OBJECT, CRYPTO, EVENTS, BUDGET, VALUE, AUTH`). Contract errors carry a `uint32` code; all others carry an `SCErrorCode` enum.
- **`SCErrorCode`** enum: `ARITH_DOMAIN, INDEX_BOUNDS, INVALID_INPUT, MISSING_VALUE, EXISTING_VALUE, EXCEEDED_LIMIT, INVALID_ACTION, INTERNAL_ERROR, UNEXPECTED_TYPE, UNEXPECTED_SIZE`.
- **Large integer structs**: `UInt128Parts {hi, lo}`, `Int128Parts {hi(signed), lo}`, `UInt256Parts {hi_hi, hi_lo, lo_hi, lo_lo}`, `Int256Parts`.
- **`ContractExecutable`** union: `WASM` (carries `wasm_hash`) or `STELLAR_ASSET` (void).
- **`SCAddress`** union (discriminant `SCAddressType`): `ACCOUNT(AccountID)`, `CONTRACT(ContractID)`, `MUXED_ACCOUNT`, `CLAIMABLE_BALANCE`, `LIQUIDITY_POOL`.
- **Collection types**: `SCVec = SCVal<>`, `SCMap = SCMapEntry<>`, `SCMapEntry = {key: SCVal, val: SCVal}`.
- **String types**: `SCBytes = opaque<>`, `SCString = string<>`, `SCSymbol = string<32>`.
- **`SCContractInstance`**: `{executable: ContractExecutable, storage: SCMap*}`.

### Stellar-ledger-entries.x — Ledger State Entries

Defines all persistent ledger entry types:

- **`LedgerEntryType`** enum (10 types): `ACCOUNT(0), TRUSTLINE(1), OFFER(2), DATA(3), CLAIMABLE_BALANCE(4), LIQUIDITY_POOL(5), CONTRACT_DATA(6), CONTRACT_CODE(7), CONFIG_SETTING(8), TTL(9)`.
- **Asset types**:
  - `AssetType` enum: `NATIVE(0), CREDIT_ALPHANUM4(1), CREDIT_ALPHANUM12(2), POOL_SHARE(3)`.
  - `Asset` union: void for native, `AlphaNum4/12` for credits (each has `assetCode + issuer`).
  - `TrustLineAsset`: extends Asset with `POOL_SHARE` variant carrying `PoolID`.
  - `ChangeTrustAsset`: extends Asset with `POOL_SHARE` variant carrying `LiquidityPoolParameters`.
  - `Price`: fractional `{n: int32, d: int32}`.
- **`AccountEntry`**: `{accountID, balance, seqNum, numSubEntries, inflationDest*, flags, homeDomain, thresholds, signers<20>}` with extension versions V1 (adds `Liabilities`), V2 (adds sponsorship tracking), V3 (adds `seqLedger`, `seqTime`).
  - `AccountFlags`: `AUTH_REQUIRED(0x1), AUTH_REVOCABLE(0x2), AUTH_IMMUTABLE(0x4), AUTH_CLAWBACK_ENABLED(0x8)`.
- **`TrustLineEntry`**: `{accountID, asset, balance, limit, flags}` with extensions for liabilities and `liquidityPoolUseCount`.
  - `TrustLineFlags`: `AUTHORIZED(1), AUTHORIZED_TO_MAINTAIN_LIABILITIES(2), TRUSTLINE_CLAWBACK_ENABLED(4)`.
- **`OfferEntry`**: `{sellerID, offerID, selling, buying, amount, price, flags}`.
- **`DataEntry`**: `{accountID, dataName, dataValue}` — arbitrary key-value data on accounts.
- **`ClaimableBalanceEntry`**: `{balanceID, claimants<10>, asset, amount}` with `ClaimPredicate` union (recursive: `UNCONDITIONAL, AND, OR, NOT, BEFORE_ABSOLUTE_TIME, BEFORE_RELATIVE_TIME`).
- **`LiquidityPoolEntry`**: Contains `LiquidityPoolConstantProductParameters {assetA, assetB, fee}` and pool state `{reserveA, reserveB, totalPoolShares, poolSharesTrustLineCount}`.
- **Soroban entries**:
  - `ContractDataEntry`: `{contract: SCAddress, key: SCVal, durability: ContractDataDurability, val: SCVal}`. Durability is `TEMPORARY(0)` or `PERSISTENT(1)`.
  - `ContractCodeEntry`: `{hash, code<>}` with optional `ContractCodeCostInputs` in V1 extension.
  - `TTLEntry`: `{keyHash, liveUntilLedgerSeq}` — tracks expiration of Soroban entries.
  - `ConfigSettingEntry`: see contract-config-setting.x below.
- **`LedgerEntry`** union: Wraps all entry types with `lastModifiedLedgerSeq` and optional `LedgerEntryExtensionV1` (carries `sponsoringID`).
- **`LedgerKey`** union: Discriminated by `LedgerEntryType`, carries lookup keys for each entry type.
- **`EnvelopeType`** enum: `TX_V0(0), SCP(1), TX(2), AUTH(3), SCPVALUE(4), TX_FEE_BUMP(5), OP_ID(6), POOL_REVOKE_OP_ID(7), CONTRACT_ID(8), SOROBAN_AUTHORIZATION(9)`.
- **Bucket types**:
  - `BucketListType`: `LIVE(0), HOT_ARCHIVE(1)`.
  - `BucketEntryType`: `METAENTRY(-1), LIVEENTRY(0), DEADENTRY(1), INITENTRY(2)`.
  - `BucketEntry` union and `HotArchiveBucketEntry` union for live and hot-archive bucket lists.
  - `BucketMetadata`: `{ledgerVersion}` with optional `BucketListType` extension.

### Stellar-transaction.x — Transactions and Operations

The largest XDR file (~2100 lines). Defines transaction structure, all 27 operation types, and all result types.

- **`OperationType`** enum (27 operations): `CREATE_ACCOUNT(0)`, `PAYMENT(1)`, `PATH_PAYMENT_STRICT_RECEIVE(2)`, `MANAGE_SELL_OFFER(3)`, `CREATE_PASSIVE_SELL_OFFER(4)`, `SET_OPTIONS(5)`, `CHANGE_TRUST(6)`, `ALLOW_TRUST(7)`, `ACCOUNT_MERGE(8)`, `INFLATION(9)`, `MANAGE_DATA(10)`, `BUMP_SEQUENCE(11)`, `MANAGE_BUY_OFFER(12)`, `PATH_PAYMENT_STRICT_SEND(13)`, `CREATE_CLAIMABLE_BALANCE(14)`, `CLAIM_CLAIMABLE_BALANCE(15)`, `BEGIN_SPONSORING_FUTURE_RESERVES(16)`, `END_SPONSORING_FUTURE_RESERVES(17)`, `REVOKE_SPONSORSHIP(18)`, `CLAWBACK(19)`, `CLAWBACK_CLAIMABLE_BALANCE(20)`, `SET_TRUST_LINE_FLAGS(21)`, `LIQUIDITY_POOL_DEPOSIT(22)`, `LIQUIDITY_POOL_WITHDRAW(23)`, `INVOKE_HOST_FUNCTION(24)`, `EXTEND_FOOTPRINT_TTL(25)`, `RESTORE_FOOTPRINT(26)`.
- **`Operation`** struct: `{sourceAccount*: MuxedAccount, body: union(OperationType)}`.
- **`MuxedAccount`** union: `ed25519` or `{id, ed25519}` for multiplexed accounts.
- **Transaction envelope hierarchy**:
  - `TransactionV0` / `TransactionV0Envelope`: Legacy format (raw ed25519 source key).
  - `Transaction` / `TransactionV1Envelope`: Current format with `MuxedAccount` source, `Preconditions`, `Memo`, `operations<100>`, optional `SorobanTransactionData`.
  - `FeeBumpTransaction` / `FeeBumpTransactionEnvelope`: Wraps an inner `TransactionV1Envelope` with `feeSource` and increased `fee`.
  - `TransactionEnvelope` union: Discriminated by `EnvelopeType` (`TX_V0, TX, TX_FEE_BUMP`).
- **Preconditions**: `Preconditions` union (`NONE, TIME, V2`). `PreconditionsV2` adds `timeBounds*, ledgerBounds*, minSeqNum*, minSeqAge, minSeqLedgerGap, extraSigners<2>`.
- **Memo**: `MemoType` enum (`NONE, TEXT, ID, HASH, RETURN`).
- **Soroban-specific types**:
  - `SorobanResources`: `{footprint: LedgerFootprint, instructions, diskReadBytes, writeBytes}`.
  - `SorobanTransactionData`: `{resources, resourceFee}` with optional `SorobanResourcesExtV0` for archived entries.
  - `HostFunction` union: `INVOKE_CONTRACT, CREATE_CONTRACT, UPLOAD_CONTRACT_WASM, CREATE_CONTRACT_V2`.
  - `SorobanAuthorizationEntry`: `{credentials, rootInvocation}` — authorization tree for Soroban calls.
  - `SorobanCredentials` union: `SOURCE_ACCOUNT(void)` or `ADDRESS(SorobanAddressCredentials)`.
  - `InvokeContractArgs`: `{contractAddress, functionName, args<>}`.
- **`HashIDPreimage`** union: Used for deterministic ID generation, discriminated by `EnvelopeType` (`OP_ID, POOL_REVOKE_OP_ID, CONTRACT_ID, SOROBAN_AUTHORIZATION`).
- **`TransactionSignaturePayload`**: `{networkId, taggedTransaction}` — the structure that is SHA-256 hashed and signed.
- **Result types**: Each operation has a corresponding `*ResultCode` enum and `*Result` union. The top-level chain is:
  - `TransactionResult`: `{feeCharged, result union by TransactionResultCode}`. For fee bumps, wraps `InnerTransactionResultPair`.
  - `TransactionResultCode` enum (18 codes): `txFEE_BUMP_INNER_SUCCESS(1), txSUCCESS(0), txFAILED(-1)`, ..., `txSOROBAN_INVALID(-17)`.
  - `OperationResult`: `{opINNER -> inner union by OperationType, or error code}`.
- **Claim atoms**: `ClaimAtom` union (V0, ORDER_BOOK, LIQUIDITY_POOL) — represents assets exchanged during offer matching.

### Stellar-ledger.x — Ledger Structure and Metadata

Defines ledger headers, upgrades, transaction sets, and close metadata:

- **`LedgerHeader`**: `{ledgerVersion, previousLedgerHash, scpValue, txSetResultHash, bucketListHash, ledgerSeq, totalCoins, feePool, inflationSeq, idPool, baseFee, baseReserve, maxTxSetSize, skipList[4]}`.
  - Extension V1 adds `flags` (`LedgerHeaderFlags`: liquidity pool trading/deposit/withdrawal disable flags).
- **`StellarValue`**: `{txSetHash, closeTime, upgrades<6>}` — the value SCP agrees on. Has `BASIC` or `SIGNED` variant (with `LedgerCloseValueSignature`).
- **`LedgerUpgrade`** union (7 types): `VERSION, BASE_FEE, MAX_TX_SET_SIZE, BASE_RESERVE, FLAGS, CONFIG, MAX_SOROBAN_TX_SET_SIZE`.
- **Transaction sets**:
  - `TransactionSet`: Legacy `{previousLedgerHash, txs<>}`.
  - `TransactionSetV1`: `{previousLedgerHash, phases<>}`.
  - `TransactionPhase` union: V0 has `TxSetComponent<>`, V1 has `ParallelTxsComponent` for parallel execution.
  - `ParallelTxsComponent`: `{baseFee*, executionStages<>}` — stages of clusters for parallel tx application.
  - `GeneralizedTransactionSet` union (v=1): wraps `TransactionSetV1`.
- **Transaction metadata** (multiple versions):
  - `TransactionMeta` union (v0-v4): Records `LedgerEntryChanges` (created/updated/removed/state/restored) before/after operations.
  - `TransactionMetaV3`: Adds `SorobanTransactionMeta` (events, returnValue, diagnosticEvents).
  - `TransactionMetaV4`: Adds `OperationMetaV2` (per-operation events), `TransactionEvent` (fee events with stage info), `SorobanTransactionMetaV2`.
  - `ContractEvent`: `{contractID*, type(SYSTEM/CONTRACT/DIAGNOSTIC), body{topics<>, data}}`.
  - `SorobanTransactionMetaExtV1`: Fee breakdown (nonRefundable, refundable, rent).
- **Ledger close metadata**:
  - `LedgerCloseMeta` union (v0, v1, v2): Packages `LedgerHeaderHistoryEntry`, transaction set, processing results, upgrade meta, SCP info.
  - V1/V2 add `totalByteSizeOfLiveSorobanState`, `evictedKeys<>`.
  - V2 uses `TransactionResultMetaV1` (adds `postTxApplyFeeProcessing`).
- **History entries**: `TransactionHistoryEntry`, `TransactionHistoryResultEntry`, `LedgerHeaderHistoryEntry`, `SCPHistoryEntry`.

### Stellar-contract-config-setting.x — Soroban Configuration

Network-wide Soroban settings stored as `CONFIG_SETTING` ledger entries:

- **`ConfigSettingID`** enum (17 settings): Controls max contract size, compute limits, ledger costs, historical data fees, event limits, bandwidth, cost model params, data size limits, state archival, execution lanes, eviction, parallel compute, SCP timing.
- **`ConfigSettingEntry`** union: Discriminated by `ConfigSettingID`.
- **Key config structs**:
  - `ConfigSettingContractComputeV0`: `ledgerMaxInstructions, txMaxInstructions, feeRatePerInstructionsIncrement, txMemoryLimit`.
  - `ConfigSettingContractLedgerCostV0`: Limits and fees for disk reads/writes, rent pricing.
  - `ConfigSettingContractLedgerCostExtV0`: `txMaxFootprintEntries, feeWrite1KB`.
  - `ConfigSettingContractBandwidthV0`: `ledgerMaxTxsSizeBytes, txMaxSizeBytes, feeTxSize1KB`.
  - `ConfigSettingContractExecutionLanesV0`: `ledgerMaxTxCount`.
  - `ConfigSettingContractParallelComputeV0`: `ledgerMaxDependentTxClusters`.
  - `StateArchivalSettings`: TTL bounds, rent rates, eviction parameters.
  - `ConfigSettingSCPTiming`: Target close time and nomination/ballot timeouts (in ms).
- **`ContractCostType`** enum (85 cost types): Covers WASM execution, memory, hashing (SHA256, Keccak256), signature verification (Ed25519, ECDSA secp256k1/r1), BLS12-381 and BN254 elliptic curve operations, WASM parsing/instantiation costs.
- **`ContractCostParamEntry`**: `{constTerm, linearTerm}` — piecewise linear cost model.
- **`EvictionIterator`**: `{bucketListLevel, isCurrBucket, bucketFileOffset}` — tracks eviction scan position.

### Stellar-overlay.x — Peer-to-Peer Network Messages

Types for node communication:

- **`MessageType`** enum (24 message types): Covers error, auth handshake, peer exchange, transaction/tx-set relay, SCP messages, flow control (`SEND_MORE/SEND_MORE_EXTENDED`), pull-mode flooding (`FLOOD_ADVERT/FLOOD_DEMAND`), and time-sliced surveys.
- **`StellarMessage`** union: Discriminated by `MessageType`, carrying the appropriate payload for each message.
- **`AuthenticatedMessage`** union: Wraps `{sequence, StellarMessage, HmacSha256Mac}`.
- **Handshake**: `Hello` (version info, networkID, peer info, auth cert, nonce), `Auth` (flow control flags).
- **Survey types**: `TimeSlicedSurveyRequestMessage`, `TopologyResponseBodyV2`, `PeerStats`, `TimeSlicedNodeData` — for network topology discovery.
- **Flow control**: `SendMore {numMessages}`, `SendMoreExtended {numMessages, numBytes}`.
- **Flooding**: `FloodAdvert {txHashes<1000>}`, `FloodDemand {txHashes<1000>}`.

### Stellar-contract-env-meta.x — Contract Environment Metadata

Minimal: defines `SCEnvMetaEntry` union with a single variant `SC_ENV_META_KIND_INTERFACE_VERSION` carrying `{protocol: uint32, preRelease: uint32}`.

### Stellar-contract-meta.x — Contract Metadata

Minimal: defines `SCMetaEntry` union with `SC_META_V0` variant carrying `SCMetaV0 {key: string, val: string}` — arbitrary key-value metadata for contracts.

### Stellar-contract-spec.x — Contract Specification (ABI)

Defines the Soroban contract ABI (Application Binary Interface):

- **`SCSpecType`** enum: All Soroban types including primitives, parameterized types (`OPTION, RESULT, VEC, MAP, TUPLE, BYTES_N`), and user-defined types (`UDT`).
- **`SCSpecTypeDef`** union: Recursive type definition supporting all `SCSpecType` variants.
- **`SCSpecEntry`** union (6 kinds): `FUNCTION_V0`, `UDT_STRUCT_V0`, `UDT_UNION_V0`, `UDT_ENUM_V0`, `UDT_ERROR_ENUM_V0`, `EVENT_V0`.
- Each entry includes documentation strings, names, and type information for contract interface description.
- **`SCSpecFunctionV0`**: `{doc, name, inputs<>, outputs<1>}`.
- **`SCSpecEventV0`**: `{doc, lib, name, prefixTopics<2>, params<>, dataFormat}`.

### Stellar-internal.x — Internal Persistence Types

Types used only within a single core instance (not cross-node):

- **`StoredTransactionSet`** union: Legacy or Generalized tx set.
- **`StoredDebugTransactionSet`**: `{txSet, ledgerSeq, scpValue}` — for debugging.
- **`PersistedSCPState`** union (v0, v1): Saved SCP state including envelopes, quorum sets, and optionally tx sets.

### Stellar-exporter.x — Ledger Export Types

- **`LedgerCloseMetaBatch`**: `{startSequence, endSequence, ledgerCloseMetas<>}` — batch of consecutive ledger close metadata for export to downstream systems.

## Key Design Patterns

1. **Union versioning**: Most types use `union switch (int v) { case 0: void; }` extensions for forward compatibility. New fields are added via new case arms.
2. **Envelope pattern**: Data (Transaction, SCP statement) is wrapped in an envelope with signatures for authentication.
3. **Result codes**: Every operation has a `*ResultCode` enum (success = 0, failures < 0) and a `*Result` union.
4. **Fee bump structure**: `FeeBumpTransaction` wraps `TransactionV1Envelope` as `innerTx`, enabling fee sponsorship. `TransactionResult` has special codes `txFEE_BUMP_INNER_SUCCESS/FAILED` that carry `InnerTransactionResultPair`.
5. **Soroban integration**: Soroban operations (`INVOKE_HOST_FUNCTION`, `EXTEND_FOOTPRINT_TTL`, `RESTORE_FOOTPRINT`) use `SorobanTransactionData` for resource declarations and `SorobanAuthorizationEntry` for per-address authorization trees.
6. **Parallel execution support**: `ParallelTxsComponent` and `ParallelTxExecutionStage` organize transactions into dependency-aware clusters for parallel application.
