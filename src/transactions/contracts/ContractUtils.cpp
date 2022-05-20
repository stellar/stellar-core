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

#include <Tracy.hpp>

namespace stellar
{

SCVal
invokeContract(std::vector<uint8_t> const& wasmCode,
               std::string const& funcName, SCVec const& args)
{
    ZoneScoped;
    auto argBuf = xdr::xdr_to_opaque(args);
    rust::Vec<uint8_t> retBuf =
        rust_bridge::invoke_contract(wasmCode, funcName, argBuf);
    SCVal ret;
    xdr::xdr_from_opaque(retBuf, ret);
    return ret;
}

}

#endif
