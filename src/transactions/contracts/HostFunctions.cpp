#include "transactions/contracts/HostFunctions.h"
#include "crypto/Hex.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/contracts/HostContext.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"

#include <Tracy.hpp>
#include <fizzy/execute.hpp>
#include <fizzy/parser.hpp>
#include <optional>

namespace stellar
{

#pragma region bindings
// The host function signature fizzy expects to have registered with it is:
//
// using HostFunctionPtr = ExecutionResult (*)(std::any& host_context,
//    Instance&, const Value* args, ExecutionContext& ctx) noexcept;
//
// This isn't _quite_ what we want to be defining: we'd like to have our
// arguments unpacked and be calling a member function on HostContext. So we
// register as host_context a closure that captures the HostContext
// member-function pointer, and pass as HostFunctionPtr a dispatcher function
// that downcasts the any to the appropriate closure type, extracts args and
// calls the closure.

fizzy::ExecutionResult
dispatchClosure0(std::any& host_context, fizzy::Instance& instance,
                 const fizzy::Value* args,
                 fizzy::ExecutionContext& ctx) noexcept
{
    auto closure0 = std::any_cast<HostClosure0>(host_context);
    return closure0(instance, ctx);
}

fizzy::ExecutionResult
dispatchClosure1(std::any& host_context, fizzy::Instance& instance,
                 const fizzy::Value* args,
                 fizzy::ExecutionContext& ctx) noexcept
{
    auto closure1 = std::any_cast<HostClosure1>(host_context);
    return closure1(instance, ctx, args[0].as<uint64_t>());
}

fizzy::ExecutionResult
dispatchClosure2(std::any& host_context, fizzy::Instance& instance,
                 const fizzy::Value* args,
                 fizzy::ExecutionContext& ctx) noexcept
{
    auto closure2 = std::any_cast<HostClosure2>(host_context);
    return closure2(instance, ctx, args[0].as<uint64_t>(),
                    args[1].as<uint64_t>());
}

fizzy::ExecutionResult
dispatchClosure3(std::any& host_context, fizzy::Instance& instance,
                 const fizzy::Value* args,
                 fizzy::ExecutionContext& ctx) noexcept
{
    auto closure3 = std::any_cast<HostClosure3>(host_context);
    return closure3(instance, ctx, args[0].as<uint64_t>(),
                    args[1].as<uint64_t>(), args[2].as<uint64_t>());
}

fizzy::ExecutionResult
dispatchClosure4(std::any& host_context, fizzy::Instance& instance,
                 const fizzy::Value* args,
                 fizzy::ExecutionContext& ctx) noexcept
{
    auto closure4 = std::any_cast<HostClosure4>(host_context);
    return closure4(instance, ctx, args[0].as<uint64_t>(),
                    args[1].as<uint64_t>(), args[2].as<uint64_t>(),
                    args[3].as<uint64_t>());
}

fizzy::ExecutionResult
dispatchClosure5(std::any& host_context, fizzy::Instance& instance,
                 const fizzy::Value* args,
                 fizzy::ExecutionContext& ctx) noexcept
{
    auto closure5 = std::any_cast<HostClosure5>(host_context);
    return closure5(instance, ctx, args[0].as<uint64_t>(),
                    args[1].as<uint64_t>(), args[2].as<uint64_t>(),
                    args[3].as<uint64_t>(), args[4].as<uint64_t>());
}

fizzy::ExecutionResult
dispatchClosure6(std::any& host_context, fizzy::Instance& instance,
                 const fizzy::Value* args,
                 fizzy::ExecutionContext& ctx) noexcept
{
    auto closure6 = std::any_cast<HostClosure6>(host_context);
    return closure6(instance, ctx, args[0].as<uint64_t>(),
                    args[1].as<uint64_t>(), args[2].as<uint64_t>(),
                    args[3].as<uint64_t>(), args[4].as<uint64_t>(),
                    args[5].as<uint64_t>());
}

void
HostFunctions::registerHostFunction(HostClosure0 clo, std::string const& module,
                                    std::string const& name)
{
    registerHostFunction(0, clo, &dispatchClosure0, module, name);
}

void
HostFunctions::registerHostFunction(HostClosure1 clo, std::string const& module,
                                    std::string const& name)
{
    registerHostFunction(1, clo, &dispatchClosure1, module, name);
}

void
HostFunctions::registerHostFunction(HostClosure2 clo, std::string const& module,
                                    std::string const& name)
{
    registerHostFunction(2, clo, &dispatchClosure2, module, name);
}

void
HostFunctions::registerHostFunction(HostClosure3 clo, std::string const& module,
                                    std::string const& name)
{
    registerHostFunction(3, clo, &dispatchClosure3, module, name);
}

void
HostFunctions::registerHostFunction(HostClosure4 clo, std::string const& module,
                                    std::string const& name)
{
    registerHostFunction(4, clo, &dispatchClosure4, module, name);
}

void
HostFunctions::registerHostFunction(HostClosure5 clo, std::string const& module,
                                    std::string const& name)
{
    registerHostFunction(5, clo, &dispatchClosure5, module, name);
}

void
HostFunctions::registerHostFunction(HostClosure6 clo, std::string const& module,
                                    std::string const& name)
{
    registerHostFunction(6, clo, &dispatchClosure6, module, name);
}

void
HostFunctions::registerHostFunction(HostMemFun0 mf, std::string const& module,
                                    std::string const& name)
{
    using namespace std::placeholders;
    HostClosure0 clo{std::bind(mf, this, _1, _2)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostFunctions::registerHostFunction(HostMemFun1 mf, std::string const& module,
                                    std::string const& name)
{
    using namespace std::placeholders;
    HostClosure1 clo{std::bind(mf, this, _1, _2, _3)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostFunctions::registerHostFunction(HostMemFun2 mf, std::string const& module,
                                    std::string const& name)
{
    using namespace std::placeholders;
    HostClosure2 clo{std::bind(mf, this, _1, _2, _3, _4)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostFunctions::registerHostFunction(HostMemFun3 mf, std::string const& module,
                                    std::string const& name)
{
    using namespace std::placeholders;
    HostClosure3 clo{std::bind(mf, this, _1, _2, _3, _4, _5)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostFunctions::registerHostFunction(HostMemFun4 mf, std::string const& module,
                                    std::string const& name)
{
    using namespace std::placeholders;
    HostClosure4 clo{std::bind(mf, this, _1, _2, _3, _4, _5, _6)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostFunctions::registerHostFunction(HostMemFun5 mf, std::string const& module,
                                    std::string const& name)
{
    using namespace std::placeholders;
    HostClosure5 clo{std::bind(mf, this, _1, _2, _3, _4, _5, _6, _7)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostFunctions::registerHostFunction(HostMemFun6 mf, std::string const& module,
                                    std::string const& name)
{
    using namespace std::placeholders;
    HostClosure6 clo{std::bind(mf, this, _1, _2, _3, _4, _5, _6, _7, _8)};
    registerHostFunction(std::move(clo), module, name);
}

#pragma endregion bindings

#pragma region hostfns
fizzy::ExecutionResult
HostFunctions::mapNew(fizzy::Instance& instance, fizzy::ExecutionContext& exec)
{
    ZoneScoped;
    return mCtx.newObject<HostMap>();
}

fizzy::ExecutionResult
HostFunctions::mapPut(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                      uint64_t map, uint64_t key, uint64_t val)
{
    ZoneScoped;
    auto keyV = HostVal::fromPayload(key);
    auto valV = HostVal::fromPayload(val);
    return mCtx.objMethod<HostMap>(map, [&](HostMap const& map) {
        return mCtx.newObject<HostMap>(map.set(keyV, valV));
    });
}

fizzy::ExecutionResult
HostFunctions::mapGet(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                      uint64_t map, uint64_t key)
{
    ZoneScoped;
    auto keyV = HostVal::fromPayload(key);
    return mCtx.objMethod<HostMap>(map, [&](HostMap const& map) {
        auto* valPtr = map.find(keyV);
        if (valPtr)
        {
            return fizzy::ExecutionResult(*valPtr);
        }
        return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
    });
}

fizzy::ExecutionResult
HostFunctions::mapHas(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                      uint64_t map, uint64_t key)
{
    ZoneScoped;
    auto keyV = HostVal::fromPayload(key);
    return mCtx.objMethod<HostMap>(map, [&](HostMap const& map) {
        auto* valPtr = map.find(keyV);
        return HostVal::fromBool(bool(valPtr));
    });
}

fizzy::ExecutionResult
HostFunctions::mapDel(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t map,
                      uint64_t key)
{
    ZoneScoped;
    auto keyV = HostVal::fromPayload(key);
    return mCtx.objMethod<HostMap>(map, [&](HostMap const& map) {
        return mCtx.newObject<HostMap>(map.erase(keyV));
    });
}

fizzy::ExecutionResult
HostFunctions::mapLen(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t map)
{
    ZoneScoped;
    return mCtx.objMethod<HostMap>(map, [&](HostMap const& map) {
        size_t sz = map.size();
        if (sz <= UINT32_MAX)
        {
            return fizzy::ExecutionResult(HostVal::fromU32(uint32_t(sz)));
        }
        return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
    });
}

fizzy::ExecutionResult
HostFunctions::mapKeys(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t map)
{
    ZoneScoped;
    return mCtx.objMethod<HostMap>(map, [&](HostMap const& map) {
        std::vector<HostVal> vec;
        for (auto const& i : map)
        {
            vec.emplace_back(i.first);
        }
        // FIXME: do we want a deep
        // structural-comparison sort?
        std::sort(vec.begin(), vec.end());
        return mCtx.newObject<HostVec>(vec.begin(), vec.end());
    });
}

fizzy::ExecutionResult
HostFunctions::vecNew(fizzy::Instance&, fizzy::ExecutionContext&)
{
    ZoneScoped;
    return mCtx.newObject<HostVec>();
}

fizzy::ExecutionResult
HostFunctions::vecGet(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                      uint64_t idx)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!idxV.isU32())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (idxV.asU32() >= vec.size())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
        }
        return fizzy::ExecutionResult(vec.at(idxV.asU32()));
    });
}
fizzy::ExecutionResult
HostFunctions::vecPut(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                      uint64_t idx, uint64_t val)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    auto valV = HostVal::fromPayload(val);
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!idxV.isU32())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (idxV.asU32() >= vec.size())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
        }
        return fizzy::ExecutionResult(
            mCtx.newObject<HostVec>(vec.set(idxV.asU32(), valV)));
    });
}
fizzy::ExecutionResult
HostFunctions::vecDel(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                      uint64_t idx)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!idxV.isU32())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (idxV.asU32() >= vec.size())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
        }
        return fizzy::ExecutionResult(
            mCtx.newObject<HostVec>(vec.erase(idxV.asU32())));
    });
}
fizzy::ExecutionResult
HostFunctions::vecLen(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        size_t sz = vec.size();
        if (sz <= UINT32_MAX)
        {
            return fizzy::ExecutionResult(HostVal::fromU32(sz));
        }
        return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
    });
}
fizzy::ExecutionResult
HostFunctions::vecPush(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                       uint64_t val)
{
    ZoneScoped;
    auto valV = HostVal::fromPayload(val);
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        return mCtx.newObject<HostVec>(vec.push_back(valV));
    });
}
fizzy::ExecutionResult
HostFunctions::vecTake(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                       uint64_t num)
{
    ZoneScoped;
    auto numV = HostVal::fromPayload(num);
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (numV.isU32())
        {
            return fizzy::ExecutionResult(
                mCtx.newObject<HostVec>(vec.take(numV.asU32())));
        }
        return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
    });
}
fizzy::ExecutionResult
HostFunctions::vecDrop(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                       uint64_t num)
{
    ZoneScoped;
    auto numV = HostVal::fromPayload(num);
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (numV.isU32())
        {
            return fizzy::ExecutionResult(
                mCtx.newObject<HostVec>(vec.drop(numV.asU32())));
        }
        return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
    });
}
fizzy::ExecutionResult
HostFunctions::vecPop(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return fizzy::ExecutionResult(
                mCtx.newObject<HostVec>(vec.erase(vec.size() - 1)));
        }
        return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
    });
}

