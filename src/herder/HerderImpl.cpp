// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "crypto/Base58.h"
#include "herder/HerderImpl.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "lib/json/json.h"
#include "scp/Node.h"

#include "medida/meter.h"
#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "xdrpp/marshal.h"

#include <ctime>

#define MAX_SLOTS_TO_REMEMBER 4

namespace stellar
{

std::unique_ptr<Herder>
Herder::create(Application& app)
{
    return make_unique<HerderImpl>(app);
}

// Static helper for HerderImpl's SCP constructor
static SCPQuorumSet
quorumSetFromApp(Application& app)
{
    SCPQuorumSet qSet;
    qSet.threshold = app.getConfig().QUORUM_THRESHOLD;
    for (auto q : app.getConfig().QUORUM_SET)
    {
        qSet.validators.push_back(q);
    }
    return qSet;
}

HerderImpl::HerderImpl(Application& app)
    : SCP(app.getConfig().VALIDATION_KEY, quorumSetFromApp(app))
    , mReceivedTransactions(4)
#ifdef _MSC_VER
    // This form of initializer causes a warning due to brace-elision on
    // clang.
    , mTxSetFetcher({TxSetFetcher(app), TxSetFetcher(app)})
#else
    // This form of initializer is "not implemented" in MSVC yet.
    , mTxSetFetcher
{
    {
        {
            TxSetFetcher(app)
        }
        ,
        {
            TxSetFetcher(app)
        }
    }
}
#endif
    , mCurrentTxSetFetcher(0)
    , mSCPQSetFetcher(app)
    , mLastTrigger(app.getClock().now())
    , mTriggerTimer(app)
    , mBumpTimer(app)
    , mRebroadcastTimer(app)
    , mApp(app)

    , mValueValid(app.getMetrics().NewMeter({"scp", "value", "valid"}, "value"))
    , mValueInvalid(
          app.getMetrics().NewMeter({"scp", "value", "invalid"}, "value"))
    , mValuePrepare(
          app.getMetrics().NewMeter({"scp", "value", "prepare"}, "value"))
    , mValueExternalize(
          app.getMetrics().NewMeter({"scp", "value", "externalize"}, "value"))

    , mBallotValid(
          app.getMetrics().NewMeter({"scp", "ballot", "valid"}, "ballot"))
    , mBallotInvalid(
          app.getMetrics().NewMeter({"scp", "ballot", "invalid"}, "ballot"))
    , mBallotPrepare(
          app.getMetrics().NewMeter({"scp", "ballot", "prepare"}, "ballot"))
    , mBallotPrepared(
          app.getMetrics().NewMeter({"scp", "ballot", "prepared"}, "ballot"))
    , mBallotCommit(
          app.getMetrics().NewMeter({"scp", "ballot", "commit"}, "ballot"))
    , mBallotCommitted(
          app.getMetrics().NewMeter({"scp", "ballot", "committed"}, "ballot"))
    , mBallotSign(
          app.getMetrics().NewMeter({"scp", "ballot", "sign"}, "ballot"))
    , mBallotValidSig(
          app.getMetrics().NewMeter({"scp", "ballot", "validsig"}, "ballot"))
    , mBallotInvalidSig(
          app.getMetrics().NewMeter({"scp", "ballot", "invalidsig"}, "ballot"))
    , mBallotExpire(
          app.getMetrics().NewMeter({"scp", "ballot", "expire"}, "ballot"))

    , mQuorumHeard(
          app.getMetrics().NewMeter({"scp", "quorum", "heard"}, "quorum"))
    , mQsetRetrieve(
          app.getMetrics().NewMeter({"scp", "qset", "retrieve"}, "qset"))

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

    , mNodeLastAccessSize(
          app.getMetrics().NewCounter({"scp", "memory", "node-last-access"}))
    , mSCPQSetFetchesSize(
          app.getMetrics().NewCounter({"scp", "memory", "qset-fetches"}))
    , mFutureEnvelopesSize(
          app.getMetrics().NewCounter({"scp", "memory", "future-envelopes"}))
    , mBallotValidationTimersSize(app.getMetrics().NewCounter(
          {"scp", "memory", "ballot-validation-timers"}))

    , mKnownNodesSize(
          app.getMetrics().NewCounter({"scp", "memory", "known-nodes"}))
    , mKnownSlotsSize(
          app.getMetrics().NewCounter({"scp", "memory", "known-slots"}))
    , mCumulativeStatements(app.getMetrics().NewCounter(
          {"scp", "memory", "cumulative-statements"}))
    , mCumulativeCachedQuorumSets(app.getMetrics().NewCounter(
          {"scp", "memory", "cumulative-cached-quorum-sets"}))

{
    // Inject our local qSet in the SCPQSetFetcher.
    SCPQuorumSetPtr qSet =
        std::make_shared<SCPQuorumSet>(std::move(quorumSetFromApp(mApp)));
    recvSCPQuorumSet(qSet);
}

HerderImpl::~HerderImpl()
{
}

void
HerderImpl::bootstrap()
{
    assert(!getSecretKey().isZero());
    assert(mApp.getConfig().FORCE_SCP);

    mApp.setState(Application::SYNCED_STATE);
    mLastClosedLedger = mApp.getLedgerManager().getLastClosedLedgerHeader();
    triggerNextLedger();
}

void
HerderImpl::validateValue(uint64 const& slotIndex, uint256 const& nodeID,
                          Value const& value,
                          std::function<void(bool)> const& cb)
{
    StellarBallot b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        mValueInvalid.Mark();
        return cb(false);
    }

