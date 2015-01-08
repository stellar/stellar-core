#ifndef __CLF_GATEWAY__
#define __CLF_GATEWAY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "clf/CLF.h"

namespace stellar
{
class CLFGateway
{
  public:
    virtual LedgerHeaderPtr getCurrentHeader() = 0;
};
}

#endif
