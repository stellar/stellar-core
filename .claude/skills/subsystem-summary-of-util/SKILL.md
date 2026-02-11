---
name: subsystem-summary-of-util
description: "read this skill for a token-efficient summary of the util subsystem"
---

# Util Subsystem — Technical Summary

The `src/util/` directory provides foundational infrastructure used across all of stellar-core. It covers timing, scheduling, logging, numeric safety, filesystem operations, serialization, data structures, threading, and various small helpers.

---

## 1. Virtual Clock & Timers (`Timer.h`, `Timer.cpp`)

### `VirtualClock`
Central timing facility with two modes: `REAL_TIME` (wall clock) and `VIRTUAL_TIME` (simulated, for tests). Owns an `asio::io_context` for IO dispatch and a `Scheduler` for main-thread action scheduling.

- **Key types**: `time_point` (steady clock), `system_time_point` (wall/calendar time), `duration`.
- **`crank(bool block)`**: The main event-loop step. Dispatches pending timers, polls IO (with exponential priority biasing under overload), runs scheduled actions, and transfers items from the thread-safe pending queue to the scheduler. In `VIRTUAL_TIME` mode, advances time to the next event if idle.
- **`postAction()`**: Thread-safe submission point for deferred work callbacks. Uses a mutex-protected pending queue; wakes the main thread via `asio::post` when enqueueing into an empty queue.
- **`setCurrentVirtualTime()`**: Advance virtual time forward (monotonic; asserts forward progress).
- **`sleep_for()`**: Real sleep in `REAL_TIME`, virtual time advance in `VIRTUAL_TIME`.
- **`shouldYield()`**: In `REAL_TIME`, returns true after 500ms has elapsed (time-slice limit). Always returns true in `VIRTUAL_TIME`.
- **Time conversion helpers**: `to_time_t`, `from_time_t`, `systemPointToTm`, `tmToSystemPoint`, `isoStringToTm`, `tmToISOString`, `systemPointToISOString`.

### `VirtualTimer`
Timer coupled to a `VirtualClock` that uses virtual time. Supports `expires_at()`, `expires_from_now()`, `async_wait()`, `cancel()`. Typical usage for all delayed / periodic operations in core.

### `VirtualClockEvent`
Internal event representation stored in the clock's priority queue. Carries a callback, trigger time, sequence number, and cancelled state.

---

## 2. Scheduler (`Scheduler.h`, `Scheduler.cpp`)

A multi-queue fair scheduler implementing a variant of the LAS (Least Attained Service) / FB (Foreground-Background) algorithm.

### Design
- Multiple named `ActionQueue`s each track cumulative runtime (`mTotalService`).
- On each `runOne()`, the queue with the **lowest** accumulated service time runs its next action.
- A latency window (default 5s) serves three roles: (1) credit cap to prevent bursty monopolization, (2) overload detection threshold, (3) idle queue expiration.
- Droppable actions are shed under overload (queue time exceeds latency window).

### `ActionQueue` (private inner class)
- Stores a deque of `Element{Action, enqueueTime}`.
- Tracks `mTotalService`, `mLastService`, idle list membership.
- `tryTrim()`: drops droppable actions that exceed the latency window.
- `runNext()`: runs the front action and updates total service (floored at `minTotalService`).

### `Scheduler` public interface
- `enqueue(name, action, type)`: Creates or reactivates a queue and appends the action.
- `runOne()`: Dequeues from the lowest-service queue, trims stale idle queues.
- `getOverloadedDuration()`: Reports how long any queue has been overloaded.
- `currentActionType()`: Returns the `ActionType` of the currently executing action.
- `shutdown()`: Clears all queues.

---

## 3. Logging (`Logging.h`, `Logging.cpp`, `SpdlogTweaks.h`, `LogPartitions.def`)

### `Logging` class
Static utility managing spdlog-based logging with partitioned loggers.

- **Partitions**: Defined in `LogPartitions.def` via X-macro (`LOG_PARTITION(name)`), yielding ~15 named partitions (e.g., Bucket, Herder, Ledger, Overlay, Tx, etc.). Each partition has its own spdlog logger with independent log level.
- **`init()`**: Creates sinks (stderr + optional file), registers all partition loggers, configures flush behavior (every 1s, immediate on error). Also initializes Rust-side logging.
- **`setLogLevel(level, partition)`**: Per-partition or global level setting.
- **`setLoggingToFile(filename)`**: Supports `{datetime}`-formatted filenames. Opens in append mode (`O_APPEND`) for durability with external log rotation.
- **`setFmt(peerID)`**: Sets the log pattern including a peer identifier.
- **`rotate()`**: Log rotation support.

