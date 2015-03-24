// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "crypto/Hex.h"
#include "crypto/SHA.h"
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

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "xdrpp/marshal.h"

#include <ctime>

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
    assert(mApp.getConfig().START_NEW_NETWORK);

    mApp.setState(Application::SYNCED_STATE);
    mLastClosedLedger = mApp.getLedgerManager().getLastClosedLedgerHeader();
    triggerNextLedger();
}

void
HerderImpl::validateValue(const uint64& slotIndex, const uint256& nodeID,
                      const Value& value, std::function<void(bool)> const& cb)
{
    if (mApp.getState() != Application::SYNCED_STATE)
    { // if we aren't synced to the network we can't validate
        return cb(true);
    }

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
                << "@" << binToHex(getLocalNodeID()).substr(0, 6)
                << " i: " << slotIndex
                << " v: " << binToHex(nodeID).substr(0, 6) << " Invalid txSet:"
                << " " << binToHex(txSet->getContentsHash()).substr(0, 6);
            this->mValueInvalid.Mark();
            return cb(false);
        }

        CLOG(DEBUG, "Herder")
            << "HerderImpl::validateValue"
            << "@" << binToHex(getLocalNodeID()).substr(0, 6)
            << " i: " << slotIndex << " v: " << binToHex(nodeID).substr(0, 6)
            << " txSet:"
            << " " << binToHex(txSet->getContentsHash()).substr(0, 6) << " OK";
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
HerderImpl::compareValues(const uint64& slotIndex, const uint32& ballotCounter,
                      const Value& v1, const Value& v2)
{
    using xdr::operator<;

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
            << "@" << binToHex(getLocalNodeID()).substr(0, 6)
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
        return -1;

    return 0;
}

void
HerderImpl::validateBallot(const uint64& slotIndex, const uint256& nodeID,
                       const SCPBallot& ballot,
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
                          << "@" << binToHex(getLocalNodeID()).substr(0, 6)
                          << " i: " << slotIndex
                          << " v: " << binToHex(nodeID).substr(0, 6)
                          << " o: " << binToHex(b.nodeID).substr(0, 6)
                          << " b: (" << ballot.counter << ","
                          << binToHex(valueHash).substr(0, 6) << ")"
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
            << "@" << binToHex(getLocalNodeID()).substr(0, 6)
            << " i: " << slotIndex << " v: " << binToHex(nodeID).substr(0, 6)
            << " o: " << binToHex(b.nodeID).substr(0, 6) << " b: ("
            << ballot.counter << "," << binToHex(valueHash).substr(0, 6) << ")"
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
    }
}

void
HerderImpl::ballotDidHearFromQuorum(const uint64& slotIndex,
                                const SCPBallot& ballot)
{
    mQuorumHeard.Mark();

    // Only validated values (current) values should trigger this.
    assert(slotIndex == mLastClosedLedger.header.ledgerSeq + 1);

    mBumpTimer.cancel();

    // Once we hear from a transitive quorum, we start a timer in case SCP
    // timeouts.
    mBumpTimer.expires_from_now(
        std::chrono::seconds((int)pow(2.0, ballot.counter)));

    // TODO: Bumping on a timeout disabled for now, tends to stall scp
    // mBumpTimer.async_wait([&]() { expireBallot(slotIndex, ballot); },
    // &VirtualTimer::onFailureNoop);
}

void
HerderImpl::valueExternalized(const uint64& slotIndex, const Value& value)
{
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
                              << "@" << binToHex(getLocalNodeID()).substr(0, 6)
                              << " Externalized StellarBallot malformed";
    }

    TxSetFramePtr externalizedSet = fetchTxSet(b.value.txSetHash, false);
    if (externalizedSet)
    {

        CLOG(INFO, "Herder")
            << "HerderImpl::valueExternalized"
            << "@" << binToHex(getLocalNodeID()).substr(0, 6)
            << " txSet: " << binToHex(b.value.txSetHash).substr(0, 6);

        // we don't need to keep fetching any of the old TX sets
        mTxSetFetcher[mCurrentTxSetFetcher].stopFetchingAll();

        mCurrentTxSetFetcher = mCurrentTxSetFetcher ? 0 : 1;
        mTxSetFetcher[mCurrentTxSetFetcher].clear();

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
        if (slotIndex > LEDGER_VALIDITY_BRACKET)
        {
            purgeSlots(slotIndex - LEDGER_VALIDITY_BRACKET);
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
        // This may not be possible as all messages are validated and should
        // therefore fetch the txSet before being considered by SCP.
        CLOG(ERROR, "Herder") << "HerderImpl::valueExternalized"
                              << "@" << binToHex(getLocalNodeID()).substr(0, 6)
                              << " Externalized txSet not found";
    }
}

