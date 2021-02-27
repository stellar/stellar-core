#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/HerderSCPDriver.h"
#include "herder/PendingEnvelopes.h"
#include "herder/TransactionQueue.h"
#include "herder/Upgrades.h"
#include "util/Timer.h"
#include "util/UnorderedMap.h"
#include "util/XDROperators.h"
#include <deque>
#include <memory>
#include <vector>

namespace medida
{
class Meter;
class Counter;
class Timer;
}

namespace stellar
{
class Application;
class LedgerManager;
class HerderSCPDriver;

/*
 * Is in charge of receiving transactions from the network.
 */
class HerderImpl : public Herder
{
  public:
    HerderImpl(Application& app);
    ~HerderImpl();

    State getState() const override;
    std::string getStateHuman() const override;

    void syncMetrics() override;

    // Bootstraps the HerderImpl if we're creating a new Network
    void bootstrap() override;
    void shutdown() override;

    void restoreState() override;

    SCP& getSCP();
    HerderSCPDriver&
    getHerderSCPDriver()
    {
        return mHerderSCPDriver;
    }

    void processExternalized(uint64 slotIndex, StellarValue const& value);
    void valueExternalized(uint64 slotIndex, StellarValue const& value);
    void emitEnvelope(SCPEnvelope const& envelope);

    TransactionQueue::AddResult
    recvTransaction(TransactionFrameBasePtr tx) override;

    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope) override;
    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                   const SCPQuorumSet& qset,
                                   TxSetFrame txset) override;

    void sendSCPStateToPeer(uint32 ledgerSeq, Peer::pointer peer) override;

    bool recvSCPQuorumSet(Hash const& hash, const SCPQuorumSet& qset) override;
    bool recvTxSet(Hash const& hash, const TxSetFrame& txset) override;
    void peerDoesntHave(MessageType type, uint256 const& itemID,
                        Peer::pointer peer) override;
    TxSetFramePtr getTxSet(Hash const& hash) override;
    SCPQuorumSetPtr getQSet(Hash const& qSetHash) override;

    void processSCPQueue();

    uint32_t getCurrentLedgerSeq() const override;
    uint32 getMinLedgerSeqToAskPeers() const override;

    SequenceNumber getMaxSeqInPendingTxs(AccountID const&) override;

    void triggerNextLedger(uint32_t ledgerSeqToTrigger,
                           bool checkTrackingSCP) override;

    void setInSyncAndTriggerNextLedger() override;

    void setUpgrades(Upgrades::UpgradeParameters const& upgrades) override;
    std::string getUpgradesJson() override;

    void forceSCPStateIntoSyncWithLastClosedLedger() override;

    bool resolveNodeID(std::string const& s, PublicKey& retKey) override;

    Json::Value getJsonInfo(size_t limit, bool fullKeys = false) override;
    Json::Value getJsonQuorumInfo(NodeID const& id, bool summary, bool fullKeys,
                                  uint64 index) override;
    Json::Value getJsonTransitiveQuorumIntersectionInfo(bool fullKeys) const;
    virtual Json::Value getJsonTransitiveQuorumInfo(NodeID const& id,
                                                    bool summary,
                                                    bool fullKeys) override;
    QuorumTracker::QuorumMap const& getCurrentlyTrackedQuorum() const override;

    virtual StellarValue
    makeStellarValue(Hash const& txSetHash, uint64_t closeTime,
                     xdr::xvector<UpgradeType, 6> const& upgrades,
                     SecretKey const& s) override;

#ifdef BUILD_TESTS
    // used for testing
    PendingEnvelopes& getPendingEnvelopes();

    TransactionQueue& getTransactionQueue();
