// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderSCPDriver.h"
#include "HerderUtils.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "herder/PendingEnvelopes.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "scp/SCP.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "xdr/Stellar-SCP.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include <Tracy.hpp>
#include <algorithm>
#include <fmt/format.h>
#include <medida/metrics_registry.h>
#include <numeric>
#include <stdexcept>
#include <xdrpp/marshal.h>

namespace stellar
{

Hash
HerderSCPDriver::getHashOf(std::vector<xdr::opaque_vec<>> const& vals) const
{
    SHA256 hasher;
    for (auto const& v : vals)
    {
        hasher.add(v);
    }
    return hasher.finish();
}

HerderSCPDriver::SCPMetrics::SCPMetrics(Application& app)
    : mEnvelopeSign(
          app.getMetrics().NewMeter({"scp", "envelope", "sign"}, "envelope"))
    , mValueValid(app.getMetrics().NewMeter({"scp", "value", "valid"}, "value"))
    , mValueInvalid(
          app.getMetrics().NewMeter({"scp", "value", "invalid"}, "value"))
    , mCombinedCandidates(app.getMetrics().NewMeter(
          {"scp", "nomination", "combinecandidates"}, "value"))
    , mNominateToPrepare(
          app.getMetrics().NewTimer({"scp", "timing", "nominated"}))
    , mPrepareToExternalize(
          app.getMetrics().NewTimer({"scp", "timing", "externalized"}))
    , mFirstToSelfExternalizeLag(app.getMetrics().NewTimer(
          {"scp", "timing", "first-to-self-externalize-lag"}))
    , mSelfToOthersExternalizeLag(app.getMetrics().NewTimer(
          {"scp", "timing", "self-to-others-externalize-lag"}))
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
    , mSCP{*this, mApp.getConfig().NODE_SEED.getPublicKey(),
           mApp.getConfig().NODE_IS_VALIDATOR, mApp.getConfig().QUORUM_SET}
    , mSCPMetrics{mApp}
    , mNominateTimeout{mApp.getMetrics().NewHistogram(
          {"scp", "timeout", "nominate"})}
    , mPrepareTimeout{mApp.getMetrics().NewHistogram(
          {"scp", "timeout", "prepare"})}
    , mLedgerSeqNominating(0)
{
}

HerderSCPDriver::~HerderSCPDriver()
{
}

void
HerderSCPDriver::stateChanged()
{
    mApp.syncOwnMetrics();
}

void
HerderSCPDriver::bootstrap()
{
    stateChanged();
    clearSCPExecutionEvents();
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
    // we're only returning "TRACKING" when we're tracking the actual network
    // (mLastTrackingSCP is also set when this happens)
    return mTrackingSCP && mLastTrackingSCP ? Herder::HERDER_TRACKING_STATE
                                            : Herder::HERDER_SYNCING_STATE;
}

void
HerderSCPDriver::restoreSCPState(uint64_t index, StellarValue const& value)
{
    mTrackingSCP = std::make_unique<ConsensusData>(index, value);
}

// envelope handling

class SCPHerderEnvelopeWrapper : public SCPEnvelopeWrapper
{
    HerderImpl& mHerder;

    SCPQuorumSetPtr mQSet;
    std::vector<TxSetFramePtr> mTxSets;

  public:
    explicit SCPHerderEnvelopeWrapper(SCPEnvelope const& e, HerderImpl& herder)
        : SCPEnvelopeWrapper(e), mHerder(herder)
    {
        // attach everything we can to the wrapper
        auto qSetH = Slot::getCompanionQuorumSetHashFromStatement(e.statement);
        mQSet = mHerder.getQSet(qSetH);
        if (!mQSet)
        {
            throw std::runtime_error(
                fmt::format("SCPHerderEnvelopeWrapper: Wrapping an unknown "
                            "qset {} from envelope",
                            hexAbbrev(qSetH)));
        }
        auto txSets = getTxSetHashes(e);
        for (auto const& txSetH : txSets)
        {
            auto txSet = mHerder.getTxSet(txSetH);
            if (txSet)
            {
                mTxSets.emplace_back(txSet);
            }
            else
            {
                throw std::runtime_error(
                    fmt::format("SCPHerderEnvelopeWrapper: Wrapping an unknown "
                                "tx set {} from envelope",
                                hexAbbrev(txSetH)));
            }
        }
    }
};

SCPEnvelopeWrapperPtr
HerderSCPDriver::wrapEnvelope(SCPEnvelope const& envelope)
{
    auto r = std::make_shared<SCPHerderEnvelopeWrapper>(envelope, mHerder);
    return r;
}

void
HerderSCPDriver::signEnvelope(SCPEnvelope& envelope)
{
    ZoneScoped;
    mSCPMetrics.mEnvelopeSign.Mark();
    mHerder.signEnvelope(mApp.getConfig().NODE_SEED, envelope);
}

void
HerderSCPDriver::emitEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    mHerder.emitEnvelope(envelope);
}

