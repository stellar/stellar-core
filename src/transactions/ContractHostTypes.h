// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"

#include <fizzy/execute.hpp>
#include <immer/box.hpp>
#include <immer/flex_vector.hpp>
#include <immer/map.hpp>
#include <xdrpp/xdrpp/types.h>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <variant>

namespace stellar
{

// We have two representations of values:
//
//  1. Fully XDR-backed representations, which have two more sub-cases:
//    1.1. Stellar "classic" ledger types: LedgerKey, LedgerEntry, etc.
//    1.2. Smart-contract specific types, with yet two _more_ sub-cases:
//      1.2.1. SCObject(...), which directly contains classic types
//      1.2.2. SCVal(...), which may directly contain SCObject(...)
//             (but may also contain other small value types like u32)
//
//  2. "Host" representations of the smart-contract types, with two sub-cases:
//    2.1. HostObject(...), mirrors SCObject & directly contains classic types
//    2.2. HostVal(...), mirrors SCVal & may _indirectly reference_ HostObjects
//
// The reason for having separate "host" and "SC" representations is that
// the "host" forms have specific characteristics that let them be used by
// smart contracts in ways "SC" forms don't support:
//
//   (a) HostVals don't contain HostObjects, they reference them by handle,
//       whereas SCVals directly contain SCObjects. Handles are ephemeral.
//
//   (b) HostVals are bit-packed into exactly 64 bits, whereas SCVals may be
//       multiple words (but have meaningful XDR union-based access).
//
//   (c) HostMaps allow hashed lookup by key, whereas SCMaps can only support
//       linear search (since they're just XDR key-val arrays).
//
//   (d) HostObject maps and vecs allow shallow copies with structural sharing,
//       whereas SCObjects can only support full deep copies.
//
// The reason for having separate Object vs. Val types, in either
// representation, is to support point (a) above: splitting HostVal from
// HostObject apart via indirection through integer handles.
//
// There are two reasons for using integer handles:
//
//   - To reduce code size and the amount of in-WASM execution that happens,
//     keeping the majority of code in common host functions that WASM code just
//     calls, rather than bundling code to serialize, deserialize, copy,
//     allocate, or manipulate in WASM code itself.
//
//   - WASM only supports primitive types like u32 or u64 in function
//     interfaces, so we have to pass or return those in any case, not large
//     structures. HostObjects never actually traverse the interface, only
//     HostVals that hold handles pointing to HostObjects on the C++ side.
//
// Besides these "implementation differences", from the user's perspective, the
// HostVal and SCVal types are semantically the same thing, the implementations
// are direct mirrors of one another, as are the HostObject and SCObject types.
// There's no particular reason to reveal the difference to users, and while the
// "raw" form of host function interfaces will take and return simple u64 values
// (the payload of a HostVal), we expect language bindings to provide a "cooked"
// or strongly-typed convenience layer for actual use by contract code, and that
// layer may just want to call the `HostVal` wrapper `Val` and the
// `HostObject`-handle wrapper `Object`.
//
// Diagram follows:
//
//  ┌─────────────────────────────────┐
//  │Transaction (XDR)                │
//  │                                 │
//  │Env                              │
//  │---                              │
//  │"k" = SCVal(SCObject(AccountID)) │
//  │"v" = SCVal(SCObject(Amount))    │
//  │                                 │
//  │invoke: "deposit(k, v)"          │
//  └─────────────────────────────────┘
//                   │
//                   │
//            Extend host env
//            with converted
//              values and
//            invoke contract
//            with named args
//                   │
//                   │
//                   ▼
//        ┌─────────────────────┐
//        │HostContext          │
//        │                     │
//        │HostEnv              │           ┌─────────────────────────────────┐
//        │-------              │           │WASM VM                          │
//        │"k" = HostVal(Obj #0)│           │┌───────────────────────────────┐│
//        │"v" = HostVal(Obj #1)│           ││WASM contract                  ││
//        │...                  │           ││                               ││
//        │                     │           ││raw                            ││
//        │                     │           ││---                            ││
//        │HostObjects          │           ││                               ││
//        │-----------          │           ││fn deposit(k: u64, v: u64)     ││
//        │[0] = AccountID      │ Handle to ││{                              ││
//        │[1] = Amount         │ HostObject││   let m: u64 = restore();     ││
//        │[2] = HostMap    ◀───┼───────────┼┼── let m: u64 = map_put(m,k,v);││
//        │...                  │           ││   persist(m);                 ││
//        └─────────────────────┘           ││}                              ││
//                   ▲                      ││                               ││
//                   │                      ││                               ││
//                   │                      ││cooked                         ││
//                   │                      ││------                         ││
//               Load/save                  ││                               ││
//              content of                  ││fn deposit(k: Val, v: Val)     ││
//              HostObjects                 ││{                              ││
//                   │                      ││   let m: Map = restore<Map>();││
//                   ▼                      ││   m.put(k,v);                 ││
//              ┌─────────┐                 ││   m.persist();                ││
//              │LedgerTxn│                 ││}                              ││
//              └─────────┘                 ││                               ││
//                   ▲                      ││                               ││
//                   │                      │└───────────────────────────────┘│
//                   ▼                      └─────────────────────────────────┘
//      ┌─────────────────────────┐
//      │Ledger (XDR)             │
//      │                         │
//      │                         │
//      │LedgerKey(AccountID)     │
//      │LedgerKey(TrustLine)     │
//      │...                      │
//      │LedgerEntry(Account)     │
//      │...                      │
//      │LedgerEntry(TrustLine)   │
//      │...                      │
//      │LedgerEntry(ContractData)│
//      └─────────────────────────┘

class HostVal
{
    uint64_t mPayload{0};
    HostVal(uint64_t payload) : mPayload(payload)
    {
    }