fizzy::ExecutionResult
HostFunctions::vecFront(fizzy::Instance&, fizzy::ExecutionContext&,
                        uint64_t vec)
{
    ZoneScoped;
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return fizzy::ExecutionResult(vec.front());
        }
        return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
    });
}

fizzy::ExecutionResult
HostFunctions::vecBack(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return fizzy::ExecutionResult(vec.back());
        }
        return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
    });
}

fizzy::ExecutionResult
HostFunctions::vecInsert(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t vec, uint64_t idx, uint64_t val)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    auto valV = HostVal::fromPayload(val);
    return mCtx.objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!idxV.isU32())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (idxV.asU32() >= vec.size())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
        }
        return fizzy::ExecutionResult(
            mCtx.newObject<HostVec>(vec.insert(idxV.asU32(), valV)));
    });
}
fizzy::ExecutionResult
HostFunctions::vecAppend(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t vecA, uint64_t vecB)
{
    ZoneScoped;
    return mCtx.objMethod<HostVec>(vecA, [&](HostVec const& vecA) {
        return mCtx.objMethod<HostVec>(vecB, [&](HostVec const& vecB) {
            return mCtx.newObject<HostVec>(vecA + vecB);
        });
    });
}

fizzy::ExecutionResult
HostFunctions::logValue(fizzy::Instance& instance,
                        fizzy::ExecutionContext& exec, uint64_t val)
{
    ZoneScoped;
    CLOG_INFO(Tx, "contract called log_value({})", HostVal::fromPayload(val));
    return HostVal::fromVoid();
}