### Log macros
- `CLOG_TRACE/DEBUG/INFO/WARNING/ERROR/FATAL(partition, fmt, ...)`: Partition-aware logging with compile-time format string checking via `FMT_STRING`.
- `LOG_TRACE/DEBUG/INFO/WARNING/ERROR/FATAL(logger, fmt, ...)`: Logger-handle-based logging.
- `LOG_CHECK`: Guards log calls with `should_log()` check to avoid formatting overhead.

### `CoutLogger`
Fallback logger when spdlog is not available. Writes to `std::cout`.

### `LogLevel` enum
`LVL_FATAL(0)` through `LVL_TRACE(5)`.

### `format_as` overloads
Provides `fmt`-compatible formatting for XDR enum types and `std::filesystem::path`.

---

## 4. Numeric Utilities (`numeric.h`, `numeric.cpp`, `numeric128.h`)

Safe arithmetic for financial calculations where overflow matters.

### 64-bit operations (`numeric.h`)
- **`bigDivide(result, A, B, C, rounding)`**: Computes `A*B/C` safely when `A*B` would overflow `int64_t`. Returns success/failure bool.
- **`bigDivideOrThrow(A, B, C, rounding)`**: Throwing variant.
- **`bigDivideUnsigned()`**: Unsigned variant.
- **`bigSquareRoot(a, b)`**: Integer square root of `a*b`, `ROUND_DOWN` only.
- **`saturatingMultiply(a, b)`**: Caps at `INT64_MAX` on overflow (non-negative inputs).
- **`saturatingAdd(a, b)`**: Unsigned, caps at type max.
- **`isRepresentableAsInt64(double)`**: Checks safe double→int64 conversion.
- **`doubleToClampedUint32(double)`**: Clamps double to uint32 range.
- **`Rounding` enum**: `ROUND_DOWN`, `ROUND_UP`.

### 128-bit operations (`numeric128.h`)
- **`bigMultiply(a, b)` / `bigMultiplyUnsigned(a, b)`**: Returns `uint128_t`.
- **`bigDivide128()` / `bigDivideUnsigned128()`**: Divide 128-bit by 64-bit.
- **`hugeDivide(result, a, B, C, rounding)`**: Computes `a*B/C` when `C < INT32_MAX * INT64_MAX`.

---

## 5. Filesystem Utilities (`Fs.h`, `Fs.cpp`, `FileSystemException.h`, `TmpDir.h`)

### `fs` namespace
- **File locking**: `lockFile()` / `unlockFile()` using `flock` (POSIX) or `CreateFile` (Win32).
- **Durable I/O**: `flushFileChanges()` (fsync), `durableRename()` (rename + dir fsync), `openFileToWrite()`.
- **Directory ops**: `exists()`, `deltree()`, `mkdir()`, `mkpath()` (recursive), `findfiles()`.
- **Path construction**: `hexStr()`, `hexDir()`, `baseName()`, `remoteName()`, `remoteDir()` — used for history archive paths.
- **Handle counting**: `getMaxHandles()`, `getOpenHandleCount()` — Linux reads `/proc/self/fd`.
- **`bufsz()`**: Returns 256KB (AWS EBS IOP alignment).

### `FileSystemException`
Extends `std::runtime_error` with `failWith()` and `failWithErrno()` static helpers that log before throwing.

### `TmpDir` / `TmpDirManager`
RAII temporary directory management. `TmpDirManager` creates and cleans a root directory; `TmpDir` creates prefixed subdirectories that are deleted on destruction.

---

## 6. XDR Stream I/O (`XDRStream.h`)

### `XDRInputFileStream`
Reads XDR-framed objects from a file one at a time.
- `readOne(T& out, SHA256* hasher)`: Reads 4-byte big-endian size header, then that many bytes, unmarshals into `out`. Optional hash accumulation.
- `readPage(T& out, key, pageSize)`: Reads a page-sized chunk and scans for a specific `LedgerKey`. Used for indexed bucket lookups.
- Supports `seek()`, `pos()`, size limits.

### `OutputFileStream`
Low-level durable write stream using ASIO buffered writes (POSIX) or `FILE*` (Win32).
- `writeBytes()`, `flush()`, `close()` with optional fsync on close.
- Cross-platform via `fs::stream_t` / `fs::native_handle_t`.

### `XDROutputFileStream`
Extends `OutputFileStream` with XDR framing.
- `writeOne(T, hasher, bytesPut)`: Encodes 4-byte size header + XDR payload. Optional hash and byte counting.
- `durableWriteOne()`: `writeOne` + flush + fsync for crash safety.

