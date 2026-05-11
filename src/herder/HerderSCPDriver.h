// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "herder/TxSetUtils.h"
#include "medida/timer.h"
#include "scp/SCPDriver.h"
#include "util/ProtocolVersion.h"
#include "util/RandomEvictionCache.h"
#include "xdr/Stellar-ledger.h"
#include <optional>

namespace medida
{
class Counter;
class Meter;
class Histogram;
}

namespace stellar
{
class Application;
class HerderImpl;
class LedgerManager;
class PendingEnvelopes;
class SCP;
class Upgrades;
class VirtualTimer;
struct StellarValue;
struct SCPEnvelope;

// First protocol version supporting the application-specific weight function
// for SCP leader election.
ProtocolVersion constexpr APPLICATION_SPECIFIC_NOMINATION_LEADER_ELECTION_PROTOCOL_VERSION =
    ProtocolVersion::V_22;

class HerderSCPDriver : public SCPDriver
{
  public:
    HerderSCPDriver(Application& app, HerderImpl& herder,
                    Upgrades const& upgrades,
                    PendingEnvelopes& pendingEnvelopes);
    ~HerderSCPDriver();

    void bootstrap();
    void stateChanged();

    SCP&
    getSCP()
    {
        return mSCP;
    }

    void recordSCPExecutionMetrics(uint64_t slotIndex);
    void recordSCPEvent(uint64_t slotIndex, bool isNomination);
    void recordSCPExternalizeEvent(uint64_t slotIndex, NodeID const& id,
                                   bool forceUpdateSelf);

    // envelope handling
    SCPEnvelopeWrapperPtr wrapEnvelope(SCPEnvelope const& envelope) override;
    void signEnvelope(SCPEnvelope& envelope) override;
    void emitEnvelope(SCPEnvelope const& envelope) override;
    bool isEnvelopeReady(SCPEnvelope const& env) const override;

    // value validation
    SCPDriver::ValidationLevel validateValue(
        uint64_t slotIndex, Value const& value, bool nomination,
        SCPDriver::ValidationExtraInfo* extraInfo = nullptr) const override;
    ValueWrapperPtr extractValidValue(uint64_t slotIndex,
                                      Value const& value) override;

    // value marshaling
    std::string toShortString(NodeID const& pk) const override;
    std::string getValueString(Value const& v) const override;

    // TODO: Docs. Mention that this function can throw if `v` cannot be
    // converted to a `StellarValue`
    // TODO: Mention in docs that this should only be called from slots with
    // slot indicies equal to LCL+1.
    Value makeSkipLedgerValueFromValue(Value const& v) const override;

    // TODO(4): Do I even need this function?
    bool isSkipLedgerValue(Value const& v) const override;

    bool isParallelTxSetDownloadEnabled() const override;
    bool protocolAllowsSkipValues() const override;

    // timer handling
    void setupTimer(uint64_t slotIndex, int timerID,
                    std::chrono::milliseconds timeout,
                    std::function<void()> cb) override;

    void stopTimer(uint64 slotIndex, int timerID) override;
    std::chrono::milliseconds computeTimeout(uint32 roundNumber,
                                             bool isNomination) override;

    // hashing support
    Hash getHashOf(std::vector<xdr::opaque_vec<>> const& vals) const override;

    // core SCP
    ValueWrapperPtr
    combineCandidates(uint64_t slotIndex,
                      ValueWrapperPtrSet const& candidates) override;
    void valueExternalized(uint64_t slotIndex, Value const& value) override;

    bool hasUpgrades(Value const& v) override;
    ValueWrapperPtr stripAllUpgrades(Value const& v) override;
    uint32_t getUpgradeNominationTimeoutLimit() const override;

    // TODO: Docs
    void noteSkipValueReplaced(uint64_t slotIndex) override;

    // Submit a value to consider for slotIndex
    // previousValue is the value from slotIndex-1
    void nominate(uint64_t slotIndex, StellarValue const& value,
                  TxSetXDRFrameConstPtr proposedSet,
                  StellarValue const& previousValue);

    SCPQuorumSetPtr getQSet(Hash const& qSetHash) override;

    // listeners
    void ballotDidHearFromQuorum(uint64_t slotIndex,
                                 SCPBallot const& ballot) override;
    void nominatingValue(uint64_t slotIndex, Value const& value) override;
    void updatedCandidateValue(uint64_t slotIndex, Value const& value) override;
    void startedBallotProtocol(uint64_t slotIndex,
                               SCPBallot const& ballot) override;
    void acceptedBallotPrepared(uint64_t slotIndex,
                                SCPBallot const& ballot) override;
    void confirmedBallotPrepared(uint64_t slotIndex,
                                 SCPBallot const& ballot) override;
    void acceptedCommit(uint64_t slotIndex, SCPBallot const& ballot) override;

