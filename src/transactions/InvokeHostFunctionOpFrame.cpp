// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
#include "rust/RustVecXdrMarshal.h"
#include "TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-ledger-entries.h"
#include <cstdint>
#include <json/json.h>
#include <medida/metrics_registry.h>
#include <xdrpp/types.h>
#include "xdr/Stellar-contract.h"
// clang-format on

#include "ledger/LedgerTxnImpl.h"
#include "rust/CppShims.h"
#include "xdr/Stellar-transaction.h"
#include <stdexcept>
#include <xdrpp/xdrpp/printer.h>

#include "ledger/LedgerManagerImpl.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTypeUtils.h"
#include "rust/RustBridge.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include "transactions/MutableTransactionResult.h"
#include <Tracy.hpp>
#include <crypto/SHA.h>

namespace stellar
{
namespace
{
CxxLedgerInfo
getLedgerInfo(SorobanNetworkConfig const& sorobanConfig, uint32_t ledgerVersion,
              uint32_t ledgerSeq, uint32_t baseReserve, TimePoint closeTime,
              Hash const& networkID)
{
    CxxLedgerInfo info{};
    info.base_reserve = baseReserve;
    info.protocol_version = ledgerVersion;
    info.sequence_number = ledgerSeq;
    info.timestamp = closeTime;
    info.memory_limit = sorobanConfig.txMemoryLimit();
    info.min_persistent_entry_ttl =
        sorobanConfig.stateArchivalSettings().minPersistentTTL;
    info.min_temp_entry_ttl =
        sorobanConfig.stateArchivalSettings().minTemporaryTTL;
    info.max_entry_ttl = sorobanConfig.stateArchivalSettings().maxEntryTTL;

    auto cpu = sorobanConfig.cpuCostParams();
    auto mem = sorobanConfig.memCostParams();

    info.cpu_cost_params = toCxxBuf(cpu);
    info.mem_cost_params = toCxxBuf(mem);

    info.network_id.reserve(networkID.size());
    for (auto c : networkID)
    {
        info.network_id.emplace_back(static_cast<unsigned char>(c));
    }
    return info;
}

CxxLedgerInfo
getLedgerInfo(SorobanNetworkConfig const& sorobanConfig,
              ParallelLedgerInfo const& parallelLedgerInfo)
{
    return getLedgerInfo(
        sorobanConfig, parallelLedgerInfo.getLedgerVersion(),
        parallelLedgerInfo.getLedgerSeq(), parallelLedgerInfo.getBaseReserve(),
        parallelLedgerInfo.getCloseTime(), parallelLedgerInfo.getNetworkID());
}

DiagnosticEvent
metricsEvent(bool success, std::string&& topic, uint64_t value)
{
    DiagnosticEvent de;
    de.inSuccessfulContractCall = success;
    de.event.type = ContractEventType::DIAGNOSTIC;
    SCVec topics = {
        makeSymbolSCVal("core_metrics"),
        makeSymbolSCVal(std::move(topic)),
    };
    de.event.body.v0().topics = topics;
    de.event.body.v0().data = makeU64SCVal(value);
    return de;
}

} // namespace

struct ReadEntryCounters
{
    uint32_t mReadEntry{0};
    uint32_t mLedgerReadByte{0};
    uint32_t mReadKeyByte{0};
    uint32_t mReadDataByte{0};
    uint32_t mReadCodeByte{0};

    // max single entity size metrics
    uint32_t mMaxReadWriteKeyByte{0};
    uint32_t mMaxReadWriteCodeByte{0};
    uint32_t mMaxReadWriteDataByte{0};

    void
    noteReadEntry(bool isCodeEntry, uint32_t keySize, uint32_t entrySize)
    {
        mReadEntry++;
        mReadKeyByte += keySize;
        mMaxReadWriteKeyByte = std::max(mMaxReadWriteKeyByte, keySize);
        mLedgerReadByte += entrySize;
        if (isCodeEntry)
        {
            mReadCodeByte += entrySize;
            mMaxReadWriteCodeByte = std::max(mMaxReadWriteCodeByte, entrySize);
        }
        else
        {
            mReadDataByte += entrySize;
            mMaxReadWriteDataByte = std::max(mMaxReadWriteDataByte, entrySize);
        }
    }
};

struct HostFunctionMetrics
{
    SorobanMetrics& mMetrics;

    uint32_t mWriteEntry{0};
    uint32_t mLedgerWriteByte{0};
    uint32_t mWriteKeyByte{0};
    uint32_t mWriteDataByte{0};
    uint32_t mWriteCodeByte{0};

    uint32_t mEmitEvent{0};
    uint32_t mEmitEventByte{0};

    // host runtime metrics
    uint64_t mCpuInsn{0};
    uint64_t mMemByte{0};
    uint64_t mInvokeTimeNsecs{0};
    uint64_t mCpuInsnExclVm{0};
    uint64_t mInvokeTimeNsecsExclVm{0};
    uint64_t mDeclaredCpuInsn{0};

    uint32_t mMaxEmitEventByte{0};

    ReadEntryCounters mReadEntryCounters;

    bool mSuccess{false};

    HostFunctionMetrics(SorobanMetrics& metrics) : mMetrics(metrics)
    {
    }

    void
    noteWriteEntry(bool isCodeEntry, uint32_t keySize, uint32_t entrySize)
    {
        mWriteEntry++;
        mReadEntryCounters.mMaxReadWriteKeyByte =
            std::max(mReadEntryCounters.mMaxReadWriteKeyByte, keySize);
        mLedgerWriteByte += entrySize;
        if (isCodeEntry)
        {
            mWriteCodeByte += entrySize;
            mReadEntryCounters.mMaxReadWriteCodeByte =
                std::max(mReadEntryCounters.mMaxReadWriteCodeByte, entrySize);
        }
        else
        {
            mWriteDataByte += entrySize;
            mReadEntryCounters.mMaxReadWriteDataByte =
                std::max(mReadEntryCounters.mMaxReadWriteDataByte, entrySize);
        }
    }

