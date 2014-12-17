#include "fba/Ballot.h"
#include "ledger/Ledger.h"
#include "generated/StellarXDR.h"

/*
    TODO.2: How do we use the excluded ballots in preparing?
*/

namespace stellar
{
namespace ballot
{
bool
isCompatible(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2)
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
compareValue(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2)
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
compare(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2)
{
    if (b1.index > b2.index)
        return true;
    if (b1.index < b2.index)
        return false;
    return compareValue(b1, b2);
}
}
}
