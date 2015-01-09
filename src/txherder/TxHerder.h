#ifndef __TXHERDER__
#define __TXHERDER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <vector>
#include "overlay/ItemFetcher.h"
#include "txherder/TxHerderGateway.h"

/*
receives tx from network
keeps track of all the Tx Sets we know about
FBA checks it to see if a give set is valid
It tells FBA to start the next round
*/

namespace stellar
{
class Application;

class TransactionFrame;
typedef shared_ptr<TransactionFrame> TransactionFramePtr;

class TxHerder : public TxHerderGateway
{
    // the transactions that we have collected during ledger close
    TxSetFramePtr mCollectingTransactionSet;

    // keep track of txs that didn't make it into last ledger.
    // be less and less likely to commit a ballot that doesn't include the old
    // ones
    map<uint256, uint32_t> mTransactionAgeMap;

    // 0- tx we got during ledger close
    // 1- one ledger ago. Will only validate a vblocking set
    // 2- two ledgers ago. Will only validate a vblock set and will rebroadcast
    // 3- three or more ledgers ago. Any set we validate must have these tx
    vector<vector<TransactionFramePtr>> mReceivedTransactions;

    std::array<TxSetFetcher, 2> mTxSetFetcher;
    int mCurrentTxSetFetcher;

		int mLedgersToWaitToParticipate;
        Application &mApp;

    LedgerPtr mLastClosedLedger;
    void removeReceivedTx(TransactionFramePtr tx);

  public:
    TxHerder(Application& app);

    ///////// GATEWAY FUNCTIONS
    // make sure this set contains any super old TXs
    BallotValidType isValidBallotValue(const Ballot& ballot);
    TxHerderGateway::SlotComparisonType
    compareSlot(const SlotBallot& ballot);

    // will start fetching this TxSet from the network if we don't know about it
    TransactionSetPtr fetchTxSet(const uint256& setHash,
                                 bool askNetwork);

    void externalizeValue(const SlotBallot& slotBallot);

    // a Tx set comes in from the wire
    void recvTransactionSet(TransactionSetPtr txSet);
    void
    doesntHaveTxSet(uint256 const& setHash, Peer::pointer peer)
    {
        mTxSetFetcher[mCurrentTxSetFetcher].doesntHave(setHash, peer);
    }

    // we are learning about a new transaction
    // return true if we should flood
    bool recvTransaction(TransactionFramePtr tx);

   

    void ledgerClosed(LedgerPtr ledger);

    /////////////////
};
}

#endif
