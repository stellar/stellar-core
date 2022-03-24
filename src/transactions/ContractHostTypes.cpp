// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/ContractHostTypes.h"
#include "ledger/LedgerTxn.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include <fizzy/execute.hpp>
#include <fizzy/instantiate.hpp>
#include <fizzy/value.hpp>
#include <iostream>

namespace stellar
{
std::string
HostVal::asSymbol() const
{
    static char dict[64] =
        "_0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string out;
    out.reserve(8);
    auto tmp = getBody();
    for (size_t off = 6; off <= 48; off += 6)
    {
        auto idx = (tmp >> (48 - off)) & 63;
        if (idx == 0)
        {
            break;
        }
        out += dict[idx - 1];
    }
    return out;
}

HostVal
HostVal::fromSymbol(std::string const& s)
{
    if (s.size() > 8)
    {
        throw std::runtime_error("bad symbol size");
    }
    uint64_t accum = 0;
    for (auto const c : s)
    {
        accum <<= 6;
        if (c == '_')
        {
            // 1 is underscore
            accum |= 1;
        }
        else if ('0' <= c && c <= '9')
        {
            // 2-11 inclusive are digits
            accum |= 2 + (c - '0');
        }
        else if ('A' <= c && c <= 'Z')
        {
            // 12-37 inclusive are uppercase
            accum |= 12 + (c - 'A');
        }
        else if ('a' <= c && c <= 'z')
        {
            // 38-63 inclusive are lowercase
            accum |= 38 + (c - 'a');
        }
        else
        {
            throw std::runtime_error("bad symbol char");
        };
    }
    accum <<= 6 * (8 - s.size());
    return fromTagAndBody(TAG_SYMBOL, accum);
}

std::ostream&
operator<<(std::ostream& out, HostVal const& v)
{
    if (v.isVoid())
    {
        out << "void";
    }
    else if (v.isBool())
    {
        out << "bool(" << v.asBool() << ')';
    }
    else if (v.isStatus())
    {
        out << "status(" << v.asStatus() << ')';
    }
    else if (v.isU32())
    {
        out << "u32(" << v.asU32() << ')';
    }
    else if (v.isI32())
    {
        out << "i32(" << v.asI32() << ')';
    }
    else if (v.isSymbol())
    {
        out << "sym(" << v.asSymbol() << ')';
    }
    else if (v.isBitSet())
    {
        out << "bitset(";
        auto tmp = v.asBitSet();
        for (auto i = 0; i < 48; ++i)
        {
            out << (tmp & 1 ? '1' : '0');
            tmp >>= 1;
        }
        out << ')';
    }
    else if (v.isTimePt())
    {
        out << "time(" << v.asTimePt() << ')';
    }
    else if (v.isObject())
    {
        out << "obj(" << v.asObject() << ')';
    }
    else
    {
        auto payload = v.payload();
        out << "unknown(" << payload << ",0x" << std::hex << payload << ')';
    }
    return out;
}

HostContextTxn::HostContextTxn(HostContext& hc)
    : mRollbackPoint(hc.mObjects.size()), mHostCtx(hc)
{
}

HostContextTxn::~HostContextTxn()
{
    if (mRollback)
    {
        mHostCtx.mHostOpCtx.value().mLedgerTxn.rollback();
        mHostCtx.mObjects.resize(mRollbackPoint);
    }
    mHostCtx.mHostOpCtx.reset();
}

void
HostContext::extendEnvironment(SCEnv const& locals)
{
    for (auto const& pair : locals)
    {
        mEnv.insert_or_assign(pair.key, xdrToHost(pair.val));
    }
}

std::optional<HostVal>
HostContext::getEnv(std::string const& name) const
{
    auto i = mEnv.find(name);
    if (i == mEnv.end())
    {
        return std::nullopt;
    }
    else
    {
        return std::make_optional(i->second);
    }
}

HostVal
HostContext::xdrToHost(SCVal const& v)
{
    switch (v.type())
    {
    case SCV_VOID:
        return HostVal::fromVoid();
    case SCV_BOOL:
        return HostVal::fromBool(v.b());
    case SCV_OBJECT:
        return HostVal::fromObject(xdrToHost(v.obj()));
    case SCV_U32:
        return HostVal::fromU32(v.u32());
    case SCV_I32:
        return HostVal::fromI32(v.i32());
    case SCV_SYMBOL:
        return HostVal::fromSymbol(v.sym());
    case SCV_BITSET:
        return HostVal::fromBitSet(v.bits());
    case SCV_TIMEPT:
        return HostVal::fromTimePt(v.time());
    case SCV_STATUS:
        return HostVal::fromStatus(v.status());
    }
}

SCVal
HostContext::hostToXdr(HostVal const& hv)
{
    SCVal out;
    if (hv.isVoid())
    {
        out.type(SCV_VOID);
    }
    else if (hv.isBool())
    {
        out.type(SCV_BOOL);
        out.b() = hv.asBool();
    }
    else if (hv.isObject())
    {
        out.type(SCV_OBJECT);
        auto idx = hv.asObject();
        if (idx < mObjects.size())
        {
            auto const& obj = mObjects.at(idx);
            out.obj() = hostToXdr(obj);
        }
    }
    else if (hv.isU32())
    {
        out.type(SCV_U32);
        out.u32() = hv.asU32();
    }
    else if (hv.isI32())
    {
        out.type(SCV_I32);
        out.i32() = hv.asI32();
    }
    else if (hv.isSymbol())
    {
        out.type(SCV_SYMBOL);
        out.sym() = hv.asSymbol();
    }
    else if (hv.isBitSet())
    {
        out.type(SCV_BITSET);
        out.bits() = hv.asBitSet();
    }
    else if (hv.isTimePt())
    {
        out.type(SCV_TIMEPT);
        out.time() = hv.asTimePt();
    }
    else if (hv.isStatus())
    {
        out.type(SCV_STATUS);
        out.status() = hv.asStatus();
    }
    return out;
}

xdr::pointer<SCObject>
HostContext::hostToXdr(std::unique_ptr<HostObject const> const& obj)
{
    if (!obj)
    {
        return nullptr;
    }
    xdr::pointer<SCObject> ptr;
    SCObject& out = ptr.activate();
    if (std::holds_alternative<HostBox>(*obj))
    {
        out.type(SCO_BOX);
        out.box() = hostToXdr(std::get<HostBox>(*obj).get());
    }
    else if (std::holds_alternative<HostVec>(*obj))
    {
        out.type(SCO_VEC);
        for (auto const& v : std::get<HostVec>(*obj))
        {
            out.vec().emplace_back(hostToXdr(v));
        }
    }
    else if (std::holds_alternative<HostMap>(*obj))
    {
        out.type(SCO_MAP);
        for (auto const& pair : std::get<HostMap>(*obj))
        {
            out.map().emplace_back(hostToXdr(pair.first),
                                   hostToXdr(pair.second));
        }
        std::sort(out.map().begin(), out.map().end());
    }
    else if (std::holds_alternative<uint64_t>(*obj))
    {
        out.type(SCO_U64);
        out.u64() = std::get<uint64_t>(*obj);
    }
    else if (std::holds_alternative<int64_t>(*obj))
    {
        out.type(SCO_I64);
        out.i64() = std::get<int64_t>(*obj);
    }
    else if (std::holds_alternative<xdr::xstring<>>(*obj))
    {
        out.type(SCO_STRING);
        out.str() = std::get<xdr::xstring<>>(*obj);
    }
    else if (std::holds_alternative<xdr::xvector<uint8_t>>(*obj))
    {
        out.type(SCO_BINARY);
        out.bin() = std::get<xdr::xvector<uint8_t>>(*obj);
    }
    else if (std::holds_alternative<LedgerKey>(*obj))
    {
        out.type(SCO_LEDGERKEY);
        out.lkey() = std::get<LedgerKey>(*obj);
    }
    else if (std::holds_alternative<SCLedgerVal>(*obj))
    {
        out.type(SCO_LEDGERVAL);
        out.lval() = std::get<SCLedgerVal>(*obj);
    }
    else if (std::holds_alternative<Operation>(*obj))
    {
        out.type(SCO_OPERATION);
        out.op().activate() = std::get<Operation>(*obj);
    }
    else if (std::holds_alternative<Transaction>(*obj))
    {
        out.type(SCO_TRANSACTION);
        out.tx().activate() = std::get<Transaction>(*obj);
    }
    return ptr;
}

size_t
HostContext::xdrToHost(std::unique_ptr<SCObject> const& obj)
{
    if (!obj)
    {
        return 0;
    }
    std::unique_ptr<HostObject const> immObj;
    switch (obj->type())
    {
    case SCO_BOX:
        immObj =
            std::make_unique<HostObject const>(HostBox(xdrToHost(obj->box())));
        break;
    case SCO_VEC:
    {
        auto immVec = HostVec();
        for (auto const& v : obj->vec())
        {
            immVec = std::move(immVec).push_back(xdrToHost(v));
        }
        immObj = std::make_unique<HostObject const>(std::move(immVec));
    }
    break;
    case SCO_MAP:
    {
        auto immMap = HostMap();
        for (auto const& pair : obj->map())
        {
            immMap =
                std::move(immMap).set(xdrToHost(pair.key), xdrToHost(pair.val));
        }
        immObj = std::make_unique<HostObject const>(std::move(immMap));
    }
    break;
    case SCO_U64:
        immObj = std::make_unique<HostObject const>(obj->u64());
        break;
    case SCO_I64:
        immObj = std::make_unique<HostObject const>(obj->i64());
        break;
    case SCO_STRING:
        immObj = std::make_unique<HostObject const>(obj->str());
        break;
    case SCO_BINARY:
        immObj = std::make_unique<HostObject const>(obj->bin());
        break;
    case SCO_LEDGERKEY:
        immObj = std::make_unique<HostObject const>(obj->lkey());
        break;
    case SCO_LEDGERVAL:
        immObj = std::make_unique<HostObject const>(obj->lval());
        break;
    case SCO_OPERATION:
        if (!obj->op())
        {
            return 0;
        }
        immObj = std::make_unique<HostObject const>(*obj->op());
        break;
    case SCO_TRANSACTION:
        if (!obj->tx())
        {
            return 0;
        }
        immObj = std::make_unique<HostObject const>(*obj->tx());
        break;
    }
    size_t idx = mObjects.size();
    mObjects.emplace_back(std::move(immObj));
    return idx;
}

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

void
HostContext::registerHostFunction(HostClosure0 clo, std::string const& module,
                                  std::string const& name)
{
    registerHostFunction(0, clo, &dispatchClosure0, module, name);
}

void
HostContext::registerHostFunction(HostClosure1 clo, std::string const& module,
                                  std::string const& name)
{
    registerHostFunction(1, clo, &dispatchClosure1, module, name);
}

void
HostContext::registerHostFunction(HostClosure2 clo, std::string const& module,
                                  std::string const& name)
{
    registerHostFunction(2, clo, &dispatchClosure2, module, name);
}

void
HostContext::registerHostFunction(HostClosure3 clo, std::string const& module,
                                  std::string const& name)
{
    registerHostFunction(3, clo, &dispatchClosure3, module, name);
}

void
HostContext::registerHostFunction(HostClosure4 clo, std::string const& module,
                                  std::string const& name)
{
    registerHostFunction(4, clo, &dispatchClosure4, module, name);
}

void
HostContext::registerHostFunction(HostMemFun0 mf, std::string const& module,
                                  std::string const& name)
{
    using namespace std::placeholders;
    HostClosure0 clo{std::bind(mf, this, _1, _2)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostContext::registerHostFunction(HostMemFun1 mf, std::string const& module,
                                  std::string const& name)
{
    using namespace std::placeholders;
    HostClosure1 clo{std::bind(mf, this, _1, _2, _3)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostContext::registerHostFunction(HostMemFun2 mf, std::string const& module,
                                  std::string const& name)
{
    using namespace std::placeholders;
    HostClosure2 clo{std::bind(mf, this, _1, _2, _3, _4)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostContext::registerHostFunction(HostMemFun3 mf, std::string const& module,
                                  std::string const& name)
{
    using namespace std::placeholders;
    HostClosure3 clo{std::bind(mf, this, _1, _2, _3, _4, _5)};
    registerHostFunction(std::move(clo), module, name);
}

void
HostContext::registerHostFunction(HostMemFun4 mf, std::string const& module,
                                  std::string const& name)
{
    using namespace std::placeholders;
    HostClosure4 clo{std::bind(mf, this, _1, _2, _3, _4, _5, _6)};
    registerHostFunction(std::move(clo), module, name);
}

fizzy::ExecutionResult
HostContext::mapNew(fizzy::Instance& instance, fizzy::ExecutionContext& exec)
{
    return newObject<HostMap>();
}

fizzy::ExecutionResult
HostContext::mapPut(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                    uint64_t map, uint64_t key, uint64_t val)
{
    auto mapV = HostVal::fromPayload(map);
    auto keyV = HostVal::fromPayload(key);
    auto valV = HostVal::fromPayload(val);

    auto const& mapObj = getObject(mapV);

    if (mapObj && std::holds_alternative<HostMap>(*mapObj))
    {
        return newObject<HostMap>(std::get<HostMap>(*mapObj).set(keyV, valV));
    }
    else
    {
        return HostVal::fromStatus(0);
    }
}

fizzy::ExecutionResult
HostContext::mapGet(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                    uint64_t map, uint64_t key)
{
    auto mapV = HostVal::fromPayload(map);
    auto keyV = HostVal::fromPayload(key);

    auto const& mapObj = getObject(mapV);

    if (mapObj && std::holds_alternative<HostMap>(*mapObj))
    {
        auto* valPtr = std::get<HostMap>(*mapObj).find(keyV);
        if (valPtr)
        {
            return *valPtr;
        }
    }
    return HostVal::fromStatus(0);
}

fizzy::ExecutionResult
HostContext::logValue(fizzy::Instance& instance, fizzy::ExecutionContext& exec,
                      uint64_t val)
{
    CLOG_INFO(Tx, "contract called log_value({})", HostVal::fromPayload(val));
    return HostVal::fromVoid();
}

fizzy::ExecutionResult
HostContext::getCurrentLedgerNum(fizzy::Instance& instance,
                                 fizzy::ExecutionContext& exec)
{
    uint32_t num = getLedgerTxn().loadHeader().current().ledgerSeq;
    return HostVal::fromU32(num);
}

fizzy::ExecutionResult
HostContext::getCurrentLedgerCloseTime(fizzy::Instance& instance,
                                       fizzy::ExecutionContext& exec)
{
    TimePoint closeTime =
        getLedgerTxn().loadHeader().current().scpValue.closeTime;
    return HostVal::fromTimePt(closeTime);
}

HostContext::HostContext()
{
    // Object 0 is predefined to always be a null unique_ptr, so we can return
    // a reference to it in contexts where users access invalid objects.
    mObjects.emplace_back(nullptr);
    registerHostFunction(&HostContext::mapNew, "env", "map_new");
    registerHostFunction(&HostContext::mapPut, "env", "map_put");
    registerHostFunction(&HostContext::mapGet, "env", "map_get");
    registerHostFunction(&HostContext::logValue, "env", "log_value");
    registerHostFunction(&HostContext::getCurrentLedgerNum, "env",
                         "get_current_ledger_num");
    registerHostFunction(&HostContext::getCurrentLedgerCloseTime, "env",
                         "get_current_ledger_close_time");
}

}