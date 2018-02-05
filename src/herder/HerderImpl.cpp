// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "herder/HerderPersistence.h"
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
#include "util/StatusManager.h"
#include "util/Timer.h"
#include "util/make_unique.h"

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/XDRStream.h"
#include "util/basen.h"
#include "xdrpp/marshal.h"

#include <ctime>
#include <lib/util/format.h>

using namespace std;

namespace stellar
{

std::unique_ptr<Herder>
Herder::create(Application& app)
{
    return make_unique<HerderImpl>(app);
}

HerderImpl::SCPMetrics::SCPMetrics(Application& app)
    : mLostSync(app.getMetrics().NewMeter({"scp", "sync", "lost"}, "sync"))
    , mBallotExpire(
          app.getMetrics().NewMeter({"scp", "ballot", "expire"}, "ballot"))
    , mEnvelopeEmit(
          app.getMetrics().NewMeter({"scp", "envelope", "emit"}, "envelope"))
    , mEnvelopeReceive(
          app.getMetrics().NewMeter({"scp", "envelope", "receive"}, "envelope"))

    , mKnownSlotsSize(
          app.getMetrics().NewCounter({"scp", "memory", "known-slots"}))
    , mCumulativeStatements(app.getMetrics().NewCounter(
          {"scp", "memory", "cumulative-statements"}))

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
    : mPendingTransactions(4)
    , mPendingEnvelopes(app, *this)
    , mHerderSCPDriver(app, *this, mUpgrades, mPendingEnvelopes)
    , mLastSlotSaved(0)
    , mTrackingTimer(app)
    , mLastTrigger(app.getClock().now())
    , mTriggerTimer(app)
    , mRebroadcastTimer(app)
    , mApp(app)
    , mLedgerManager(app.getLedgerManager())
    , mSCPMetrics(app)
{
    Hash hash = getSCP().getLocalNode()->getQuorumSetHash();
    mPendingEnvelopes.addSCPQuorumSet(hash,
                                      getSCP().getLocalNode()->getQuorumSet());
}

HerderImpl::~HerderImpl()
{
}

Herder::State
HerderImpl::getState() const
{
    return mHerderSCPDriver.getState();
}

SCP&
HerderImpl::getSCP()
{
    return mHerderSCPDriver.getSCP();
}

void
HerderImpl::syncMetrics()
{
    mHerderSCPDriver.syncMetrics();
}

std::string
HerderImpl::getStateHuman() const
{
    static const char* stateStrings[HERDER_NUM_STATE] = {
        "HERDER_SYNCING_STATE", "HERDER_TRACKING_STATE"};
    return std::string(stateStrings[getState()]);
}

void
HerderImpl::bootstrap()
{
    CLOG(INFO, "Herder") << "Force joining SCP with local state";
    assert(getSCP().isValidator());
    assert(mApp.getConfig().FORCE_SCP);

    mLedgerManager.setState(LedgerManager::LM_SYNCED_STATE);
    mHerderSCPDriver.bootstrap();

    mLastTrigger = mApp.getClock().now() - Herder::EXP_LEDGER_TIMESPAN_SECONDS;
    ledgerClosed();
}

void
HerderImpl::updateSCPCounters()
{
    mSCPMetrics.mKnownSlotsSize.set_count(getSCP().getKnownSlotsCount());
    mSCPMetrics.mCumulativeStatements.set_count(
        getSCP().getCumulativeStatemtCount());
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
HerderImpl::valueExternalized(uint64 slotIndex, StellarValue const& value)
{
    updateSCPCounters();

    // called both here and at the end (this one is in case of an exception)
    trackingHeartBeat();

    if (Logging::logDebug("Herder"))
        CLOG(DEBUG, "Herder") << "HerderSCPDriver::valueExternalized"
                              << " txSet: " << hexAbbrev(value.txSetHash);

    TxSetFramePtr externalizedSet = mPendingEnvelopes.getTxSet(value.txSetHash);

    // trigger will be recreated when the ledger is closed
    // we do not want it to trigger while downloading the current set
    // and there is no point in taking a position after the round is over
    mTriggerTimer.cancel();

    // save the SCP messages in the database
    mApp.getHerderPersistence().saveSCPHistory(
        static_cast<uint32>(slotIndex),
        getSCP().getExternalizingState(slotIndex));

    // reflect upgrades with the ones included in this SCP round
    {
        bool updated;
        auto newUpgrades = mUpgrades.removeUpgrades(
            value.upgrades.begin(), value.upgrades.end(), updated);
        if (updated)
        {
            setUpgrades(newUpgrades);
        }
    }

    // tell the LedgerManager that this value got externalized
    // LedgerManager will perform the proper action based on its internal
    // state: apply, trigger catchup, etc
    LedgerCloseData ledgerData(mHerderSCPDriver.lastConsensusLedgerIndex(),
                               externalizedSet, value);
    mLedgerManager.valueExternalized(ledgerData);

    // perform cleanups
    updatePendingTransactions(externalizedSet->mTransactions);

    // Evict slots that are outside of our ledger validity bracket
    if (slotIndex > MAX_SLOTS_TO_REMEMBER)
    {
        getSCP().purgeSlots(slotIndex - MAX_SLOTS_TO_REMEMBER);
    }

    ledgerClosed();

    // heart beat *after* doing all the work (ensures that we do not include
    // the overhead of externalization in the way we track SCP)
    trackingHeartBeat();
}

void
HerderImpl::rebroadcast()
{
    for (auto const& e :
         getSCP().getLatestMessagesSend(mLedgerManager.getLedgerNum()))
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
        CLOG(DEBUG, "Herder")
            << "emitEnvelope"
            << " s:" << envelope.statement.pledges.type() << " i:" << slotIndex
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

    if (envelope.statement.nodeID == getSCP().getLocalNode()->getNodeID())
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

    if (mHerderSCPDriver.trackingSCP())
    {
        // when tracking, we can filter messages based on the information we got
        // from consensus for the max ledger

        // note that this filtering will cause a node on startup
        // to potentially drop messages outside of the bracket
        // causing it to discard CONSENSUS_STUCK_TIMEOUT_SECONDS worth of
        // ledger closing
        maxLedgerSeq = mHerderSCPDriver.nextConsensusLedgerIndex() +
                       LEDGER_VALIDITY_BRACKET;
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

Herder::EnvelopeStatus
HerderImpl::recvSCPEnvelope(SCPEnvelope const& envelope,
                            const SCPQuorumSet& qset, TxSetFrame txset)
{
    mPendingEnvelopes.addTxSet(txset.getContentsHash(),
                               envelope.statement.slotIndex,
                               std::make_shared<TxSetFrame>(txset));
    mPendingEnvelopes.addSCPQuorumSet(sha256(xdr::xdr_to_opaque(qset)), qset);
    return recvSCPEnvelope(envelope);
}

void
HerderImpl::sendSCPStateToPeer(uint32 ledgerSeq, PeerPtr peer)
{
    if (getSCP().empty())
    {
        return;
    }

    if (getSCP().getLowSlotIndex() > std::numeric_limits<uint32_t>::max() ||
        getSCP().getHighSlotIndex() >= std::numeric_limits<uint32_t>::max())
    {
        return;
    }

    auto minSeq =
        std::max(ledgerSeq, static_cast<uint32_t>(getSCP().getLowSlotIndex()));
    auto maxSeq = static_cast<uint32_t>(getSCP().getHighSlotIndex());

    for (uint32_t seq = minSeq; seq <= maxSeq; seq++)
    {
        auto const& envelopes = getSCP().getCurrentState(seq);

        if (envelopes.size() != 0)
        {
            CLOG(DEBUG, "Herder")
                << "Send state " << envelopes.size() << " for ledger " << seq;

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
    if (mHerderSCPDriver.trackingSCP())
    {
        // drop obsolete slots
        if (mHerderSCPDriver.nextConsensusLedgerIndex() > MAX_SLOTS_TO_REMEMBER)
        {
            mPendingEnvelopes.eraseBelow(
                mHerderSCPDriver.nextConsensusLedgerIndex() -
                MAX_SLOTS_TO_REMEMBER);
        }

        processSCPQueueUpToIndex(mHerderSCPDriver.nextConsensusLedgerIndex());
    }
    else
    {
        // we don't know which ledger we're in
        // try to consume the messages from the queue
        // starting from the smallest slot
        for (auto& slot : mPendingEnvelopes.readySlots())
        {
            processSCPQueueUpToIndex(slot);
            if (mHerderSCPDriver.trackingSCP())
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
            getSCP().receiveEnvelope(env);
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

    mPendingEnvelopes.slotClosed(mHerderSCPDriver.lastConsensusLedgerIndex());

    mApp.getOverlayManager().ledgerClosed(
        mHerderSCPDriver.lastConsensusLedgerIndex());

    uint64_t nextIndex = mHerderSCPDriver.nextConsensusLedgerIndex();

    // process any statements up to this slot (this may trigger externalize)
    processSCPQueueUpToIndex(nextIndex);

    // if externalize got called for a future slot, we don't
    // need to trigger (the now obsolete) next round
    if (nextIndex != mHerderSCPDriver.nextConsensusLedgerIndex())
    {
        return;
    }

    // If we are not a validating node and just watching SCP we don't call
    // triggerNextLedger. Likewise if we are not in synced state.
    if (!getSCP().isValidator())
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
    return mHerderSCPDriver.getQSet(qSetHash);
}

uint32_t
HerderImpl::getCurrentLedgerSeq() const
{
    uint32_t res = mLedgerManager.getLastClosedLedgerNum();

    if (mHerderSCPDriver.trackingSCP() &&
        res < mHerderSCPDriver.trackingSCP()->mConsensusIndex)
    {
        res = static_cast<uint32_t>(
            mHerderSCPDriver.trackingSCP()->mConsensusIndex);
    }
    if (mHerderSCPDriver.lastTrackingSCP() &&
        res < mHerderSCPDriver.lastTrackingSCP()->mConsensusIndex)
    {
        res = static_cast<uint32_t>(
            mHerderSCPDriver.lastTrackingSCP()->mConsensusIndex);
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
    if (!mHerderSCPDriver.trackingSCP() || !mLedgerManager.isSynced())
    {
        CLOG(DEBUG, "Herder") << "triggerNextLedger: skipping (out of sync) : "
                              << mApp.getStateHuman();
        return;
    }
    updateSCPCounters();

    // our first choice for this round's set is all the tx we have collected
    // during last ledger close
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    auto proposedSet = std::make_shared<TxSetFrame>(lcl.hash);

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
    auto upgrades = mUpgrades.createUpgradesFor(lcl.header);
    for (auto const& upgrade : upgrades)
    {
        Value v(xdr::xdr_to_opaque(upgrade));
        if (v.size() >= UpgradeType::max_size())
        {
            CLOG(ERROR, "Herder")
                << "HerderImpl::triggerNextLedger"
                << " exceeded size for upgrade step (got " << v.size()
                << " ) for upgrade type " << std::to_string(upgrade.type());
        }
        else
        {
            newProposedValue.upgrades.emplace_back(v.begin(), v.end());
        }
    }

    mHerderSCPDriver.nominate(slotIndex, newProposedValue, proposedSet,
                              lcl.header.scpValue);
}

void
HerderImpl::setUpgrades(Upgrades::UpgradeParameters const& upgrades)
{
    mUpgrades.setParameters(upgrades, mApp.getConfig());
    persistUpgrades();

    auto desc = mUpgrades.toString();

    if (!desc.empty())
    {
        auto message = fmt::format("Armed with network upgrades: {}", desc);
        auto prev = mApp.getStatusManager().getStatusMessage(
            StatusCategory::REQUIRES_UPGRADES);
        if (prev != message)
        {
            CLOG(INFO, "Herder") << message;
            mApp.getStatusManager().setStatusMessage(
                StatusCategory::REQUIRES_UPGRADES, message);
        }
    }
    else
    {
        CLOG(INFO, "Herder") << "Network upgrades cleared";
        mApp.getStatusManager().removeStatusMessage(
            StatusCategory::REQUIRES_UPGRADES);
    }
}

std::string
HerderImpl::getUpgradesJson()
{
    return mUpgrades.getParameters().toJson();
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
            auto const& envelopes = getSCP().getCurrentState(seq);
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

void
HerderImpl::dumpInfo(Json::Value& ret, size_t limit)
{
    ret["you"] =
        mApp.getConfig().toStrKey(getSCP().getSecretKey().getPublicKey());

    getSCP().dumpInfo(ret, limit);

    mPendingEnvelopes.dumpInfo(ret, limit);
}

void
HerderImpl::dumpQuorumInfo(Json::Value& ret, NodeID const& id, bool summary,
                           uint64 index)
{
    ret["node"] = mApp.getConfig().toStrKey(id);

    getSCP().dumpQuorumInfo(ret["slots"], id, summary, index);
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

    for (auto const& e : getSCP().getLatestMessagesSend(slot))
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
    mHerderSCPDriver.restoreSCPState(lcl.header.ledgerSeq, lcl.header.scpValue);

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
            mPendingEnvelopes.addSCPQuorumSet(hash, qset);
        }
        for (auto const& e : latestEnvs)
        {
            getSCP().setStateFromEnvelope(e.statement.slotIndex, e);
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
HerderImpl::persistUpgrades()
{
    auto s = mUpgrades.getParameters().toJson();
    mApp.getPersistentState().setState(PersistentState::kLedgerUpgrades, s);
}

void
HerderImpl::restoreUpgrades()
{
    std::string s =
        mApp.getPersistentState().getState(PersistentState::kLedgerUpgrades);
    if (!s.empty())
    {
        Upgrades::UpgradeParameters p;
        p.fromJson(s);
        try
        {
            // use common code to set status
            setUpgrades(p);
        }
        catch (std::exception e)
        {
            CLOG(INFO, "Herder") << "Error restoring upgrades '" << e.what()
                                 << "' with upgrades '" << s << "'";
        }
    }
}

void
HerderImpl::restoreState()
{
    restoreSCPState();
    restoreUpgrades();
}

void
HerderImpl::trackingHeartBeat()
{
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return;
    }

    assert(mHerderSCPDriver.trackingSCP());
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
    mHerderSCPDriver.lostSync();

    processSCPQueue();
}
}
