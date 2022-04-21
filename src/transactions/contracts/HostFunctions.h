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

#ifdef BUILD_TESTS
    std::optional<std::function<uint64_t(uint64_t)>> mTestFunctionCallback;
    fizzy::ExecutionResult testHostFunction(fizzy::Instance& instance,
                                            fizzy::ExecutionContext& exec,
                                            uint64_t arg);
#endif
};

}