void
HerderImpl::nodeTouched(const uint256& nodeID)
{
    // We simply store the time of last access each time a node is touched by
    // SCP. That way we can evict old irrelevant nodes at each round.
    mNodeLastAccess[nodeID] = mApp.getClock().now();
}

void
HerderImpl::retrieveQuorumSet(const uint256& nodeID, const Hash& qSetHash,
                          std::function<void(const SCPQuorumSet&)> const& cb)
{
    mQsetRetrieve.Mark();
    CLOG(DEBUG, "Herder") << "HerderImpl::retrieveQuorumSet"
                          << "@" << binToHex(getLocalNodeID()).substr(0, 6)
                          << " qSet: " << binToHex(qSetHash).substr(0, 6);
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
    }
    else
    {
        retrieve(qSet);
    }
}

void
HerderImpl::rebroadcast()
{
    CLOG(DEBUG, "Herder") << "HerderImpl:rebroadcast"
                          << "@" << binToHex(getLocalNodeID()).substr(0, 6);

    mEnvelopeEmit.Mark();
    mApp.getOverlayManager().broadcastMessage(mLastSentMessage, true);
    startRebroadcastTimer();
}

void
HerderImpl::startRebroadcastTimer()
{
    if (!mApp.getConfig().MANUAL_CLOSE)
    {
        mRebroadcastTimer.expires_from_now(std::chrono::seconds(2));

        mRebroadcastTimer.async_wait(std::bind(&HerderImpl::rebroadcast, this),
                                     &VirtualTimer::onFailureNoop);
    }
}

void
HerderImpl::emitEnvelope(const SCPEnvelope& envelope)
{
    // We don't emit any envelope as long as we're not fully synced
    if (mApp.getState() != Application::SYNCED_STATE)
    {
        return;
    }

    mLastSentMessage.type(SCP_MESSAGE);
    mLastSentMessage.envelope() = envelope;

    rebroadcast();
}

TxSetFramePtr
HerderImpl::fetchTxSet(const uint256& txSetHash, bool askNetwork)
{
    return mTxSetFetcher[mCurrentTxSetFetcher].fetchItem(txSetHash, askNetwork);
}

