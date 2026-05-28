// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LedgerCmp.h"

namespace
{
template <typename T>
std::partial_ordering
lexComparePartial(T&& lhs1, T&& rhs1)
{
    return lhs1 <=> rhs1;
}

template <typename T, typename... U>
std::partial_ordering
lexComparePartial(T&& lhs1, T&& rhs1, U&&... args)
{
    if (auto cmp = lhs1 <=> rhs1; cmp != std::partial_ordering::equivalent)
    {
        return cmp;
    }
    return lexComparePartial(std::forward<U>(args)...);
}
} // namespace

namespace stellar
{
std::partial_ordering
compareLedgerKeys(LedgerKey const& a, LedgerKey const& b)
{
    LedgerEntryType aty = a.type();
    LedgerEntryType bty = b.type();

    if (aty < bty)
        return std::partial_ordering::less;

    if (aty > bty)
        return std::partial_ordering::greater;

    switch (aty)
    {
    case ACCOUNT:
        return a.account().accountID <=> b.account().accountID;
    case TRUSTLINE:
        return lexComparePartial(a.trustLine().accountID,
                                 b.trustLine().accountID, a.trustLine().asset,
                                 b.trustLine().asset);
    case OFFER:
        return lexComparePartial(a.offer().sellerID, b.offer().sellerID,
                                 a.offer().offerID, b.offer().offerID);
    case DATA:
        return lexComparePartial(a.data().accountID, b.data().accountID,
                                 a.data().dataName, b.data().dataName);
    case CLAIMABLE_BALANCE:
        return a.claimableBalance().balanceID <=>
               b.claimableBalance().balanceID;
    case LIQUIDITY_POOL:
        return a.liquidityPool().liquidityPoolID <=>
               b.liquidityPool().liquidityPoolID;
    case CONTRACT_DATA:
    {
        return lexComparePartial(
            a.contractData().contract, b.contractData().contract,
            a.contractData().key, b.contractData().key,
            a.contractData().durability, b.contractData().durability);
    }
    case CONTRACT_CODE:
        return lexComparePartial(a.contractCode().hash, b.contractCode().hash);
    case CONFIG_SETTING:
    {
        return a.configSetting().configSettingID <=>
               b.configSetting().configSettingID;
    }
    case TTL:
        return lexComparePartial(a.ttl().keyHash, b.ttl().keyHash);
    }

    return std::partial_ordering::unordered;
}
} // namespace stellar
