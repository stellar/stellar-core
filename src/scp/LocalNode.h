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
    LocalNode(SecretKey const& secretKey, SCPQuorumSet const& qSet, SCP* SCP);

    void updateQuorumSet(SCPQuorumSet const& qSet);

    SCPQuorumSet const& getQuorumSet();
    Hash const& getQuorumSetHash();
    SecretKey const& getSecretKey();

  private:
    const SecretKey mSecretKey;
    SCPQuorumSet mQSet;
    Hash mQSetHash;
};
}
