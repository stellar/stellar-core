#include "ledger/SorobanMetrics.h"
#include "util/MetricsRegistry.h"

#include <medida/histogram.h>
#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>

namespace stellar
{
void
SorobanApplyMetrics::merge(SorobanApplyMetrics&& other)
{
    auto drain = [](std::vector<int64_t>& dst, std::vector<int64_t>&& src) {
        if (dst.empty())
        {
            dst = std::move(src);
        }
        else
        {
            dst.insert(dst.end(), src.begin(), src.end());
        }
    };

    mHostFnOpReadEntry += other.mHostFnOpReadEntry;
    mHostFnOpWriteEntry += other.mHostFnOpWriteEntry;
    mHostFnOpReadKeyByte += other.mHostFnOpReadKeyByte;
    mHostFnOpWriteKeyByte += other.mHostFnOpWriteKeyByte;
    mHostFnOpReadLedgerByte += other.mHostFnOpReadLedgerByte;
    mHostFnOpReadDataByte += other.mHostFnOpReadDataByte;
    mHostFnOpReadCodeByte += other.mHostFnOpReadCodeByte;
    mHostFnOpWriteLedgerByte += other.mHostFnOpWriteLedgerByte;
    mHostFnOpWriteDataByte += other.mHostFnOpWriteDataByte;
    mHostFnOpWriteCodeByte += other.mHostFnOpWriteCodeByte;
    mHostFnOpEmitEvent += other.mHostFnOpEmitEvent;
    mHostFnOpEmitEventByte += other.mHostFnOpEmitEventByte;
    mHostFnOpCpuInsn += other.mHostFnOpCpuInsn;
    mHostFnOpMemByte += other.mHostFnOpMemByte;
    mHostFnOpCpuInsnExclVm += other.mHostFnOpCpuInsnExclVm;
    mHostFnOpMaxRwKeyByte += other.mHostFnOpMaxRwKeyByte;
    mHostFnOpMaxRwDataByte += other.mHostFnOpMaxRwDataByte;
    mHostFnOpMaxRwCodeByte += other.mHostFnOpMaxRwCodeByte;
    mHostFnOpMaxEmitEventByte += other.mHostFnOpMaxEmitEventByte;
    mHostFnOpSuccess += other.mHostFnOpSuccess;
    mHostFnOpFailure += other.mHostFnOpFailure;
    mExtFpTtlOpReadLedgerByte += other.mExtFpTtlOpReadLedgerByte;
    mRestoreFpOpReadLedgerByte += other.mRestoreFpOpReadLedgerByte;
    mRestoreFpOpWriteLedgerByte += other.mRestoreFpOpWriteLedgerByte;

    mLedgerTxCount += other.mLedgerTxCount;
    mLedgerCpuInsn += other.mLedgerCpuInsn;
    mLedgerTxsSizeByte += other.mLedgerTxsSizeByte;
    mLedgerReadEntry += other.mLedgerReadEntry;
    mLedgerReadByte += other.mLedgerReadByte;
    mLedgerWriteEntry += other.mLedgerWriteEntry;
    mLedgerWriteByte += other.mLedgerWriteByte;
    mLedgerInsnsCount += other.mLedgerInsnsCount;
    mLedgerInsnsExclVmCount += other.mLedgerInsnsExclVmCount;
    mLedgerHostFnExecTimeNsecs += other.mLedgerHostFnExecTimeNsecs;

    drain(mHostFnOpInvokeTimeNsecs, std::move(other.mHostFnOpInvokeTimeNsecs));
    drain(mHostFnOpInvokeTimeNsecsExclVm,
          std::move(other.mHostFnOpInvokeTimeNsecsExclVm));
    drain(mHostFnOpInvokeTimeFsecsCpuInsnRatio,
          std::move(other.mHostFnOpInvokeTimeFsecsCpuInsnRatio));
    drain(mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm,
          std::move(other.mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm));
    drain(mHostFnOpDeclaredInsnsUsageRatio,
          std::move(other.mHostFnOpDeclaredInsnsUsageRatio));
    drain(mHostFnOpExecNsecs, std::move(other.mHostFnOpExecNsecs));
    drain(mExtFpTtlOpExecNsecs, std::move(other.mExtFpTtlOpExecNsecs));
    drain(mRestoreFpOpExecNsecs, std::move(other.mRestoreFpOpExecNsecs));
    drain(mTxSizeByte, std::move(other.mTxSizeByte));
    drain(mTxApplyNsecs, std::move(other.mTxApplyNsecs));
    drain(mOpApplyNsecs, std::move(other.mOpApplyNsecs));
}

SorobanMetricsRegistry::SorobanMetricsRegistry(MetricsRegistry& metrics)
    : /* ledger-wide metrics */
    mLedgerTxCount(metrics.NewHistogram({"soroban", "ledger", "tx-count"}))
    , mLedgerCpuInsn(metrics.NewHistogram({"soroban", "ledger", "cpu-insn"}))
    , mLedgerTxsSizeByte(
          metrics.NewHistogram({"soroban", "ledger", "txs-size-byte"}))
    , mLedgerReadEntry(
          metrics.NewHistogram({"soroban", "ledger", "read-entry"}))
    , mLedgerReadLedgerByte(
          metrics.NewHistogram({"soroban", "ledger", "read-ledger-byte"}))
    , mLedgerWriteEntry(
          metrics.NewHistogram({"soroban", "ledger", "write-entry"}))
    , mLedgerWriteLedgerByte(
          metrics.NewHistogram({"soroban", "ledger", "write-ledger-byte"}))
    , mLedgerHostFnCpuInsnsRatio(metrics.NewHistogram(
          {"soroban", "host-fn-op", "ledger-cpu-insns-ratio"}))
    , mLedgerHostFnCpuInsnsRatioExclVm(metrics.NewHistogram(
          {"soroban", "host-fn-op", "ledger-cpu-insns-ratio-excl-vm"}))

