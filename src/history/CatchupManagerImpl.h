#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/CatchupManager.h"
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

    medida::Meter& mCatchupStart;
    medida::Meter& mCatchupSuccess;
    medida::Meter& mCatchupFailure;

  public:
    CatchupManagerImpl(Application& app);
    ~CatchupManagerImpl() override;

    void historyCaughtup() override;

    void catchupHistory(
        uint32_t initLedger, CatchupMode mode,
        std::function<void(asio::error_code const& ec, CatchupMode mode,
                           LedgerHeaderHistoryEntry const& lastClosed)>
            handler,
        bool manualCatchup) override;

    std::string getStatus() const override;

    uint64_t getCatchupStartCount() const override;
    uint64_t getCatchupSuccessCount() const override;
    uint64_t getCatchupFailureCount() const override;
};
}
