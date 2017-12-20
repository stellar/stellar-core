// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderSCPDriver.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "herder/PendingEnvelopes.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "scp/SCP.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger-entries.h"
#include <medida/metrics_registry.h>
#include <xdrpp/marshal.h>

namespace stellar
{

HerderSCPDriver::SCPMetrics::SCPMetrics(Application& app)
    : mEnvelopeSign(
          app.getMetrics().NewMeter({"scp", "envelope", "sign"}, "envelope"))
    , mEnvelopeValidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "validsig"}, "envelope"))
    , mEnvelopeInvalidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "invalidsig"}, "envelope"))
    , mValueValid(app.getMetrics().NewMeter({"scp", "value", "valid"}, "value"))
    , mValueInvalid(
          app.getMetrics().NewMeter({"scp", "value", "invalid"}, "value"))
    , mValueExternalize(
          app.getMetrics().NewMeter({"scp", "value", "externalize"}, "value"))
    , mQuorumHeard(
          app.getMetrics().NewMeter({"scp", "quorum", "heard"}, "quorum"))
    , mNominatingValue(
          app.getMetrics().NewMeter({"scp", "value", "nominating"}, "value"))
    , mUpdatedCandidate(
          app.getMetrics().NewMeter({"scp", "value", "candidate"}, "value"))
    , mStartBallotProtocol(
          app.getMetrics().NewMeter({"scp", "ballot", "started"}, "ballot"))
    , mAcceptedBallotPrepared(app.getMetrics().NewMeter(
          {"scp", "ballot", "accepted-prepared"}, "ballot"))
    , mConfirmedBallotPrepared(app.getMetrics().NewMeter(
          {"scp", "ballot", "confirmed-prepared"}, "ballot"))
    , mAcceptedCommit(app.getMetrics().NewMeter(
          {"scp", "ballot", "accepted-commit"}, "ballot"))

    , mHerderStateCurrent(
          app.getMetrics().NewCounter({"herder", "state", "current"}))
    , mHerderStateChanges(
          app.getMetrics().NewTimer({"herder", "state", "changes"}))
{
}

HerderSCPDriver::HerderSCPDriver(Application& app, HerderImpl& herder,
                                 Upgrades const& upgrades,
                                 PendingEnvelopes& pendingEnvelopes)
    : mApp{app}
    , mHerder{herder}
    , mLedgerManager{mApp.getLedgerManager()}
    , mUpgrades{upgrades}
    , mPendingEnvelopes{pendingEnvelopes}
    , mSCP(*this, mApp.getConfig().NODE_SEED,
           mApp.getConfig().NODE_IS_VALIDATOR, mApp.getConfig().QUORUM_SET)
    , mSCPMetrics{mApp}
    , mLastStateChange{mApp.getClock().now()}
{
}

HerderSCPDriver::~HerderSCPDriver()
{
}

void
HerderSCPDriver::stateChanged()
{
    mSCPMetrics.mHerderStateCurrent.set_count(static_cast<int64_t>(getState()));
    auto now = mApp.getClock().now();
    mSCPMetrics.mHerderStateChanges.Update(now - mLastStateChange);
    mLastStateChange = now;
    mApp.syncOwnMetrics();
}

void
HerderSCPDriver::bootstrap()
{
    stateChanged();
}

void
HerderSCPDriver::lostSync()
{
    stateChanged();

    // transfer ownership to mHerderSCPDriver.lastTrackingSCP()
    mLastTrackingSCP.reset(mTrackingSCP.release());
}

Herder::State
HerderSCPDriver::getState() const
{
    return mTrackingSCP && mLastTrackingSCP ? Herder::HERDER_TRACKING_STATE
                                            : Herder::HERDER_SYNCING_STATE;
}

void
HerderSCPDriver::syncMetrics()
{
    auto c = mSCPMetrics.mHerderStateCurrent.count();
    auto n = static_cast<int64_t>(getState());
    if (c != n)
    {
        mSCPMetrics.mHerderStateCurrent.set_count(n);
    }
}

void
HerderSCPDriver::restoreSCPState(uint64_t index, StellarValue const& value)
{
    mTrackingSCP = make_unique<ConsensusData>(index, value);
}

// envelope handling

