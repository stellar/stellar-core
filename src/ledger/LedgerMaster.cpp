#include "LedgerMaster.h"

namespace stellar
{
    /* NICOLAS
	LedgerMaster::pointer gLedgerMaster;

    LedgerMaster::LedgerMaster() : mCurrentDB(getApp().getWorkingLedgerDB())
	{
		mCaughtUp = false;
        reset();
	}

    void LedgerMaster::reset()
    {
        mCurrentCLF = LegacyCLF::pointer(new LegacyCLF()); // change this to BucketList when we are ready
        mLastLedgerHash = stellarxdr::uint256();
    }

	Ledger::pointer LedgerMaster::getCurrentLedger()
	{
		return(Ledger::pointer());
	}

	// called by txherder
	void LedgerMaster::externalizeValue(TransactionSet::pointer txSet, uint64_t closeTime)
	{
		closeLedger(txSet);

	}

	// called by CLF
	void LedgerMaster::ledgerHashComputed(stellarxdr::uint256& hash)
	{
		// SANITY
	}

    bool LedgerMaster::ensureSync(ripple::Ledger::pointer lastClosedLedger)
    {
        bool res = false;
        // first, make sure we're in sync with the world
        if (lastClosedLedger->getHash() != mLastLedgerHash)
        {
            std::vector<stellarxdr::uint256> needed=lastClosedLedger->getNeededAccountStateHashes(1,NULL);
            if(needed.size())
            {
                // we're missing some nodes
                return false;
            }

            try
            {
                CanonicalLedgerForm::pointer newCLF, currentCLF(new LegacyCLF(lastClosedLedger));
                mCurrentDB.beginTransaction();
                try
                {
                    newCLF = catchUp(currentCLF);
                }
                catch (...)
                {
                    mCurrentDB.endTransaction(true);
                }

                if (newCLF)
                {
                    mCurrentDB.endTransaction(false);
                    setLastClosedLedger(newCLF);
                    res = true;
                }
            }
            catch (...)
            {
                // problem applying to the database
                WriteLog(ripple::lsERROR, ripple::Ledger) << "database error";
            }
        }
        else
        { // already tracking proper ledger
            res = true;
        }

        return res;
    }

    void LedgerMaster::beginClosingLedger()
    {
        // ready to make changes
        mCurrentDB.beginTransaction();
        assert(mCurrentDB.getTransactionLevel() == 1); // should be top level transaction
    }

	bool  LedgerMaster::commitLedgerClose(ripple::Ledger::pointer ledger)
	{
        bool res = false;
        CanonicalLedgerForm::pointer newCLF;

        assert(ledger->getParentHash() == mLastLedgerHash); // should not happen

        try
        {
            CanonicalLedgerForm::pointer nl(new LegacyCLF(ledger));
            try
            {
                // only need to update ledger related fields as the account state is already in SQL
                updateDBFromLedger(nl);
                newCLF = nl;
            }
            catch (std::runtime_error const &)
            {
                WriteLog(ripple::lsERROR, ripple::Ledger) << "Ledger close: could not update database";
            }

            if (newCLF != nullptr)
            {
                mCurrentDB.endTransaction(false);
                setLastClosedLedger(newCLF);
                res = true;
            }
            else
            {
                mCurrentDB.endTransaction(true);
            }
        }
        catch (...)
        {
        }
        return res;
	}

    void LedgerMaster::setLastClosedLedger(CanonicalLedgerForm::pointer ledger)
    {
        // should only be done outside of transactions, to guarantee state reflects what is on disk
        assert(mCurrentDB.getTransactionLevel() == 0);
        mCurrentCLF = ledger;
        mLastLedgerHash = ledger->getHash();
        WriteLog(ripple::lsINFO, ripple::Ledger) << "Store at " << mLastLedgerHash;
    }

    void LedgerMaster::abortLedgerClose()
    {
        mCurrentDB.endTransaction(true);
    }

	void LedgerMaster::loadLastKnownCLF()
	{
        bool needreset = true;
        stellarxdr::uint256 lkcl = getLastClosedLedgerHash();
        if (lkcl.isNonZero()) {
            // there is a ledger in the database
            if (mCurrentCLF->load(lkcl)) {
                mLastLedgerHash = lkcl;
                needreset = false;
            }
        }
        if (needreset) {
            reset();
        }
	}

	CanonicalLedgerForm::pointer LedgerMaster::catchUp(CanonicalLedgerForm::pointer updatedCurrentCLF)
	{
		// new SLE , old SLE
		SHAMap::Delta delta;
        bool needFull = false;

        WriteLog(ripple::lsINFO, ripple::Ledger) << "catching up from " << mCurrentCLF->getHash() << " to " << updatedCurrentCLF;

        try
        {
            if (mCurrentCLF->getHash().isZero())
            {
                needFull = true;
            }
            else
            {
		        updatedCurrentCLF->getDeltaSince(mCurrentCLF,delta);
            }
        }
        catch (std::runtime_error const &e)
        {
            WriteLog(ripple::lsWARNING, ripple::Ledger) << "Could not compute delta: " << e.what();
            needFull = true;
        };

        if (needFull){
            return importLedgerState(updatedCurrentCLF->getHash());
        }

        // incremental update

        mCurrentDB.beginTransaction();

        try {
            BOOST_FOREACH(SHAMap::Delta::value_type it, delta)
		    {
                SLE::pointer newEntry = updatedCurrentCLF->getLegacyLedger()->getSLEi(it.first);
                SLE::pointer oldEntry = mCurrentCLF->getLegacyLedger()->getSLEi(it.first);

			    if(newEntry)
			    {
				    LedgerEntry::pointer entry = LedgerEntry::makeEntry(newEntry);
				    if(oldEntry)
				    {	// SLE updated
					    if(entry) entry->storeChange();
				    } else
				    {	// SLE added
					    if(entry) entry->storeAdd();
				    }
			    } else
			    { // SLE must have been deleted
                    assert(oldEntry);
				    LedgerEntry::pointer entry = LedgerEntry::makeEntry(oldEntry);
				    if(entry) entry->storeDelete();
			    }			
		    }
            updateDBFromLedger(updatedCurrentCLF);
        }
        catch (...) {
            mCurrentDB.endTransaction(true);
            throw;
        }

        mCurrentDB.endTransaction(false);

        return updatedCurrentCLF;
	}


    static void importHelper(SLE::ref curEntry, LedgerMaster &lm) {
        LedgerEntry::pointer entry = LedgerEntry::makeEntry(curEntry);
        if(entry) {
            entry->storeAdd();
        }
        // else entry type we don't care about
    }
    
    CanonicalLedgerForm::pointer LedgerMaster::importLedgerState(stellarxdr::uint256 ledgerHash)
    {
        CanonicalLedgerForm::pointer res;

        WriteLog(ripple::lsINFO, ripple::Ledger) << "Importing full ledger " << ledgerHash;

        CanonicalLedgerForm::pointer newLedger = LegacyCLF::pointer(new LegacyCLF());

        if (newLedger->load(ledgerHash)) {
            mCurrentDB.beginTransaction();
            try {
                // delete all
                LedgerEntry::dropAll(mCurrentDB);

                // import all anew
                newLedger->getLegacyLedger()->visitStateItems(BIND_TYPE (&importHelper, P_1, boost::ref (*this)));

                updateDBFromLedger(newLedger);
            }
            catch (...) {
                mCurrentDB.endTransaction(true);
                WriteLog(ripple::lsWARNING, ripple::Ledger) << "Could not import state";
                return CanonicalLedgerForm::pointer();
            }
            mCurrentDB.endTransaction(false);
            res = newLedger;
        }
        return res;
    }

    void LedgerMaster::updateDBFromLedger(CanonicalLedgerForm::pointer ledger)
    {
        stellarxdr::uint256 currentHash = ledger->getHash();
        string hex(to_string(currentHash));

        mCurrentDB.setState(mCurrentDB.getStoreStateName(LedgerDatabase::kLastClosedLedger), hex.c_str());
    }

    stellarxdr::uint256 LedgerMaster::getLastClosedLedgerHash()
    {
        string h = mCurrentDB.getState(mCurrentDB.getStoreStateName(LedgerDatabase::kLastClosedLedger));
        return stellarxdr::uint256(h); // empty string -> 0
    }

    void LedgerMaster::closeLedger(TransactionSet::pointer txSet)
	{
        mCurrentDB.beginTransaction();
        assert(mCurrentDB.getTransactionLevel() == 1);
        try {
		// apply tx set to the last ledger
            // todo: needs the logic to deal with partial failure
		    for(int n = 0; n < txSet->mTransactions.size(); n++)
		    {
			    txSet->mTransactions[n].apply();
		    }

		    // save collected changes to the bucket list
		    mCurrentCLF->closeLedger();

		    // save set to the history
		    txSet->store();
        }
        catch (...)
        {
            mCurrentDB.endTransaction(true);
            throw;
        }
        mCurrentDB.endTransaction(false);
        // SANITY this code is incomplete
	}
    */

}
