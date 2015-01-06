// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LocalNode.h"

#include "xdrpp/marshal.h"
#include "util/types.h"
#include "crypto/SHA.h"

namespace stellar
{

LocalNode::LocalNode(const uint256& validationSeed,
                     const FBAQuorumSet& qSet)
    : Node(makePublicKey(validationSeed), -1)
    , mValidationSeed(validationSeed)
    , mQSet(qSet)
    , mQSetHash(sha512_256(xdr::xdr_to_msg(qSet)))
{
}

void 
LocalNode::updateQuorumSet(const FBAQuorumSet& qSet)
{
    cacheQuorumSet(qSet);

    mQSetHash = sha512_256(xdr::xdr_to_msg(qSet));
    mQSet = qSet;
}

const FBAQuorumSet& 
LocalNode::getQuorumSet()
{
    return mQSet;
}

const uint256& 
LocalNode::getQuorumSetHash()
{
  return mQSetHash;
}

}
