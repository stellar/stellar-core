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
    CxxLedgerInfo info;
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
validateContractLedgerEntry(LedgerEntry const& le, size_t nByte,
                            SorobanNetworkConfig const& config)
{
    releaseAssert(!isSorobanEntry(le.data) || getLeType(le.data) == DATA_ENTRY);

    // check contract code size limit
    if (le.data.type() == CONTRACT_CODE &&
        config.maxContractSizeBytes() <
            le.data.contractCode().body.code().size())
    {
        return false;
    }
    // check contract data entry size limit
    if (le.data.type() == CONTRACT_DATA &&
        config.maxContractDataEntrySizeBytes() < nByte)
    {
        return false;
    }
    return true;
}

template <typename T>
std::vector<uint8_t>
toVec(T const& t)
{
    return std::vector<uint8_t>(xdr::xdr_to_opaque(t));
}

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
    Config const& cfg, InvokeHostFunctionOutput const& output)
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
        mParentTx.pushDiagnosticEvents(std::move(diagnosticEvents));
    }
}

struct HostFunctionMetrics
{
    medida::MetricsRegistry& mMetrics;

    size_t mReadEntry{0};
    size_t mWriteEntry{0};

    size_t mLedgerReadByte{0};
    size_t mLedgerWriteByte{0};

    size_t mReadKeyByte{0};
    size_t mWriteKeyByte{0};

    size_t mReadDataByte{0};
    size_t mWriteDataByte{0};

    size_t mReadCodeByte{0};
    size_t mWriteCodeByte{0};

    size_t mEmitEvent{0};
    size_t mEmitEventByte{0};

    size_t mCpuInsn{0};
    size_t mMemByte{0};

    size_t mMetadataSizeByte{0};

    bool mSuccess{false};

    HostFunctionMetrics(medida::MetricsRegistry& metrics) : mMetrics(metrics)
    {
    }

    bool
    isCodeKey(LedgerKey const& lk)
    {
        return lk.type() == CONTRACT_CODE;
    }

    void
    noteReadEntry(LedgerKey const& lk, size_t n)
    {
        mReadEntry++;
        auto keySize = xdr::xdr_size(lk);
        mReadKeyByte += keySize;
        mLedgerReadByte += keySize + n;
        if (isCodeKey(lk))
        {
            mReadCodeByte += n;
        }
        else
        {
            mReadDataByte += n;
        }
    }

    void
    noteWriteEntry(LedgerKey const& lk, size_t n)
    {
        mWriteEntry++;
        auto keySize = xdr::xdr_size(lk);
        mWriteKeyByte += keySize;
        mLedgerWriteByte += keySize + n;
        if (isCodeKey(lk))
        {
            mWriteCodeByte += n;
        }
        else
        {
            mWriteDataByte += n;
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

        mMetrics
            .NewMeter({"soroban", "host-fn-op", "metadata-size-byte"}, "byte")
            .Mark(mMetadataSizeByte);

        mMetrics.NewMeter({"soroban", "host-fn-op", "cpu-insn"}, "insn")
            .Mark(mCpuInsn);
        mMetrics.NewMeter({"soroban", "host-fn-op", "mem-byte"}, "byte")
            .Mark(mMemByte);

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
    UnorderedMap<LedgerKey, uint32_t> originalExpirations;
    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    auto reserveSize = footprint.readOnly.size() + footprint.readWrite.size();
    ledgerEntryCxxBufs.reserve(reserveSize);

    auto addReads = [&ledgerEntryCxxBufs, &ltx, &metrics, &sorobanConfig,
                     &originalExpirations](auto const& keys) -> bool {
        for (auto const& lk : keys)
        {
            // Load without record for readOnly to avoid writing them later
            auto ltxe = ltx.loadWithoutRecord(lk, /*loadExpiredEntry=*/false);
            size_t nByte{0};
            if (ltxe)
            {
                auto const& le = ltxe.current();
                auto buf = toCxxBuf(le);
                nByte = buf.data->size();
                // Typically invalid entry read should not happen unless some
                // backward-incompatible change happened (e.g. reducing the
                // contract data size limit) that renders previously valid
                // entries no longer valid.
                if (!validateContractLedgerEntry(le, nByte, sorobanConfig))
                {
                    return false;
                }
                ledgerEntryCxxBufs.emplace_back(std::move(buf));
                if (isSorobanEntry(le.data))
                {
                    originalExpirations.emplace(LedgerEntryKey(le),
                                                getExpirationLedger(le));
                }
            }
            metrics.noteReadEntry(lk, nByte);
        }
        return true;
    };

    if (!addReads(footprint.readWrite))
    {
        innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }
    // Metadata includes the ledger entry changes which we
    // approximate as the size of the RW entries that we read
    // plus size of the RW entries that we write back to the ledger.
    metrics.mMetadataSizeByte += metrics.mLedgerReadByte;
    if (resources.extendedMetaDataSizeBytes < metrics.mMetadataSizeByte)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }

    if (!addReads(footprint.readOnly))
    {
        innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }

    if (resources.readBytes < metrics.mLedgerReadByte)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }

    CxxBuf hostFnCxxBuf = toCxxBuf(mInvokeHostFunction.hostFunction);
    rust::Vec<CxxBuf> authEntryCxxBufs;
    authEntryCxxBufs.reserve(mInvokeHostFunction.auth.size());
    for (auto const& authEntry : mInvokeHostFunction.auth)
    {
        authEntryCxxBufs.push_back(toCxxBuf(authEntry));
    }

    InvokeHostFunctionOutput out{};
    try
    {
        auto timeScope = metrics.getExecTimer();
        CxxBuf basePrngSeedBuf;
        basePrngSeedBuf.data = std::make_unique<std::vector<uint8_t>>();
        basePrngSeedBuf.data->assign(sorobanBasePrngSeed.begin(),
                                     sorobanBasePrngSeed.end());

        out = rust_bridge::invoke_host_function(
            cfg.CURRENT_LEDGER_PROTOCOL_VERSION,
            cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS, hostFnCxxBuf,
            toCxxBuf(resources), toCxxBuf(getSourceID()), authEntryCxxBufs,
            getLedgerInfo(ltx, cfg, sorobanConfig), ledgerEntryCxxBufs,
            basePrngSeedBuf);

        if (out.success)
        {
            metrics.mSuccess = true;
        }
        else
        {
            maybePopulateDiagnosticEvents(cfg, out);
        }
    }
    catch (std::exception& e)
    {
        CLOG_DEBUG(Tx, "Exception caught while invoking host fn: {}", e.what());
    }

    metrics.mCpuInsn = out.cpu_insns;
    metrics.mMemByte = out.mem_bytes;
    if (!metrics.mSuccess)
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

    // Create or update every entry returned
    std::unordered_set<LedgerKey> keys;
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
        metrics.noteWriteEntry(lk, buf.data.size());
        if (resources.writeBytes < metrics.mLedgerWriteByte)
        {
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        // Temp entries can be overwritten even when expired
        auto ltxe = ltx.load(lk, /*loadExpiredEntry=*/true);
        if (ltxe)
        {
            ltxe.current() = le;
        }
        else
        {
            ltx.create(le);
        }

        keys.emplace(std::move(lk));
    }

    for (auto const& bumps : out.expiration_bumps)
    {
        LedgerKey lk;
        xdr::xdr_from_opaque(bumps.ledger_key.data, lk);

        auto minExpirationLedger = bumps.min_expiration;

        if (!isSorobanEntry(lk))
        {
            continue;
        }
        auto ltxe = ltx.load(lk);
        if (ltxe && getExpirationLedger(ltxe.current()) <= minExpirationLedger)
        {
            setExpirationLedger(ltxe.current(), minExpirationLedger);
        }
    }

    metrics.mMetadataSizeByte += metrics.mLedgerWriteByte;
    if (resources.extendedMetaDataSizeBytes < metrics.mMetadataSizeByte)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }

    // Erase every entry not returned
    for (auto const& lk : footprint.readWrite)
    {
        if (keys.find(lk) == keys.end())
        {
            auto ltxe = ltx.load(lk);
            if (ltxe)
            {
                ltx.erase(lk);
            }
        }
    }

    // Append events to the enclosing TransactionFrame, where
    // they'll be picked up and transferred to the TxMeta.

    InvokeHostFunctionSuccessPreImage success;
    for (auto const& buf : out.contract_events)
    {
        metrics.mEmitEvent++;
        metrics.mEmitEventByte += buf.data.size();
        metrics.mMetadataSizeByte += buf.data.size();
        if (resources.extendedMetaDataSizeBytes < metrics.mMetadataSizeByte)
        {
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }
        ContractEvent evt;
        xdr::xdr_from_opaque(buf.data, evt);
        success.events.emplace_back(evt);
    }

    maybePopulateDiagnosticEvents(cfg, out);
    mParentTx.consumeRefundableSorobanResource(metrics.mMetadataSizeByte);

    xdr::xdr_from_opaque(out.result_value.data, success.returnValue);
    innerResult().code(INVOKE_HOST_FUNCTION_SUCCESS);
    innerResult().success() = xdrSha256(success);

    mParentTx.pushContractEvents(std::move(success.events));
    mParentTx.setReturnValue(std::move(success.returnValue));
    mParentTx.pushInitialExpirations(std::move(originalExpirations));
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
