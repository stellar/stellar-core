// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "work/WorkWithCallback.h"
#include "util/Logging.h"

namespace stellar
{

WorkWithCallback::WorkWithCallback(
    Application& app, std::string name,
    std::function<bool(Application& app)> callback)
    : BasicWork(app, std::move(name), BasicWork::RETRY_NEVER)
    , mCallback(callback)
{
    if (!mCallback)
    {
        throw std::invalid_argument("Must provide a valid function. Use "
                                    "BasicWork if a callback is not needed.");
    }
}

BasicWork::State
WorkWithCallback::onRun()
{
    bool res;
    try
    {
        res = mCallback(mApp);
    }
    catch (std::runtime_error const& er)
    {
        CLOG_ERROR(Work,
                   "WorkWithCallback {} error: cannot execute callback, {}",
                   getName(), er.what());
        return BasicWork::State::WORK_FAILURE;
    }

    if (!res)
    {
        CLOG_ERROR(Work, "WorkWithCallback {} error: callback failed",
                   getName());
        return BasicWork::State::WORK_FAILURE;
    }
    return BasicWork::State::WORK_SUCCESS;
}
}
