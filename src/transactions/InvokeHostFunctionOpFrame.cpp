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
bool
isCodeKey(LedgerKey const& lk)
{
    return lk.type() == CONTRACT_CODE;
}

template <typename T>
std::vector<uint8_t>
toVec(T const& t)
{
    return std::vector<uint8_t>(xdr::xdr_to_opaque(t));
}

template <typename T>
CxxBuf
toCxxBuf(T const& t)
{
    return CxxBuf{std::make_unique<std::vector<uint8_t>>(toVec(t))};
}

CxxLedgerInfo
getLedgerInfo(AbstractLedgerTxn& ltx, AppConnector& app,
              SorobanNetworkConfig const& sorobanConfig)
{
    CxxLedgerInfo info{};
    auto const& hdr = ltx.loadHeader().current();
    info.base_reserve = hdr.baseReserve;
    info.protocol_version = hdr.ledgerVersion;
    info.sequence_number = hdr.ledgerSeq;
    info.timestamp = hdr.scpValue.closeTime;
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

    auto& networkID = app.getNetworkID();
    info.network_id.reserve(networkID.size());
    for (auto c : networkID)
    {
        info.network_id.emplace_back(static_cast<unsigned char>(c));
    }
    return info;
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

struct HostFunctionMetrics
{
    SorobanMetrics& mMetrics;

    uint32_t mReadEntry{0};
    uint32_t mWriteEntry{0};

    uint32_t mLedgerReadByte{0};
    uint32_t mLedgerWriteByte{0};

    uint32_t mReadKeyByte{0};
    uint32_t mWriteKeyByte{0};

    uint32_t mReadDataByte{0};
    uint32_t mWriteDataByte{0};

    uint32_t mReadCodeByte{0};
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

    // max single entity size metrics
    uint32_t mMaxReadWriteKeyByte{0};
    uint32_t mMaxReadWriteDataByte{0};
    uint32_t mMaxReadWriteCodeByte{0};
    uint32_t mMaxEmitEventByte{0};

    bool mSuccess{false};

    HostFunctionMetrics(SorobanMetrics& metrics) : mMetrics(metrics)
    {
    }

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

    void
    noteWriteEntry(bool isCodeEntry, uint32_t keySize, uint32_t entrySize)
    {
        mWriteEntry++;
        mMaxReadWriteKeyByte = std::max(mMaxReadWriteKeyByte, keySize);
        mLedgerWriteByte += entrySize;
        if (isCodeEntry)
        {
            mWriteCodeByte += entrySize;
            mMaxReadWriteCodeByte = std::max(mMaxReadWriteCodeByte, entrySize);
        }
        else
        {
            mWriteDataByte += entrySize;
            mMaxReadWriteDataByte = std::max(mMaxReadWriteDataByte, entrySize);
        }
    }

    ~HostFunctionMetrics()
    {
        mMetrics.mHostFnOpReadEntry.Mark(mReadEntry);
        mMetrics.mHostFnOpWriteEntry.Mark(mWriteEntry);

        mMetrics.mHostFnOpReadKeyByte.Mark(mReadKeyByte);
        mMetrics.mHostFnOpWriteKeyByte.Mark(mWriteKeyByte);

        mMetrics.mHostFnOpReadLedgerByte.Mark(mLedgerReadByte);
        mMetrics.mHostFnOpReadDataByte.Mark(mReadDataByte);
        mMetrics.mHostFnOpReadCodeByte.Mark(mReadCodeByte);

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

        mMetrics.mHostFnOpMaxRwKeyByte.Mark(mMaxReadWriteKeyByte);
        mMetrics.mHostFnOpMaxRwDataByte.Mark(mMaxReadWriteDataByte);
        mMetrics.mHostFnOpMaxRwCodeByte.Mark(mMaxReadWriteCodeByte);
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
    HostFunctionMetrics const& metrics, SorobanTxData& sorobanData) const
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
            metricsEvent(metrics.mSuccess, "read_entry", metrics.mReadEntry));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "write_entry", metrics.mWriteEntry));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "ledger_read_byte", metrics.mLedgerReadByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "ledger_write_byte", metrics.mLedgerWriteByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "read_key_byte", metrics.mReadKeyByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "write_key_byte", metrics.mWriteKeyByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "read_data_byte", metrics.mReadDataByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "write_data_byte", metrics.mWriteDataByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "read_code_byte", metrics.mReadCodeByte));
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
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "max_rw_key_byte", metrics.mMaxReadWriteKeyByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "max_rw_data_byte",
                         metrics.mMaxReadWriteDataByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "max_rw_code_byte",
                         metrics.mMaxReadWriteCodeByte));
        diagnosticEvents.emplace_back(metricsEvent(metrics.mSuccess,
                                                   "max_emit_event_byte",
                                                   metrics.mMaxEmitEventByte));

        sorobanData.pushDiagnosticEvents(diagnosticEvents);
    }
}

