#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "work/WorkParent.h"

namespace stellar
{

class Application;
class NtpWork;

/**
 * Class that periodically check if local time is synchronized with NTP server.
 * NTP server is passed in construtor as a domain name or string representation
 * of ip address.
 *
 * Its adds status to StatusManager in case time is not synchronized.
 */
class NtpSynchronizationChecker : public WorkParent
{
  public:
    // Time between checks.
    static std::chrono::hours const CHECK_FREQUENCY;

    explicit NtpSynchronizationChecker(Application& app, std::string ntpServer);
    ~NtpSynchronizationChecker();

    /*
     * Start checking for NTP time synchronization.
     */
    void start();
    /*
     * Stop checking for NTP time synchronization.
     * Must be called when io_service of Application is still active.
     */
    void shutdown();

  protected:
    VirtualTimer mCheckTimer;
    std::string mNtpServer;
    std::shared_ptr<NtpWork> mNtpWork;
    bool mIsShutdown;

    virtual void notify(const std::string& childChanged) override;

    void updateStatusManager();
    void scheduleNextCheck();
};
}
