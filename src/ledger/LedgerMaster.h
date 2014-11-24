#ifndef __LEDGERMASTER__
#define __LEDGERMASTER__

#include "Ledger.h"
#include "clf/CanonicalLedgerForm.h"
#include "txherder/TransactionSet.h"
#include "ledger/LedgerDatabase.h"
#include "ledger/LedgerGateway.h"

/*
Holds the current ledger
Applies the tx set to the last ledger to get the next one
Hands the old ledger off to the history
*/

namespace stellar
{
	class LedgerMaster : public LedgerGateway
	{
		bool mCaughtUp;
		CanonicalLedgerForm::pointer mCurrentCLF;
        LedgerDatabase mCurrentDB;
        stellarxdr::uint256 mLastLedgerHash;
		
    public:

        typedef std::shared_ptr<LedgerMaster>           pointer;
        typedef const std::shared_ptr<LedgerMaster>&    ref;

		LedgerMaster();

		//////// GATEWAY FUNCTIONS
		// called by txherder
		void externalizeValue(TransactionSet::pointer txSet, uint64_t closeTime);

		// called by CLF
		void ledgerHashComputed(stellarxdr::uint256& hash);
		
		///////



		// called on startup to get the last CLF we knew about
		void loadLastKnownCLF();

		
        // establishes that our internal representation is in sync with passed ledger
        bool ensureSync(Ledger::pointer lastClosedLedger);

        // called before starting to make changes to the db
        void beginClosingLedger();
        // called every time we successfully closed a ledger
		bool commitLedgerClose(Ledger::pointer ledger);
        // called when we could not close the ledger
        void abortLedgerClose();

		Ledger::pointer getCurrentLedger();

		void closeLedger(TransactionSet::pointer txSet);
		CanonicalLedgerForm::pointer getCurrentCLF(){ return(mCurrentCLF); }

    private:

        // helper methods: returns new value of CLF in database 
        
        // called when we successfully sync to the network
		CanonicalLedgerForm::pointer catchUp(CanonicalLedgerForm::pointer currentCLF);
        CanonicalLedgerForm::pointer importLedgerState(stellarxdr::uint256 ledgerHash);
        
        void updateDBFromLedger(CanonicalLedgerForm::pointer ledger);
        
        void setLastClosedLedger(CanonicalLedgerForm::pointer ledger);
        stellarxdr::uint256 getLastClosedLedgerHash();

        void reset();
	};

	extern LedgerMaster::pointer gLedgerMaster;
}

#endif




