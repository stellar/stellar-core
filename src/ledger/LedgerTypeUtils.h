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

ContractLedgerEntryType getLeType(LedgerEntry::_data_t const& e);
ContractLedgerEntryType getLeType(LedgerKey const& k);

void setLeType(LedgerEntry& e, ContractLedgerEntryType leType);
void setLeType(LedgerKey& k, ContractLedgerEntryType leType);

LedgerEntry expirationExtensionFromDataEntry(LedgerEntry const& le);
#endif

bool autoBumpEnabled(LedgerEntry const& e);

template <typename T> bool isSorobanExtEntry(T const& e);
template <typename T> bool isSorobanDataEntry(T const& e);

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
           e.contractData().type == ContractDataType::TEMPORARY;
#endif
    return false;
}

}