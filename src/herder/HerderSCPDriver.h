#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "scp/SCPDriver.h"
#include "xdr/Stellar-ledger.h"

namespace medida
{
class Counter;
class Meter;
class Timer;
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
    struct ConsensusData
    {
        uint64_t mConsensusIndex;
        StellarValue mConsensusValue;
        ConsensusData(uint64_t index, StellarValue const& b)
            : mConsensusIndex(index), mConsensusValue(b)
        {
        }
    };

    HerderSCPDriver(Application& app, HerderImpl& herder,
                    Upgrades const& upgrades,
                    PendingEnvelopes& pendingEnvelopes);
    ~HerderSCPDriver();

    void bootstrap();
    void lostSync();

    Herder::State getState() const;

    ConsensusData*
    trackingSCP() const
    {
        return mTrackingSCP.get();
    }
    ConsensusData*
    lastTrackingSCP() const
    {
        return mLastTrackingSCP.get();
    }

    void restoreSCPState(uint64_t index, StellarValue const& value);

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

    SCP&
    getSCP()
    {
        return mSCP;
    }

    void recordSCPExecutionMetrics(uint64_t slotIndex);
    void recordSCPEvent(uint64_t slotIndex, bool isNomination);

    // envelope handling
    void signEnvelope(SCPEnvelope& envelope) override;
    bool verifyEnvelope(SCPEnvelope const& envelope) override;
    void emitEnvelope(SCPEnvelope const& envelope) override;

    // value validation
    SCPDriver::ValidationLevel validateValue(uint64_t slotIndex,
                                             Value const& value,
                                             bool nomination) override;
    Value extractValidValue(uint64_t slotIndex, Value const& value) override;

    // value marshaling
    std::string toShortString(PublicKey const& pk) const override;
    std::string getValueString(Value const& v) const override;

    // timer handling
    void setupTimer(uint64_t slotIndex, int timerID,
                    std::chrono::milliseconds timeout,
                    std::function<void()> cb) override;

    // core SCP
    Value combineCandidates(uint64_t slotIndex,
                            std::set<Value> const& candidates) override;
    void valueExternalized(uint64_t slotIndex, Value const& value) override;

    // Submit a value to consider for slotIndex
    // previousValue is the value from slotIndex-1
    void nominate(uint64_t slotIndex, StellarValue const& value,
                  TxSetFramePtr proposedSet, StellarValue const& previousValue);

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

    optional<VirtualClock::time_point> getPrepareStart(uint64_t slotIndex);

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
        medida::Meter& mEnvelopeValidSig;
        medida::Meter& mEnvelopeInvalidSig;

        medida::Meter& mValueValid;
        medida::Meter& mValueInvalid;

        // listeners
        medida::Meter& mCombinedCandidates;

        // Timers for nomination and ballot protocols
        medida::Timer& mNominateToPrepare;
        medida::Timer& mPrepareToExternalize;

        SCPMetrics(Application& app);
    };

    SCPMetrics mSCPMetrics;

    struct SCPTiming
    {
        optional<VirtualClock::time_point> mNominationStart;
        optional<VirtualClock::time_point> mPrepareStart;
    };

    // Map of time points for each slot to measure key protocol metrics:
    // * nomination to first prepare
    // * first prepare to externalize
    std::map<uint64_t, SCPTiming> mSCPExecutionTimes;

    uint32_t mLedgerSeqNominating;
    Value mCurrentValue;

    // timers used by SCP
    // indexed by slotIndex, timerID
    std::map<uint64_t, std::map<int, std::unique_ptr<VirtualTimer>>> mSCPTimers;

    // if the local instance is tracking the current state of SCP
    // herder keeps track of the consensus index and ballot
    // when not set, it just means that herder will try to snap to any slot that
    // reached consensus
    // on startup, this can be set to a value persisted from the database
    std::unique_ptr<ConsensusData> mTrackingSCP;

    // when losing track of consensus, we remember the consensus value so that
    // we can ignore older ledgers (as we potentially receive old messages)
    // it only tracks actual consensus values (learned when externalizing)
    std::unique_ptr<ConsensusData> mLastTrackingSCP;

    void stateChanged();

    bool checkCloseTime(uint64_t slotIndex, uint64_t lastCloseTime,
                        StellarValue const& b) const;

    SCPDriver::ValidationLevel
    validateValueHelper(uint64_t slotIndex, StellarValue const& sv) const;

    // returns true if the local instance is in a state compatible with
    // this slot
    bool isSlotCompatibleWithCurrentState(uint64_t slotIndex) const;

    void logQuorumInformation(uint64_t index);

    void clearSCPExecutionEvents();

    void timerCallbackWrapper(uint64_t slotIndex, int timerID,
                              std::function<void()> cb);
};
}
