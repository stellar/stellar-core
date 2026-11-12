// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/Herder.h"
#include "herder/HerderSCPDriver.h"
#include "herder/LedgerCloseData.h"
#include "herder/PendingEnvelopes.h"
#include "herder/QuorumIntersectionChecker.h"
#include "herder/TxSetFrame.h"
#include "herder/Upgrades.h"
#include "ledger/LedgerHeaderUtils.h"
#include "overlay/NetworkConstants.h"
#include "util/Timer.h"
#include "util/UnorderedMap.h"
#include "util/XDROperators.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
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
    struct ConsensusData
    {
        uint64_t mConsensusIndex{0};
        ConsensusTime mConsensusCloseTime;
    };

    void setTrackingSCPState(uint64_t index, StellarValue const& value,
                             bool isTrackingNetwork) override;

    // returns the latest known ledger from the network, requires Herder to be
    // in fully booted state
    uint32 trackingConsensusLedgerIndex() const override;

    ConsensusTime trackingConsensusCloseTime() const;

    // the ledger index that we expect to externalize next
    uint32
    nextConsensusLedgerIndex() const
    {
        return trackingConsensusLedgerIndex() + 1;
    }

    void lostSync();

    // A transaction accepted from a client blocks its source account on this
    // node for this long at most (the overlay stops sending it after the
    // same time, see LOCAL_TX_MAX_AGE in overlay/src/flood/tx_router.rs).
    static constexpr std::chrono::seconds LOCAL_TX_PENDING_TIMEOUT{300};

    // THE place that decides which nodes get the transactions this node
    // accepts. Returns the validators to send them to, or nullopt for
    // "every peer" (the flood). With the leader schedule: the leaders of the
    // node's current round and of the next one for the slot after the LCL,
    // and, when the slot after that starts a new leader term, its expected
    // first leader.
    // Everyone if the node is not tracking or not synced, if there is no
    // schedule, or if the next ledger uses a legacy tx set (which every
    // validator builds for itself).
    std::optional<std::vector<NodeID>> computeTxFloodTargets();
    // Recomputes the targets and tells the overlay if they changed. Called
    // when a ledger closes, when SCP enters a round, and when sync is lost.
    void updateTxFloodTargets();
#ifdef BUILD_TESTS
    // What the overlay was last told (nullopt = every peer).
    std::optional<std::vector<NodeID>> const&
    getTxFloodTargetsForTesting() const
    {
        return mTxFloodTargets;
    }
    // Every change of the targets sent to the overlay, with all the target
    // function had to go on: the LCL at that time (the schedule of `slot`,
    // the slot after it, is a function of its header, and the seeded order
    // of the slot after that a function of its hash) and the node's round
    // in `slot` (0 = not started).
    struct TxFloodTargetUpdate
    {
        uint64_t slot;
        uint32_t round;
        std::optional<std::vector<NodeID>> targets;
        LedgerHeaderHistoryEntry lcl;
    };
    // `targets` with the node's present state.
    TxFloodTargetUpdate describeTxFloodTargetsForTesting(
        std::optional<std::vector<NodeID>> targets);
    std::deque<TxFloodTargetUpdate> mTxFloodTargetHistoryForTesting;
#endif

    HerderImpl(Application& app);
    ~HerderImpl();

    State getState() const override;
    std::string getStateHuman(State st) const override;

    void syncMetrics() override;

    // Bootstraps the HerderImpl if we're creating a new Network
    void bootstrap() override;
    void shutdown() override;

    void start() override;

    void lastClosedLedgerIncreased(bool latest, TxSetXDRFrameConstPtr txSet,
                                   bool queueRebuildNeeded) override;

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

    // Called with every statement this node emits. If the statement votes
    // for a value for the ledger after the LCL, hands that ledger to the
    // apply thread for speculative execution (see
    // LedgerManager::startSpeculativeApply), unless it was already.
    void maybeStartSpeculativeApply(SCPEnvelope const& envelope);