    /* tx-wide metrics */
    , mTxSizeByte(metrics.NewHistogram({"soroban", "tx", "size-byte"}))
    , mTransactionApply(metrics.NewTimer({"ledger", "transaction", "apply"}))
    , mOperationApply(metrics.NewTimer({"ledger", "operation", "apply"}))
    /* InvokeHostFunctionOp metrics */
    , mHostFnOpReadEntry(
          metrics.NewMeter({"soroban", "host-fn-op", "read-entry"}, "entry"))
    , mHostFnOpWriteEntry(
          metrics.NewMeter({"soroban", "host-fn-op", "write-entry"}, "entry"))
    , mHostFnOpReadKeyByte(
          metrics.NewMeter({"soroban", "host-fn-op", "read-key-byte"}, "byte"))
    , mHostFnOpWriteKeyByte(
          metrics.NewMeter({"soroban", "host-fn-op", "write-key-byte"}, "byte"))
    , mHostFnOpReadLedgerByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "read-ledger-byte"}, "byte"))
    , mHostFnOpReadDataByte(
          metrics.NewMeter({"soroban", "host-fn-op", "read-data-byte"}, "byte"))
    , mHostFnOpReadCodeByte(
          metrics.NewMeter({"soroban", "host-fn-op", "read-code-byte"}, "byte"))
    , mHostFnOpWriteLedgerByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "write-ledger-byte"}, "byte"))
    , mHostFnOpWriteDataByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "write-data-byte"}, "byte"))
    , mHostFnOpWriteCodeByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "write-code-byte"}, "byte"))
    , mHostFnOpEmitEvent(
          metrics.NewMeter({"soroban", "host-fn-op", "emit-event"}, "event"))
    , mHostFnOpEmitEventByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "emit-event-byte"}, "byte"))
    , mHostFnOpCpuInsn(
          metrics.NewMeter({"soroban", "host-fn-op", "cpu-insn"}, "insn"))
    , mHostFnOpMemByte(
          metrics.NewMeter({"soroban", "host-fn-op", "mem-byte"}, "byte"))
    , mHostFnOpInvokeTimeNsecs(
          metrics.NewTimer({"soroban", "host-fn-op", "invoke-time-nsecs"}))
    , mHostFnOpCpuInsnExclVm(metrics.NewMeter(
          {"soroban", "host-fn-op", "cpu-insn-excl-vm"}, "insn"))
    , mHostFnOpInvokeTimeNsecsExclVm(metrics.NewTimer(
          {"soroban", "host-fn-op", "invoke-time-nsecs-excl-vm"}))
    , mHostFnOpInvokeTimeFsecsCpuInsnRatio(metrics.NewHistogram(
          {"soroban", "host-fn-op", "invoke-time-fsecs-cpu-insn-ratio"}))
    , mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm(
          metrics.NewHistogram({"soroban", "host-fn-op",
                                "invoke-time-fsecs-cpu-insn-ratio-excl-vm"}))
    , mHostFnOpDeclaredInsnsUsageRatio(metrics.NewHistogram(
          {"soroban", "host-fn-op", "declared-cpu-insns-usage-ratio"}))
    , mHostFnOpMaxRwKeyByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "max-rw-key-byte"}, "byte"))
    , mHostFnOpMaxRwDataByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "max-rw-data-byte"}, "byte"))
    , mHostFnOpMaxRwCodeByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "max-rw-code-byte"}, "byte"))
    , mHostFnOpMaxEmitEventByte(metrics.NewMeter(
          {"soroban", "host-fn-op", "max-emit-event-byte"}, "byte"))
    , mHostFnOpSuccess(
          metrics.NewMeter({"soroban", "host-fn-op", "success"}, "call"))
    , mHostFnOpFailure(
          metrics.NewMeter({"soroban", "host-fn-op", "failure"}, "call"))
    , mHostFnOpExec(metrics.NewTimer({"soroban", "host-fn-op", "exec"}))
    /* ExtendFootprintTTLOp metrics */
    , mExtFpTtlOpReadLedgerByte(metrics.NewMeter(
          {"soroban", "ext-fprint-ttl-op", "read-ledger-byte"}, "byte"))
    , mExtFpTtlOpExec(
          metrics.NewTimer({"soroban", "ext-fprint-ttl-op", "exec"}))
    /* RestoreFootprintOp metrics */
    , mRestoreFpOpReadLedgerByte(metrics.NewMeter(
          {"soroban", "restore-fprint-op", "read-ledger-byte"}, "byte"))
    , mRestoreFpOpWriteLedgerByte(metrics.NewMeter(
          {"soroban", "restore-fprint-op", "write-ledger-byte"}, "byte"))
    , mRestoreFpOpExec(
          metrics.NewTimer({"soroban", "restore-fprint-op", "exec"}))
    /* network config metrics */
    , mConfigContractDataKeySizeBytes(
          metrics.NewCounter({"soroban", "config", "contract-max-rw-key-byte"}))
    , mConfigMaxContractDataEntrySizeBytes(metrics.NewCounter(
          {"soroban", "config", "contract-max-rw-data-byte"}))
    , mConfigMaxContractSizeBytes(metrics.NewCounter(
          {"soroban", "config", "contract-max-rw-code-byte"}))
    , mConfigTxMaxSizeByte(
          metrics.NewCounter({"soroban", "config", "tx-max-size-byte"}))
    , mConfigTxMaxCpuInsn(
          metrics.NewCounter({"soroban", "config", "tx-max-cpu-insn"}))
    , mConfigTxMemoryLimitBytes(
          metrics.NewCounter({"soroban", "config", "tx-max-mem-byte"}))
    , mConfigTxMaxDiskReadEntries(
          metrics.NewCounter({"soroban", "config", "tx-max-read-entry"}))
    , mConfigTxMaxDiskReadBytes(
          metrics.NewCounter({"soroban", "config", "tx-max-read-ledger-byte"}))
    , mConfigTxMaxWriteLedgerEntries(
          metrics.NewCounter({"soroban", "config", "tx-max-write-entry"}))
    , mConfigTxMaxWriteBytes(
          metrics.NewCounter({"soroban", "config", "tx-max-write-ledger-byte"}))
    , mConfigMaxContractEventsSizeBytes(
          metrics.NewCounter({"soroban", "config", "tx-max-emit-event-byte"}))
    , mConfigLedgerMaxTxCount(
          metrics.NewCounter({"soroban", "config", "ledger-max-tx-count"}))
    , mConfigLedgerMaxInstructions(
          metrics.NewCounter({"soroban", "config", "ledger-max-cpu-insn"}))
    , mConfigLedgerMaxTxsSizeByte(
          metrics.NewCounter({"soroban", "config", "ledger-max-txs-size-byte"}))
    , mConfigLedgerMaxDiskReadEntries(
          metrics.NewCounter({"soroban", "config", "ledger-max-read-entry"}))
    , mConfigLedgerMaxDiskReadBytes(metrics.NewCounter(
          {"soroban", "config", "ledger-max-read-ledger-byte"}))
    , mConfigLedgerMaxWriteEntries(
          metrics.NewCounter({"soroban", "config", "ledger-max-write-entry"}))
    , mConfigLedgerMaxWriteBytes(metrics.NewCounter(
          {"soroban", "config", "ledger-max-write-ledger-byte"}))
    , mConfigBucketListTargetSizeByte(metrics.NewCounter(
          {"soroban", "config", "bucket-list-target-size-byte"}))
    , mConfigFeeWrite1KB(
          metrics.NewCounter({"soroban", "config", "fee-write-1kb"}))

    /* Module cache related metrics */
    , mModuleCacheNumEntries(
          metrics.NewCounter({"soroban", "module-cache", "num-entries"}))
    , mModuleCompilationTime(
          metrics.NewTimer({"soroban", "module-cache", "compilation-time"}))
    , mModuleCacheRebuildTime(
          metrics.NewTimer({"soroban", "module-cache", "rebuild-time"}))
    , mModuleCacheRebuildBytes(
          metrics.NewCounter({"soroban", "module-cache", "rebuild-bytes"}))
    , mContractCodeStateSize(metrics.NewCounter(
          {"soroban", "in-memory-state", "contract-code-size"}))
    , mContractDataStateSize(metrics.NewCounter(
          {"soroban", "in-memory-state", "contract-data-size"}))
    , mContractCodeEntryCount(metrics.NewCounter(
          {"soroban", "in-memory-state", "contract-code-entries"}))
    , mContractDataEntryCount(metrics.NewCounter(
          {"soroban", "in-memory-state", "contract-data-entries"}))