    ~HostFunctionMetrics()
    {
        mMetrics.mHostFnOpReadEntry.Mark(mReadEntryCounters.mReadEntry);

        mMetrics.mHostFnOpReadKeyByte.Mark(mReadEntryCounters.mReadKeyByte);
        mMetrics.mHostFnOpReadLedgerByte.Mark(
            mReadEntryCounters.mLedgerReadByte);
        mMetrics.mHostFnOpReadDataByte.Mark(mReadEntryCounters.mReadDataByte);
        mMetrics.mHostFnOpReadCodeByte.Mark(mReadEntryCounters.mReadCodeByte);

        mMetrics.mHostFnOpMaxRwKeyByte.Mark(
            mReadEntryCounters.mMaxReadWriteKeyByte);
        mMetrics.mHostFnOpMaxRwDataByte.Mark(
            mReadEntryCounters.mMaxReadWriteDataByte);
        mMetrics.mHostFnOpMaxRwCodeByte.Mark(
            mReadEntryCounters.mMaxReadWriteCodeByte);

        mMetrics.mHostFnOpWriteEntry.Mark(mWriteEntry);

        mMetrics.mHostFnOpWriteKeyByte.Mark(mWriteKeyByte);

        mMetrics.mHostFnOpWriteLedgerByte.Mark(mLedgerWriteByte);
        mMetrics.mHostFnOpWriteDataByte.Mark(mWriteDataByte);
        mMetrics.mHostFnOpWriteCodeByte.Mark(mWriteCodeByte);

        mMetrics.mHostFnOpEmitEvent.Mark(mEmitEvent);
        mMetrics.mHostFnOpEmitEventByte.Mark(mEmitEventByte);

        mMetrics.mHostFnOpCpuInsn.Mark(mCpuInsn);
        mMetrics.mHostFnOpMemByte.Mark(mMemByte);
        mMetrics.mHostFnOpInvokeTimeNsecs.Update(
            std::chrono::nanoseconds(mInvokeTimeNsecs));
        mMetrics.mHostFnOpCpuInsnExclVm.Mark(mCpuInsnExclVm);
        mMetrics.mHostFnOpInvokeTimeNsecsExclVm.Update(
            std::chrono::nanoseconds(mInvokeTimeNsecsExclVm));
        mMetrics.mHostFnOpInvokeTimeFsecsCpuInsnRatio.Update(
            mInvokeTimeNsecs * 1000000 / std::max(mCpuInsn, uint64_t(1)));
        mMetrics.mHostFnOpInvokeTimeFsecsCpuInsnRatioExclVm.Update(
            mInvokeTimeNsecsExclVm * 1000000 /
            std::max(mCpuInsnExclVm, uint64_t(1)));
        mMetrics.mHostFnOpDeclaredInsnsUsageRatio.Update(
            mCpuInsn * 1000000 / std::max(mDeclaredCpuInsn, uint64_t(1)));

        mMetrics.mHostFnOpMaxEmitEventByte.Mark(mMaxEmitEventByte);

        mMetrics.accumulateModelledCpuInsns(mCpuInsn, mCpuInsnExclVm,
                                            mInvokeTimeNsecs);

        if (mSuccess)
        {
            mMetrics.mHostFnOpSuccess.Mark();
        }
        else
        {
            mMetrics.mHostFnOpFailure.Mark();
        }
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.mHostFnOpExec.TimeScope();
    }
};

InvokeHostFunctionOpFrame::InvokeHostFunctionOpFrame(
    Operation const& op, TransactionFrame const& parentTx)
    : OperationFrame(op, parentTx)
    , mInvokeHostFunction(mOperation.body.invokeHostFunctionOp())
{
}

bool
InvokeHostFunctionOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

void
InvokeHostFunctionOpFrame::maybePopulateDiagnosticEvents(
    Config const& cfg, InvokeHostFunctionOutput const& output,
    HostFunctionMetrics const& metrics, DiagnosticEventBuffer& buffer) const
{
    if (cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS)
    {
        xdr::xvector<DiagnosticEvent> diagnosticEvents;
        diagnosticEvents.reserve(output.diagnostic_events.size() + 20);
        for (auto const& e : output.diagnostic_events)
        {
            DiagnosticEvent evt;
            xdr::xdr_from_opaque(e.data, evt);
            diagnosticEvents.emplace_back(evt);
            CLOG_DEBUG(Tx, "Soroban diagnostic event: {}",
                       xdr::xdr_to_string(evt));
        }

        // add additional diagnostic events for metrics
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "read_entry",
                         metrics.mReadEntryCounters.mReadEntry));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "write_entry", metrics.mWriteEntry));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "ledger_read_byte",
                         metrics.mReadEntryCounters.mLedgerReadByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "ledger_write_byte", metrics.mLedgerWriteByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "read_key_byte",
                         metrics.mReadEntryCounters.mReadKeyByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "write_key_byte", metrics.mWriteKeyByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "read_data_byte",
                         metrics.mReadEntryCounters.mReadDataByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "write_data_byte", metrics.mWriteDataByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "read_code_byte",
                         metrics.mReadEntryCounters.mReadCodeByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "write_code_byte", metrics.mWriteCodeByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "emit_event", metrics.mEmitEvent));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "emit_event_byte", metrics.mEmitEventByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "cpu_insn", metrics.mCpuInsn));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "mem_byte", metrics.mMemByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "invoke_time_nsecs", metrics.mInvokeTimeNsecs));
        // skip publishing `cpu_insn_excl_vm` and `invoke_time_nsecs_excl_vm`,
        // we are mostly interested in those internally
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "max_rw_key_byte",
                         metrics.mReadEntryCounters.mMaxReadWriteKeyByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "max_rw_data_byte",
                         metrics.mReadEntryCounters.mMaxReadWriteDataByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "max_rw_code_byte",
                         metrics.mReadEntryCounters.mMaxReadWriteCodeByte));
        diagnosticEvents.emplace_back(metricsEvent(metrics.mSuccess,
                                                   "max_emit_event_byte",
                                                   metrics.mMaxEmitEventByte));

        buffer.pushDiagnosticEvents(diagnosticEvents);
    }
}

