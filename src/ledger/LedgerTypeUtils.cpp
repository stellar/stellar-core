// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTypeUtils.h"
#include "crypto/SHA.h"
#include "util/GlobalChecks.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include <fmt/format.h>

namespace stellar
{

bool
isLive(LedgerEntry const& e, uint32_t cutoffLedger)
{
    releaseAssert(e.data.type() == TTL);
    return e.data.ttl().liveUntilLedgerSeq >= cutoffLedger;
}

LedgerKey
getTTLKey(LedgerEntry const& e)
{
    return getTTLKey(LedgerEntryKey(e));
}

LedgerKey
getTTLKey(LedgerKey const& e)
{
    releaseAssert(e.type() == CONTRACT_CODE || e.type() == CONTRACT_DATA);
    LedgerKey k;
    k.type(TTL);
    k.ttl().keyHash = sha256(xdr::xdr_to_opaque(e));
    return k;
}

LedgerEntryTypeAndDurability
bucketEntryToLedgerEntryAndDurabilityType(BucketEntry const& be)
{
    auto bet = be.type();
    LedgerEntryType let;
    bool isTemp = false;
    if (bet == INITENTRY || bet == LIVEENTRY)
    {
        auto le = be.liveEntry().data;
        let = le.type();
        isTemp = isTemporaryEntry(le);
    }
    else if (bet == DEADENTRY)
    {
        auto lk = be.deadEntry();
        let = lk.type();
        isTemp = isTemporaryEntry(lk);
    }
    else
    {
        auto label = xdr::xdr_traits<BucketEntryType>::enum_name(bet);
        throw std::runtime_error(
            fmt::format("Unsupported BucketEntryType {}", label));
    }
    switch (let)
    {
    case ACCOUNT:
        return LedgerEntryTypeAndDurability::ACCOUNT;
    case TRUSTLINE:
        return LedgerEntryTypeAndDurability::TRUSTLINE;
    case OFFER:
        return LedgerEntryTypeAndDurability::OFFER;
    case DATA:
        return LedgerEntryTypeAndDurability::DATA;
    case CLAIMABLE_BALANCE:
        return LedgerEntryTypeAndDurability::CLAIMABLE_BALANCE;
    case LIQUIDITY_POOL:
        return LedgerEntryTypeAndDurability::LIQUIDITY_POOL;
    case CONTRACT_DATA:
        return isTemp ? LedgerEntryTypeAndDurability::TEMPORARY_CONTRACT_DATA
                      : LedgerEntryTypeAndDurability::PERSISTENT_CONTRACT_DATA;
    case CONTRACT_CODE:
        return LedgerEntryTypeAndDurability::CONTRACT_CODE;
    case CONFIG_SETTING:
        return LedgerEntryTypeAndDurability::CONFIG_SETTING;
    case TTL:
        return LedgerEntryTypeAndDurability::TTL;
    default:
        auto label = xdr::xdr_traits<LedgerEntryType>::enum_name(let);
        throw std::runtime_error(
            fmt::format("Unknown LedgerEntryType {}", label));
    }
}

std::string
toString(LedgerEntryTypeAndDurability const type)
{
    switch (type)
    {
    case LedgerEntryTypeAndDurability::ACCOUNT:
        return "ACCOUNT";
    case LedgerEntryTypeAndDurability::TRUSTLINE:
        return "TRUSTLINE";
    case LedgerEntryTypeAndDurability::OFFER:
        return "OFFER";
    case LedgerEntryTypeAndDurability::DATA:
        return "DATA";
    case LedgerEntryTypeAndDurability::CLAIMABLE_BALANCE:
        return "CLAIMABLE_BALANCE";
    case LedgerEntryTypeAndDurability::LIQUIDITY_POOL:
        return "LIQUIDITY_POOL";
    case LedgerEntryTypeAndDurability::TEMPORARY_CONTRACT_DATA:
        return "TEMPORARY_CONTRACT_DATA";
    case LedgerEntryTypeAndDurability::PERSISTENT_CONTRACT_DATA:
        return "PERSISTENT_CONTRACT_DATA";
    case LedgerEntryTypeAndDurability::CONTRACT_CODE:
        return "CONTRACT_CODE";
    case LedgerEntryTypeAndDurability::CONFIG_SETTING:
        return "CONFIG_SETTING";
    case LedgerEntryTypeAndDurability::TTL:
        return "TTL";
    default:
        throw std::runtime_error(
            fmt::format(FMT_STRING("unknown LedgerEntryTypeAndDurability {:d}"),
                        static_cast<uint32_t>(type)));
    }
}
};