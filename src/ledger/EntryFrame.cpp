// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/EntryFrame.h"
#include "LedgerMaster.h"

namespace stellar
{
 
EntryFrame::EntryFrame()
{

}

EntryFrame::EntryFrame(const LedgerEntry& from) : mEntry(from)
{

}

uint256 EntryFrame::getIndex()
{
    if(isZero(mIndex))
    {
        calculateIndex();
    }
    return mIndex;
}

     /*
    I ended up with this kind of BS when I tried to drop these Frame classes and just use the .x classes
    void storeDeleteAccount(const LedgerEntry& entry, Json::Value& txResult, LedgerMaster& ledgerMaster);
    void storeDeleteOffer(const LedgerEntry& entry, Json::Value& txResult, LedgerMaster& ledgerMaster);
    void storeDeleteTrust(const LedgerEntry& entry, Json::Value& txResult, LedgerMaster& ledgerMaster);

    void getIndex(const LedgerEntry& entry, const uint256& retIndex)
    {
        switch(entry.type())
        {
        case LedgerTypes::NONE:
            return;
        case LedgerTypes::ACCOUNT:
        case LedgerTypes::OFFER:
        case LedgerTypes::TRUSTLINE:
        }
    }
   
    void storeDelete(const LedgerEntry& entry, Json::Value& txResult, LedgerMaster& ledgerMaster)
    {
        switch(entry.type())
        {
        case LedgerTypes::NONE:
            return;
        case LedgerTypes::ACCOUNT:
            return storeDeleteAccount(entry, txResult, ledgerMaster);
        case LedgerTypes::OFFER:
        case LedgerTypes::TRUSTLINE:
        }
    }
    void storeChange(const LedgerEntry& entry, const LedgerEntry& startFrom, Json::Value& txResult, LedgerMaster& ledgerMaster)
    {

    }

    void storeAdd(const LedgerEntry& entry, Json::Value& txResult, LedgerMaster& ledgerMaster)
    {

    } */
    
    /* 
	LedgerEntry::pointer LedgerEntry::makeEntry(SLE::pointer sle)
	{
		switch(sle->getType())
		{
		case ltACCOUNT_ROOT:
			return std::make_shared<AccountEntry>(sle);

		case ltRIPPLE_STATE:
			return std::make_shared<TrustLine>(sle);

		case ltOFFER:
			return std::make_shared<OfferEntry>(sle);
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
		return(uint256());
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
