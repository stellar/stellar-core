#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <vector>
#include "herder/Herder.h"
#include "overlay/ItemFetcher.h"
#include "scp/SCP.h"
#include "util/Timer.h"

// Expected time between two ledger close.
#define EXP_LEDGER_TIMESPAN_SECONDS 4

// Maximum timeout for SCP consensus.
#define MAX_SCP_TIMEOUT_SECONDS 240

// Maximum time slip between nodes.
#define MAX_TIME_SLIP_SECONDS 60

// How many seconds of inactivity before evicting a node.
#define NODE_EXPIRATION_SECONDS 240

// How many ledger in past/future we consider an envelope viable.
#define LEDGER_VALIDITY_BRACKET 1000

namespace medida
{
class Meter;
class Counter;
}

namespace stellar
{
class Application;

using xdr::operator<;
using xdr::operator==;

/*
 * Drives the SCP protocol (is an SCP::Client). It is also incharge of
 * receiving transactions from the network.
 */
class HerderImpl : public Herder, public SCP
{
  public:
    HerderImpl(Application& app);
    ~HerderImpl();

    // Bootstraps the HerderImpl if we're creating a new Network
    void bootstrap() override;

    // SCP methods
    void validateValue(const uint64& slotIndex, const uint256& nodeID,
                       const Value& value, std::function<void(bool)> const& cb) override;
    int compareValues(const uint64& slotIndex, const uint32& ballotCounter,
                      const Value& v1, const Value& v2) override;

    void validateBallot(const uint64& slotIndex, const uint256& nodeID,
                        const SCPBallot& ballot,
                        std::function<void(bool)> const& cb) override;

    void ballotDidHearFromQuorum(const uint64& slotIndex,
                                 const SCPBallot& ballot) override;
    void valueExternalized(const uint64& slotIndex, const Value& value) override;

    void nodeTouched(const uint256& nodeID) override;

    void retrieveQuorumSet(const uint256& nodeID, const Hash& qSetHash,
                           std::function<void(const SCPQuorumSet&)> const& cb) override;
    void emitEnvelope(const SCPEnvelope& envelope) override;

    // Extra SCP methods overridden solely to increment metrics.
    void ballotDidPrepare(const uint64& slotIndex,
                          const SCPBallot& ballot) override;
    void ballotDidPrepared(const uint64& slotIndex,
                           const SCPBallot& ballot) override;
    void ballotDidCommit(const uint64& slotIndex,
                         const SCPBallot& ballot) override;
    void ballotDidCommitted(const uint64& slotIndex,
                            const SCPBallot& ballot) override;
    void envelopeSigned() override;
    void envelopeVerified(bool) override;

    // Herder methods
    TxSetFramePtr fetchTxSet(const uint256& txSetHash, bool askNetwork) override;
    void recvTxSet(TxSetFramePtr txSet) override;
    void doesntHaveTxSet(uint256 const& txSethash, PeerPtr peer) override;

    SCPQuorumSetPtr fetchSCPQuorumSet(const uint256& qSetHash, bool askNetwork) override;
    void recvSCPQuorumSet(SCPQuorumSetPtr qSet) override;
    void doesntHaveSCPQuorumSet(uint256 const& qSetHash, PeerPtr peer) override;

    // returns whether the transaction should be flooded
    bool recvTransaction(TransactionFramePtr tx) override;

    void recvSCPEnvelope(SCPEnvelope envelope,
                         std::function<void(EnvelopeState)> const& cb = [](bool)
                         {
    }) override;

    void ledgerClosed(LedgerHeaderHistoryEntry const& ledger) override;

    void triggerNextLedger() override;

    void dumpInfo(Json::Value& ret) override;

  private:
    void removeReceivedTx(TransactionFramePtr tx);
    void expireBallot(const uint64& slotIndex, const SCPBallot& ballot);

    void startRebroadcastTimer();
    void rebroadcast();

    // StellarBallot internal signature/verification
    void signStellarBallot(StellarBallot& b);
    bool verifyStellarBallot(const StellarBallot& b);

    void updateSCPCounters();

    // 0- tx we got during ledger close
    // 1- one ledger ago. rebroadcast
    // 2- two ledgers ago.
    std::vector<std::vector<TransactionFramePtr>> mReceivedTransactions;

    // Time of last access to a node, used to evict unused nodes.
    std::map<uint256, VirtualClock::time_point> mNodeLastAccess;

    // need to keep the old tx sets around for at least one Consensus round in
    // case some stragglers are still need the old txsets in order to close
    std::array<TxSetFetcher, 2> mTxSetFetcher;
    int mCurrentTxSetFetcher;
    std::map<Hash, std::vector<std::function<void(TxSetFramePtr)>>>
        mTxSetFetches;

    SCPQSetFetcher mSCPQSetFetcher;
    std::map<Hash, std::vector<std::function<void(SCPQuorumSetPtr)>>>
        mSCPQSetFetches;

    std::map<
        uint64,
        std::vector<std::pair<SCPEnvelope, std::function<void(EnvelopeState)>>>>
        mFutureEnvelopes;

    std::map<SCPBallot,
             std::map<uint256, std::vector<std::shared_ptr<VirtualTimer>>>>
        mBallotValidationTimers;

    LedgerHeaderHistoryEntry mLastClosedLedger;

    VirtualClock::time_point mLastTrigger;
    VirtualTimer mTriggerTimer;

    VirtualTimer mBumpTimer;
    VirtualTimer mRebroadcastTimer;
    Value mCurrentValue;
    StellarMessage mLastSentMessage;

    Application& mApp;

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

    medida::Counter& mNodeLastAccessSize;
    medida::Counter& mSCPQSetFetchesSize;
    medida::Counter& mFutureEnvelopesSize;
    medida::Counter& mBallotValidationTimersSize;

    // Counters for stuff in parent class (SCP)
    // that we monitor on a best-effort basis from
    // here.
    medida::Counter& mKnownNodesSize;
    medida::Counter& mKnownSlotsSize;

    // Counters for things reached-through the
    // SCP maps: Slots and Nodes
    medida::Counter& mCumulativeStatements;
    medida::Counter& mCumulativeCachedQuorumSets;
};
}
