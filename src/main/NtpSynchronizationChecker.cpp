// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "NtpSynchronizationChecker.h"

#include "herder/Herder.h"
#include "main/Application.h"
#include "util/NtpWork.h"
#include "util/StatusManager.h"

#include <iostream>

namespace stellar
{

std::chrono::hours const NtpSynchronizationChecker::CHECK_FREQUENCY(24);

NtpSynchronizationChecker::NtpSynchronizationChecker(Application& app,
                                                     std::string ntpServer)
    : WorkParent(app)
    , mCheckTimer(app)
    , mNtpServer(std::move(ntpServer))
    , mIsShutdown(false)
{
}

NtpSynchronizationChecker::~NtpSynchronizationChecker()
{
    clearChildren();
}

void
NtpSynchronizationChecker::notify(const std::string&)
{
    if (allChildrenSuccessful())
    {
        clearChildren();
        updateStatusManager();
        mNtpWork.reset();
        scheduleNextCheck();
    }
}

void
NtpSynchronizationChecker::updateStatusManager()
{
    auto& statusManager = app().getStatusManager();
    if (mNtpWork->isSynchronized())
    {
        statusManager.removeStatusMessage(StatusCategory::NTP);
    }
    else
    {
        statusManager.setStatusMessage(
            StatusCategory::NTP,
            "Local time is not synchronized with NTP time.");
    }
}

void
NtpSynchronizationChecker::scheduleNextCheck()
{
    std::weak_ptr<NtpSynchronizationChecker> weak =
        std::static_pointer_cast<NtpSynchronizationChecker>(shared_from_this());
    mCheckTimer.expires_from_now(CHECK_FREQUENCY);
    mCheckTimer.async_wait(
        [weak]() {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->start();
        },
        &VirtualTimer::onFailureNoop);
}

void
NtpSynchronizationChecker::start()
{
    if (mIsShutdown)
    {
        return;
    }

    mNtpWork = addWork<NtpWork>(mNtpServer, Herder::MAX_TIME_SLIP_SECONDS / 2);
    advanceChildren();
}

void
NtpSynchronizationChecker::shutdown()
{
    if (mIsShutdown)
    {
        return;
    }

    mIsShutdown = true;
    clearChildren();
    mNtpWork.reset();
}
}
