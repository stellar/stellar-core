// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/contracts/HostContext.h"
#include "crypto/Hex.h"
#include "ledger/LedgerTxn.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include "xdr/Stellar-types.h"
#include <Tracy.hpp>
#include <TracyC.h>
#include <boost/multiprecision/cpp_int/import_export.hpp>
#include <cstdint>
#include <fizzy/execute.hpp>
#include <fizzy/parser.hpp>
#include <stdexcept>
#include <variant>

namespace stellar
{

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
}

HostContextTxn
HostContext::beginTxn(AbstractLedgerTxn& outerLtx)
{
    releaseAssert(mInvokeCtxs.empty());
    mInvokeCtxs.emplace_back(InvokeContractContext{outerLtx});
    return HostContextTxn(*this);
}

static std::pair<uint32_t, uint32_t>
getStatusPair(SCStatus const& s)
{
    uint32_t code{0};
    uint32_t type = uint32_t(s.type());
    switch (s.type())
    {
    case SST_OK:
        break;
    case SST_UNKNOWN_ERROR:
        code = s.unknownCode();
        break;
    default:
        type = uint32_t(SST_UNKNOWN_ERROR);
        break;
    }
    return std::make_pair(type, code);
}

static SCStatus
getStatus(std::pair<uint32_t, uint32_t> const& s)
{
    SCStatus status;
    status.type(SST_UNKNOWN_ERROR);
    switch (SCStatusType(s.first))
    {
    case SST_OK:
        status.type(SST_OK);
        break;
    case SST_UNKNOWN_ERROR:
    default:
        status.unknownCode() = s.second;
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
        return HostVal::fromStatusTypeAndCode(getStatusPair(v.status()));
    default:
        throw std::runtime_error("unknown SCVal type in xdrToHost");
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
        out.status() = getStatus(hv.asStatusTypeAndCode());
    }
    else
    {
        throw std::runtime_error("unknown HostVal type in hostToXdr");
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
        out.box() = hostToXdr(std::get<HostBox>(*obj).get().mVal);
    }
    else if (std::holds_alternative<HostVec>(*obj))
    {
        out.type(SCO_VEC);
        for (auto const& v : std::get<HostVec>(*obj))
        {
            out.vec().emplace_back(hostToXdr(v.mVal));
        }
    }
    else if (std::holds_alternative<HostMap>(*obj))
    {
        out.type(SCO_MAP);
        for (auto const& pair : std::get<HostMap>(*obj))
        {
            // We should only ever have HostValInContext values
            // from the same HostContext as this.
            releaseAssert(this == pair.first.mCtx);
            out.map().emplace_back(hostToXdr(pair.first.mVal),
                                   hostToXdr(pair.second.mVal));
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
    else if (std::holds_alternative<HostBigInt>(*obj))
    {
        out.type(SCO_BIGINT);
        auto bi = std::back_inserter(out.bi().magnitude);
        boost::multiprecision::export_bits(std::get<HostBigInt>(*obj), bi, 8);
        out.bi().positive = std::get<HostBigInt>(*obj).sign() >= 0;
    }
    else if (std::holds_alternative<HostBigRat>(*obj))
    {
        out.type(SCO_BIGRAT);
        HostBigRat const& rat = std::get<HostBigRat>(*obj);
        HostBigInt num = boost::multiprecision::numerator(rat);
        HostBigInt den = boost::multiprecision::numerator(rat);
        auto nu = std::back_inserter(out.br().numerator);
        auto de = std::back_inserter(out.br().denominator);
        boost::multiprecision::export_bits(num, nu, 8);
        boost::multiprecision::export_bits(den, de, 8);
        out.bi().positive = rat.sign() >= 0;
    }
    else if (std::holds_alternative<xdr::pointer<LedgerKey>>(*obj))
    {
        out.type(SCO_LEDGERKEY);
        auto const& ptr = std::get<xdr::pointer<LedgerKey>>(*obj);
        if (ptr)
        {
            out.lkey().activate() = *ptr;
        }
    }
    else if (std::holds_alternative<xdr::pointer<Operation>>(*obj))
    {
        out.type(SCO_OPERATION);
        auto const& ptr = std::get<xdr::pointer<Operation>>(*obj);
        if (ptr)
        {
            out.op().activate() = *ptr;
        }
    }
    else if (std::holds_alternative<xdr::pointer<OperationResult>>(*obj))
    {
        out.type(SCO_OPERATION_RESULT);
        auto const& ptr = std::get<xdr::pointer<OperationResult>>(*obj);
        if (ptr)
        {
            out.ores().activate() = *ptr;
        }
    }
    else if (std::holds_alternative<xdr::pointer<Transaction>>(*obj))
    {
        out.type(SCO_TRANSACTION);
        auto const& ptr = std::get<xdr::pointer<Transaction>>(*obj);
        if (ptr)
        {
            out.tx().activate() = *ptr;
        }
    }
    else if (std::holds_alternative<Asset>(*obj))
    {
        out.type(SCO_ASSET);
        out.asset() = std::get<Asset>(*obj);
    }
    else if (std::holds_alternative<Price>(*obj))
    {
        out.type(SCO_PRICE);
        out.price() = std::get<Price>(*obj);
    }
    else if (std::holds_alternative<AccountID>(*obj))
    {
        out.type(SCO_ACCOUNTID);
        out.accountID() = std::get<AccountID>(*obj);
    }
    else
    {
        throw std::runtime_error("unknown HostObject type in hostToXdr");
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
        immObj = std::make_unique<HostObject const>(
            HostBox(HostValInContext{xdrToHost(obj->box()), this}));
        break;
    case SCO_VEC:
    {
        auto immVec = HostVec();
        for (auto const& v : obj->vec())
        {
            immVec = std::move(immVec).push_back(
                HostValInContext{xdrToHost(v), this});
        }
        immObj = std::make_unique<HostObject const>(std::move(immVec));
    }
    break;
    case SCO_MAP:
    {
        auto immMap = HostMap();
        for (auto const& pair : obj->map())
        {
            immMap = std::move(immMap).set(
                HostValInContext{xdrToHost(pair.key), this},
                HostValInContext{xdrToHost(pair.val), this});
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
    case SCO_BIGINT:
    {
        HostBigInt bn;
        boost::multiprecision::import_bits(bn, obj->bi().magnitude.begin(),
                                           obj->bi().magnitude.end(), 8);
        if (!obj->bi().positive)
        {
            bn = -bn;
        }
        immObj = std::make_unique<HostObject const>(bn);
    }
    break;
    case SCO_BIGRAT:
    {
        HostBigInt num, den;
        boost::multiprecision::import_bits(num, obj->br().numerator.begin(),
                                           obj->br().numerator.end(), 8);
        boost::multiprecision::import_bits(den, obj->br().denominator.begin(),
                                           obj->br().denominator.end(), 8);
        HostBigRat br(num, den);
        if (!obj->br().positive)
        {
            br = -br;
        }
        immObj = std::make_unique<HostObject const>(br);
    }
    break;
    case SCO_LEDGERKEY:
        immObj = xdrPointerToHostObjectPointer(obj->lkey());
        break;
    case SCO_OPERATION:
        immObj = xdrPointerToHostObjectPointer(obj->op());
        break;
    case SCO_OPERATION_RESULT:
        immObj = xdrPointerToHostObjectPointer(obj->ores());
        break;
    case SCO_TRANSACTION:
        immObj = xdrPointerToHostObjectPointer(obj->tx());
        break;
    case SCO_ASSET:
        immObj = std::make_unique<HostObject const>(obj->asset());
        break;
    case SCO_PRICE:
        immObj = std::make_unique<HostObject const>(obj->price());
        break;
    case SCO_ACCOUNTID:
        immObj = std::make_unique<HostObject const>(obj->accountID());
        break;
    default:
        throw std::runtime_error("unknown SCObject type in xdrToHost");
    }
    size_t idx = mObjects.size();
    mObjects.emplace_back(std::move(immObj));
    return idx;
}

std::optional<fizzy::ExecutionResult>
HostContext::invokeWasmFunction(std::vector<uint8_t> const& wasmModule,
                                std::string const& function,
                                std::vector<HostVal> const& args) const
{
    ZoneScoped;
    // Parse the contract code.
    std::basic_string_view<uint8_t> codeView(wasmModule.data(),
                                             wasmModule.size());

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
        return fizzy::instantiate(std::move(mod), std::move(importedFunctions));
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
        CLOG_TRACE(Tx, "invoking WASM contract func {}", function);

        size_t i = 0;
        while (typesMatch && (i < argTypes.size()))
        {
            if (argTypes[i] != fizzy::ValType::i64)
            {
                CLOG_TRACE(Tx, "arg type {} mismatch", i);
                typesMatch = false;
                break;
            }
            CLOG_TRACE(Tx, "   arg {}: {}", i, args[i]);
            fizzyArgs.emplace_back(args[i].payload());
            ++i;
        }

        if (func_opt->output_types.size() != 1 ||
            func_opt->output_types[0] != fizzy::ValType::i64)
        {
            CLOG_TRACE(Tx, "return type mismatch");
            typesMatch = false;
        }

        // Fail on type mismatch.
        if (!typesMatch)
        {
            CLOG_TRACE(Tx, "invocation type mismatch");
            return std::nullopt;
        }

        // Execute WASM.
        fizzy::ExecutionContext ctx;
        {
            ZoneNamedN(execZone, "exec WASM", true);
            return func_opt.value().function(*instance, fizzyArgs.data(), ctx);
        }
    }
    else
    {
        CLOG_TRACE(Tx, "function '{}' not found", function);
        return std::nullopt;
    }
}

HostContext::HostContext() : mHostFunctions(*this)
{
    // Object 0 is predefined to always be a null unique_ptr, so we can return
    // a reference to it in contexts where users access invalid objects.
    mObjects.emplace_back(nullptr);
}

}