#ifndef __FBA_LOCAL_NODE__
#define __FBA_LOCAL_NODE__

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
    LocalNode(const uint256& validationSeed);

    void setCurrentQuorumSet(const FBAQuorumSet& qSet);
    const FBAQuorumSet& getCurrentQuorumSet();

  private:
    const uint256&                  mValidationSeed;
    FBAQuorumSet                    mCurrentQuorumSet;
};
}

#endif
