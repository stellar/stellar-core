// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

#include "transactions/contracts/ContractUtils.h"
#include "crypto/Hex.h"
#include "rust/RustBridge.h"
#include "rust/RustVecXdrMarshal.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "util/types.h"
#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-transaction.h"

#include <Tracy.hpp>

namespace stellar
{

template <typename T>
XDRBuf
toXDRBuf(T const& t)
{
    return XDRBuf{
        std::make_unique<std::vector<uint8_t>>(xdr::xdr_to_opaque(t))};
}

// Invoke a contract with a given function name and args vector, where
// ledgerEntries is a vector of buffers each of which contains a ledger entry in
// the footprint of the contract. The keys in footprint and ledgerEntries
// must be the same, and ledgerEntries must contain an entry
//
// {
//    contract_id,
//    key: ScVal::Static(ScStatic::LedgerKeyContractCodeWasm)
//    val: ScVal::Object(Some(ScObject::Binary(blob)))
// }
//
// which contains the WASM blob that will be run.
SCVal
invokeContract(Hash const& contract_id, std::string const& funcName,
               SCVec const& args, LedgerFootprint const& footprint,
               std::vector<std::unique_ptr<std::vector<uint8_t>>> ledgerEntries)
{
    ZoneScoped;
    rust::Vec<XDRBuf> xdrBufs;
    xdrBufs.reserve(ledgerEntries.size());
    for (auto& p : ledgerEntries)
    {
        xdrBufs.push_back(XDRBuf{std::move(p)});
    }
    rust::Vec<uint8_t> retBuf = rust_bridge::invoke_contract(
        toXDRBuf(contract_id), funcName, toXDRBuf(args), toXDRBuf(footprint),
        xdrBufs);
    SCVal ret;
    xdr::xdr_from_opaque(retBuf, ret);
    return ret;
}

}

#endif