#ifdef BUILD_TESTS
    TxSubmitStatus recvTransaction(TransactionFrameBasePtr tx,
                                   bool submittedFromSelf,
                                   bool isLoadgenTx = false) override;
#else
    TxSubmitStatus recvTransaction(TransactionFrameBasePtr tx,
                                   bool submittedFromSelf) override;
#endif

    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope) override;
#ifdef BUILD_TESTS
    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                   const SCPQuorumSet& qset,
                                   TxSetXDRFrameConstPtr txset) override;
    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                   SCPQuorumSet const& qset,
                                   StellarMessage const& txset) override;

    void externalizeValue(TxSetXDRFrameConstPtr txSet, uint32_t ledgerSeq,
                          ConsensusTime closeTime,
                          xdr::xvector<UpgradeType, 6> const& upgrades,
                          std::optional<SecretKey> skToSignValue) override;

    VirtualTimer const&
    getTriggerTimer() const override
    {
        return mTriggerTimer;
    }

    uint32_t mTriggerNextLedgerSeq{0};

    // Deterministic mempool input for scheduling tests. Construction and SCP
    // still run through their production paths.
    std::function<std::vector<TransactionEnvelope>(size_t)>
        mGetTopTransactionsForTesting;
    friend class LeaderBuildTestAccess;
    friend class ProposalPrevalidationTestAccess;

    std::optional<uint32_t> mMaxClassicTxSize;
    std::optional<uint32_t> mMaxTxSizeOverride;
    void
    setMaxClassicTxSize(uint32 bytes) override
    {
        mMaxClassicTxSize = std::make_optional<uint32_t>(bytes);
    }
    void
    setMaxTxSize(uint32 bytes) override
    {
        mMaxTxSizeOverride = bytes;
    }
    std::optional<uint32_t> mFlowControlExtraBuffer;
    void
    setFlowControlExtraBufferSize(uint32 bytes) override
    {
        mFlowControlExtraBuffer = std::make_optional<uint32_t>(bytes);
    }

    // When set, this node nominates the tx set returned by this function
    // (given the LCL) instead of the one it builds.
    std::function<TxSetXDRFrameConstPtr(LedgerHeaderHistoryEntry const&)>
        mProposedTxSetOverride;
