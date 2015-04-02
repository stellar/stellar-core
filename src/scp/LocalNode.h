#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <vector>

#include "scp/SCP.h"
#include "scp/Node.h"

namespace stellar
{
/**
 * This is one Node in the stellar network
 */
class LocalNode : public Node
{
  public:
    LocalNode(const SecretKey& secretKey, const SCPQuorumSet& qSet, SCP* SCP);

    void updateQuorumSet(const SCPQuorumSet& qSet);

    const SCPQuorumSet& getQuorumSet();
    const Hash& getQuorumSetHash();
    const SecretKey& getSecretKey();

  private:
    const SecretKey mSecretKey;
    SCPQuorumSet mQSet;
    Hash mQSetHash;
};
}
