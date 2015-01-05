// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "FBA.h"

#include "fba/LocalNode.h"

namespace stellar
{

FBA::FBA(const uint256& validationSeed,
         bool validating, 
         const FBAQuorumSet& qSetLocal,
         std::shared_ptr<Client> client)
    : mValidating(validating)
    , mClient(client)
{
  mLocalNode = new LocalNode(validationSeed);
}

}