    if (mApp.getState() != Application::SYNCED_STATE)
    { // if we aren't synced to the network we can't validate
        // but we still need to fetch the tx set
        fetchTxSet(b.value.txSetHash, true);
        return cb(true);
    }

    // First of all let's verify the internal Stellar Ballot signature is
    // correct.
    if (!verifyStellarBallot(b))
    {
        mValueInvalid.Mark();
        return cb(false);
    }

    // All tests that are relative to mLastClosedLedger are executed only once
    // we are fully synced up
    // Check slotIndex.
    if (mLastClosedLedger.header.ledgerSeq + 1 != slotIndex)
    {
        mValueInvalid.Mark();
        return cb(false);
    }

    // Check closeTime (not too old)
    if (b.value.closeTime <= mLastClosedLedger.header.closeTime)
    {
        mValueInvalid.Mark();
        return cb(false);
    }

    // Check closeTime (not too far in future)
    uint64_t timeNow = mApp.timeNow();
    if (b.value.closeTime > timeNow + MAX_TIME_SLIP_SECONDS)
    {
        mValueInvalid.Mark();
        return cb(false);
    }

    // make sure all the tx we have in the old set are included
    auto validate = [cb, b, slotIndex, nodeID, this](TxSetFramePtr txSet)
    {
        // Check txSet (only if we're fully synced)
        if (!txSet->checkValid(mApp))
        {
            CLOG(DEBUG, "Herder")
                << "HerderImpl::validateValue"
                << "@" << hexAbbrev(getLocalNodeID()) << " i: " << slotIndex
                << " n: " << hexAbbrev(nodeID) << " Invalid txSet:"
                << " " << hexAbbrev(txSet->getContentsHash());
            this->mValueInvalid.Mark();
            return cb(false);
        }

        CLOG(DEBUG, "Herder")
            << "HerderImpl::validateValue"
            << "@" << hexAbbrev(getLocalNodeID()) << " i: " << slotIndex
            << " n: " << hexAbbrev(nodeID) << " txSet:"
            << " " << hexAbbrev(txSet->getContentsHash()) << " OK";
        this->mValueValid.Mark();
        return cb(true);
    };

    TxSetFramePtr txSet = fetchTxSet(b.value.txSetHash, true);
    if (!txSet)
    {
        mTxSetFetches[b.value.txSetHash].push_back(validate);
    }
    else
    {
        validate(txSet);
    }
}

int
HerderImpl::compareValues(uint64 const& slotIndex, uint32 const& ballotCounter,
                          Value const& v1, Value const& v2)
{
    using xdr::operator<;

    if (!v1.size())
    {
        if (!v2.size())
            return 0;
        return -1;
    }
    else if (!v2.size())
        return 1;

    StellarBallot b1;
    StellarBallot b2;
    try
    {
        xdr::xdr_from_opaque(v1, b1);
        xdr::xdr_from_opaque(v2, b2);
    }
    catch (...)
    {
        // This should not be possible. Values are validated before they
        // are compared.
        CLOG(ERROR, "Herder")
            << "HerderImpl::compareValues"
            << "@" << hexAbbrev(getLocalNodeID())
            << " Unexpected invalid value format. v1:" << binToHex(v1)
            << " v2:" << binToHex(v2);

        assert(false);
        return 0;
    }

    // Unverified StellarBallot shouldn't be possible either for the precise
    // same reasons.
    assert(verifyStellarBallot(b1));
    assert(verifyStellarBallot(b2));

    // Ordering is based on H(slotIndex, ballotCounter, nodeID). Such that the
    // round king value gets privileged over other values. Given the hash
    // function used, a new monarch is coronated for each round of SCP (ballot
    // counter) and each slotIndex.

    auto s1 = SHA256::create();
    s1->add(xdr::xdr_to_opaque(slotIndex));
    s1->add(xdr::xdr_to_opaque(ballotCounter));
    s1->add(xdr::xdr_to_opaque(b1.nodeID));
    auto h1 = s1->finish();

    auto s2 = SHA256::create();
    s2->add(xdr::xdr_to_opaque(slotIndex));
    s2->add(xdr::xdr_to_opaque(ballotCounter));
    s2->add(xdr::xdr_to_opaque(b2.nodeID));
    auto h2 = s2->finish();

    if (h1 < h2)
        return -1;
    if (h2 < h1)
        return 1;

    if (b1.value < b2.value)
        return -1;
    if (b2.value < b1.value)
        return 1;

    return 0;
}

