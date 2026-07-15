#include "ledger/SorobanMetrics.h"
#include "util/MetricsRegistry.h"

#include <medida/histogram.h>
#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>
#include <unordered_map>

namespace stellar
{
SorobanMetrics::SorobanMetrics(MetricsRegistry& metrics)
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
SorobanMetrics::accumulateModelledCpuInsns(uint64_t insnsCount,
                                           uint64_t insnsExclVmCount,
                                           uint64_t hostFnExecTimeNsecs)
{
    mLedgerInsnsCount += insnsCount;
    mLedgerInsnsExclVmCount += insnsExclVmCount;
    mLedgerHostFnExecTimeNsecs += hostFnExecTimeNsecs;
}

void
SorobanMetrics::accumulateLedgerTxCount(uint64_t txCount)
{
    mCounterLedgerTxCount += txCount;
}
void
SorobanMetrics::accumulateLedgerCpuInsn(uint64_t cpuInsn)
{
    mCounterLedgerCpuInsn += cpuInsn;
}
void
SorobanMetrics::accumulateLedgerTxsSizeByte(uint64_t txsSizeByte)
{
    mCounterLedgerTxsSizeByte += txsSizeByte;
}
void
SorobanMetrics::accumulateLedgerReadEntry(uint64_t readEntry)
{
    mCounterLedgerReadEntry += readEntry;
}
void
SorobanMetrics::accumulateLedgerReadByte(uint64_t readByte)
{
    mCounterLedgerReadByte += readByte;
}
void
SorobanMetrics::accumulateLedgerWriteEntry(uint64_t writeEntry)
{
    mCounterLedgerWriteEntry += writeEntry;
}
void
SorobanMetrics::accumulateLedgerWriteByte(uint64_t writeByte)
{
    mCounterLedgerWriteByte += writeByte;
}

SorobanMetrics::ApplyMetricsBatch&
SorobanMetrics::getApplyThreadBatch()
{
    std::lock_guard<std::mutex> lock(mApplyBatchesMutex);
    auto [it, inserted] = mApplyBatches.try_emplace(std::this_thread::get_id());
    if (inserted)
    {
        it->second = std::make_unique<ApplyMetricsBatch>();
    }
    return *it->second;
}

void
SorobanMetrics::flushApplyMetricsBatches()
{
    std::vector<std::unique_ptr<ApplyMetricsBatch>> batches;
    {
        std::lock_guard<std::mutex> lock(mApplyBatchesMutex);
        if (mApplyBatches.empty())
        {
            return;
        }
        batches.reserve(mApplyBatches.size());
        for (auto& [_, v] : mApplyBatches)
        {
            batches.emplace_back(std::move(v));
        }
        mApplyBatches.clear();
    }

    ApplyMetricsBatch total;
    auto take = [](uint64_t& v) {
        auto res = v;
        v = 0;
        return res;
    };
    auto drain = [](std::vector<int64_t>& dst, std::vector<int64_t>& src) {
        dst.insert(dst.end(), src.begin(), src.end());
        src.clear();
    };
    for (auto const& b : batches)
    {
        std::lock_guard<std::mutex> lock(b->mMutex);
        total.mHostFnOpReadEntry += take(b->mHostFnOpReadEntry);
        total.mHostFnOpWriteEntry += take(b->mHostFnOpWriteEntry);
        total.mHostFnOpReadKeyByte += take(b->mHostFnOpReadKeyByte);
        total.mHostFnOpWriteKeyByte += take(b->mHostFnOpWriteKeyByte);
        total.mHostFnOpReadLedgerByte += take(b->mHostFnOpReadLedgerByte);
        total.mHostFnOpReadDataByte += take(b->mHostFnOpReadDataByte);
        total.mHostFnOpReadCodeByte += take(b->mHostFnOpReadCodeByte);
        total.mHostFnOpWriteLedgerByte += take(b->mHostFnOpWriteLedgerByte);
        total.mHostFnOpWriteDataByte += take(b->mHostFnOpWriteDataByte);
        total.mHostFnOpWriteCodeByte += take(b->mHostFnOpWriteCodeByte);
        total.mHostFnOpEmitEvent += take(b->mHostFnOpEmitEvent);
        total.mHostFnOpEmitEventByte += take(b->mHostFnOpEmitEventByte);
        total.mHostFnOpCpuInsn += take(b->mHostFnOpCpuInsn);
        total.mHostFnOpMemByte += take(b->mHostFnOpMemByte);
        total.mHostFnOpCpuInsnExclVm += take(b->mHostFnOpCpuInsnExclVm);
        total.mHostFnOpMaxRwKeyByte += take(b->mHostFnOpMaxRwKeyByte);
        total.mHostFnOpMaxRwDataByte += take(b->mHostFnOpMaxRwDataByte);
        total.mHostFnOpMaxRwCodeByte += take(b->mHostFnOpMaxRwCodeByte);
        total.mHostFnOpMaxEmitEventByte += take(b->mHostFnOpMaxEmitEventByte);
        total.mHostFnOpSuccess += take(b->mHostFnOpSuccess);
        total.mHostFnOpFailure += take(b->mHostFnOpFailure);
        total.mExtFpTtlOpReadLedgerByte += take(b->mExtFpTtlOpReadLedgerByte);
        total.mRestoreFpOpReadLedgerByte += take(b->mRestoreFpOpReadLedgerByte);
        total.mRestoreFpOpWriteLedgerByte +=
            take(b->mRestoreFpOpWriteLedgerByte);

        drain(total.mHostFnOpInvokeTimeNsecs, b->mHostFnOpInvokeTimeNsecs);
        drain(total.mHostFnOpInvokeTimeNsecsExclVm,
              b->mHostFnOpInvokeTimeNsecsExclVm);
        drain(total.mHostFnOpInvokeTimeFsecsCpuInsnRatio,
              b->mHostFnOpInvokeTimeFsecsCpuInsnRatio);
        drain(total.mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm,
              b->mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm);
        drain(total.mHostFnOpDeclaredInsnsUsageRatio,
              b->mHostFnOpDeclaredInsnsUsageRatio);
        drain(total.mHostFnOpExecNsecs, b->mHostFnOpExecNsecs);
        drain(total.mExtFpTtlOpExecNsecs, b->mExtFpTtlOpExecNsecs);
        drain(total.mRestoreFpOpExecNsecs, b->mRestoreFpOpExecNsecs);
        drain(total.mTxSizeByte, b->mTxSizeByte);
        drain(total.mTxApplyNsecs, b->mTxApplyNsecs);
        drain(total.mOpApplyNsecs, b->mOpApplyNsecs);
    }

    // Publish into the underlying medida metrics, one bulk call per metric.
    // Zero meter increments are skipped (a Mark(0) does not change any
    // observable value); empty sample batches are no-ops in UpdateMany.
    auto markIf = [](medida::Meter& meter, uint64_t value) {
        if (value != 0)
        {
            meter.Mark(value);
        }
    };
    markIf(mHostFnOpReadEntry, total.mHostFnOpReadEntry);
    markIf(mHostFnOpWriteEntry, total.mHostFnOpWriteEntry);
    markIf(mHostFnOpReadKeyByte, total.mHostFnOpReadKeyByte);
    markIf(mHostFnOpWriteKeyByte, total.mHostFnOpWriteKeyByte);
    markIf(mHostFnOpReadLedgerByte, total.mHostFnOpReadLedgerByte);
    markIf(mHostFnOpReadDataByte, total.mHostFnOpReadDataByte);
    markIf(mHostFnOpReadCodeByte, total.mHostFnOpReadCodeByte);
    markIf(mHostFnOpWriteLedgerByte, total.mHostFnOpWriteLedgerByte);
    markIf(mHostFnOpWriteDataByte, total.mHostFnOpWriteDataByte);
    markIf(mHostFnOpWriteCodeByte, total.mHostFnOpWriteCodeByte);
    markIf(mHostFnOpEmitEvent, total.mHostFnOpEmitEvent);
    markIf(mHostFnOpEmitEventByte, total.mHostFnOpEmitEventByte);
    markIf(mHostFnOpCpuInsn, total.mHostFnOpCpuInsn);
    markIf(mHostFnOpMemByte, total.mHostFnOpMemByte);
    markIf(mHostFnOpCpuInsnExclVm, total.mHostFnOpCpuInsnExclVm);
    markIf(mHostFnOpMaxRwKeyByte, total.mHostFnOpMaxRwKeyByte);
    markIf(mHostFnOpMaxRwDataByte, total.mHostFnOpMaxRwDataByte);
    markIf(mHostFnOpMaxRwCodeByte, total.mHostFnOpMaxRwCodeByte);
    markIf(mHostFnOpMaxEmitEventByte, total.mHostFnOpMaxEmitEventByte);
    markIf(mHostFnOpSuccess, total.mHostFnOpSuccess);
    markIf(mHostFnOpFailure, total.mHostFnOpFailure);
    markIf(mExtFpTtlOpReadLedgerByte, total.mExtFpTtlOpReadLedgerByte);
    markIf(mRestoreFpOpReadLedgerByte, total.mRestoreFpOpReadLedgerByte);
    markIf(mRestoreFpOpWriteLedgerByte, total.mRestoreFpOpWriteLedgerByte);

    mHostFnOpInvokeTimeNsecs.UpdateMany(total.mHostFnOpInvokeTimeNsecs);
    mHostFnOpInvokeTimeNsecsExclVm.UpdateMany(
        total.mHostFnOpInvokeTimeNsecsExclVm);
    mHostFnOpInvokeTimeFsecsCpuInsnRatio.UpdateMany(
        total.mHostFnOpInvokeTimeFsecsCpuInsnRatio);
    mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm.UpdateMany(
        total.mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm);
    mHostFnOpDeclaredInsnsUsageRatio.UpdateMany(
        total.mHostFnOpDeclaredInsnsUsageRatio);
    mHostFnOpExec.UpdateMany(total.mHostFnOpExecNsecs);
    mExtFpTtlOpExec.UpdateMany(total.mExtFpTtlOpExecNsecs);
    mRestoreFpOpExec.UpdateMany(total.mRestoreFpOpExecNsecs);
    mTxSizeByte.UpdateMany(total.mTxSizeByte);
    mTransactionApply.UpdateMany(total.mTxApplyNsecs);
    mOperationApply.UpdateMany(total.mOpApplyNsecs);
}

void
SorobanMetrics::publishAndResetLedgerWideMetrics()
{
    flushApplyMetricsBatches();

    mLedgerTxCount.Update(mCounterLedgerTxCount);
    mLedgerCpuInsn.Update(mCounterLedgerCpuInsn);
    mLedgerTxsSizeByte.Update(mCounterLedgerTxsSizeByte);
    mLedgerReadEntry.Update(mCounterLedgerReadEntry);
    mLedgerReadLedgerByte.Update(mCounterLedgerReadByte);
    mLedgerWriteEntry.Update(mCounterLedgerWriteEntry);
    mLedgerWriteLedgerByte.Update(mCounterLedgerWriteByte);
    mLedgerHostFnCpuInsnsRatio.Update(
        mLedgerHostFnExecTimeNsecs * 1000000 /
        std::max(mLedgerInsnsCount.load(), uint64_t(1)));

    mLedgerHostFnCpuInsnsRatioExclVm.Update(
        mLedgerHostFnExecTimeNsecs * 1000000 /
        std::max(mLedgerInsnsExclVmCount.load(), uint64_t(1)));

    mCounterLedgerTxCount = 0;
    mCounterLedgerCpuInsn = 0;
    mCounterLedgerTxsSizeByte = 0;
    mCounterLedgerReadEntry = 0;
    mCounterLedgerReadByte = 0;
    mCounterLedgerWriteEntry = 0;
    mCounterLedgerWriteByte = 0;
    mLedgerHostFnExecTimeNsecs = 0;
    mLedgerInsnsCount = 0;
    mLedgerInsnsExclVmCount = 0;
}
}
