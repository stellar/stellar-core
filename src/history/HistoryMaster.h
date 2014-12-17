#ifndef __HISTORYMASTER__
#define __HISTORYMASTER__

#include "history/HistoryGateway.h"

namespace stellar
{
class Application;
class HistoryMaster : public HistoryGateway
{
    Application &mApp;
public:
    HistoryMaster(Application& app);
};
}

#endif
