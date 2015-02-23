#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "generated/StellarXDR.h"

namespace stellar
{

// Helper for getting a LedgerKey from a LedgerEntry.
LedgerKey
LedgerEntryKey(LedgerEntry const& e);

/**
 * Compare two LedgerEntries or LedgerKeys for 'identity', not content.
 *
 * LedgerEntries are identified iff they have:
 *
 *   - The same type
 *     - If accounts, then with same accountID
 *     - If trustlines, then with same (accountID, currency) pair
 *     - If offers, then with same (accountID, sequence) pair
 *
 * Equivalently: Two LedgerEntries have the same 'identity' iff their
 * corresponding LedgerKeys are exactly equal. This operator _could_ be
 * implemented in terms of extracting 2 LedgerKeys from 2 LedgerEntries and
 * doing operator< on them, but that would be comparatively inefficient.
 */
struct
LedgerEntryIdCmp
{
    template <typename T, typename U>
    bool operator()(T const& a,
                    U const& b) const
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
        {
            auto const& atl = a.trustLine();
            auto const& btl = b.trustLine();
            if (atl.accountID < btl.accountID)
                return true;
            if (atl.accountID > btl.accountID)
                return false;
            {
                using xdr::operator<;
                return atl.currency < btl.currency;
            }
        }

        case OFFER:
        {
            auto const& aof = a.offer();
            auto const& bof = b.offer();
            if (aof.accountID < bof.accountID)
                return true;
            if (aof.accountID > bof.accountID)
                return false;
            return aof.sequence < bof.sequence;
        }
        }
        return false;
    }
};

/**
 * Compare two CLFEntries for identity by comparing their respective
 * LedgerEntries (ignoring their hashes, as the LedgerEntryIdCmp ignores their
 * bodies).
 */
struct
CLFEntryIdCmp
{
    LedgerEntryIdCmp mCmp;
    bool operator()(CLFEntry const& a,
                    CLFEntry const& b) const
    {
        CLFType aty = a.type();
        CLFType bty = b.type();

        if (aty == LIVEENTRY)
        {
            if (bty == LIVEENTRY)
            {
                return mCmp(a.liveEntry(), b.liveEntry());
            }
            else
            {
                return mCmp(a.liveEntry(), b.deadEntry());
            }
        }
        else
        {
            if (bty == LIVEENTRY)
            {
                return mCmp(a.deadEntry(), b.liveEntry());
            }
            else
            {
                return mCmp(a.deadEntry(), b.deadEntry());
            }
        }
    }
};

}