    static const uint64_t OBJ_VOID = 0;
    static const uint64_t OBJ_BOOL_TRUE = 1;
    static const uint64_t OBJ_BOOL_FALSE = 2;
    static const uint64_t OBJ_DYNAMIC_BASE = 0x10000;

    static inline uint64_t
    rotate_right(uint64_t x, size_t s)
    {
        uint64_t r = s & 63;
        return (x >> r) | (x << (64 - r));
    }

    static inline uint64_t
    rotate_left(uint64_t x, size_t s)
    {
        uint64_t r = s & 63;
        return (x << r) | (x >> (64 - r));
    }

    static HostVal
    fromTagAndBody(uint16_t tag3, uint64_t body48)
    {
        releaseAssert(tag3 < 7);
        body48 = rotate_left(body48, 16);
        releaseAssert((body48 & 0xffff) == 0);
        return HostVal{body48 | tag3};
    }

  public:
    static const uint16_t TAG_OBJECT = 0;
    static const uint16_t TAG_U32 = 1;
    static const uint16_t TAG_I32 = 2;
    static const uint16_t TAG_SYMBOL = 3;
    static const uint16_t TAG_BITSET = 4;
    static const uint16_t TAG_TIMEPT = 5;
    static const uint16_t TAG_STATUS = 6;

    bool
    operator==(HostVal const& other) const
    {
        return other.payload() == mPayload;
    }

    bool
    operator<(HostVal const& other) const
    {
        return other.payload() < mPayload;
    }

    uint16_t
    getTag() const
    {
        return uint16_t(mPayload);
    }

    uint64_t
    getBody() const
    {
        return (mPayload >> 16);
    }

    bool
    hasTag(uint8_t tag) const
    {
        return getTag() == tag;
    }

    bool
    isVoid() const
    {
        return hasTag(TAG_OBJECT) && (getBody() == OBJ_VOID);
    }

    bool
    isBool() const
    {
        auto body = getBody();
        return hasTag(TAG_OBJECT) &&
               (body == OBJ_BOOL_TRUE || body == OBJ_BOOL_FALSE);
    }

    bool
    asBool() const
    {
        releaseAssert(isBool());
        return getBody() == OBJ_BOOL_TRUE;
    }

    bool
    isStatus() const
    {
        return hasTag(TAG_STATUS);
    }

    uint32_t
    asStatus() const
    {
        releaseAssert(isStatus());
        return uint32_t(getBody());
    }

    bool
    isU32() const
    {
        return hasTag(TAG_U32);
    }

    uint32_t
    asU32() const
    {
        releaseAssert(isU32());
        return uint32_t(getBody());
    }

    bool
    isI32() const
    {
        return hasTag(TAG_I32);
    }

    int32_t
    asI32() const
    {
        releaseAssert(isI32());
        return int32_t(getBody());
    }

    bool
    isSymbol() const
    {
        return hasTag(TAG_SYMBOL);
    }

    std::string asSymbol() const;

    bool
    isBitSet() const
    {
        return hasTag(TAG_BITSET);
    }

    uint64_t
    asBitSet() const
    {
        releaseAssert(isBitSet());
        return getBody();
    }

    bool
    isTimePt() const
    {
        return hasTag(TAG_TIMEPT);
    }

    uint64_t
    asTimePt() const
    {
        releaseAssert(isTimePt());
        return getBody();
    }

    bool
    isObject() const
    {
        return hasTag(TAG_OBJECT) && getBody() >= OBJ_DYNAMIC_BASE;
    }

