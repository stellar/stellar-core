// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
#include "util/GlobalChecks.h"
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

struct HostFunctionMetrics
{
    medida::MetricsRegistry& mMetrics;

    bool mPreflight{false};
    char const* mFnType{nullptr};

    size_t mReadEntry{0};
    size_t mWriteEntry{0};

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

    HostFunctionMetrics(medida::MetricsRegistry& metrics, HostFunctionType hf,
                        bool isPreflight)
        : mMetrics(metrics), mPreflight(isPreflight)
    {
        if (hf == HOST_FUNCTION_TYPE_INVOKE_CONTRACT)
        {
            if (isPreflight)
            {
                mFnType = "invoke-preflight";
            }
            else
            {
                mFnType = "invoke-contract";
            }
        }
        else
        {
            if (isPreflight)
            {
                mFnType = "create-preflight";
            }
            else
            {
                mFnType = "create-contract";
            }
        }
    }

    bool
    isCodeKey(LedgerKey const& lk)
    {
        return lk.type() == CONTRACT_DATA &&
               lk.contractData().key.type() == SCValType::SCV_STATIC &&
               lk.contractData().key.ic() == SCS_LEDGER_KEY_CONTRACT_CODE;
    }

    void
    noteReadEntry(LedgerKey const& lk, size_t n)
    {
        mReadEntry++;
        mReadKeyByte += xdr::xdr_size(lk);
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
        mWriteKeyByte += xdr::xdr_size(lk);
        if (isCodeKey(lk))
        {
            mReadCodeByte += n;
        }
        else
        {
            mReadDataByte += n;
        }
    }

    ~HostFunctionMetrics()
    {
        mMetrics.NewMeter({"host-fn", mFnType, "read-entry"}, "entry")
            .Mark(mReadEntry);
        mMetrics.NewMeter({"host-fn", mFnType, "write-entry"}, "entry")
            .Mark(mWriteEntry);

        mMetrics.NewMeter({"host-fn", mFnType, "read-key-byte"}, "byte")
            .Mark(mReadKeyByte);
        mMetrics.NewMeter({"host-fn", mFnType, "write-key-byte"}, "byte")
            .Mark(mWriteKeyByte);

        mMetrics.NewMeter({"host-fn", mFnType, "read-data-byte"}, "byte")
            .Mark(mReadDataByte);
        mMetrics.NewMeter({"host-fn", mFnType, "read-code-byte"}, "byte")
            .Mark(mReadCodeByte);

        if (!mPreflight)
        {
            // We don't track the ledger entry bytes written during preflight as
            // they're not actually written anywhere, just dropped on the floor.
            mMetrics.NewMeter({"host-fn", mFnType, "write-data-byte"}, "byte")
                .Mark(mWriteDataByte);
            mMetrics.NewMeter({"host-fn", mFnType, "write-code-byte"}, "byte")
                .Mark(mWriteCodeByte);
        }

        mMetrics.NewMeter({"host-fn", mFnType, "emit-event"}, "event")
            .Mark(mEmitEvent);
        mMetrics.NewMeter({"host-fn", mFnType, "emit-event-byte"}, "byte")
            .Mark(mEmitEventByte);

        mMetrics.NewMeter({"host-fn", mFnType, "cpu-insn"}, "insn")
            .Mark(mCpuInsn);
        mMetrics.NewMeter({"host-fn", mFnType, "mem-byte"}, "byte")
            .Mark(mMemByte);

        if (mSuccess)
        {
            mMetrics.NewMeter({"host-fn", mFnType, "success"}, "call").Mark();
        }
        else
        {
            mMetrics.NewMeter({"host-fn", mFnType, "failure"}, "call").Mark();
        }
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.NewTimer({"host-fn", mFnType, "exec"}).TimeScope();
    }
};