void
HerderSCPDriver::signEnvelope(SCPEnvelope& envelope)
{
    mSCPMetrics.mEnvelopeSign.Mark();
    envelope.signature = mSCP.getSecretKey().sign(xdr::xdr_to_opaque(
        mApp.getNetworkID(), ENVELOPE_TYPE_SCP, envelope.statement));
}

bool
HerderSCPDriver::verifyEnvelope(SCPEnvelope const& envelope)
{
    auto b = PubKeyUtils::verifySig(
        envelope.statement.nodeID, envelope.signature,
        xdr::xdr_to_opaque(mApp.getNetworkID(), ENVELOPE_TYPE_SCP,
                           envelope.statement));
    if (b)
    {
        mSCPMetrics.mEnvelopeValidSig.Mark();
    }
    else
    {
        mSCPMetrics.mEnvelopeInvalidSig.Mark();
    }

    return b;
}

void
HerderSCPDriver::emitEnvelope(SCPEnvelope const& envelope)
{
    mHerder.emitEnvelope(envelope);
}

// value validation

bool
HerderSCPDriver::isSlotCompatibleWithCurrentState(uint64_t slotIndex) const
{
    bool res = false;
    if (mLedgerManager.isSynced())
    {
        auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
        res = (slotIndex == (lcl.header.ledgerSeq + 1));
    }

    return res;
}

SCPDriver::ValidationLevel
HerderSCPDriver::validateValueHelper(uint64_t slotIndex,
                                     StellarValue const& b) const
{
    uint64_t lastCloseTime;

    bool compat = isSlotCompatibleWithCurrentState(slotIndex);

    if (compat)
    {
        lastCloseTime = mLedgerManager.getLastClosedLedgerHeader()
                            .header.scpValue.closeTime;
    }
    else
    {
        if (!mTrackingSCP)
        {
            // if we're not tracking, there is not much more we can do to
            // validate
            return SCPDriver::kMaybeValidValue;
        }

        // Check slotIndex.
        if (nextConsensusLedgerIndex() > slotIndex)
        {
            // we already moved on from this slot
            // still send it through for emitting the final messages
            return SCPDriver::kMaybeValidValue;
        }
        if (nextConsensusLedgerIndex() < slotIndex)
        {
            // this is probably a bug as "tracking" means we're processing
            // messages only for smaller slots
            CLOG(ERROR, "Herder")
                << "HerderSCPDriver::validateValue"
                << " i: " << slotIndex
                << " processing a future message while tracking";

            return SCPDriver::kInvalidValue;
        }
        lastCloseTime = mTrackingSCP->mConsensusValue.closeTime;
    }

    // Check closeTime (not too old)
    if (b.closeTime <= lastCloseTime)
    {
        return SCPDriver::kInvalidValue;
    }

    // Check closeTime (not too far in future)
    uint64_t timeNow = mApp.timeNow();
    if (b.closeTime > timeNow + Herder::MAX_TIME_SLIP_SECONDS.count())
    {
        return SCPDriver::kInvalidValue;
    }

    if (!compat)
    {
        // this is as far as we can go if we don't have the state
        return SCPDriver::kMaybeValidValue;
    }

    Hash const& txSetHash = b.txSetHash;

    // we are fully synced up

    TxSetFramePtr txSet = mPendingEnvelopes.getTxSet(txSetHash);

    SCPDriver::ValidationLevel res;

    if (!txSet)
    {
        CLOG(ERROR, "Herder") << "HerderSCPDriver::validateValue"
                              << " i: " << slotIndex << " txSet not found?";

        res = SCPDriver::kInvalidValue;
    }
    else if (!txSet->checkValid(mApp))
    {
        if (Logging::logDebug("Herder"))
            CLOG(DEBUG, "Herder") << "HerderSCPDriver::validateValue"
                                  << " i: " << slotIndex << " Invalid txSet:"
                                  << " " << hexAbbrev(txSet->getContentsHash());
        res = SCPDriver::kInvalidValue;
    }
    else
    {
        if (Logging::logDebug("Herder"))
            CLOG(DEBUG, "Herder")
                << "HerderSCPDriver::validateValue"
                << " i: " << slotIndex
                << " txSet: " << hexAbbrev(txSet->getContentsHash()) << " OK";
        res = SCPDriver::kFullyValidatedValue;
    }
    return res;
}

