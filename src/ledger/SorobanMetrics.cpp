#include "ledger/SorobanMetrics.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"

namespace stellar
{
SorobanMetrics::SorobanMetrics(medida::MetricsRegistry& metrics)
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
    /* tx-wide metrics */
    , mTxSizeByte(metrics.NewHistogram({"soroban", "tx", "size-byte"}))
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
    , mConfigContractMaxRwKeyByte(
          metrics.NewCounter({"soroban", "config", "contract-max-rw-key-byte"}))
    , mConfigContractMaxRwDataByte(metrics.NewCounter(
          {"soroban", "config", "contract-max-rw-data-byte"}))
    , mConfigContractMaxRwCodeByte(metrics.NewCounter(
          {"soroban", "config", "contract-max-rw-code-byte"}))
    , mConfigTxMaxSizeByte(
          metrics.NewCounter({"soroban", "config", "tx-max-size-byte"}))
    , mConfigTxMaxCpuInsn(
          metrics.NewCounter({"soroban", "config", "tx-max-cpu-insn"}))
    , mConfigTxMaxMemByte(
          metrics.NewCounter({"soroban", "config", "tx-max-mem-byte"}))
    , mConfigTxMaxReadEntry(
          metrics.NewCounter({"soroban", "config", "tx-max-read-entry"}))
    , mConfigTxMaxReadLedgerByte(
          metrics.NewCounter({"soroban", "config", "tx-max-read-ledger-byte"}))
    , mConfigTxMaxWriteEntry(
          metrics.NewCounter({"soroban", "config", "tx-max-write-entry"}))
    , mConfigTxMaxWriteLedgerByte(
          metrics.NewCounter({"soroban", "config", "tx-max-write-ledger-byte"}))
    , mConfigTxMaxEmitEventByte(
          metrics.NewCounter({"soroban", "config", "tx-max-emit-event-byte"}))
    , mConfigLedgerMaxTxCount(
          metrics.NewCounter({"soroban", "config", "ledger-max-tx-count"}))
    , mConfigLedgerMaxCpuInsn(
          metrics.NewCounter({"soroban", "config", "ledger-max-cpu-insn"}))
    , mConfigLedgerMaxTxsSizeByte(
          metrics.NewCounter({"soroban", "config", "ledger-max-txs-size-byte"}))
    , mConfigLedgerMaxReadEntry(
          metrics.NewCounter({"soroban", "config", "ledger-max-read-entry"}))
    , mConfigLedgerMaxReadLedgerByte(metrics.NewCounter(
          {"soroban", "config", "ledger-max-read-ledger-byte"}))
    , mConfigLedgerMaxWriteEntry(
          metrics.NewCounter({"soroban", "config", "ledger-max-write-entry"}))
    , mConfigLedgerMaxWriteLedgerByte(metrics.NewCounter(
          {"soroban", "config", "ledger-max-write-ledger-byte"}))
    , mConfigBucketListTargetSizeByte(metrics.NewCounter(
          {"soroban", "config", "bucket-list-target-size-byte"}))
{
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

void
SorobanMetrics::publishAndResetLedgerWideMetrics()
{
    mLedgerTxCount.Update(mCounterLedgerTxCount);
    mLedgerCpuInsn.Update(mCounterLedgerCpuInsn);
    mLedgerTxsSizeByte.Update(mCounterLedgerTxsSizeByte);
    mLedgerReadEntry.Update(mCounterLedgerReadEntry);
    mLedgerReadLedgerByte.Update(mCounterLedgerReadByte);
    mLedgerWriteEntry.Update(mCounterLedgerWriteEntry);
    mLedgerWriteLedgerByte.Update(mCounterLedgerWriteByte);

    mCounterLedgerTxCount = 0;
    mCounterLedgerCpuInsn = 0;
    mCounterLedgerTxsSizeByte = 0;
    mCounterLedgerReadEntry = 0;
    mCounterLedgerReadByte = 0;
    mCounterLedgerWriteEntry = 0;
    mCounterLedgerWriteByte = 0;
}
}