    // Ballot blocked on txset tracking methods
    // Called when balloting becomes blocked waiting for a txset download
    void recordBallotBlockedOnTxSet(uint64_t slotIndex,
                                    Value const& value) override;
    // Called when balloting is unblocked (setting mCommit) to measure and
    // record how long we were blocked
    void measureAndRecordBallotBlockedOnTxSet(uint64_t slotIndex,
                                              Value const& value) override;

    std::optional<VirtualClock::time_point> getPrepareStart(uint64_t slotIndex);

    // validate close time as much as possible
    bool checkCloseTime(uint64_t slotIndex, uint64_t lastCloseTime,
                        StellarValue const& b) const;

    // wraps a *valid* StellarValue (throws if it can't find txSet/qSet)
    ValueWrapperPtr wrapStellarValue(StellarValue const& sv);

    ValueWrapperPtr wrapValue(Value const& sv) override;

    // clean up slots outside the valid range [minSlotIndex, maxSlotIndex]
    // Either bound may be nullopt to skip that direction.
    void purgeSlotsOutsideRange(std::optional<uint64_t> minSlotIndex,
                                std::optional<uint64_t> maxSlotIndex,
                                uint64 slotToKeep);

    // Called when a tx set is received to update any ValueWrappers that were
    // created before the tx set was available (for parallel tx set
    // downloading).
    void onTxSetReceived(Hash const& txSetHash, TxSetXDRFrameConstPtr txSet);

    double getExternalizeLag(NodeID const& id) const;

    Json::Value getQsetLagInfo(bool summary, bool fullKeys);

    // report the nodes identified as maybe dead (missing in SCP during the last
    // CHECK_FOR_DEAD_NODES_MINUTES interval)
    Json::Value getMaybeDeadNodes(bool fullKeys);

    // Begin a new interval for checking for dead nodes--set current dead nodes
    // as missing nodes from previous interval
    void startCheckForDeadNodesInterval();

    std::optional<std::chrono::milliseconds>
    getTxSetDownloadWaitTime(Value const& v) const override;
    std::chrono::milliseconds getTxSetDownloadTimeout() const override;

    // Application-specific weight function. This function uses the quality
    // levels from automatic quorum set generation to determine the weight of a
    // validator. It is designed to ensure that:
    // 1. Orgs of equal quality have equal chances of winning leader election.
    // 2. Higher quality orgs win more frequently than lower quality orgs.
    uint64 getNodeWeight(NodeID const& nodeID, SCPQuorumSet const& qset,
                         bool isLocalNode) const override;
    // For caching TxSet validity. Consist of {lcl.hash, txSetHash,
    // lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset}
    using TxSetValidityKey = std::tuple<Hash, Hash, uint64_t, uint64_t>;

    class TxSetValidityKeyHash
    {
      public:
        size_t operator()(TxSetValidityKey const& key) const;
    };
    void cacheValidTxSet(ApplicableTxSetFrame const& txSet,
                         LedgerHeaderHistoryEntry const& lcl,
                         uint64_t closeTimeOffset) const;
#ifdef BUILD_TESTS
    RandomEvictionCache<TxSetValidityKey, bool, TxSetValidityKeyHash>&
    getTxSetValidityCache()
    {
        return mTxSetValidCache;
    }

    // Get the number of nomination timeouts that occurred for a given slot
    std::optional<int64_t> getNominationTimeouts(uint64_t slotIndex) const;
#endif

  private:
    Application& mApp;
    HerderImpl& mHerder;
    LedgerManager& mLedgerManager;
    Upgrades const& mUpgrades;
    PendingEnvelopes& mPendingEnvelopes;
    SCP mSCP;

    // Registry of ValueWrappers that were created before their tx set was
    // available. Maps txSetHash -> weak_ptrs to wrappers awaiting that tx set.
    // When onTxSetReceived() is called, we update any waiting wrappers.
    // Cleanup of expired weak_ptrs happens in purgeSlots().
    std::map<Hash, std::vector<std::weak_ptr<ValueWrapper>>>
        mPendingTxSetWrappers;

