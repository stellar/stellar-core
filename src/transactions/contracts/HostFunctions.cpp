#include "crypto/Hex.h"
#include "transactions/InvokeContractOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/contracts/HostContext.h"
#include "util/Logging.h"
#include "xdr/Stellar-transaction.h"

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
            return fizzy::ExecutionResult(*valPtr);
        }
        return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
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
            return fizzy::ExecutionResult(HostVal::fromU32(uint32_t(sz)));
        }
        return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
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
        if (!idxV.isU32())
        {
            return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (idxV.asU32() >= vec.size())
        {
            return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
        }
        return fizzy::ExecutionResult(vec.at(idxV.asU32()));
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
        if (!idxV.isU32())
        {
            return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (idxV.asU32() >= vec.size())
        {
            return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
        }
        return fizzy::ExecutionResult(
            newObject<HostVec>(vec.set(idxV.asU32(), valV)));
    });
}
fizzy::ExecutionResult
HostContext::vecDel(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec,
                    uint64_t idx)
{
    ZoneScoped;
    auto idxV = HostVal::fromPayload(idx);
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!idxV.isU32())
        {
            return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (idxV.asU32() >= vec.size())
        {
            return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
        }
        return fizzy::ExecutionResult(
            newObject<HostVec>(vec.erase(idxV.asU32())));
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
            return fizzy::ExecutionResult(HostVal::fromU32(sz));
        }
        return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
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
            return fizzy::ExecutionResult(
                newObject<HostVec>(vec.take(numV.asU32())));
        }
        return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
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
            return fizzy::ExecutionResult(
                newObject<HostVec>(vec.drop(numV.asU32())));
        }
        return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
    });
}
fizzy::ExecutionResult
HostContext::vecPop(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return fizzy::ExecutionResult(
                newObject<HostVec>(vec.erase(vec.size() - 1)));
        }
        return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
    });
}

fizzy::ExecutionResult
HostContext::vecFront(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return fizzy::ExecutionResult(vec.front());
        }
        return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
    });
}

fizzy::ExecutionResult
HostContext::vecBack(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t vec)
{
    ZoneScoped;
    return objMethod<HostVec>(vec, [&](HostVec const& vec) {
        if (!vec.empty())
        {
            return fizzy::ExecutionResult(vec.back());
        }
        return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
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
        if (!idxV.isU32())
        {
            return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        if (idxV.asU32() >= vec.size())
        {
            return hostTrap(HOST_TRAP_VALUE_OUT_OF_RANGE);
        }
        return fizzy::ExecutionResult(
            newObject<HostVec>(vec.insert(idxV.asU32(), valV)));
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
HostContext::getLastOperationResult(fizzy::Instance&, fizzy::ExecutionContext&)
{
    return newObject<OperationResult>(mLastOperationResult);
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
                                    mInvokeCtxs.back().mInvokeOp.getParentTx());

                                bool ok = pof.doApply(getLedgerTxn());

                                // Stash detailed result code for detailed
                                // inspection.
                                mLastOperationResult = res;

                                if (res.code() != opINNER ||
                                    res.tr().type() != PAYMENT)
                                {
                                    return fizzy::ExecutionResult(
                                        HostVal::statusUnknownError());
                                }
                                if (ok && res.tr().paymentResult().code() ==
                                              PAYMENT_SUCCESS)
                                {
                                    // Collapse the happy path into a single
                                    // "ok" guest status.
                                    return fizzy::ExecutionResult(
                                        HostVal::statusOK());
                                }
                                else
                                {
                                    // Fish out the code of the result for cheap
                                    // inspection.
                                    uint32_t code =
                                        res.tr().paymentResult().code();
                                    auto status =
                                        HostVal::fromStatus(std::make_pair(
                                            SST_PAYMENT_RESULT, code));
                                    return fizzy::ExecutionResult(status);
                                }
                            }
                            return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
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
            return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }
        auto& contractLK = lk.contractCode();
        auto func = HostVal::fromPayload(function);
        if (!func.isSymbol())
        {
            return hostTrap(HOST_TRAP_VALUE_HAS_WRONG_TYPE);
        }

        auto res = [&]() {
            LedgerTxn ltxInner(getLedgerTxn());
            HostContextTxn htx(beginInnerTxn(ltxInner));
            return invokeContract(contractLK.owner, contractLK.contractID,
                                  func.asSymbol(), args);
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
HostContext::bigNumFromU64(fizzy::Instance&, fizzy::ExecutionContext&,
                           uint64_t rhs)
{
    ZoneScoped;
    return newObject<HostBigNum>(rhs);
}

fizzy::ExecutionResult
HostContext::bigNumAdd(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lhs + rhs);
        });
    });
}

