#ifndef __BALLOT__
#define __BALLOT__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "txherder/TxSetFrame.h"
#include "fba/Node.h"
#include "generated/StellarXDR.h"
#include "fba/FBA.h"

namespace stellar
{

namespace ballot
{
// returns true if b1 is ranked higher
bool compare(const Ballot& b1, const Ballot& b2);
bool compareValue(const Ballot& b1, const Ballot& b2);
bool isCompatible(const Ballot& b1, const Ballot& b2);
}
}

#endif