---

## 7. Data Structures

### `RandomEvictionCache<K, V>` (`RandomEvictionCache.h`)
Fixed-size cache with random-2-choices eviction (not LRU). More robust under pathological access patterns.
- Stores entries in `unordered_map` with stable pointers in a `vector` for random access.
- Tracks generation counter for last-access ordering.
- `put()`, `get()`, `maybeGet()`, `exists()`, `erase_if()`, `clear()`.
- Maintains hit/miss/insert/update/evict counters.

### `BitSet` (`BitSet.h`)
Value-semantic C++ wrapper around a C bitset (`cbitset.h`). Uses inline storage for small sets (≤64 bits) to avoid heap allocation.
- Full set operations: union (`|`), intersection (`&`), difference (`-`), symmetric difference.
- Counting variants: `unionCount()`, `intersectionCount()`, etc.
- Iteration: `nextSet(i)` for scanning set bits.
- Hash function support via `BitSet::HashFunction`.

### `UnorderedMap<K, V>` / `UnorderedSet<K>` (`UnorderedMap.h`, `UnorderedSet.h`)
Type aliases for `std::unordered_map` / `std::unordered_set` using `RandHasher` to prevent hash-flooding attacks.

### `BinaryFuseFilter<T>` (`BinaryFuseFilter.h`, `BinaryFuseFilter.cpp`)
Probabilistic membership-test filter (like Bloom filter but more space-efficient). Wraps `binary_fuse_t` library. Supports 8/16/32-bit widths with corresponding false-positive rates (1/256, 1/65536, 1/4B). Serializable via cereal to `SerializedBinaryFuseFilter` XDR type. Used for efficient key-membership checks in bucket indexes.

### `TarjanSCCCalculator` (`TarjanSCCCalculator.h`)
Tarjan's algorithm for computing strongly connected components of a directed graph. Uses `BitSet` for SCC representation. Used in quorum analysis (SCP).

---

## 8. Threading Utilities

### `ThreadAnnotations.h`
Comprehensive Clang thread-safety annotation macros (`GUARDED_BY`, `REQUIRES`, `ACQUIRE`, `RELEASE`, `TRY_ACQUIRE`, etc.). Provides annotated mutex wrappers:
- **`Mutex`**: Wraps `std::mutex` with annotations.
- **`SharedMutex`**: Wraps `std::shared_mutex` with `Lock()`/`LockShared()`.
- **`RecursiveMutex`**: Wraps `std::recursive_mutex`.
- **`MutexLocker` / `RecursiveMutexLocker` / `SharedLockExclusive` / `SharedLockShared`**: RAII lock guards with annotations.

### `GlobalChecks.h` / `GlobalChecks.cpp`
- **`threadIsMain()`**: Checks if current thread is the main thread (captured at static init).
- **`releaseAssert(e)`**: Assert that is NOT compiled out by `NDEBUG`. Prints backtrace and aborts.
- **`releaseAssertOrThrow(e)`**: Like `releaseAssert` but throws `runtime_error` instead of aborting.
- **`dbgAssert(e)`**: Debug-only assert.
- **`LockGuard` / `RecursiveLockGuard`**: Aliases that adapt between Tracy-instrumented and plain lock guards.

### `Thread.h` / `Thread.cpp`
- `runCurrentThreadWithLowPriority()` / `runCurrentThreadWithMediumPriority()`: OS-level thread priority adjustment.
- `futureIsReady(std::future|std::shared_future)`: Non-blocking readiness check.

### `JitterInjection.h` / `JitterInjection.cpp`
Testing framework for probabilistic thread-delay injection (enabled via `BUILD_THREAD_JITTER`).
- `JitterInjector::injectDelay(probability, minUsec, maxUsec)`: Probabilistically sleeps to expose race conditions.
- `JITTER_INJECT_DELAY()`, `JITTER_YIELD()` macros: Compile to no-ops in production.
- Reproducible via seeded RNG tied to test seed.

### `NonCopyable.h`
Base structs: `NonCopyable`, `NonMovable`, `NonMovableOrCopyable` — disable copy/move semantics via deleted special members.

---

## 9. Metrics & Performance Monitoring

### `MetricsRegistry` (`MetricsRegistry.h`, `MetricsRegistry.cpp`)
Extends `medida::MetricsRegistry` with `SimpleTimer` support. `NewSimpleTimer()` registers lightweight timers. `syncSimpleTimerStats()` syncs max values (called on `/metrics` endpoint).

