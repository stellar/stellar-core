// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// This class exists to cache soroban metrics: resource usage and network config
// limits. It also performs aggregation of ledger-wide resource usage across
// different operations.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace medida
{
class Timer;
class Meter;
class Counter;
class Histogram;
}

namespace stellar
{
class MetricsRegistry;

class SorobanMetrics
{
  public:
    // Accumulates apply-path metric updates from a single thread. Hot apply
    // code records into its own thread's batch (brief, uncontended lock
    // acquisition), and the batches are drained into the underlying
    // process-wide medida metrics once per ledger on the main thread via
    // publishAndResetLedgerWideMetrics(). This keeps shared metric state (its
    // locks and cache lines) off the parallel apply threads.
    struct ApplyMetricsBatch
    {
        std::mutex mMutex;

        // Pending Meter increments (Marks summed since the last publish).
        uint64_t mHostFnOpReadEntry{0};
        uint64_t mHostFnOpWriteEntry{0};
        uint64_t mHostFnOpReadKeyByte{0};
        uint64_t mHostFnOpWriteKeyByte{0};
        uint64_t mHostFnOpReadLedgerByte{0};
        uint64_t mHostFnOpReadDataByte{0};
        uint64_t mHostFnOpReadCodeByte{0};
        uint64_t mHostFnOpWriteLedgerByte{0};
        uint64_t mHostFnOpWriteDataByte{0};
        uint64_t mHostFnOpWriteCodeByte{0};
        uint64_t mHostFnOpEmitEvent{0};
        uint64_t mHostFnOpEmitEventByte{0};
        uint64_t mHostFnOpCpuInsn{0};
        uint64_t mHostFnOpMemByte{0};
        uint64_t mHostFnOpCpuInsnExclVm{0};
        uint64_t mHostFnOpMaxRwKeyByte{0};
        uint64_t mHostFnOpMaxRwDataByte{0};
        uint64_t mHostFnOpMaxRwCodeByte{0};
        uint64_t mHostFnOpMaxEmitEventByte{0};
        uint64_t mHostFnOpSuccess{0};
        uint64_t mHostFnOpFailure{0};
        uint64_t mExtFpTtlOpReadLedgerByte{0};
        uint64_t mRestoreFpOpReadLedgerByte{0};
        uint64_t mRestoreFpOpWriteLedgerByte{0};

        // Pending sample streams for percentile-bearing histograms/timers
        // (timer samples are in nanoseconds).
        std::vector<int64_t> mHostFnOpInvokeTimeNsecs;
        std::vector<int64_t> mHostFnOpInvokeTimeNsecsExclVm;
        std::vector<int64_t> mHostFnOpInvokeTimeFsecsCpuInsnRatio;
        std::vector<int64_t> mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm;
        std::vector<int64_t> mHostFnOpDeclaredInsnsUsageRatio;
        std::vector<int64_t> mHostFnOpExecNsecs;
        std::vector<int64_t> mExtFpTtlOpExecNsecs;
        std::vector<int64_t> mRestoreFpOpExecNsecs;
        std::vector<int64_t> mTxSizeByte;
        std::vector<int64_t> mTxApplyNsecs;
        std::vector<int64_t> mOpApplyNsecs;
    };

  private:
    std::atomic<uint64_t> mCounterLedgerTxCount{0};
    std::atomic<uint64_t> mCounterLedgerCpuInsn{0};
    std::atomic<uint64_t> mCounterLedgerTxsSizeByte{0};
    std::atomic<uint64_t> mCounterLedgerReadEntry{0};
    std::atomic<uint64_t> mCounterLedgerReadByte{0};
    std::atomic<uint64_t> mCounterLedgerWriteEntry{0};
    std::atomic<uint64_t> mCounterLedgerWriteByte{0};

    // These are modified within InvokeHostFunctionOp
    std::atomic<uint64_t> mLedgerInsnsCount{0};
    std::atomic<uint64_t> mLedgerInsnsExclVmCount{0};
    std::atomic<uint64_t> mLedgerHostFnExecTimeNsecs{0};

