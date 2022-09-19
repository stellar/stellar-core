// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
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

    rust::Vec<Bytes> retBufs;
    try
    {
        retBufs = rust_bridge::invoke_host_function(
            toXDRBuf(mInvokeHostFunction.function),
            toXDRBuf(mInvokeHostFunction.parameters),
            toXDRBuf(mInvokeHostFunction.footprint), ledgerEntryXdrBufs);
    }
    catch (std::exception&)
    {
        innerResult().code(INVOKE_HOST_FUNCTION_TRAPPED);
        return false;
    }

    // Create or update every entry returned
    std::unordered_set<LedgerKey> keys;
    for (auto const& buf : retBufs)
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

struct PreflightResults
{
    LedgerFootprint mFootprint;
    SCVal mResult;
    uint64_t mCpuInsns;
    uint64_t mMemBytes;
};

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
void
PreflightCallbacks::set_result_footprint(rust::Vec<uint8_t> const& footprint)
{
    xdr::xdr_from_opaque(footprint, mRes.mFootprint);
}
void
PreflightCallbacks::set_result_value(rust::Vec<uint8_t> const& value)
{
    xdr::xdr_from_opaque(value, mRes.mResult);
}
void
PreflightCallbacks::set_result_cpu_insns(uint64_t cpu)
{
    mRes.mCpuInsns = cpu;
}
void
PreflightCallbacks::set_result_mem_bytes(uint64_t mem)
{
    mRes.mMemBytes = mem;
}

Json::Value
InvokeHostFunctionOpFrame::preflight(Application& app,
                                     InvokeHostFunctionOp const& op)
{
    PreflightResults res;
    auto cb = std::make_unique<PreflightCallbacks>(app, res);
    Json::Value root;
    try
    {
        rust_bridge::preflight_host_function(
            toXDRVec(op.function), toXDRVec(op.parameters), std::move(cb));
        root["status"] = "OK";
        root["result"] = toOpaqueBase64(res.mResult);
        root["footprint"] = toOpaqueBase64(res.mFootprint);
        root["cpu_insns"] = Json::UInt64(res.mCpuInsns);
        root["mem_bytes"] = Json::UInt64(res.mMemBytes);
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
