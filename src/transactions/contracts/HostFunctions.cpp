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

#ifdef BUILD_TESTS
fizzy::ExecutionResult
HostFunctions::testHostFunction(fizzy::Instance& instance,
                                fizzy::ExecutionContext& exec, uint64_t arg)
{
    ZoneScoped;
    if (mTestFunctionCallback)
    {
        return fizzy::Value((*mTestFunctionCallback)(arg));
    }
    else
    {
        return fizzy::Trap;
    }
}
#endif

HostFunctions::HostFunctions(HostContext& hc) : mCtx(hc)
{
#ifdef BUILD_TESTS
    registerHostFunction(&HostFunctions::testHostFunction, "env",
                         "testHostFunction");
#endif
}

}
