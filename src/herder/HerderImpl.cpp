// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "herder/HerderUtils.h"
#include "herder/LedgerCloseData.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerManager.h"
#include "lib/json/json.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/PersistentState.h"
#include "overlay/OverlayManager.h"
#include "scp/LocalNode.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/XDRStream.h"
#include "util/basen.h"
#include "xdrpp/marshal.h"

#include <ctime>

using namespace std;
using namespace soci;

namespace stellar
{

std::unique_ptr<Herder>
Herder::create(Application& app)
{
    return make_unique<HerderImpl>(app);
}

HerderImpl::SCPMetrics::SCPMetrics(Application& app)
    : mValueValid(app.getMetrics().NewMeter({"scp", "value", "valid"}, "value"))
    , mValueInvalid(
          app.getMetrics().NewMeter({"scp", "value", "invalid"}, "value"))
    , mNominatingValue(
          app.getMetrics().NewMeter({"scp", "value", "nominating"}, "value"))
    , mValueExternalize(
          app.getMetrics().NewMeter({"scp", "value", "externalize"}, "value"))
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
    , mBallotExpire(
          app.getMetrics().NewMeter({"scp", "ballot", "expire"}, "ballot"))

    , mQuorumHeard(
          app.getMetrics().NewMeter({"scp", "quorum", "heard"}, "quorum"))

    , mLostSync(app.getMetrics().NewMeter({"scp", "sync", "lost"}, "sync"))

    , mEnvelopeEmit(
          app.getMetrics().NewMeter({"scp", "envelope", "emit"}, "envelope"))
    , mEnvelopeReceive(
          app.getMetrics().NewMeter({"scp", "envelope", "receive"}, "envelope"))
    , mEnvelopeSign(
          app.getMetrics().NewMeter({"scp", "envelope", "sign"}, "envelope"))
    , mEnvelopeValidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "validsig"}, "envelope"))
    , mEnvelopeInvalidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "invalidsig"}, "envelope"))

    , mKnownSlotsSize(
          app.getMetrics().NewCounter({"scp", "memory", "known-slots"}))
    , mCumulativeStatements(app.getMetrics().NewCounter(
          {"scp", "memory", "cumulative-statements"}))

    , mHerderStateCurrent(
          app.getMetrics().NewCounter({"herder", "state", "current"}))
    , mHerderStateChanges(
          app.getMetrics().NewTimer({"herder", "state", "changes"}))

    , mHerderPendingTxs0(
          app.getMetrics().NewCounter({"herder", "pending-txs", "age0"}))
    , mHerderPendingTxs1(
          app.getMetrics().NewCounter({"herder", "pending-txs", "age1"}))
    , mHerderPendingTxs2(
          app.getMetrics().NewCounter({"herder", "pending-txs", "age2"}))
    , mHerderPendingTxs3(
          app.getMetrics().NewCounter({"herder", "pending-txs", "age3"}))
{
}

HerderImpl::HerderImpl(Application& app)
    : mSCP(*this, app.getConfig().NODE_SEED, app.getConfig().NODE_IS_VALIDATOR,
           app.getConfig().QUORUM_SET)
    , mPendingTransactions(4)
    , mPendingEnvelopes(app, *this)
    , mLastSlotSaved(0)
    , mLastStateChange(app.getClock().now())
    , mTrackingTimer(app)
    , mLastTrigger(app.getClock().now())
    , mTriggerTimer(app)
    , mRebroadcastTimer(app)
    , mApp(app)
    , mLedgerManager(app.getLedgerManager())
    , mSCPMetrics(app)
{
    Hash hash = mSCP.getLocalNode()->getQuorumSetHash();
    mPendingEnvelopes.addSCPQuorumSet(hash, 0,
                                      mSCP.getLocalNode()->getQuorumSet());
}

HerderImpl::~HerderImpl()
{
}

Herder::State
HerderImpl::getState() const
{
    return (mTrackingSCP && mLastTrackingSCP) ? HERDER_TRACKING_STATE
                                              : HERDER_SYNCING_STATE;
}

void
HerderImpl::syncMetrics()
{
    int64_t c = mSCPMetrics.mHerderStateCurrent.count();
    int64_t n = static_cast<int64_t>(getState());
    if (c != n)
    {
        mSCPMetrics.mHerderStateCurrent.set_count(n);
    }
}

std::string
HerderImpl::getStateHuman() const
{
    static const char* stateStrings[HERDER_NUM_STATE] = {
        "HERDER_SYNCING_STATE", "HERDER_TRACKING_STATE"};
    return std::string(stateStrings[getState()]);
}

void
HerderImpl::stateChanged()
{
    mSCPMetrics.mHerderStateCurrent.set_count(static_cast<int64_t>(getState()));
    auto now = mApp.getClock().now();
    mSCPMetrics.mHerderStateChanges.Update(now - mLastStateChange);
    mLastStateChange = now;
    mApp.syncOwnMetrics();
}

void
HerderImpl::bootstrap()
{
    CLOG(INFO, "Herder") << "Force joining SCP with local state";
    assert(mSCP.isValidator());
    assert(mApp.getConfig().FORCE_SCP);

    mLedgerManager.setState(LedgerManager::LM_SYNCED_STATE);
    stateChanged();

    mLastTrigger = mApp.getClock().now() - Herder::EXP_LEDGER_TIMESPAN_SECONDS;
    ledgerClosed();
}

bool
HerderImpl::isSlotCompatibleWithCurrentState(uint64 slotIndex)
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
HerderImpl::validateValueHelper(uint64 slotIndex, StellarValue const& b)
{
    uint64 lastCloseTime;

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
                << "HerderImpl::validateValue"
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
    if (b.closeTime > timeNow + MAX_TIME_SLIP_SECONDS.count())
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
        CLOG(ERROR, "Herder") << "HerderImpl::validateValue"
                              << " i: " << slotIndex << " txSet not found?";

        res = SCPDriver::kInvalidValue;
    }
    else if (!txSet->checkValid(mApp))
    {
        if (Logging::logDebug("Herder"))
            CLOG(DEBUG, "Herder") << "HerderImpl::validateValue"
                                  << " i: " << slotIndex << " Invalid txSet:"
                                  << " " << hexAbbrev(txSet->getContentsHash());
        res = SCPDriver::kInvalidValue;
    }
    else
    {
        if (Logging::logDebug("Herder"))
            CLOG(DEBUG, "Herder")
                << "HerderImpl::validateValue"
                << " i: " << slotIndex
                << " txSet: " << hexAbbrev(txSet->getContentsHash()) << " OK";
        res = SCPDriver::kFullyValidatedValue;
    }
    return res;
}

