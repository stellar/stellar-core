#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This class exists to cache soroban metrics: resource usage and network config
// limits. It also performs aggregation of ledger-wide resource usage across
// different operations.
#include <atomic>
#include <cstdint>

namespace medida
{
class Timer;
class Meter;
class Counter;
class Histogram;
class MetricsRegistry;
}

namespace stellar
{

class SorobanMetrics
{
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
    medida::Counter& mConfigtxMaxDiskReadEntries;
    medida::Counter& mConfigtxMaxDiskReadBytes;
    medida::Counter& mConfigTxMaxWriteLedgerEntries;
    medida::Counter& mConfigTxMaxWriteBytes;
    medida::Counter& mConfigMaxContractEventsSizeBytes;
    medida::Counter& mConfigLedgerMaxTxCount;
    medida::Counter& mConfigLedgerMaxInstructions;
    medida::Counter& mConfigLedgerMaxTxsSizeByte;
    medida::Counter& mConfigledgerMaxDiskReadEntries;
    medida::Counter& mConfigledgerMaxDiskReadBytes;
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

    SorobanMetrics(medida::MetricsRegistry& metrics);

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
}