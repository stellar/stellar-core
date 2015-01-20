#pragma once

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
               public FBA
{
  public:
    Herder(Application& app);
    ~Herder();

    // Bootstraps the Herder if we're creating a new Network
    void bootstrap();
    
    // FBA methods
    void validateValue(const uint64& slotIndex,
                       const uint256& nodeID,
                       const Value& value,
                       std::function<void(bool)> const& cb);

    void validateBallot(const uint64& slotIndex,
                        const uint256& nodeID,
                        const FBABallot& ballot,
                        std::function<void(bool)> const& cb);

    void valueExternalized(const uint64& slotIndex,
                           const Value& value);

    void retrieveQuorumSet(const uint256& nodeID,
                           const Hash& qSetHash,
                           std::function<void(const FBAQuorumSet&)> const& cb);
    void emitEnvelope(const FBAEnvelope& envelope);
    
    // HerderGateway methods
    TxSetFramePtr fetchTxSet(const uint256& setHash, bool askNetwork);
    void recvTxSet(TxSetFramePtr txSet);
    void doesntHaveTxSet(uint256 const& setHash, PeerPtr peer);

    FBAQuorumSetPtr fetchFBAQuorumSet(uint256 const& qSetHash, bool askNetwork);
    void recvFBAQuorumSet(FBAQuorumSetPtr qSet);
    void doesntHaveFBAQuorumSet(uint256 const& qSetHash, PeerPtr peer);

    // returns whether the transaction should be flooded
    bool recvTransaction(TransactionFramePtr tx);

    void recvFBAEnvelope(FBAEnvelope envelope,
                         std::function<void(bool)> const& cb = [] (bool) { });

    void ledgerClosed(LedgerHeader& ledger);
    
  private:
    void removeReceivedTx(TransactionFramePtr tx);
    void advanceToNextLedger();

    // the transactions that we have collected during ledger close
    TxSetFramePtr                                  mCollectingTransactionSet;

    // keep track of txs that didn't make it into last ledger. Be less and less
    // likely to commit a ballot that doesn't include the old ones
    std::map<uint256, uint32_t>                    mTransactionAgeMap;

    // 0- tx we got during ledger close
    // 1- one ledger ago. Will only validate a vblocking set
    // 2- two ledgers ago. Will only validate a vblock set and will rebroadcast
    // 3- three or more ledgers ago. Any set we validate must have these tx
    std::vector<std::vector<TransactionFramePtr>>  mReceivedTransactions;

    // need to keep the old tx sets around for at least one Consensus round
    //  in case some stragglers are still need the old txsets in order to close
    std::array<TxSetFetcher, 2>                    mTxSetFetcher;
    int                                            mCurrentTxSetFetcher;
    std::map<Hash, 
        std::vector<
            std::function<void(TxSetFramePtr)>>>   mTxSetFetches;

    FBAQSetFetcher                                 mFBAQSetFetcher;
    std::map<Hash,
        std::vector<
            std::function<void(FBAQuorumSetPtr)>>> mFBAQSetFetches;

    std::map<uint64,
        std::vector<
            std::pair<FBAEnvelope, 
                      std::function<void(bool)>>>> mFutureEnvelopes;

    int                                            mLedgersToWaitToParticipate;
    LedgerHeader                                   mLastClosedLedger;

    Application&                                   mApp;
};
}
