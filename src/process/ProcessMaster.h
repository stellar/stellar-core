#ifndef __PROCESSMASTER__
#define __PROCESSMASTER__

#include "process/ProcessGateway.h"

namespace stellar
{

class ProcessMaster : public ProcessGateway
{
    Application &mApp;

    // These are only used on POSIX, but they're harmless here.
    asio::signal_set mSigChild;
    std::map<int, std::shared_ptr<ProcessExitEvent::Impl>> mImpls;
    void startSignalWait();
    void handleSignalWait();

public:
    ProcessMaster(Application& app);
    ProcessExitEvent runProcess(std::string const& cmdLine);
};
}

#endif
