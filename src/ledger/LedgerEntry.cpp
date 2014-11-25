#include "LedgerEntry.h"
#include "LedgerMaster.h"
#include "TrustLine.h"
#include "OfferEntry.h"

namespace stellar
{
    /* NICOLAS
	LedgerEntry::pointer LedgerEntry::makeEntry(SLE::pointer sle)
	{
		switch(sle->getType())
		{
		case ltACCOUNT_ROOT:
			return LedgerEntry::pointer(new AccountEntry(sle));

		case ltRIPPLE_STATE:
			return LedgerEntry::pointer(new TrustLine(sle));

		case ltOFFER:
			return LedgerEntry::pointer(new OfferEntry(sle));
		}
		return(LedgerEntry::pointer());
	}

	uint256 LedgerEntry::getIndex()
	{
		if(mIndex.isZero()) calculateIndex();
		return(mIndex);
	}

	// these will do the appropriate thing in the DB and the Canonical Ledger form
	void LedgerEntry::storeDelete()
	{
		deleteFromDB();
		
		gLedgerMaster->getCurrentCLF()->deleteEntry( getHash() );
	}

	void LedgerEntry::storeChange()
	{
		updateInDB();

		gLedgerMaster->getCurrentCLF()->deleteEntry(getHash());
	}

	void LedgerEntry::storeAdd()
	{
		insertIntoDB();

		gLedgerMaster->getCurrentCLF()->deleteEntry(getHash());
	}


	// NICOLAS
	uint256 LedgerEntry::getHash()
	{
		return(stellarxdr::uint256());
		//if(!mSLE) makeSLE();
		//return(mSLE->getHash());
	}

    // NICOLAS use a registration pattern instead as we also need factories
    void LedgerEntry::dropAll(LedgerDatabase &db)
    {
        // NICOLAS implement this for the actual ledger entry ~~ the ledger class seems to conflict with this
        AccountEntry::dropAll(db);
        TrustLine::dropAll(db);
        OfferEntry::dropAll(db);
    }
    */
}