void
HerderImpl::validateBallot(uint64 const& slotIndex, uint256 const& nodeID,
                           SCPBallot const& ballot,
                           std::function<void(bool)> const& cb)
{
    StellarBallot b;
    try
    {
        xdr::xdr_from_opaque(ballot.value, b);
    }
    catch (...)
    {
        mBallotInvalid.Mark();
        return cb(false);
    }

    // Check closeTime (not too far in the future)
    uint64_t timeNow = mApp.timeNow();
    if (b.value.closeTime > timeNow + MAX_TIME_SLIP_SECONDS)
    {
        mBallotInvalid.Mark();
        return cb(false);
    }

    // Check the ballot counter is not growing too rapidly. We ignore ballots
    // that were triggered before the expected series of timeouts (accepting
    // MAX_TIME_SLIP_SECONDS as error). This prevents ballot counter
    // exhaustion attacks.
    uint64_t lastTrigger = VirtualClock::to_time_t(mLastTrigger);
    uint64_t sumTimeouts = 0;
    // The second condition is to prevent attackers from emitting ballots whose
    // verification would busy lock us.
    for (int unsigned i = 0;
         i < ballot.counter &&
         (timeNow + MAX_TIME_SLIP_SECONDS) >= (lastTrigger + sumTimeouts);
         i++)
    {
        sumTimeouts += std::min(MAX_SCP_TIMEOUT_SECONDS, (int)pow(2.0, i));
    }
    // This inequality is effectively a limitation on `ballot.counter`
    if ((timeNow + MAX_TIME_SLIP_SECONDS) < (lastTrigger + sumTimeouts))
    {
        mBallotInvalid.Mark();
        return cb(false);
    }

    // Check baseFee (within range of desired fee).
    if (b.value.baseFee < mApp.getConfig().DESIRED_BASE_FEE * .5)
    {
        mBallotInvalid.Mark();
        return cb(false);
    }
    if (b.value.baseFee > mApp.getConfig().DESIRED_BASE_FEE * 2)
    {
        mBallotInvalid.Mark();
        return cb(false);
    }

    // Ignore ourselves if we're just watching SCP.
    if (getSecretKey().isZero() && nodeID == getLocalNodeID())
    {
        mBallotInvalid.Mark();
        return cb(false);
    }

    // No need to check if all the txs are in the txSet as this is decided by
    // the king of that round. Just check that we believe that this ballot is
    // actually from the king itself.
    bool isKing = true;
    bool isTrusted = false;
    for (auto vID : getLocalQuorumSet().validators)
    {
        // A ballot is trusted if its value is generated or prepared by a node
        // in our qSet.
        if (b.nodeID == vID || b.nodeID == getLocalNodeID())
        {
            isTrusted = true;
        }

        auto sProposed = SHA256::create();
        sProposed->add(xdr::xdr_to_opaque(slotIndex));
        sProposed->add(xdr::xdr_to_opaque(ballot.counter));
        sProposed->add(xdr::xdr_to_opaque(b.nodeID));
        auto hProposed = sProposed->finish();

        auto sContender = SHA256::create();
        sContender->add(xdr::xdr_to_opaque(slotIndex));
        sContender->add(xdr::xdr_to_opaque(ballot.counter));
        sContender->add(xdr::xdr_to_opaque(vID));
        auto hContender = sContender->finish();

        // A ballot is king (locally) only if it is higher than any potential
        // ballots from nodes in our qSet.
        if (hProposed < hContender)
        {
            isKing = false;
        }
    }

    uint256 valueHash = sha256(xdr::xdr_to_opaque(ballot.value));

    CLOG(DEBUG, "Herder") << "HerderImpl::validateBallot"
                          << "@" << hexAbbrev(getLocalNodeID())
                          << " i: " << slotIndex << " v: " << hexAbbrev(nodeID)
                          << " o: " << hexAbbrev(b.nodeID) << " b: ("
                          << ballot.counter << "," << hexAbbrev(valueHash)
                          << ")"
                          << " isTrusted: " << isTrusted
                          << " isKing: " << isKing
                          << " timeout: " << pow(2.0, ballot.counter) / 2;

    if (isKing && isTrusted)
    {
        mBallotValid.Mark();
        return cb(true);
    }
    else
    {
        CLOG(DEBUG, "Herder")
            << "start timer"
            << "@" << hexAbbrev(getLocalNodeID()) << " i: " << slotIndex
            << " v: " << hexAbbrev(nodeID) << " o: " << hexAbbrev(b.nodeID)
            << " b: (" << ballot.counter << "," << hexAbbrev(valueHash) << ")"
            << " isTrusted: " << isTrusted << " isKing: " << isKing
            << " timeout: " << pow(2.0, ballot.counter) / 2;
        // Create a timer to wait for current SCP timeout / 2 before accepting
        // that ballot.
        std::shared_ptr<VirtualTimer> ballotTimer =
            std::make_shared<VirtualTimer>(mApp);
        ballotTimer->expires_from_now(std::chrono::milliseconds(
            (int)(1000 * pow(2.0, ballot.counter) / 2)));
        ballotTimer->async_wait(
            [cb, this]()
            {
                this->mBallotValid.Mark();
                return cb(true);
            },
            VirtualTimer::onFailureNoop);
        mBallotValidationTimers[ballot][nodeID].push_back(ballotTimer);

        // Check if the nodes that have requested validation for this ballot
        // is a v-blocking. If so, rush validation by canceling all timers.
        std::vector<uint256> nodes;
        for (auto it : mBallotValidationTimers[ballot])
        {
            nodes.push_back(it.first);
        }
        if (isVBlocking(nodes))
        {
            // This will cancel all timers.
            mBallotValidationTimers.erase(ballot);
        }
        mBallotValidationTimersSize.set_count(mBallotValidationTimers.size());
    }
}

