#ifndef __HERDER__
#define __HERDER__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <vector>
#include "herder/HerderGateway.h"
#include "overlay/ItemFetcher.h"
#include "fba/FBA.h"

// beyond this then the ballot is considered invalid
#define MAX_SECONDS_LEDGER_CLOSE_IN_FUTURE 2

// how far in the future to guess the ledger will close
#define NUM_SECONDS_IN_CLOSE 2

namespace stellar
{
class Application;

/*
 * Drives the FBA protocol (is an FBA::Client). It is also incharge of
 * receiving transactions from the network.
 */
class Herder : public HerderGateway,
               public FBA::Client
{
  public:
    Herder(Application& app);
    ~Herder();


    // FBA::Client methods
    void validateBallot(const uint64& slotIndex,
                        const uint256& nodeID,
                        const FBABallot& ballot,
                        const Hash& evidence,
                        std::function<void(bool)> const& cb);

    void ballotDidPrepare(const uint64& slotIndex,
                          const FBABallot& ballot,
                          const Hash& evidence);
    void ballotDidCommit(const uint64& slotIndex,
                         const FBABallot& ballot);

    void valueCancelled(const uint64& slotIndex,
                        const Hash& valueHash);
    void valueExternalized(const uint64& slotIndex,
                           const Hash& valueHash);

    void retrieveQuorumSet(const uint256& nodeID,
                           const Hash& qSetHash);
    void emitEnvelope(const FBAEnvelope& envelope);
    
    void retransmissionHinted(const uint64& slotIndex,
                              const uint256& nodeID);

    // HerderGateway methods
    TxSetFramePtr fetchTxSet(const uint256& setHash, bool askNetwork);
    void recvTxSet(TxSetFramePtr txSet);
    void doesntHaveTxSet(uint256 const& setHash, PeerPtr peer);

    FBAQuorumSetPtr fetchFBAQuorumSet(uint256 const& qSetHash, bool askNetwork);
    void recvFBAQuorumSet(FBAQuorumSetPtr qSet);
    void doesntHaveFBAQuorumSet(uint256 const& qSetHash, PeerPtr peer);

    // returns wether the transaction should be flooded
    bool recvTransaction(TransactionFramePtr tx);

    void recvFBAEnvelope(FBAEnvelope envelope);

    void ledgerClosed(LedgerHeader& ledger);
    
  private:
    void removeReceivedTx(TransactionFramePtr tx);

    // the transactions that we have collected during ledger close
    TxSetFramePtr                       mCollectingTransactionSet;

    // keep track of txs that didn't make it into last ledger.
    // be less and less likely to commit a ballot that doesn't include the old
    // ones
    map<uint256, uint32_t>              mTransactionAgeMap;

    // 0- tx we got during ledger close
    // 1- one ledger ago. Will only validate a vblocking set
    // 2- two ledgers ago. Will only validate a vblock set and will rebroadcast
    // 3- three or more ledgers ago. Any set we validate must have these tx
    vector<vector<TransactionFramePtr>> mReceivedTransactions;

    std::array<TxSetFetcher, 2>         mTxSetFetcher;
    int                                 mCurrentTxSetFetcher;
    FBAQSetFetcher                      mFBAQSetFetcher;

    int                                 mLedgersToWaitToParticipate;
    LedgerHeader                        mLastClosedLedger;

    Application&                        mApp;
    FBA*                                mFBA;
};
}

#endif
