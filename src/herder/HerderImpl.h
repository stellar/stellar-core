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
class HerderImpl : public Herder, public SCPDriver
{
    SCP mSCP;

  public:
    HerderImpl(Application& app);
    ~HerderImpl();

    State getState() const override;
    std::string getStateHuman() const override;

    // Bootstraps the HerderImpl if we're creating a new Network
    void bootstrap() override;

    // SCP methods

    void signEnvelope(SCPEnvelope& envelope) override;
    bool verifyEnvelope(SCPEnvelope const& envelope) override;

    bool validateValue(uint64 slotIndex, Value const& value) override;

    Value extractValidValue(uint64 slotIndex, Value const& value) override;

    std::string getValueString(Value const& v) const override;

    void ballotDidHearFromQuorum(uint64 slotIndex,
                                 SCPBallot const& ballot) override;

    void valueExternalized(uint64 slotIndex, Value const& value) override;

    void nominatingValue(uint64 slotIndex, Value const& value) override;

    Value combineCandidates(uint64 slotIndex,
                            std::set<Value> const& candidates) override;

    void setupTimer(uint64 slotIndex, int timerID,
                    std::chrono::milliseconds timeout,
                    std::function<void()> cb) override;

    void emitEnvelope(SCPEnvelope const& envelope) override;
    bool recvTransactions(TxSetFramePtr txSet);
    // Extra SCP methods overridden solely to increment metrics.
    void updatedCandidateValue(uint64 slotIndex, Value const& value) override;
    void startedBallotProtocol(uint64 slotIndex,
                               SCPBallot const& ballot) override;
    void acceptedBallotPrepared(uint64 slotIndex,
                                SCPBallot const& ballot) override;
    void confirmedBallotPrepared(uint64 slotIndex,
                                 SCPBallot const& ballot) override;
    void acceptedCommit(uint64 slotIndex, SCPBallot const& ballot) override;

    TransactionSubmitStatus recvTransaction(TransactionFramePtr tx) override;

    void recvSCPEnvelope(SCPEnvelope const& envelope) override;

    void recvSCPQuorumSet(Hash hash, const SCPQuorumSet& qset) override;
    void recvTxSet(Hash hash, const TxSetFrame& txset) override;
    void peerDoesntHave(MessageType type, uint256 const& itemID,
                        PeerPtr peer) override;
    TxSetFramePtr getTxSet(Hash hash) override;
    SCPQuorumSetPtr getQSet(const Hash& qSetHash) override;

    void processSCPQueue();

    uint32_t getCurrentLedgerSeq() const override;

    SequenceNumber getMaxSeqInPendingTxs(AccountID const&) override;

    void triggerNextLedger(uint32_t ledgerSeqToTrigger) override;

    void dumpInfo(Json::Value& ret) override;

  private:
    void ledgerClosed();
    void removeReceivedTx(TransactionFramePtr tx);
    void expireBallot(uint64 slotIndex, SCPBallot const& ballot);

    // returns true if upgrade is a valid upgrade step
    // in which case it also sets upgradeType
    bool validateUpgradeStep(uint64 slotIndex, UpgradeType const& upgrade,
                             LedgerUpgradeType& upgradeType);
    bool validateValueHelper(uint64 slotIndex, StellarValue const& sv);

    void startRebroadcastTimer();
    void rebroadcast();
    void broadcast(SCPEnvelope const& e);

    void updateSCPCounters();

    void processSCPQueueAtIndex(uint64 slotIndex);

    // returns true if the local instance is in a state compatible with
    // this slot
    bool isSlotCompatibleWithCurrentState(uint64 slotIndex);

    // 0- tx we got during ledger close
    // 1- one ledger ago. rebroadcast
    // 2- two ledgers ago.
    std::vector<std::vector<TransactionFramePtr>> mReceivedTransactions;

    PendingEnvelopes mPendingEnvelopes;

    std::map<SCPBallot,
             std::map<NodeID, std::vector<std::shared_ptr<VirtualTimer>>>>
        mBallotValidationTimers;

    void herderOutOfSync();

    struct ConsensusData
    {
        uint64 mConsensusIndex;
        StellarValue mConsensusValue;
        ConsensusData(uint64 index, StellarValue const& b)
            : mConsensusIndex(index), mConsensusValue(b)
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

    VirtualTimer mRebroadcastTimer;
    Value mCurrentValue;

    // timers used by SCP
    // indexed by slotIndex, timerID
    std::map<uint64, std::map<int, std::unique_ptr<VirtualTimer>>> mSCPTimers;

    Application& mApp;
    LedgerManager& mLedgerManager;

    struct SCPMetrics
    {
        medida::Meter& mValueValid;
        medida::Meter& mValueInvalid;
        medida::Meter& mNominatingValue;
        medida::Meter& mValueExternalize;

        medida::Meter& mUpdatedCandidate;
        medida::Meter& mStartBallotProtocol;
        medida::Meter& mAcceptedBallotPrepared;
        medida::Meter& mConfirmedBallotPrepared;
        medida::Meter& mAcceptedCommit;

        medida::Meter& mBallotValid;
        medida::Meter& mBallotInvalid;
        medida::Meter& mBallotSign;
        medida::Meter& mBallotValidSig;
        medida::Meter& mBallotInvalidSig;
        medida::Meter& mBallotExpire;

        medida::Meter& mQuorumHeard;
        medida::Meter& mQsetRetrieve;

        medida::Meter& mLostSync;

        medida::Meter& mEnvelopeEmit;
        medida::Meter& mEnvelopeReceive;
        medida::Meter& mEnvelopeSign;
        medida::Meter& mEnvelopeValidSig;
        medida::Meter& mEnvelopeInvalidSig;

        medida::Counter& mSCPQSetFetchesSize;
        medida::Counter& mBallotValidationTimersSize;

        // Counters for stuff in parent class (SCP)
        // that we monitor on a best-effort basis from
        // here.
        medida::Counter& mKnownSlotsSize;

        // Counters for things reached-through the
        // SCP maps: Slots and Nodes
        medida::Counter& mCumulativeStatements;
        medida::Counter& mCumulativeCachedQuorumSets;

        SCPMetrics(Application& app);
    };

    SCPMetrics mSCPMetrics;
};
}
