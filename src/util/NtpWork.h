#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/NtpClient.h"
#include "util/Timer.h"
#include "work/Work.h"

#include <chrono>

namespace stellar
{

/**
 * Work item for checking NTP time synchronization.
 *
 * This work is configured to be retry forever - in case of failure its
 * restarted
 * by work manager. In case of success value of check is obtainable by
 * isSynchronized
 * getter. Acceptable difference in seconds is passed as a constructor paremeter
 * tolerance.
 *
 * This class takes care of timeouts of NtpClient.
 */
class NtpWork : public Work
{
  public:
    /**
     * Costructor accepts address of ntp server to use in form of domain address
     * or string
     * representation of ip address.
     * Acceptable tolerance is passed as number of seconds. If value of
     * tolerance is less than zero
     * then behavior of this constructor is undefined.
     */
    explicit NtpWork(Application& app, WorkParent& parent,
                     std::string ntpServer, std::chrono::seconds tolerance);
    virtual ~NtpWork();

    /**
     * If work is successfull, returns if difference between local time and NTP
     * time is
     * within acceptable tolerance (passed in constructor).
     * If work is unfinished or unsuccessfull, true is returned.
     */
    bool isSynchronized() const;

  private:
    std::string mNtpServer;
    std::chrono::seconds mTolerance;
    VirtualTimer mTimeoutTimer;
    bool mIsSynchronized;
    std::shared_ptr<NtpClient> mNtpClient;

    virtual void onRun() override;

    void onNtpClientSuccess(long time);
    void onNtpClientFailure();
};
}