bool
HerderImpl::validateUpgradeStep(uint64 slotIndex, UpgradeType const& upgrade,
                                LedgerUpgradeType& upgradeType)
{
    LedgerUpgrade lupgrade;

    try
    {
        xdr::xdr_from_opaque(upgrade, lupgrade);
    }
    catch (xdr::xdr_runtime_error&)
    {
        return false;
    }

    bool res;
    switch (lupgrade.type())
    {
    case LEDGER_UPGRADE_VERSION:
    {
        uint32 newVersion = lupgrade.newLedgerVersion();
        res = (newVersion == mApp.getConfig().LEDGER_PROTOCOL_VERSION);
    }
    break;
    case LEDGER_UPGRADE_BASE_FEE:
    {
        uint32 newFee = lupgrade.newBaseFee();
        // allow fee to move within a 2x distance from the one we have in our
        // config
        res = (newFee >= mApp.getConfig().DESIRED_BASE_FEE * .5) &&
              (newFee <= mApp.getConfig().DESIRED_BASE_FEE * 2);
    }
    break;
    case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
    {
        // allow max to be within 30% of the config value
        uint32 newMax = lupgrade.newMaxTxSetSize();
        res = (newMax >= mApp.getConfig().DESIRED_MAX_TX_PER_LEDGER * 7 / 10) &&
              (newMax <= mApp.getConfig().DESIRED_MAX_TX_PER_LEDGER * 13 / 10);
    }
    break;
    default:
        res = false;
    }
    if (res)
    {
        upgradeType = lupgrade.type();
    }
    return res;
}

void
HerderImpl::signEnvelope(SCPEnvelope& envelope)
{
    mSCPMetrics.mEnvelopeSign.Mark();
    envelope.signature = mSCP.getSecretKey().sign(xdr::xdr_to_opaque(
        mApp.getNetworkID(), ENVELOPE_TYPE_SCP, envelope.statement));
}