fizzy::ExecutionResult
HostContext::bigNumSub(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lhs - rhs);
        });
    });
}
fizzy::ExecutionResult
HostContext::bigNumMul(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lhs * rhs);
        });
    });
}
fizzy::ExecutionResult
HostContext::bigNumDiv(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lhs / rhs);
        });
    });
}
fizzy::ExecutionResult
HostContext::bigNumRem(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lhs % rhs);
        });
    });
}

fizzy::ExecutionResult
HostContext::bigNumAnd(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lhs & rhs);
        });
    });
}
fizzy::ExecutionResult
HostContext::bigNumOr(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                      uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lhs | rhs);
        });
    });
}
fizzy::ExecutionResult
HostContext::bigNumXor(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lhs ^ rhs);
        });
    });
}
fizzy::ExecutionResult
HostContext::bigNumShl(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    // NB: rhs is a plain uint64_t here
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return newObject<HostBigNum>(lhs << rhs);
    });
}
fizzy::ExecutionResult
HostContext::bigNumShr(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    // NB: rhs is a plain uint64_t here
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return newObject<HostBigNum>(lhs >> rhs);
    });
}

fizzy::ExecutionResult
HostContext::bigNumCmp(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
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
HostContext::bigNumIsZero(fizzy::Instance&, fizzy::ExecutionContext&,
                          uint64_t x)
{
    ZoneScoped;
    return objMethod<HostBigNum>(
        x, [&](HostBigNum const& x) { return HostVal::fromBool(x.is_zero()); });
}

fizzy::ExecutionResult
HostContext::bigNumNeg(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t x)
{
    ZoneScoped;
    return objMethod<HostBigNum>(
        x, [&](HostBigNum const& x) { return newObject<HostBigNum>(-x); });
}
fizzy::ExecutionResult
HostContext::bigNumNot(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t x)
{
    ZoneScoped;
    return objMethod<HostBigNum>(
        x, [&](HostBigNum const& x) { return newObject<HostBigNum>(~x); });
}

fizzy::ExecutionResult
HostContext::bigNumGcd(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(gcd(lhs, rhs));
        });
    });
}
fizzy::ExecutionResult
HostContext::bigNumLcm(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return objMethod<HostBigNum>(rhs, [&](HostBigNum const& rhs) {
            return newObject<HostBigNum>(lcm(lhs, rhs));
        });
    });
}
fizzy::ExecutionResult
HostContext::bigNumPow(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t lhs,
                       uint64_t rhs)
{
    ZoneScoped;
    // NB: rhs is plain uint64_t here
    return objMethod<HostBigNum>(lhs, [&](HostBigNum const& lhs) {
        return newObject<HostBigNum>(pow(lhs, rhs));
    });
}
fizzy::ExecutionResult
HostContext::bigNumPowMod(fizzy::Instance&, fizzy::ExecutionContext&,
                          uint64_t p, uint64_t q, uint64_t m)
{
    ZoneScoped;
    return objMethod<HostBigNum>(p, [&](HostBigNum const& p) {
        return objMethod<HostBigNum>(q, [&](HostBigNum const& q) {
            return objMethod<HostBigNum>(m, [&](HostBigNum const& m) {
                return newObject<HostBigNum>(powm(p, q, m));
            });
        });
    });
}

fizzy::ExecutionResult
HostContext::bigNumSqrt(fizzy::Instance&, fizzy::ExecutionContext&, uint64_t x)
{
    ZoneScoped;
    return objMethod<HostBigNum>(
        x, [&](HostBigNum const& x) { return newObject<HostBigNum>(sqrt(x)); });
}