#endif
    std::vector<SCPEnvelope> getSCPStateForPeer(uint32 ledgerSeq) override;

    bool recvSCPQuorumSet(Hash const& hash, SCPQuorumSet const& qset) override;
    bool recvTxSet(Hash const& hash, TxSetXDRFrameConstPtr txset) override;
    void txSetStoredByOverlay(Hash const& hash) override;
    TxSetResult getTxSet(Hash const& hash) override;
    SCPQuorumSetPtr getQSet(Hash const& qSetHash) override;

    // process ready SCP messages. This may trigger the node to externalze new
    // slots. Use `synchronous=false` to avoid arbitrary delays of closing many
    // ledgers at once.
    void processSCPQueue(bool synchronous);

    uint32_t getMaxClassicTxSize() const override;
    uint32_t getFlowControlExtraBuffer() const override;

    uint32_t
    getMaxTxSize() const override
    {
#ifdef BUILD_TESTS
        if (mMaxTxSizeOverride)
        {
            return *mMaxTxSizeOverride;
        }
#endif
        return mMaxTxSize;
    }

    uint32 getMinLedgerSeqToAskPeers() const override;

    uint32_t getMinLedgerSeqToRemember() const override;

    bool isNewerStatement(SCPStatement const& oldSt,
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
    makeStellarValue(Hash const& txSetHash, ConsensusTime closeTime,
                     xdr::xvector<UpgradeType, 6> const& upgrades,
                     SecretKey const& s) override;

    void startTxSetGCTimer();

#ifdef BUILD_TESTS
    PendingEnvelopes& getPendingEnvelopes();
    Upgrades const& getUpgrades() const;
#endif

    // helper function to verify envelopes are signed
    bool verifyEnvelope(SCPEnvelope const& envelope);
    // helper function to sign envelopes
    void signEnvelope(SecretKey const& s, SCPEnvelope& envelope);

    // helper function to verify SCPValues signatures
    bool verifyStellarValueSignature(StellarValue const& sv);

    void maybeHandleUpgrade() override;

  private:
    // return true if values referenced by envelope have a valid close time:
    // * it's within the allowed range (using lcl if possible)
    // * it's recent enough (if `enforceRecent` is set)
    bool checkCloseTime(SCPEnvelope const& envelope, bool enforceRecent);

    // Given a candidate close time, determine an offset needed to make it
    // valid (at current system time). Returns 0 if ct is already valid
    std::chrono::milliseconds
    ctValidityOffset(ConsensusTime closeTime,
                     std::chrono::milliseconds maxCtOffset =
                         std::chrono::milliseconds::zero());

    void setupTriggerNextLedger();

    struct PreparedTxSet
    {
        Hash previousLedgerHash;
        uint32_t ledgerSeq;
        ConsensusTime closeTime;
        TxSetXDRFrameConstPtr txSet;
        ApplicableTxSetFrameConstPtr applicableTxSet;
        std::vector<Hash> invalidTxHashes;
    };
    // How long before the trigger the first-round leader builds its set, so
    // that the set is built and on its way to the peers when consensus
    // starts; what arrives in between waits for the next ledger.
    //  * At least a quarter of the target close time, but no more than
    //    250 ms of a long one.
    //  * Long enough for the build this node expects (`expectedBuild`, see
    //    expectedTxSetBuildDuration; with a quarter to spare) plus the time
    //    the pushed set needs to reach the peers.
    //  * Never more than half of the target close time.
    static std::chrono::milliseconds
    prepareTxSetLeadTime(std::chrono::milliseconds expectedCloseTime,
                         std::chrono::milliseconds expectedBuild);
    // How long this node's latest tx set build took (zero before the first),
    // what it cost per transaction of the set it built (zero until it has
    // built a set of some size), and how many transactions the last closed
    // ledger's set held.
    std::chrono::milliseconds mLastTxSetBuildDuration{0};
    std::chrono::nanoseconds mTxSetBuildCostPerTx{0};
    size_t mLastClosedTxSetSize{0};
    // What the next build is expected to take: a build like this node's last
    // one, or what a set the size of the last closed ledger's costs at this
    // node's rate, whichever is longer. The second matters for a node that
    // has not built for a while: its last build may be from before the load
    // rose (every node's first term under load starts that way), and a lead
    // time sized for it would make the first ledger of the term late.
    std::chrono::milliseconds expectedTxSetBuildDuration() const;
    std::optional<PreparedTxSet> mPreparedTxSet;
    // `pushToPeers`: this is the set the node is about to propose as a round
    // leader, so the overlay sends it to every peer as soon as its XDR exists
    // instead of waiting for their requests (see pushTxSetToPeers).
    PreparedTxSet buildTxSet(uint32_t ledgerSeq, ConsensusTime closeTime,
                             bool pushToPeers);
    // Leader's tx-set push: ask the overlay to send a set it already holds
    // (cacheTxSet) to every connected peer. Peers use a pushed set only if
    // an SCP statement names its hash; one that missed it requests it.
    void pushTxSetToPeers(TxSetXDRFrameConstPtr const& txSet,
                          uint32_t ledgerSeq);
    void prepareTxSet(uint32_t ledgerSeq, ConsensusTime closeTime);
    void discardPreparedTxSet();
    ValueWrapperPtr makeProposalValue(uint32_t ledgerSeq);
    // Whether a ledger closing on an LCL with this protocol uses a legacy
    // (non-generalized) tx set, which every validator builds locally.
    static bool buildsLegacyTxSet(uint32_t lclLedgerVersion);
    // Upper bound on the transactions a tx set for the next ledger can hold.
    size_t maxTxSetCandidates() const;
    // How many valid candidates a build validates per phase (classic, then
    // Soroban if the protocol supports it): 5/4 of the phase's capacity.
    std::vector<size_t> validTxTargets(uint32_t lclLedgerVersion) const;
    std::vector<TransactionEnvelope> pullTopTransactions(size_t count);
    // The same pull as encoded by the overlay (see OverlayIPC), to decode
    // off the main thread.
    std::vector<uint8_t> pullTopTransactionsPayload(size_t count);

    // A signature prewarm: background tasks verifying the signatures of the
    // candidates that the build of `ledgerSeq` validates first (and that the
    // leader's set for it is selected from), warming the signature cache for
    // that build and for validating the leader's set.
    struct SignaturePrewarm
    {
        uint32_t ledgerSeq{0};
        std::chrono::steady_clock::time_point start;
        std::atomic<bool> stop{false};
        // Decoded and selected once, by whichever task gets there first.
        std::once_flag selected;
        std::vector<TransactionEnvelope> candidates;
        std::atomic<size_t> checkedTxs{0};
        std::atomic<size_t> verifiedSignatures{0};
        std::atomic<size_t> runningTasks{0};
        // Nanoseconds from `start` until the last task finished, or -1.
        std::atomic<int64_t> finishedAfterNs{-1};
    };
    // Starts a prewarm for ledger `ledgerSeq`, replacing any running one.
    // `pendingPerPhase` counts the transactions per phase that are still in
    // the mempool but not candidates for `ledgerSeq` (those of the value being
    // externalized): the prewarm looks past that many more.
    void prewarmSignatureCache(uint32_t ledgerSeq,
                               std::vector<size_t> const& pendingPerPhase);
    std::shared_ptr<SignaturePrewarm> mSignaturePrewarm;

    // Pre-validation of the next ledger's candidates.
    //
    // A node that expects to lead ledger N+1 (in round 1, or in round 2 if
    // the first leader times out) starts validating its candidates as soon as
    // it has executed ledger N speculatively, while N is still going through
    // consensus: background tasks run `checkValid` on the mempool's top
    // transactions against the state that execution left (see
    // ExecutedLedgerState). When N has closed with the value that was
    // executed, the build of N+1 takes those results instead of validating
    // the same transactions on the main thread (see mValidationCache).
    struct ProposalPrevalidation
    {
        // The executed ledger (N) and its value: the results hold on top of
        // N if and only if N closes with this value.
        uint32_t ledgerSeq{0};
        StellarValue value;
        std::chrono::steady_clock::time_point start;
        std::atomic<bool> stop{false};
        // Decoded and selected once, by whichever task gets there first.
        std::once_flag selected;
        std::vector<TransactionFrameBasePtr> candidates;
        // Guards everything below. Tasks hand over their results in small
        // batches, so whoever stops the job takes what is there and never
        // waits for a task: nothing is published after `stop` is set.
        std::mutex mutex;
        std::vector<std::pair<Hash, TxValidationResult>> published;
        size_t pendingTasks{0};
        size_t activeTasks{0};
        // Nanoseconds from `start` until the last task finished, or -1.
        std::atomic<int64_t> finishedAfterNs{-1};
    };
    std::shared_ptr<ProposalPrevalidation> mProposalPrevalidation;
    // Whether the local node would lead round 1 or 2 of the ledger after
    // `slotIndex` if `slotIndex` closes with `value`.
    bool mayLeadAfter(uint64_t slotIndex, StellarValue const& value,
                      bool appliedTransactions);
    // Called on the main thread when the speculative execution of a ledger
    // has finished (`state` is null if it was not executed).
    void startProposalPrevalidation(
        std::shared_ptr<ExecutedLedgerState const> state);
    // Stops the running pre-validation, if any. With `lcl`, its results are
    // moved into mValidationCache if they hold on top of `lcl`.
    void stopProposalPrevalidation(LedgerHeaderHistoryEntry const* lcl);

    // Validation results for candidates of the next ledger, computed against
    // the last closed ledger `lclHash`: by the pre-validation above, and by
    // every build on that ledger. A later build on the same ledger (the build
    // at the trigger after an early one, a later round's) only validates what
    // it has not seen.
    struct ValidationCache
    {
        Hash lclHash;
        TxValidationCache results;
    };
    ValidationCache mValidationCache;
    // The cache for a build on top of `lcl`, emptied if it was for another
    // ledger and topped up with the pre-validation's results.
    TxValidationCache* validationCacheFor(LedgerHeaderHistoryEntry const& lcl);

    // Tx sets the overlay holds on stable storage: received from it, or
    // handed to it after building them. persistSCPState leaves those out of
    // the database; after a restart they come back from the overlay (see
    // restoreSCPState).
    UnorderedSet<Hash> mTxSetsStoredByOverlay;

    // The (slot, value) last handed to the ledger apply thread for speculative
    // execution, so that further statements for the same value don't restart
    // it.
    std::optional<std::pair<uint64, Value>> mSpeculativeApplyValue;

  public:
    // Called when slot `slotIndex` starts its ballot protocol with a value
    // whose tx set is `txSetHash`: prewarms the next ledger's candidates while
    // this slot finishes balloting, externalizes and closes.
    void prewarmNextLedgerSignatures(uint64_t slotIndex, Hash const& txSetHash);
    // Stops a running signature prewarm and records its metrics: called when
    // validation or a local build is about to verify the signatures itself.
    void stopSignaturePrewarm();

  private:
    // Compute the trigger-timer anchor point using the local node's
    // prepare-start timestamp for the previous slot. Returns a pessimistic
    // estimate (now - expectedClose) if no prepare-start is recorded.
    VirtualClock::time_point
    triggerAnchorFromPrepareStart(uint64_t lastIndex,
                                  VirtualClock::time_point now,
                                  std::chrono::milliseconds expectedClose);

    // Compute the trigger-timer anchor point using the network-agreed
    // consensus close time on the system clock. Falls back to
    // triggerAnchorFromPrepareStart if consensus close time is unavailable
    // or if the local clock is significantly drifting from the network time.
    VirtualClock::time_point triggerAnchorFromConsensusCloseTime(
        uint64_t lastIndex, VirtualClock::time_point now,
        std::chrono::milliseconds expectedClose);

    void startOutOfSyncTimer();
    void outOfSyncRecovery();
    void broadcast(SCPEnvelope const& e);

    void processSCPQueueUpToIndex(uint64 slotIndex);
    // While out of sync, step over slots for which no SCP messages exist to
    // the ready slots beyond them; see the definition for why.
    void processReadySlotsPastGaps();
    void newSlotExternalized(StellarValue const& value);
    void purgeOldSlots();
    void purgeOldPersistedTxSets();
    void writeDebugTxSet(LedgerCloseData const& lcd);

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
    void fetchRestoredTxSets(SCPEnvelope const& envelope);

    // Map SCP slots to local time of nomination and the time slot was
    // externalized by the network
    std::map<uint32_t, std::pair<ConsensusTime, std::optional<ConsensusTime>>>
        mDriftCTSlidingWindow;

    // saves upgrade parameters
    void persistUpgrades();
    void restoreUpgrades();

    // called every time we get ledger externalized
    // ensures that if we don't hear from the network, we throw the herder into
    // indeterminate mode
    void trackingHeartBeat();

    VirtualTimer mTriggerTimer;
    VirtualTimer mPrepareTxSetTimer;

    // What updateTxFloodTargets last told the overlay (nullopt = everyone,
    // which is also what the overlay starts with).
    std::optional<std::vector<NodeID>> mTxFloodTargets;

    // Transactions this node accepted from clients and has not seen
    // externalized, by source account: one per account at a time. Not used
    // in overlay-only mode.
    struct LocalPendingTx
    {
        Hash fullHash;
        SequenceNumber seqNum;
        VirtualClock::time_point acceptedAt;
        // The ledger whose set uses up its sequence number, once such a set
        // externalized (0 before); the record goes when that ledger is
        // closed.
        uint32_t externalizedIn;
    };
    UnorderedMap<AccountID, LocalPendingTx> mLocalPendingTxs;
    // The checks a transaction passes before this node takes it and sends it
    // on: one pending transaction per source account, and checkValid
    // against the LCL (skipped if `skipValidityCheck`).
    TxSubmitStatus checkTxAdmission(TransactionFrameBasePtr const& tx,
                                    bool skipValidityCheck);
    // Drops the records of accounts whose transaction externalized in
    // `txSet` or timed out; returns the hashes of pending transactions that
    // can no longer apply (another transaction of the account used the
    // sequence number), for the overlay to drop.
    std::vector<Hash> settleLocalPendingTxs(TxSetXDRFrame const& txSet,
                                            uint32_t txSetLedgerSeq);
    // Drops the records settleLocalPendingTxs marked, once the ledger that
    // settled them is the LCL.
    void forgetSettledLocalPendingTxs();
    // Drops the records of these transactions (removed from the mempool
    // because they failed validation in this node's own build).
    void forgetLocalPendingTxs(std::vector<Hash> const& txHashes);

    VirtualTimer mOutOfSyncTimer;

    VirtualTimer mTxSetGarbageCollectTimer;

    // Every CHECK_FOR_DEAD_NODES_MINUTES, we keep track of all nodes that SCP
    // reports as missing throughout the interval.
    VirtualTimer mCheckForDeadNodesTimer;
    void startCheckForDeadNodesInterval();

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

        // Marked when the trigger timer falls back from the
        // network-close-time anchor to the local prepare-start anchor.
        medida::Meter& mTriggerPrepareStartFallback;

        // Time spent building the tx set proposed at nomination.
        medida::Timer& mTxSetBuild;

        // How long after its scheduled time the trigger timer callback ran
        // (main-thread congestion)
        medida::Timer& mTriggerLateness;

        // Marked when the next trigger is already due when it is scheduled,
        // i.e. the node is behind the target block time
        medida::Meter& mTriggerImmediate;

        // Time spent in persistSCPState, which runs on the main thread in
        // front of every SCP statement this node broadcasts.
        medida::Timer& mPersistSCPState;

        SCPMetrics(Application& app);
    };

    SCPMetrics mSCPMetrics;

    // Check that the quorum map intersection state is up to date, and if not
    // run a background job that re-analyzes the current quorum map.
    void checkAndMaybeReanalyzeQuorumMap();

    void checkAndMaybeReanalyzeQuorumMapV2();

    // erase all data for ledgers outside the range [minSlot, maxSlot].
    // Either bound may be nullopt to skip that direction.
    // Always preserves the first ledger on the current checkpoint so peers
    // can catchup without waiting for the next checkpoint.
    void eraseOutsideRange(std::optional<uint32> minSlot,
                           std::optional<uint32> maxSlot);

    std::shared_ptr<QuorumMapIntersectionState> mLastQuorumMapIntersectionState;

    State mState;
    void setState(State st);

    // Information about the most recent tracked SCP slot
    // Set regardless of whether the local instance if fully in sync with the
    // network or not (Herder::State is used to properly track the state of
    // Herder) On startup, this variable is set to LCL
    ConsensusData mTrackingSCP;

    uint32_t mMaxTxSize{0};

    UnorderedSet<LedgerKey>
    recomputeKeysToFilter(uint32_t protocolVersion) const;
};
}