void
HerderImpl::ballotDidHearFromQuorum(uint64 const& slotIndex,
                                    SCPBallot const& ballot)
{
    mQuorumHeard.Mark();

    // Don't have a bump timer if we aren't in sync
    if(slotIndex == mLastClosedLedger.header.ledgerSeq + 1)
    {
        int seconds = (int)pow(2.0, ballot.counter + 1);
        CLOG(DEBUG, "Herder")
            << "start bumptimer"
            << " i: " << slotIndex
            << " ballot counter: " << ballot.counter << ","
            << " timeout: " << seconds;

        // Once we hear from a transitive quorum, we start a timer in case SCP
        // timeouts.
        mBumpTimer.expires_from_now(std::chrono::seconds(seconds));

        mBumpTimer.async_wait([&]() { expireBallot(slotIndex, ballot); },
            &VirtualTimer::onFailureNoop);
    }
}

void
HerderImpl::updateSCPCounters()
{
    mKnownNodesSize.set_count(getKnownNodesCount());
    mKnownSlotsSize.set_count(getKnownSlotsCount());
    mCumulativeStatements.set_count(getCumulativeStatemtCount());
    mCumulativeCachedQuorumSets.set_count(getCumulativeCachedQuorumSetCount());
}

void
HerderImpl::valueExternalized(uint64 const& slotIndex, Value const& value)
{
    updateSCPCounters();
    mValueExternalize.Mark();
    mBumpTimer.cancel();
    StellarBallot b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        // This may not be possible as all messages are validated and should
        // therefore contain a valid StellarBallot.
        CLOG(ERROR, "Herder") << "HerderImpl::valueExternalized"
                              << "@" << hexAbbrev(getLocalNodeID())
                              << " Externalized StellarBallot malformed";
    }

    TxSetFramePtr externalizedSet = fetchTxSet(b.value.txSetHash, true);
    if (externalizedSet)
    {

        CLOG(DEBUG, "Herder") << "HerderImpl::valueExternalized"
                              << "@" << hexAbbrev(getLocalNodeID())
                              << " txSet: " << hexAbbrev(b.value.txSetHash);

        // we don't need to keep fetching any of the old TX sets
        mTxSetFetcher[mCurrentTxSetFetcher].stopFetchingAll();

        mRebroadcastTimer.cancel();

        mCurrentTxSetFetcher = mCurrentTxSetFetcher ? 0 : 1;
        mTxSetFetcher[mCurrentTxSetFetcher].clear();

        // We trigger next ledger EXP_LEDGER_TIMESPAN_SECONDS after our last
        // trigger.
        mTriggerTimer.cancel();

        // Triggers sync if not already syncing.
        LedgerCloseData ledgerData(static_cast<uint32_t>(slotIndex),
                                   externalizedSet, b.value.closeTime,
                                   b.value.baseFee);
        mApp.getLedgerManager().externalizeValue(ledgerData);

        // remove all these tx from mReceivedTransactions
        for (auto tx : externalizedSet->mTransactions)
        {
            removeReceivedTx(tx);
        }
        // rebroadcast those left in set 1
        assert(mReceivedTransactions.size() >= 2);
        for (auto& tx : mReceivedTransactions[1])
        {
            auto msg = tx->toStellarMessage();
            mApp.getOverlayManager().broadcastMessage(msg);
        }

        // Evict nodes that weren't touched for more than
        auto now = mApp.getClock().now();
        for (auto it : mNodeLastAccess)
        {
            if ((now - it.second) >
                std::chrono::seconds(NODE_EXPIRATION_SECONDS))
            {
                purgeNode(it.first);
            }
        }

        // Evict slots that are outside of our ledger validity bracket
        if (slotIndex > MAX_SLOTS_TO_REMEMBER)
        {
            purgeSlots(slotIndex - MAX_SLOTS_TO_REMEMBER);
        }

        // Move all the remaining to the next highest level don't move the
        // largest array.
        for (size_t n = mReceivedTransactions.size() - 1; n > 0; n--)
        {
            for (auto& tx : mReceivedTransactions[n - 1])
            {
                mReceivedTransactions[n].push_back(tx);
            }
            mReceivedTransactions[n - 1].clear();
        }
    }
    else
    {
        auto cb = [slotIndex, value, this](TxSetFramePtr txSet)
        {
            this->valueExternalized(slotIndex, value);
        };

        mTxSetFetches[b.value.txSetHash].push_back(cb);


        // This may not be possible as all messages are validated and should
        // therefore fetch the txSet before being considered by SCP.
        CLOG(ERROR, "Herder") << "HerderImpl::valueExternalized"
                              << "@" << hexAbbrev(getLocalNodeID())
                              << " Externalized txSet not found: "
                              << hexAbbrev(b.value.txSetHash);
    }
}

