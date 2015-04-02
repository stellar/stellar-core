// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "LocalNode.h"

#include "xdrpp/marshal.h"
#include "util/types.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"

namespace stellar
{

LocalNode::LocalNode(SecretKey const& secretKey, SCPQuorumSet const& qSet,
                     SCP* SCP)
    : Node(secretKey.getPublicKey(), SCP, -1)
    , mSecretKey(secretKey)
    , mQSet(qSet)
    , mQSetHash(sha256(xdr::xdr_to_opaque(qSet)))
{
    cacheQuorumSet(qSet);

    CLOG(INFO, "SCP") << "LocalNode::LocalNode"
                      << "@" << hexAbbrev(mNodeID)
                      << " qSet: " << hexAbbrev(mQSetHash);
}

void
LocalNode::updateQuorumSet(SCPQuorumSet const& qSet)
{
    cacheQuorumSet(qSet);

    mQSetHash = sha256(xdr::xdr_to_opaque(qSet));
    mQSet = qSet;
}

SCPQuorumSet const&
LocalNode::getQuorumSet()
{
    return mQSet;
}

Hash const&
LocalNode::getQuorumSetHash()
{
    return mQSetHash;
}

SecretKey const&
LocalNode::getSecretKey()
{
    return mSecretKey;
}
}
