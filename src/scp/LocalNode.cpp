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

LocalNode::LocalNode(const SecretKey& secretKey, const SCPQuorumSet& qSet,
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
LocalNode::updateQuorumSet(const SCPQuorumSet& qSet)
{
    cacheQuorumSet(qSet);

    mQSetHash = sha256(xdr::xdr_to_opaque(qSet));
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