    size_t
    asObject() const
    {
        releaseAssert(isObject());
        return size_t(getBody() - OBJ_DYNAMIC_BASE);
    }

    uint64_t
    payload() const
    {
        return mPayload;
    }
    static HostVal
    fromPayload(uint64_t u)
    {
        return HostVal(u);
    }
    static HostVal
    fromVoid()
    {
        return fromTagAndBody(TAG_OBJECT, OBJ_VOID);
    }
    static HostVal
    fromBool(bool b)
    {
        return fromTagAndBody(TAG_OBJECT, b ? OBJ_BOOL_TRUE : OBJ_BOOL_FALSE);
    }
    static HostVal
    fromStatus(uint32_t status)
    {
        return fromTagAndBody(TAG_STATUS, uint64_t(status));
    }
    static HostVal
    fromU32(uint32_t u32)
    {
        return fromTagAndBody(TAG_U32, uint64_t(u32));
    }
    static HostVal
    fromI32(int32_t i32)
    {
        return fromTagAndBody(TAG_I32, uint64_t(i32) & 0xffffffffULL);
    }
    static HostVal fromSymbol(std::string const& s);

    static HostVal
    fromTimePt(uint64_t time)
    {
        return fromTagAndBody(TAG_TIMEPT, time);
    }
    static HostVal
    fromBitSet(uint64_t bits)
    {
        return fromTagAndBody(TAG_BITSET, bits);
    }
    static HostVal
    fromObject(size_t idx)
    {
        return fromTagAndBody(TAG_OBJECT, uint64_t(idx) + OBJ_DYNAMIC_BASE);
    }

    operator fizzy::ExecutionResult() const
    {
        return fizzy::Value{payload()};
    }
};

std::ostream& operator<<(std::ostream& out, HostVal const& v);

using HostBox = immer::box<HostVal>;
using HostVec = immer::flex_vector<HostVal>;
using HostMap = immer::map<HostVal, HostVal>;
using HostObject = std::variant<HostBox, HostVec, HostMap, uint64_t, int64_t,
                                xdr::xstring<>, xdr::xvector<uint8_t>,
                                LedgerKey, SCLedgerVal, Operation, Transaction>;

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

class InvokeContractOpFrame;
struct HostOpContext
{
    HostContext& mHostContext;
    InvokeContractOpFrame& mInvokeOp;
    AbstractLedgerTxn& mLedgerTxn;
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
    std::optional<HostOpContext> mHostOpCtx;

    friend class HostContextTxn;

    template <typename HObj, typename... Args>
    HostVal
    newObject(Args&&... args)
    {
        size_t idx = mObjects.size();
        HObj obj(std::forward<Args>(args)...);
        mObjects.emplace_back(
            std::make_unique<HostObject const>(std::move(obj)));
        return HostVal::fromObject(idx);
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

  public:
    HostContext();

    HostContextTxn
    beginOpTxn(InvokeContractOpFrame& op, AbstractLedgerTxn& ltx)
    {
        releaseAssert(!mHostOpCtx);
        mHostOpCtx.emplace(HostOpContext{*this, op, ltx});
        return HostContextTxn(*this);
    }

    AbstractLedgerTxn&
    getLedgerTxn()
    {
        releaseAssert(mHostOpCtx);
        return mHostOpCtx->mLedgerTxn;
    }

    InvokeContractOpFrame&
    getOpFrame()
    {
        releaseAssert(mHostOpCtx);
        return mHostOpCtx->mInvokeOp;
    }

    void extendEnvironment(SCEnv const& locals);
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

    // Host functions follow -- plumbing above currently supports 0..4 uint64_t
    // arguments.
    fizzy::ExecutionResult mapNew(fizzy::Instance&, fizzy::ExecutionContext&);
    fizzy::ExecutionResult mapPut(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t map, uint64_t key, uint64_t val);
    fizzy::ExecutionResult mapGet(fizzy::Instance&, fizzy::ExecutionContext&,
                                  uint64_t map, uint64_t key);
    fizzy::ExecutionResult logValue(fizzy::Instance&, fizzy::ExecutionContext&,
                                    uint64_t);
    fizzy::ExecutionResult getCurrentLedgerNum(fizzy::Instance&,
                                               fizzy::ExecutionContext&);
    fizzy::ExecutionResult getCurrentLedgerCloseTime(fizzy::Instance&,
                                                     fizzy::ExecutionContext&);
};

}

namespace std
{
template <> struct hash<stellar::HostVal>
{
    size_t
    operator()(stellar::HostVal const& hv) const noexcept
    {
        return std::hash<uint64_t>()(hv.payload());
    }
};
}
