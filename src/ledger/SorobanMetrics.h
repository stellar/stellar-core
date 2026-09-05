// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// This class exists to cache soroban metrics: resource usage and network config
// limits. It also performs aggregation of ledger-wide resource usage across
// different operations.
#include <chrono>
#include <cstdint>
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

// Collection of counters and sample metric streams for Soroban.
//
// For the sake of optimization, the counters here are simple accumulators and
// vectors, which then can be merged and flushed into Medida for the actual
// publish.
//
// Note: this struct is _not_ threadsafe, and meant to be accumulated
// per-thread.
struct SorobanApplyMetrics
{
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

    // Pending ledger-wide accumulator values, published as single per-ledger
    // samples into the corresponding histograms.
    uint64_t mLedgerTxCount{0};
    uint64_t mLedgerCpuInsn{0};
    uint64_t mLedgerTxsSizeByte{0};
    uint64_t mLedgerReadEntry{0};
    uint64_t mLedgerReadByte{0};
    uint64_t mLedgerWriteEntry{0};
    uint64_t mLedgerWriteByte{0};
    uint64_t mLedgerInsnsCount{0};
    uint64_t mLedgerInsnsExclVmCount{0};
    uint64_t mLedgerHostFnExecTimeNsecs{0};

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

    // Adds all the accumulated values and samples of `other` into this
    // instance.
    void merge(SorobanApplyMetrics&& other);
};

// Collection of the Medida metrics related to Soroban.
class SorobanMetricsRegistry
{
#ifdef BUILD_TESTS
  public:
#else
  private:
#endif
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
    // and "ledger.operation.apply" timers. Unlike the rest of the metrics
    // here these are shared with the Classic apply path, the class name
    // ambiguity is a trade-off here (SorobanAndClassicMetricsRegistry doesn't
    // read too well).
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

  public:
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

    SorobanMetricsRegistry(MetricsRegistry& metrics);

    // Records the provided apply metrics into the underlying medida metrics.
    void recordApplyMetrics(SorobanApplyMetrics const& metrics);
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
}
