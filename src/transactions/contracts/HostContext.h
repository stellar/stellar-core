// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/InvokeContractOpFrame.h"
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

// All of our host functions take N i64 inputs and return 1 i64 output. The
// values being passed are (statically) either full/wide i64s or i64s carrying
// smaller values embedded into a HostVal structure. Because of this, host
// function signatures only vary by a single arity number.
using HostClosure0 = std::function<fizzy::ExecutionResult(
    fizzy::Instance&, fizzy::ExecutionContext&)>;
using HostClosure1 = std::function<fizzy::ExecutionResult(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t)>;
using HostClosure2 = std::function<fizzy::ExecutionResult(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t)>;
using HostClosure3 = std::function<fizzy::ExecutionResult(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t)>;
using HostClosure4 = std::function<fizzy::ExecutionResult(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t)>;
using HostClosure5 = std::function<fizzy::ExecutionResult(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t)>;
using HostClosure6 = std::function<fizzy::ExecutionResult(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t)>;

class HostContext;

using HostMemFun0 = fizzy::ExecutionResult (HostContext::*)(
    fizzy::Instance&, fizzy::ExecutionContext&);
using HostMemFun1 = fizzy::ExecutionResult (HostContext::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t);
using HostMemFun2 = fizzy::ExecutionResult (HostContext::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t);
using HostMemFun3 = fizzy::ExecutionResult (HostContext::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t);
using HostMemFun4 = fizzy::ExecutionResult (HostContext::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t);
using HostMemFun5 = fizzy::ExecutionResult (HostContext::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t);
using HostMemFun6 = fizzy::ExecutionResult (HostContext::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t);

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
    std::vector<fizzy::ImportedFunction> mHostFunctions;
    std::map<std::string, HostVal> mEnv;
    std::vector<InvokeContractContext> mInvokeCtxs;

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

    friend class HostContextTxn;

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

    template <typename Closure>
    void
    registerHostFunction(size_t arity, Closure clo,
                         fizzy::HostFunctionPtr dispatcher,
                         std::string const& module, std::string const& name)
    {
        std::vector<fizzy::ValType> inTypes{arity, fizzy::ValType::i64};
        std::optional<fizzy::ValType> outType{fizzy::ValType::i64};
        fizzy::ExecuteFunction func{dispatcher, std::move(clo)};
        fizzy::ImportedFunction ifunc{std::string(module), std::string(name),
                                      std::move(inTypes), std::move(outType),
                                      std::move(func)};
        mHostFunctions.emplace_back(std::move(ifunc));
    }

    void registerHostFunction(HostClosure0 clo, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostClosure1 clo, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostClosure2 clo, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostClosure3 clo, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostClosure4 clo, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostClosure5 clo, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostClosure6 clo, std::string const& module,
                              std::string const& name);

    void registerHostFunction(HostMemFun0 mf, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostMemFun1 mf, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostMemFun2 mf, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostMemFun3 mf, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostMemFun4 mf, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostMemFun5 mf, std::string const& module,
                              std::string const& name);
    void registerHostFunction(HostMemFun6 mf, std::string const& module,
                              std::string const& name);

  public:
    HostContext();

    HostContextTxn
    beginOpTxn(InvokeContractOpFrame& op, AbstractLedgerTxn& ltx)
    {
        releaseAssert(mInvokeCtxs.empty());
        mInvokeCtxs.emplace_back(InvokeContractContext{
            *this, op, ltx, op.getContractOwner(), op.getContractID()});
        return HostContextTxn(*this);
    }

    HostContextTxn
    beginInnerTxn(AbstractLedgerTxn& innerLtx, AccountID const& owner,
                  int64_t contractID)
    {
        releaseAssert(!mInvokeCtxs.empty());
        auto& curr = mInvokeCtxs.back();
        InvokeContractContext next{curr.mHostContext, curr.mInvokeOp, innerLtx,
                                   owner, contractID};
        mInvokeCtxs.emplace_back(std::move(next));
        return HostContextTxn(*this);
    }

    AbstractLedgerTxn&
    getLedgerTxn()
    {
        releaseAssert(!mInvokeCtxs.empty());
        return mInvokeCtxs.back().mLedgerTxn;
    }

    InvokeContractOpFrame&
    getOpFrame()
    {
        releaseAssert(!mInvokeCtxs.empty());
        return mInvokeCtxs.back().mInvokeOp;
    }

    std::variant<HostVal, InvokeContractResultCode>
    invokeContract(AccountID const& owner, int64_t contractID,
                   std::string const& function,
                   std::vector<HostVal> const& args);

    void extendEnvironment(SCEnv const& locals);
    void extendEnvironment(SCSymbol const& sym, HostVal hv);
    std::optional<HostVal> getEnv(std::string const& name) const;

    std::vector<fizzy::ImportedFunction> const&
    getHostFunctions() const
    {
        return mHostFunctions;
    }

    HostVal xdrToHost(SCVal const& v);

    SCVal hostToXdr(HostVal const& hv);

    xdr::pointer<SCObject>
    hostToXdr(std::unique_ptr<HostObject const> const& obj);

    size_t xdrToHost(std::unique_ptr<SCObject> const& obj);

    void registerHostFunctions();

    // Host functions follow -- plumbing above currently supports 0..4 uint64_t
    // arguments.

    fizzy::ExecutionResult mapNew(fizzy::Instance&, fizzy::ExecutionContext&);
    fizzy::ExecutionResult mapPut(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t map, uint64_t key, uint64_t val);
    fizzy::ExecutionResult mapGet(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t map, uint64_t key);
    fizzy::ExecutionResult mapDel(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t map, uint64_t key);
    fizzy::ExecutionResult mapLen(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t map);
    fizzy::ExecutionResult mapKeys(fizzy::Instance&, fizzy::ExecutionContext&,
                                   uint64_t map);
    fizzy::ExecutionResult mapHas(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t map, uint64_t key);

    fizzy::ExecutionResult vecNew(fizzy::Instance&, fizzy::ExecutionContext&);
    fizzy::ExecutionResult vecGet(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t vec, uint64_t idx);
    fizzy::ExecutionResult vecPut(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t vec, uint64_t idx, uint64_t val);
    fizzy::ExecutionResult vecDel(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t vec, uint64_t idx);
    fizzy::ExecutionResult vecLen(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t vec);
    fizzy::ExecutionResult vecPush(fizzy::Instance&, fizzy::ExecutionContext&,
                                   uint64_t vec, uint64_t val);
    fizzy::ExecutionResult vecTake(fizzy::Instance&, fizzy::ExecutionContext&,
                                   uint64_t vec, uint64_t num);
    fizzy::ExecutionResult vecDrop(fizzy::Instance&, fizzy::ExecutionContext&,
                                   uint64_t vec, uint64_t num);
    fizzy::ExecutionResult vecFront(fizzy::Instance&, fizzy::ExecutionContext&,
                                    uint64_t vec);
    fizzy::ExecutionResult vecBack(fizzy::Instance&, fizzy::ExecutionContext&,
                                   uint64_t vec);
    fizzy::ExecutionResult vecPop(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t vec);
    fizzy::ExecutionResult vecInsert(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t vec, uint64_t idx, uint64_t val);
    fizzy::ExecutionResult vecAppend(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t vecA, uint64_t vecB);

    fizzy::ExecutionResult logValue(fizzy::Instance&, fizzy::ExecutionContext&,
                                    uint64_t);
    fizzy::ExecutionResult getCurrentLedgerNum(fizzy::Instance&,
                                               fizzy::ExecutionContext&);
    fizzy::ExecutionResult getCurrentLedgerCloseTime(fizzy::Instance&,
                                                     fizzy::ExecutionContext&);
    fizzy::ExecutionResult getLastOperationResult(fizzy::Instance&,
                                                  fizzy::ExecutionContext&);

    LedgerKey currentContractDataLedgerKey(uint64_t key);
    fizzy::ExecutionResult putContractData(fizzy::Instance&,
                                           fizzy::ExecutionContext&,
                                           uint64_t key, uint64_t val);
    fizzy::ExecutionResult
    hasContractData(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t key);
    fizzy::ExecutionResult
    getContractData(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t key);
    fizzy::ExecutionResult
    delContractData(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t key);

    fizzy::ExecutionResult pay(fizzy::Instance&, fizzy::ExecutionContext&,
                               uint64_t src, uint64_t dst, uint64_t asset,
                               uint64_t amount);

    fizzy::ExecutionResult call0(fizzy::Instance&, fizzy::ExecutionContext&,
                                 uint64_t contract, uint64_t function);

    fizzy::ExecutionResult call1(fizzy::Instance&, fizzy::ExecutionContext&,
                                 uint64_t contract, uint64_t function,
                                 uint64_t a);

    fizzy::ExecutionResult call2(fizzy::Instance&, fizzy::ExecutionContext&,
                                 uint64_t contract, uint64_t function,
                                 uint64_t a, uint64_t b);

    fizzy::ExecutionResult call3(fizzy::Instance&, fizzy::ExecutionContext&,
                                 uint64_t contract, uint64_t function,
                                 uint64_t a, uint64_t b, uint64_t c);

    fizzy::ExecutionResult call4(fizzy::Instance&, fizzy::ExecutionContext&,
                                 uint64_t contract, uint64_t function,
                                 uint64_t a, uint64_t b, uint64_t c,
                                 uint64_t d);

    fizzy::ExecutionResult callN(fizzy::Instance&, fizzy::ExecutionContext&,
                                 uint64_t contract, uint64_t function,
                                 std::vector<HostVal> const& args);

    fizzy::ExecutionResult
    bigNumFromU64(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t rhs);

    fizzy::ExecutionResult bigNumAdd(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumSub(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumMul(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumDiv(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumRem(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);

    fizzy::ExecutionResult bigNumAnd(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumOr(fizzy::Instance&, fizzy::ExecutionContext&,
                                    uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumXor(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumShl(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumShr(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);

    fizzy::ExecutionResult bigNumCmp(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumIsZero(fizzy::Instance&,
                                        fizzy::ExecutionContext&, uint64_t x);

    fizzy::ExecutionResult bigNumNeg(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t x);
    fizzy::ExecutionResult bigNumNot(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t x);

    fizzy::ExecutionResult bigNumGcd(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumLcm(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumPow(fizzy::Instance&, fizzy::ExecutionContext&,
                                     uint64_t lhs, uint64_t rhs);
    fizzy::ExecutionResult bigNumPowMod(fizzy::Instance&,
                                        fizzy::ExecutionContext&, uint64_t p,
                                        uint64_t q, uint64_t m);
    fizzy::ExecutionResult bigNumSqrt(fizzy::Instance&,
                                      fizzy::ExecutionContext&, uint64_t x);
};

}
