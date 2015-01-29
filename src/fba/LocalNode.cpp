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

LocalNode::LocalNode(const SecretKey& secretKey,
                     const FBAQuorumSet& qSet,
                     FBA* FBA)
    : Node(secretKey.getPublicKey(), FBA, -1)
    , mSecretKey(secretKey)
    , mQSet(qSet)
    , mQSetHash(sha512_256(xdr::xdr_to_msg(qSet)))
{
    cacheQuorumSet(qSet);

    CLOG(INFO, "FBA") << "LocalNode::LocalNode"
        << "@" << binToHex(mNodeID).substr(0,6)
        << " qSet: " << binToHex(mQSetHash).substr(0,6);
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

const Hash& 
LocalNode::getQuorumSetHash()
{
  return mQSetHash;
}

}
