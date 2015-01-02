#ifndef __NODE__
#define __NODE__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include <vector>

#include "fba/FBA.h"

namespace stellar
{
/**
 * This is one Node in the stellar network
 */
class LocalNode : public Node
{
  public:
    LocalNode(const uint256& nodeID);

    void setCurrentQuorumSet(const FBAQuorumSet& qset);
    const FBAQuorumSet& getCurrentQuorumSet();

  private:
    FBAQuorumSet&                   mCurrentQuorumSet;
};
}

#endif