#endif

    // helper function to verify envelopes are signed
    bool verifyEnvelope(SCPEnvelope const& envelope);
    // helper function to sign envelopes
    void signEnvelope(SecretKey const& s, SCPEnvelope& envelope);

    // helper function to verify SCPValues are signed
    bool verifyStellarValueSignature(StellarValue const& sv);

  private:
    // return true if values referenced by envelope have a valid close time:
    // * it's within the allowed range (using lcl if possible)
    // * it's recent enough (if `enforceRecent` is set)
    bool checkCloseTime(SCPEnvelope const& envelope, bool enforceRecent);

    // Given a candidate close time, determine an offset needed to make it
    // valid (at current system time). Returns 0 if ct is already valid
    std::chrono::milliseconds
    ctValidityOffset(uint64_t ct, std::chrono::milliseconds maxCtOffset =
                                      std::chrono::milliseconds::zero());

    void ledgerClosed(bool synchronous);

    void maybeTriggerNextLedger(bool synchronous);

    void startOutOfSyncTimer();
    void outOfSyncRecovery();
    void broadcast(SCPEnvelope const& e);

    void processSCPQueueUpToIndex(uint64 slotIndex);

    TransactionQueue mTransactionQueue;

    void
    updateTransactionQueue(std::vector<TransactionFrameBasePtr> const& applied);

    PendingEnvelopes mPendingEnvelopes;
    Upgrades mUpgrades;
    HerderSCPDriver mHerderSCPDriver;

    void herderOutOfSync();

    // attempt to retrieve additional SCP messages from peers
    void getMoreSCPState();

    // last slot that was persisted into the database
    // keep track of all messages for MAX_SLOTS_TO_REMEMBER slots
    uint64 mLastSlotSaved;

    // timer that detects that we're stuck on an SCP slot
    VirtualTimer mTrackingTimer;

    // tracks the last time externalize was called
    VirtualClock::time_point mLastExternalize;

    // saves the SCP messages that the instance sent out last
    void persistSCPState(uint64 slot);
    // restores SCP state based on the last messages saved on disk
    void restoreSCPState();

    // saves upgrade parameters
    void persistUpgrades();
    void restoreUpgrades();

    // called every time we get ledger externalized
    // ensures that if we don't hear from the network, we throw the herder into
    // indeterminate mode
    void trackingHeartBeat();

    VirtualTimer mTriggerTimer;

    VirtualTimer mOutOfSyncTimer;

    Application& mApp;
    LedgerManager& mLedgerManager;

    struct SCPMetrics
    {
        medida::Meter& mLostSync;

        medida::Meter& mEnvelopeEmit;
        medida::Meter& mEnvelopeReceive;

        // Counters for things reached-through the
        // SCP maps: Slots and Nodes
        medida::Counter& mCumulativeStatements;

        // envelope signature verification
        medida::Meter& mEnvelopeValidSig;
        medida::Meter& mEnvelopeInvalidSig;

        SCPMetrics(Application& app);
    };

    SCPMetrics mSCPMetrics;

    // Check that the quorum map intersection state is up to date, and if not
    // run a background job that re-analyzes the current quorum map.
    void checkAndMaybeReanalyzeQuorumMap();

    // erase all data for ledgers strictly less than ledgerSeq
    void eraseBelow(uint32 ledgerSeq);

    struct QuorumMapIntersectionState
    {
        uint32_t mLastCheckLedger{0};
        uint32_t mLastGoodLedger{0};
        size_t mNumNodes{0};
        Hash mLastCheckQuorumMapHash{};
        Hash mCheckingQuorumMapHash{};
        bool mRecalculating{false};
        std::atomic<bool> mInterruptFlag{false};
        std::pair<std::vector<PublicKey>, std::vector<PublicKey>>
            mPotentialSplit{};
        std::set<std::set<PublicKey>> mIntersectionCriticalNodes{};

        bool
        hasAnyResults() const
        {
            return mLastGoodLedger != 0;
        }

        bool
        enjoysQuorunIntersection() const
        {
            return mLastCheckLedger == mLastGoodLedger;
        }
    };
    QuorumMapIntersectionState mLastQuorumMapIntersectionState;

    uint32_t getMinLedgerSeqToRemember() const;
};
}