bool
InvokeHostFunctionOpFrame::doPreloadEntriesForParallelApply(
    AppConnector& app, SorobanMetrics& sorobanMetrics, AbstractLedgerTxn& ltx,
    ThreadEntryMap& entryMap, OperationResult& res,
    DiagnosticEventBuffer& buffer) const
{
    ReadEntryCounters readEntryCounters;

    auto const& resources = mParentTx.sorobanResources();
    auto config = app.getConfig();
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;

    auto getEntries = [&](xdr::xvector<LedgerKey> const& keys) -> bool {
        for (auto const& lk : keys)
        {
            uint32_t entrySize = 0u;

            if (isSorobanEntry(lk))
            {
                auto ttlKey = getTTLKey(lk);
                auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
                if (ttlLtxe)
                {
                    if (isLive(ttlLtxe.current(), ledgerSeq))
                    {
                        entryMap.emplace(ttlKey,
                                         ThreadEntry{ttlLtxe.current(), false});

                        auto ltxe = ltx.loadWithoutRecord(lk);
                        entrySize = static_cast<uint32_t>(
                            xdr::xdr_size(ltxe.current()));
                        entryMap.emplace(lk,
                                         ThreadEntry{ltxe.current(), false});
                    }
                    else if (isPersistentEntry(lk))
                    {
                        entryMap.emplace(ttlKey,
                                         ThreadEntry{ttlLtxe.current(), false});
                    }
                    else
                    {
                        // Temp entry is expired, so treat the TTL as if it
                        // doesn't exist
                        entryMap.emplace(ttlKey,
                                         ThreadEntry{std::nullopt, false});
                        entryMap.emplace(lk, ThreadEntry{std::nullopt, false});
                    }

                    // We aren't adding the entry key if it isn't live. This
                    // means we will not try to access the entry after this
                    // point for this transaction.
                }
                else
                {
                    entryMap.emplace(ttlKey, ThreadEntry{std::nullopt, false});
                    entryMap.emplace(lk, ThreadEntry{std::nullopt, false});
                }
            }
            else
            {
                auto ltxe = ltx.loadWithoutRecord(lk);
                if (ltxe)
                {
                    entrySize =
                        static_cast<uint32_t>(xdr::xdr_size(ltxe.current()));
                    entryMap.emplace(lk, ThreadEntry{ltxe.current(), false});
                }
            }

            uint32_t keySize = static_cast<uint32_t>(xdr::xdr_size(lk));
            readEntryCounters.noteReadEntry(isContractCodeEntry(lk), keySize,
                                            entrySize);

            if (resources.diskReadBytes < readEntryCounters.mLedgerReadByte)
            {
                innerResult(res).code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);

                buffer.pushApplyTimeDiagnosticError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-read mresources exceeds amount specified",
                    {makeU64SCVal(readEntryCounters.mLedgerReadByte),
                     makeU64SCVal(resources.diskReadBytes)});

                // Only mark on failure because we'll also count read bytes
                // during apply to also count anything read from the hot
                // archive.
                sorobanMetrics.mHostFnOpReadEntry.Mark(
                    readEntryCounters.mReadEntry);

                sorobanMetrics.mHostFnOpReadKeyByte.Mark(
                    readEntryCounters.mReadKeyByte);
                sorobanMetrics.mHostFnOpReadLedgerByte.Mark(
                    readEntryCounters.mLedgerReadByte);
                sorobanMetrics.mHostFnOpReadDataByte.Mark(
                    readEntryCounters.mReadDataByte);
                sorobanMetrics.mHostFnOpReadCodeByte.Mark(
                    readEntryCounters.mReadCodeByte);

                sorobanMetrics.mHostFnOpMaxRwKeyByte.Mark(
                    readEntryCounters.mMaxReadWriteKeyByte);
                sorobanMetrics.mHostFnOpMaxRwDataByte.Mark(
                    readEntryCounters.mMaxReadWriteDataByte);
                sorobanMetrics.mHostFnOpMaxRwCodeByte.Mark(
                    readEntryCounters.mMaxReadWriteCodeByte);

                return false;
            }
        }
        return true;
    };
    bool success = getEntries(mParentTx.sorobanResources().footprint.readOnly);
    if (success)
    {
        success = getEntries(mParentTx.sorobanResources().footprint.readWrite);
    }
    return success;
}

