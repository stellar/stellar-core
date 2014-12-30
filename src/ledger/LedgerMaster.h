#ifndef __LEDGERMASTER__
#define __LEDGERMASTER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Ledger.h"
#include "clf/CanonicalLedgerForm.h"
#include "database/Database.h"
#include "ledger/LedgerGateway.h"

/*
Holds the current ledger
Applies the tx set to the last ledger to get the next one
Hands the old ledger off to the history
*/

namespace stellar
{
    class Application;

	class LedgerMaster : public LedgerGateway
	{
		bool mCaughtUp;
		// CanonicalLedgerForm::pointer mCurrentCLF;
        // LATER LedgerDatabase mCurrentDB;
        //uint256 mLastLedgerHash;
        Ledger::pointer mCurrentLedger;
        Database mDatabase;

        Application &mApp;

        void startCatchUp();

        LedgerHeaderPtr getCurrentHeader();
        // called on startup to get the last CLF we knew about
        void syncWithCLF();

    public:

        typedef std::shared_ptr<LedgerMaster>           pointer;
        typedef const std::shared_ptr<LedgerMaster>&    ref;

		LedgerMaster(Application& app);

        

		//////// GATEWAY FUNCTIONS
		// called by txherder
		void externalizeValue(const SlotBallot& slotBallot, TransactionSetPtr txSet);

		// called by CLF
        void recvDelta(CLFDeltaPtr delta, LedgerHeaderPtr header);

        int64_t getFee();
        int64_t getLedgerNum();
        int64_t getMinBalance(int32_t ownerCount);
		
		///////

        void startNewLedger();

		
        // establishes that our internal representation is in sync with passed ledger
        bool ensureSync(Ledger::pointer lastClosedLedger);

        // called before starting to make changes to the db
        void beginClosingLedger();
        // called every time we successfully closed a ledger
		bool commitLedgerClose(Ledger::pointer ledger);
        // called when we could not close the ledger
        void abortLedgerClose();

		Ledger::pointer getCurrentLedger();

        Database& getDatabase() { return mDatabase;  }

		void closeLedger(TransactionSetPtr txSet);
		

    private:

        // helper methods: returns new value of CLF in database 
        
        // called when we successfully sync to the network
		CanonicalLedgerForm::pointer catchUp(CanonicalLedgerForm::pointer currentCLF);
        CanonicalLedgerForm::pointer importLedgerState(uint256 ledgerHash);
        
        void updateDBFromLedger(CanonicalLedgerForm::pointer ledger);
        
        void setLastClosedLedger(CanonicalLedgerForm::pointer ledger);
        uint256 getLastClosedLedgerHash();

        void reset();
	};

	extern LedgerMaster::pointer gLedgerMaster;
}

#endif