fizzy::ExecutionResult
HostFunctions::getCurrentLedgerNum(fizzy::Instance& instance,
                                   fizzy::ExecutionContext& exec)
{
    ZoneScoped;
    uint32_t num = mCtx.getLedgerTxn().loadHeader().current().ledgerSeq;
    return HostVal::fromU32(num);
}

fizzy::ExecutionResult
HostFunctions::getCurrentLedgerCloseTime(fizzy::Instance& instance,
                                         fizzy::ExecutionContext& exec)
{
    ZoneScoped;
    // NB: this returns a raw u64, not a HostVal.
    TimePoint closeTime =
        mCtx.getLedgerTxn().loadHeader().current().scpValue.closeTime;
    return fizzy::Value{closeTime};
}

LedgerKey
HostFunctions::currentContractDataLedgerKey(uint64_t key)
{
    ZoneScoped;
    HostVal keyV = HostVal::fromPayload(key);
    SCVal keyX = mCtx.hostToXdr(keyV);
    LedgerKey lk;
    lk.type(CONTRACT_DATA);
    lk.contractData().owner = mCtx.getInvokeContext().mContractOwner;
    lk.contractData().contractID = mCtx.getInvokeContext().mContractID;
    lk.contractData().key.activate();
    *lk.contractData().key = keyX;
    return lk;
}

