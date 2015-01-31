#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <vector>
#include "herder/HerderGateway.h"
#include "overlay/ItemFetcher.h"
#include "fba/FBA.h"
#include "util/Timer.h"

// Expected time between two ledger close.
#define EXP_LEDGER_TIMESPAN_SECONDS 2

// Maximum timeout for FBA consensus.
#define MAX_FBA_TIMEOUT_SECONDS 240

// Maximum time slip between nodes
#define MAX_TIME_SLIP_SECONDS 60

// How many ledger in past/future we consider an envelope viable.
#define LEDGER_VALIDITY_BRACKET 10

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
    int compareValues(const uint64& slotIndex, 
                      const uint32& ballotCounter,
                      const Value& v1, const Value& v2);

    void validateBallot(const uint64& slotIndex,
                        const uint256& nodeID,
                        const FBABallot& ballot,
                        std::function<void(bool)> const& cb);

    void ballotDidHearFromQuorum(const uint64& slotIndex,
                                 const FBABallot& ballot);
    void valueExternalized(const uint64& slotIndex,
                           const Value& value);

    void retrieveQuorumSet(const uint256& nodeID,
                           const Hash& qSetHash,
                           std::function<void(const FBAQuorumSet&)> const& cb);
    void emitEnvelope(const FBAEnvelope& envelope);
    
    // HerderGateway methods
    TxSetFramePtr fetchTxSet(const uint256& txSetHash, bool askNetwork);
    void recvTxSet(TxSetFramePtr txSet);
    void doesntHaveTxSet(uint256 const& txSethash, PeerPtr peer);

    FBAQuorumSetPtr fetchFBAQuorumSet(const uint256& qSetHash, bool askNetwork);
    void recvFBAQuorumSet(FBAQuorumSetPtr qSet);
    void doesntHaveFBAQuorumSet(uint256 const& qSetHash, PeerPtr peer);

    // returns whether the transaction should be flooded
    bool recvTransaction(TransactionFramePtr tx);

    void recvFBAEnvelope(FBAEnvelope envelope,
                         std::function<void(bool)> const& cb = [] (bool) { });

    void ledgerClosed(LedgerHeader& ledger);
    
  private:
    void removeReceivedTx(TransactionFramePtr tx);
    void triggerNextLedger(const asio::error_code& ec);
    void expireBallot(const asio::error_code& ec, 
                      const uint64& slotIndex,
                      const FBABallot& ballot);

    // 0- tx we got during ledger close
    // 1- one ledger ago. Will only validate a vblocking set
    // 2- two ledgers ago. Will only validate a vblock set and will rebroadcast
    // 3- three or more ledgers ago. Any set we validate must have these tx
    std::vector<std::vector<TransactionFramePtr>>  mReceivedTransactions;

    // need to keep the old tx sets around for at least one Consensus round in
    // case some stragglers are still need the old txsets in order to close
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

    unsigned                                       mLedgersToWaitToParticipate;
    LedgerHeader                                   mLastClosedLedger;

    VirtualClock::time_point                       mLastTrigger;
    VirtualTimer                                   mTriggerTimer;

    VirtualTimer                                   mBumpTimer;
    Value                                          mLocalValue;

    Application&                                   mApp;
};
}
