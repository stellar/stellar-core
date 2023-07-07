// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"

namespace stellar
{

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
uint32_t
getExpirationLedger(LedgerEntry const& e)
{
    releaseAssert(isSorobanEntry(e.data));
    if (e.data.type() == CONTRACT_DATA)
    {
        return e.data.contractData().expirationLedgerSeq;
    }

    return e.data.contractCode().expirationLedgerSeq;
}

void
setExpirationLedger(LedgerEntry& e, uint32_t expiration)
{
    releaseAssert(isSorobanEntry(e.data));
    if (e.data.type() == CONTRACT_DATA)
    {
        e.data.contractData().expirationLedgerSeq = expiration;
    }
    else
    {
        e.data.contractCode().expirationLedgerSeq = expiration;
    }
}

void
setLeType(LedgerEntry& e, ContractEntryBodyType leType)
{
    releaseAssert(isSorobanEntry(e.data));
    if (e.data.type() == CONTRACT_DATA)
    {
        e.data.contractData().body.bodyType(leType);
    }
    else
    {
        e.data.contractCode().body.bodyType(leType);
    }
}

void
setLeType(LedgerKey& k, ContractEntryBodyType leType)
{
    releaseAssert(isSorobanEntry(k));
    if (k.type() == CONTRACT_DATA)
    {
        k.contractData().bodyType = leType;
    }
    else
    {
        k.contractCode().bodyType = leType;
    }
}

ContractEntryBodyType
getLeType(LedgerKey const& k)
{
    releaseAssert(isSorobanEntry(k));
    if (k.type() == CONTRACT_CODE)
    {
        return k.contractCode().bodyType;
    }

    return k.contractData().bodyType;
}

ContractEntryBodyType
getLeType(LedgerEntry::_data_t const& e)
{
    releaseAssert(isSorobanEntry(e));
    if (e.type() == CONTRACT_CODE)
    {
        return e.contractCode().body.bodyType();
    }

    return e.contractData().body.bodyType();
}

LedgerEntry
expirationExtensionFromDataEntry(LedgerEntry const& le)
{
    releaseAssert(isSorobanDataEntry(le.data));
    LedgerEntry extLe;
    if (le.data.type() == CONTRACT_CODE)
    {
        extLe.data.type(CONTRACT_CODE);
        extLe.data.contractCode().expirationLedgerSeq = getExpirationLedger(le);
        extLe.data.contractCode().body.bodyType(EXPIRATION_EXTENSION);
    }
    else
    {
        extLe.data.type(CONTRACT_DATA);
        extLe.data.contractData().expirationLedgerSeq = getExpirationLedger(le);
        extLe.data.contractData().body.bodyType(EXPIRATION_EXTENSION);
    }

    return extLe;
}
#endif

bool
isLive(LedgerEntry const& e, uint32_t expirationCutoff)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return !isSorobanEntry(e.data) ||
           getExpirationLedger(e) >= expirationCutoff;
#endif
    return true;
}

bool
autoBumpEnabled(LedgerEntry const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    releaseAssertOrThrow(isSorobanDataEntry(e.data));

    // CONTRACT_CODE always has autobump enabled. For CONTRACT_DATA, check if
    // the NO_AUTOBUMP flag set
    return e.data.type() == CONTRACT_CODE ||
           !(e.data.contractData().body.data().flags &
             ContractDataFlags::NO_AUTOBUMP);
#endif
    return false;
}

template <typename T>
bool
isSorobanExtEntry(T const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return isSorobanEntry(e) && getLeType(e) == EXPIRATION_EXTENSION;
#endif
    return false;
}

template bool
isSorobanExtEntry<LedgerEntry::_data_t>(LedgerEntry::_data_t const& e);
template bool isSorobanExtEntry<LedgerKey>(LedgerKey const& e);

template <typename T>
bool
isSorobanDataEntry(T const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return isSorobanEntry(e) && getLeType(e) == DATA_ENTRY;
#endif
    return false;
}

template bool
isSorobanDataEntry<LedgerEntry::_data_t>(LedgerEntry::_data_t const& e);
template bool isSorobanDataEntry<LedgerKey>(LedgerKey const& e);

template <typename T>
bool
isRestorableEntry(T const& e)
{
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    return e.type() == CONTRACT_CODE ||
           (e.type() == CONTRACT_DATA &&
            e.contractData().durability == PERSISTENT);
#endif
    return false;
}

template bool
isRestorableEntry<LedgerEntry::_data_t>(LedgerEntry::_data_t const& e);
template bool isRestorableEntry<LedgerKey>(LedgerKey const& e);
};