SCPDriver::ValidationLevel
HerderSCPDriver::validateValue(uint64_t slotIndex, Value const& value,
                               bool nomination)
{
    StellarValue b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        mSCPMetrics.mValueInvalid.Mark();
        return SCPDriver::kInvalidValue;
    }

    SCPDriver::ValidationLevel res = validateValueHelper(slotIndex, b);
    if (res != SCPDriver::kInvalidValue)
    {
        LedgerUpgradeType lastUpgradeType = LEDGER_UPGRADE_VERSION;
        // check upgrades
        for (size_t i = 0; i < b.upgrades.size(); i++)
        {
            LedgerUpgradeType thisUpgradeType;
            if (!mUpgrades.isValid(b.closeTime, b.upgrades[i], thisUpgradeType,
                                   nomination, mApp.getConfig()))
            {
                CLOG(TRACE, "Herder")
                    << "HerderSCPDriver::validateValue invalid step at index "
                    << i;
                res = SCPDriver::kInvalidValue;
            }
            if (i != 0 && (lastUpgradeType >= thisUpgradeType))
            {
                CLOG(TRACE, "Herder")
                    << "HerderSCPDriver::validateValue out of "
                       "order upgrade step at index "
                    << i;
                res = SCPDriver::kInvalidValue;
            }

            lastUpgradeType = thisUpgradeType;
        }
    }

    if (res)
    {
        mSCPMetrics.mValueValid.Mark();
    }
    else
    {
        mSCPMetrics.mValueInvalid.Mark();
    }
    return res;
}

Value
HerderSCPDriver::extractValidValue(uint64_t slotIndex, Value const& value)
{
    StellarValue b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        return Value();
    }
    Value res;
    if (validateValueHelper(slotIndex, b) == SCPDriver::kFullyValidatedValue)
    {
        // remove the upgrade steps we don't like
        LedgerUpgradeType thisUpgradeType;
        for (auto it = b.upgrades.begin(); it != b.upgrades.end();)
        {
            if (!mUpgrades.isValid(b.closeTime, *it, thisUpgradeType, true,
                                   mApp.getConfig()))
            {
                it = b.upgrades.erase(it);
            }
            else
            {
                it++;
            }
        }

        res = xdr::xdr_to_opaque(b);
    }

    return res;
}

// value marshaling

std::string
HerderSCPDriver::toShortString(PublicKey const& pk) const
{
    return mApp.getConfig().toShortString(pk);
}

std::string
HerderSCPDriver::getValueString(Value const& v) const
{
    StellarValue b;
    if (v.empty())
    {
        return "[:empty:]";
    }

    try
    {
        xdr::xdr_from_opaque(v, b);

        return stellarValueToString(b);
    }
    catch (...)
    {
        return "[:invalid:]";
    }
}

// timer handling

void
HerderSCPDriver::setupTimer(uint64_t slotIndex, int timerID,
                            std::chrono::milliseconds timeout,
                            std::function<void()> cb)
{
    // don't setup timers for old slots
    if (slotIndex <= mApp.getHerder().getCurrentLedgerSeq())
    {
        mSCPTimers.erase(slotIndex);
        return;
    }

    auto& slotTimers = mSCPTimers[slotIndex];

    auto it = slotTimers.find(timerID);
    if (it == slotTimers.end())
    {
        it = slotTimers.emplace(timerID, make_unique<VirtualTimer>(mApp)).first;
    }
    auto& timer = *it->second;
    timer.cancel();
    timer.expires_from_now(timeout);
    timer.async_wait(cb, &VirtualTimer::onFailureNoop);
}

// core SCP

