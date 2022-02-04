// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/InvokeContractOpFrame.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "transactions/ContractHostTypes.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include <Tracy.hpp>
#include <TracyC.h>
#include <fizzy/execute.hpp>
#include <fizzy/lib/fizzy/instantiate.hpp>
#include <fizzy/parser.hpp>
#include <fizzy/types.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <xdrpp/marshal.h>
namespace stellar
{
InvokeContractOpFrame::InvokeContractOpFrame(Operation const& op,
                                             OperationResult& res,
                                             TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mInvokeContract(mOperation.body.invokeContractOp())
{
}

bool
InvokeContractOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 19;
}

}

namespace stellar
{

bool
InvokeContractOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    ZoneNamedN(applyZone, "InvokeContractOp apply", true);
    LedgerTxn ltxInner(ltx);
    HostContext& hostCtx = mParentTx.getHostContext();
    HostContextTxn htx(hostCtx.beginOpTxn(*this, ltxInner));

    auto codeLtxEntry = stellar::loadContractCode(
        ltxInner, mInvokeContract.owner, mInvokeContract.contractID);
    ContractCodeEntry const& codeEntry =
        codeLtxEntry.current().data.contractCode();
    switch (codeEntry.body.type())
    {
    case CONTRACT_CODE_WASM:
    {
        try
        {
            // Parse the contract code.
            auto const& wasm = codeEntry.body.wasm();
            std::basic_string_view<uint8_t> codeView(wasm.code.data(),
                                                     wasm.code.size());

            std::unique_ptr<const fizzy::Module> mod = [&]() {
                ZoneNamedN(parseZone, "parse WASM", true);
                return fizzy::parse(codeView);
            }();

            // Resolve function imports.
            std::vector<fizzy::ExternalFunction> importedFunctions =
                fizzy::resolve_imported_functions(*mod,
                                                  hostCtx.getHostFunctions());

            // Instantiate the module.
            std::unique_ptr<fizzy::Instance> instance = [&]() {
                ZoneNamedN(instantiateZone, "instantiate WASM", true);
                return fizzy::instantiate(std::move(mod),
                                          std::move(importedFunctions));
            }();

            // Look up the requested function.
            std::optional<fizzy::ExternalFunction> func_opt =
                fizzy::find_exported_function(*instance,
                                              mInvokeContract.function);

            // If it exists...
            if (func_opt.has_value())
            {
                // Extend the local environment.
                hostCtx.extendEnvironment(mInvokeContract.locals);

                // Look up requested symbols and pass as args to func.
                std::vector<fizzy::Value> fizzyArgs;
                auto const& argTypes = func_opt->input_types;
                bool typesMatch =
                    argTypes.size() == mInvokeContract.arguments.size();

                CLOG_INFO(Tx, "invoking WASM contract {} func {}",
                          mInvokeContract.contractID, mInvokeContract.function);

                size_t i = 0;
                while (typesMatch && (i < argTypes.size()))
                {
                    if (argTypes[i] != fizzy::ValType::i64)
                    {
                        typesMatch = false;
                        break;
                    }
                    auto const& argName = mInvokeContract.arguments.at(i);
                    auto a = hostCtx.getEnv(argName);
                    if (!a)
                    {
                        CLOG_WARNING(Tx, "Can't find '{}' in environment",
                                     argName);
                        innerResult().code(INVOKE_CONTRACT_MALFORMED);
                        return false;
                    }
                    CLOG_INFO(Tx, "   arg {}: {}={}", i, argName, *a);
                    fizzyArgs.emplace_back(a->payload());
                    ++i;
                }

                // Fail on type mismatch.
                if (!typesMatch)
                {
                    CLOG_WARNING(Tx, "invocation type mismatch");
                    innerResult().code(INVOKE_CONTRACT_MALFORMED);
                    return false;
                }

                // Execute WASM.
                fizzy::ExecutionContext ctx;
                auto result = [&]() {
                    ZoneNamedN(execZone, "exec WASM", true);
                    return func_opt.value().function(*instance,
                                                     fizzyArgs.data(), ctx);
                }();

                // Convert results.
                if (result.trapped)
                {
                    CLOG_WARNING(Tx, "contract {} func {} trapped",
                                 mInvokeContract.contractID,
                                 mInvokeContract.function);
                    innerResult().code(INVOKE_CONTRACT_TRAPPED);
                    innerResult().trapCode().type(CONTRACT_CODE_WASM);
                    innerResult().trapCode().wasmTrap() = WASM_TRAP_UNSPECIFIED;
                }
                else
                {
                    CLOG_INFO(Tx, "contract {} func {} succeeded",
                              mInvokeContract.contractID,
                              mInvokeContract.function);
                    // Contracts must return exactly one value.
                    if (!result.has_value)
                    {
                        CLOG_WARNING(Tx,
                                     "contract function did not return value");
                        innerResult().code(INVOKE_CONTRACT_HOST_ERR);
                        return false;
                    }
                    if (func_opt->output_types.size() != 1)
                    {
                        CLOG_WARNING(Tx,
                                     "contract function has unexpected number "
                                     "of return types: {}",
                                     func_opt->output_types.size());
                        innerResult().code(INVOKE_CONTRACT_HOST_ERR);
                        return false;
                    }

                    // FIXME: we might also want to allow the invocation to ask
                    // to materialize one of the local refs _back_ to a value to
                    // return to the user / write to the txresults set.
                    innerResult().code(INVOKE_CONTRACT_SUCCESS);
                    HostVal hv =
                        HostVal::fromPayload(result.value.as<int64_t>());
                    CLOG_INFO(Tx, "contract return value {}", hv);
                    innerResult().returnVal() = hostCtx.hostToXdr(hv);
                }
            }
        }
        catch (std::runtime_error& e)
        {
            CLOG_WARNING(Tx, "contract failed: {}", e.what());
            innerResult().code(INVOKE_CONTRACT_HOST_ERR);
            return false;
        }
        break;
    }

    default:
        innerResult().code(INVOKE_CONTRACT_HOST_ERR);
        return false;
    }
    htx.commit();
    ltxInner.commit();
    innerResult().code(INVOKE_CONTRACT_SUCCESS);
    return true;
}

bool
InvokeContractOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    return true;
}

void
InvokeContractOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
    keys.emplace(
        contractCodeKey(mInvokeContract.owner, mInvokeContract.contractID));
    keys.emplace(
        contractDataKey(mInvokeContract.owner, mInvokeContract.contractID));
}
}