void
HerderImpl::nodeTouched(uint256 const& nodeID)
{
    // We simply store the time of last access each time a node is touched by
    // SCP. That way we can evict old irrelevant nodes at each round.
    mNodeLastAccess[nodeID] = mApp.getClock().now();
    mNodeLastAccessSize.set_count(mNodeLastAccess.size());
}

void
HerderImpl::retrieveQuorumSet(
    uint256 const& nodeID, Hash const& qSetHash,
    std::function<void(SCPQuorumSet const&)> const& cb)
{
    mQsetRetrieve.Mark();
    CLOG(DEBUG, "Herder") << "HerderImpl::retrieveQuorumSet"
                          << "@" << hexAbbrev(getLocalNodeID())
                          << " qSet: " << hexAbbrev(qSetHash);
    auto retrieve = [cb, this](SCPQuorumSetPtr qSet)
    {
        return cb(*qSet);
    };

    // Peer Overlays and nodeIDs have no relationship for now. Sow we just
    // retrieve qSetHash by asking the whole overlay.
    SCPQuorumSetPtr qSet = fetchSCPQuorumSet(qSetHash, true);
    if (!qSet)
    {
        mSCPQSetFetches[qSetHash].push_back(retrieve);
        mSCPQSetFetchesSize.set_count(mSCPQSetFetches.size());
    }
    else
    {
        retrieve(qSet);
    }
}

void
HerderImpl::rebroadcast()
{
    if(mLastSentMessage.type()== SCP_MESSAGE)
    {
        CLOG(DEBUG, "Herder") << "rebroadcast "
            << " s:" << mLastSentMessage.envelope().statement.pledges.type()
            << " i:" << mLastSentMessage.envelope().statement.slotIndex
            << " b:" << mLastSentMessage.envelope().statement.ballot.counter
            << " v:" << binToHex(ByteSlice(&mLastSentMessage.envelope().statement.ballot.value[96], 3));

        mEnvelopeEmit.Mark();
        mApp.getOverlayManager().broadcastMessage(mLastSentMessage, true);
        startRebroadcastTimer();
    }
}

void
HerderImpl::startRebroadcastTimer()
{
    if (!mApp.getConfig().MANUAL_CLOSE && mLastSentMessage.type()==SCP_MESSAGE)
    {
        mRebroadcastTimer.expires_from_now(std::chrono::seconds(2));

        mRebroadcastTimer.async_wait(std::bind(&HerderImpl::rebroadcast, this),
                                     &VirtualTimer::onFailureNoop);
    }
}

void
HerderImpl::emitEnvelope(SCPEnvelope const& envelope)
{
    // We don't emit any envelope as long as we're not fully synced
    if (mApp.getState() != Application::SYNCED_STATE ||
        mCurrentValue.empty())
    {
        return;
    }

    mLastSentMessage.type(SCP_MESSAGE);
    mLastSentMessage.envelope() = envelope;

    rebroadcast();
}

TxSetFramePtr
HerderImpl::fetchTxSet(uint256 const& txSetHash, bool askNetwork)
{
    // set false the first time to make sure we only ask network at most once
    TxSetFramePtr ret = mTxSetFetcher[0].fetchItem(txSetHash, false);
    if (!ret)
        ret = mTxSetFetcher[1].fetchItem(txSetHash, askNetwork);
    return ret;
}

void
HerderImpl::recvTxSet(TxSetFramePtr txSet)
{
    // add all txs to next set in case they don't get in this ledger
    for(auto tx : txSet->sortForApply())
    {
        recvTransaction(tx);
    }

    mTxSetFetcher[mCurrentTxSetFetcher].recvItem(txSet);

    // Runs any pending validation on this txSet.
    auto it = mTxSetFetches.find(txSet->getContentsHash());
    if(it != mTxSetFetches.end())
    {
        for(auto validate : it->second)
        {
            validate(txSet);
        }
        mTxSetFetches.erase(it);
    }
}

