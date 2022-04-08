#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "transactions/contracts/HostVal.h"
#include "xdr/Stellar-ledger-entries.h"
#include <fizzy/execute.hpp>

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

class HostFunctions;

using HostMemFun0 = fizzy::ExecutionResult (HostFunctions::*)(
    fizzy::Instance&, fizzy::ExecutionContext&);
using HostMemFun1 = fizzy::ExecutionResult (HostFunctions::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t);
using HostMemFun2 = fizzy::ExecutionResult (HostFunctions::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t);
using HostMemFun3 = fizzy::ExecutionResult (HostFunctions::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t);
using HostMemFun4 = fizzy::ExecutionResult (HostFunctions::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t);
using HostMemFun5 = fizzy::ExecutionResult (HostFunctions::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t);
using HostMemFun6 = fizzy::ExecutionResult (HostFunctions::*)(
    fizzy::Instance&, fizzy::ExecutionContext&, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t);

class HostContext;

class HostFunctions
{
    HostContext& mCtx;
    std::vector<fizzy::ImportedFunction> mImportedFunctions;

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
        mImportedFunctions.emplace_back(std::move(ifunc));
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
    HostFunctions(HostContext&);

    std::vector<fizzy::ImportedFunction> const&
    getImportedFunctions() const
    {
        return mImportedFunctions;
    }

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