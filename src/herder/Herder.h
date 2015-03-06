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

// Maximum time slip between nodes.
#define MAX_TIME_SLIP_SECONDS 60

// How many seconds of inactivity before evicting a node.
#define NODE_EXPIRATION_SECONDS 240

// How many ledger in past/future we consider an envelope viable.
#define LEDGER_VALIDITY_BRACKET 10

namespace medida { class Meter; }

namespace stellar
{
class Application;

using xdr::operator<;
using xdr::operator==;

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

    void nodeTouched(const uint256& nodeID);

    void retrieveQuorumSet(const uint256& nodeID,
                           const Hash& qSetHash,
                           std::function<void(const FBAQuorumSet&)> const& cb);
    void emitEnvelope(const FBAEnvelope& envelope);

    // Extra FBA methods overridden solely to increment metrics.
    void ballotDidPrepare(const uint64& slotIndex, const FBABallot& ballot) override;
    void ballotDidPrepared(const uint64& slotIndex, const FBABallot& ballot) override;
    void ballotDidCommit(const uint64& slotIndex, const FBABallot& ballot) override;
    void ballotDidCommitted(const uint64& slotIndex, const FBABallot& ballot) override;
    void envelopeSigned() override;
    void envelopeVerified(bool) override;

    
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

    void startRebroadcastTimer();
    void rebroadcast(const asio::error_code& ec);

    // StellarBallot internal signature/verification
    void signStellarBallot(StellarBallot& b);
    bool verifyStellarBallot(const StellarBallot& b);

    // 0- tx we got during ledger close
    // 1- one ledger ago. rebroadcast
    // 2- two ledgers ago. 
    std::vector<std::vector<TransactionFramePtr>>  mReceivedTransactions;


    // Time of last access to a node, used to evict unused nodes.
    std::map<uint256, VirtualClock::time_point>    mNodeLastAccess;

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

    std::map<FBABallot,
        std::map<uint256,
             std::vector<
                 std::shared_ptr<VirtualTimer>>>>  mBallotValidationTimers;

    
    LedgerHeader                                   mLastClosedLedger;

    VirtualClock::time_point                       mLastTrigger;
    VirtualTimer                                   mTriggerTimer;

    VirtualTimer                                   mBumpTimer;
    VirtualTimer                                   mRebroadcastTimer;
    Value                                          mCurrentValue;
    StellarMessage                                 mLastSentMessage;

    Application&                                   mApp;

    medida::Meter& mValueValid;
    medida::Meter& mValueInvalid;
    medida::Meter& mValuePrepare;
    medida::Meter& mValueExternalize;

    medida::Meter& mBallotValid;
    medida::Meter& mBallotInvalid;
    medida::Meter& mBallotPrepare;
    medida::Meter& mBallotPrepared;
    medida::Meter& mBallotCommit;
    medida::Meter& mBallotCommitted;
    medida::Meter& mBallotSign;
    medida::Meter& mBallotValidSig;
    medida::Meter& mBallotInvalidSig;
    medida::Meter& mBallotExpire;

    medida::Meter& mQuorumHeard;
    medida::Meter& mQsetRetrieve;

    medida::Meter& mEnvelopeEmit;
    medida::Meter& mEnvelopeReceive;
    medida::Meter& mEnvelopeSign;
    medida::Meter& mEnvelopeValidSig;
    medida::Meter& mEnvelopeInvalidSig;

};
}
