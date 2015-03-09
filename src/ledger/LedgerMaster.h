#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"

#include <string>
#include "ledger/LedgerGateway.h"
#include "ledger/LedgerHeaderFrame.h"
#include "main/PersistentState.h"

/*
Holds the current ledger
Applies the tx set to the last ledger to get the next one
Hands the old ledger off to the history
*/

namespace medida { class Timer; }

namespace stellar
{
    class Application;
    class Database;
    class LedgerDelta;

    

    class LedgerMaster : public LedgerGateway
    {
        LedgerHeaderHistoryEntry mLastClosedLedger;
        LedgerHeaderFrame::pointer mCurrentLedger;

        Application &mApp;
        medida::Timer& mTransactionApply;
        medida::Timer& mLedgerClose;

        uint64_t mLastCloseTime;

        void startCatchUp();
        
        std::vector<LedgerCloseData> mSyncingLedgers;

        void historyCaughtup(asio::error_code const& ec);

    public:

        typedef std::shared_ptr<LedgerMaster>           pointer;
        typedef const std::shared_ptr<LedgerMaster>&    ref;

        LedgerMaster(Application& app);

        //////// GATEWAY FUNCTIONS
        // called by txherder
        void externalizeValue(LedgerCloseData ledgerData);

        uint64_t getLedgerNum();
        int64_t getMinBalance(uint32_t ownerCount);
        int64_t getTxFee(); // fee is a 32 bit but we use 64 to avoid overflow when doing math
        uint64_t getCloseTime();

        ///////

        uint64_t secondsSinceLastLedgerClose();

        void startNewLedger();
        void loadLastKnownLedger();

        // establishes that our internal representation is in sync with passed ledger
        //bool ensureSync(Ledger::pointer lastClosedLedger);

        // called before starting to make changes to the db
        void beginClosingLedger();
        // called every time we successfully closed a ledger
        //bool commitLedgerClose(Ledger::pointer ledger);
        // called when we could not close the ledger
        void abortLedgerClose();

        LedgerHeader& getCurrentLedgerHeader();
        LedgerHeaderHistoryEntry& getLastClosedLedgerHeader();

        Database& getDatabase();

		void closeLedger(LedgerCloseData ledgerData);

    private:
        void closeLedgerHelper(bool updateCurrent, LedgerDelta const& delta);


    };
}