Value
HerderSCPDriver::combineCandidates(uint64_t slotIndex,
                                   std::set<Value> const& candidates)
{
    Hash h;

    StellarValue comp(h, 0, emptyUpgradeSteps, 0);

    std::map<LedgerUpgradeType, LedgerUpgrade> upgrades;

    std::set<TransactionFramePtr> aggSet;

    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();

    Hash candidatesHash;

    std::vector<StellarValue> candidateValues;

    for (auto const& c : candidates)
    {
        candidateValues.emplace_back();
        StellarValue& sv = candidateValues.back();

        xdr::xdr_from_opaque(c, sv);
        candidatesHash ^= sha256(c);

        // max closeTime
        if (comp.closeTime < sv.closeTime)
        {
            comp.closeTime = sv.closeTime;
        }
        for (auto const& upgrade : sv.upgrades)
        {
            LedgerUpgrade lupgrade;
            xdr::xdr_from_opaque(upgrade, lupgrade);
            auto it = upgrades.find(lupgrade.type());
            if (it == upgrades.end())
            {
                upgrades.emplace(std::make_pair(lupgrade.type(), lupgrade));
            }
            else
            {
                LedgerUpgrade& clUpgrade = it->second;
                switch (lupgrade.type())
                {
                case LEDGER_UPGRADE_VERSION:
                    // pick the highest version
                    clUpgrade.newLedgerVersion() =
                        std::max(clUpgrade.newLedgerVersion(),
                                 lupgrade.newLedgerVersion());
                    break;
                case LEDGER_UPGRADE_BASE_FEE:
                    // take the max fee
                    clUpgrade.newBaseFee() =
                        std::max(clUpgrade.newBaseFee(), lupgrade.newBaseFee());
                    break;
                case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
                    // take the max tx set size
                    clUpgrade.newMaxTxSetSize() =
                        std::max(clUpgrade.newMaxTxSetSize(),
                                 lupgrade.newMaxTxSetSize());
                    break;
                case LEDGER_UPGRADE_BASE_RESERVE:
                    // take the max base reserve
                    clUpgrade.newBaseReserve() = std::max(
                        clUpgrade.newBaseReserve(), lupgrade.newBaseReserve());
                    break;
                default:
                    // should never get there with values that are not valid
                    throw std::runtime_error("invalid upgrade step");
                }
            }
        }
    }

    // take the txSet with the highest number of transactions,
    // highest xored hash that we have
    TxSetFramePtr bestTxSet;
    {
        Hash highest;
        TxSetFramePtr highestTxSet;
        for (auto const& sv : candidateValues)
        {
            TxSetFramePtr cTxSet = mPendingEnvelopes.getTxSet(sv.txSetHash);

            if (cTxSet && cTxSet->previousLedgerHash() == lcl.hash)
            {
                if (!highestTxSet ||
                    (cTxSet->mTransactions.size() >
                     highestTxSet->mTransactions.size()) ||
                    ((cTxSet->mTransactions.size() ==
                      highestTxSet->mTransactions.size()) &&
                     lessThanXored(highest, sv.txSetHash, candidatesHash)))
                {
                    highestTxSet = cTxSet;
                    highest = sv.txSetHash;
                }
            }
        }
        // make a copy as we're about to modify it and we don't want to mess
        // with the txSet cache
        bestTxSet = std::make_shared<TxSetFrame>(*highestTxSet);
    }

    for (auto const& upgrade : upgrades)
    {
        Value v(xdr::xdr_to_opaque(upgrade.second));
        comp.upgrades.emplace_back(v.begin(), v.end());
    }

    std::vector<TransactionFramePtr> removed;

    // just to be sure
    bestTxSet->trimInvalid(mApp, removed);
    comp.txSetHash = bestTxSet->getContentsHash();

    if (removed.size() != 0)
    {
        CLOG(WARNING, "Herder") << "Candidate set had " << removed.size()
                                << " invalid transactions";

        // post to avoid triggering SCP handling code recursively
        mApp.getClock().getIOService().post([this, bestTxSet]() {
            mPendingEnvelopes.recvTxSet(bestTxSet->getContentsHash(),
                                        bestTxSet);
        });
    }

    return xdr::xdr_to_opaque(comp);
}

