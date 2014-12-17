// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "LedgerMaster.h"
#include "main/Application.h"
#include "util/Logging.h"

/*
The ledger module:
    1) gets the externalized tx set
    2) applies this set to the previous ledger
    3) sends the changed entries to the CLF
    4) saves the changed entries to SQL
    5) saves the ledger hash and header to SQL
    6) sends the new ledger hash and the tx set to the history
    7) sends the new ledger hash and header to the TxHerder
    

catching up to network:
    1) Wait for FBA to tell us what the network is on now
    2) Ask network for the the delta between what it has now and our ledger last ledger  

    // TODO.1 need to make sure the CLF and the SQL Ledger are in sync on start up
    // TODO.1 make sure you validate incoming Deltas to see that it gives you the CLF you want
    // TODO.1 do we need to store some validation history?

*/
namespace stellar
{

LedgerMaster::LedgerMaster(Application& app) : mApp(app)
{
	mCaughtUp = false;
    //syncWithCLF();
}

void LedgerMaster::startNewLedger()
{
    // TODO.2

}

LedgerHeaderPtr LedgerMaster::getCurrentHeader()
{
    // TODO.1
    return LedgerHeaderPtr();
    //return mCurrentLedger->mHeader;
}

// make sure our state is consistent with the CLF
void LedgerMaster::syncWithCLF()
{
    LedgerHeaderPtr clfHeader=mApp.getCLFGateway().getCurrentHeader();
    LedgerHeaderPtr sqlHeader = getCurrentHeader();

    if(clfHeader->hash == sqlHeader->hash)
    {
        CLOG(DEBUG, "Ledger") << "CLF and SQL headers match.";
    } else
    {  // ledgers don't match
        // LATER try to sync them
        CLOG(ERROR, "Ledger") << "CLF and SQL headers don't match. Aborting";
    }
}

// called by txherder
void LedgerMaster::externalizeValue(const stellarxdr::SlotBallot& slotBallot, TransactionSet::pointer txSet)
{
    if(getCurrentHeader()->hash == slotBallot.previousLedgerHash)
    {
        mCaughtUp = true;
        closeLedger(txSet);
    }
    else
    { // we need to catch up
        mCaughtUp = false;
        if(mApp.mState == Application::CATCHING_UP_STATE)
        {  // we are already trying to catch up
            CLOG(DEBUG, "Ledger") << "Missed a ledger while trying to catch up.";
        } else
        {  // start trying to catchup
            startCatchUp();
        }
    }
}

// we have some last ledger that is in the DB
// we need to 
void LedgerMaster::startCatchUp()
{
    mApp.mState = Application::CATCHING_UP_STATE;
    mApp.getOverlayGateway().fetchDelta(getCurrentHeader()->hash, getCurrentHeader()->ledgerSeq );

}

// called by CLF
// when we either get in a delta from the network or the CLF computes the next ledger hash
// delta is null in the second case
void LedgerMaster::recvDelta(CLFDeltaPtr delta, LedgerHeaderPtr header)
{
    if(delta)
    { // we got this delta in from the network
        mApp.mState = Application::CONNECTED_STATE;
            
    } else
    {

    }
}

    

void LedgerMaster::closeLedger(TransactionSet::pointer txSet)
{
    for(auto tx : txSet->mTransactions)
    {
        tx->apply();
    }
}


/* NICOLAS

Ledger::pointer LedgerMaster::getCurrentLedger()
{
	return(Ledger::pointer());
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
            CanonicalLedgerForm::pointer newCLF, currentCLF = std::make_shared<LegacyCLF>(lastClosedLedger));
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
            CLOG(ripple::ERROR, ripple::Ledger) << "database error";
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
        CanonicalLedgerForm::pointer nl = std::make_shared<LegacyCLF>(ledger);
        try
        {
            // only need to update ledger related fields as the account state is already in SQL
            updateDBFromLedger(nl);
            newCLF = nl;
        }
        catch (std::runtime_error const &)
        {
            CLOG(ripple::ERROR, ripple::Ledger) << "Ledger close: could not update database";
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
    CLOG(ripple::lsINFO, ripple::Ledger) << "Store at " << mLastLedgerHash;
}

void LedgerMaster::abortLedgerClose()
{
    mCurrentDB.endTransaction(true);
}

	

CanonicalLedgerForm::pointer LedgerMaster::catchUp(CanonicalLedgerForm::pointer updatedCurrentCLF)
{
	// new SLE , old SLE
	SHAMap::Delta delta;
    bool needFull = false;

    CLOG(ripple::lsINFO, ripple::Ledger) << "catching up from " << mCurrentCLF->getHash() << " to " << updatedCurrentCLF;

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
        CLOG(ripple::WARNING, ripple::Ledger) << "Could not compute delta: " << e.what();
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

    CLOG(ripple::lsINFO, ripple::Ledger) << "Importing full ledger " << ledgerHash;

    CanonicalLedgerForm::pointer newLedger = std::make_shared<LegacyCLF>();

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
            CLOG(ripple::WARNING, ripple::Ledger) << "Could not import state";
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
    // NICOLAS this code is incomplete
}
*/

}
