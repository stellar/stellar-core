#include "crypto/Hex.h"
#include "transactions/InvokeContractOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/contracts/HostContext.h"
#include "util/Logging.h"

#include <Tracy.hpp>
#include <fizzy/execute.hpp>
#include <fizzy/parser.hpp>

namespace stellar
{

fizzy::ExecutionResult
HostContext::mapNew(fizzy::Instance& instance, fizzy::ExecutionContext& exec)
{
    ZoneScoped;
    return newObject<HostMap>();
}

fizzy::ExecutionResult
HostContext::mapPut(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                    uint64_t map, uint64_t key, uint64_t val)
{
    ZoneScoped;
    auto keyV = HostVal::fromPayload(key);
    auto valV = HostVal::fromPayload(val);
    return objMethod<HostMap>(map, [&](HostMap const& map) {
        return newObject<HostMap>(map.set(keyV, valV));
    });
}

fizzy::ExecutionResult
HostContext::mapGet(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                    uint64_t map, uint64_t key)
{
    ZoneScoped;
    auto keyV = HostVal::fromPayload(key);
    return objMethod<HostMap>(map, [&](HostMap const& map) {
        auto* valPtr = map.find(keyV);
        if (valPtr)
        {
            return *valPtr;
        }
        return HostVal::fromStatus(0);
    });
}

fizzy::ExecutionResult
HostContext::mapDel(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t map,
                    uint64_t key)
{
    ZoneScoped;
    auto keyV = HostVal::fromPayload(key);
    return objMethod<HostMap>(map, [&](HostMap const& map) {
        return newObject<HostMap>(map.erase(keyV));
    });
}

fizzy::ExecutionResult
HostContext::mapLen(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t map)
{
    ZoneScoped;
    return objMethod<HostMap>(map, [&](HostMap const& map) {
        size_t sz = map.size();
        if (sz <= UINT32_MAX)
        {
            return HostVal::fromU32(uint32_t(sz));
        }
        return HostVal::fromStatus(0);
    });
}

fizzy::ExecutionResult
HostContext::mapKeys(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t map)
{
    ZoneScoped;
    return objMethod<HostMap>(map, [&](HostMap const& map) {
        std::vector<HostVal> vec;
        for (auto const& i : map)
        {
            vec.emplace_back(i.first);
        }
        // FIXME: do we want a deep
        // structural-comparison sort?
        std::sort(vec.begin(), vec.end());
        return newObject<HostVec>(vec.begin(), vec.end());
    });
}

fizzy::ExecutionResult
HostContext::vecNew(fizzy::Instance&, fizzy::ExecutionContext&)
{
    ZoneScoped;
    return newObject<HostVec>();
}

fizzy::ExecutionResult
HostContext::vecGet(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                    uint64_t idx)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (idxV.isU32() && idxV.asU32() < vec.size())
        {
            return vec.at(idxV.asU32());
        }
        return HostVal::fromStatus(0);
    });
}
fizzy::ExecutionResult
HostContext::vecPut(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                    uint64_t idx, uint64_t val)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    auto valV = HostVal::fromPayload(val);
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (idxV.isU32() && idxV.asU32() < vec.size())
        {
            return newObject<HostVec>(vec.set(idxV.asU32(), valV));
        }
        return HostVal::fromStatus(0);
    });
}
fizzy::ExecutionResult
HostContext::vecDel(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                    uint64_t idx)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (idxV.isU32() && idxV.asU32() < vec.size())
        {
            return newObject<HostVec>(vec.erase(idxV.asU32()));
        }
        return HostVal::fromStatus(0);
    });
}
fizzy::ExecutionResult
HostContext::vecLen(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        size_t sz = vec.size();
        if (sz <= UINT32_MAX)
        {
            return HostVal::fromU32(sz);
        }
        return HostVal::fromStatus(0);
    });
}
fizzy::ExecutionResult
HostContext::vecPush(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                     uint64_t val)
{
    ZoneScoped;
    auto valV = HostVal::fromPayload(val);
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        return newObject<HostVec>(vec.push_back(valV));
    });
}
fizzy::ExecutionResult
HostContext::vecTake(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                     uint64_t num)
{
    ZoneScoped;
    auto numV = HostVal::fromPayload(num);
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (numV.isU32())
        {
            return newObject<HostVec>(vec.take(numV.asU32()));
        }
        return HostVal::fromStatus(0);
    });
}
fizzy::ExecutionResult
HostContext::vecDrop(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                     uint64_t num)
{
    ZoneScoped;
    auto numV = HostVal::fromPayload(num);
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (numV.isU32())
        {
            return newObject<HostVec>(vec.drop(numV.asU32()));
        }
        return HostVal::fromStatus(0);
    });
}
fizzy::ExecutionResult
HostContext::vecPop(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return newObject<HostVec>(vec.erase(vec.size() - 1));
        }
        return HostVal::fromStatus(0);
    });
}