ParallelTxReturnVal
InvokeHostFunctionOpFrame::doParallelApply(
    AppConnector& app,
    ThreadEntryMap const& entryMap, // Must not be shared between threads
    Config const& appConfig, SorobanNetworkConfig const& sorobanConfig,
    Hash const& sorobanBasePrngSeed, ParallelLedgerInfo const& ledgerInfo,
    SorobanMetrics& sorobanMetrics, OperationResult& res,
    SorobanTxData& sorobanData, OpEventManager& opEventManager) const
{
    ZoneNamedN(applyZone, "InvokeHostFunctionOpFrame doParallelApply", true);

    std::vector<LedgerEntryChange> changes;

    HostFunctionMetrics metrics(sorobanMetrics);
    auto timeScope = metrics.getExecTimer();

    // Get the entries for the footprint
    rust::Vec<CxxBuf> ledgerEntryCxxBufs;
    rust::Vec<CxxBuf> ttlEntryCxxBufs;

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    auto footprintLength =
        footprint.readOnly.size() + footprint.readWrite.size();
    auto hotArchive = app.copySearchableHotArchiveBucketListSnapshot();

    ledgerEntryCxxBufs.reserve(footprintLength);
    ttlEntryCxxBufs.reserve(footprintLength);

    auto& diagnosticEvents = opEventManager.getDiagnosticEventsBuffer();
    auto addReads = [&hotArchive, &metrics, &ledgerEntryCxxBufs,
                     &ttlEntryCxxBufs, &entryMap, &sorobanConfig, &appConfig,
                     &diagnosticEvents, &sorobanData, &res, &ledgerInfo,
                     this](auto const& keys) -> bool {
        for (auto const& lk : keys)
        {
            uint32_t entrySize = 0u;
            std::optional<TTLEntry> ttlEntry;
            bool sorobanEntryLive = false;

            // For soroban entries, check if the entry is expired before loading
            if (isSorobanEntry(lk))
            {
                auto ttlKey = getTTLKey(lk);
                auto ttlIter = entryMap.find(ttlKey);
                if (ttlIter != entryMap.end() && ttlIter->second.mLedgerEntry)
                {
                    if (!isLive(*(ttlIter->second.mLedgerEntry),
                                ledgerInfo.getLedgerSeq()))
                    {
                        // For temporary entries, treat the expired entry as
                        // if the key did not exist
                        if (!isTemporaryEntry(lk))
                        {
                            if (lk.type() == CONTRACT_CODE)
                            {
                                diagnosticEvents.pushApplyTimeDiagnosticError(
                                    SCE_VALUE, SCEC_INVALID_INPUT,
                                    "trying to access an archived contract "
                                    "code "
                                    "entry",
                                    {makeBytesSCVal(lk.contractCode().hash)});
                            }
                            else if (lk.type() == CONTRACT_DATA)
                            {
                                diagnosticEvents.pushApplyTimeDiagnosticError(
                                    SCE_VALUE, SCEC_INVALID_INPUT,
                                    "trying to access an archived contract "
                                    "data "
                                    "entry",
                                    {makeAddressSCVal(
                                         lk.contractData().contract),
                                     lk.contractData().key});
                            }
                            // Cannot access an archived entry
                            this->innerResult(res).code(
                                INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                            return false;
                        }
                    }
                    else
                    {
                        sorobanEntryLive = true;
                        ttlEntry =
                            ttlIter->second.mLedgerEntry.value().data.ttl();
                    }
                }
                // Starting in protocol 23, we must check the Hot Archive for
                // new keys. If a new key is actually archived, fail the op.
                else if (isPersistentEntry(lk) &&
                         protocolVersionStartsFrom(
                             ledgerInfo.getLedgerVersion(),
                             HotArchiveBucket::
                                 FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                {
                    auto archiveEntry = hotArchive->load(lk);
                    if (archiveEntry)
                    {
                        if (lk.type() == CONTRACT_CODE)
                        {
                            diagnosticEvents.pushApplyTimeDiagnosticError(
                                SCE_VALUE, SCEC_INVALID_INPUT,
                                "trying to access an archived contract code "
                                "entry",
                                {makeBytesSCVal(lk.contractCode().hash)});
                        }
                        else if (lk.type() == CONTRACT_DATA)
                        {
                            diagnosticEvents.pushApplyTimeDiagnosticError(
                                SCE_VALUE, SCEC_INVALID_INPUT,
                                "trying to access an archived contract data "
                                "entry",
                                {makeAddressSCVal(lk.contractData().contract),
                                 lk.contractData().key});
                        }
                        // Cannot access an archived entry
                        this->innerResult(res).code(
                            INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                        return false;
                    }
                }
            }

            if (!isSorobanEntry(lk) || sorobanEntryLive)
            {
                auto entryIter = entryMap.find(lk);
                if (entryIter != entryMap.end() &&
                    entryIter->second.mLedgerEntry)
                {
                    auto leBuf = toCxxBuf(*(entryIter->second.mLedgerEntry));
                    entrySize = static_cast<uint32_t>(leBuf.data->size());

                    // For entry types that don't have an ttlEntry (i.e.
                    // Accounts), the rust host expects an "empty" CxxBuf such
                    // that the buffer has a non-null pointer that points to an
                    // empty byte vector
                    auto ttlBuf =
                        ttlEntry
                            ? toCxxBuf(*ttlEntry)
                            : CxxBuf{std::make_unique<std::vector<uint8_t>>()};

                    ledgerEntryCxxBufs.emplace_back(std::move(leBuf));
                    ttlEntryCxxBufs.emplace_back(std::move(ttlBuf));
                }
                else if (isSorobanEntry(lk))
                {
                    releaseAssertOrThrow(!ttlEntry);
                }
            }

            // Only used for diagnostic events because loads are done in
            // doPreloadEntriesForParallelApply
            uint32_t keySize = static_cast<uint32_t>(xdr::xdr_size(lk));
            metrics.mReadEntryCounters.noteReadEntry(isContractCodeEntry(lk),
                                                     keySize, entrySize);

            if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                             appConfig, mParentTx,
                                             diagnosticEvents))
            {
                this->innerResult(res).code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }
        }
        return true;
    };

    if (!addReads(footprint.readOnly))
    {
        // Error code set in addReads
        return {false, {}};
    }

    if (!addReads(footprint.readWrite))
    {
        // Error code set in addReads
        return {false, {}};
    }

    rust::Vec<CxxBuf> authEntryCxxBufs;
    authEntryCxxBufs.reserve(mInvokeHostFunction.auth.size());
    for (auto const& authEntry : mInvokeHostFunction.auth)
    {
        authEntryCxxBufs.emplace_back(toCxxBuf(authEntry));
    }

    InvokeHostFunctionOutput out{};
    out.success = false;
    try
    {
        CxxBuf basePrngSeedBuf{};
        basePrngSeedBuf.data = std::make_unique<std::vector<uint8_t>>();
        basePrngSeedBuf.data->assign(sorobanBasePrngSeed.begin(),
                                     sorobanBasePrngSeed.end());
        auto moduleCache = app.getModuleCache();

        out = rust_bridge::invoke_host_function(
            appConfig.CURRENT_LEDGER_PROTOCOL_VERSION,
            appConfig.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS, resources.instructions,
            toCxxBuf(mInvokeHostFunction.hostFunction), toCxxBuf(resources),
            toCxxBuf(getSourceID()), authEntryCxxBufs,
            getLedgerInfo(sorobanConfig, ledgerInfo), ledgerEntryCxxBufs,
            ttlEntryCxxBufs, basePrngSeedBuf,
            sorobanConfig.rustBridgeRentFeeConfiguration(), *moduleCache);
        metrics.mCpuInsn = out.cpu_insns;
        metrics.mMemByte = out.mem_bytes;
        metrics.mInvokeTimeNsecs = out.time_nsecs;
        metrics.mCpuInsnExclVm = out.cpu_insns_excluding_vm_instantiation;
        metrics.mInvokeTimeNsecsExclVm =
            out.time_nsecs_excluding_vm_instantiation;
        if (!out.success)
        {
            maybePopulateDiagnosticEvents(appConfig, out, metrics,
                                          diagnosticEvents);
        }
    }
    catch (std::exception& e)
    {
        // Host invocations should never throw an exception, so encountering
        // one would be an internal error.
        out.is_internal_error = true;
        CLOG_DEBUG(Tx, "Exception caught while invoking host fn: {}", e.what());
    }

    if (!out.success)
    {
        if (out.is_internal_error)
        {
            throw std::runtime_error(
                "Got internal error during Soroban host invocation.");
        }
        if (resources.instructions < out.cpu_insns)
        {
            diagnosticEvents.pushApplyTimeDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation instructions exceeds amount specified",
                {makeU64SCVal(out.cpu_insns),
                 makeU64SCVal(resources.instructions)});

            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
        else if (sorobanConfig.txMemoryLimit() < out.mem_bytes)
        {
            diagnosticEvents.pushApplyTimeDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation memory usage exceeds network config limit",
                {makeU64SCVal(out.mem_bytes),
                 makeU64SCVal(sorobanConfig.txMemoryLimit())});
            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
        else
        {
            innerResult(res).code(INVOKE_HOST_FUNCTION_TRAPPED);
        }
        return {false, {}};
    }

    // Keep track of updates we need to make
    ModifiedEntryMap opEntryMap;

    // Create or update every entry returned.
    UnorderedSet<LedgerKey> createdAndModifiedKeys;
    UnorderedSet<LedgerKey> createdKeys;
    for (auto const& buf : out.modified_ledger_entries)
    {
        LedgerEntry le;
        xdr::xdr_from_opaque(buf.data, le);
        if (!validateContractLedgerEntry(LedgerEntryKey(le), buf.data.size(),
                                         sorobanConfig, appConfig, mParentTx,
                                         diagnosticEvents))
        {
            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return {false, {}};
        }

        auto lk = LedgerEntryKey(le);
        createdAndModifiedKeys.insert(lk);

        uint32_t keySize = static_cast<uint32_t>(xdr::xdr_size(lk));
        uint32_t entrySize = static_cast<uint32_t>(buf.data.size());

        // ttlEntry write fees come out of refundableFee, already
        // accounted for by the host
        if (lk.type() != TTL)
        {
            metrics.noteWriteEntry(isContractCodeEntry(lk), keySize, entrySize);
            if (resources.writeBytes < metrics.mLedgerWriteByte)
            {
                diagnosticEvents.pushApplyTimeDiagnosticError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-write resources exceeds amount specified",
                    {makeU64SCVal(metrics.mLedgerWriteByte),
                     makeU64SCVal(resources.writeBytes)});
                innerResult(res).code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return {false, {}};
            }
        }

        opEntryMap.emplace(lk, le);
        auto iter = entryMap.find(lk);
        if (iter == entryMap.end() || !iter->second.mLedgerEntry)
        {
            createdKeys.insert(lk);
        }
    }

    // Check that each newly created ContractCode or ContractData entry also
    // creates an ttlEntry
    for (auto const& key : createdKeys)
    {
        if (isSorobanEntry(key))
        {
            auto ttlKey = getTTLKey(key);
            releaseAssertOrThrow(createdKeys.find(ttlKey) != createdKeys.end());
        }
        else
        {
            releaseAssertOrThrow(key.type() == TTL);
        }
    }

    // Erase every entry not returned.
    // NB: The entries that haven't been touched are passed through
    // from host, so this should never result in removing an entry
    // that hasn't been removed by host explicitly.
    for (auto const& lk : footprint.readWrite)
    {
        if (createdAndModifiedKeys.find(lk) == createdAndModifiedKeys.end())
        {
            auto entryIter = entryMap.find(lk);
            if (entryIter != entryMap.end() && entryIter->second.mLedgerEntry)
            {
                releaseAssertOrThrow(isSorobanEntry(lk));
                opEntryMap.emplace(lk, std::nullopt);

                // Also delete associated ttlEntry
                auto ttlLK = getTTLKey(lk);

                auto ttlIter = entryMap.find(ttlLK);
                releaseAssertOrThrow(ttlIter != entryMap.end());
                opEntryMap.emplace(ttlLK, std::nullopt);
            }
        }
    }

    // Append events to the enclosing TransactionFrame, where
    // they'll be picked up and transferred to the TxMeta.
    InvokeHostFunctionSuccessPreImage success{};
    success.events.reserve(out.contract_events.size());
    for (auto const& buf : out.contract_events)
    {
        metrics.mEmitEvent++;
        uint32_t eventSize = static_cast<uint32_t>(buf.data.size());
        metrics.mEmitEventByte += eventSize;
        metrics.mMaxEmitEventByte =
            std::max(metrics.mMaxEmitEventByte, eventSize);
        if (sorobanConfig.txMaxContractEventsSizeBytes() <
            metrics.mEmitEventByte)
        {
            diagnosticEvents.pushApplyTimeDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "total events size exceeds network config maximum",
                {makeU64SCVal(metrics.mEmitEventByte),
                 makeU64SCVal(sorobanConfig.txMaxContractEventsSizeBytes())});
            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return {false, {}};
        }
        ContractEvent evt;
        xdr::xdr_from_opaque(buf.data, evt);
        success.events.emplace_back(evt);
    }
    maybePopulateDiagnosticEvents(appConfig, out, metrics, diagnosticEvents);

    metrics.mEmitEventByte += static_cast<uint32>(out.result_value.data.size());
    if (sorobanConfig.txMaxContractEventsSizeBytes() < metrics.mEmitEventByte)
    {
        diagnosticEvents.pushApplyTimeDiagnosticError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "return value pushes events size above network config maximum",
            {makeU64SCVal(metrics.mEmitEventByte),
             makeU64SCVal(sorobanConfig.txMaxContractEventsSizeBytes())});
        innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return {false, {}};
    }

    if (!sorobanData.consumeRefundableSorobanResources(
            metrics.mEmitEventByte, out.rent_fee, ledgerInfo.getLedgerVersion(),
            sorobanConfig, appConfig, mParentTx, diagnosticEvents))
    {
        innerResult(res).code(INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        return {false, {}};
    }

    xdr::xdr_from_opaque(out.result_value.data, success.returnValue);
    innerResult(res).code(INVOKE_HOST_FUNCTION_SUCCESS);
    innerResult(res).success() = xdrSha256(success);

    opEventManager.pushContractEvents(success.events);
    sorobanData.setReturnValue(success.returnValue);
    metrics.mSuccess = true;

    return {true, std::move(opEntryMap)};
}

