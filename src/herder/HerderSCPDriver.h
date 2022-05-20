#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "herder/TxSetUtils.h"
#include "medida/timer.h"
#include "scp/SCPDriver.h"
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

    // value validation
    SCPDriver::ValidationLevel validateValue(uint64_t slotIndex,
                                             Value const& value,
                                             bool nomination) override;
    ValueWrapperPtr extractValidValue(uint64_t slotIndex,
                                      Value const& value) override;

    // value marshaling
    std::string toShortString(NodeID const& pk) const override;
    std::string getValueString(Value const& v) const override;

    // timer handling
    void setupTimer(uint64_t slotIndex, int timerID,
                    std::chrono::milliseconds timeout,
                    std::function<void()> cb) override;

    // hashing support
    Hash getHashOf(std::vector<xdr::opaque_vec<>> const& vals) const override;

    // core SCP
    ValueWrapperPtr
    combineCandidates(uint64_t slotIndex,
                      ValueWrapperPtrSet const& candidates) override;
    void valueExternalized(uint64_t slotIndex, Value const& value) override;

    // Submit a value to consider for slotIndex
    // previousValue is the value from slotIndex-1
    void nominate(uint64_t slotIndex, StellarValue const& value,
                  TxSetFrameConstPtr proposedSet,
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

    std::optional<VirtualClock::time_point> getPrepareStart(uint64_t slotIndex);

    // converts a Value into a StellarValue
    // returns false on error
    bool toStellarValue(Value const& v, StellarValue& sv);

    // validate close time as much as possible
    bool checkCloseTime(uint64_t slotIndex, uint64_t lastCloseTime,
                        StellarValue const& b) const;

    // wraps a *valid* StellarValue (throws if it can't find txSet/qSet)
    ValueWrapperPtr wrapStellarValue(StellarValue const& sv);

    ValueWrapperPtr wrapValue(Value const& sv) override;

    // clean up older slots
    void purgeSlots(uint64_t maxSlotIndex);

    double getExternalizeLag(NodeID const& id) const;

    Json::Value getQsetLagInfo(bool summary, bool fullKeys);

  private:
    Application& mApp;
    HerderImpl& mHerder;
    LedgerManager& mLedgerManager;
    Upgrades const& mUpgrades;
    PendingEnvelopes& mPendingEnvelopes;
    SCP mSCP;

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

        SCPMetrics(Application& app);
    };

    SCPMetrics mSCPMetrics;

    // Nomination timeouts per ledger
    medida::Histogram& mNominateTimeout;
    // Prepare timeouts per ledger
    medida::Histogram& mPrepareTimeout;

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
    // For caching TxSet validity. Consist of {lcl.hash, txSetHash,
    // lowerBoundCloseTimeOffset, upperBoundCloseTimeOffset}
    using TxSetValidityKey = std::tuple<Hash, Hash, uint64_t, uint64_t>;

    class TxSetValidityKeyHash
    {
      public:
        size_t operator()(TxSetValidityKey const& key) const;
    };
    // validity of txSet
    mutable RandomEvictionCache<TxSetValidityKey, bool, TxSetValidityKeyHash>
        mTxSetValidCache;

    SCPDriver::ValidationLevel validateValueHelper(uint64_t slotIndex,
                                                   StellarValue const& sv,
                                                   bool nomination) const;

    void logQuorumInformation(uint64_t index);

    void clearSCPExecutionEvents();

    void timerCallbackWrapper(uint64_t slotIndex, int timerID,
                              std::function<void()> cb);

    void recordLogTiming(VirtualClock::time_point start,
                         VirtualClock::time_point end, medida::Timer& timer,
                         std::string const& logStr,
                         std::chrono::nanoseconds threshold,
                         uint64_t slotIndex);

    bool checkAndCacheTxSetValid(TxSetFrameConstPtr TxSet,
                                 uint64_t closeTimeOffset) const;
};
}
