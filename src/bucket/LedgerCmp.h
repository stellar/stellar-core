#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include "overlay/StellarXDR.h"

namespace stellar
{
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
 */
struct LedgerEntryIdCmp
{
    template <typename T, typename U>
    auto
    operator()(T const& a, U const& b) const
        -> decltype(a.type(), b.type(), bool())
    {
        using xdr::operator<;

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
        {
            auto const& atl = a.trustLine();
            auto const& btl = b.trustLine();
            if (atl.accountID < btl.accountID)
                return true;
            if (btl.accountID < atl.accountID)
                return false;
            {
                return atl.asset < btl.asset;
            }
        }

        case OFFER:
        {
            auto const& aof = a.offer();
            auto const& bof = b.offer();
            if (aof.sellerID < bof.sellerID)
                return true;
            if (bof.sellerID < aof.sellerID)
                return false;
            return aof.offerID < bof.offerID;
        }
        case DATA:
        {
            auto const& ad = a.data();
            auto const& bd = b.data();
            if (ad.accountID < bd.accountID)
                return true;
            if (bd.accountID < ad.accountID)
                return false;
            {
                return ad.dataName < bd.dataName;
            }
        }
        }
        return false;
    }
};

/**
 * Compare two BucketEntries for identity by comparing their respective
 * LedgerEntries (ignoring their hashes, as the LedgerEntryIdCmp ignores their
 * bodies).
 */
struct BucketEntryIdCmp
{
    bool
    operator()(BucketEntry const& a, BucketEntry const& b) const
    {
        BucketEntryType aty = a.type();
        BucketEntryType bty = b.type();

        if (aty == LIVEENTRY)
        {
            if (bty == LIVEENTRY)
            {
                return LedgerEntryIdCmp{}(a.liveEntry().data,
                                          b.liveEntry().data);
            }
            else
            {
                return LedgerEntryIdCmp{}(a.liveEntry().data, b.deadEntry());
            }
        }
        else
        {
            if (bty == LIVEENTRY)
            {
                return LedgerEntryIdCmp{}(a.deadEntry(), b.liveEntry().data);
            }
            else
            {
                return LedgerEntryIdCmp{}(a.deadEntry(), b.deadEntry());
            }
        }
    }
};
}
