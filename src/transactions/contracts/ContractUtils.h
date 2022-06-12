#pragma once

// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

#include "xdr/Stellar-contract.h"
#include "xdr/Stellar-transaction.h"
#include <cstdint>
#include <string>
#include <vector>

namespace stellar
{
SCVal invokeContract(
    Hash const& contract_id, std::string const& funcName, SCVec const& args,
    LedgerFootprint const& footprint,
    std::vector<std::unique_ptr<std::vector<uint8_t>>> ledgerEntries);
}
#endif