bool
InvokeHostFunctionOpFrame::doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                                   Hash const& sorobanBasePrngSeed,
                                   OperationResult& res,
                                   std::shared_ptr<SorobanTxData> sorobanData,
                                   OpEventManager& opEventManager) const
{
    releaseAssertOrThrow(sorobanData);
    ZoneNamedN(applyZone, "InvokeHostFunctionOpFrame apply", true);

    Config const& appConfig = app.getConfig();
    HostFunctionMetrics metrics(app.getSorobanMetrics());
    auto timeScope = metrics.getExecTimer();
    auto const& sorobanConfig = app.getSorobanNetworkConfigForApply();

    // Get the entries for the footprint
    rust::Vec<CxxBuf> ledgerEntryCxxBufs;
    rust::Vec<CxxBuf> ttlEntryCxxBufs;

    auto const& resources = mParentTx.sorobanResources();
    metrics.mDeclaredCpuInsn = resources.instructions;

    auto const& footprint = resources.footprint;
    auto footprintLength =
        footprint.readOnly.size() + footprint.readWrite.size();
    auto hotArchive = app.copySearchableHotArchiveBucketListSnapshot();

    ledgerEntryCxxBufs.reserve(footprintLength);
    ttlEntryCxxBufs.reserve(footprintLength);

    auto& diagnosticEvents = opEventManager.getDiagnosticEventsBuffer();
    auto addReads = [&ledgerEntryCxxBufs, &ttlEntryCxxBufs, &ltx, &metrics,
                     &resources, &sorobanConfig, &appConfig, &diagnosticEvents,
                     &res, &hotArchive, this](auto const& keys) -> bool {
        for (auto const& lk : keys)
        {
            uint32_t keySize = static_cast<uint32_t>(xdr::xdr_size(lk));
            uint32_t entrySize = 0u;
            std::optional<TTLEntry> ttlEntry;
            bool sorobanEntryLive = false;

            // For soroban entries, check if the entry is expired before loading
            if (isSorobanEntry(lk))
            {
                auto ttlKey = getTTLKey(lk);
                auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
                if (ttlLtxe)
                {
                    if (!isLive(ttlLtxe.current(), ltx.getHeader().ledgerSeq))
                    {
                        // For temporary entries, treat the expired entry as
                        // if the key did not exist
                        if (!isTemporaryEntry(lk))
                        {
                            if (lk.type() == CONTRACT_CODE)
                            {
                                diagnosticEvents.pushApplyTimeDiagnosticError(
                                    SCE_VALUE, SCEC_INVALID_INPUT,
                                    "trying to access an archived contract "
                                    "code "
                                    "entry",
                                    {makeBytesSCVal(lk.contractCode().hash)});
                            }
                            else if (lk.type() == CONTRACT_DATA)
                            {
                                diagnosticEvents.pushApplyTimeDiagnosticError(
                                    SCE_VALUE, SCEC_INVALID_INPUT,
                                    "trying to access an archived contract "
                                    "data "
                                    "entry",
                                    {makeAddressSCVal(
                                         lk.contractData().contract),
                                     lk.contractData().key});
                            }
                            // Cannot access an archived entry
                            this->innerResult(res).code(
                                INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                            return false;
                        }
                    }
                    else
                    {
                        sorobanEntryLive = true;
                        ttlEntry = ttlLtxe.current().data.ttl();
                    }
                }
                // TODO:This can be removed if both parallel soroban and
                // persistent eviction make it into the same protocol.
                // If ttlLtxe doesn't exist, this is a new Soroban entry
                // Starting in protocol 23, we must check the Hot Archive for
                // new keys. If a new key is actually archived, fail the op.
                else if (isPersistentEntry(lk) &&
                         protocolVersionStartsFrom(
                             ltx.getHeader().ledgerVersion,
                             HotArchiveBucket::
                                 FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
                {
                    auto archiveEntry = hotArchive->load(lk);
                    if (archiveEntry)
                    {
                        if (lk.type() == CONTRACT_CODE)
                        {
                            diagnosticEvents.pushApplyTimeDiagnosticError(
                                SCE_VALUE, SCEC_INVALID_INPUT,
                                "trying to access an archived contract code "
                                "entry",
                                {makeBytesSCVal(lk.contractCode().hash)});
                        }
                        else if (lk.type() == CONTRACT_DATA)
                        {
                            diagnosticEvents.pushApplyTimeDiagnosticError(
                                SCE_VALUE, SCEC_INVALID_INPUT,
                                "trying to access an archived contract data "
                                "entry",
                                {makeAddressSCVal(lk.contractData().contract),
                                 lk.contractData().key});
                        }
                        // Cannot access an archived entry
                        this->innerResult(res).code(
                            INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                        return false;
                    }
                }
            }

            if (!isSorobanEntry(lk) || sorobanEntryLive)
            {
                auto ltxe = ltx.loadWithoutRecord(lk);
                if (ltxe)
                {
                    auto leBuf = toCxxBuf(ltxe.current());
                    entrySize = static_cast<uint32_t>(leBuf.data->size());

                    // For entry types that don't have an ttlEntry (i.e.
                    // Accounts), the rust host expects an "empty" CxxBuf such
                    // that the buffer has a non-null pointer that points to an
                    // empty byte vector
                    auto ttlBuf =
                        ttlEntry
                            ? toCxxBuf(*ttlEntry)
                            : CxxBuf{std::make_unique<std::vector<uint8_t>>()};

                    ledgerEntryCxxBufs.emplace_back(std::move(leBuf));
                    ttlEntryCxxBufs.emplace_back(std::move(ttlBuf));
                }
                else if (isSorobanEntry(lk))
                {
                    releaseAssertOrThrow(!ttlEntry);
                }
            }

            metrics.mReadEntryCounters.noteReadEntry(isContractCodeEntry(lk),
                                                     keySize, entrySize);
            if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                             appConfig, mParentTx,
                                             diagnosticEvents))
            {
                this->innerResult(res).code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            if (resources.diskReadBytes <
                metrics.mReadEntryCounters.mLedgerReadByte)
            {
                diagnosticEvents.pushApplyTimeDiagnosticError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-read resources exceeds amount specified",
                    {makeU64SCVal(metrics.mReadEntryCounters.mLedgerReadByte),
                     makeU64SCVal(resources.diskReadBytes)});

                this->innerResult(res).code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }
        }
        return true;
    };

    if (!addReads(footprint.readOnly))
    {
        // Error code set in addReads
        return false;
    }

    if (!addReads(footprint.readWrite))
    {
        // Error code set in addReads
        return false;
    }

    rust::Vec<CxxBuf> authEntryCxxBufs;
    authEntryCxxBufs.reserve(mInvokeHostFunction.auth.size());
    for (auto const& authEntry : mInvokeHostFunction.auth)
    {
        authEntryCxxBufs.emplace_back(toCxxBuf(authEntry));
    }

    InvokeHostFunctionOutput out{};
    out.success = false;
    try
    {
        CxxBuf basePrngSeedBuf{};
        basePrngSeedBuf.data = std::make_unique<std::vector<uint8_t>>();
        basePrngSeedBuf.data->assign(sorobanBasePrngSeed.begin(),
                                     sorobanBasePrngSeed.end());
        auto moduleCache = app.getModuleCache();

        auto const& lh = ltx.loadHeader().current();
        out = rust_bridge::invoke_host_function(
            appConfig.CURRENT_LEDGER_PROTOCOL_VERSION,
            appConfig.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS, resources.instructions,
            toCxxBuf(mInvokeHostFunction.hostFunction), toCxxBuf(resources),
            toCxxBuf(getSourceID()), authEntryCxxBufs,
            getLedgerInfo(sorobanConfig, lh.ledgerVersion, lh.ledgerSeq,
                          lh.baseReserve, lh.scpValue.closeTime,
                          app.getNetworkID()),
            ledgerEntryCxxBufs, ttlEntryCxxBufs, basePrngSeedBuf,
            sorobanConfig.rustBridgeRentFeeConfiguration(), *moduleCache);
        metrics.mCpuInsn = out.cpu_insns;
        metrics.mMemByte = out.mem_bytes;
        metrics.mInvokeTimeNsecs = out.time_nsecs;
        metrics.mCpuInsnExclVm = out.cpu_insns_excluding_vm_instantiation;
        metrics.mInvokeTimeNsecsExclVm =
            out.time_nsecs_excluding_vm_instantiation;
        if (!out.success)
        {
            maybePopulateDiagnosticEvents(appConfig, out, metrics,
                                          diagnosticEvents);
        }
    }
    catch (std::exception& e)
    {
        // Host invocations should never throw an exception, so encountering
        // one would be an internal error.
        out.is_internal_error = true;
        CLOG_DEBUG(Tx, "Exception caught while invoking host fn: {}", e.what());
    }

    if (!out.success)
    {
        if (out.is_internal_error)
        {
            throw std::runtime_error(
                "Got internal error during Soroban host invocation.");
        }
        if (resources.instructions < out.cpu_insns)
        {
            diagnosticEvents.pushApplyTimeDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation instructions exceeds amount specified",
                {makeU64SCVal(out.cpu_insns),
                 makeU64SCVal(resources.instructions)});
            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
        else if (sorobanConfig.txMemoryLimit() < out.mem_bytes)
        {
            diagnosticEvents.pushApplyTimeDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation memory usage exceeds network config limit",
                {makeU64SCVal(out.mem_bytes),
                 makeU64SCVal(sorobanConfig.txMemoryLimit())});
            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
        else
        {
            innerResult(res).code(INVOKE_HOST_FUNCTION_TRAPPED);
        }
        return false;
    }

    // Create or update every entry returned.
    UnorderedSet<LedgerKey> createdAndModifiedKeys;
    UnorderedSet<LedgerKey> createdKeys;
    for (auto const& buf : out.modified_ledger_entries)
    {
        LedgerEntry le;
        xdr::xdr_from_opaque(buf.data, le);
        if (!validateContractLedgerEntry(LedgerEntryKey(le), buf.data.size(),
                                         sorobanConfig, appConfig, mParentTx,
                                         diagnosticEvents))
        {
            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        auto lk = LedgerEntryKey(le);
        createdAndModifiedKeys.insert(lk);

        uint32_t keySize = static_cast<uint32_t>(xdr::xdr_size(lk));
        uint32_t entrySize = static_cast<uint32_t>(buf.data.size());

        // ttlEntry write fees come out of refundableFee, already
        // accounted for by the host
        if (lk.type() != TTL)
        {
            metrics.noteWriteEntry(isContractCodeEntry(lk), keySize, entrySize);
            if (resources.writeBytes < metrics.mLedgerWriteByte)
            {
                diagnosticEvents.pushApplyTimeDiagnosticError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-write resources exceeds amount specified",
                    {makeU64SCVal(metrics.mLedgerWriteByte),
                     makeU64SCVal(resources.writeBytes)});
                innerResult(res).code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }
        }

        auto ltxe = ltx.load(lk);
        if (ltxe)
        {
            ltxe.current() = le;
        }
        else
        {
            ltx.create(le);
            createdKeys.insert(lk);
        }
    }

    // Check that each newly created ContractCode or ContractData entry also
    // creates an ttlEntry
    for (auto const& key : createdKeys)
    {
        if (isSorobanEntry(key))
        {
            auto ttlKey = getTTLKey(key);
            releaseAssertOrThrow(createdKeys.find(ttlKey) != createdKeys.end());
        }
        else
        {
            releaseAssertOrThrow(key.type() == TTL);
        }
    }

    // Erase every entry not returned.
    // NB: The entries that haven't been touched are passed through
    // from host, so this should never result in removing an entry
    // that hasn't been removed by host explicitly.
    for (auto const& lk : footprint.readWrite)
    {
        if (createdAndModifiedKeys.find(lk) == createdAndModifiedKeys.end())
        {
            auto ltxe = ltx.load(lk);
            if (ltxe)
            {
                releaseAssertOrThrow(isSorobanEntry(lk));
                ltx.erase(lk);

                // Also delete associated ttlEntry
                auto ttlLK = getTTLKey(lk);
                auto ttlLtxe = ltx.load(ttlLK);
                releaseAssertOrThrow(ttlLtxe);
                ltx.erase(ttlLK);
            }
        }
    }

    // Append events to the enclosing TransactionFrame, where
    // they'll be picked up and transferred to the TxMeta.
    InvokeHostFunctionSuccessPreImage success{};
    success.events.reserve(out.contract_events.size());
    for (auto const& buf : out.contract_events)
    {
        metrics.mEmitEvent++;
        uint32_t eventSize = static_cast<uint32_t>(buf.data.size());
        metrics.mEmitEventByte += eventSize;
        metrics.mMaxEmitEventByte =
            std::max(metrics.mMaxEmitEventByte, eventSize);
        if (sorobanConfig.txMaxContractEventsSizeBytes() <
            metrics.mEmitEventByte)
        {
            diagnosticEvents.pushApplyTimeDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "total events size exceeds network config maximum",
                {makeU64SCVal(metrics.mEmitEventByte),
                 makeU64SCVal(sorobanConfig.txMaxContractEventsSizeBytes())});
            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }
        ContractEvent evt;
        xdr::xdr_from_opaque(buf.data, evt);
        success.events.emplace_back(evt);
    }

    maybePopulateDiagnosticEvents(appConfig, out, metrics, diagnosticEvents);

    metrics.mEmitEventByte += static_cast<uint32>(out.result_value.data.size());
    if (sorobanConfig.txMaxContractEventsSizeBytes() < metrics.mEmitEventByte)
    {
        diagnosticEvents.pushApplyTimeDiagnosticError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "return value pushes events size above network config maximum",
            {makeU64SCVal(metrics.mEmitEventByte),
             makeU64SCVal(sorobanConfig.txMaxContractEventsSizeBytes())});
        innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }

    if (!sorobanData->consumeRefundableSorobanResources(
            metrics.mEmitEventByte, out.rent_fee,
            ltx.loadHeader().current().ledgerVersion, sorobanConfig, appConfig,
            mParentTx, diagnosticEvents))
    {
        innerResult(res).code(INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }

    xdr::xdr_from_opaque(out.result_value.data, success.returnValue);
    innerResult(res).code(INVOKE_HOST_FUNCTION_SUCCESS);
    innerResult(res).success() = xdrSha256(success);

    opEventManager.pushContractEvents(success.events);
    sorobanData->setReturnValue(success.returnValue);
    metrics.mSuccess = true;
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValidForSoroban(
    SorobanNetworkConfig const& networkConfig, Config const& appConfig,
    uint32_t ledgerVersion, OperationResult& res,
    DiagnosticEventBuffer* diagnosticEvents) const
{
    // check wasm size if uploading contract
    auto const& hostFn = mInvokeHostFunction.hostFunction;
    if (hostFn.type() == HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM &&
        hostFn.wasm().size() > networkConfig.maxContractSizeBytes())
    {
        pushValidationTimeDiagnosticError(
            diagnosticEvents, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "uploaded Wasm size exceeds network config maximum contract size",
            {makeU64SCVal(hostFn.wasm().size()),
             makeU64SCVal(networkConfig.maxContractSizeBytes())});
        return false;
    }
    if (hostFn.type() == HOST_FUNCTION_TYPE_CREATE_CONTRACT)
    {
        auto const& preimage = hostFn.createContract().contractIDPreimage;
        if (preimage.type() == CONTRACT_ID_PREIMAGE_FROM_ASSET &&
            !isAssetValid(preimage.fromAsset(), ledgerVersion))
        {
            pushValidationTimeDiagnosticError(
                diagnosticEvents, SCE_VALUE, SCEC_INVALID_INPUT,
                "invalid asset to create contract from");
            return false;
        }
    }
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(uint32_t ledgerVersion,
                                        OperationResult& res) const
{
    throw std::runtime_error(
        "InvokeHostFunctionOpFrame::doCheckValid needs Config");
}

void
InvokeHostFunctionOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
InvokeHostFunctionOpFrame::isSoroban() const
{
    return true;
}
}
