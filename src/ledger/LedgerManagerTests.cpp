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
    using LedgerManagerImpl::applyBufferedLedgers;
    using LedgerManagerImpl::continueCatchup;
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

TEST_CASE("new ledger comes from network after last applyBufferedLedgers is "
          "scheduled",
          "[ledger]")
{
    VirtualClock clock;
    auto app = createTestApplication<LedgerManagerTestApplication>(
        clock, getTestConfig());
    app->start();

    auto ledgerCloseData = [](uint32_t ledger) {
        auto txSet = std::make_shared<TxSetFrame>(Hash{});
        StellarValue sv{txSet->getContentsHash(), 2, emptyUpgradeSteps, 0};
        return LedgerCloseData{ledger, txSet, sv};
    };

    auto& ledgerManager = app->getLedgerManager();
    ledgerManager.initializeCatchup(ledgerCloseData(2));
    ledgerManager.continueCatchup(ledgerCloseData(3));

    ledgerManager.setCatchupState(
        LedgerManager::CatchupState::APPLYING_BUFFERED_LEDGERS);
    ledgerManager.applyBufferedLedgers();

    while (clock.crank())
    {
        if (ledgerManager.syncingLedgersEmpty())
        {
            REQUIRE(ledgerManager.getCatchupState() ==
                    LedgerManager::CatchupState::APPLYING_BUFFERED_LEDGERS);
            break;
        }
    }

    ledgerManager.continueCatchup(ledgerCloseData(5));
    REQUIRE(ledgerManager.getCatchupState() ==
            LedgerManager::CatchupState::APPLYING_BUFFERED_LEDGERS);

    clock.crank(); // crash
}
