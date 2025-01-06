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
constexpr uint32 const SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER = 2;

class Application;
class LedgerManager;
class HerderSCPDriver;

/*
 * Is in charge of receiving transactions from the network.
 */
class HerderImpl : public Herder
{
  public:
    struct ConsensusData
    {
        uint64_t mConsensusIndex{0};
        TimePoint mConsensusCloseTime{0};
    };

    void setTrackingSCPState(uint64_t index, StellarValue const& value,
                             bool isTrackingNetwork) override;

    // returns the latest known ledger from the network, requires Herder to be
    // in fully booted state
    uint32 trackingConsensusLedgerIndex() const override;

    TimePoint trackingConsensusCloseTime() const;

    // the ledger index that we expect to externalize next
    uint32
    nextConsensusLedgerIndex() const
    {
        return trackingConsensusLedgerIndex() + 1;
    }

    void lostSync();

    HerderImpl(Application& app);
    ~HerderImpl();

    State getState() const override;
    std::string getStateHuman(State st) const override;

    void syncMetrics() override;

    // Bootstraps the HerderImpl if we're creating a new Network
    void bootstrap() override;
    void shutdown() override;

    void start() override;

    void lastClosedLedgerIncreased(bool latest,
                                   TxSetXDRFrameConstPtr txSet) override;

    SCP& getSCP();
    HerderSCPDriver&
    getHerderSCPDriver()
    {
        return mHerderSCPDriver;
    }

    bool
    isTracking() const override
    {
        return mState == State::HERDER_TRACKING_NETWORK_STATE;
    }

    void processExternalized(uint64 slotIndex, StellarValue const& value,
                             bool isLatestSlot);
    void valueExternalized(uint64 slotIndex, StellarValue const& value,
                           bool isLatestSlot);
    void emitEnvelope(SCPEnvelope const& envelope);

    TransactionQueue::AddResult
    recvTransaction(TransactionFrameBasePtr tx,
                    bool submittedFromSelf) override;

    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope) override;
#ifdef BUILD_TESTS
    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                   const SCPQuorumSet& qset,
                                   TxSetXDRFrameConstPtr txset) override;
    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                   const SCPQuorumSet& qset,
                                   StellarMessage const& txset) override;

    void externalizeValue(TxSetXDRFrameConstPtr txSet, uint32_t ledgerSeq,
                          uint64_t closeTime,
                          xdr::xvector<UpgradeType, 6> const& upgrades,
                          std::optional<SecretKey> skToSignValue) override;

    VirtualTimer const&
    getTriggerTimer() const override
    {
        return mTriggerTimer;
    }

    uint32_t mTriggerNextLedgerSeq{0};

    std::optional<uint32_t> mMaxClassicTxSize;
    void
    setMaxClassicTxSize(uint32 bytes) override
    {
        mMaxClassicTxSize = std::make_optional<uint32_t>(bytes);
    }
    void
    setMaxTxSize(uint32 bytes) override
    {
        mMaxTxSize = bytes;
    }
    std::optional<uint32_t> mFlowControlExtraBuffer;
    void
    setFlowControlExtraBufferSize(uint32 bytes) override
    {
        mFlowControlExtraBuffer = std::make_optional<uint32_t>(bytes);
    }
#endif
    void sendSCPStateToPeer(uint32 ledgerSeq, Peer::pointer peer) override;

    bool recvSCPQuorumSet(Hash const& hash, const SCPQuorumSet& qset) override;
    bool recvTxSet(Hash const& hash, TxSetXDRFrameConstPtr txset) override;
    void peerDoesntHave(MessageType type, uint256 const& itemID,
                        Peer::pointer peer) override;
    TxSetXDRFrameConstPtr getTxSet(Hash const& hash) override;
    SCPQuorumSetPtr getQSet(Hash const& qSetHash) override;

    void processSCPQueue();

    uint32_t getMaxClassicTxSize() const override;
    uint32_t getFlowControlExtraBuffer() const override;

    uint32_t
    getMaxTxSize() const override
    {
        return mMaxTxSize;
    }

    uint32 getMinLedgerSeqToAskPeers() const override;

    uint32_t getMinLedgerSeqToRemember() const override;

    bool isNewerNominationOrBallotSt(SCPStatement const& oldSt,
                                     SCPStatement const& newSt) override;

    uint32_t getMostRecentCheckpointSeq() override;

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

    virtual void beginApply() override;

    void startTxSetGCTimer();

#ifdef BUILD_TESTS
    // used for testing
    PendingEnvelopes& getPendingEnvelopes();

    ClassicTransactionQueue& getTransactionQueue() override;
    SorobanTransactionQueue& getSorobanTransactionQueue() override;
    bool sourceAccountPending(AccountID const& accountID) const override;
#endif

    // helper function to verify envelopes are signed
    bool verifyEnvelope(SCPEnvelope const& envelope);
    // helper function to sign envelopes
    void signEnvelope(SecretKey const& s, SCPEnvelope& envelope);

    // helper function to verify SCPValues are signed
    bool verifyStellarValueSignature(StellarValue const& sv);

    size_t getMaxQueueSizeOps() const override;
    size_t getMaxQueueSizeSorobanOps() const override;
    void maybeHandleUpgrade() override;

    bool isBannedTx(Hash const& hash) const override;
    TransactionFrameBaseConstPtr getTx(Hash const& hash) const override;

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

    void setupTriggerNextLedger();

    void startOutOfSyncTimer();
    void outOfSyncRecovery();
    void broadcast(SCPEnvelope const& e);

    void processSCPQueueUpToIndex(uint64 slotIndex);
    void safelyProcessSCPQueue(bool synchronous);
    void newSlotExternalized(bool synchronous, StellarValue const& value);
    void purgeOldPersistedTxSets();
    void writeDebugTxSet(LedgerCloseData const& lcd);

    ClassicTransactionQueue mTransactionQueue;
    std::unique_ptr<SorobanTransactionQueue> mSorobanTransactionQueue;

    void updateTransactionQueue(TxSetXDRFrameConstPtr txSet);
    void maybeSetupSorobanQueue(uint32_t protocolVersion);

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

    // Map SCP slots to local time of nomination and the time slot was
    // externalized by the network
    std::map<uint32_t, std::pair<uint64_t, std::optional<uint64_t>>>
        mDriftCTSlidingWindow;

    // saves upgrade parameters
    void persistUpgrades();
    void restoreUpgrades();

    // called every time we get ledger externalized
    // ensures that if we don't hear from the network, we throw the herder into
    // indeterminate mode
    void trackingHeartBeat();

    VirtualTimer mTriggerTimer;

    VirtualTimer mOutOfSyncTimer;

    VirtualTimer mTxSetGarbageCollectTimer;

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

    // erase all data for ledgers strictly less than ledgerSeq except for the
    // first ledger on the current checkpoint. Hold onto this ledger so
    // peers can catchup without waiting for the next checkpoint.
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

    State mState;
    void setState(State st);

    // Information about the most recent tracked SCP slot
    // Set regardless of whether the local instance if fully in sync with the
    // network or not (Herder::State is used to properly track the state of
    // Herder) On startup, this variable is set to LCL
    ConsensusData mTrackingSCP;

    uint32_t mMaxTxSize{0};
};
}
