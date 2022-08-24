// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
#include "util/GlobalChecks.h"
#include <json/json.h>
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
    for (auto c : cfg.NETWORK_PASSPHRASE)
    {
        info.network_passphrase.push_back(static_cast<unsigned char>(c));
    }
    return info;
}

template <typename T>
std::vector<uint8_t>
toXDRVec(T const& t)
{
    return std::vector<uint8_t>(xdr::xdr_to_opaque(t));
}

template <typename T>
XDRBuf
toXDRBuf(T const& t)
{
    return XDRBuf{std::make_unique<std::vector<uint8_t>>(toXDRVec(t))};
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

bool
InvokeHostFunctionOpFrame::doApply(AbstractLedgerTxn& ltx, Config const& cfg)
{
    // Get the entries for the footprint
    rust::Vec<XDRBuf> ledgerEntryXdrBufs;
    ledgerEntryXdrBufs.reserve(mInvokeHostFunction.footprint.readOnly.size() +
                               mInvokeHostFunction.footprint.readWrite.size());
    for (auto const& lk : mInvokeHostFunction.footprint.readOnly)
    {
        // Load without record for readOnly to avoid writing them later
        auto ltxe = ltx.loadWithoutRecord(lk);
        if (ltxe)
        {
            ledgerEntryXdrBufs.emplace_back(toXDRBuf(ltxe.current()));
        }
    }
    for (auto const& lk : mInvokeHostFunction.footprint.readWrite)
    {
        auto ltxe = ltx.load(lk);
        if (ltxe)
        {
            ledgerEntryXdrBufs.emplace_back(toXDRBuf(ltxe.current()));
        }
    }

    InvokeHostFunctionOutput out;
    try
    {
        out = rust_bridge::invoke_host_function(
            toXDRBuf(mInvokeHostFunction.function),
            toXDRBuf(mInvokeHostFunction.parameters),
            toXDRBuf(mInvokeHostFunction.footprint), toXDRBuf(getSourceID()),
            getLedgerInfo(ltx, cfg), ledgerEntryXdrBufs);
    }
    catch (std::exception&)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_TRAPPED);
        return false;
    }

    // Create or update every entry returned
    std::unordered_set<LedgerKey> keys;
    for (auto const& buf : out.modified_ledger_entries)
    {
        LedgerEntry le;
        xdr::xdr_from_opaque(buf.vec, le);
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

    // TODO: plumb contract events out to the txmeta when it is updated to
    // accept them.

    innerResult().code(INVOKE_HOST_FUNCTION_SUCCESS);
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

XDRBuf
PreflightCallbacks::get_ledger_entry(rust::Vec<uint8_t> const& key)
{
    LedgerKey lk;
    xdr::xdr_from_opaque(key, lk);
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto lte = ltx.load(lk);
    if (lte)
    {
        return toXDRBuf(lte.current());
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
    return (bool)lte;
}

Json::Value
InvokeHostFunctionOpFrame::preflight(Application& app,
                                     InvokeHostFunctionOp const& op,
                                     AccountID const& sourceAccount)
{
    auto cb = std::make_unique<PreflightCallbacks>(app);
    Json::Value root;
    try
    {
       LedgerTxn ltx(app.getLedgerTxnRoot()); 
        PreflightHostFunctionOutput out = rust_bridge::preflight_host_function(
            toXDRVec(op.function), toXDRVec(op.parameters), toXDRVec(sourceAccount), getLedgerInfo(ltx, app.getConfig()), std::move(cb));
        SCVal result_value;
        LedgerFootprint storage_footprint;
        xdr::xdr_from_opaque(out.result_value.vec, result_value);
        xdr::xdr_from_opaque(out.storage_footprint.vec, storage_footprint);
        root["status"] = "OK";
        root["result"] = toOpaqueBase64(result_value);
        Json::Value events(Json::ValueType::arrayValue);
        for (auto const& e : out.contract_events)
        {
            // TODO: possibly change this to a single XDR-array-of-events rather
            // than a JSON array of separate XDR events; requires changing the
            // XDR if we want to do this.
            ContractEvent evt;
            xdr::xdr_from_opaque(e.vec, evt);
            events.append(Json::Value(toOpaqueBase64(evt)));
        }
        root["events"] = events;
        root["footprint"] = toOpaqueBase64(storage_footprint);
        root["cpu_insns"] = Json::UInt64(out.cpu_insns);
        root["mem_bytes"] = Json::UInt64(out.mem_bytes);
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
