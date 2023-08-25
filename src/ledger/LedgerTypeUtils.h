#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "util/XDROperators.h"

namespace stellar
{
bool isLive(LedgerEntry const& e, uint32_t expirationCutoff);

template <typename T>
bool
isSorobanEntry(T const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return e.type() == CONTRACT_DATA || e.type() == CONTRACT_CODE;
#else
    return false;
#endif
}

template <typename T>
bool
isTemporaryEntry(T const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return e.type() == CONTRACT_DATA &&
           e.contractData().durability == ContractDataDurability::TEMPORARY;
#else
    return false;
#endif
}

template <typename T>
bool
isPersistentEntry(T const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return e.type() == CONTRACT_CODE ||
           (e.type() == CONTRACT_DATA &&
            e.contractData().durability == PERSISTENT);
#else
    return false;
#endif
}
}