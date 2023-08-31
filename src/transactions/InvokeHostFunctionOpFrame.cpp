// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include <cstdint>
#include <json/json.h>
#include <medida/metrics_registry.h>
#include <xdrpp/types.h>
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "rust/RustVecXdrMarshal.h"
#endif
// clang-format on

#include "ledger/LedgerTxnImpl.h"
#include "rust/CppShims.h"
#include "xdr/Stellar-transaction.h"
#include <stdexcept>
#include <xdrpp/xdrpp/printer.h>

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "rust/RustBridge.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include <crypto/SHA.h>

namespace stellar
{
namespace
{
struct LedgerEntryRentState
{
    bool readOnly{};
    uint32_t oldExpirationLedger{};
    uint32_t newExpirationLedger{};
    uint32_t oldSize{};
    uint32_t newSize{};
};

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
getLedgerInfo(AbstractLedgerTxn& ltx, Config const& cfg,
              SorobanNetworkConfig const& sorobanConfig)
{
    CxxLedgerInfo info{};
    auto const& hdr = ltx.loadHeader().current();
    info.base_reserve = hdr.baseReserve;
    info.protocol_version = hdr.ledgerVersion;
    info.sequence_number = hdr.ledgerSeq;
    info.timestamp = hdr.scpValue.closeTime;
    info.memory_limit = sorobanConfig.txMemoryLimit();
    info.min_persistent_entry_expiration =
        sorobanConfig.stateExpirationSettings().minPersistentEntryExpiration;
    info.min_temp_entry_expiration =
        sorobanConfig.stateExpirationSettings().minTempEntryExpiration;
    info.max_entry_expiration =
        sorobanConfig.stateExpirationSettings().maxEntryExpiration;
    info.autobump_ledgers =
        sorobanConfig.stateExpirationSettings().autoBumpLedgers;
    info.cpu_cost_params = toCxxBuf(sorobanConfig.cpuCostParams());
    info.mem_cost_params = toCxxBuf(sorobanConfig.memCostParams());
    // TODO: move network id to config to not recompute hash
    auto networkID = sha256(cfg.NETWORK_PASSPHRASE);
    for (auto c : networkID)
    {
        info.network_id.push_back(static_cast<unsigned char>(c));
    }
    return info;
}

bool
validateContractLedgerEntry(LedgerEntry const& le, size_t entrySize,
                            SorobanNetworkConfig const& config)
{
    releaseAssertOrThrow(!isSorobanEntry(le.data) ||
                         getLeType(le.data) == DATA_ENTRY);

    // check contract code size limit
    if (le.data.type() == CONTRACT_CODE &&
        config.maxContractSizeBytes() <
            le.data.contractCode().body.code().size())
    {
        return false;
    }
    // check contract data entry size limit
    if (le.data.type() == CONTRACT_DATA &&
        config.maxContractDataEntrySizeBytes() < entrySize)
    {
        return false;
    }
    return true;
}

SCVal
makeSymbol(std::string const& str)
{
    SCVal val(SCV_SYMBOL);
    val.sym().assign(str.begin(), str.end());
    return val;
}

SCVal
makeU64(uint64_t u)
{
    SCVal val(SCV_U64);
    val.u64() = u;
    return val;
}

DiagnosticEvent
metricsEvent(bool success, std::string const& topic, uint32 value)
{
    DiagnosticEvent de;
    de.inSuccessfulContractCall = success;
    de.event.type = ContractEventType::DIAGNOSTIC;
    SCVec topics = {
        makeSymbol("core_metrics"),
        makeSymbol(topic),
    };
    de.event.body.v0().topics = topics;
    de.event.body.v0().data = makeU64(value);
    return de;
}

} // namespace

struct HostFunctionMetrics
{
    medida::MetricsRegistry& mMetrics;

    uint32 mReadEntry{0};
    uint32 mWriteEntry{0};

    uint32 mLedgerReadByte{0};
    uint32 mLedgerWriteByte{0};

    uint32 mReadKeyByte{0};
    uint32 mWriteKeyByte{0};

    uint32 mReadDataByte{0};
    uint32 mWriteDataByte{0};

    uint32 mReadCodeByte{0};
    uint32 mWriteCodeByte{0};

    uint32 mEmitEvent{0};
    uint32 mEmitEventByte{0};

    // host runtime metrics
    uint32 mCpuInsn{0};
    uint32 mMemByte{0};
    uint32 mInvokeTimeNsecs{0};
    uint32 mInsnNsecRatio{0};

