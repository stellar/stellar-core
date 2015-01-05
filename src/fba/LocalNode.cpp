// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LocalNode.h"

#include "util/types.h"

namespace stellar
{

LocalNode::LocalNode(const uint256& validationSeed)
    : Node(makePublicKey(validationSeed))
    , mValidationSeed(validationSeed)
{
    mCurrentQuorumSet = mQSetUnknown;
}

void 
LocalNode::setCurrentQuorumSet(const FBAQuorumSet& qSet)
{
    mCurrentQuorumSet = qSet;
}

const FBAQuorumSet& 
LocalNode::getCurrentQuorumSet()
{
    return mCurrentQuorumSet;
}

}