// value validation

bool
HerderSCPDriver::checkCloseTime(uint64_t slotIndex, uint64_t lastCloseTime,
                                StellarValue const& b) const
{
    // Check closeTime (not too old)
    if (b.closeTime <= lastCloseTime)
    {
        CLOG_TRACE(Herder, "Close time too old for slot {}, got {} vs {}",
                   slotIndex, b.closeTime, lastCloseTime);
        return false;
    }

    // Check closeTime (not too far in future)
    uint64_t timeNow = mApp.timeNow();
    if (b.closeTime > timeNow + Herder::MAX_TIME_SLIP_SECONDS.count())
    {
        CLOG_TRACE(Herder,
                   "Close time too far in future for slot {}, got {} vs {}",
                   slotIndex, b.closeTime, timeNow);
        return false;
    }
    return true;
}

SCPDriver::ValidationLevel
HerderSCPDriver::validateValueHelper(uint64_t slotIndex, StellarValue const& b,
                                     bool nomination) const
{
    uint64_t lastCloseTime;
    ZoneScoped;
    if (b.ext.v() != STELLAR_VALUE_SIGNED)
    {
        CLOG_TRACE(Herder,
                   "HerderSCPDriver::validateValue i: {} invalid value type - "
                   "expected SIGNED",
                   slotIndex);
        return SCPDriver::kInvalidValue;
    }

    {
        ZoneNamedN(sigZone, "signature check", true);
        if (!mHerder.verifyStellarValueSignature(b))
        {
            return SCPDriver::kInvalidValue;
        }
    }

    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader().header;
    // when checking close time, start with what we have locally
    lastCloseTime = lcl.scpValue.closeTime;

    // if this value is not for our local state,
    // perform as many checks as we can
    if (slotIndex != (lcl.ledgerSeq + 1))
    {
        if (slotIndex == lcl.ledgerSeq)
        {
            // previous ledger
            if (b.closeTime != lastCloseTime)
            {
                CLOG_TRACE(Herder,
                           "Got a bad close time for ledger {}, got {} vs {}",
                           slotIndex, b.closeTime, lastCloseTime);
                return SCPDriver::kInvalidValue;
            }
        }
        else if (slotIndex < lcl.ledgerSeq)
        {
            // basic sanity check on older value
            if (b.closeTime >= lastCloseTime)
            {
                CLOG_TRACE(Herder,
                           "Got a bad close time for ledger {}, got {} vs {}",
                           slotIndex, b.closeTime, lastCloseTime);
                return SCPDriver::kInvalidValue;
            }
        }
        else if (!checkCloseTime(slotIndex, lastCloseTime, b))
        {
            // future messages must be valid compared to lastCloseTime
            return SCPDriver::kInvalidValue;
        }

        if (!mTrackingSCP)
        {
            // if we're not tracking, there is not much more we can do to
            // validate
            if (Logging::logTrace("Herder"))
            {
                CLOG_TRACE(Herder, "MaybeValidValue (not tracking) for slot {}",
                           slotIndex);
            }
            return SCPDriver::kMaybeValidValue;
        }

        // Check slotIndex.
        if (nextConsensusLedgerIndex() > slotIndex)
        {
            // we already moved on from this slot
            // still send it through for emitting the final messages
            if (Logging::logTrace("Herder"))
            {
                CLOG_TRACE(
                    Herder,
                    "MaybeValidValue (already moved on) for slot {}, at {}",
                    slotIndex, nextConsensusLedgerIndex());
            }
            return SCPDriver::kMaybeValidValue;
        }
        if (nextConsensusLedgerIndex() < slotIndex)
        {
            // this is probably a bug as "tracking" means we're processing
            // messages only for smaller slots
            CLOG_ERROR(
                Herder,
                "HerderSCPDriver::validateValue i: {} processing a future "
                "message while tracking (tracking: {}, last: {} ) ",
                slotIndex, mTrackingSCP->mConsensusIndex,
                (mLastTrackingSCP ? mLastTrackingSCP->mConsensusIndex : 0));
            return SCPDriver::kInvalidValue;
        }

        // when tracking, we use the tracked time for last close time
        lastCloseTime = mTrackingSCP->mConsensusValue.closeTime;
        if (!checkCloseTime(slotIndex, lastCloseTime, b))
        {
            return SCPDriver::kInvalidValue;
        }

        // this is as far as we can go if we don't have the state
        if (Logging::logTrace("Herder"))
        {
            CLOG_TRACE(Herder,
                       "Can't validate locally, value may be valid for slot {}",
                       slotIndex);
        }
        return SCPDriver::kMaybeValidValue;
    }

    // the value is against the local state, we can perform all checks

    if (!checkCloseTime(slotIndex, lastCloseTime, b))
    {
        return SCPDriver::kInvalidValue;
    }

    Hash const& txSetHash = b.txSetHash;
    TxSetFramePtr txSet = mPendingEnvelopes.getTxSet(txSetHash);

    SCPDriver::ValidationLevel res;

    auto closeTimeOffset = b.closeTime - lastCloseTime;

    if (!txSet)
    {
        CLOG_ERROR(Herder, "validateValue i:{} unknown txSet {}", slotIndex,
                   hexAbbrev(txSetHash));

        res = SCPDriver::kInvalidValue;
    }
    else if (!txSet->checkValid(mApp, closeTimeOffset, closeTimeOffset))
    {
        if (Logging::logDebug("Herder"))
            CLOG_DEBUG(Herder,
                       "HerderSCPDriver::validateValue i: {} invalid txSet {}",
                       slotIndex, hexAbbrev(txSetHash));
        res = SCPDriver::kInvalidValue;
    }
    else
    {
        if (Logging::logDebug("Herder"))
            CLOG_DEBUG(Herder,
                       "HerderSCPDriver::validateValue i: {} valid txSet {}",
                       slotIndex, hexAbbrev(txSetHash));
        res = SCPDriver::kFullyValidatedValue;
    }
    return res;
}