void
HostContext::registerHostFunctions()
{
    // Movule 'x' is miscellaneous context-access functions
    registerHostFunction(&HostContext::logValue, "x", "$_");
    registerHostFunction(&HostContext::getLastOperationResult, "x", "$0");

    // Module 'm' is map functions
    registerHostFunction(&HostContext::mapNew, "m", "$_");
    registerHostFunction(&HostContext::mapPut, "m", "$0");
    registerHostFunction(&HostContext::mapGet, "m", "$1");
    registerHostFunction(&HostContext::mapDel, "m", "$2");
    registerHostFunction(&HostContext::mapLen, "m", "$3");
    registerHostFunction(&HostContext::mapKeys, "m", "$4");

    // Module 'v' is vec functions
    registerHostFunction(&HostContext::vecNew, "v", "$_");
    registerHostFunction(&HostContext::vecPut, "v", "$0");
    registerHostFunction(&HostContext::vecGet, "v", "$1");
    registerHostFunction(&HostContext::vecDel, "v", "$2");
    registerHostFunction(&HostContext::vecLen, "v", "$3");

    registerHostFunction(&HostContext::vecPush, "v", "$4");
    registerHostFunction(&HostContext::vecPop, "v", "$5");
    registerHostFunction(&HostContext::vecTake, "v", "$6");
    registerHostFunction(&HostContext::vecDrop, "v", "$7");
    registerHostFunction(&HostContext::vecFront, "v", "$8");
    registerHostFunction(&HostContext::vecBack, "v", "$9");
    registerHostFunction(&HostContext::vecInsert, "v", "$A");
    registerHostFunction(&HostContext::vecAppend, "v", "$B");

    // Module 'l' is ledger-access functions
    registerHostFunction(&HostContext::getCurrentLedgerNum, "l", "$_");
    registerHostFunction(&HostContext::getCurrentLedgerCloseTime, "l", "$0");
    registerHostFunction(&HostContext::pay, "l", "$1");

    // Module 'c' is cross-contract functions
    registerHostFunction(&HostContext::call0, "c", "$_");
    registerHostFunction(&HostContext::call1, "c", "$0");
    registerHostFunction(&HostContext::call2, "c", "$1");
    registerHostFunction(&HostContext::call3, "c", "$2");
    registerHostFunction(&HostContext::call4, "c", "$3");

    // Module 'b' is BigNum functions
    registerHostFunction(&HostContext::bigNumFromU64, "b", "$_");
    registerHostFunction(&HostContext::bigNumAdd, "b", "$0");
    registerHostFunction(&HostContext::bigNumSub, "b", "$1");
    registerHostFunction(&HostContext::bigNumMul, "b", "$2");
    registerHostFunction(&HostContext::bigNumDiv, "b", "$3");
    registerHostFunction(&HostContext::bigNumRem, "b", "$4");
    registerHostFunction(&HostContext::bigNumAnd, "b", "$5");
    registerHostFunction(&HostContext::bigNumOr, "b", "$6");
    registerHostFunction(&HostContext::bigNumXor, "b", "$7");
    registerHostFunction(&HostContext::bigNumShl, "b", "$8");
    registerHostFunction(&HostContext::bigNumShr, "b", "$9");
    registerHostFunction(&HostContext::bigNumCmp, "b", "$A");
    registerHostFunction(&HostContext::bigNumIsZero, "b", "$B");

    registerHostFunction(&HostContext::bigNumNeg, "b", "$C");
    registerHostFunction(&HostContext::bigNumNot, "b", "$D");

    registerHostFunction(&HostContext::bigNumGcd, "b", "$E");
    registerHostFunction(&HostContext::bigNumLcm, "b", "$F");
    registerHostFunction(&HostContext::bigNumPow, "b", "$G");
    registerHostFunction(&HostContext::bigNumPowMod, "b", "$H");
    registerHostFunction(&HostContext::bigNumSqrt, "b", "$I");
}
}