fizzy::ExecutionResult
HostFunctions::putContractData(fizzy::Instance&, fizzy::ExecutionContext&,
                               uint64_t key, uint64_t val)
{
    ZoneScoped;
    LedgerKey lk = currentContractDataLedgerKey(key);
    if (!mCtx.writePermitted(lk))
    {
        return mCtx.hostTrap(HOST_TRAP_LEDGER_ENTRY_DENIED);
    }
    HostVal keyV = HostVal::fromPayload(key);
    HostVal valV = HostVal::fromPayload(val);
    auto& ltx = mCtx.getLedgerTxn();

    auto lte = ltx.load(lk);
    if (lte)
    {
        CLOG_TRACE(Tx, "putContractData updating existing entry under {}",
                   keyV);
        releaseAssert(lte.current().data.contractData().val);
        *lte.current().data.contractData().val = mCtx.hostToXdr(valV);
    }
    else
    {
        CLOG_TRACE(Tx, "putContractData creating new entry under {}", keyV);
        LedgerEntry le;
        le.data.type(CONTRACT_DATA);
        le.data.contractData().owner = lk.contractData().owner;
        le.data.contractData().contractID = lk.contractData().contractID;
        le.data.contractData().key.activate();
        le.data.contractData().val.activate();
        *le.data.contractData().key = *lk.contractData().key;
        *le.data.contractData().val = mCtx.hostToXdr(valV);
        ltx.create(le);
    }
    return HostVal::fromVoid();
}