    // max single entity size metrics
    uint32 mMaxReadWriteKeyByte{0};
    uint32 mMaxReadWriteDataByte{0};
    uint32 mMaxReadWriteCodeByte{0};
    uint32 mMaxEmitEventByte{0};

    bool mSuccess{false};

    HostFunctionMetrics(medida::MetricsRegistry& metrics) : mMetrics(metrics)
    {
    }

    void
    noteReadEntry(bool isCodeEntry, uint32 keySize, uint32 entrySize)
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
    noteWriteEntry(bool isCodeEntry, uint32 keySize, uint32 entrySize)
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
        mMetrics.NewMeter({"soroban", "host-fn-op", "read-entry"}, "entry")
            .Mark(mReadEntry);
        mMetrics.NewMeter({"soroban", "host-fn-op", "write-entry"}, "entry")
            .Mark(mWriteEntry);

        mMetrics.NewMeter({"soroban", "host-fn-op", "read-key-byte"}, "byte")
            .Mark(mReadKeyByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "write-key-byte"}, "byte")
            .Mark(mWriteKeyByte);

        mMetrics.NewMeter({"soroban", "host-fn-op", "read-ledger-byte"}, "byte")
            .Mark(mLedgerReadByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "read-data-byte"}, "byte")
            .Mark(mReadDataByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "read-code-byte"}, "byte")
            .Mark(mReadCodeByte);

        mMetrics
            .NewMeter({"soroban", "host-fn-op", "write-ledger-byte"}, "byte")
            .Mark(mLedgerWriteByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "write-data-byte"}, "byte")
            .Mark(mWriteDataByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "write-code-byte"}, "byte")
            .Mark(mWriteCodeByte);

        mMetrics.NewMeter({"soroban", "host-fn-op", "emit-event"}, "event")
            .Mark(mEmitEvent);
        mMetrics.NewMeter({"soroban", "host-fn-op", "emit-event-byte"}, "byte")
            .Mark(mEmitEventByte);

        mMetrics.NewMeter({"soroban", "host-fn-op", "cpu-insn"}, "insn")
            .Mark(mCpuInsn);
        mMetrics.NewMeter({"soroban", "host-fn-op", "mem-byte"}, "byte")
            .Mark(mMemByte);
        mMetrics
            .NewMeter({"soroban", "host-fn-op", "invoke-time-nsecs"}, "time")
            .Mark(mInvokeTimeNsecs);
        mMetrics.NewMeter({"soroban", "host-fn-op", "insn-nsec-ratio"}, "ratio")
            .Mark(mInsnNsecRatio);

        mMetrics.NewMeter({"soroban", "host-fn-op", "max-rw-key-byte"}, "byte")
            .Mark(mMaxReadWriteKeyByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "max-rw-data-byte"}, "byte")
            .Mark(mMaxReadWriteDataByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "max-rw-code-byte"}, "byte")
            .Mark(mMaxReadWriteCodeByte);
        mMetrics
            .NewMeter({"soroban", "host-fn-op", "max-emit-event-byte"}, "byte")
            .Mark(mMaxEmitEventByte);

        if (mSuccess)
        {
            mMetrics.NewMeter({"soroban", "host-fn-op", "success"}, "call")
                .Mark();
        }
        else
        {
            mMetrics.NewMeter({"soroban", "host-fn-op", "failure"}, "call")
                .Mark();
        }
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.NewTimer({"soroban", "host-fn-op", "exec"}).TimeScope();
    }
};

InvokeHostFunctionOpFrame::InvokeHostFunctionOpFrame(Operation const& op,
                                                     OperationResult& res,
                                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mInvokeHostFunction(mOperation.body.invokeHostFunctionOp())
{
}

bool
InvokeHostFunctionOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
InvokeHostFunctionOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error(
        "InvokeHostFunctionOpFrame::doApply needs Config and base PRNG seed");
}

