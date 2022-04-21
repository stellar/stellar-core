#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-contract.h"
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

static constexpr ProtocolVersion FIRST_PROTOCOL_SUPPORTING_SMART_CONTRACTS =
    ProtocolVersion::V_20;

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
//        │[1] = Amount         │ HostObject││   let m: u64 = load();        ││
//        │[2] = HostMap    ◀───┼───────────┼┼── let m: u64 = map_put(m,k,v);││
//        │...                  │           ││   save(m);                    ││
//        └─────────────────────┘           ││}                              ││
//                   ▲                      ││                               ││
//                   │                      ││                               ││
//                   │                      ││cooked                         ││
//                   │                      ││------                         ││
//               Load/save                  ││                               ││
//              content of                  ││fn deposit(k: Val, v: Val)     ││
//              HostObjects                 ││{                              ││
//                   │                      ││   let m: Map = load<Map>();   ││
//                   ▼                      ││   m.put(k,v);                 ││
//              ┌─────────┐                 ││   save<Map>(m);               ││
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

using StatusTypeAndCode = std::pair<uint32_t, uint32_t>;

class HostVal
{
    uint64_t mPayload{0};
    HostVal(uint64_t payload) : mPayload(payload)
    {
    }

    static const size_t WORD_BITS = 64;
    static const size_t TAG_BITS = 3;
    static const uint64_t TAG_MASK = (1ULL << TAG_BITS) - 1;
    static_assert(TAG_MASK == 0x7);

    // Objects and statuses are split into a major and minor part.
    static const size_t MAJOR_BITS = 32;
    static const size_t MINOR_BITS = 28;
    static const uint64_t MAJOR_MASK = (1ULL << MAJOR_BITS) - 1;
    static const uint64_t MINOR_MASK = (1ULL << MINOR_BITS) - 1;
    static_assert(MAJOR_MASK == 0xffff'ffff);
    static_assert(MINOR_MASK == 0x0fff'ffff);

    // Words are roughly composed of [MAJOR:32 | MINOR:28 | TAG:3 | 0/1 ]
    static_assert(MAJOR_BITS + MINOR_BITS + TAG_BITS + 1 == WORD_BITS);

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
        releaseAssert((u63 & (1ULL << (WORD_BITS - 1))) == 0);
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
        releaseAssert(body < (1ULL << (WORD_BITS - TAG_BITS)));
        releaseAssert(tag < (1 << TAG_BITS));
        body <<= TAG_BITS;
        body |= tag;
        return HostVal{(body << 1) | 1};
    }

    static HostVal
    fromObjectTypeAndHandle(uint32_t objectType, uint32_t handle)
    {
        releaseAssert(objectType < (1UL << MINOR_BITS));
        return fromBodyAndTag(
            (uint64_t(handle) << MINOR_BITS) | uint64_t(objectType), TAG_OBJECT);
    }

  public:
    HostVal() : mPayload(fromVoid().mPayload)
    {
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
        return uint8_t((mPayload >> 1) & TAG_MASK);
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
        return (mPayload >> (TAG_BITS + 1));
    }

    uint32_t
    getObjectType() const
    {
        releaseAssert(hasTag(TAG_OBJECT));
        return uint32_t(getBody() & MINOR_MASK);
    }

    bool
    isObjectType(uint32_t objectType) const
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

    StatusTypeAndCode
    asStatusTypeAndCode() const
    {
        releaseAssert(isStatus());
        uint64_t body = getBody();
        uint32_t code = uint32_t(body >> MINOR_BITS);
        uint32_t type = uint32_t(body);
        return std::make_pair(type, code);
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
        // a lie from the user; we just embed the object type when packing a
        // handle into an object HostVal, so the user can observe it without
        // calling a host function.
        return size_t(getBody() >> MINOR_BITS);
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
    fromStatusTypeAndCode(StatusTypeAndCode typeAndCode)
    {
        releaseAssert(typeAndCode.first < (1UL << MINOR_BITS));
        uint64_t type = uint64_t(typeAndCode.first);
        uint64_t code = uint64_t(typeAndCode.second) << MINOR_BITS;
        return fromBodyAndTag(code | type, TAG_STATUS);
    }
    static HostVal
    statusOK()
    {
        return fromStatusTypeAndCode(std::make_pair(SST_OK, 0));
    }
    static HostVal
    statusUnknownError()
    {
        return fromStatusTypeAndCode(std::make_pair(SST_UNKNOWN_ERROR, 0));
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
        return fromBodyAndTag(uint64_t(i32) & MAJOR_MASK, TAG_I32);
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
        return fromObjectTypeAndHandle(uint8_t(ty), uint64_t(idx));
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

class HostContext;
struct HostValInContext
{
    HostVal mVal;
    HostContext const* mCtx;
    size_t getHash() const;
    bool operator==(HostValInContext const& other) const;
    bool
    operator!=(HostValInContext const& other) const
    {
        return !(*this == other);
    }
    bool operator<(HostValInContext const& other) const;
};

}

namespace std
{
template <> struct hash<stellar::HostValInContext>
{
    size_t
    operator()(stellar::HostValInContext const& hv) const noexcept
    {
        return hv.getHash();
    }
};
}

namespace stellar
{
using HostBox = immer::box<HostValInContext>;
using HostVec = immer::flex_vector<HostValInContext>;
using HostMap = immer::map<HostValInContext, HostValInContext>;
using HostBigInt = boost::multiprecision::cpp_int;
using HostBigRat = boost::multiprecision::cpp_rational;
using HostObject =
    std::variant<HostBox, HostVec, HostMap, uint64_t, int64_t, xdr::xstring<>,
                 xdr::xvector<uint8_t>, HostBigInt, HostBigRat,
                 xdr::pointer<LedgerKey>, xdr::pointer<Operation>,
                 xdr::pointer<OperationResult>, xdr::pointer<Transaction>,
                 Asset, Price, AccountID>;

static_assert(sizeof(HostBox) == 1 * sizeof(intptr_t),
              "unexpected HostBox size");
static_assert(sizeof(HostVec) == 4 * sizeof(intptr_t),
              "unexpected HostVec size");
static_assert(sizeof(HostMap) == 2 * sizeof(intptr_t),
              "unexpected HostMap size");
static_assert(sizeof(HostBigInt) == 4 * sizeof(intptr_t),
              "unexpected HostBigInt size");
static_assert(sizeof(HostBigRat) == 8 * sizeof(intptr_t),
              "unexpected HostBigRat size");
static_assert(sizeof(HostObject) == 10 * sizeof(intptr_t),
              "unexpected HostObject size");

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
HostVal::fromObject<HostBigInt>(size_t idx)
{
    return fromObject(SCO_BIGINT, idx);
}

template <>
inline HostVal
HostVal::fromObject<HostBigRat>(size_t idx)
{
    return fromObject(SCO_BIGRAT, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::pointer<LedgerKey>>(size_t idx)
{
    return fromObject(SCO_LEDGERKEY, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::pointer<Operation>>(size_t idx)
{
    return fromObject(SCO_OPERATION, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::pointer<OperationResult>>(size_t idx)
{
    return fromObject(SCO_OPERATION_RESULT, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::pointer<Transaction>>(size_t idx)
{
    return fromObject(SCO_TRANSACTION, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::pointer<Asset>>(size_t idx)
{
    return fromObject(SCO_ASSET, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::pointer<Price>>(size_t idx)
{
    return fromObject(SCO_PRICE, idx);
}

template <>
inline HostVal
HostVal::fromObject<xdr::pointer<AccountID>>(size_t idx)
{
    return fromObject(SCO_ACCOUNTID, idx);
}

}