fizzy::ExecutionResult
HostFunctions::hasContractData(fizzy::Instance&, fizzy::ExecutionContext&,
                               uint64_t key)
{
    ZoneScoped;
    LedgerKey lk = currentContractDataLedgerKey(key);
    if (!mCtx.readPermitted(lk))
    {
        return mCtx.hostTrap(HOST_TRAP_LEDGER_ENTRY_DENIED);
    }
    auto& ltx = mCtx.getLedgerTxn();
    auto lte = ltx.load(lk);
    return HostVal::fromBool(bool(lte));
}

fizzy::ExecutionResult
HostFunctions::getContractData(fizzy::Instance&, fizzy::ExecutionContext&,
                               uint64_t key)
{
    ZoneScoped;
    LedgerKey lk = currentContractDataLedgerKey(key);
    if (!mCtx.readPermitted(lk))
    {
        return mCtx.hostTrap(HOST_TRAP_LEDGER_ENTRY_DENIED);
    }
    HostVal keyV = HostVal::fromPayload(key);
    auto& ltx = mCtx.getLedgerTxn();
    auto lte = ltx.load(lk);
    if (lte)
    {
        CLOG_TRACE(Tx, "getContractData found entry under {}", keyV);
        lte.current().data.contractData().val.activate();
        return mCtx.xdrToHost(*lte.current().data.contractData().val);
    }
    else
    {
        CLOG_TRACE(Tx, "getContractData missing entry under {}", keyV);
        return mCtx.hostTrap(HOST_TRAP_VALUE_NOT_FOUND);
    }
}

fizzy::ExecutionResult
HostFunctions::delContractData(fizzy::Instance&, fizzy::ExecutionContext&,
                               uint64_t key)
{
    ZoneScoped;
    LedgerKey lk = currentContractDataLedgerKey(key);
    if (!mCtx.writePermitted(lk))
    {
        return mCtx.hostTrap(HOST_TRAP_LEDGER_ENTRY_DENIED);
    }
    auto& ltx = mCtx.getLedgerTxn();
    ltx.erase(lk);
    return HostVal::fromVoid();
}

fizzy::ExecutionResult
HostFunctions::getLastOperationResult(fizzy::Instance&,
                                      fizzy::ExecutionContext&)
{
    return mCtx.newObject<OperationResult>(mCtx.mLastOperationResult);
}

fizzy::ExecutionResult
HostFunctions::pay(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t src,
                   uint64_t dst, uint64_t asset, uint64_t amount)
{
    ZoneScoped;
    CLOG_INFO(Tx, "pay({},{},{},{})", src, dst, asset, amount);

    return mCtx.objMethod<LedgerKey>(src, [&](LedgerKey const& srcLK) {
        return mCtx.objMethod<LedgerKey>(dst, [&](LedgerKey const& dstLK) {
            return mCtx.objMethod<SCLedgerVal>(asset, [&](SCLedgerVal const&
                                                              assetV) {
                return mCtx.objMethod<SCLedgerVal>(
                    amount, [&](SCLedgerVal const& amountV) {
                        if (srcLK.type() == ACCOUNT &&
                            dstLK.type() == ACCOUNT &&
                            assetV.type() == SCLV_ASSET &&
                            amountV.type() == SCLV_AMOUNT)
                        {

                            bool permitted = (mCtx.writePermitted(srcLK) &&
                                              mCtx.writePermitted(dstLK));

                            std::optional<AccountID> issuer{std::nullopt};
                            switch (assetV.assetVal().type())
                            {
                            case ASSET_TYPE_NATIVE:
                                break;
                            case ASSET_TYPE_POOL_SHARE:
                                // These can't be paid.
                                return mCtx.hostTrap(
                                    HOST_TRAP_VALUE_HAS_WRONG_TYPE);
                            case ASSET_TYPE_CREDIT_ALPHANUM4:
                                issuer.emplace(
                                    assetV.assetVal().alphaNum4().issuer);
                                break;
                            case ASSET_TYPE_CREDIT_ALPHANUM12:
                                issuer.emplace(
                                    assetV.assetVal().alphaNum12().issuer);
                                break;
                            }
                            if (issuer)
                            {
                                LedgerKey issuerLK;
                                issuerLK.type(ACCOUNT);
                                issuerLK.account().accountID = *issuer;
                                permitted =
                                    permitted && mCtx.readPermitted(issuerLK);
                            }

                            if (!permitted)
                            {
                                return mCtx.hostTrap(
                                    HOST_TRAP_LEDGER_ENTRY_DENIED);
                            }

                            Operation op;
                            op.sourceAccount.activate();
                            op.sourceAccount->ed25519() =
                                srcLK.account().accountID.ed25519();
                            op.body.type(PAYMENT);
                            PaymentOp& pop = op.body.paymentOp();
                            pop.amount = amountV.amountVal();
                            pop.asset = assetV.assetVal();
                            pop.destination.type(KEY_TYPE_ED25519);
                            pop.destination.ed25519() =
                                dstLK.account().accountID.ed25519();

                            CLOG_INFO(Tx,
                                      "contract attempting to pay {} {} "
                                      "from {} to {}",
                                      pop.amount, assetToString(pop.asset),
                                      hexAbbrev(op.sourceAccount->ed25519()),
                                      hexAbbrev(pop.destination.ed25519()));

                            OperationResult res;
                            PaymentOpFrame pof(op, res,
                                               mCtx.getOpFrame().getParentTx());

                            bool ok = pof.doApply(mCtx.getLedgerTxn());

                            // Stash detailed result code for
                            // detailed inspection.
                            mCtx.mLastOperationResult = res;

                            if (res.code() != opINNER ||
                                res.tr().type() != PAYMENT)
                            {
                                return fizzy::ExecutionResult(
                                    HostVal::statusUnknownError());
                            }
                            if (ok && res.tr().paymentResult().code() ==
                                          PAYMENT_SUCCESS)
                            {
                                // Collapse the happy path into a
                                // single "ok" guest status.
                                return fizzy::ExecutionResult(
                                    HostVal::statusOK());
                            }
                            else
                            {
                                // Fish out the code of the result
                                // for cheap inspection.
                                uint32_t code = res.tr().paymentResult().code();
                                auto status = HostVal::fromStatus(
                                    std::make_pair(SST_PAYMENT_RESULT, code));
                                return fizzy::ExecutionResult(status);
                            }
                        }
                        return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
                    });
            });
        });
    });
}

