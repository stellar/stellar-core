// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/contracts/HostContext.h"
#include "crypto/Hex.h"
#include "ledger/LedgerTxn.h"
#include "transactions/InvokeContractOpFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"
#include <Tracy.hpp>
#include <TracyC.h>
#include <boost/multiprecision/cpp_int/import_export.hpp>
#include <cstdint>
#include <fizzy/execute.hpp>
#include <fizzy/parser.hpp>
#include <variant>

namespace stellar
{

#pragma region transactions

HostContextTxn::HostContextTxn(HostContext& hc)
    : mRollbackPoint(hc.mObjects.size()), mHostCtx(hc)
{
}

HostContextTxn::~HostContextTxn()
{
    if (mRollback)
    {
        mHostCtx.mInvokeCtxs.back().mLedgerTxn.rollback();
        mHostCtx.mObjects.resize(mRollbackPoint);
    }
    mHostCtx.mInvokeCtxs.pop_back();
    if (mHostCtx.mInvokeCtxs.empty())
    {
        // If we're at the top-level, clear the read/write sets.
        mHostCtx.mReadSet.clear();
        mHostCtx.mWriteSet.clear();
    }
}

HostContextTxn
HostContext::beginOpTxn(InvokeContractOpFrame& op, AbstractLedgerTxn& ltx)
{
    releaseAssert(mInvokeCtxs.empty());
    releaseAssert(mReadSet.empty());
    releaseAssert(mWriteSet.empty());
    auto& iop = op.getOperation().body.invokeContractOp();
    mLastOperationResult.code(opINNER);
    mLastOperationResult.tr().type(INVOKE_CONTRACT);
    mLastOperationResult.tr().invokeContractResult() = InvokeContractResult();
    mReadSet.clear();
    mWriteSet.clear();
    for (auto const& r : iop.readSet)
    {
        mReadSet.insert(r);
    }
    for (auto const& w : iop.writeSet)
    {
        mWriteSet.insert(w);
    }
    mInvokeCtxs.emplace_back(
        InvokeContractContext{*this, op, ltx, iop.owner, iop.contractID});
    return HostContextTxn(*this);
}

HostContextTxn
HostContext::beginInnerTxn(AbstractLedgerTxn& innerLtx, AccountID const& owner,
                           int64_t contractID)
{
    releaseAssert(!mInvokeCtxs.empty());
    auto& curr = mInvokeCtxs.back();
    InvokeContractContext next{curr.mHostContext, curr.mInvokeOp, innerLtx,
                               owner, contractID};
    mInvokeCtxs.emplace_back(std::move(next));
    return HostContextTxn(*this);
}

#pragma endregion transactions

#pragma region environment

void
HostContext::extendEnvironment(SCEnv const& locals)
{
    ZoneScoped;
    for (auto const& pair : locals)
    {
        extendEnvironment(pair.key, xdrToHost(pair.val));
    }
}

void
HostContext::extendEnvironment(SCSymbol const& sym, HostVal hv)
{
    mEnv.insert_or_assign(sym, hv);
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

#pragma endregion environment

#pragma region conversions
static std::pair<uint32_t, uint32_t>
getStatusPair(SCStatus const& s)
{
    uint32_t code{0};
    switch (s.type())
    {
    case SST_OK:
        break;
    case SST_WASM_TRAP_CODE:
        code = uint32_t(s.wasmTrap());
        break;
    case SST_HOST_TRAP_CODE:
        code = uint32_t(s.hostTrap());
        break;
    case SST_PAYMENT_RESULT:
        code = uint32_t(s.paymentResult());
        break;
    default:
        return std::make_pair(SST_UNKNOWN, 0);
    }
    return std::make_pair(uint32_t(s.type()), code);
}

static SCStatus
getStatus(std::pair<uint32_t, uint32_t> const& s)
{
    SCStatus status;
    status.type(SST_UNKNOWN);
    switch (SCStatusType(s.first))
    {
    case SST_OK:
        status.type(SST_OK);
        break;
    case SST_WASM_TRAP_CODE:
        if (xdr::xdr_traits<WasmTrapCode>::enum_name(WasmTrapCode(s.second)))
        {
            status.type(SST_WASM_TRAP_CODE);
            status.wasmTrap() = WasmTrapCode(s.second);
        }
        break;
    case SST_HOST_TRAP_CODE:
        if (xdr::xdr_traits<HostTrapCode>::enum_name(HostTrapCode(s.second)))
        {
            status.type(SST_HOST_TRAP_CODE);
            status.hostTrap() = HostTrapCode(s.second);
        }
        break;
    case SST_PAYMENT_RESULT:
        if (xdr::xdr_traits<PaymentResultCode>::enum_name(
                PaymentResultCode(s.second)))
        {
            status.type(SST_PAYMENT_RESULT);
            status.paymentResult() = PaymentResultCode(s.second);
        }
        break;
    default:
        status.type(SST_UNKNOWN);
        break;
    }
    return status;
}

HostVal
HostContext::xdrToHost(SCVal const& v)
{
    ZoneScoped;
    switch (v.type())
    {
    case SCV_U63:
        return HostVal::fromU63(v.u63());
    case SCV_U32:
        return HostVal::fromU32(v.u32());
    case SCV_I32:
        return HostVal::fromI32(v.i32());
    case SCV_STATIC:
        return HostVal::fromStatic(v.ic());
    case SCV_OBJECT:
    {
        SCObjectType ty = SCO_BOX;
        if (v.obj())
        {
            ty = v.obj()->type();
        }
        return HostVal::fromObject(ty, xdrToHost(v.obj()));
    }
    case SCV_SYMBOL:
        return HostVal::fromSymbol(v.sym());
    case SCV_BITSET:
        return HostVal::fromBitSet(v.bits());
    case SCV_STATUS:
        return HostVal::fromStatus(getStatusPair(v.status()));
    }
}

SCVal
HostContext::hostToXdr(HostVal const& hv)
{
    ZoneScoped;
    SCVal out;
    if (hv.isU63())
    {
        out.type(SCV_U63);
        out.u63() = hv.asU63();
    }
    else if (hv.isVoid())
    {
        out.type(SCV_STATIC);
        out.ic() = SCS_VOID;
    }
    else if (hv.isBool())
    {
        out.type(SCV_STATIC);
        out.ic() = hv.asBool() ? SCS_TRUE : SCS_FALSE;
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
    else if (hv.isStatus())
    {
        out.type(SCV_STATUS);
        out.status() = getStatus(hv.asStatus());
    }
    return out;
}

xdr::pointer<SCObject>
HostContext::hostToXdr(std::unique_ptr<HostObject const> const& obj)
{
    ZoneScoped;
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
    else if (std::holds_alternative<OperationResult>(*obj))
    {
        out.type(SCO_OPERATION_RESULT);
        out.ores().activate() = std::get<OperationResult>(*obj);
    }
    else if (std::holds_alternative<Transaction>(*obj))
    {
        out.type(SCO_TRANSACTION);
        out.tx().activate() = std::get<Transaction>(*obj);
    }
    else if (std::holds_alternative<HostBigNum>(*obj))
    {
        out.type(SCO_BIGNUM);
        auto bi = std::back_inserter(out.bn().magnitude);
        boost::multiprecision::export_bits(std::get<HostBigNum>(*obj), bi, 8);
        out.bn().positive = std::get<HostBigNum>(*obj) >= 0;
    }
    else
    {
        throw std::runtime_error("unknown host object type");
    }
    return ptr;
}

size_t
HostContext::xdrToHost(std::unique_ptr<SCObject> const& obj)
{
    ZoneScoped;
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
    case SCO_OPERATION_RESULT:
        if (!obj->ores())
        {
            return 0;
        }
        immObj = std::make_unique<HostObject const>(*obj->ores());
        break;
    case SCO_TRANSACTION:
        if (!obj->tx())
        {
            return 0;
        }
        immObj = std::make_unique<HostObject const>(*obj->tx());
        break;
    case SCO_BIGNUM:
    {
        HostBigNum bn;
        boost::multiprecision::import_bits(bn, obj->bn().magnitude.begin(),
                                           obj->bn().magnitude.end(), 8);
        if (!obj->bn().positive)
        {
            bn = -bn;
        }
        immObj = std::make_unique<HostObject const>(bn);
    }
    break;
    }
    size_t idx = mObjects.size();
    mObjects.emplace_back(std::move(immObj));
    return idx;
}
#pragma endregion conversions

std::variant<HostVal, InvokeContractResultCode>
HostContext::invokeContract(AccountID const& owner, int64_t contractID,
                            std::string const& function,
                            std::vector<HostVal> const& args)
{
    ZoneScoped;
    auto& ltxInner = getLedgerTxn();
    auto codeLtxEntry = stellar::loadContractCode(ltxInner, owner, contractID);

    if (!codeLtxEntry)
    {
        CLOG_WARNING(Tx, "contract owner={} id={} not found",
                     hexAbbrev(owner.ed25519()), contractID);
        return INVOKE_CONTRACT_MALFORMED;
    }

    ContractCodeEntry const& codeEntry =
        codeLtxEntry.current().data.contractCode();

    switch (codeEntry.body.type())
    {
    case CONTRACT_CODE_WASM:
    {
        // Parse the contract code.
        auto const& wasm = codeEntry.body.wasm();
        std::basic_string_view<uint8_t> codeView(wasm.data(), wasm.size());

        std::unique_ptr<const fizzy::Module> mod = [&]() {
            ZoneNamedN(parseZone, "parse WASM", true);
            return fizzy::parse(codeView);
        }();

        // Resolve function imports.
        std::vector<fizzy::ExternalFunction> importedFunctions =
            fizzy::resolve_imported_functions(
                *mod, mHostFunctions.getImportedFunctions());

        // Instantiate the module.
        std::unique_ptr<fizzy::Instance> instance = [&]() {
            ZoneNamedN(instantiateZone, "instantiate WASM", true);
            return fizzy::instantiate(std::move(mod),
                                      std::move(importedFunctions));
        }();

        // Look up the requested function.
        std::optional<fizzy::ExternalFunction> func_opt =
            fizzy::find_exported_function(*instance, function);

        // If it exists...
        if (func_opt.has_value())
        {
            // Look up requested symbols and pass as args to func.
            std::vector<fizzy::Value> fizzyArgs;
            auto const& argTypes = func_opt->input_types;
            bool typesMatch = argTypes.size() == args.size();
            CLOG_INFO(Tx, "invoking WASM contract {} func {}", contractID,
                      function);

            size_t i = 0;
            while (typesMatch && (i < argTypes.size()))
            {
                if (argTypes[i] != fizzy::ValType::i64)
                {
                    CLOG_WARNING(Tx, "arg type {} mismatch", i);
                    typesMatch = false;
                    break;
                }
                CLOG_INFO(Tx, "   arg {}: {}", i, args[i]);
                fizzyArgs.emplace_back(args[i].payload());
                ++i;
            }

            if (func_opt->output_types.size() != 1 ||
                func_opt->output_types[0] != fizzy::ValType::i64)
            {
                CLOG_WARNING(Tx, "return type mismatch");
                typesMatch = false;
            }

            // Fail on type mismatch.
            if (!typesMatch)
            {
                CLOG_WARNING(Tx, "invocation type mismatch");
                return INVOKE_CONTRACT_MALFORMED;
            }

            // Execute WASM.
            fizzy::ExecutionContext ctx;
            auto result = [&]() {
                ZoneNamedN(execZone, "exec WASM", true);
                return func_opt.value().function(*instance, fizzyArgs.data(),
                                                 ctx);
            }();

            // Convert results.
            if (result.trapped)
            {
                CLOG_WARNING(Tx, "contract {} func {} trapped", contractID,
                             function);
                return INVOKE_CONTRACT_TRAPPED;
            }
            else if (!result.has_value)
            {
                // Result _should_ have an error by type, so if not there
                // was something wrong in the VM.
                CLOG_WARNING(Tx, "contract {} func {} returned with no value",
                             contractID, function);
                return INVOKE_CONTRACT_HOST_ERR;
            }
            else
            {
                auto hv = HostVal::fromPayload(result.value.as<uint64_t>());
                CLOG_INFO(Tx, "contract {} func {} succeeded with val {}",
                          contractID, function, hv);
                return hv;
            }
        }
        else
        {
            CLOG_WARNING(Tx, "function '{}' not found in contract {}", function,
                         contractID);
            return INVOKE_CONTRACT_MALFORMED;
        }
    }
    default:
        CLOG_WARNING(Tx, "contract {} is of unknown type", contractID);
        return INVOKE_CONTRACT_HOST_ERR;
    }
}

HostContext::HostContext() : mHostFunctions(*this)
{
    // Object 0 is predefined to always be a null unique_ptr, so we can return
    // a reference to it in contexts where users access invalid objects.
    mObjects.emplace_back(nullptr);
}

}