void
HerderImpl::doesntHaveTxSet(uint256 const& txSetHash, PeerPtr peer)
{
    mTxSetFetcher[mCurrentTxSetFetcher].doesntHave(txSetHash, peer);
}

SCPQuorumSetPtr
HerderImpl::fetchSCPQuorumSet(uint256 const& qSetHash, bool askNetwork)
{
    return mSCPQSetFetcher.fetchItem(qSetHash, askNetwork);
}

void
HerderImpl::recvSCPQuorumSet(SCPQuorumSetPtr qSet)
{
    CLOG(TRACE, "Herder") << "HerderImpl::recvSCPQuorumSet"
                          << "@" << hexAbbrev(getLocalNodeID()) << " qSet: "
                          << hexAbbrev(sha256(xdr::xdr_to_opaque(*qSet)));

    if (mSCPQSetFetcher.recvItem(qSet))
    {
        // someone cares about this set
        uint256 qSetHash = sha256(xdr::xdr_to_opaque(*qSet));

        // Runs any pending retrievals on this qSet
        auto it = mSCPQSetFetches.find(qSetHash);
        if (it != mSCPQSetFetches.end())
        {
            for (auto retrieve : it->second)
            {
                retrieve(qSet);
            }
            mSCPQSetFetches.erase(it);
            mSCPQSetFetchesSize.set_count(mSCPQSetFetches.size());
        }
    }
}

void
HerderImpl::doesntHaveSCPQuorumSet(uint256 const& qSetHash, PeerPtr peer)
{
    mSCPQSetFetcher.doesntHave(qSetHash, peer);
}

bool
HerderImpl::recvTransaction(TransactionFramePtr tx)
{
    Hash const& txID = tx->getFullHash();

    // determine if we have seen this tx before and if not if it has the right
    // seq num
    int64_t totFee = tx->getFee(mApp);
    SequenceNumber highSeq = 0;

    for (auto& list : mReceivedTransactions)
    {
        for (auto oldTX : list)
        {
            if (txID == oldTX->getFullHash())
            {
                tx->getResult().result.code(txDUPLICATE);
                return false;
            }
            if (oldTX->getSourceID() == tx->getSourceID())
            {
                totFee += oldTX->getFee(mApp);
                if (oldTX->getSeqNum() > highSeq)
                {
                    highSeq = oldTX->getSeqNum();
                }
            }
        }
    }

    if (!tx->checkValid(mApp, highSeq))
    {
        return false;
    }

    if (tx->getSourceAccount().getBalanceAboveReserve(mApp.getLedgerManager()) <
        totFee)
    {
        tx->getResult().result.code(txINSUFFICIENT_BALANCE);
        return false;
    }

    mReceivedTransactions[0].push_back(tx);

    return true;
}

void
HerderImpl::recvSCPEnvelope(SCPEnvelope envelope,
                            std::function<void(EnvelopeState)> const& cb)
{
    CLOG(DEBUG, "Herder") << "recvSCPEnvelope"
                          << " from: " << hexAbbrev(envelope.nodeID) 
                          << " s:" << envelope.statement.pledges.type() 
                          << " i:" << envelope.statement.slotIndex
                          << " b:" << envelope.statement.ballot.counter
                          << " v:" << binToHex(ByteSlice(&envelope.statement.ballot.value[96],3))
                          << " a:" << mApp.getState();

    if (mApp.getState() == Application::SYNCED_STATE)
    {
        uint32_t minLedgerSeq = mLastClosedLedger.header.ledgerSeq;
        uint32_t maxLedgerSeq =
            mLastClosedLedger.header.ledgerSeq + LEDGER_VALIDITY_BRACKET;

        // If we are fully synced and the envelopes are out of our validity
        // brackets, we just ignore them.
        if (envelope.statement.slotIndex > maxLedgerSeq ||
            envelope.statement.slotIndex <= minLedgerSeq)
        {
            CLOG(DEBUG, "Herder") << "Ignoring SCPEnvelope outside of range: "
                << envelope.statement.slotIndex << "( " << minLedgerSeq << ","
                << maxLedgerSeq << ")";
            return;
        }

        // If we are synced, and we see envelopes that are for future ledgers
        // (or the current ledger, if a validator and curr is empty) then we
        // should store the envelopes for later replay.
        uint32_t currLedger = mLastClosedLedger.header.ledgerSeq + 1;
        if (!getSecretKey().isZero() &&
            envelope.statement.slotIndex == currLedger &&
            mCurrentValue.empty())
        {
            mFutureEnvelopes[envelope.statement.slotIndex].push_back(
                std::make_pair(envelope, cb));
            mFutureEnvelopesSize.set_count(mFutureEnvelopes.size());
            CLOG(DEBUG, "Herder") 
                << "Adding envelope to future-envelopes queue while waiting for trigger";
            return;
        }
        else if (envelope.statement.slotIndex > currLedger)
        {
            CLOG(DEBUG, "Herder") << "Adding envelope to future-envelopes queue";

            mFutureEnvelopes[envelope.statement.slotIndex].push_back(
                std::make_pair(envelope, cb));
            mFutureEnvelopesSize.set_count(mFutureEnvelopes.size());

            if (checkFutureCommitted(envelope))
            {
                // a quorum slice has left us behind
                CLOG(INFO, "Herder")
                    << "Left behind. Catching up to our slice at "
                    << envelope.statement.slotIndex;
                valueExternalized(envelope.statement.slotIndex,
                                  envelope.statement.ballot.value);
            }

            return;
        }

    }
    startRebroadcastTimer();

    mEnvelopeReceive.Mark();
    return receiveEnvelope(envelope, cb);
}

