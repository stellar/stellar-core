#ifndef __HISTORYMASTER__
#define __HISTORYMASTER__
// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "history/HistoryGateway.h"

namespace stellar
{
class Application;
class HistoryMaster : public HistoryGateway
{
    Application& mApp;

  public:
    HistoryMaster(Application& app);
};
}

#endif