    // Registry of EnvelopeWrappers that were created before their tx set was
    // available. Maps txSetHash -> weak_ptrs to wrappers awaiting that tx set.
    // When onTxSetReceived() is called, we update any waiting wrappers.
    // Cleanup of expired weak_ptrs happens in purgeSlots().
    std::map<Hash, std::vector<std::weak_ptr<SCPEnvelopeWrapper>>>
        mPendingTxSetEnvelopeWrappers;

    struct SCPMetrics
    {
        medida::Meter& mEnvelopeSign;

        medida::Meter& mValueValid;
        medida::Meter& mValueInvalid;

        // listeners
        medida::Meter& mCombinedCandidates;

        // Timers for nomination and ballot protocols
        medida::Timer& mNominateToPrepare;
        medida::Timer& mPrepareToExternalize;

        // Timers tracking externalize messages
        medida::Timer& mFirstToSelfExternalizeLag;
        medida::Timer& mSelfToOthersExternalizeLag;

        // Timer tracking how long balloting was blocked waiting for a txset
        // download (time spent in kAwaitingDownload before setting mCommit)
        medida::Timer& mBallotBlockedOnTxSet;

        // Tracks how many ledgers we externalized using a skip value.
        medida::Counter& mSkipExternalized;
        // Counts replacements of proposed values with the synthesized skip
        // value.
        medida::Counter& mSkipValueReplaced;

        SCPMetrics(Application& app);
    };

    SCPMetrics mSCPMetrics;

    // Nomination timeouts per ledger
    medida::Histogram& mNominateTimeout;
    // Prepare timeouts per ledger
    medida::Histogram& mPrepareTimeout;
    // Unique values referenced per ledger
    medida::Histogram& mUniqueValues;

    // Externalize lag tracking for nodes in qset
    UnorderedMap<NodeID, medida::Timer> mQSetLag;

    struct SCPTiming
    {
        std::optional<VirtualClock::time_point> mNominationStart;
        std::optional<VirtualClock::time_point> mPrepareStart;

        // Nomination timeouts before first prepare
        int64_t mNominationTimeoutCount{0};
        // Prepare timeouts before externalize
        int64_t mPrepareTimeoutCount{0};

        // externalize timing information
        std::optional<VirtualClock::time_point> mFirstExternalize;
        std::optional<VirtualClock::time_point> mSelfExternalize;

        // Tracks when balloting first became blocked on each txset in this
        // slot.
        std::map<Value, VirtualClock::time_point> mBallotBlockedOnTxSetStart;
    };

    // Map of time points for each slot to measure key protocol metrics:
    // * nomination to first prepare
    // * first prepare to externalize
    std::map<uint64_t, SCPTiming> mSCPExecutionTimes;

    uint32_t mLedgerSeqNominating;
    ValueWrapperPtr mCurrentValue;

    // timers used by SCP
    // indexed by slotIndex, timerID
    std::map<uint64_t, std::map<int, std::unique_ptr<VirtualTimer>>> mSCPTimers;

    // The nodes that have exclusively been marked missing during the current
    // interval.
    std::set<NodeID> mMissingNodes;
    // The nodes that were missing during the previous interval.
    std::set<NodeID> mDeadNodes;

    // validity of txSet
    mutable RandomEvictionCache<TxSetValidityKey, bool, TxSetValidityKeyHash>
        mTxSetValidCache;

    // TODO: Docs
    bool checkValueTypeAndSkipHashInvariant(StellarValue const& sv) const;

    SCPDriver::ValidationLevel validateValueAgainstLocalState(
        uint64_t slotIndex, StellarValue const& sv, bool nomination,
        SCPDriver::ValidationExtraInfo* extraInfo) const;

    SCPDriver::ValidationLevel
    validatePastOrFutureValue(uint64_t slotIndex, StellarValue const& b,
                              LedgerHeaderHistoryEntry const& lcl) const;
    void logQuorumInformationAndUpdateMetrics(uint64_t index);

    void clearSCPExecutionEvents();

    void timerCallbackWrapper(uint64_t slotIndex, int timerID,
                              std::function<void()> cb);

    void recordLogTiming(VirtualClock::time_point start,
                         VirtualClock::time_point end, medida::Timer& timer,
                         std::string const& logStr,
                         std::chrono::nanoseconds threshold,
                         uint64_t slotIndex);

    bool checkAndCacheTxSetValid(TxSetXDRFrame const& txSet,
                                 LedgerHeaderHistoryEntry const& lcl,
                                 uint64_t closeTimeOffset) const;

    bool deserializeAndValidateStellarValue(Value const& value,
                                            StellarValue& sv) const;
    void extractValidUpgrades(StellarValue& sv, bool nomination) const;
};
}
