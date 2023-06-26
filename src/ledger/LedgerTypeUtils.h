#pragma once

// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "util/XDROperators.h"

namespace stellar
{

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

uint32_t getExpirationLedger(LedgerEntry const& e);
void setExpirationLedger(LedgerEntry& e, uint32_t expiration);

ContractEntryBodyType getLeType(LedgerEntry::_data_t const& e);
ContractEntryBodyType getLeType(LedgerKey const& k);

void setLeType(LedgerEntry& e, ContractEntryBodyType leType);
void setLeType(LedgerKey& k, ContractEntryBodyType leType);

LedgerEntry expirationExtensionFromDataEntry(LedgerEntry const& le);
#endif

bool isLive(LedgerEntry const& e, uint32_t expirationCutoff);
bool autoBumpEnabled(LedgerEntry const& e);

template <typename T> bool isSorobanExtEntry(T const& e);
template <typename T> bool isSorobanDataEntry(T const& e);
template <typename T> bool isRestorableEntry(T const& e);

template <typename T>
bool
isSorobanEntry(T const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return e.type() == CONTRACT_DATA || e.type() == CONTRACT_CODE;
#endif

    return false;
}

template <typename T>
bool
isTemporaryEntry(T const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return e.type() == CONTRACT_DATA &&
           e.contractData().durability == ContractDataDurability::TEMPORARY;
#endif
    return false;
}

}