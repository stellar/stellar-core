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

LocalNode::LocalNode(const SecretKey& secretKey, const SCPQuorumSet& qSet,
                     SCP* SCP)
    : Node(secretKey.getPublicKey(), SCP, -1)
    , mSecretKey(secretKey)
    , mQSet(qSet)
    , mQSetHash(sha256(xdr::xdr_to_msg(qSet)))
{
    cacheQuorumSet(qSet);

    CLOG(INFO, "SCP") << "LocalNode::LocalNode"
                      << "@" << binToHex(mNodeID).substr(0, 6)
                      << " qSet: " << binToHex(mQSetHash).substr(0, 6);
}

void
LocalNode::updateQuorumSet(const SCPQuorumSet& qSet)
{
    cacheQuorumSet(qSet);

    mQSetHash = sha256(xdr::xdr_to_msg(qSet));
    mQSet = qSet;
}

const SCPQuorumSet&
LocalNode::getQuorumSet()
{
    return mQSet;
}

const Hash&
LocalNode::getQuorumSetHash()
{
    return mQSetHash;
}

const SecretKey&
LocalNode::getSecretKey()
{
    return mSecretKey;
}
}
