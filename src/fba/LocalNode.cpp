// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LocalNode.h"

#include "xdrpp/marshal.h"
#include "util/types.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"

namespace stellar
{

LocalNode::LocalNode(const uint256& validationSeed,
                     const FBAQuorumSet& qSet,
                     FBA* FBA)
    : Node(makePublicKey(validationSeed), FBA, -1)
    , mValidationSeed(validationSeed)
    , mQSet(qSet)
    , mQSetHash(sha512_256(xdr::xdr_to_msg(qSet)))
{
    cacheQuorumSet(qSet);

    LOG(INFO) << "LocalNode::LocalNode"
              << "@" << binToHex(mNodeID).substr(0,6)
              << " " << binToHex(mQSetHash).substr(0,6);
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
