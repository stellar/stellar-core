#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include <vector>

#include "fba/FBA.h"
#include "fba/Node.h"

namespace stellar
{
/**
 * This is one Node in the stellar network
 */
class LocalNode : public Node
{
  public:
    LocalNode(const uint256& validationSeed,
              const FBAQuorumSet& qSet,
              FBA* FBA);

    void updateQuorumSet(const FBAQuorumSet& qSet);

    const FBAQuorumSet& getQuorumSet();
    const Hash& getQuorumSetHash();

  private:
    const uint256                   mValidationSeed;
    FBAQuorumSet                    mQSet;
    Hash                            mQSetHash;
};
}


