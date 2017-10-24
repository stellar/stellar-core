// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "NtpWork.h"

#include "main/Application.h"
#include "util/make_unique.h"

#include <iostream>

namespace stellar
{

NtpWork::NtpWork(Application& app, WorkParent& parent, std::string ntpServer,
                 std::chrono::seconds tolerance)
    : Work(app, parent, "ntp-work", RETRY_FOREVER)
    , mNtpServer(std::move(ntpServer))
    , mTolerance(tolerance)
    , mTimeoutTimer(app)
    , mIsSynchronized(true)
{
    assert(mTolerance >= std::chrono::seconds::zero());
}

NtpWork::~NtpWork()
{
    clearChildren();
}

void
NtpWork::onRun()
{
    std::weak_ptr<NtpWork> weak =
        std::static_pointer_cast<NtpWork>(shared_from_this());
    mNtpClient =
        std::make_shared<NtpClient>(app().getWorkerIOService(), mNtpServer,
                                    [weak](long time) {
                                        auto self = weak.lock();
                                        if (self)
                                        {
                                            self->onNtpClientSuccess(time);
                                        }
                                    },
                                    [weak]() {
                                        auto self = weak.lock();
                                        if (self)
                                        {
                                            self->onNtpClientFailure();
                                        }
                                    });
    mNtpClient->getTime();

    mTimeoutTimer.expires_from_now(std::chrono::seconds(5));
    mTimeoutTimer.async_wait(
        [weak]() {
            auto self = weak.lock();
            if (self)
            {
                self->mNtpClient.reset(); // reset will call destructor which
                                          // will call failure callback if
                                          // needed
            }
        },
        &VirtualTimer::onFailureNoop);
}

void
NtpWork::onNtpClientSuccess(long int time)
{
    auto localNow = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    auto ntpNow = std::chrono::seconds(time);
    auto diff = localNow - ntpNow;
    diff = diff > std::chrono::seconds::zero() ? diff : -diff;

    mIsSynchronized = diff <= mTolerance;
    scheduleSuccess();
}

void
NtpWork::onNtpClientFailure()
{
    scheduleFailure();
}

bool
NtpWork::isSynchronized() const
{
    return mIsSynchronized;
}
}
