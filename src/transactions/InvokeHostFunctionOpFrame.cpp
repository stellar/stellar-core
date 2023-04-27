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

CxxLedgerInfo
getLedgerInfo(AbstractLedgerTxn& ltx, Config const& cfg)
{
    CxxLedgerInfo info;
    auto const& hdr = ltx.loadHeader().current();
    info.base_reserve = hdr.baseReserve;
    info.protocol_version = hdr.ledgerVersion;
    info.sequence_number = hdr.ledgerSeq;
    info.timestamp = hdr.scpValue.closeTime;
    // TODO: move network id to config to not recompute hash
    auto networkID = sha256(cfg.NETWORK_PASSPHRASE);
    for (auto c : networkID)
    {
        info.network_id.push_back(static_cast<unsigned char>(c));
    }
    return info;
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

InvokeHostFunctionOpFrame::InvokeHostFunctionOpFrame(Operation const& op,
                                                     OperationResult& res,
                                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mInvokeHostFunction(mOperation.body.invokeHostFunctionOp())
{
}

ThresholdLevel
InvokeHostFunctionOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}

bool
InvokeHostFunctionOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
InvokeHostFunctionOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error("InvokeHostFunctionOpFrame::doApply needs Config");
}

void
InvokeHostFunctionOpFrame::maybePopulateDiagnosticEvents(
    Config const& cfg, InvokeHostFunctionOutput const& output)
{
    if (cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS)
    {
        OperationDiagnosticEvents diagnosticEvents;
        for (auto const& e : output.diagnostic_events)
        {
            DiagnosticEvent evt;
            xdr::xdr_from_opaque(e.data, evt);
            diagnosticEvents.events.emplace_back(evt);
        }
        mParentTx.pushDiagnosticEvents(diagnosticEvents);
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
InvokeHostFunctionOpFrame::doApply(AbstractLedgerTxn& ltx, Config const& cfg,
                                   medida::MetricsRegistry& metricsReg)
{
    HostFunctionMetrics metrics(metricsReg);

    // Get the entries for the footprint
    rust::Vec<CxxBuf> ledgerEntryCxxBufs;
    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    ledgerEntryCxxBufs.reserve(footprint.readOnly.size() +
                               footprint.readWrite.size());
    auto addReads = [&ledgerEntryCxxBufs, &ltx, &metrics](auto const& keys) {
        for (auto const& lk : keys)
        {
            // Load without record for readOnly to avoid writing them later
            auto ltxe = ltx.loadWithoutRecord(lk);
            size_t nByte{0};
            if (ltxe)
            {
                auto buf = toCxxBuf(ltxe.current());
                nByte = buf.data->size();
                ledgerEntryCxxBufs.emplace_back(std::move(buf));
            }
            metrics.noteReadEntry(lk, nByte);
        }
    };

    addReads(footprint.readOnly);
    addReads(footprint.readWrite);

    rust::Vec<CxxBuf> hostFnCxxBufs;
    hostFnCxxBufs.reserve(mInvokeHostFunction.functions.size());
    for (auto const& hostFn : mInvokeHostFunction.functions)
    {
        hostFnCxxBufs.emplace_back(toCxxBuf(hostFn));
    }

    InvokeHostFunctionOutput out;
    try
    {
        auto timeScope = metrics.getExecTimer();

        out = rust_bridge::invoke_host_functions(
            cfg.CURRENT_LEDGER_PROTOCOL_VERSION,
            cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS, hostFnCxxBufs,
            toCxxBuf(resources), toCxxBuf(getSourceID()),
            getLedgerInfo(ltx, cfg), ledgerEntryCxxBufs);
        metrics.mSuccess = true;

        if (!out.success)
        {
            maybePopulateDiagnosticEvents(cfg, out);

            innerResult().code(INVOKE_HOST_FUNCTION_TRAPPED);
            return false;
        }
    }
    catch (std::exception&)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_TRAPPED);
        return false;
    }

    metrics.mCpuInsn = out.cpu_insns;
    metrics.mMemByte = out.mem_bytes;

    // Create or update every entry returned
    std::unordered_set<LedgerKey> keys;
    for (auto const& buf : out.modified_ledger_entries)
    {
        LedgerEntry le;
        xdr::xdr_from_opaque(buf.data, le);
        auto lk = LedgerEntryKey(le);

        metrics.noteWriteEntry(lk, buf.data.size());

        auto ltxe = ltx.load(lk);
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
    OperationEvents events;
    for (auto const& buf : out.contract_events)
    {
        metrics.mEmitEvent++;
        metrics.mEmitEventByte += buf.data.size();
        ContractEvent evt;
        xdr::xdr_from_opaque(buf.data, evt);
        events.events.push_back(evt);
    }
    mParentTx.pushContractEvents(events);

    maybePopulateDiagnosticEvents(cfg, out);

    innerResult().code(INVOKE_HOST_FUNCTION_SUCCESS);

    auto& results = innerResult().success();
    results.resize(out.result_values.size());
    for (size_t i = 0; i < results.size(); ++i)
    {
        xdr::xdr_from_opaque(out.result_values[i].data, results[i]);
    }

    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    return true;
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