bool
HerderImpl::verifyEnvelope(SCPEnvelope const& envelope)
{
    bool b = PubKeyUtils::verifySig(
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

SCPDriver::ValidationLevel
HerderImpl::validateValue(uint64 slotIndex, Value const& value)
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
            if (!validateUpgradeStep(slotIndex, b.upgrades[i], thisUpgradeType))
            {
                CLOG(TRACE, "Herder")
                    << "HerderImpl::validateValue invalid step at index " << i;
                res = SCPDriver::kInvalidValue;
            }
            if (i != 0 && (lastUpgradeType >= thisUpgradeType))
            {
                CLOG(TRACE, "Herder") << "HerderImpl::validateValue out of "
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
HerderImpl::extractValidValue(uint64 slotIndex, Value const& value)
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

            if (!validateUpgradeStep(slotIndex, *it, thisUpgradeType))
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

std::string
HerderImpl::toShortString(PublicKey const& pk) const
{
    return mApp.getConfig().toShortString(pk);
}

std::string
HerderImpl::getValueString(Value const& v) const
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

void
HerderImpl::ballotDidHearFromQuorum(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mQuorumHeard.Mark();
}

void
HerderImpl::updateSCPCounters()
{
    mSCPMetrics.mKnownSlotsSize.set_count(mSCP.getKnownSlotsCount());
    mSCPMetrics.mCumulativeStatements.set_count(
        mSCP.getCumulativeStatemtCount());
}

static uint64_t
countTxs(HerderImpl::AccountTxMap const& acc)
{
    uint64_t sz = 0;
    for (auto const& a : acc)
    {
        sz += a.second->mTransactions.size();
    }
    return sz;
}

static std::shared_ptr<HerderImpl::TxMap>
findOrAdd(HerderImpl::AccountTxMap& acc, AccountID const& aid)
{
    std::shared_ptr<HerderImpl::TxMap> txmap = nullptr;
    auto i = acc.find(aid);
    if (i == acc.end())
    {
        txmap = std::make_shared<HerderImpl::TxMap>();
        acc.insert(std::make_pair(aid, txmap));
    }
    else
    {
        txmap = i->second;
    }
    return txmap;
}

void
HerderImpl::logQuorumInformation(uint64 index)
{
    std::string res;
    Json::Value v;
    dumpQuorumInfo(v, mSCP.getLocalNodeID(), true, index);
    auto slots = v.get("slots", "");
    if (!slots.empty())
    {
        std::string indexs = std::to_string(static_cast<uint32>(index));
        auto i = slots.get(indexs, "");
        if (!i.empty())
        {
            Json::FastWriter fw;
            CLOG(INFO, "Herder") << "Quorum information for " << index << " : "
                                 << fw.write(i);
        }
    }
}

void
HerderImpl::valueExternalized(uint64 slotIndex, Value const& value)
{
    updateSCPCounters();
    mSCPMetrics.mValueExternalize.Mark();

    auto it = mSCPTimers.begin(); // cancel all timers below this slot
    while (it != mSCPTimers.end() && it->first <= slotIndex)
    {
        it = mSCPTimers.erase(it);
    }

    if (slotIndex <= getCurrentLedgerSeq())
    {
        // externalize may trigger on older slots:
        //  * when the current instance starts up
        //  * when getting back in sync (a gap potentially opened)
        // in both cases it's safe to just ignore those as we're already
        // tracking a more recent state
        CLOG(DEBUG, "Herder") << "Ignoring old ledger externalize "
                              << slotIndex;
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
        CLOG(ERROR, "Herder") << "HerderImpl::valueExternalized"
                              << " Externalized StellarValue malformed";
        // no point in continuing as 'b' contains garbage at this point
        abort();
    }

    Hash const& txSetHash = b.txSetHash;

    if (Logging::logDebug("Herder"))
        CLOG(DEBUG, "Herder") << "HerderImpl::valueExternalized"
                              << " txSet: " << hexAbbrev(txSetHash);

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

    trackingHeartBeat();

    TxSetFramePtr externalizedSet = mPendingEnvelopes.getTxSet(txSetHash);

    // trigger will be recreated when the ledger is closed
    // we do not want it to trigger while downloading the current set
    // and there is no point in taking a position after the round is over
    mTriggerTimer.cancel();

    // save the SCP messages in the database
    saveSCPHistory(slotIndex);

    // tell the LedgerManager that this value got externalized
    // LedgerManager will perform the proper action based on its internal
    // state: apply, trigger catchup, etc
    LedgerCloseData ledgerData(lastConsensusLedgerIndex(), externalizedSet, b);
    mLedgerManager.externalizeValue(ledgerData);

    // perform cleanups
    updatePendingTransactions(externalizedSet->mTransactions);

    // Evict slots that are outside of our ledger validity bracket
    if (slotIndex > MAX_SLOTS_TO_REMEMBER)
    {
        mSCP.purgeSlots(slotIndex - MAX_SLOTS_TO_REMEMBER);
    }

    ledgerClosed();
}

void
HerderImpl::nominatingValue(uint64 slotIndex, Value const& value)
{
    if (Logging::logDebug("Herder"))
        CLOG(DEBUG, "Herder") << "nominatingValue i:" << slotIndex
                              << " v: " << getValueString(value);

    if (!value.empty())
    {
        mSCPMetrics.mNominatingValue.Mark();
    }
}

Value
HerderImpl::combineCandidates(uint64 slotIndex,
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
                    if (clUpgrade.newLedgerVersion() <
                        lupgrade.newLedgerVersion())
                    {
                        clUpgrade.newLedgerVersion() =
                            lupgrade.newLedgerVersion();
                    }
                    break;
                case LEDGER_UPGRADE_BASE_FEE:
                    // take the max fee
                    if (clUpgrade.newBaseFee() < lupgrade.newBaseFee())
                    {
                        clUpgrade.newBaseFee() = lupgrade.newBaseFee();
                    }
                    break;
                case LEDGER_UPGRADE_MAX_TX_SET_SIZE:
                    // take the max tx set size
                    if (clUpgrade.newMaxTxSetSize() <
                        lupgrade.newMaxTxSetSize())
                    {
                        clUpgrade.newMaxTxSetSize() =
                            lupgrade.newMaxTxSetSize();
                    }
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
            TxSetFramePtr cTxSet = getTxSet(sv.txSetHash);

            if (cTxSet && cTxSet->previousLedgerHash() == lcl.hash)
            {
                if (!highestTxSet || (cTxSet->mTransactions.size() >
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
HerderImpl::setupTimer(uint64 slotIndex, int timerID,
                       std::chrono::milliseconds timeout,
                       std::function<void()> cb)
{
    // don't setup timers for old slots
    if (slotIndex <= getCurrentLedgerSeq())
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

void
HerderImpl::rebroadcast()
{
    for (auto const& e :
         mSCP.getLatestMessagesSend(mLedgerManager.getLedgerNum()))
    {
        broadcast(e);
    }
    startRebroadcastTimer();
}

void
HerderImpl::broadcast(SCPEnvelope const& e)
{
    if (!mApp.getConfig().MANUAL_CLOSE)
    {
        StellarMessage m;
        m.type(SCP_MESSAGE);
        m.envelope() = e;

        CLOG(DEBUG, "Herder") << "broadcast "
                              << " s:" << e.statement.pledges.type()
                              << " i:" << e.statement.slotIndex;

        mSCPMetrics.mEnvelopeEmit.Mark();
        mApp.getOverlayManager().broadcastMessage(m, true);
    }
}

void
HerderImpl::startRebroadcastTimer()
{
    mRebroadcastTimer.expires_from_now(std::chrono::seconds(2));

    mRebroadcastTimer.async_wait(std::bind(&HerderImpl::rebroadcast, this),
                                 &VirtualTimer::onFailureNoop);
}

void
HerderImpl::emitEnvelope(SCPEnvelope const& envelope)
{
    uint64 slotIndex = envelope.statement.slotIndex;

    if (Logging::logDebug("Herder"))
        CLOG(DEBUG, "Herder") << "emitEnvelope"
                              << " s:" << envelope.statement.pledges.type()
                              << " i:" << slotIndex
                              << " a:" << mApp.getStateHuman();

    persistSCPState(slotIndex);

    broadcast(envelope);

    // this resets the re-broadcast timer
    startRebroadcastTimer();
}

void
HerderImpl::TxMap::addTx(TransactionFramePtr tx)
{
    auto const& h = tx->getFullHash();
    if (mTransactions.find(h) != mTransactions.end())
    {
        return;
    }
    mTransactions.insert(std::make_pair(h, tx));
    mMaxSeq = std::max(tx->getSeqNum(), mMaxSeq);
    mTotalFees += tx->getFee();
}

void
HerderImpl::TxMap::recalculate()
{
    mMaxSeq = 0;
    mTotalFees = 0;
    for (auto const& pair : mTransactions)
    {
        mMaxSeq = std::max(pair.second->getSeqNum(), mMaxSeq);
        mTotalFees += pair.second->getFee();
    }
}

Herder::TransactionSubmitStatus
HerderImpl::recvTransaction(TransactionFramePtr tx)
{
    soci::transaction sqltx(mApp.getDatabase().getSession());
    mApp.getDatabase().setCurrentTransactionReadOnly();

    auto const& acc = tx->getSourceID();
    auto const& txID = tx->getFullHash();

    // determine if we have seen this tx before and if not if it has the right
    // seq num
    int64_t totFee = tx->getFee();
    SequenceNumber highSeq = 0;

    for (auto& map : mPendingTransactions)
    {
        auto i = map.find(acc);
        if (i != map.end())
        {
            auto& txmap = i->second;
            auto j = txmap->mTransactions.find(txID);
            if (j != txmap->mTransactions.end())
            {
                return TX_STATUS_DUPLICATE;
            }
            totFee += txmap->mTotalFees;
            highSeq = std::max(highSeq, txmap->mMaxSeq);
        }
    }

    if (!tx->checkValid(mApp, highSeq))
    {
        return TX_STATUS_ERROR;
    }

    if (tx->getSourceAccount().getBalanceAboveReserve(mLedgerManager) < totFee)
    {
        tx->getResult().result.code(txINSUFFICIENT_BALANCE);
        return TX_STATUS_ERROR;
    }

    if (Logging::logTrace("Herder"))
        CLOG(TRACE, "Herder") << "recv transaction " << hexAbbrev(txID)
                              << " for " << KeyUtils::toShortString(acc);

    auto txmap = findOrAdd(mPendingTransactions[0], acc);
    txmap->addTx(tx);

    return TX_STATUS_PENDING;
}

Herder::EnvelopeStatus
HerderImpl::recvSCPEnvelope(SCPEnvelope const& envelope)
{
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    if (Logging::logDebug("Herder"))
        CLOG(DEBUG, "Herder")
            << "recvSCPEnvelope"
            << " from: "
            << mApp.getConfig().toShortString(envelope.statement.nodeID)
            << " s:" << envelope.statement.pledges.type()
            << " i:" << envelope.statement.slotIndex
            << " a:" << mApp.getStateHuman();

    if (envelope.statement.nodeID == mSCP.getLocalNode()->getNodeID())
    {
        CLOG(DEBUG, "Herder") << "recvSCPEnvelope: skipping own message";
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    mSCPMetrics.mEnvelopeReceive.Mark();

    uint32_t minLedgerSeq = getCurrentLedgerSeq();
    if (minLedgerSeq > MAX_SLOTS_TO_REMEMBER)
    {
        minLedgerSeq -= MAX_SLOTS_TO_REMEMBER;
    }

    uint32_t maxLedgerSeq = std::numeric_limits<uint32>::max();

    if (mTrackingSCP)
    {
        // when tracking, we can filter messages based on the information we got
        // from consensus for the max ledger

        // note that this filtering will cause a node on startup
        // to potentially drop messages outside of the bracket
        // causing it to discard CONSENSUS_STUCK_TIMEOUT_SECONDS worth of
        // ledger closing
        maxLedgerSeq = nextConsensusLedgerIndex() + LEDGER_VALIDITY_BRACKET;
    }

    // If envelopes are out of our validity brackets, we just ignore them.
    if (envelope.statement.slotIndex > maxLedgerSeq ||
        envelope.statement.slotIndex < minLedgerSeq)
    {
        CLOG(DEBUG, "Herder") << "Ignoring SCPEnvelope outside of range: "
                              << envelope.statement.slotIndex << "( "
                              << minLedgerSeq << "," << maxLedgerSeq << ")";
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    auto status = mPendingEnvelopes.recvSCPEnvelope(envelope);
    if (status == Herder::ENVELOPE_STATUS_READY)
    {
        processSCPQueue();
    }
    return status;
}

void
HerderImpl::sendSCPStateToPeer(uint32 ledgerSeq, PeerPtr peer)
{
    uint32 minSeq, maxSeq;

    if (ledgerSeq == 0)
    {
        const uint32 nbLedgers = 3;
        const uint32 minLedger = 2;

        // include the most recent slot
        maxSeq = getCurrentLedgerSeq() + 1;

        if (maxSeq >= minLedger + nbLedgers)
        {
            minSeq = maxSeq - nbLedgers;
        }
        else
        {
            minSeq = minLedger;
        }
    }
    else
    {
        minSeq = maxSeq = ledgerSeq;
    }

    // use uint64_t for seq to prevent overflows
    for (uint64_t seq = minSeq; seq <= maxSeq; seq++)
    {
        auto const& envelopes = mSCP.getCurrentState(seq);

        if (envelopes.size() != 0)
        {
            CLOG(DEBUG, "Herder") << "Send state " << envelopes.size()
                                  << " for ledger " << seq;

            for (auto const& e : envelopes)
            {
                StellarMessage m;
                m.type(SCP_MESSAGE);
                m.envelope() = e;
                peer->sendMessage(m);
            }
        }
    }
}

void
HerderImpl::processSCPQueue()
{
    if (mTrackingSCP)
    {
        // drop obsolete slots
        if (nextConsensusLedgerIndex() > MAX_SLOTS_TO_REMEMBER)
        {
            mPendingEnvelopes.eraseBelow(nextConsensusLedgerIndex() -
                                         MAX_SLOTS_TO_REMEMBER);
        }

        processSCPQueueUpToIndex(nextConsensusLedgerIndex());
    }
    else
    {
        // we don't know which ledger we're in
        // try to consume the messages from the queue
        // starting from the smallest slot
        for (auto& slot : mPendingEnvelopes.readySlots())
        {
            processSCPQueueUpToIndex(slot);
            if (mTrackingSCP)
            {
                // one of the slots externalized
                // we go back to regular flow
                break;
            }
        }
    }
}

void
HerderImpl::processSCPQueueUpToIndex(uint64 slotIndex)
{
    while (true)
    {
        SCPEnvelope env;
        if (mPendingEnvelopes.pop(slotIndex, env))
        {
            mSCP.receiveEnvelope(env);
        }
        else
        {
            return;
        }
    }
}

void
HerderImpl::ledgerClosed()
{
    mTriggerTimer.cancel();

    updateSCPCounters();
    CLOG(TRACE, "Herder") << "HerderImpl::ledgerClosed";

    mPendingEnvelopes.slotClosed(lastConsensusLedgerIndex());

    mApp.getOverlayManager().ledgerClosed(lastConsensusLedgerIndex());

    uint64_t nextIndex = nextConsensusLedgerIndex();

    // process any statements up to this slot (this may trigger externalize)
    processSCPQueueUpToIndex(nextIndex);

    // if externalize got called for a future slot, we don't
    // need to trigger (the now obsolete) next round
    if (nextIndex != nextConsensusLedgerIndex())
    {
        return;
    }

    // If we are not a validating node and just watching SCP we don't call
    // triggerNextLedger. Likewise if we are not in synced state.
    if (!mSCP.isValidator())
    {
        CLOG(DEBUG, "Herder")
            << "Non-validating node, not triggering ledger-close.";
        return;
    }

    if (!mLedgerManager.isSynced())
    {
        CLOG(DEBUG, "Herder")
            << "Not presently synced, not triggering ledger-close.";
        return;
    }

    auto seconds = Herder::EXP_LEDGER_TIMESPAN_SECONDS;
    if (mApp.getConfig().ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
    {
        seconds = std::chrono::seconds(1);
    }
    if (mApp.getConfig().ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING)
    {
        seconds = std::chrono::seconds(
            mApp.getConfig().ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING);
    }

    auto now = mApp.getClock().now();
    if ((now - mLastTrigger) < seconds)
    {
        auto timeout = seconds - (now - mLastTrigger);
        mTriggerTimer.expires_from_now(timeout);
    }
    else
    {
        mTriggerTimer.expires_from_now(std::chrono::nanoseconds(0));
    }

    if (!mApp.getConfig().MANUAL_CLOSE)
        mTriggerTimer.async_wait(std::bind(&HerderImpl::triggerNextLedger, this,
                                           static_cast<uint32_t>(nextIndex)),
                                 &VirtualTimer::onFailureNoop);
}

void
HerderImpl::removeReceivedTxs(std::vector<TransactionFramePtr> const& dropTxs)
{
    for (auto& m : mPendingTransactions)
    {
        if (m.empty())
        {
            continue;
        }

        std::set<std::shared_ptr<TxMap>> toRecalculate;

        for (auto const& tx : dropTxs)
        {
            auto const& acc = tx->getSourceID();
            auto const& txID = tx->getFullHash();
            auto i = m.find(acc);
            if (i != m.end())
            {
                auto& txs = i->second->mTransactions;
                auto j = txs.find(txID);
                if (j != txs.end())
                {
                    txs.erase(j);
                    if (txs.empty())
                    {
                        m.erase(i);
                    }
                    else
                    {
                        toRecalculate.insert(i->second);
                    }
                }
            }
        }

        for (auto txm : toRecalculate)
        {
            txm->recalculate();
        }
    }
}

bool
HerderImpl::recvSCPQuorumSet(Hash const& hash, const SCPQuorumSet& qset)
{
    return mPendingEnvelopes.recvSCPQuorumSet(hash, qset);
}

bool
HerderImpl::recvTxSet(Hash const& hash, const TxSetFrame& t)
{
    TxSetFramePtr txset(new TxSetFrame(t));
    return mPendingEnvelopes.recvTxSet(hash, txset);
}

void
HerderImpl::peerDoesntHave(MessageType type, uint256 const& itemID,
                           PeerPtr peer)
{
    mPendingEnvelopes.peerDoesntHave(type, itemID, peer);
}

TxSetFramePtr
HerderImpl::getTxSet(Hash const& hash)
{
    return mPendingEnvelopes.getTxSet(hash);
}

SCPQuorumSetPtr
HerderImpl::getQSet(Hash const& qSetHash)
{
    return mPendingEnvelopes.getQSet(qSetHash);
}

uint32_t
HerderImpl::getCurrentLedgerSeq() const
{
    uint32_t res = mLedgerManager.getLastClosedLedgerNum();

    if (mTrackingSCP && res < mTrackingSCP->mConsensusIndex)
    {
        res = static_cast<uint32_t>(mTrackingSCP->mConsensusIndex);
    }
    if (mLastTrackingSCP && res < mLastTrackingSCP->mConsensusIndex)
    {
        res = static_cast<uint32_t>(mLastTrackingSCP->mConsensusIndex);
    }
    return res;
}

SequenceNumber
HerderImpl::getMaxSeqInPendingTxs(AccountID const& acc)
{
    SequenceNumber highSeq = 0;
    for (auto const& m : mPendingTransactions)
    {
        auto i = m.find(acc);
        if (i == m.end())
        {
            continue;
        }
        highSeq = std::max(i->second->mMaxSeq, highSeq);
    }
    return highSeq;
}

// called to take a position during the next round
// uses the state in LedgerManager to derive a starting position
void
HerderImpl::triggerNextLedger(uint32_t ledgerSeqToTrigger)
{
    if (!mTrackingSCP || !mLedgerManager.isSynced())
    {
        CLOG(DEBUG, "Herder") << "triggerNextLedger: skipping (out of sync) : "
                              << mApp.getStateHuman();
        return;
    }
    updateSCPCounters();

    // our first choice for this round's set is all the tx we have collected
    // during last ledger close
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    TxSetFramePtr proposedSet = std::make_shared<TxSetFrame>(lcl.hash);

    for (auto const& m : mPendingTransactions)
    {
        for (auto const& pair : m)
        {
            for (auto const& tx : pair.second->mTransactions)
            {
                proposedSet->add(tx.second);
            }
        }
    }

    std::vector<TransactionFramePtr> removed;
    proposedSet->trimInvalid(mApp, removed);
    removeReceivedTxs(removed);

    proposedSet->surgePricingFilter(mLedgerManager);

    if (!proposedSet->checkValid(mApp))
    {
        throw std::runtime_error("wanting to emit an invalid txSet");
    }

    auto txSetHash = proposedSet->getContentsHash();

    // use the slot index from ledger manager here as our vote is based off
    // the last closed ledger stored in ledger manager
    uint32_t slotIndex = lcl.header.ledgerSeq + 1;

    // Inform the item fetcher so queries from other peers about his txSet
    // can be answered. Note this can trigger SCP callbacks, externalize, etc
    // if we happen to build a txset that we were trying to download.
    mPendingEnvelopes.addTxSet(txSetHash, slotIndex, proposedSet);

    // no point in sending out a prepare:
    // externalize was triggered on a more recent ledger
    if (ledgerSeqToTrigger != slotIndex)
    {
        return;
    }

    // We store at which time we triggered consensus
    mLastTrigger = mApp.getClock().now();

    // We pick as next close time the current time unless it's before the last
    // close time. We don't know how much time it will take to reach consensus
    // so this is the most appropriate value to use as closeTime.
    uint64_t nextCloseTime = VirtualClock::to_time_t(mLastTrigger);
    if (nextCloseTime <= lcl.header.scpValue.closeTime)
    {
        nextCloseTime = lcl.header.scpValue.closeTime + 1;
    }

    StellarValue newProposedValue(txSetHash, nextCloseTime, emptyUpgradeSteps,
                                  0);

    // see if we need to include some upgrades
    auto upgrades = prepareUpgrades(lcl.header);

    for (auto const& upgrade : upgrades)
    {
        Value v(xdr::xdr_to_opaque(upgrade));
        if (v.size() >= UpgradeType::max_size())
        {
            CLOG(ERROR, "Herder") << "HerderImpl::triggerNextLedger"
                                  << " exceeded size for upgrade step (got "
                                  << v.size() << " ) for upgrade type "
                                  << std::to_string(upgrade.type());
        }
        else
        {
            newProposedValue.upgrades.emplace_back(v.begin(), v.end());
        }
    }

    mCurrentValue = xdr::xdr_to_opaque(newProposedValue);
    mLedgerSeqNominating = slotIndex;

    uint256 valueHash = sha256(xdr::xdr_to_opaque(mCurrentValue));
    CLOG(DEBUG, "Herder") << "HerderImpl::triggerNextLedger"
                          << " txSet.size: "
                          << proposedSet->mTransactions.size()
                          << " previousLedgerHash: "
                          << hexAbbrev(proposedSet->previousLedgerHash())
                          << " value: " << hexAbbrev(valueHash)
                          << " slot: " << slotIndex;

    Value prevValue = xdr::xdr_to_opaque(lcl.header.scpValue);

    mSCP.nominate(slotIndex, mCurrentValue, prevValue);
}

std::vector<LedgerUpgrade>
HerderImpl::prepareUpgrades(const LedgerHeader& header) const
{
    auto result = std::vector<LedgerUpgrade>{};

    if (header.ledgerVersion != mApp.getConfig().LEDGER_PROTOCOL_VERSION)
    {
        auto timeForUpgrade =
            !mApp.getConfig().PREFERRED_UPGRADE_DATETIME ||
            VirtualClock::tmToPoint(
                *mApp.getConfig().PREFERRED_UPGRADE_DATETIME) <=
                mApp.getClock().now();
        if (timeForUpgrade)
        {
            result.emplace_back(LEDGER_UPGRADE_VERSION);
            result.back().newLedgerVersion() =
                mApp.getConfig().LEDGER_PROTOCOL_VERSION;
        }
    }
    if (header.baseFee != mApp.getConfig().DESIRED_BASE_FEE)
    {
        result.emplace_back(LEDGER_UPGRADE_BASE_FEE);
        result.back().newBaseFee() = mApp.getConfig().DESIRED_BASE_FEE;
    }
    if (header.maxTxSetSize != mApp.getConfig().DESIRED_MAX_TX_PER_LEDGER)
    {
        result.emplace_back(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        result.back().newMaxTxSetSize() =
            mApp.getConfig().DESIRED_MAX_TX_PER_LEDGER;
    }

    return result;
}

bool
HerderImpl::resolveNodeID(std::string const& s, PublicKey& retKey)
{
    bool r = mApp.getConfig().resolveNodeID(s, retKey);
    if (!r)
    {
        if (s.size() > 1 && s[0] == '@')
        {
            std::string arg = s.substr(1);
            // go through SCP messages of the previous ledger
            // (to increase the chances of finding the node)
            uint32 seq = getCurrentLedgerSeq();
            if (seq > 2)
            {
                seq--;
            }
            auto const& envelopes = mSCP.getCurrentState(seq);
            for (auto const& e : envelopes)
            {
                std::string curK = KeyUtils::toStrKey(e.statement.nodeID);
                if (curK.compare(0, arg.size(), arg) == 0)
                {
                    retKey = e.statement.nodeID;
                    r = true;
                    break;
                }
            }
        }
    }
    return r;
}

// Extra SCP methods overridden solely to increment metrics.
void
HerderImpl::updatedCandidateValue(uint64 slotIndex, Value const& value)
{
    mSCPMetrics.mUpdatedCandidate.Mark();
}

void
HerderImpl::startedBallotProtocol(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mStartBallotProtocol.Mark();
}
void
HerderImpl::acceptedBallotPrepared(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mAcceptedBallotPrepared.Mark();
}

void
HerderImpl::confirmedBallotPrepared(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mConfirmedBallotPrepared.Mark();
}

void
HerderImpl::acceptedCommit(uint64 slotIndex, SCPBallot const& ballot)
{
    mSCPMetrics.mAcceptedCommit.Mark();
}

void
HerderImpl::dumpInfo(Json::Value& ret, size_t limit)
{
    ret["you"] = mApp.getConfig().toStrKey(mSCP.getSecretKey().getPublicKey());

    mSCP.dumpInfo(ret, limit);

    mPendingEnvelopes.dumpInfo(ret, limit);
}

void
HerderImpl::dumpQuorumInfo(Json::Value& ret, NodeID const& id, bool summary,
                           uint64 index)
{
    ret["node"] = mApp.getConfig().toStrKey(id);

    mSCP.dumpQuorumInfo(ret["slots"], id, summary, index);
}

void
HerderImpl::persistSCPState(uint64 slot)
{
    if (slot < mLastSlotSaved)
    {
        return;
    }

    mLastSlotSaved = slot;

    // saves SCP messages and related data (transaction sets, quorum sets)
    xdr::xvector<SCPEnvelope> latestEnvs;
    std::map<Hash, TxSetFramePtr> txSets;
    std::map<Hash, SCPQuorumSetPtr> quorumSets;

    for (auto const& e : mSCP.getLatestMessagesSend(slot))
    {
        latestEnvs.emplace_back(e);

        // saves transaction sets referred by the statement
        for (auto const& h : getTxSetHashes(e))
        {
            auto txSet = mPendingEnvelopes.getTxSet(h);
            if (txSet)
            {
                txSets.insert(std::make_pair(h, txSet));
            }
        }
        Hash qsHash = Slot::getCompanionQuorumSetHashFromStatement(e.statement);
        SCPQuorumSetPtr qSet = mPendingEnvelopes.getQSet(qsHash);
        if (qSet)
        {
            quorumSets.insert(std::make_pair(qsHash, qSet));
        }
    }

    xdr::xvector<TransactionSet> latestTxSets;
    for (auto it : txSets)
    {
        latestTxSets.emplace_back();
        it.second->toXDR(latestTxSets.back());
    }

    xdr::xvector<SCPQuorumSet> latestQSets;
    for (auto it : quorumSets)
    {
        latestQSets.emplace_back(*it.second);
    }

    auto latestSCPData =
        xdr::xdr_to_opaque(latestEnvs, latestTxSets, latestQSets);
    std::string scpState;
    scpState = bn::encode_b64(latestSCPData);

    mApp.getPersistentState().setState(PersistentState::kLastSCPData, scpState);
}

void
HerderImpl::restoreSCPState()
{
    // setup a sufficient state that we can participate in consensus
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    mTrackingSCP =
        make_unique<ConsensusData>(lcl.header.ledgerSeq, lcl.header.scpValue);

    trackingHeartBeat();

    // load saved state from database
    auto latest64 =
        mApp.getPersistentState().getState(PersistentState::kLastSCPData);

    if (latest64.empty())
    {
        return;
    }

    std::vector<uint8_t> buffer;
    bn::decode_b64(latest64, buffer);

    xdr::xvector<SCPEnvelope> latestEnvs;
    xdr::xvector<TransactionSet> latestTxSets;
    xdr::xvector<SCPQuorumSet> latestQSets;

    try
    {
        xdr::xdr_from_opaque(buffer, latestEnvs, latestTxSets, latestQSets);

        for (auto const& txset : latestTxSets)
        {
            TxSetFramePtr cur =
                make_shared<TxSetFrame>(mApp.getNetworkID(), txset);
            Hash h = cur->getContentsHash();
            mPendingEnvelopes.addTxSet(h, 0, cur);
        }
        for (auto const& qset : latestQSets)
        {
            Hash hash = sha256(xdr::xdr_to_opaque(qset));
            mPendingEnvelopes.addSCPQuorumSet(hash, 0, qset);
        }
        for (auto const& e : latestEnvs)
        {
            mSCP.setStateFromEnvelope(e.statement.slotIndex, e);
        }

        if (latestEnvs.size() != 0)
        {
            mLastSlotSaved = latestEnvs.back().statement.slotIndex;
            startRebroadcastTimer();
        }
    }
    catch (std::exception& e)
    {
        // we may have exceptions when upgrading the protocol
        // this should be the only time we get exceptions decoding old messages.
        CLOG(INFO, "Herder") << "Error while restoring old scp messages, "
                                "proceeding without them : "
                             << e.what();
    }
}

void
HerderImpl::trackingHeartBeat()
{
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return;
    }

    assert(mTrackingSCP);
    mTrackingTimer.expires_from_now(
        std::chrono::seconds(CONSENSUS_STUCK_TIMEOUT_SECONDS));
    mTrackingTimer.async_wait(std::bind(&HerderImpl::herderOutOfSync, this),
                              &VirtualTimer::onFailureNoop);
}

void
HerderImpl::updatePendingTransactions(
    std::vector<TransactionFramePtr> const& applied)
{
    // remove all these tx from mPendingTransactions
    removeReceivedTxs(applied);

    // drop the highest level
    mPendingTransactions.erase(--mPendingTransactions.end());

    // shift entries up
    mPendingTransactions.emplace_front();

    // rebroadcast entries, sorted in apply-order to maximize chances of
    // propagation
    {
        Hash h;
        TxSetFrame toBroadcast(h);
        for (auto const& l : mPendingTransactions)
        {
            for (auto const& pair : l)
            {
                for (auto const& tx : pair.second->mTransactions)
                {
                    toBroadcast.add(tx.second);
                }
            }
        }
        for (auto tx : toBroadcast.sortForApply())
        {
            auto msg = tx->toStellarMessage();
            mApp.getOverlayManager().broadcastMessage(msg);
        }
    }

    mSCPMetrics.mHerderPendingTxs0.set_count(countTxs(mPendingTransactions[0]));
    mSCPMetrics.mHerderPendingTxs1.set_count(countTxs(mPendingTransactions[1]));
    mSCPMetrics.mHerderPendingTxs2.set_count(countTxs(mPendingTransactions[2]));
    mSCPMetrics.mHerderPendingTxs3.set_count(countTxs(mPendingTransactions[3]));
}

void
HerderImpl::herderOutOfSync()
{
    CLOG(INFO, "Herder") << "Lost track of consensus";

    Json::Value v;
    dumpInfo(v, 20);
    std::string s = v.toStyledString();
    CLOG(INFO, "Herder") << "Out of sync context: " << s;

    mSCPMetrics.mLostSync.Mark();
    stateChanged();

    // transfer ownership to mLastTrackingSCP
    mLastTrackingSCP.reset(mTrackingSCP.release());

    processSCPQueue();
}

void
HerderImpl::saveSCPHistory(uint64 index)
{
    uint32 seq = static_cast<uint32>(index);

    auto envs = mSCP.getExternalizingState(seq);
    if (!envs.empty())
    {
        std::unordered_map<Hash, SCPQuorumSetPtr> usedQSets;

        auto& db = mApp.getDatabase();

        soci::transaction txscope(db.getSession());

        {
            auto prepClean = db.getPreparedStatement(
                "DELETE FROM scphistory WHERE ledgerseq =:l");

            auto& st = prepClean.statement();
            st.exchange(use(seq));
            st.define_and_bind();
            {
                auto timer = db.getDeleteTimer("scphistory");
                st.execute(true);
            }
        }
        for (auto const& e : envs)
        {
            auto const& qHash =
                Slot::getCompanionQuorumSetHashFromStatement(e.statement);
            usedQSets.insert(std::make_pair(qHash, getQSet(qHash)));

            std::string nodeIDStrKey = KeyUtils::toStrKey(e.statement.nodeID);

            auto envelopeBytes(xdr::xdr_to_opaque(e));

            std::string envelopeEncoded;
            envelopeEncoded = bn::encode_b64(envelopeBytes);

            auto prepEnv =
                db.getPreparedStatement("INSERT INTO scphistory "
                                        "(nodeid, ledgerseq, envelope) VALUES "
                                        "(:n, :l, :e)");

            auto& st = prepEnv.statement();
            st.exchange(use(nodeIDStrKey));
            st.exchange(use(seq));
            st.exchange(use(envelopeEncoded));
            st.define_and_bind();
            {
                auto timer = db.getInsertTimer("scphistory");
                st.execute(true);
            }
            if (st.get_affected_rows() != 1)
            {
                throw std::runtime_error("Could not update data in SQL");
            }
        }

        for (auto const& p : usedQSets)
        {
            std::string qSetH = binToHex(p.first);

            auto prepUpQSet = db.getPreparedStatement(
                "UPDATE scpquorums SET "
                "lastledgerseq = :l WHERE qsethash = :h");

            auto& stUp = prepUpQSet.statement();
            stUp.exchange(use(seq));
            stUp.exchange(use(qSetH));
            stUp.define_and_bind();
            {
                auto timer = db.getInsertTimer("scpquorums");
                stUp.execute(true);
            }
            if (stUp.get_affected_rows() != 1)
            {
                auto qSetBytes(xdr::xdr_to_opaque(*p.second));

                std::string qSetEncoded;
                qSetEncoded = bn::encode_b64(qSetBytes);

                auto prepInsQSet = db.getPreparedStatement(
                    "INSERT INTO scpquorums "
                    "(qsethash, lastledgerseq, qset) VALUES "
                    "(:h, :l, :v);");

                auto& stIns = prepInsQSet.statement();
                stIns.exchange(use(qSetH));
                stIns.exchange(use(seq));
                stIns.exchange(use(qSetEncoded));
                stIns.define_and_bind();
                {
                    auto timer = db.getInsertTimer("scpquorums");
                    stIns.execute(true);
                }
                if (stIns.get_affected_rows() != 1)
                {
                    throw std::runtime_error("Could not update data in SQL");
                }
            }
        }

        txscope.commit();
    }
}

size_t
Herder::copySCPHistoryToStream(Database& db, soci::session& sess,
                               uint32_t ledgerSeq, uint32_t ledgerCount,
                               XDROutputFileStream& scpHistory)
{
    uint32_t begin = ledgerSeq, end = ledgerSeq + ledgerCount;
    size_t n = 0;

    // all known quorum sets
    std::unordered_map<Hash, SCPQuorumSet> qSets;

    for (uint32_t curLedgerSeq = begin; curLedgerSeq < end; curLedgerSeq++)
    {
        // SCP envelopes for this ledger
        // quorum sets missing in this batch of envelopes
        std::set<Hash> missingQSets;

        SCPHistoryEntry hEntryV;
        hEntryV.v(0);
        auto& hEntry = hEntryV.v0();
        auto& lm = hEntry.ledgerMessages;
        lm.ledgerSeq = curLedgerSeq;

        auto& curEnvs = lm.messages;

        // fetch SCP messages from history
        {
            std::string envB64;

            auto timer = db.getSelectTimer("scphistory");

            soci::statement st =
                (sess.prepare << "SELECT envelope FROM scphistory "
                                 "WHERE ledgerseq = :cur ORDER BY nodeid",
                 into(envB64), use(curLedgerSeq));

            st.execute(true);

            while (st.got_data())
            {
                curEnvs.emplace_back();
                auto& env = curEnvs.back();

                std::vector<uint8_t> envBytes;
                bn::decode_b64(envB64, envBytes);

                xdr::xdr_get g1(&envBytes.front(), &envBytes.back() + 1);
                xdr_argpack_archive(g1, env);

                // record new quorum sets encountered
                Hash const& qSetHash =
                    Slot::getCompanionQuorumSetHashFromStatement(env.statement);
                if (qSets.find(qSetHash) == qSets.end())
                {
                    missingQSets.insert(qSetHash);
                }

                n++;

                st.fetch();
            }
        }

        // fetch the quorum sets from the db
        for (auto const& q : missingQSets)
        {
            std::string qset64, qSetHashHex;

            hEntry.quorumSets.emplace_back();
            auto& qset = hEntry.quorumSets.back();

            qSetHashHex = binToHex(q);

            auto timer = db.getSelectTimer("scpquorums");

            soci::statement st = (sess.prepare << "SELECT qset FROM scpquorums "
                                                  "WHERE qsethash = :h",
                                  into(qset64), use(qSetHashHex));

            st.execute(true);

            if (!st.got_data())
            {
                throw std::runtime_error(
                    "corrupt database state: missing quorum set");
            }

            std::vector<uint8_t> qSetBytes;
            bn::decode_b64(qset64, qSetBytes);

            xdr::xdr_get g1(&qSetBytes.front(), &qSetBytes.back() + 1);
            xdr_argpack_archive(g1, qset);
        }

        if (curEnvs.size() != 0)
        {
            scpHistory.writeOne(hEntryV);
        }
    }

    return n;
}

void
Herder::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS scphistory";

    db.getSession() << "DROP TABLE IF EXISTS scpquorums";

    db.getSession() << "CREATE TABLE scphistory ("
                       "nodeid      CHARACTER(56) NOT NULL,"
                       "ledgerseq   INT NOT NULL CHECK (ledgerseq >= 0),"
                       "envelope    TEXT NOT NULL"
                       ")";

    db.getSession() << "CREATE INDEX scpenvsbyseq ON scphistory(ledgerseq)";

    db.getSession() << "CREATE TABLE scpquorums ("
                       "qsethash      CHARACTER(64) NOT NULL,"
                       "lastledgerseq INT NOT NULL CHECK (lastledgerseq >= 0),"
                       "qset          TEXT NOT NULL,"
                       "PRIMARY KEY (qsethash)"
                       ")";
}

void
Herder::deleteOldEntries(Database& db, uint32_t ledgerSeq)
{
    db.getSession() << "DELETE FROM scphistory WHERE ledgerseq <= "
                    << ledgerSeq;
    db.getSession() << "DELETE FROM scpquorums WHERE lastledgerseq <= "
                    << ledgerSeq;
}
}
