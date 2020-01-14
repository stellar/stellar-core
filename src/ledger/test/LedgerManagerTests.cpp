// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManagerImpl.h"
#include "test/TestUtils.h"
#include "test/test.h"

#include <lib/catch.hpp>

using namespace stellar;

namespace stellar
{

class LedgerManagerForTests : public LedgerManagerImpl
{
  public:
    using LedgerManagerImpl::continueCatchup;
    using LedgerManagerImpl::finalizeCatchup;
    using LedgerManagerImpl::initializeCatchup;
    using LedgerManagerImpl::setCatchupState;

    LedgerManagerForTests(Application& app) : LedgerManagerImpl(app)
    {
    }

    bool
    syncingLedgersEmpty() const
    {
        return mSyncingLedgers.empty();
    }

    void
    closeLedger(LedgerCloseData const& lcd) override
    {
        LedgerHeader next;
        next.ledgerSeq = lcd.getLedgerSeq();
        advanceLedgerPointers(next);
    }
};

class LedgerManagerTestApplication : public TestApplication
{
  public:
    LedgerManagerTestApplication(VirtualClock& clock, Config const& cfg)
        : TestApplication(clock, cfg)
    {
    }

    virtual LedgerManagerForTests&
    getLedgerManager() override
    {
        auto& overlay = ApplicationImpl::getLedgerManager();
        return static_cast<LedgerManagerForTests&>(overlay);
    }

  private:
    virtual std::unique_ptr<LedgerManager>
    createLedgerManager() override
    {
        return std::make_unique<LedgerManagerForTests>(*this);
    }
};
}