fizzy::ExecutionResult
HostFunctions::call0(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                     uint64_t contract, uint64_t function)
{
    std::vector<HostVal> args{};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostFunctions::call1(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                     uint64_t contract, uint64_t function, uint64_t a)
{
    std::vector<HostVal> args{HostVal::fromPayload(a)};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostFunctions::call2(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                     uint64_t contract, uint64_t function, uint64_t a,
                     uint64_t b)
{
    std::vector<HostVal> args{HostVal::fromPayload(a), HostVal::fromPayload(b)};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostFunctions::call3(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                     uint64_t contract, uint64_t function, uint64_t a,
                     uint64_t b, uint64_t c)
{
    std::vector<HostVal> args{HostVal::fromPayload(a), HostVal::fromPayload(b),
                              HostVal::fromPayload(c)};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostFunctions::call4(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                     uint64_t contract, uint64_t function, uint64_t a,
                     uint64_t b, uint64_t c, uint64_t d)
{
    std::vector<HostVal> args{HostVal::fromPayload(a), HostVal::fromPayload(b),
                              HostVal::fromPayload(c), HostVal::fromPayload(d)};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostFunctions::callN(fizzy::Instance&, fizzy::ExecutionContext&,
                     uint64_t contract, uint64_t function,
                     std::vector<HostVal> const& args)
{
    ZoneScoped;
    return mCtx.objMethod<LedgerKey>(contract, [&](LedgerKey const& lk) {
        if (lk.type() != CONTRACT_CODE)
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (!mCtx.readPermitted(lk))
        {
            return mCtx.hostTrap(HOST_TRAP_LEDGER_ENTRY_DENIED);
        }
        auto& contractKey = lk.contractCode();
        auto func = HostVal::fromPayload(function);
        if (!func.isSymbol())
        {
            return mCtx.hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }

        auto res = [&]() {
            LedgerTxn ltxInner(mCtx.getLedgerTxn());
            HostContextTxn htx(mCtx.beginInnerTxn(ltxInner, contractKey.owner,
                                                  contractKey.contractID));
            return mCtx.invokeContract(contractKey.owner,
                                       contractKey.contractID, func.asSymbol(),
                                       args);
        }();
        if (std::holds_alternative<HostVal>(res))
        {
            return fizzy::ExecutionResult(std::get<HostVal>(res));
        }
        else
        {
            auto rc = std::get<InvokeContractResultCode>(res);
            HostVal status = HostVal::fromStatus(
                std::make_pair(SST_INVOKE_CONTRACT_RESULT, uint32_t(rc)));
            return fizzy::ExecutionResult(status);
        }
    });
}

fizzy::ExecutionResult
HostFunctions::bigNumFromU64(fizzy::Instance&, fizzy::ExecutionContext&,
                             uint64_t rhs)
{
    ZoneScoped;
    return mCtx.newObject<HostBigNum>(rhs);
}

fizzy::ExecutionResult
HostFunctions::bigNumAdd(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lhs + rhs);
        });
    });
}

