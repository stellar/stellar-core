#include "txherder/TxHerder.h"
#include "ledger/LedgerMaster.h"
#include "main/Application.h"
#include <time.h>
#include "util/Logging.h"
#include "txherder/TransactionSet.h"
#include "lib/util/easylogging++.h"
#include "fba/FBA.h"

namespace stellar
{
TxHerder::TxHerder(Application& app)
    : mCollectingTransactionSet(std::make_shared<TransactionSet>())
    , mReceivedTransactions(4)
#ifdef _MSC_VER
    // This form of initializer causes a warning due to brace-elision on
    // clang.
    , mTxSetFetcher({TxSetFetcher(app), TxSetFetcher(app)})
#else
    // This form of initializer is "not implemented" in MSVC yet.
    , mTxSetFetcher
{
    {
        {
            TxSetFetcher(app)
        }
        ,
        {
            TxSetFetcher(app)
        }
    }
}
#endif
    , mCurrentTxSetFetcher(0)
    , mCloseCount(0)
    , mApp(app)
{
}

// make sure all the tx we have in the old set are included
// make sure the timestamp isn't too far in the future
// make sure the base fee is within a certain range of your desired fee
TxHerderGateway::BallotValidType
TxHerder::isValidBallotValue(stellarxdr::Ballot const& ballot)
{
    if (ballot.baseFee < mApp.mConfig.DESIRED_BASE_FEE * .5)
        return INVALID_BALLOT;
    if (ballot.baseFee > mApp.mConfig.DESIRED_BASE_FEE * 2)
        return INVALID_BALLOT;

    TransactionSetPtr txSet = fetchTxSet(ballot.txSetHash, true);
    if (!txSet)
    {
        LOG(ERROR) << "isValidBallotValue when we don't know the txSet";
        return INVALID_BALLOT;
    }

    for (auto tx : mReceivedTransactions[mReceivedTransactions.size() - 1])
    {
        if (find(txSet->mTransactions.begin(), txSet->mTransactions.end(),
                 tx) == txSet->mTransactions.end())
            return INVALID_BALLOT;
    }
    // check timestamp
    if (ballot.closeTime <= mLastClosedLedger->mHeader.closeTime)
        return INVALID_BALLOT;

    uint64_t timeNow = time(nullptr);
    if (ballot.closeTime > timeNow + MAX_SECONDS_LEDGER_CLOSE_IN_FUTURE)
        return FUTURE_BALLOT;

    return VALID_BALLOT;
}

TxHerderGateway::SlotComparisonType
TxHerder::compareSlot(stellarxdr::SlotBallot const& slotBallot)
{
    if (slotBallot.ledgerIndex > mLastClosedLedger->mHeader.ledgerSeq)
        return (TxHerderGateway::FUTURE_SLOT);
    if (slotBallot.ledgerIndex < mLastClosedLedger->mHeader.ledgerSeq)
        return (TxHerderGateway::PAST_SLOT);
    if (slotBallot.previousLedgerHash == mLastClosedLedger->mHeader.hash)
        return (TxHerderGateway::SAME_SLOT);
    return TxHerderGateway::INCOMPATIBLIE_SLOT;
}

bool
TxHerder::isTxKnown(stellarxdr::uint256 const& txHash)
{
    for (auto list : mReceivedTransactions)
    {
        for (auto tx : list)
        {
            if (tx->getHash() == txHash)
                return true;
        }
    }

    return (false);
}

// a Tx set comes in from the wire
void
TxHerder::recvTransactionSet(TransactionSetPtr txSet)
{
    if (mTxSetFetcher[mCurrentTxSetFetcher].recvItem(txSet))
    { // someone cares about this set
        for (auto tx : txSet->mTransactions)
        {
            recvTransaction(tx);
        }
        mApp.getFBAGateway().transactionSetAdded(txSet);
    }
}

// return true if we should flood
bool
TxHerder::recvTransaction(TransactionPtr tx)
{
    stellarxdr::uint256 txHash = tx->getHash();
    if (!isTxKnown(txHash))
    {
        if (tx->isValid())
        {

            mReceivedTransactions[0].push_back(tx);

            return true;
        }
    }
    return false;
}

// will start fetching this TxSet from the network if we don't know about it
TransactionSetPtr
TxHerder::fetchTxSet(stellarxdr::uint256 const& setHash, bool askNetwork)
{
    return mTxSetFetcher[mCurrentTxSetFetcher].fetchItem(setHash, askNetwork);
}

void
TxHerder::removeReceivedTx(TransactionPtr dropTX)
{
    for (auto list : mReceivedTransactions)
    {
        for (auto iter = list.begin(); iter != list.end();)
        {
            if ((iter.operator->())->get()->getHash() == dropTX->getHash())
            {
                list.erase(iter);
                return;
            }
            else
                iter++;
        }
    }
}

// called by FBA
void
TxHerder::externalizeValue(stellarxdr::SlotBallot const& slotBallot)
{
    TransactionSet::pointer externalizedSet =
        fetchTxSet(slotBallot.ballot.txSetHash, false);
    if (externalizedSet)
    {
        // we don't need to keep fetching any of the old TX sets
        mTxSetFetcher[mCurrentTxSetFetcher].stopFetchingAll();
        if (mCurrentTxSetFetcher)
            mCurrentTxSetFetcher = 0;
        else
            mCurrentTxSetFetcher = 1;
        mTxSetFetcher[mCurrentTxSetFetcher].clear();

        mApp.getLedgerGateway().externalizeValue(slotBallot, externalizedSet);

        // remove all these tx from mReceivedTransactions
        for (auto tx : externalizedSet->mTransactions)
        {
            removeReceivedTx(tx);
        }
        // rebroadcast those left in set 1
        for (auto tx : mReceivedTransactions[1])
        {
            auto msg = tx->toStellarMessage();
            mApp.getPeerMaster().broadcastMessage(msg, Peer::pointer());
        }

        // move all the remaining to the next highest level
        // don't move the largest array
        for (int n = mReceivedTransactions.size() - 2; n >= 0; n--)
        {
            for (auto tx : mReceivedTransactions[n])
            {
                mReceivedTransactions[n + 1].push_back(tx);
            }
            mReceivedTransactions[n].clear();
        }
    }
    else
        CLOG(ERROR, "TxHerder") << "externalizeValue txset not found: ";
}

// called by Ledger
void
TxHerder::ledgerClosed(LedgerPtr ledger)
{
    // our first choice for this round's set is all the tx we have collected
    // during last ledger close
    TransactionSet::pointer proposedSet = std::make_shared<TransactionSet>();
    for (auto list : mReceivedTransactions)
    {
        for (auto tx : list)
        {
            proposedSet->add(tx);
        }
    }

    mLastClosedLedger = ledger;

    uint64_t firstBallotTime = time(nullptr) + NUM_SECONDS_IN_CLOSE;
    if (firstBallotTime <= mLastClosedLedger->mHeader.closeTime)
        firstBallotTime = mLastClosedLedger->mHeader.closeTime + 1;

    recvTransactionSet(proposedSet);
    stellarxdr::SlotBallot firstBallot;
    firstBallot.ledgerIndex = mLastClosedLedger->mHeader.ledgerSeq;
    firstBallot.previousLedgerHash = mLastClosedLedger->mHeader.hash;
    firstBallot.ballot.index = 1;
    firstBallot.ballot.closeTime = firstBallotTime;
    firstBallot.ballot.baseFee = mApp.mConfig.DESIRED_BASE_FEE;
    firstBallot.ballot.txSetHash = proposedSet->getContentsHash();

    mCloseCount++;
    // don't participate in FBA for a few ledger closes so you make sure you
    // don't
    // send PREPAREs that don't include old tx
    if (mCloseCount > 2 && (!isZero(mApp.mConfig.VALIDATION_SEED)))
        mApp.getFBAGateway().setValidating(true);

    mApp.getFBAGateway().startNewRound(firstBallot);
}
}
