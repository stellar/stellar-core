#ifndef __HISTORYMASTER_H__
#define __HISTORYMASTER_H__

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