### `SimpleTimer` / `SimpleTimerContext` (`SimpleTimer.h`, `SimpleTimer.cpp`)
Lightweight replacement for `medida::Timer` using counters (sum, count, max). Avoids histogram overhead. Uses `medida::Counter` internally for metric export. `TimeScope()` returns an RAII context for automatic timing.

### `MetricResetter` (`MetricResetter.h`, `MetricResetter.cpp`)
Implements `medida::MetricProcessor` to reset all metric types (Counter, Meter, Histogram, Timer, Buckets).

### `LogSlowExecution` (`LogSlowExecution.h`, `LogSlowExecution.cpp`)
RAII guard that logs a warning if a scope takes longer than a threshold (default 1 second). Two modes:
- `AUTOMATIC_RAII`: Logs on destructor.
- `MANUAL`: Caller checks `checkElapsedTime()`.
- `RateLimitedLog`: Subclass with zero threshold for rate-limited logging.

---

## 10. Type Utilities (`types.h`, `types.cpp`)

Core type definitions and helpers for XDR/Stellar types.
- **`Blob`**: `std::vector<unsigned char>`.
- **`LedgerKeySet`**: `std::set<LedgerKey, LedgerEntryIdCmp>`.
- **`LedgerEntryKey(LedgerEntry)`**: Extracts the key from an entry.
- **Asset helpers**: `assetToString()`, `getIssuer()`, `isIssuer()`, `assetCodeToStr()`, `strToAssetCode()`, `isAssetValid()`, `compareAsset()`.
- **`getBucketLedgerKey()`**: Extracts `LedgerKey` from `BucketEntry` or `HotArchiveBucketEntry`.
- **`addBalance(balance, delta, max)`**: Safe balance addition with overflow check.
- **`isZero(uint256)`**, **`lessThanXored()`**: Hash/XOR comparison helpers.
- **`unsignedToSigned()`**: Safe conversion with overflow check.
- **`formatSize()`**: Human-readable size formatting.
- **`iequals()`**: Case-insensitive string comparison.
- **Price comparison operators**: `>=`, `>`, `==`.
- **ASCII helpers**: `isAsciiAlphaNumeric()`, `isAsciiNonControl()`, `toAsciiLower()` — locale-independent.
- **`roundDown(v, m)`**: Round down to largest multiple of power-of-2.

---

## 11. Resource Tracking (`TxResource.h`, `TxResource.cpp`)

### `Resource`
Multi-dimensional resource vector for transaction resource accounting (operations, instructions, byte sizes, ledger entries). Supports:
- 1-element (classic), 2-element (classic with bytes), or 7-element (Soroban) resource tuples.
- Arithmetic: `+=`, `-=`, `+`, `-`, with overflow-safe `bigDivideOrThrow`.
- Comparisons: `<=`, `==`, `>`, `anyLessThan()`, `anyGreater()`.
- `subtractNonNegative()`, `limitTo()`, `saturatedMultiplyByDouble()`.
- `Resource::Type` enum: `OPERATIONS`, `INSTRUCTIONS`, `TX_BYTE_SIZE`, `DISK_READ_BYTES`, `WRITE_BYTES`, `READ_LEDGER_ENTRIES`, `WRITE_LEDGER_ENTRIES`.

---

## 12. XDR Query Engine (`xdrquery/`)

A mini query language for filtering XDR objects by field values.

### Components
- **`XDRFieldResolver.h`**: Template-based XDR field traversal using `xdr_traits`. Resolves dotted field paths (e.g., `"data.account.balance"`) to values. Handles unions, enums, public keys (string representation), opaque arrays (hex).
- **`XDRQueryEval.h`**: Expression evaluation engine. `EvalNode` hierarchy: `LiteralNode`, `ColumnNode`, `BoolOpNode` (AND/OR/NOT), `ComparisonOpNode` (==, !=, <, >, <=, >=). `DynamicXDRGetter` interface for runtime field access.
- **`XDRQuery.h`**: `TypedDynamicXDRGetterResolver<T>` connects XDR types to the query engine. `matchXDRQuery()` function parses and evaluates queries against XDR objects.
- **`XDRQueryError.h`**: Exception type for query parsing/evaluation errors.

---

## 13. Serialization Helpers

### `XDRCereal.h` / `XDRCereal.cpp`
Custom `cereal_override` functions for human-readable JSON serialization of XDR types. Overrides for: public keys (StrKey), assets (code+issuer), enums (string names), opaque data (hex), optional pointers (null handling), SCAddress, MuxedAccount, ConfigUpgradeSetKey.
- `xdrToCerealString(t, name, compact)`: Serialize any XDR type to JSON string.

