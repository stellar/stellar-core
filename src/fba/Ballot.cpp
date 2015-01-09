// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/Ballot.h"
#include "generated/StellarXDR.h"

/*
    TODO.2: How do we use the excluded ballots in preparing?
*/

namespace stellar
{
namespace ballot
{
bool
isCompatible(const Ballot& b1, const Ballot& b2)
{
    if (b1.closeTime != b2.closeTime)
        return false;
    if (b1.baseFee != b2.baseFee)
        return false;
    if (b1.txSetHash != b2.txSetHash)
        return false;

    return true;
}

// favor longer close times and greater fees !
bool
compareValue(const Ballot& b1, const Ballot& b2)
{
    if (b1.txSetHash != b2.txSetHash)
        return (b1.txSetHash > b2.txSetHash);
    if (b1.closeTime != b2.closeTime)
        return (b1.closeTime > b2.closeTime);
    if (b1.baseFee > b2.baseFee)
        return true;

    return false;
}

// returns true if b1 is higher
// returns false if b1 is lower or they are the same
bool
compare(const Ballot& b1, const Ballot& b2)
{
    if (b1.index > b2.index)
        return true;
    if (b1.index < b2.index)
        return false;
    return compareValue(b1, b2);
}
}
}