fizzy::ExecutionResult
HostFunctions::bigNumSub(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lhs - rhs);
        });
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumMul(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lhs * rhs);
        });
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumDiv(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lhs / rhs);
        });
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumRem(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lhs % rhs);
        });
    });
}

fizzy::ExecutionResult
HostFunctions::bigNumAnd(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lhs & rhs);
        });
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumOr(fizzy::Instance&, fizzy::ExecutionContext&,
                        uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lhs | rhs);
        });
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumXor(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lhs ^ rhs);
        });
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumShl(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    // NB: rhs is a plain uint64_t here
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.newObject<HostBigNum>(lhs << rhs);
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumShr(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    // NB: rhs is a plain uint64_t here
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.newObject<HostBigNum>(lhs >> rhs);
    });
}

fizzy::ExecutionResult
HostFunctions::bigNumCmp(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            int i = lhs.compare(rhs);
            if (i > 0)
            {
                return HostVal::fromI32(1);
            }
            else if (i < 0)
            {
                return HostVal::fromI32(-1);
            }
            else
            {
                return HostVal::fromI32(0);
            }
        });
    });
}

fizzy::ExecutionResult
HostFunctions::bigNumIsZero(fizzy::Instance&, fizzy::ExecutionContext&,
                            uint64_t x)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(
        x, [&](HostBigNum const& x) { return HostVal::fromBool(x.is_zero()); });
}

fizzy::ExecutionResult
HostFunctions::bigNumNeg(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t x)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(
        x, [&](HostBigNum const& x) { return mCtx.newObject<HostBigNum>(-x); });
}
fizzy::ExecutionResult
HostFunctions::bigNumNot(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t x)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(
        x, [&](HostBigNum const& x) { return mCtx.newObject<HostBigNum>(~x); });
}

fizzy::ExecutionResult
HostFunctions::bigNumGcd(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(gcd(lhs, rhs));
        });
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumLcm(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return mCtx.newObject<HostBigNum>(lcm(lhs, rhs));
        });
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumPow(fizzy::Instance&, fizzy::ExecutionContext&,
                         uint64_t lhs, uint64_t rhs)
{
    ZoneScoped;
    // NB: rhs is plain uint64_t here
    return mCtx.objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return mCtx.newObject<HostBigNum>(pow(lhs, rhs));
    });
}
fizzy::ExecutionResult
HostFunctions::bigNumPowMod(fizzy::Instance&, fizzy::ExecutionContext&,
                            uint64_t p, uint64_t q, uint64_t m)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(p, [&](HostBigNum const& p) {
        return mCtx.objMethod<HostBigNum>(q, [&](HostBigNum const& q) {
            return mCtx.objMethod<HostBigNum>(m, [&](HostBigNum const& m) {
                return mCtx.newObject<HostBigNum>(powm(p, q, m));
            });
        });
    });
}

fizzy::ExecutionResult
HostFunctions::bigNumSqrt(fizzy::Instance&, fizzy::ExecutionContext&,
                          uint64_t x)
{
    ZoneScoped;
    return mCtx.objMethod<HostBigNum>(x, [&](HostBigNum const& x) {
        return mCtx.newObject<HostBigNum>(sqrt(x));
    });
}