    // All per-thread batches handed out by getApplyThreadBatch(), drained on
    // each publishAndResetLedgerWideMetrics() call.
    std::mutex mApplyBatchesMutex;
    std::unordered_map<std::thread::id, std::unique_ptr<ApplyMetricsBatch>>
        mApplyBatches;

    void flushApplyMetricsBatches();

  public:
    // ledger-wide metrics
    medida::Histogram& mLedgerTxCount;
    medida::Histogram& mLedgerCpuInsn;
    medida::Histogram& mLedgerTxsSizeByte;
    medida::Histogram& mLedgerReadEntry;
    medida::Histogram& mLedgerReadLedgerByte;
    medida::Histogram& mLedgerWriteEntry;
    medida::Histogram& mLedgerWriteLedgerByte;
    medida::Histogram& mLedgerHostFnCpuInsnsRatio;
    medida::Histogram& mLedgerHostFnCpuInsnsRatioExclVm;

    // tx-wide metrics
    medida::Histogram& mTxSizeByte;

    // Cached references to the (op-kind-agnostic) "ledger.transaction.apply"
    // and "ledger.operation.apply" timers: the parallel apply path records
    // per-tx/per-op samples into its ApplyMetricsBatch and they are published
    // into these at ledger close. These are the same timer instances the
    // sequential apply path updates directly via registry lookups.
    medida::Timer& mTransactionApply;
    medida::Timer& mOperationApply;

    // `InvokeHostFunctionOp` metrics
    medida::Meter& mHostFnOpReadEntry;
    medida::Meter& mHostFnOpWriteEntry;
    medida::Meter& mHostFnOpReadKeyByte;
    medida::Meter& mHostFnOpWriteKeyByte;
    medida::Meter& mHostFnOpReadLedgerByte;
    medida::Meter& mHostFnOpReadDataByte;
    medida::Meter& mHostFnOpReadCodeByte;
    medida::Meter& mHostFnOpWriteLedgerByte;
    medida::Meter& mHostFnOpWriteDataByte;
    medida::Meter& mHostFnOpWriteCodeByte;
    medida::Meter& mHostFnOpEmitEvent;
    medida::Meter& mHostFnOpEmitEventByte;
    medida::Meter& mHostFnOpCpuInsn;
    medida::Meter& mHostFnOpMemByte;
    medida::Timer& mHostFnOpInvokeTimeNsecs;
    medida::Meter& mHostFnOpCpuInsnExclVm;
    medida::Timer& mHostFnOpInvokeTimeNsecsExclVm;
    medida::Histogram& mHostFnOpInvokeTimeFsecsCpuInsnRatio;
    medida::Histogram& mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm;
    medida::Histogram& mHostFnOpDeclaredInsnsUsageRatio;
    medida::Meter& mHostFnOpMaxRwKeyByte;
    medida::Meter& mHostFnOpMaxRwDataByte;
    medida::Meter& mHostFnOpMaxRwCodeByte;
    medida::Meter& mHostFnOpMaxEmitEventByte;
    medida::Meter& mHostFnOpSuccess;
    medida::Meter& mHostFnOpFailure;
    medida::Timer& mHostFnOpExec;

    // `ExtendFootprintTTLOp` metrics
    medida::Meter& mExtFpTtlOpReadLedgerByte;
    medida::Timer& mExtFpTtlOpExec;

    // `RestoreFootprintOp` metrics
    medida::Meter& mRestoreFpOpReadLedgerByte;
    medida::Meter& mRestoreFpOpWriteLedgerByte;
    medida::Timer& mRestoreFpOpExec;

