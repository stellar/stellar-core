// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
#include "rust/RustVecXdrMarshal.h"
// clang-format on
#include "transactions/InvokeHostFunctionOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "rust/RustBridge.h"

namespace stellar
{

template <typename T>
XDRBuf
toXDRBuf(T const& t)
{
    return XDRBuf{
        std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(t))};
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
    catch (std::exception& e)
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
}