fizzy::ExecutionResult
HostContext::vecFront(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return vec.front();
        }
        return HostVal::fromStatus(0);
    });
}

fizzy::ExecutionResult
HostContext::vecBack(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return vec.back();
        }
        return HostVal::fromStatus(0);
    });
}

fizzy::ExecutionResult
HostContext::vecInsert(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                       uint64_t idx, uint64_t val)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    auto valV = HostVal::fromPayload(val);
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (idxV.isU32() && idxV.asU32() < vec.size())
        {
            return newObject<HostVec>(vec.insert(idxV.asU32(), valV));
        }
        return HostVal::fromStatus(0);
    });
}
fizzy::ExecutionResult
HostContext::vecAppend(fizzy::Instance&, fizzy::ExecutionContext&,
                       uint64_t vecA, uint64_t vecB)
{
    ZoneScoped;
    return objMethod<HostVec>(vecA, [&](HostVec const& vecA) {
        return objMethod<HostVec>(vecB, [&](HostVec const& vecB) {
            return newObject<HostVec>(vecA + vecB);
        });
    });
}

fizzy::ExecutionResult
HostContext::logValue(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                      uint64_t val)
{
    ZoneScoped;
    CLOG_INFO(Tx, "contract called log_value({})", HostVal::fromPayload(val));
    return HostVal::fromVoid();
}

fizzy::ExecutionResult
HostContext::getCurrentLedgerNum(fizzy::Instance& instance,
                                 fizzy::ExecutionContext& exec)
{
    ZoneScoped;
    uint32_t num = getLedgerTxn().loadHeader().current().ledgerSeq;
    return HostVal::fromU32(num);
}

fizzy::ExecutionResult
HostContext::getCurrentLedgerCloseTime(fizzy::Instance& instance,
                                       fizzy::ExecutionContext& exec)
{
    ZoneScoped;
    // NB: this returns a raw u64, not a HostVal.
    TimePoint closeTime =
        getLedgerTxn().loadHeader().current().scpValue.closeTime;
    return fizzy::Value{closeTime};
}

fizzy::ExecutionResult
HostContext::pay(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t src,
                 uint64_t dst, uint64_t asset, uint64_t amount)
{
    ZoneScoped;
    CLOG_INFO(Tx, "pay({},{},{},{})", src, dst, asset, amount);

    return objMethod<LedgerKey>(src, [&](LedgerKey const& srcLK) {
        return objMethod<LedgerKey>(dst, [&](LedgerKey const& dstLK) {
            return objMethod<SCLedgerVal>(
                asset, [&](SCLedgerVal const& assetV) {
                    return objMethod<SCLedgerVal>(
                        amount, [&](SCLedgerVal const& amountV) {
                            if (srcLK.type() == ACCOUNT &&
                                dstLK.type() == ACCOUNT &&
                                assetV.type() == SCLV_ASSET &&
                                amountV.type() == SCLV_AMOUNT)
                            {

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

                                CLOG_INFO(
                                    Tx,
                                    "contract attempting to pay {} {} "
                                    "from {} to {}",
                                    pop.amount, assetToString(pop.asset),
                                    hexAbbrev(op.sourceAccount->ed25519()),
                                    hexAbbrev(pop.destination.ed25519()));

                                OperationResult res;
                                PaymentOpFrame pof(
                                    op, res,
                                    mHostOpCtx->mInvokeOp.getParentTx());

                                if (pof.doApply(getLedgerTxn()))
                                {
                                    return HostVal::fromBool(true);
                                }
                            }
                            return HostVal::fromStatus(0);
                        });
                });
        });
    });
}