SCPDriver::ValidationLevel
HerderSCPDriver::validateValue(uint64_t slotIndex, Value const& value,
                               bool nomination)
{
    ZoneScoped;
    StellarValue b;
    try
    {
        ZoneNamedN(xdrZone, "XDR deserialize", true);
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        mSCPMetrics.mValueInvalid.Mark();
        return SCPDriver::kInvalidValue;
    }

    SCPDriver::ValidationLevel res =
        validateValueHelper(slotIndex, b, nomination);
    if (res != SCPDriver::kInvalidValue)
    {
        auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();

        LedgerUpgradeType lastUpgradeType = LEDGER_UPGRADE_VERSION;
        // check upgrades
        for (size_t i = 0;
             i < b.upgrades.size() && res != SCPDriver::kInvalidValue; i++)
        {
            LedgerUpgradeType thisUpgradeType;
            if (!mUpgrades.isValid(b.upgrades[i], thisUpgradeType, nomination,
                                   mApp.getConfig(), lcl.header))
            {
                CLOG_TRACE(
                    Herder,
                    "HerderSCPDriver::validateValue invalid step at index {}",
                    i);
                res = SCPDriver::kInvalidValue;
            }
            else if (i != 0 && (lastUpgradeType >= thisUpgradeType))
            {
                CLOG_TRACE(Herder,
                           "HerderSCPDriver::validateValue out of "
                           "order upgrade step at index {}",
                           i);
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

ValueWrapperPtr
HerderSCPDriver::extractValidValue(uint64_t slotIndex, Value const& value)
{
    ZoneScoped;
    StellarValue b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        return nullptr;
    }
    ValueWrapperPtr res;
    if (validateValueHelper(slotIndex, b, true) ==
        SCPDriver::kFullyValidatedValue)
    {
        auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();

        // remove the upgrade steps we don't like
        LedgerUpgradeType thisUpgradeType;
        for (auto it = b.upgrades.begin(); it != b.upgrades.end();)
        {
            if (!mUpgrades.isValid(*it, thisUpgradeType, true, mApp.getConfig(),
                                   lcl.header))
            {
                it = b.upgrades.erase(it);
            }
            else
            {
                it++;
            }
        }

        res = wrapStellarValue(b);
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

        return stellarValueToString(mApp.getConfig(), b);
    }
    catch (...)
    {
        return "[:invalid:]";
    }
}

// timer handling
void
HerderSCPDriver::timerCallbackWrapper(uint64_t slotIndex, int timerID,
                                      std::function<void()> cb)
{
    // reschedule timers for future slots when tracking
    if (trackingSCP() && nextConsensusLedgerIndex() != slotIndex)
    {
        CLOG_WARNING(
            Herder, "Herder rescheduled timer {} for slot {} with next slot {}",
            timerID, slotIndex, nextConsensusLedgerIndex());
        setupTimer(slotIndex, timerID, std::chrono::seconds(1),
                   std::bind(&HerderSCPDriver::timerCallbackWrapper, this,
                             slotIndex, timerID, cb));
    }
    else
    {
        auto SCPTimingIt = mSCPExecutionTimes.find(slotIndex);
        if (SCPTimingIt != mSCPExecutionTimes.end())
        {
            auto& SCPTiming = SCPTimingIt->second;
            if (timerID == Slot::BALLOT_PROTOCOL_TIMER)
            {
                // Timeout happened in between first prepare and externalize
                ++SCPTiming.mPrepareTimeoutCount;
            }
            else
            {
                if (!SCPTiming.mPrepareStart)
                {
                    // Timeout happened between nominate and first prepare
                    ++SCPTiming.mNominationTimeoutCount;
                }
            }
        }

        cb();
    }
}

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
        it = slotTimers.emplace(timerID, std::make_unique<VirtualTimer>(mApp))
                 .first;
    }
    auto& timer = *it->second;
    timer.cancel();
    if (cb)
    {
        timer.expires_from_now(timeout);
        timer.async_wait(std::bind(&HerderSCPDriver::timerCallbackWrapper, this,
                                   slotIndex, timerID, cb),
                         &VirtualTimer::onFailureNoop);
    }
}

// returns true if l < r
// lh, rh are the hashes of l,h
static bool
compareTxSets(TxSetFrameConstPtr l, TxSetFrameConstPtr r, Hash const& lh,
              Hash const& rh, LedgerHeader const& header, Hash const& s)
{
    if (l == nullptr)
    {
        return r != nullptr;
    }
    if (r == nullptr)
    {
        return false;
    }
    auto lSize = l->size(header);
    auto rSize = r->size(header);
    if (lSize < rSize)
    {
        return true;
    }
    else if (lSize > rSize)
    {
        return false;
    }
    if (header.ledgerVersion >= 11)
    {
        auto lFee = l->getTotalFees(header);
        auto rFee = r->getTotalFees(header);
        if (lFee < rFee)
        {
            return true;
        }
        else if (lFee > rFee)
        {
            return false;
        }
    }
    return lessThanXored(lh, rh, s);
}

ValueWrapperPtr
HerderSCPDriver::combineCandidates(uint64_t slotIndex,
                                   ValueWrapperPtrSet const& candidates)
{
    ZoneScoped;
    CLOG_DEBUG(Herder, "Combining {} candidates", candidates.size());
    mSCPMetrics.mCombinedCandidates.Mark(candidates.size());

    std::map<LedgerUpgradeType, LedgerUpgrade> upgrades;

    std::set<TransactionFramePtr> aggSet;

    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();

    Hash candidatesHash;

    std::vector<StellarValue> candidateValues;

    for (auto const& c : candidates)
    {
        candidateValues.emplace_back();
        StellarValue& sv = candidateValues.back();

        xdr::xdr_from_opaque(c->getValue(), sv);
        candidatesHash ^= sha256(c->getValue());

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

    StellarValue comp;
    // take the txSet with the biggest size, highest xored hash that we have
    {
        auto highest = candidateValues.cend();
        TxSetFrameConstPtr highestTxSet;
        for (auto it = candidateValues.cbegin(); it != candidateValues.cend();
             ++it)
        {
            auto const& sv = *it;
            auto const cTxSet = mPendingEnvelopes.getTxSet(sv.txSetHash);
            if (cTxSet && cTxSet->previousLedgerHash() == lcl.hash &&
                (!highestTxSet ||
                 compareTxSets(highestTxSet, cTxSet, highest->txSetHash,
                               sv.txSetHash, lcl.header, candidatesHash)))
            {
                highest = it;
                highestTxSet = cTxSet;
            }
        };
        if (highest == candidateValues.cend())
        {
            throw std::runtime_error(
                "No highest candidate transaction set found");
        }
        comp = *highest;
    }
    comp.upgrades.clear();
    for (auto const& upgrade : upgrades)
    {
        Value v(xdr::xdr_to_opaque(upgrade.second));
        comp.upgrades.emplace_back(v.begin(), v.end());
    }

    auto res = wrapStellarValue(comp);
    return res;
}

bool
HerderSCPDriver::toStellarValue(Value const& v, StellarValue& sv)
{
    try
    {
        xdr::xdr_from_opaque(v, sv);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

void
HerderSCPDriver::valueExternalized(uint64_t slotIndex, Value const& value)
{
    ZoneScoped;
    auto it = mSCPTimers.begin(); // cancel all timers below this slot
    while (it != mSCPTimers.end() && it->first <= slotIndex)
    {
        it = mSCPTimers.erase(it);
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
        CLOG_ERROR(Herder, "HerderSCPDriver::valueExternalized "
                           "Externalized StellarValue malformed");
        CLOG_ERROR(Herder, "{}", REPORT_INTERNAL_BUG);
        // no point in continuing as 'b' contains garbage at this point
        abort();
    }

    // externalize may trigger on older slots:
    //  * when the current instance starts up
    //  * when getting back in sync (a gap potentially opened)
    // in both cases do limited processing on older slots; more importantly,
    // deliver externalize events to LedgerManager
    bool isLatestSlot = slotIndex > mApp.getHerder().getCurrentLedgerSeq();

    // Only update tracking state when newer slot comes in
    if (isLatestSlot)
    {
        // log information from older ledger to increase the chances that
        // all messages made it
        if (slotIndex > 2)
        {
            logQuorumInformation(slotIndex - 2);
        }

        if (mCurrentValue)
        {
            // stop nomination
            // this may or may not be the ledger that is currently externalizing
            // in both cases, we want to stop nomination as:
            // either we're closing the current ledger (typical case)
            // or we're going to trigger catchup from history
            mSCP.stopNomination(mLedgerSeqNominating);
            mCurrentValue.reset();
        }

        if (!mTrackingSCP)
        {
            stateChanged();
        }

        mTrackingSCP = std::make_unique<ConsensusData>(slotIndex, b);

        if (!mLastTrackingSCP)
        {
            mLastTrackingSCP = std::make_unique<ConsensusData>(*mTrackingSCP);
        }

        // record lag
        recordSCPExternalizeEvent(slotIndex, mSCP.getLocalNodeID(), false);

        recordSCPExecutionMetrics(slotIndex);

        mHerder.valueExternalized(slotIndex, b);

        // update externalize time so that we don't include the time spent in
        // `mHerder.valueExternalized`
        recordSCPExternalizeEvent(slotIndex, mSCP.getLocalNodeID(), true);
    }
    else
    {
        mHerder.valueExternalized(slotIndex, b);
    }
}

void
HerderSCPDriver::logQuorumInformation(uint64_t index)
{
    std::string res;
    auto v = mApp.getHerder().getJsonQuorumInfo(mSCP.getLocalNodeID(), true,
                                                false, index);
    auto qset = v.get("qset", "");
    if (!qset.empty())
    {
        std::string indexs = std::to_string(static_cast<uint32>(index));
        Json::FastWriter fw;
        CLOG_INFO(Herder, "Quorum information for {} : {}", index,
                  fw.write(qset));
    }
}

void
HerderSCPDriver::nominate(uint64_t slotIndex, StellarValue const& value,
                          TxSetFramePtr proposedSet,
                          StellarValue const& previousValue)
{
    ZoneScoped;
    mCurrentValue = wrapStellarValue(value);
    mLedgerSeqNominating = static_cast<uint32_t>(slotIndex);

    auto valueHash = xdrSha256(mCurrentValue->getValue());
    CLOG_DEBUG(Herder,
               "HerderSCPDriver::triggerNextLedger txSet.size: {} "
               "previousLedgerHash: {} value: {} slot: {}",
               proposedSet->mTransactions.size(),
               hexAbbrev(proposedSet->previousLedgerHash()),
               hexAbbrev(valueHash), slotIndex);

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
}

void
HerderSCPDriver::nominatingValue(uint64_t slotIndex, Value const& value)
{
    if (Logging::logDebug("Herder"))
        CLOG_DEBUG(Herder, "nominatingValue i:{} v: {}", slotIndex,
                   getValueString(value));
}

void
HerderSCPDriver::updatedCandidateValue(uint64_t slotIndex, Value const& value)
{
}

void
HerderSCPDriver::startedBallotProtocol(uint64_t slotIndex,
                                       SCPBallot const& ballot)
{
    recordSCPEvent(slotIndex, false);
}
void
HerderSCPDriver::acceptedBallotPrepared(uint64_t slotIndex,
                                        SCPBallot const& ballot)
{
}

void
HerderSCPDriver::confirmedBallotPrepared(uint64_t slotIndex,
                                         SCPBallot const& ballot)
{
}

void
HerderSCPDriver::acceptedCommit(uint64_t slotIndex, SCPBallot const& ballot)
{
}

optional<VirtualClock::time_point>
HerderSCPDriver::getPrepareStart(uint64_t slotIndex)
{
    optional<VirtualClock::time_point> res;
    auto it = mSCPExecutionTimes.find(slotIndex);
    if (it != mSCPExecutionTimes.end())
    {
        res = it->second.mPrepareStart;
    }
    return res;
}

Json::Value
HerderSCPDriver::getQsetLagInfo(bool summary, bool fullKeys)
{
    Json::Value ret;
    double totalLag = 0;
    int numNodes = 0;

    auto qSet = getSCP().getLocalQuorumSet();
    LocalNode::forAllNodes(qSet, [&](NodeID const& n) {
        auto lag = getExternalizeLag(n);
        if (lag > 0)
        {
            if (!summary)
            {
                ret[toStrKey(n, fullKeys)] = static_cast<Json::UInt64>(lag);
            }
            else
            {
                totalLag += lag;
                numNodes++;
            }
        }
        return true;
    });

    if (summary && numNodes > 0)
    {
        double avgLag = totalLag / numNodes;
        ret = static_cast<Json::UInt64>(avgLag);
    }

    return ret;
}

double
HerderSCPDriver::getExternalizeLag(NodeID const& id) const
{
    auto n = mQSetLag.find(id);

    if (n == mQSetLag.end())
    {
        return 0.0;
    }

    return n->second.GetSnapshot().get75thPercentile();
}

void
HerderSCPDriver::recordSCPEvent(uint64_t slotIndex, bool isNomination)
{

    auto& timing = mSCPExecutionTimes[slotIndex];
    VirtualClock::time_point start = mApp.getClock().now();

    if (isNomination)
    {
        timing.mNominationStart =
            make_optional<VirtualClock::time_point>(start);
    }
    else
    {
        timing.mPrepareStart = make_optional<VirtualClock::time_point>(start);
    }
}

void
HerderSCPDriver::recordSCPExternalizeEvent(uint64_t slotIndex, NodeID const& id,
                                           bool forceUpdateSelf)
{
    auto& timing = mSCPExecutionTimes[slotIndex];
    auto now = mApp.getClock().now();

    if (!timing.mFirstExternalize)
    {
        timing.mFirstExternalize = make_optional<VirtualClock::time_point>(now);
    }

    if (id == mSCP.getLocalNodeID())
    {
        if (!timing.mSelfExternalize)
        {
            recordLogTiming(*timing.mFirstExternalize, now,
                            mSCPMetrics.mFirstToSelfExternalizeLag,
                            "first to self externalize lag",
                            std::chrono::nanoseconds::zero(), slotIndex);
        }
        if (!timing.mSelfExternalize || forceUpdateSelf)
        {
            timing.mSelfExternalize =
                make_optional<VirtualClock::time_point>(now);
        }
    }
    else
    {
        // Record externalize delay
        if (timing.mSelfExternalize)
        {
            recordLogTiming(
                *timing.mSelfExternalize, now,
                mSCPMetrics.mSelfToOthersExternalizeLag,
                fmt::format("self to {} externalize lag", toShortString(id)),
                std::chrono::nanoseconds::zero(), slotIndex);
        }

        // Record lag for other nodes
        auto& lag = mQSetLag[id];
        recordLogTiming(
            *timing.mFirstExternalize, now, lag,
            fmt::format("first to {} externalize lag", toShortString(id)),
            std::chrono::nanoseconds::zero(), slotIndex);
    }
}

void
HerderSCPDriver::recordLogTiming(VirtualClock::time_point start,
                                 VirtualClock::time_point end,
                                 medida::Timer& timer,
                                 std::string const& logStr,
                                 std::chrono::nanoseconds threshold,
                                 uint64_t slotIndex)
{
    auto delta =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    if (Logging::logDebug("Herder"))
    {
        auto msCount =
            std::chrono::duration_cast<std::chrono::milliseconds>(delta)
                .count();
        CLOG_DEBUG(Herder, "{} delta for slot {} is {} ms", logStr, slotIndex,
                   msCount);
    }
    if (delta >= threshold)
    {
        timer.Update(delta);
    }
};

void
HerderSCPDriver::recordSCPExecutionMetrics(uint64_t slotIndex)
{
    auto externalizeStart = mApp.getClock().now();

    // Use threshold of 0 in case of a single node
    auto& qset = mApp.getConfig().QUORUM_SET;
    auto isSingleNode = qset.innerSets.size() == 0 &&
                        qset.validators.size() == 1 &&
                        qset.validators[0] == getSCP().getLocalNodeID();
    auto threshold = isSingleNode ? std::chrono::nanoseconds::zero()
                                  : Herder::TIMERS_THRESHOLD_NANOSEC;

    auto SCPTimingIt = mSCPExecutionTimes.find(slotIndex);
    if (SCPTimingIt == mSCPExecutionTimes.end())
    {
        return;
    }

    auto& SCPTiming = SCPTimingIt->second;

    mNominateTimeout.Update(SCPTiming.mNominationTimeoutCount);
    mPrepareTimeout.Update(SCPTiming.mPrepareTimeoutCount);

    // Compute nomination time
    if (SCPTiming.mNominationStart && SCPTiming.mPrepareStart)
    {
        recordLogTiming(*SCPTiming.mNominationStart, *SCPTiming.mPrepareStart,
                        mSCPMetrics.mNominateToPrepare, "Nominate", threshold,
                        slotIndex);
    }

    // Compute prepare time
    if (SCPTiming.mPrepareStart)
    {
        recordLogTiming(*SCPTiming.mPrepareStart, externalizeStart,
                        mSCPMetrics.mPrepareToExternalize, "Prepare", threshold,
                        slotIndex);
    }
}

void
HerderSCPDriver::purgeSlots(uint64_t maxSlotIndex)
{
    // Clean up timings map
    auto it = mSCPExecutionTimes.begin();
    while (it != mSCPExecutionTimes.end() && it->first < maxSlotIndex)
    {
        it = mSCPExecutionTimes.erase(it);
    }

    getSCP().purgeSlots(maxSlotIndex);
}

void
HerderSCPDriver::clearSCPExecutionEvents()
{
    mSCPExecutionTimes.clear();
}

// Value handling
class SCPHerderValueWrapper : public ValueWrapper
{
    HerderImpl& mHerder;

    TxSetFramePtr mTxSet;

  public:
    explicit SCPHerderValueWrapper(StellarValue const& sv, Value const& value,
                                   HerderImpl& herder)
        : ValueWrapper(value), mHerder(herder)
    {
        mTxSet = mHerder.getTxSet(sv.txSetHash);
        if (!mTxSet)
        {
            throw std::runtime_error(fmt::format(
                "SCPHerderValueWrapper tried to bind an unknown tx set {}",
                hexAbbrev(sv.txSetHash)));
        }
    }
};

ValueWrapperPtr
HerderSCPDriver::wrapValue(Value const& val)
{
    StellarValue sv;
    auto b = mHerder.getHerderSCPDriver().toStellarValue(val, sv);
    if (!b)
    {
        throw std::runtime_error(fmt::format(
            "Invalid value in SCPHerderValueWrapper {}", binToHex(val)));
    }
    auto res = std::make_shared<SCPHerderValueWrapper>(sv, val, mHerder);
    return res;
}

ValueWrapperPtr
HerderSCPDriver::wrapStellarValue(StellarValue const& sv)
{
    auto val = xdr::xdr_to_opaque(sv);
    auto res = std::make_shared<SCPHerderValueWrapper>(sv, val, mHerder);
    return res;
}
}