bool
InvokeHostFunctionOpFrame::doApply(AbstractLedgerTxn& ltx, Config const& cfg,
                                   medida::MetricsRegistry& metricsReg)
{
    HostFunctionMetrics metrics(metricsReg, mInvokeHostFunction.function.type(),
                                false);

    // Get the entries for the footprint
    rust::Vec<CxxBuf> ledgerEntryCxxBufs;
    ledgerEntryCxxBufs.reserve(mInvokeHostFunction.footprint.readOnly.size() +
                               mInvokeHostFunction.footprint.readWrite.size());
    for (auto const& lk : mInvokeHostFunction.footprint.readOnly)
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
    for (auto const& lk : mInvokeHostFunction.footprint.readWrite)
    {
        auto ltxe = ltx.load(lk);
        size_t nByte{0};
        if (ltxe)
        {
            auto buf = toCxxBuf(ltxe.current());
            nByte = buf.data->size();
            ledgerEntryCxxBufs.emplace_back(std::move(buf));
        }
        metrics.noteWriteEntry(lk, nByte);
    }

    rust::Vec<CxxBuf> contractAuthEntryCxxBufs;
    if (mInvokeHostFunction.function.type() ==
        HOST_FUNCTION_TYPE_INVOKE_CONTRACT)
    {
        contractAuthEntryCxxBufs.reserve(mInvokeHostFunction.auth.size());
        for (auto const& authEntry : mInvokeHostFunction.auth)
        {
            contractAuthEntryCxxBufs.push_back(toCxxBuf(authEntry));
        }
    }

    InvokeHostFunctionOutput out;
    try
    {
        auto timeScope = metrics.getExecTimer();
        out = rust_bridge::invoke_host_function(
            toCxxBuf(mInvokeHostFunction.function),
            toCxxBuf(mInvokeHostFunction.footprint), toCxxBuf(getSourceID()),
            contractAuthEntryCxxBufs, getLedgerInfo(ltx, cfg),
            ledgerEntryCxxBufs);
        metrics.mSuccess = true;
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
    for (auto const& lk : mInvokeHostFunction.footprint.readWrite)
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
    for (auto const& buf : out.contract_events)
    {
        metrics.mEmitEvent++;
        metrics.mEmitEventByte += buf.data.size();
        ContractEvent evt;
        xdr::xdr_from_opaque(buf.data, evt);
        mParentTx.pushContractEvent(evt);
    }

    innerResult().code(INVOKE_HOST_FUNCTION_SUCCESS);
    xdr::xdr_from_opaque(out.result_value.data, innerResult().success());
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    if (mParentTx.getNumOperations() > 1)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_MALFORMED);
        return false;
    }
    return true;
}

void
InvokeHostFunctionOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

CxxBuf
PreflightCallbacks::get_ledger_entry(rust::Vec<uint8_t> const& key)
{
    LedgerKey lk;
    xdr::xdr_from_opaque(key, lk);
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto lte = ltx.load(lk);
    if (lte)
    {
        auto buf = toCxxBuf(lte.current());
        mMetrics.noteReadEntry(lk, buf.data->size());
        return buf;
    }
    else
    {
        throw std::runtime_error("missing key for get");
    }
}
bool
PreflightCallbacks::has_ledger_entry(rust::Vec<uint8_t> const& key)
{
    LedgerKey lk;
    xdr::xdr_from_opaque(key, lk);
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto lte = ltx.load(lk);
    mMetrics.noteReadEntry(lk, lte ? xdr::xdr_size(lte.current()) : 0);
    return (bool)lte;
}

Json::Value
InvokeHostFunctionOpFrame::preflight(Application& app,
                                     InvokeHostFunctionOp const& op,
                                     AccountID const& sourceAccount)
{
    HostFunctionMetrics metrics(app.getMetrics(), op.function.type(), true);

    auto cb = std::make_unique<PreflightCallbacks>(app, metrics);

    Json::Value root;
    CxxLedgerInfo li;

    // Scope our use of an ltx to a block here so that a new one can be opened
    // later in the preflight callbacks.
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        root["ledger"] = ltx.loadHeader().current().ledgerSeq;
        li = getLedgerInfo(ltx, app.getConfig());
    }

    try
    {
        PreflightHostFunctionOutput out;
        {
            auto timeScope = metrics.getExecTimer();
            out = rust_bridge::preflight_host_function(
                toVec(op.function), toVec(sourceAccount), li, std::move(cb));
            metrics.mSuccess = true;
        }
        SCVal result_value;
        LedgerFootprint storage_footprint;
        xdr::xdr_from_opaque(out.result_value.data, result_value);
        xdr::xdr_from_opaque(out.storage_footprint.data, storage_footprint);
        root["status"] = "OK";
        root["result"] = toOpaqueBase64(result_value);
        Json::Value events(Json::ValueType::arrayValue);
        for (auto const& e : out.contract_events)
        {
            // TODO: possibly change this to a single XDR-array-of-events rather
            // than a JSON array of separate XDR events; requires changing the
            // XDR if we want to do this.
            metrics.mEmitEvent++;
            metrics.mEmitEventByte += e.data.size();
            ContractEvent evt;
            xdr::xdr_from_opaque(e.data, evt);
            events.append(Json::Value(toOpaqueBase64(evt)));
        }
        root["events"] = events;
        root["footprint"] = toOpaqueBase64(storage_footprint);
        root["cpu_insns"] = Json::UInt64(out.cpu_insns);
        root["mem_bytes"] = Json::UInt64(out.mem_bytes);

        metrics.mCpuInsn = out.cpu_insns;
        metrics.mMemByte = out.mem_bytes;
    }
    catch (std::exception& e)
    {
        root["status"] = "ERROR";
        root["detail"] = e.what();
    }
    return root;
}

}
#endif // ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