### `BufferedAsioCerealOutputArchive.h`
Custom cereal output archive that writes through `OutputFileStream` (supporting fsync) instead of `std::ofstream`.

### `XDROperators.h`
Imports `xdr::operator==` and `xdr::operator<` into the `stellar` namespace.

---

## 14. Hashing Utilities

### `RandHasher<T>` (`RandHasher.h`)
Hash functor that XORs the output of `std::hash<T>` with a random mixer value (initialized once at startup). Prevents hash-flooding DoS attacks on hash tables.

### `HashOfHash` (`HashOfHash.h`, `HashOfHash.cpp`)
Specialization of `std::hash<stellar::uint256>` for use in hash maps keyed by transaction/ledger hashes.

---

## 15. Protocol Version Utilities (`ProtocolVersion.h`, `ProtocolVersion.cpp`)

### `ProtocolVersion` enum
Enumerates all protocol versions `V_0` through `V_26`.

### Comparison functions
- `protocolVersionIsBefore(version, beforeVersion)`: Strictly less than.
- `protocolVersionStartsFrom(version, fromVersion)`: Greater than or equal.
- `protocolVersionEquals(version, equalsVersion)`: Exact match.

### Notable constants
- `SOROBAN_PROTOCOL_VERSION = V_20`
- `PARALLEL_SOROBAN_PHASE_PROTOCOL_VERSION = V_23`
- `AUTO_RESTORE_PROTOCOL_VERSION = V_23`

---

## 16. Other Utilities

### `StatusManager` (`StatusManager.h`)
Manages status messages by category (`HISTORY_CATCHUP`, `HISTORY_PUBLISH`, `NTP`, `REQUIRES_UPGRADES`). Used for the `/info` JSON endpoint.

### `SecretValue` (`SecretValue.h`)
Wrapper around `std::string` to prevent accidental logging of sensitive values (DB passwords, secret keys).

### `Decoder.h`
Base32/Base64 encoding and decoding helpers (`encode_b32`, `encode_b64`, `decode_b32`, `decode_b64`).

### `Math.h` / `Math.cpp`
Random number utilities: `rand_fraction()`, `rand_flip()`, `rand_uniform()`, `rand_element()`, `k_means()`, `closest_cluster()`, `exponentialBackoff()`. `initializeAllGlobalState()` initializes all PRNG and hash seeds. `reinitializeAllGlobalStateWithSeed()` (test-only) resets for reproducibility.

### `MetaUtils.h` / `MetaUtils.cpp`
`normalizeMeta()`: Sorts order-agnostic fields in `TransactionMeta` / `LedgerCloseMeta` for deterministic comparison/hashing.

### `DebugMetaUtils.h` / `DebugMetaUtils.cpp`
Manages debug meta-data files (XDR segments of ledger metadata). Handles file naming, path construction, listing, and cleanup based on configurable segment sizes (256 ledgers).

### `Backtrace.h` / `Backtrace.cpp`
`printCurrentBacktrace()`: Prints stack trace via the Rust bridge.

### `must_use.h`
`MUST_USE` macro: Maps to `__attribute__((warn_unused_result))` where supported.

### `Algorithm.h`
`split()` function template: Splits a vector into a map of vectors keyed by an extractor function.

### `asio.h`
Correctly configures and includes ASIO headers with proper preprocessor definitions (`ASIO_SEPARATE_COMPILATION`, `ASIO_STANDALONE`). Must be included before other system headers.

### `TcmallocConfig.cpp`
Tcmalloc configuration (if tcmalloc is the allocator).

---

## Key Data Flows

1. **Main event loop**: `VirtualClock::crank()` → dispatches timers → polls ASIO IO → runs `Scheduler::runOne()` → transfers pending actions from thread-safe queue to scheduler.

2. **Cross-thread work submission**: Any thread → `VirtualClock::postAction()` (mutex-protected queue) → main thread dequeues into `Scheduler` → fair-scheduled execution.

3. **XDR file I/O**: Write path: `XDROutputFileStream::writeOne()` → 4-byte size header + XDR marshal → buffered write → optional fsync. Read path: `XDRInputFileStream::readOne()` → read size header → read payload → XDR unmarshal.

4. **Metrics flow**: Code uses `medida::Timer`/`SimpleTimer` → `MetricsRegistry` collects → `syncSimpleTimerStats()` synchronizes max tracking → exported via `/metrics` endpoint.

5. **Resource accounting**: `Resource` vectors created per-transaction → arithmetic operations check against ledger limits → `anyGreater()`/`<=` comparisons for admission control.