void
HerderSCPDriver::valueExternalized(uint64_t slotIndex, Value const& value)
{
    mSCPMetrics.mValueExternalize.Mark();

    auto it = mSCPTimers.begin(); // cancel all timers below this slot
    while (it != mSCPTimers.end() && it->first <= slotIndex)
    {
        it = mSCPTimers.erase(it);
    }

    if (slotIndex <= mApp.getHerder().getCurrentLedgerSeq())
    {
        // externalize may trigger on older slots:
        //  * when the current instance starts up
        //  * when getting back in sync (a gap potentially opened)
        // in both cases it's safe to just ignore those as we're already
        // tracking a more recent state
        CLOG(DEBUG, "Herder")
            << "Ignoring old ledger externalize " << slotIndex;
        return;
    }

    StellarValue b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        // This may not be possible as all messages are validated and should
        // therefore contain a valid StellarValue.
        CLOG(ERROR, "Herder") << "HerderSCPDriver::valueExternalized"
                              << " Externalized StellarValue malformed";
        // no point in continuing as 'b' contains garbage at this point
        abort();
    }

    // log information from older ledger to increase the chances that
    // all messages made it
    if (slotIndex > 2)
    {
        logQuorumInformation(slotIndex - 2);
    }

    if (!mCurrentValue.empty())
    {
        // stop nomination
        // this may or may not be the ledger that is currently externalizing
        // in both cases, we want to stop nomination as:
        // either we're closing the current ledger (typical case)
        // or we're going to trigger catchup from history
        mSCP.stopNomination(mLedgerSeqNominating);
        mCurrentValue.clear();
    }

    if (!mTrackingSCP)
    {
        stateChanged();
    }

    mTrackingSCP = make_unique<ConsensusData>(slotIndex, b);

    if (!mLastTrackingSCP)
    {
        mLastTrackingSCP = make_unique<ConsensusData>(*mTrackingSCP);
    }

    mHerder.valueExternalized(slotIndex, b);
}

void
HerderSCPDriver::logQuorumInformation(uint64_t index)
{
    std::string res;
    Json::Value v;
    mApp.getHerder().dumpQuorumInfo(v, mSCP.getLocalNodeID(), true, index);
    auto slots = v.get("slots", "");
    if (!slots.empty())
    {
        std::string indexs = std::to_string(static_cast<uint32>(index));
        auto i = slots.get(indexs, "");
        if (!i.empty())
        {
            Json::FastWriter fw;
            CLOG(INFO, "Herder")
                << "Quorum information for " << index << " : " << fw.write(i);
        }
    }
}

void
HerderSCPDriver::nominate(uint64_t slotIndex, StellarValue const& value,
                          TxSetFramePtr proposedSet,
                          StellarValue const& previousValue)
{
    mCurrentValue = xdr::xdr_to_opaque(value);
    mLedgerSeqNominating = static_cast<uint32_t>(slotIndex);

    auto valueHash = sha256(xdr::xdr_to_opaque(mCurrentValue));
    CLOG(DEBUG, "Herder") << "HerderSCPDriver::triggerNextLedger"
                          << " txSet.size: "
                          << proposedSet->mTransactions.size()
                          << " previousLedgerHash: "
                          << hexAbbrev(proposedSet->previousLedgerHash())
                          << " value: " << hexAbbrev(valueHash)
                          << " slot: " << slotIndex;

    auto prevValue = xdr::xdr_to_opaque(previousValue);
    mSCP.nominate(slotIndex, mCurrentValue, prevValue);
}

SCPQuorumSetPtr
HerderSCPDriver::getQSet(Hash const& qSetHash)
{
    return mPendingEnvelopes.getQSet(qSetHash);
}

void
HerderSCPDriver::ballotDidHearFromQuorum(uint64_t, SCPBallot const&)
{
    mSCPMetrics.mQuorumHeard.Mark();
}

void
HerderSCPDriver::nominatingValue(uint64_t slotIndex, Value const& value)
{
    if (Logging::logDebug("Herder"))
        CLOG(DEBUG, "Herder") << "nominatingValue i:" << slotIndex
                              << " v: " << getValueString(value);

    if (!value.empty())
    {
        mSCPMetrics.mNominatingValue.Mark();
    }
}

void
HerderSCPDriver::updatedCandidateValue(uint64_t slotIndex, Value const& value)
{
    mSCPMetrics.mUpdatedCandidate.Mark();
}

void
HerderSCPDriver::startedBallotProtocol(uint64_t slotIndex,
                                       SCPBallot const& ballot)
{
    mSCPMetrics.mStartBallotProtocol.Mark();
}
void
HerderSCPDriver::acceptedBallotPrepared(uint64_t slotIndex,
                                        SCPBallot const& ballot)
{
    mSCPMetrics.mAcceptedBallotPrepared.Mark();
}

void
HerderSCPDriver::confirmedBallotPrepared(uint64_t slotIndex,
                                         SCPBallot const& ballot)
{
    mSCPMetrics.mConfirmedBallotPrepared.Mark();
}

void
HerderSCPDriver::acceptedCommit(uint64_t slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mAcceptedCommit.Mark();
}
}