void
InvokeHostFunctionOpFrame::maybePopulateDiagnosticEvents(
    Config const& cfg, InvokeHostFunctionOutput const& output,
    HostFunctionMetrics const& metrics)
{
    if (cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS)
    {
        xdr::xvector<DiagnosticEvent> diagnosticEvents;
        for (auto const& e : output.diagnostic_events)
        {
            DiagnosticEvent evt;
            xdr::xdr_from_opaque(e.data, evt);
            diagnosticEvents.emplace_back(evt);
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

        mParentTx.pushDiagnosticEvents(std::move(diagnosticEvents));
    }
}

bool
InvokeHostFunctionOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx,
                                   Hash const& sorobanBasePrngSeed)
{
    Config const& cfg = app.getConfig();
    HostFunctionMetrics metrics(app.getMetrics());
    auto const& sorobanConfig =
        app.getLedgerManager().getSorobanNetworkConfig(ltx);

    // Get the entries for the footprint
    rust::Vec<CxxBuf> ledgerEntryCxxBufs;

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    auto footprintLength =
        footprint.readOnly.size() + footprint.readWrite.size();

    ledgerEntryCxxBufs.reserve(footprintLength);

    auto addReads = [&ledgerEntryCxxBufs, &ltx, &metrics, &resources,
                     this](auto const& keys, bool readOnly) -> bool {
        for (auto const& lk : keys)
        {
            uint32 keySize = static_cast<uint32>(xdr::xdr_size(lk));
            uint32 entrySize = 0u;
            // Load without record for readOnly to avoid writing them later
            auto ltxe = ltx.loadWithoutRecord(lk);
            if (ltxe)
            {
                auto const& le = ltxe.current();
                bool shouldAddEntry = true;
                if (!isLive(le, ltx.getHeader().ledgerSeq))
                {
                    if (isTemporaryEntry(lk))
                    {
                        // For temporary entries, treat the expired entry as if
                        // the key did not exist
                        shouldAddEntry = false;
                    }
                    else
                    {
                        // Cannot access an expired entry
                        this->innerResult().code(
                            INVOKE_HOST_FUNCTION_ENTRY_EXPIRED);
                        return false;
                    }
                }

                if (shouldAddEntry)
                {
                    auto buf = toCxxBuf(le);
                    entrySize = static_cast<uint32>(buf.data->size());
                    ledgerEntryCxxBufs.emplace_back(std::move(buf));
                }
            }
            metrics.noteReadEntry(isCodeKey(lk), keySize, entrySize);

            if (resources.readBytes < metrics.mLedgerReadByte)
            {
                this->innerResult().code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }
        }
        return true;
    };

    if (!addReads(footprint.readOnly, true))
    {
        // Error code set in addReads
        return false;
    }

    if (!addReads(footprint.readWrite, false))
    {
        // Error code set in addReads
        return false;
    }

    rust::Vec<CxxBuf> authEntryCxxBufs;
    authEntryCxxBufs.reserve(mInvokeHostFunction.auth.size());
    for (auto const& authEntry : mInvokeHostFunction.auth)
    {
        authEntryCxxBufs.push_back(toCxxBuf(authEntry));
    }

    InvokeHostFunctionOutput out{};
    out.success = false;
    try
    {
        auto timeScope = metrics.getExecTimer();
        CxxBuf basePrngSeedBuf{};
        basePrngSeedBuf.data = std::make_unique<std::vector<uint8_t>>();
        basePrngSeedBuf.data->assign(sorobanBasePrngSeed.begin(),
                                     sorobanBasePrngSeed.end());

        out = rust_bridge::invoke_host_function(
            cfg.CURRENT_LEDGER_PROTOCOL_VERSION,
            cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS, resources.instructions,
            toCxxBuf(mInvokeHostFunction.hostFunction), toCxxBuf(resources),
            toCxxBuf(getSourceID()), authEntryCxxBufs,
            getLedgerInfo(ltx, cfg, sorobanConfig), ledgerEntryCxxBufs,
            basePrngSeedBuf, sorobanConfig.rustBridgeRentFeeConfiguration());

        if (!out.success)
        {
            maybePopulateDiagnosticEvents(cfg, out, metrics);
        }
    }
    catch (std::exception& e)
    {
        CLOG_DEBUG(Tx, "Exception caught while invoking host fn: {}", e.what());
    }

    metrics.mCpuInsn = static_cast<uint32>(out.cpu_insns);
    metrics.mMemByte = static_cast<uint32>(out.mem_bytes);
    metrics.mInvokeTimeNsecs = static_cast<uint32>(out.time_nsecs);
    metrics.mInsnNsecRatio =
        metrics.mCpuInsn / std::max(metrics.mInvokeTimeNsecs, (uint32)1);

    if (!out.success)
    {
        if (resources.instructions < out.cpu_insns ||
            sorobanConfig.txMemoryLimit() < out.mem_bytes)
        {
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
        else
        {
            innerResult().code(INVOKE_HOST_FUNCTION_TRAPPED);
        }
        return false;
    }

    // Create or update every entry returned.
    UnorderedSet<LedgerKey> remainingRWKeys;
    for (auto const& buf : out.modified_ledger_entries)
    {
        LedgerEntry le;
        xdr::xdr_from_opaque(buf.data, le);
        if (!validateContractLedgerEntry(le, buf.data.size(), sorobanConfig))
        {
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        auto lk = LedgerEntryKey(le);
        remainingRWKeys.insert(lk);

        uint32 keySize = static_cast<uint32>(xdr::xdr_size(lk));
        uint32 entrySize = static_cast<uint32>(buf.data.size());
        metrics.noteWriteEntry(isCodeKey(lk), keySize, entrySize);
        if (resources.writeBytes < metrics.mLedgerWriteByte)
        {
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        auto ltxe = ltx.load(lk);
        if (ltxe)
        {
            ltxe.current() = le;
        }
        else
        {
            ltx.create(le);
        }
    }

    // Erase every entry not returned.
    // NB: The entries that haven't been touched are passed through
    // from host, so this should never result in removing an entry
    // that hasn't been removed by host explicitly.
    for (auto const& lk : footprint.readWrite)
    {
        if (remainingRWKeys.find(lk) == remainingRWKeys.end())
        {
            auto ltxe = ltx.load(lk);
            if (ltxe)
            {
                ltx.erase(lk);
            }
        }
    }

    // Apply expiration bumps for read-only entries (expiration for RW
    // entries is updated within the modified entry.
    for (auto const& bump : out.read_only_bumps)
    {
        LedgerKey lk;
        xdr::xdr_from_opaque(bump.ledger_key.data, lk);
        releaseAssertOrThrow(isSorobanDataEntry(lk));
        auto ltxe = ltx.load(lk);
        releaseAssertOrThrow(ltxe);
        // TODO: this should use expiration extension for RO entries.
        setExpirationLedger(ltxe.current(), bump.min_expiration);
    }

    // Append events to the enclosing TransactionFrame, where
    // they'll be picked up and transferred to the TxMeta.
    InvokeHostFunctionSuccessPreImage success{};
    for (auto const& buf : out.contract_events)
    {
        metrics.mEmitEvent++;
        uint32 eventSize = static_cast<uint32>(buf.data.size());
        metrics.mEmitEventByte += eventSize;
        metrics.mMaxEmitEventByte =
            std::max(metrics.mMaxEmitEventByte, eventSize);
        if (sorobanConfig.txMaxContractEventsSizeBytes() <
            metrics.mEmitEventByte)
        {
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }
        ContractEvent evt;
        xdr::xdr_from_opaque(buf.data, evt);
        success.events.emplace_back(evt);
    }

    maybePopulateDiagnosticEvents(cfg, out, metrics);

    metrics.mEmitEventByte += out.result_value.data.size();
    if (sorobanConfig.txMaxContractEventsSizeBytes() < metrics.mEmitEventByte)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }

    if (!mParentTx.consumeRefundableSorobanResources(
            metrics.mEmitEventByte, out.rent_fee,
            ltx.loadHeader().current().ledgerVersion, sorobanConfig, cfg))
    {
        innerResult().code(INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }

    xdr::xdr_from_opaque(out.result_value.data, success.returnValue);
    innerResult().code(INVOKE_HOST_FUNCTION_SUCCESS);
    innerResult().success() = xdrSha256(success);

    mParentTx.pushContractEvents(std::move(success.events));
    mParentTx.setReturnValue(std::move(success.returnValue));
    metrics.mSuccess = true;
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(SorobanNetworkConfig const& config,
                                        uint32_t ledgerVersion)
{
    // check wasm size if uploading contract
    auto const& hostFn = mInvokeHostFunction.hostFunction;
    if (hostFn.type() == HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM &&
        hostFn.wasm().size() > config.maxContractSizeBytes())
    {
        return false;
    }
    if (hostFn.type() == HOST_FUNCTION_TYPE_CREATE_CONTRACT)
    {
        auto const& preimage = hostFn.createContract().contractIDPreimage;
        if (preimage.type() == CONTRACT_ID_PREIMAGE_FROM_ASSET &&
            !isAssetValid(preimage.fromAsset(), ledgerVersion))
        {
            return false;
        }
    }
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(uint32_t ledgerVersion)
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
#endif // ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