HostFunctions::HostFunctions(HostContext& hc) : mCtx(hc)
{
    // Module 'x' is miscellaneous context-access functions
    registerHostFunction(&HostFunctions::logValue, "x", "$_");
    registerHostFunction(&HostFunctions::getLastOperationResult, "x", "$0");

    // Module 'm' is map functions
    registerHostFunction(&HostFunctions::mapNew, "m", "$_");
    registerHostFunction(&HostFunctions::mapPut, "m", "$0");
    registerHostFunction(&HostFunctions::mapGet, "m", "$1");
    registerHostFunction(&HostFunctions::mapDel, "m", "$2");
    registerHostFunction(&HostFunctions::mapLen, "m", "$3");
    registerHostFunction(&HostFunctions::mapKeys, "m", "$4");
    registerHostFunction(&HostFunctions::mapHas, "m", "$5");

    // Module 'v' is vec functions
    registerHostFunction(&HostFunctions::vecNew, "v", "$_");
    registerHostFunction(&HostFunctions::vecPut, "v", "$0");
    registerHostFunction(&HostFunctions::vecGet, "v", "$1");
    registerHostFunction(&HostFunctions::vecDel, "v", "$2");
    registerHostFunction(&HostFunctions::vecLen, "v", "$3");

    registerHostFunction(&HostFunctions::vecPush, "v", "$4");
    registerHostFunction(&HostFunctions::vecPop, "v", "$5");
    registerHostFunction(&HostFunctions::vecTake, "v", "$6");
    registerHostFunction(&HostFunctions::vecDrop, "v", "$7");
    registerHostFunction(&HostFunctions::vecFront, "v", "$8");
    registerHostFunction(&HostFunctions::vecBack, "v", "$9");
    registerHostFunction(&HostFunctions::vecInsert, "v", "$A");
    registerHostFunction(&HostFunctions::vecAppend, "v", "$B");

    // Module 'l' is ledger-access functions
    registerHostFunction(&HostFunctions::getCurrentLedgerNum, "l", "$_");
    registerHostFunction(&HostFunctions::getCurrentLedgerCloseTime, "l", "$0");
    registerHostFunction(&HostFunctions::pay, "l", "$1");
    registerHostFunction(&HostFunctions::putContractData, "l", "$2");
    registerHostFunction(&HostFunctions::hasContractData, "l", "$3");
    registerHostFunction(&HostFunctions::getContractData, "l", "$4");
    registerHostFunction(&HostFunctions::delContractData, "l", "$5");

    // Module 'c' is cross-contract functions
    registerHostFunction(&HostFunctions::call0, "c", "$_");
    registerHostFunction(&HostFunctions::call1, "c", "$0");
    registerHostFunction(&HostFunctions::call2, "c", "$1");
    registerHostFunction(&HostFunctions::call3, "c", "$2");
    registerHostFunction(&HostFunctions::call4, "c", "$3");

    // Module 'b' is BigNum functions
    registerHostFunction(&HostFunctions::bigNumFromU64, "b", "$_");
    registerHostFunction(&HostFunctions::bigNumAdd, "b", "$0");
    registerHostFunction(&HostFunctions::bigNumSub, "b", "$1");
    registerHostFunction(&HostFunctions::bigNumMul, "b", "$2");
    registerHostFunction(&HostFunctions::bigNumDiv, "b", "$3");
    registerHostFunction(&HostFunctions::bigNumRem, "b", "$4");
    registerHostFunction(&HostFunctions::bigNumAnd, "b", "$5");
    registerHostFunction(&HostFunctions::bigNumOr, "b", "$6");
    registerHostFunction(&HostFunctions::bigNumXor, "b", "$7");
    registerHostFunction(&HostFunctions::bigNumShl, "b", "$8");
    registerHostFunction(&HostFunctions::bigNumShr, "b", "$9");
    registerHostFunction(&HostFunctions::bigNumCmp, "b", "$A");
    registerHostFunction(&HostFunctions::bigNumIsZero, "b", "$B");

    registerHostFunction(&HostFunctions::bigNumNeg, "b", "$C");
    registerHostFunction(&HostFunctions::bigNumNot, "b", "$D");

    registerHostFunction(&HostFunctions::bigNumGcd, "b", "$E");
    registerHostFunction(&HostFunctions::bigNumLcm, "b", "$F");
    registerHostFunction(&HostFunctions::bigNumPow, "b", "$G");
    registerHostFunction(&HostFunctions::bigNumPowMod, "b", "$H");
    registerHostFunction(&HostFunctions::bigNumSqrt, "b", "$I");
}
#pragma endregion hostfns

}
