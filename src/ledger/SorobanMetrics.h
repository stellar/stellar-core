#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This class exists to cache soroban metrics: resource usage and network config
// limits. It also performs aggregation of ledger-wide resource usage across
// different operations.
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
    medida::MetricsRegistry& mMetrics;
    uint64_t mLedgerCpuInsn{0};
    uint64_t mLedgerReadEntry{0};
    uint64_t mLedgerReadByte{0};
    uint64_t mLedgerWriteEntry{0};
    uint64_t mLedgerWriteByte{0};

  public:
    // InvokeHostFunctionOp
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
    medida::Meter& mHostFnOpMaxRwKeyByte;
    medida::Meter& mHostFnOpMaxRwDataByte;
    medida::Meter& mHostFnOpMaxRwCodeByte;
    medida::Meter& mHostFnOpMaxEmitEventByte;
    medida::Meter& mHostFnOpSuccess;
    medida::Meter& mHostFnOpFailure;
    medida::Timer& mHostFnOpExec;

    // ExtendFootprintTTLOp
    medida::Meter& mExtFpTtlOpReadLedgerByte;
    medida::Timer& mExtFpTtlOpExec;

    // RestoreFootprintOp
    medida::Meter& mRestoreFpOpReadLedgerByte;
    medida::Meter& mRestoreFpOpWriteLedgerByte;
    medida::Timer& mRestoreFpOpExec;

    // network config
    medida::Counter& mConfigContractMaxRwKeyByte;
    medida::Counter& mConfigContractMaxRwDataByte;
    medida::Counter& mConfigContractMaxRwCodeByte;
    medida::Counter& mConfigTxMaxCpuInsn;
    medida::Counter& mConfigTxMaxMemByte;
    medida::Counter& mConfigTxMaxReadEntry;
    medida::Counter& mConfigTxMaxReadLedgerByte;
    medida::Counter& mConfigTxMaxWriteEntry;
    medida::Counter& mConfigTxMaxWriteLedgerByte;
    medida::Counter& mConfigTxMaxEmitEventByte;
    medida::Counter& mConfigLedgerMaxCpuInsn;
    medida::Counter& mConfigLedgerMaxReadEntry;
    medida::Counter& mConfigLedgerMaxReadLedgerByte;
    medida::Counter& mConfigLedgerMaxWriteEntry;
    medida::Counter& mConfigLedgerMaxWriteLedgerByte;
    medida::Counter& mConfigBucketListTargetSizeByte;

    SorobanMetrics(medida::MetricsRegistry& metrics);

    void accumulateLedgerCpuInsn(uint64_t cpuInsn);
    void accumulateLedgerReadEntry(uint64_t readEntry);
    void accumulateLedgerReadByte(uint64_t readByte);
    void accumulateLedgerWriteEntry(uint64_t writeEntry);
    void accumulateLedgerWriteByte(uint64_t writeByte);

    void publishAndResetLedgerWideMetrics();
};
}