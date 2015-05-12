#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>
#include <memory>
#include "herder/Herder.h"
#include "scp/SCP.h"
#include "util/Timer.h"
#include <overlay/ItemFetcher.h>
#include "PendingEnvelopes.h"

namespace medida
{
class Meter;
class Counter;
}

namespace stellar
{
class Application;
class LedgerManager;

using xdr::operator<;
using xdr::operator==;

/*
 * Drives the SCP protocol (is an SCP::Client). It is also in charge of
 * receiving transactions from the network.
 */
class HerderImpl : public Herder, public SCP
{
  public:
    HerderImpl(Application& app);
    ~HerderImpl();

    State getState() const override;
    std::string getStateHuman() const override;

    // Bootstraps the HerderImpl if we're creating a new Network
    void bootstrap() override;

    // SCP methods
    void validateValue(uint64 const& slotIndex, uint256 const& nodeID,
                       Value const& value,
                       std::function<void(bool)> const& cb) override;
    int compareValues(uint64 const& slotIndex, uint32 const& ballotCounter,
                      Value const& v1, Value const& v2) override;

    void validateBallot(uint64 const& slotIndex, uint256 const& nodeID,
                        SCPBallot const& ballot,
                        std::function<void(bool)> const& cb) override;
    void triggerAllBallotTimers(SCPBallot const& ballot);
    void startBallotTimer(uint64 const& slotIndex, uint256 const& nodeID,
        SCPBallot const& ballot,
        std::chrono::milliseconds timeout,
        std::function<void(bool)> const& cb);

    void ballotDidHearFromQuorum(uint64 const& slotIndex,
                                 SCPBallot const& ballot) override;
    void valueExternalized(uint64 const& slotIndex,
                           Value const& value) override;

    void nodeTouched(uint256 const& nodeID) override;

    void retrieveQuorumSet(
        uint256 const& nodeID, Hash const& qSetHash,
        std::function<void(SCPQuorumSet const&)> const& cb) override;
    void emitEnvelope(SCPEnvelope const& envelope) override;
    bool recvTransactions(TxSetFrame &txSet);
    // Extra SCP methods overridden solely to increment metrics.
    void ballotDidPrepare(uint64 const& slotIndex,
                          SCPBallot const& ballot) override;
    void ballotDidPrepared(uint64 const& slotIndex,
                           SCPBallot const& ballot) override;
    void ballotDidCommit(uint64 const& slotIndex,
                         SCPBallot const& ballot) override;
    void ballotDidCommitted(uint64 const& slotIndex,
                            SCPBallot const& ballot) override;
    void envelopeSigned() override;
    void envelopeVerified(bool) override;


    // returns whether the transaction should be flooded
    bool recvTransaction(TransactionFramePtr tx) override;

    void recvSCPEnvelope(SCPEnvelope envelope,
                         std::function<void(EnvelopeState)> const& cb = [](bool)
                         {
                         }) override;

    void processSCPQueue();

    uint32_t getCurrentLedgerSeq() const override;

    void triggerNextLedger(uint32_t ledgerSeqToTrigger) override;

    void dumpInfo(Json::Value& ret) override;

  private:
    void ledgerClosed();
    void removeReceivedTx(TransactionFramePtr tx);
    void expireBallot(uint64 const& slotIndex, SCPBallot const& ballot);

    void startRebroadcastTimer();
    void broadcast();

    // StellarBallot internal signature/verification
    void signStellarBallot(StellarBallot& b);
    bool verifyStellarBallot(StellarBallot const& b);

    void updateSCPCounters();

    void processSCPQueueAtIndex(uint64 slotIndex);
    
    // 0- tx we got during ledger close
    // 1- one ledger ago. rebroadcast
    // 2- two ledgers ago.
    std::vector<std::vector<TransactionFramePtr>> mReceivedTransactions;

    // Time of last access to a node, used to evict unused nodes.
    std::map<uint256, VirtualClock::time_point> mNodeLastAccess;

    PendingEnvelopes mPendingEnvelopes;

    std::map<SCPBallot,
             std::map<uint256, std::vector<std::shared_ptr<VirtualTimer>>>>
        mBallotValidationTimers;

    std::map<uint256, TxSetTrackerPtr> mProposedSetTrackers;

    void herderOutOfSync();

    struct ConsensusData
    {
        uint64 mConsensusIndex;
        StellarBallot mConsensusBallot;
        ConsensusData(uint64 index, StellarBallot const& b)
            : mConsensusIndex(index), mConsensusBallot(b)
        {
        }
    };

    // if the local instance is tracking the current state of SCP
    // herder keeps track of the consensus index and ballot
    // when not set, it just means that herder will try to snap to any slot that
    // reached consensus it can
    std::unique_ptr<ConsensusData> mTrackingSCP;

    // the ledger index that was last externalized
    uint32
    lastConsensusLedgerIndex() const
    {
        assert(mTrackingSCP->mConsensusIndex <= UINT32_MAX);
        return static_cast<uint32>(mTrackingSCP->mConsensusIndex);
    }

    // the ledger index that we expect to externalize next
    uint32
    nextConsensusLedgerIndex() const
    {
        return lastConsensusLedgerIndex() + 1;
    }

    // timer that detects that we're stuck on an SCP slot
    VirtualTimer mTrackingTimer;

    // called every time we get ledger externalized
    // ensures that if we don't hear from the network, we throw the herder into
    // indeterminate mode
    void trackingHeartBeat();

    VirtualClock::time_point mLastTrigger;
    VirtualTimer mTriggerTimer;

    VirtualTimer mBumpTimer;
    VirtualTimer mRebroadcastTimer;
    Value mCurrentValue;
    StellarMessage mLastSentMessage;

    Application& mApp;
    LedgerManager& mLedgerManager;

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
