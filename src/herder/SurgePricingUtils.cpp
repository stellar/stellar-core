// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "util/numeric.h"

namespace stellar
{

using namespace std;

int
feeRate3WayCompare(int64 lFeeBid, uint32 lNbOps, int64 rFeeBid, uint32 rNbOps)
{
    // compare fee/numOps between l and r
    // getNumOperations >= 1 because SurgeCompare can only be used on
    // valid transactions
    //
    // Let f1, f2 be the two fee bids, and let n1, n2 be the two
    // operation counts. We want to calculate the boolean comparison
    // "f1 / n1 < f2 / n2" but, since these are uint128s, we want to
    // avoid the truncating division or use of floating point.
    //
    // Therefore we multiply both sides by n1 * n2, and cancel:
    //
    //               f1 / n1 < f2 / n2
    //  == f1 * n1 * n2 / n1 < f2 * n1 * n2 / n2
    //  == f1 *      n2      < f2 * n1

    auto v1 = bigMultiply(lFeeBid, rNbOps);
    auto v2 = bigMultiply(rFeeBid, lNbOps);
    if (v1 < v2)
    {
        return -1;
    }
    else if (v1 > v2)
    {
        return 1;
    }
    return 0;
}

int
feeRate3WayCompare(TransactionFrameBasePtr const& l,
                   TransactionFrameBasePtr const& r)
{
    return feeRate3WayCompare(l->getFeeBid(), l->getNumOperations(),
                              r->getFeeBid(), r->getNumOperations());
}
} // namespace stellar
