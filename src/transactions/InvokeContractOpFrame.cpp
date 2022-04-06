// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/InvokeContractOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/TransactionUtils.h"
#include "transactions/contracts/HostContext.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include <Tracy.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <xdrpp/marshal.h>
namespace stellar
{
InvokeContractOpFrame::InvokeContractOpFrame(Operation const& op,
                                             OperationResult& res,
                                             TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mInvokeContract(mOperation.body.invokeContractOp())
{
}

bool
InvokeContractOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 19;
}

}

namespace stellar
{

bool
InvokeContractOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "InvokeContractOp apply", true);
    LedgerTxn ltxInner(ltx);
    HostContext& hostCtx = mParentTx.getHostContext();
    HostContextTxn htx(hostCtx.beginOpTxn(*this, ltxInner));

    try
    {
        CLOG_INFO(Tx, "invoke contract op: contract {} func {}",
                  mInvokeContract.contractID, mInvokeContract.function);

        // Extend the local environment.
        hostCtx.extendEnvironment(mInvokeContract.locals);

        // Look up requested symbols and pass as args to func.
        std::vector<HostVal> hostArgs;
        size_t i = 0;
        for (SCSymbol const& argName : mInvokeContract.arguments)
        {
            auto a = hostCtx.getEnv(argName);
            if (!a)
            {
                CLOG_WARNING(Tx, "Can't find '{}' in environment", argName);
                innerResult().code(INVOKE_CONTRACT_MALFORMED);
                return false;
            }
            CLOG_INFO(Tx, "   arg {}: env[{}]={}", i, argName, *a);
            hostArgs.emplace_back(*a);
            ++i;
        }
        auto result = hostCtx.invokeContract(
            mInvokeContract.owner, mInvokeContract.contractID,
            mInvokeContract.function, hostArgs);

        if (std::holds_alternative<HostVal>(result))
        {
            innerResult().code(INVOKE_CONTRACT_SUCCESS);
            HostVal hv = std::get<HostVal>(result);
            if (mInvokeContract.definition)
            {
                CLOG_INFO(Tx, "storing contract result {} in env[{}]", hv,
                          *mInvokeContract.definition);
                hostCtx.extendEnvironment(*mInvokeContract.definition, hv);
            }
            innerResult().returnVal() = hostCtx.hostToXdr(hv);
        }
        else
        {
            auto code = std::get<InvokeContractResultCode>(result);
            innerResult().code(code);
            if (code == INVOKE_CONTRACT_TRAPPED)
            {
                innerResult().trap().type(GUEST_TRAPPED);
                innerResult().trap().guestTrap().type(CONTRACT_CODE_WASM);
                innerResult().trap().guestTrap().wasmTrap() =
                    WASM_TRAP_UNSPECIFIED;
            }
            return false;
        }
    }
    catch (std::runtime_error& e)
    {
        CLOG_WARNING(Tx, "contract failed: {}", e.what());
        innerResult().code(INVOKE_CONTRACT_HOST_ERR);
        return false;
    }

    htx.commit();
    ltxInner.commit();
    innerResult().code(INVOKE_CONTRACT_SUCCESS);
    return true;
}

bool
InvokeContractOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    return true;
}

void
InvokeContractOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(
        contractCodeKey(mInvokeContract.owner, mInvokeContract.contractID));
    keys.emplace(
        contractDataKey(mInvokeContract.owner, mInvokeContract.contractID));
}
}