    // `NetworkConfig` metrics
    medida::Counter& mConfigContractDataKeySizeBytes;
    medida::Counter& mConfigMaxContractDataEntrySizeBytes;
    medida::Counter& mConfigMaxContractSizeBytes;
    medida::Counter& mConfigTxMaxSizeByte;
    medida::Counter& mConfigTxMaxCpuInsn;
    medida::Counter& mConfigTxMemoryLimitBytes;
    medida::Counter& mConfigTxMaxDiskReadEntries;
    medida::Counter& mConfigTxMaxDiskReadBytes;
    medida::Counter& mConfigTxMaxWriteLedgerEntries;
    medida::Counter& mConfigTxMaxWriteBytes;
    medida::Counter& mConfigMaxContractEventsSizeBytes;
    medida::Counter& mConfigLedgerMaxTxCount;
    medida::Counter& mConfigLedgerMaxInstructions;
    medida::Counter& mConfigLedgerMaxTxsSizeByte;
    medida::Counter& mConfigLedgerMaxDiskReadEntries;
    medida::Counter& mConfigLedgerMaxDiskReadBytes;
    medida::Counter& mConfigLedgerMaxWriteEntries;
    medida::Counter& mConfigLedgerMaxWriteBytes;
    medida::Counter& mConfigBucketListTargetSizeByte;
    medida::Counter& mConfigFeeWrite1KB;

    // Module cache related metrics
    medida::Counter& mModuleCacheNumEntries;
    medida::Timer& mModuleCompilationTime;
    medida::Timer& mModuleCacheRebuildTime;
    medida::Counter& mModuleCacheRebuildBytes;

    // In-memory state metrics
    medida::Counter& mContractCodeStateSize;
    medida::Counter& mContractDataStateSize;
    medida::Counter& mContractCodeEntryCount;
    medida::Counter& mContractDataEntryCount;

    SorobanMetrics(MetricsRegistry& metrics);

    // Returns the calling thread's metrics batch for this SorobanMetrics
    // instance, creating and registering it on first use.
    ApplyMetricsBatch& getApplyThreadBatch();

    void accumulateModelledCpuInsns(uint64_t insnsCount,
                                    uint64_t insnsExclVmCount,
                                    uint64_t execTimeNsecs);
    void accumulateLedgerTxCount(uint64_t txCount);
    void accumulateLedgerCpuInsn(uint64_t cpuInsn);
    void accumulateLedgerTxsSizeByte(uint64_t txsSizeByte);
    void accumulateLedgerReadEntry(uint64_t readEntry);
    void accumulateLedgerReadByte(uint64_t readByte);
    void accumulateLedgerWriteEntry(uint64_t writeEntry);
    void accumulateLedgerWriteByte(uint64_t writeByte);

    void publishAndResetLedgerWideMetrics();
};

// Adds the wall-clock duration of its lifetime (in nanoseconds) to a
// caller-provided accumulator on destruction.
class ScopedNsecsTimer
{
  public:
    explicit ScopedNsecsTimer(uint64_t& target)
        : mTarget(target), mStart(std::chrono::steady_clock::now())
    {
    }

    ScopedNsecsTimer(ScopedNsecsTimer const&) = delete;
    ScopedNsecsTimer& operator=(ScopedNsecsTimer const&) = delete;

    ~ScopedNsecsTimer()
    {
        mTarget += std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - mStart)
                       .count();
    }

  private:
    uint64_t& mTarget;
    std::chrono::steady_clock::time_point mStart;
};

// Times its lifetime and records the elapsed nanoseconds as a sample in the
// given vector of the calling thread's ApplyMetricsBatch.
class BatchedTimerScope
{
  public:
    using SampleField =
        std::vector<int64_t> SorobanMetrics::ApplyMetricsBatch::*;

    BatchedTimerScope(SorobanMetrics& metrics, SampleField field)
        : mMetrics(metrics)
        , mField(field)
        , mStart(std::chrono::steady_clock::now())
    {
    }

    BatchedTimerScope(BatchedTimerScope const&) = delete;
    BatchedTimerScope& operator=(BatchedTimerScope const&) = delete;

    ~BatchedTimerScope()
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - mStart)
                           .count();
        auto& batch = mMetrics.getApplyThreadBatch();
        std::lock_guard<std::mutex> lock(batch.mMutex);
        (batch.*mField).push_back(elapsed);
    }

  private:
    SorobanMetrics& mMetrics;
    SampleField mField;
    std::chrono::steady_clock::time_point mStart;
};
}