bool
InvokeHostFunctionOpFrame::doApply(
    AppConnector& app, AbstractLedgerTxn& ltx, Hash const& sorobanBasePrngSeed,
    OperationResult& res, std::shared_ptr<SorobanTxData> sorobanData) const
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

    auto addReads = [&ledgerEntryCxxBufs, &ttlEntryCxxBufs, &ltx, &metrics,
                     &resources, &sorobanConfig, &appConfig, sorobanData, &res,
                     &hotArchive, this](auto const& keys) -> bool {
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
                                sorobanData->pushApplyTimeDiagnosticError(
                                    appConfig, SCE_VALUE, SCEC_INVALID_INPUT,
                                    "trying to access an archived contract "
                                    "code "
                                    "entry",
                                    {makeBytesSCVal(lk.contractCode().hash)});
                            }
                            else if (lk.type() == CONTRACT_DATA)
                            {
                                sorobanData->pushApplyTimeDiagnosticError(
                                    appConfig, SCE_VALUE, SCEC_INVALID_INPUT,
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
                // If ttlLtxe doesn't exist, this is a new Soroban entry
                // Starting in protocol 23, we must check the Hot Archive for
                // new keys. If a new key is actually archived, fail the op.
                if (isPersistentEntry(lk) &&
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
                            sorobanData->pushApplyTimeDiagnosticError(
                                appConfig, SCE_VALUE, SCEC_INVALID_INPUT,
                                "trying to access an archived contract code "
                                "entry",
                                {makeBytesSCVal(lk.contractCode().hash)});
                        }
                        else if (lk.type() == CONTRACT_DATA)
                        {
                            sorobanData->pushApplyTimeDiagnosticError(
                                appConfig, SCE_VALUE, SCEC_INVALID_INPUT,
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

            metrics.noteReadEntry(isCodeKey(lk), keySize, entrySize);
            if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                             appConfig, mParentTx,
                                             *sorobanData))
            {
                this->innerResult(res).code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            if (resources.readBytes < metrics.mLedgerReadByte)
            {
                sorobanData->pushApplyTimeDiagnosticError(
                    appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-read resources exceeds amount specified",
                    {makeU64SCVal(metrics.mLedgerReadByte),
                     makeU64SCVal(resources.readBytes)});

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

        out = rust_bridge::invoke_host_function(
            appConfig.CURRENT_LEDGER_PROTOCOL_VERSION,
            appConfig.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS, resources.instructions,
            toCxxBuf(mInvokeHostFunction.hostFunction), toCxxBuf(resources),
            toCxxBuf(getSourceID()), authEntryCxxBufs,
            getLedgerInfo(ltx, app, sorobanConfig), ledgerEntryCxxBufs,
            ttlEntryCxxBufs, basePrngSeedBuf,
            sorobanConfig.rustBridgeRentFeeConfiguration());
        metrics.mCpuInsn = out.cpu_insns;
        metrics.mMemByte = out.mem_bytes;
        metrics.mInvokeTimeNsecs = out.time_nsecs;
        metrics.mCpuInsnExclVm = out.cpu_insns_excluding_vm_instantiation;
        metrics.mInvokeTimeNsecsExclVm =
            out.time_nsecs_excluding_vm_instantiation;
        if (!out.success)
        {
            maybePopulateDiagnosticEvents(appConfig, out, metrics,
                                          *sorobanData);
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
            sorobanData->pushApplyTimeDiagnosticError(
                appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation instructions exceeds amount specified",
                {makeU64SCVal(out.cpu_insns),
                 makeU64SCVal(resources.instructions)});
            innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
        else if (sorobanConfig.txMemoryLimit() < out.mem_bytes)
        {
            sorobanData->pushApplyTimeDiagnosticError(
                appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
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
                                         *sorobanData))
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
            metrics.noteWriteEntry(isCodeKey(lk), keySize, entrySize);
            if (resources.writeBytes < metrics.mLedgerWriteByte)
            {
                sorobanData->pushApplyTimeDiagnosticError(
                    appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
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
            sorobanData->pushApplyTimeDiagnosticError(
                appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
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

    maybePopulateDiagnosticEvents(appConfig, out, metrics, *sorobanData);

    metrics.mEmitEventByte += static_cast<uint32>(out.result_value.data.size());
    if (sorobanConfig.txMaxContractEventsSizeBytes() < metrics.mEmitEventByte)
    {
        sorobanData->pushApplyTimeDiagnosticError(
            appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "return value pushes events size above network config maximum",
            {makeU64SCVal(metrics.mEmitEventByte),
             makeU64SCVal(sorobanConfig.txMaxContractEventsSizeBytes())});
        innerResult(res).code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }

    if (!sorobanData->consumeRefundableSorobanResources(
            metrics.mEmitEventByte, out.rent_fee,
            ltx.loadHeader().current().ledgerVersion, sorobanConfig, appConfig,
            mParentTx))
    {
        innerResult(res).code(INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }

    xdr::xdr_from_opaque(out.result_value.data, success.returnValue);
    innerResult(res).code(INVOKE_HOST_FUNCTION_SUCCESS);
    innerResult(res).success() = xdrSha256(success);

    sorobanData->pushContractEvents(success.events);
    sorobanData->setReturnValue(success.returnValue);
    metrics.mSuccess = true;
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValidForSoroban(
    SorobanNetworkConfig const& networkConfig, Config const& appConfig,
    uint32_t ledgerVersion, OperationResult& res,
    SorobanTxData& sorobanData) const
{
    // check wasm size if uploading contract
    auto const& hostFn = mInvokeHostFunction.hostFunction;
    if (hostFn.type() == HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM &&
        hostFn.wasm().size() > networkConfig.maxContractSizeBytes())
    {
        sorobanData.pushValidationTimeDiagnosticError(
            appConfig, SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
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
            sorobanData.pushValidationTimeDiagnosticError(
                appConfig, SCE_VALUE, SCEC_INVALID_INPUT,
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
