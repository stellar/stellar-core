#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "util/XDROperators.h"

namespace stellar
{

template <typename T>
bool
lexCompare(T&& lhs1, T&& rhs1)
{
    return lhs1 < rhs1;
}

template <typename T, typename... U>
bool
lexCompare(T&& lhs1, T&& rhs1, U&&... args)
{
    if (lhs1 < rhs1)
    {
        return true;
    }
    else if (rhs1 < lhs1)
    {
        return false;
    }
    return lexCompare(std::forward<U>(args)...);
}

/**
 * Compare two LedgerEntries or LedgerKeys for 'identity', not content.
 *
 * LedgerEntries are identified iff they have:
 *
 *   - The same type
 *     - If accounts, then with same accountID
 *     - If trustlines, then with same (accountID, asset) pair
 *     - If offers, then with same (sellerID, sequence) pair
 *
 * Equivalently: Two LedgerEntries have the same 'identity' iff their
 * corresponding LedgerKeys are exactly equal. This operator _could_ be
 * implemented in terms of extracting 2 LedgerKeys from 2 LedgerEntries and
 * doing operator< on them, but that would be comparatively inefficient.
 *
 * Entries are first ordered by LedgerEntryType, then by key/key pair for the
 * given type.
 */
struct LedgerEntryCmpByType
{
    template <typename T, typename U>
    auto
    operator()(T const& a, U const& b) const
        -> decltype(a.type(), b.type(), bool())
    {
        LedgerEntryType aty = a.type();
        LedgerEntryType bty = b.type();

        if (aty < bty)
            return true;

        if (aty > bty)
            return false;

        switch (aty)
        {
        case ACCOUNT:
            return a.account().accountID < b.account().accountID;
        case TRUSTLINE:
            return lexCompare(a.trustLine().accountID, b.trustLine().accountID,
                              a.trustLine().asset, b.trustLine().asset);
        case OFFER:
            return lexCompare(a.offer().sellerID, b.offer().sellerID,
                              a.offer().offerID, b.offer().offerID);
        case DATA:
            return lexCompare(a.data().accountID, b.data().accountID,
                              a.data().dataName, b.data().dataName);
        case CLAIMABLE_BALANCE:
            return a.claimableBalance().balanceID <
                   b.claimableBalance().balanceID;
        case LIQUIDITY_POOL:
            return a.liquidityPool().liquidityPoolID <
                   b.liquidityPool().liquidityPoolID;
        }
        return false;
    }
};

/**
 * Compare two LedgerEntries or LedgerKeys for 'identity', not content.
 *
 * LedgerEntries are identified iff they have:
 *
 *   - The same type
 *     - If accounts, then with same accountID
 *     - If trustlines, then with same (accountID, asset) pair
 *     - If offers, then with same (sellerID, sequence) pair
 *
 * Equivalently: Two LedgerEntries have the same 'identity' iff their
 * corresponding LedgerKeys are exactly equal. This operator _could_ be
 * implemented in terms of extracting 2 LedgerKeys from 2 LedgerEntries and
 * doing operator< on them, but that would be comparatively inefficient.
 *
 * Entries that are accounts or account subentries are ordered first, followed
 * by non-account related entries. Account subentries that have
 * the same accountID are grouped together. If two entries have the same
 * accountID, they are then sorted by LedgerEntryType and then by the associated
 * key/key pair for the given type.
 */
struct LedgerEntryCmpByAccount
{
    template <typename T, typename U>
    auto
    operator()(T const& a, U const& b) const
        -> decltype(a.type(), b.type(), bool())
    {
        LedgerEntryType aty = a.type();
        LedgerEntryType bty = b.type();

        // If at least one account is not associated with an account or
        // account subentry
        if (aty >= CLAIMABLE_BALANCE || bty >= CLAIMABLE_BALANCE)
        {
            // Order by type if types differ
            if (aty < bty)
                return true;

            if (aty > bty)
                return false;

            // Order by ID if types are the same
            switch (aty)
            {
            case CLAIMABLE_BALANCE:
                return a.claimableBalance().balanceID <
                       b.claimableBalance().balanceID;
            case LIQUIDITY_POOL:
                return a.liquidityPool().liquidityPoolID <
                       b.liquidityPool().liquidityPoolID;
            default:
                throw std::runtime_error("Unexpected entry type.");
            }
            return false;
        }

        // Else if both entries are associated with an account or account
        // subentry
        auto getAccountID = [](auto const& e, LedgerEntryType const& et) {
            switch (et)
            {
            case ACCOUNT:
                return e.account().accountID;
            case TRUSTLINE:
                return e.trustLine().accountID;
            case OFFER:
                return e.offer().sellerID;
            case DATA:
                return e.data().accountID;
            default:
                throw std::runtime_error("Unexpected entry type.");
            }
        };

        // Order by AccountID if they are different
        auto aid = getAccountID(a, aty);
        auto bid = getAccountID(b, bty);
        if (aid < bid)
            return true;

        if (bid < aid)
            return false;

        // Else if order by type
        if (aty < bty)
            return true;

        if (bty < aty)
            return false;

        // Else if both entries are assocaited with the same accountID and
        // are the same type
        switch (aty)
        {
        case ACCOUNT:
            // Entries are identical
            return false;
        case TRUSTLINE:
            return lexCompare(aid, bid, a.trustLine().asset,
                              b.trustLine().asset);
        case OFFER:
            return lexCompare(aid, bid, a.offer().offerID, b.offer().offerID);
        case DATA:
            return lexCompare(aid, bid, a.data().dataName, b.data().dataName);
        default:
            throw std::runtime_error("Unexpected entry type.");
        }
        return false;
    }
};

namespace
{

// Compares BucketEntry's using given LedgerCmp function.
template <class LedgerCmp>
bool
bucketEntryIdCmpInternal(BucketEntry const& a, BucketEntry const& b)
{
    BucketEntryType aty = a.type();
    BucketEntryType bty = b.type();

    // METAENTRY sorts below all other entries, comes first in buckets.
    if (aty == METAENTRY || bty == METAENTRY)
    {
        return aty < bty;
    }

    if (aty == LIVEENTRY || aty == INITENTRY)
    {
        if (bty == LIVEENTRY || bty == INITENTRY)
        {
            return LedgerCmp{}(a.liveEntry().data, b.liveEntry().data);
        }
        else
        {
            if (bty != DEADENTRY)
            {
                throw std::runtime_error("Malformed bucket: unexpected "
                                         "non-INIT/LIVE/DEAD entry.");
            }
            return LedgerCmp{}(a.liveEntry().data, b.deadEntry());
        }
    }
    else
    {
        if (aty != DEADENTRY)
        {
            throw std::runtime_error(
                "Malformed bucket: unexpected non-INIT/LIVE/DEAD entry.");
        }
        if (bty == LIVEENTRY || bty == INITENTRY)
        {
            return LedgerCmp{}(a.deadEntry(), b.liveEntry().data);
        }
        else
        {
            if (bty != DEADENTRY)
            {
                throw std::runtime_error("Malformed bucket: unexpected "
                                         "non-INIT/LIVE/DEAD entry.");
            }
            return LedgerCmp{}(a.deadEntry(), b.deadEntry());
        }
    }
}
}

typedef std::function<bool(BucketEntry const&, BucketEntry const&)>
    BucketEntryIdCmpProto;

enum class BucketSortOrder
{
    SortByType,
    SortByAccount
};

/**
 * Compare two BucketEntries for identity by comparing their respective
 * LedgerEntries (ignoring their hashes, as the LedgerEntryCmpByType ignores
 * their bodies). Template parameter determines if entries are sorted by type or
 * by account.
 */
template <BucketSortOrder type>
bool
BucketEntryIdCmp(BucketEntry const& a, BucketEntry const& b)
{
    if constexpr (type == BucketSortOrder::SortByType)
    {
        return bucketEntryIdCmpInternal<LedgerEntryCmpByType>(a, b);
    }
    else
    {
        return bucketEntryIdCmpInternal<LedgerEntryCmpByAccount>(a, b);
    }
}
}
