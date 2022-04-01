// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/GlobalChecks.h"
#include "xdr/Stellar-transaction.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <fizzy/execute.hpp>
#include <immer/box.hpp>
#include <immer/flex_vector.hpp>
#include <immer/map.hpp>
#include <xdrpp/xdrpp/types.h>

#include <cstdint>
#include <iosfwd>
#include <memory>
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

  public:
    static const uint8_t TAG_U32 = 0;
    static const uint8_t TAG_I32 = 1;
    static const uint8_t TAG_STATIC = 2;
    static const uint8_t TAG_OBJECT = 3;
    static const uint8_t TAG_SYMBOL = 4;
    static const uint8_t TAG_BITSET = 5;
    static const uint8_t TAG_STATUS = 6;

    static const uint32_t STATIC_VOID = 0;
    static const uint32_t STATIC_TRUE = 1;
    static const uint32_t STATIC_FALSE = 2;

    static HostVal
    fromU63(uint64_t u63)
    {
        releaseAssert((u63 & (1ULL << 63)) == 0);
        return HostVal{u63 << 1};
    }

    static HostVal
    fromPositiveInt64(int64_t i)
    {
        releaseAssert(i >= 0);
        uint64_t u63 = uint64_t(i);
        return fromU63(u63);
    }

  private:
    static HostVal
    fromBodyAndTag(uint64_t body, uint8_t tag)
    {
        releaseAssert(body < (1ULL << 60));
        releaseAssert(tag < 8);
        return HostVal{(body << 4) | (tag << 1) | 1};
    }

    static HostVal
    fromObjectTypeAndIndex(uint8_t objectType, uint64_t index)
    {
        releaseAssert(objectType < 16);
        releaseAssert(index < (1ULL << 48));
        return fromBodyAndTag((index << 12) | objectType, TAG_OBJECT);
    }

  public:
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

    bool
    isU63() const
    {
        return !(mPayload & 1);
    }

    int64_t
    asU63() const
    {
        return mPayload >> 1;
    }

    uint8_t
    getTag() const
    {
        releaseAssert(!isU63());
        return uint8_t((mPayload >> 1) & 7);
    }

    bool
    hasTag(uint8_t tag) const
    {
        return !isU63() && (getTag() == tag);
    }

    uint64_t
    getBody() const
    {
        releaseAssert(!isU63());
        return (mPayload >> 4);
    }

    uint16_t
    getObjectType() const
    {
        releaseAssert(hasTag(TAG_OBJECT));
        return uint8_t(getBody() & 0xfff);
    }

    bool
    isObjectType(uint16_t objectType) const
    {
        return hasTag(TAG_OBJECT) && (getObjectType() == objectType);
    }

    bool
    isStaticVal(uint32_t staticVal) const
    {
        return hasTag(TAG_STATIC) && (getBody() == staticVal);
    }

    bool
    isVoid() const
    {
        return isStaticVal(STATIC_VOID);
    }

    bool
    isBool() const
    {
        return isStaticVal(STATIC_TRUE) || isStaticVal(STATIC_FALSE);
    }

    bool
    asBool() const
    {
        releaseAssert(isBool());
        return isStaticVal(STATIC_TRUE);
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
    isObject() const
    {
        return hasTag(TAG_OBJECT);
    }

    size_t
    asObject() const
    {
        releaseAssert(isObject());
        // NB: we never _extract_ the object type of a HostVal since it might be
        // a lie from the user; we just embed the object type when packing an
        // index into an object HostVal, so the user can observe it without
        // calling a host function.
        return size_t(getBody() >> 12);
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
        return fromBodyAndTag(STATIC_VOID, TAG_STATIC);
    }
    static HostVal
    fromBool(bool b)
    {
        return fromBodyAndTag(b ? STATIC_TRUE : STATIC_FALSE, TAG_STATIC);
    }
    static HostVal
    fromStatus(uint32_t status)
    {
        return fromBodyAndTag(uint64_t(status), TAG_STATUS);
    }
    static HostVal
    fromStatic(SCStatic st)
    {
        return fromBodyAndTag(uint64_t(st), TAG_STATIC);
    }
    static HostVal
    fromU32(uint32_t u32)
    {
        return fromBodyAndTag(uint64_t(u32), TAG_U32);
    }
    static HostVal
    fromI32(int32_t i32)
    {
        return fromBodyAndTag(uint64_t(i32) & 0xffffffffULL, TAG_I32);
    }
    static HostVal fromSymbol(std::string const& s);

    static HostVal
    fromBitSet(uint64_t bits)
    {
        return fromBodyAndTag(bits, TAG_BITSET);
    }

    static HostVal
    fromObject(SCObjectType ty, size_t idx)
    {
        return fromObjectTypeAndIndex(uint8_t(ty), uint64_t(idx));
    }

    template <typename HObj>
    static HostVal
    fromObject(size_t idx)
    {
        throw std::runtime_error("unhandled object type");
    }

    operator fizzy::ExecutionResult() const
    {
        return fizzy::Value{payload()};
    }
};

std::ostream& operator<<(std::ostream& out, HostVal const& v);
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

namespace stellar
{
using HostBox = immer::box<HostVal>;
using HostVec = immer::flex_vector<HostVal>;
using HostMap = immer::map<HostVal, HostVal>;
using HostBigNum = boost::multiprecision::cpp_int;
using HostObject =
    std::variant<HostBox, HostVec, HostMap, uint64_t, int64_t, xdr::xstring<>,
                 xdr::xvector<uint8_t>, LedgerKey, SCLedgerVal, Operation,
                 Transaction, HostBigNum>;

template <>
inline HostVal
HostVal::fromObject<HostBox>(size_t idx)
{
    return fromObject(SCO_BOX, idx);
}

template <>
inline HostVal
HostVal::fromObject<HostVec>(size_t idx)
{
    return fromObject(SCO_VEC, idx);
}

template <>
inline HostVal
HostVal::fromObject<HostMap>(size_t idx)
{
    return fromObject(SCO_MAP, idx);
}

template <>
inline HostVal
HostVal::fromObject<uint64_t>(size_t idx)
{
    return fromObject(SCO_U64, idx);
}

template <>
inline HostVal
HostVal::fromObject<int64_t>(size_t idx)
{
    return fromObject(SCO_I64, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::xstring<>>(size_t idx)
{
    return fromObject(SCO_STRING, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::xvector<uint8_t>>(size_t idx)
{
    return fromObject(SCO_BINARY, idx);
}

template <>
inline HostVal
HostVal::fromObject<LedgerKey>(size_t idx)
{
    return fromObject(SCO_LEDGERKEY, idx);
}

template <>
inline HostVal
HostVal::fromObject<SCLedgerVal>(size_t idx)
{
    return fromObject(SCO_LEDGERVAL, idx);
}

template <>
inline HostVal
HostVal::fromObject<Operation>(size_t idx)
{
    return fromObject(SCO_OPERATION, idx);
}

template <>
inline HostVal
HostVal::fromObject<Transaction>(size_t idx)
{
    return fromObject(SCO_TRANSACTION, idx);
}

template <>
inline HostVal
HostVal::fromObject<HostBigNum>(size_t idx)
{
    return fromObject(SCO_BIGNUM, idx);
}
}