// returns true if we have been left behind :(
// see: walter the lazy mouse
bool
HerderImpl::checkFutureCommitted(SCPEnvelope& envelope)
{
    if(envelope.statement.pledges.type() == COMMITTED)
    { // is this a committed statement
        const SCPQuorumSet& qset=getLocalQuorumSet();

        if(find(qset.validators.begin(), qset.validators.end(),
            envelope.nodeID) != qset.validators.end())
        {// is it from someone we care about?
            auto& list = mQuorumAheadOfUs[envelope.statement.slotIndex];
            if(find(list.begin(), list.end(), envelope) == list.end())
            {// is it a new one for the list?
                // TODO: we probably want to fetch the txset here to save time
                list.push_back(envelope);
                unsigned int count = 0;
                for(auto& env : list)
                {
                    if(env.statement.ballot.value ==
                        envelope.statement.ballot.value) count++;

                    if(count>=qset.threshold)
                    {// do we have enough of these for the same ballot?
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void
HerderImpl::ledgerClosed(LedgerHeaderHistoryEntry const& ledger)
{
    mTriggerTimer.cancel();

    updateSCPCounters();
    CLOG(TRACE, "Herder") << "HerderImpl::ledgerClosed@"
                          << "@" << hexAbbrev(getLocalNodeID())
                          << " ledger: " << hexAbbrev(ledger.hash);

    // we're not running SCP anymore
    mCurrentValue.clear();
    mQuorumAheadOfUs.erase(ledger.header.ledgerSeq-1);

    mLastClosedLedger = ledger;

    // As the current slotIndex changes we cancel all pending validation
    // timers. Since the value externalized, the messages that this generates
    // wont' have any impact.
    mBallotValidationTimers.clear();
    mBallotValidationTimersSize.set_count(mBallotValidationTimers.size());

    // If we are not a validating node and just watching SCP we don't call
    // triggerNextLedger. Likewise if we are not in synced state.
    if (getSecretKey().isZero())
    {
        CLOG(DEBUG, "Herder")
            << "Non-validating node, not triggering ledger-close.";
        return;
    }

    if (mApp.getState() != Application::SYNCED_STATE)
    {
        CLOG(DEBUG, "Herder")
            << "Not presently synced, not triggering ledger-close.";
        return;
    }

    size_t seconds = EXP_LEDGER_TIMESPAN_SECONDS;
    if (mApp.getConfig().ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
    {
        seconds = 1;
    }

    auto now = mApp.getClock().now();
    if ((now - mLastTrigger) < std::chrono::seconds(seconds))
    {
        auto timeout = std::chrono::seconds(seconds) - (now - mLastTrigger);
        mTriggerTimer.expires_from_now(timeout);
    }
    else
    {
        mTriggerTimer.expires_from_now(std::chrono::nanoseconds(0));
    }

    if (!mApp.getConfig().MANUAL_CLOSE)
        mTriggerTimer.async_wait(
            std::bind(&HerderImpl::triggerNextLedger, this),
            &VirtualTimer::onFailureNoop);
}

void
HerderImpl::removeReceivedTx(TransactionFramePtr dropTx)
{
    for (auto& list : mReceivedTransactions)
    {
        for (auto iter = list.begin(); iter != list.end();)
        {
            if ((iter.operator->())->get()->getFullHash() ==
                dropTx->getFullHash())
            {
                list.erase(iter);
                return;
            }
            else
            {
                iter++;
            }
        }
    }
}

// called to start the next round of SCP
void
HerderImpl::triggerNextLedger()
{
    updateSCPCounters();
    // We store at which time we triggered consensus
    mLastTrigger = mApp.getClock().now();

    // our first choice for this round's set is all the tx we have collected
    // during last ledger close
    TxSetFramePtr proposedSet =
        std::make_shared<TxSetFrame>(mLastClosedLedger.hash);
    for (auto& list : mReceivedTransactions)
    {
        for (auto& tx : list)
        {
            proposedSet->add(tx);
        }
    }

    std::vector<TransactionFramePtr> removed;
    proposedSet->trimInvalid(mApp, removed);
    for (auto& tx : removed)
    {
        removeReceivedTx(tx);
    }

    recvTxSet(proposedSet);

    uint64_t slotIndex = mLastClosedLedger.header.ledgerSeq + 1;

    // We pick as next close time the current time unless it's before the last
    // close time. We don't know how much time it will take to reach consensus
    // so this is the most appropriate value to use as closeTime.
    uint64_t nextCloseTime = VirtualClock::to_time_t(mLastTrigger);
    if (nextCloseTime <= mLastClosedLedger.header.closeTime)
    {
        nextCloseTime = mLastClosedLedger.header.closeTime + 1;
    }

    StellarBallot b;
    b.value.txSetHash = proposedSet->getContentsHash();
    b.value.closeTime = nextCloseTime;
    b.value.baseFee = mApp.getConfig().DESIRED_BASE_FEE;
    signStellarBallot(b);

    mCurrentValue = xdr::xdr_to_opaque(b);

    uint256 valueHash = sha256(xdr::xdr_to_opaque(mCurrentValue));
    CLOG(DEBUG, "Herder") << "HerderImpl::triggerNextLedger"
                          << "@" << hexAbbrev(getLocalNodeID())
                          << " txSet.size: "
                          << proposedSet->mTransactions.size()
                          << " previousLedgerHash: "
                          << hexAbbrev(proposedSet->previousLedgerHash())
                          << " value: " << hexAbbrev(valueHash)
                          << " slot: " << slotIndex;

    // We prepare that value. If we're monarch, the ballot will be validated,
    // and if we're not it'll just get ignored.
    mValuePrepare.Mark();
    prepareValue(slotIndex, mCurrentValue);

    // process any statements that we got before this ledger closed
    for (auto& p : mFutureEnvelopes[slotIndex])
    {
        recvSCPEnvelope(p.first, p.second);
    }
    mFutureEnvelopes.erase(slotIndex);
    mFutureEnvelopesSize.set_count(mFutureEnvelopes.size());
}

void
HerderImpl::expireBallot(uint64 const& slotIndex, SCPBallot const& ballot)

{
    mBallotExpire.Mark();
    assert(slotIndex == mLastClosedLedger.header.ledgerSeq + 1);

    // We prepare the value while bumping the ballot counter. If we're monarch,
    // this prepare will go through. If not, we will have bumped our ballot.
    mValuePrepare.Mark();
    prepareValue(slotIndex, mCurrentValue, true);
}

void
HerderImpl::signStellarBallot(StellarBallot& b)
{
    mBallotSign.Mark();
    b.nodeID = getSecretKey().getPublicKey();
    b.signature = getSecretKey().sign(xdr::xdr_to_opaque(b.value));
}

bool
HerderImpl::verifyStellarBallot(StellarBallot const& b)
{
    auto v = PublicKey::verifySig(b.nodeID, b.signature,
                                  xdr::xdr_to_opaque(b.value));
    if (v)
    {
        mBallotValidSig.Mark();
    }
    else
    {
        mBallotInvalidSig.Mark();
    }
    return v;
}

// Extra SCP methods overridden solely to increment metrics.
void
HerderImpl::ballotDidPrepare(uint64 const& slotIndex, SCPBallot const& ballot)
{
    mBallotPrepare.Mark();
}

void
HerderImpl::ballotDidPrepared(uint64 const& slotIndex, SCPBallot const& ballot)
{
    mBallotPrepared.Mark();
}

void
HerderImpl::ballotDidCommit(uint64 const& slotIndex, SCPBallot const& ballot)
{
    mBallotCommit.Mark();
}

void
HerderImpl::ballotDidCommitted(uint64 const& slotIndex, SCPBallot const& ballot)
{
    mBallotCommitted.Mark();
}

void
HerderImpl::envelopeSigned()
{
    mEnvelopeSign.Mark();
}

void
HerderImpl::envelopeVerified(bool valid)
{
    if (valid)
    {
        mEnvelopeValidSig.Mark();
    }
    else
    {
        mEnvelopeInvalidSig.Mark();
    }
}

void
HerderImpl::dumpInfo(Json::Value& ret)
{
    int count = 0;
    for (auto& item : mKnownNodes)
    {
        ret["nodes"][count++] =
            toBase58Check(VER_ACCOUNT_ID, item.second->getNodeID()).c_str();
    }

    ret["you"] = hexAbbrev(getSecretKey().getPublicKey());

    for (auto& item : mKnownSlots)
    {
        item.second->dumpInfo(ret);
    }

    count = 0;
    for(auto& item : mQuorumAheadOfUs)
    {
        for(auto& envelope : item.second)
        {
            std::ostringstream output;
            output << "i:" << item.first << " n:" <<
                binToHex(envelope.nodeID).substr(0, 6);

            ret["ahead"][count++] = output.str();
        }
    }
}
}
