#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupManager.h"
#include <memory>

namespace medida
{
class Meter;
}

namespace stellar
{

class Application;
class Work;

class CatchupManagerImpl : public CatchupManager
{
    Application& mApp;
    std::shared_ptr<Work> mCatchupWork;

  public:
    CatchupManagerImpl(Application& app);
    ~CatchupManagerImpl() override;

    void historyCaughtup() override;

    void catchupHistory(CatchupConfiguration catchupConfiguration,
                        CatchupWork::ProgressHandler handler) override;

    std::string getStatus() const override;

    void logAndUpdateCatchupStatus(bool contiguous,
                                   std::string const& message) override;
    void logAndUpdateCatchupStatus(bool contiguous) override;
};
}
