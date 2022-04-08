#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/InvokeContractOpFrame.h"
#include "transactions/contracts/HostFunctions.h"
#include "transactions/contracts/HostVal.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"

#include <cstdint>
#include <fizzy/execute.hpp>
#include <map>
#include <stdexcept>
#include <variant>

namespace stellar
{

class InvokeContractOpFrame;
struct InvokeContractContext
{
    HostContext& mHostContext;
    InvokeContractOpFrame& mInvokeOp;
    AbstractLedgerTxn& mLedgerTxn;
    AccountID mContractOwner;
    int64_t mContractID;
};
class HostContextTxn
{
    bool mRollback{true};
    size_t mRollbackPoint;
    HostContext& mHostCtx;
    HostContextTxn(HostContext& hc);
    friend class HostContext;

  public:
    ~HostContextTxn();
    void
    commit()
    {
        mRollback = false;
    }
};

class HostContext
{
    std::vector<std::unique_ptr<HostObject const>> mObjects;
    std::map<std::string, HostVal> mEnv;
    std::vector<InvokeContractContext> mInvokeCtxs;
    UnorderedSet<LedgerKey> mReadSet;
    UnorderedSet<LedgerKey> mWriteSet;
    HostFunctions mHostFunctions;

    friend class HostContextTxn;
    friend class HostFunctions;

    // mLastOperationResult is set by any host function that
    // calls an XDR operation or invokes cross-contract. We
    // return just a Status value by default, since this is
    // probably enough for most users, and there's no reason
    // to chew up an object slot with an OperationResult
    // unless the user asks to materialize "the last one".
    //
    // We also store any host trap code that occurs while
    // processing a host function in here, so that the
    // caller of a nested invocation can get the cause of
    // a host trap.
    OperationResult mLastOperationResult;

  public:
    HostContext();

    HostContextTxn beginOpTxn(InvokeContractOpFrame& op,
                              AbstractLedgerTxn& ltx);

    HostContextTxn beginInnerTxn(AbstractLedgerTxn& innerLtx,
                                 AccountID const& owner, int64_t contractID);

    InvokeContractContext const&
    getInvokeContext() const
    {
        releaseAssert(!mInvokeCtxs.empty());
        return mInvokeCtxs.back();
    }

    AbstractLedgerTxn&
    getLedgerTxn() const
    {
        return getInvokeContext().mLedgerTxn;
    }

    InvokeContractOpFrame&
    getOpFrame() const
    {
        return getInvokeContext().mInvokeOp;
    }

    InvokeContractOp const&
    getOp() const
    {
        return getOpFrame().getOperation().body.invokeContractOp();
    }

    bool
    writePermitted(LedgerKey const& lk)
    {
        return mWriteSet.find(lk) != mWriteSet.end();
    }

    bool
    readPermitted(LedgerKey const& lk)
    {
        return writePermitted(lk) || mReadSet.find(lk) != mReadSet.end();
    }

    void extendEnvironment(SCEnv const& locals);
    void extendEnvironment(SCSymbol const& sym, HostVal hv);
    std::optional<HostVal> getEnv(std::string const& name) const;

    HostVal xdrToHost(SCVal const& v);

    SCVal hostToXdr(HostVal const& hv);

    xdr::pointer<SCObject>
    hostToXdr(std::unique_ptr<HostObject const> const& obj);

    size_t xdrToHost(std::unique_ptr<SCObject> const& obj);

    fizzy::ExecutionResult
    hostTrap(HostTrapCode code)
    {
        mLastOperationResult.code(opINNER);
        mLastOperationResult.tr().type(INVOKE_CONTRACT);
        mLastOperationResult.tr().invokeContractResult().code(
            INVOKE_CONTRACT_TRAPPED);
        mLastOperationResult.tr().invokeContractResult().trap().type(
            HOST_TRAPPED);
        mLastOperationResult.tr().invokeContractResult().trap().hostTrap() =
            code;
        return fizzy::Trap;
    }

    OperationResult const&
    getLastOperationResult() const
    {
        return mLastOperationResult;
    }

    template <typename HObj, typename... Args>
    HostVal
    newObject(Args&&... args)
    {
        size_t idx = mObjects.size();
        HObj obj(std::forward<Args>(args)...);
        mObjects.emplace_back(
            std::make_unique<HostObject const>(std::move(obj)));
        return HostVal::fromObject<HObj>(idx);
    }

    std::unique_ptr<HostObject const> const&
    getObject(HostVal const& hv)
    {
        if (hv.isObject() && hv.asObject() < mObjects.size())
        {
            return mObjects.at(hv.asObject());
        }
        else
        {
            return mObjects.at(0);
        }
    }

    template <typename LedgerKeyT>
    std::optional<LedgerKeyT>
    getLedgerKeyObject(HostVal const& hv)
    {
        auto const& ptr = getObject(hv);
        if (ptr && std::holds_alternative<LedgerKey>(*ptr))
        {
            return std::make_optional(std::get<LedgerKey>(*ptr));
        }
        return std::nullopt;
    }

    template <typename ObjType>
    fizzy::ExecutionResult
    objMethod(uint64_t objID,
              std::function<fizzy::ExecutionResult(ObjType const&)> fn)
    {
        auto v = HostVal::fromPayload(objID);
        auto const& objPtr = getObject(v);
        if (objPtr && std::holds_alternative<ObjType>(*objPtr))
        {
            return fn(std::get<ObjType>(*objPtr));
        }
        else
        {
            return fizzy::Trap;
        }
    }

    std::variant<HostVal, InvokeContractResultCode>
    invokeContract(AccountID const& owner, int64_t contractID,
                   std::string const& function,
                   std::vector<HostVal> const& args);
};

}
