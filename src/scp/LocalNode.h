#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
    LocalNode(const SecretKey& secretKey,
              const SCPQuorumSet& qSet,
              SCP* SCP);

    void updateQuorumSet(const SCPQuorumSet& qSet);

    const SCPQuorumSet& getQuorumSet();
    const Hash& getQuorumSetHash();
    const SecretKey& getSecretKey();

  private:
    const SecretKey                 mSecretKey;
    SCPQuorumSet                    mQSet;
    Hash                            mQSetHash;
};
}

