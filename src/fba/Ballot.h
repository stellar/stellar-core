#ifndef __BALLOT__
#define __BALLOT__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "txherder/TransactionSet.h"
#include "fba/Node.h"
#include "generated/StellarXDR.h"
#include "fba/FBA.h"

namespace stellar
{
class Ledger;
typedef std::shared_ptr<Ledger> LedgerPtr;

namespace ballot
{
// returns true if b1 is ranked higher
bool compare(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2);
bool compareValue(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2);
bool isCompatible(const stellarxdr::Ballot& b1, const stellarxdr::Ballot& b2);
}
}

#endif
