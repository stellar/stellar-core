#include "BucketList.h"

// TODO.2  all below 


namespace stellar
{

    bool BucketList::load(stellarxdr::uint256 ledgerHash)
    {

            return(true);
    }

    LedgerHeaderPtr BucketList::getCurrentHeader()
    {
        // TODO.1
        return LedgerHeaderPtr();
        //return mCurrentLedger->mHeader;
    }

    void BucketList::calculateHash()
    {

    }

    // this is called when we are catching up to the network
    // we need to apply this delta to our current CLF
    void BucketList::recvDelta(CLFDeltaPtr delta)
    {

    }

    void BucketList::addEntry(stellarxdr::uint256& newHash, 
        LedgerEntry::pointer newEntry)
    {

    }

    void BucketList::updateEntry(stellarxdr::uint256& oldHash,
        stellarxdr::uint256& newHash, LedgerEntry::pointer updatedEntry)
    {

    }
    void BucketList::deleteEntry(stellarxdr::uint256& hash)
    {

    }

    // need to call after all the tx have been applied to save that last
    // versions of the ledger entries into the buckets
    void BucketList::closeLedger()
    {

    }

    stellarxdr::uint256 BucketList::getHash()
    {
            return(mHash);
    }

}