{
}

void
SorobanMetricsRegistry::recordApplyMetrics(SorobanApplyMetrics const& metrics)
{
    // Publish into the underlying medida metrics, one bulk call per metric.
    // Zero meter increments are skipped (a Mark(0) does not change any
    // observable value); empty sample metric vectors are no-ops in UpdateMany.
    auto markIf = [](medida::Meter& meter, uint64_t value) {
        if (value != 0)
        {
            meter.Mark(value);
        }
    };
    markIf(mHostFnOpReadEntry, metrics.mHostFnOpReadEntry);
    markIf(mHostFnOpWriteEntry, metrics.mHostFnOpWriteEntry);
    markIf(mHostFnOpReadKeyByte, metrics.mHostFnOpReadKeyByte);
    markIf(mHostFnOpWriteKeyByte, metrics.mHostFnOpWriteKeyByte);
    markIf(mHostFnOpReadLedgerByte, metrics.mHostFnOpReadLedgerByte);
    markIf(mHostFnOpReadDataByte, metrics.mHostFnOpReadDataByte);
    markIf(mHostFnOpReadCodeByte, metrics.mHostFnOpReadCodeByte);
    markIf(mHostFnOpWriteLedgerByte, metrics.mHostFnOpWriteLedgerByte);
    markIf(mHostFnOpWriteDataByte, metrics.mHostFnOpWriteDataByte);
    markIf(mHostFnOpWriteCodeByte, metrics.mHostFnOpWriteCodeByte);
    markIf(mHostFnOpEmitEvent, metrics.mHostFnOpEmitEvent);
    markIf(mHostFnOpEmitEventByte, metrics.mHostFnOpEmitEventByte);
    markIf(mHostFnOpCpuInsn, metrics.mHostFnOpCpuInsn);
    markIf(mHostFnOpMemByte, metrics.mHostFnOpMemByte);
    markIf(mHostFnOpCpuInsnExclVm, metrics.mHostFnOpCpuInsnExclVm);
    markIf(mHostFnOpMaxRwKeyByte, metrics.mHostFnOpMaxRwKeyByte);
    markIf(mHostFnOpMaxRwDataByte, metrics.mHostFnOpMaxRwDataByte);
    markIf(mHostFnOpMaxRwCodeByte, metrics.mHostFnOpMaxRwCodeByte);
    markIf(mHostFnOpMaxEmitEventByte, metrics.mHostFnOpMaxEmitEventByte);
    markIf(mHostFnOpSuccess, metrics.mHostFnOpSuccess);
    markIf(mHostFnOpFailure, metrics.mHostFnOpFailure);
    markIf(mExtFpTtlOpReadLedgerByte, metrics.mExtFpTtlOpReadLedgerByte);
    markIf(mRestoreFpOpReadLedgerByte, metrics.mRestoreFpOpReadLedgerByte);
    markIf(mRestoreFpOpWriteLedgerByte, metrics.mRestoreFpOpWriteLedgerByte);

    mHostFnOpInvokeTimeNsecs.UpdateMany(metrics.mHostFnOpInvokeTimeNsecs);
    mHostFnOpInvokeTimeNsecsExclVm.UpdateMany(
        metrics.mHostFnOpInvokeTimeNsecsExclVm);
    mHostFnOpInvokeTimeFsecsCpuInsnRatio.UpdateMany(
        metrics.mHostFnOpInvokeTimeFsecsCpuInsnRatio);
    mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm.UpdateMany(
        metrics.mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm);
    mHostFnOpDeclaredInsnsUsageRatio.UpdateMany(
        metrics.mHostFnOpDeclaredInsnsUsageRatio);
    mHostFnOpExec.UpdateMany(metrics.mHostFnOpExecNsecs);
    mExtFpTtlOpExec.UpdateMany(metrics.mExtFpTtlOpExecNsecs);
    mRestoreFpOpExec.UpdateMany(metrics.mRestoreFpOpExecNsecs);
    mTxSizeByte.UpdateMany(metrics.mTxSizeByte);
    mTransactionApply.UpdateMany(metrics.mTxApplyNsecs);
    mOperationApply.UpdateMany(metrics.mOpApplyNsecs);

    mLedgerTxCount.Update(metrics.mLedgerTxCount);
    mLedgerCpuInsn.Update(metrics.mLedgerCpuInsn);
    mLedgerTxsSizeByte.Update(metrics.mLedgerTxsSizeByte);
    mLedgerReadEntry.Update(metrics.mLedgerReadEntry);
    mLedgerReadLedgerByte.Update(metrics.mLedgerReadByte);
    mLedgerWriteEntry.Update(metrics.mLedgerWriteEntry);
    mLedgerWriteLedgerByte.Update(metrics.mLedgerWriteByte);
    mLedgerHostFnCpuInsnsRatio.Update(
        metrics.mLedgerHostFnExecTimeNsecs * 1000000 /
        std::max(metrics.mLedgerInsnsCount, uint64_t(1)));
    mLedgerHostFnCpuInsnsRatioExclVm.Update(
        metrics.mLedgerHostFnExecTimeNsecs * 1000000 /
        std::max(metrics.mLedgerInsnsExclVmCount, uint64_t(1)));
}
}