fizzy::ExecutionResult
HostContext::call0(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                   uint64_t contract, uint64_t function)
{
    std::vector<HostVal> args{};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostContext::call1(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                   uint64_t contract, uint64_t function, uint64_t a)
{
    std::vector<HostVal> args{HostVal::fromPayload(a)};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostContext::call2(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                   uint64_t contract, uint64_t function, uint64_t a, uint64_t b)
{
    std::vector<HostVal> args{HostVal::fromPayload(a), HostVal::fromPayload(b)};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostContext::call3(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                   uint64_t contract, uint64_t function, uint64_t a, uint64_t b,
                   uint64_t c)
{
    std::vector<HostVal> args{HostVal::fromPayload(a), HostVal::fromPayload(b),
                              HostVal::fromPayload(c)};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostContext::call4(fizzy::Instance& instance, fizzy::ExecutionContext& ctx,
                   uint64_t contract, uint64_t function, uint64_t a, uint64_t b,
                   uint64_t c, uint64_t d)
{
    std::vector<HostVal> args{HostVal::fromPayload(a), HostVal::fromPayload(b),
                              HostVal::fromPayload(c), HostVal::fromPayload(d)};
    return callN(instance, ctx, contract, function, args);
}

fizzy::ExecutionResult
HostContext::callN(fizzy::Instance&, fizzy::ExecutionContext&,
                   uint64_t contract, uint64_t function,
                   std::vector<HostVal> const& args)
{
    ZoneScoped;
    return objMethod<LedgerKey>(contract, [&](LedgerKey const& lk) {
        if (lk.type() != CONTRACT_CODE)
        {
            return HostVal::fromStatus(0);
        }
        auto& contractLK = lk.contractCode();
        auto func = HostVal::fromPayload(function);
        if (!func.isSymbol())
        {
            return HostVal::fromStatus(0);
        }
        auto res = invokeContract(contractLK.owner, contractLK.contractID,
                                  func.asSymbol(), args);
        if (std::holds_alternative<HostVal>(res))
        {
            return std::get<HostVal>(res);
        }
        else
        {
            return HostVal::fromStatus(0);
        }
    });
}

void
HostContext::registerHostFunctions()
{
    registerHostFunction(&HostContext::mapNew, "env", "host__map_new");
    registerHostFunction(&HostContext::mapPut, "env", "host__map_put");
    registerHostFunction(&HostContext::mapGet, "env", "host__map_get");
    registerHostFunction(&HostContext::mapDel, "env", "host__map_del");
    registerHostFunction(&HostContext::mapLen, "env", "host__map_len");
    registerHostFunction(&HostContext::mapKeys, "env", "host__map_keys");

    registerHostFunction(&HostContext::vecNew, "env", "host__vec_new");
    registerHostFunction(&HostContext::vecPut, "env", "host__vec_put");
    registerHostFunction(&HostContext::vecGet, "env", "host__vec_get");
    registerHostFunction(&HostContext::vecDel, "env", "host__vec_del");
    registerHostFunction(&HostContext::vecLen, "env", "host__vec_len");

    registerHostFunction(&HostContext::vecPush, "env", "host__vec_push");
    registerHostFunction(&HostContext::vecPop, "env", "host__vec_pop");
    registerHostFunction(&HostContext::vecTake, "env", "host__vec_take");
    registerHostFunction(&HostContext::vecDrop, "env", "host__vec_drop");
    registerHostFunction(&HostContext::vecFront, "env", "host__vec_front");
    registerHostFunction(&HostContext::vecBack, "env", "host__vec_back");
    registerHostFunction(&HostContext::vecInsert, "env", "host__vec_insert");
    registerHostFunction(&HostContext::vecAppend, "env", "host__vec_append");

    registerHostFunction(&HostContext::logValue, "env", "host__log_value");
    registerHostFunction(&HostContext::getCurrentLedgerNum, "env",
                         "host__get_current_ledger_num");
    registerHostFunction(&HostContext::getCurrentLedgerCloseTime, "env",
                         "host__get_current_ledger_close_time");
    registerHostFunction(&HostContext::pay, "env", "host__pay");

    registerHostFunction(&HostContext::call0, "env", "host__call0");
    registerHostFunction(&HostContext::call1, "env", "host__call1");
    registerHostFunction(&HostContext::call2, "env", "host__call2");
    registerHostFunction(&HostContext::call3, "env", "host__call3");
    registerHostFunction(&HostContext::call4, "env", "host__call4");
}
}
