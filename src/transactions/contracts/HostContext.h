#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
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
#include <xdrpp/xdrpp/types.h>

namespace stellar
{

struct InvokeContractContext
{
    AbstractLedgerTxn& mLedgerTxn;
    // TODO: add references to any other context
    // of a contract-invocation here.
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
    std::vector<InvokeContractContext> mInvokeCtxs;
    HostFunctions mHostFunctions;

    friend class HostContextTxn;
    friend class HostFunctions;

  public:
    HostContext();

    HostContextTxn beginTxn(AbstractLedgerTxn& outerLtx);

    HostFunctions&
    getHostFunctions()
    {
        return mHostFunctions;
    }

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

    HostVal xdrToHost(SCVal const& v);

    SCVal hostToXdr(HostVal const& hv);

    xdr::pointer<SCObject>
    hostToXdr(std::unique_ptr<HostObject const> const& obj);

    size_t xdrToHost(std::unique_ptr<SCObject> const& obj);

    template <typename X>
    std::unique_ptr<HostObject const>
    xdrPointerToHostObjectPointer(xdr::pointer<X> const& ptr)
    {
        if (!ptr)
        {
            return nullptr;
        }
        xdr::pointer<X> clone;
        clone.activate();
        *clone = *ptr;
        return std::make_unique<HostObject const>(std::move(clone));
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
    getObject(HostVal const& hv) const
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

    template <typename ObjType>
    fizzy::ExecutionResult
    objMethod(uint64_t objID,
              std::function<fizzy::ExecutionResult(ObjType const&)> fn) const
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

    std::optional<fizzy::ExecutionResult>
    invokeWasmFunction(std::vector<uint8_t> const& wasmModule,
                       std::string const& function,
                       std::vector<HostVal> const& args) const;
};

}