void
HerderImpl::recvTxSet(TxSetFramePtr txSet)
{
    if (mTxSetFetcher[mCurrentTxSetFetcher].recvItem(txSet))
    {
        // someone cares about this set
        for (auto tx : txSet->sortForApply())
        {
            recvTransaction(tx);
        }

        // Runs any pending validation on this txSet.
        auto it = mTxSetFetches.find(txSet->getContentsHash());
        if (it != mTxSetFetches.end())
        {
            for (auto validate : it->second)
            {
                validate(txSet);
            }
            mTxSetFetches.erase(it);
        }
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
    CLOG(DEBUG, "Herder") << "HerderImpl::recvSCPQuorumSet"
                          << "@" << binToHex(getLocalNodeID()).substr(0, 6)
                          << " qSet: "
                          << binToHex(sha256(xdr::xdr_to_opaque(*qSet)))
                                 .substr(0, 6);

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

    // don't consider minBalance since you want to allow them to still send
    // around credit etc
    if (tx->getSourceAccount().getBalance() < totFee)
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
    CLOG(DEBUG, "Herder") << "HerderImpl::recvSCPEnvelope@"
                          << "@" << binToHex(getLocalNodeID()).substr(0, 6);

    if (mApp.getState() == Application::SYNCED_STATE)
    {
        uint32_t minLedgerSeq =
            (mLastClosedLedger.header.ledgerSeq < LEDGER_VALIDITY_BRACKET)
                ? 0
                : (mLastClosedLedger.header.ledgerSeq -
                   LEDGER_VALIDITY_BRACKET);
        uint32_t maxLedgerSeq =
            mLastClosedLedger.header.ledgerSeq + LEDGER_VALIDITY_BRACKET;

        // If we are fully synced and the envelopes are out of our validity
        // brackets, we just ignore them.
        if (envelope.statement.slotIndex > maxLedgerSeq ||
            envelope.statement.slotIndex < minLedgerSeq)
        {
            return;
        }

        // If we are fully synced and we see envelopes that are from future
        // ledgers we store them for later replay.
        // we also need to store envelopes for the upcoming SCP round if needed
        uint32_t nextLedger = mLastClosedLedger.header.ledgerSeq + 1;

        if (envelope.statement.slotIndex > nextLedger ||
            (envelope.statement.slotIndex == nextLedger &&
             mCurrentValue.empty()))
        {
            mFutureEnvelopes[envelope.statement.slotIndex].push_back(
                std::make_pair(envelope, cb));
            return;
        }
    }
    startRebroadcastTimer();

    mEnvelopeReceive.Mark();
    return receiveEnvelope(envelope, cb);
}

void
HerderImpl::ledgerClosed(LedgerHeaderHistoryEntry const& ledger)
{
    CLOG(TRACE, "Herder") << "HerderImpl::ledgerClosed@"
                          << "@" << binToHex(getLocalNodeID()).substr(0, 6)
                          << " ledger: " << binToHex(ledger.hash).substr(0, 6);

    // we're not running SCP anymore
    mCurrentValue.clear();

    mLastClosedLedger = ledger;

    // As the current slotIndex changes we cancel all pending validation
    // timers. Since the value externalized, the messages that this generates
    // wont' have any impact.
    mBallotValidationTimers.clear();

    // If we are not a validating not and just watching SCP we don't call
    // triggerNextLedger
    if (getSecretKey().isZero())
    {
        return;
    }

    // We trigger next ledger EXP_LEDGER_TIMESPAN_SECONDS after our last
    // trigger.
    mTriggerTimer.cancel();

    auto now = mApp.getClock().now();
    if ((now - mLastTrigger) <
        std::chrono::seconds(EXP_LEDGER_TIMESPAN_SECONDS))
    {
        auto timeout = std::chrono::seconds(EXP_LEDGER_TIMESPAN_SECONDS) -
                       (now - mLastTrigger);
        mTriggerTimer.expires_from_now(timeout);
    }
    else
    {
        mTriggerTimer.expires_from_now(std::chrono::nanoseconds(0));
    }

    if (!mApp.getConfig().MANUAL_CLOSE)
        mTriggerTimer.async_wait(std::bind(&HerderImpl::triggerNextLedger, this),
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
    // We store at which time we triggered consensus
    mLastTrigger = mApp.getClock().now();

    // our first choice for this round's set is all the tx we have collected
    // during last ledger close
    TxSetFramePtr proposedSet =
        std::make_shared<TxSetFrame>(mLastClosedLedger.hash);
    for (auto& list : mReceivedTransactions)
    {
        for(auto iter = list.begin(); iter != list.end(); )
        {
            auto& tx = *iter;
            if(tx->checkValid(mApp, 0))
            {
                proposedSet->add(tx);
                iter++;
            } else
            { // still include badseq since they could still be valid
                if(tx->getResultCode() == txBAD_SEQ)
                {
                    proposedSet->add(tx);
                    iter++;
                } else
                { // drop txs that will never be valid
                    iter = list.erase(iter);
                }
            }
        }
    }

    proposedSet->sortForHash();
    proposedSet->checkValid(mApp, true);

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
    CLOG(INFO, "Herder") << "HerderImpl::triggerNextLedger"
                         << "@" << binToHex(getLocalNodeID()).substr(0, 6)
                         << " txSet.size: " << proposedSet->mTransactions.size()
                         << " previousLedgerHash: "
                         << binToHex(proposedSet->previousLedgerHash())
                                .substr(0, 6)
                         << " value: " << binToHex(valueHash).substr(0, 6);

    // We prepare that value. If we're monarch, the ballot will be validated,
    // and
    // if we're not it'll just get ignored.
    mValuePrepare.Mark();
    prepareValue(slotIndex, mCurrentValue);

    for (auto& p : mFutureEnvelopes[slotIndex])
    {
        recvSCPEnvelope(p.first, p.second);
    }
    mFutureEnvelopes.erase(slotIndex);
}

void
HerderImpl::expireBallot(const uint64& slotIndex, const SCPBallot& ballot)

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
HerderImpl::verifyStellarBallot(const StellarBallot& b)
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
HerderImpl::ballotDidPrepare(const uint64& slotIndex, const SCPBallot& ballot)
{
    mBallotPrepare.Mark();
}

void
HerderImpl::ballotDidPrepared(const uint64& slotIndex, const SCPBallot& ballot)
{
    mBallotPrepared.Mark();
}

void
HerderImpl::ballotDidCommit(const uint64& slotIndex, const SCPBallot& ballot)
{
    mBallotCommit.Mark();
}

void
HerderImpl::ballotDidCommitted(const uint64& slotIndex, const SCPBallot& ballot)
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
}
