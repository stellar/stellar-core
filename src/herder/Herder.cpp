// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Herder.h"

#include <ctime>
#include "math.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerMaster.h"
#include "overlay/PeerMaster.h"
#include "main/Application.h"
#include "main/Config.h"
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"
#include "util/Logging.h"
#include "lib/util/easylogging++.h"

namespace stellar
{

// Static helper for Herder's FBA constructor
static FBAQuorumSet
quorumSetFromApp(Application& app)
{
    FBAQuorumSet qSet;
    qSet.threshold = app.getConfig().QUORUM_THRESHOLD;
    for (auto q : app.getConfig().QUORUM_SET)
    {
        qSet.validators.push_back(q);
    }
    return qSet;
}

Herder::Herder(Application& app)
    : FBA(app.getConfig().VALIDATION_KEY,
          quorumSetFromApp(app))
    , mReceivedTransactions(4)
#ifdef _MSC_VER
    // This form of initializer causes a warning due to brace-elision on
    // clang.
    , mTxSetFetcher({ TxSetFetcher(app), TxSetFetcher(app) })
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
    , mFBAQSetFetcher(app)
    , mLedgersToWaitToParticipate(3)
    , mLastTrigger(app.getClock().now())
    , mTriggerTimer(app.getClock())
    , mBumpTimer(app.getClock())
    , mApp(app)
{
}

Herder::~Herder()
{
}

void
Herder::bootstrap()
{
    assert(mApp.getConfig().START_NEW_NETWORK);

    mLastClosedLedger = mApp.getLedgerMaster().getLastClosedLedgerHeader();
    mLedgersToWaitToParticipate = 0;
    triggerNextLedger(asio::error_code());
}

void 
Herder::validateValue(const uint64& slotIndex,
                      const uint256& nodeID,
                      const Value& value,
                      std::function<void(bool)> const& cb)
{
    StellarBallot b;
    try
    {
        xdr::xdr_from_opaque(value, b);
    }
    catch (...)
    {
        return cb(false);
    }

    // All tests that are relative to mLastClosedLedger are executed only once
    // we are fully synced up
    if (mLedgersToWaitToParticipate == 0)
    {
        // Check slotIndex.
        if (mLastClosedLedger.ledgerSeq + 1 != slotIndex)
        {
            return cb(false);
        }
        // Check closeTime (not too old)
        if (b.closeTime <= mLastClosedLedger.closeTime)
        {
            return cb(false);
        }
    }

    // make sure all the tx we have in the old set are included
    auto validate = [cb,b,this] (TxSetFramePtr txSet)
    {
        // Check txSet (only if we're fully synced)
        if(mLedgersToWaitToParticipate == 0 && !txSet->checkValid(mApp))
        {
            CLOG(ERROR, "Herder") << "invalid txSet";
            return cb(false);
        }
        
        LOG(DEBUG) << "[hrd] Herder::validateValue"
                   << "@" << binToHex(mApp.getConfig().VALIDATION_KEY.getSeed()).substr(0,6)
                   << " OK";
        return cb(true);
    };
    
    TxSetFramePtr txSet = fetchTxSet(b.txSetHash, true);
    if (!txSet)
    {
        mTxSetFetches[b.txSetHash].push_back(validate);
    }
    else
    {
        validate(txSet);
    }
}

void 
Herder::validateBallot(const uint64& slotIndex,
                       const uint256& nodeID,
                       const FBABallot& ballot,
                       std::function<void(bool)> const& cb)
{
    StellarBallot b;
    try
    {
        xdr::xdr_from_opaque(ballot.value, b);
    }
    catch (...)
    {
        return cb(false);
    }

    // Check closeTime (not too far in the future)
    uint64_t timeNow = VirtualClock::pointToTimeT(mApp.getClock().now());
    if (b.closeTime > timeNow + MAX_TIME_SLIP_SECONDS)
    {
        return cb(false);
    }

    // Check the ballot counter is not growing too rapidly. We ignore ballots
    // that were triggered before the expected series of timeouts (accepting
    // MAX_TIME_SLIP_SECONDS as error). This prevents ballot counter
    // exhaustion attacks.
    uint64_t lastTrigger = VirtualClock::pointToTimeT(mLastTrigger);
    uint64_t sumTimeouts = 0;
    for (int i = 0; i < ballot.counter; i ++)
    {
        sumTimeouts += std::min(MAX_FBA_TIMEOUT_SECONDS, (int)pow(2.0, i));
    }
    // This inequality is effectively a limitation on `ballot.counter`
    if (timeNow + MAX_TIME_SLIP_SECONDS < (int)(lastTrigger + sumTimeouts))
    {
        return cb(false);
    }

    // Check baseFee (within range of desired fee).
    if (b.baseFee < mApp.getConfig().DESIRED_BASE_FEE * .5)
    {
        return cb(false);
    }
    if (b.baseFee > mApp.getConfig().DESIRED_BASE_FEE * 2)
    {
        return cb(false);
    }

    // make sure all the tx we have in the old set are included
    auto validate = [cb,b,this] (TxSetFramePtr txSet)
    {
        // Check we have all the 3-level txs in mReceivedTransactions
        for (auto tx : mReceivedTransactions[mReceivedTransactions.size() - 1])
        {
            if (find(txSet->mTransactions.begin(), txSet->mTransactions.end(),
                     tx) == txSet->mTransactions.end())
            {
                return cb(false);
            }
        }
        
        LOG(DEBUG) << "[hrd] validateBallot"
                   << "@" << binToHex(mApp.getConfig().VALIDATION_KEY.getSeed()).substr(0,6)
                   << " OK";
        return cb(true);
    };
    
    TxSetFramePtr txSet = fetchTxSet(b.txSetHash, true);
    if (!txSet)
    {
        mTxSetFetches[b.txSetHash].push_back(validate);
    }
    else
    {
        validate(txSet);
    }
}

void 
Herder::ballotDidPrepared(const uint64& slotIndex,
                          const FBABallot& ballot)
{
    // If we're not fully synced, we just don't timeout FBA, so no need to
    // store mBumpValue
    if (mLedgersToWaitToParticipate > 0)
    {
        return;
    }
    // Only validated values (current) values should trigger this.
    assert(slotIndex != mLastClosedLedger.ledgerSeq + 1);

    // We keep around the maximal value that prepared.
    if (compareValues(mBumpValue, ballot.value) < 0)
    {
        mBumpValue = ballot.value;
    }
}

void
Herder::ballotDidHearFromQuorum(const uint64& slotIndex,
                                const FBABallot& ballot)
{
    // If we're not fully synced, we just don't timeout FBA.
    if (mLedgersToWaitToParticipate > 0)
    {
        return;
    }
    // Only validated values (current) values should trigger this.
    assert(slotIndex != mLastClosedLedger.ledgerSeq + 1);

    mBumpTimer.cancel();

    // Once we hear from a transitive quorum, we start a timer in case FBA
    // timeouts.
    mBumpTimer.async_wait(std::bind(&Herder::expireBallot, this, 
                                    std::placeholders::_1, 
                                    slotIndex, ballot));
    mBumpTimer.expires_from_now(
        std::chrono::seconds((int)pow(2.0, ballot.counter)));
}

void 
Herder::valueExternalized(const uint64& slotIndex,
                          const Value& value)
{
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
        CLOG(ERROR, "Herder") << "Externalized StellarBallot malformed";
    }
    
    TxSetFramePtr externalizedSet = fetchTxSet(b.txSetHash, false);
    if (externalizedSet)
    {
        // we don't need to keep fetching any of the old TX sets
        mTxSetFetcher[mCurrentTxSetFetcher].stopFetchingAll();

        mCurrentTxSetFetcher = mCurrentTxSetFetcher ? 0 : 1;
        mTxSetFetcher[mCurrentTxSetFetcher].clear();

        // Triggers sync if not already syncing.
        mApp.getLedgerGateway().externalizeValue(externalizedSet);

        // remove all these tx from mReceivedTransactions
        for (auto tx : externalizedSet->mTransactions)
        {
            removeReceivedTx(tx);
        }
        // rebroadcast those left in set 1
        assert(mReceivedTransactions.size() >= 2);
        for (auto tx : mReceivedTransactions[1])
        {
            auto msg = tx->toStellarMessage();
            mApp.getOverlayGateway().broadcastMessage(msg);
        }

        // move all the remaining to the next highest level
        // don't move the largest array
        for (size_t n = mReceivedTransactions.size() - 1; n > 0; n--)
        {
            for (auto tx : mReceivedTransactions[n-1])
            {
                mReceivedTransactions[n].push_back(tx);
            }
            mReceivedTransactions[n-1].clear();
        }
    }
    else
    {
        // This may not be possible as all messages are validated and should
        // therefore fetch the txSet before being considered by FBA.
        CLOG(ERROR, "Herder") << "Externalized txSet not found";
    }
}

void 
Herder::retrieveQuorumSet(const uint256& nodeID,
                          const Hash& qSetHash,
                          std::function<void(const FBAQuorumSet&)> const& cb)
{
    auto retrieve = [cb] (FBAQuorumSetPtr qSet)
    {
        return cb(*qSet);
    };

    // Peer Overlays and nodeIDs have no relationship for now. Sow we just
    // retrieve qSetHash by asking the whole overlay.
    FBAQuorumSetPtr qSet = fetchFBAQuorumSet(qSetHash, true);
    if (!qSet)
    {
        mFBAQSetFetches[qSetHash].push_back(retrieve);
    }
    else
    {
        retrieve(qSet);
    }
}

void 
Herder::emitEnvelope(const FBAEnvelope& envelope)
{
    LOG(DEBUG) << "[hrd] Herder:emitEnvelope"
               << "@" << binToHex(mApp.getConfig().VALIDATION_KEY.getSeed()).substr(0,6)
               << " mLedgersToWaitToParticipate: " << mLedgersToWaitToParticipate;
    // We don't emit any envelope as long as we're not fully synced
    if (mLedgersToWaitToParticipate > 0)
    {
        return;
    }
    
    StellarMessage msg;
    msg.type(FBA_MESSAGE);
    msg.envelope() = envelope;

    mApp.getOverlayGateway().broadcastMessage(msg);
}

TxSetFramePtr
Herder::fetchTxSet(uint256 const& setHash, 
                   bool askNetwork)
{
    return mTxSetFetcher[mCurrentTxSetFetcher].fetchItem(setHash, askNetwork);
}

void
Herder::recvTxSet(TxSetFramePtr txSet)
{
    if (mTxSetFetcher[mCurrentTxSetFetcher].recvItem(txSet))
    { 
        // someone cares about this set
        for (auto tx : txSet->mTransactions)
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
Herder::doesntHaveTxSet(uint256 const& txSetHash, 
                        PeerPtr peer)
{
    mTxSetFetcher[mCurrentTxSetFetcher].doesntHave(txSetHash, peer);
}


FBAQuorumSetPtr
Herder::fetchFBAQuorumSet(uint256 const& qSetHash, 
                          bool askNetwork)
{
    return mFBAQSetFetcher.fetchItem(qSetHash, askNetwork);
}

void 
Herder::recvFBAQuorumSet(FBAQuorumSetPtr qSet)
{
    if (mFBAQSetFetcher.recvItem(qSet))
    { 
        // someone cares about this set
        uint256 qSetHash = sha512_256(xdr::xdr_to_msg(*qSet));

        // Runs any pending retrievals on this qSet
        auto it = mFBAQSetFetches.find(qSetHash);
        if (it != mFBAQSetFetches.end())
        {
            for (auto retrieve : it->second)
            {
                retrieve(qSet);
            }
            mFBAQSetFetches.erase(it);
        }
    }
}

void 
Herder::doesntHaveFBAQuorumSet(uint256 const& qSetHash, 
                               PeerPtr peer)
{
    mFBAQSetFetcher.doesntHave(qSetHash, peer);
}



bool
Herder::recvTransaction(TransactionFramePtr tx)
{
    Hash& txID = tx->getFullHash();

    // determine if we have seen this tx before and if not if it has the right
    // seq num
    int numOthers=0;
    for (auto list : mReceivedTransactions)
    {
        for (auto oldTX : list)
        {
            if (txID == oldTX->getFullHash())
            {
                return false;
            }
            if (oldTX->getSourceID() == tx->getSourceID())
            {
                numOthers++;
            }
        }
    }

    if (!tx->loadAccount(mApp)) 
    {
        return false;
    }
    
    // don't flood any tx with to old a seq num
    if (tx->getSeqNum() < tx->getSourceAccount().getSeqNum() + 1) 
    {
        return false;
    }
    
    // don't consider minBalance since you want to allow them to still send
    // around credit etc
    if (tx->getSourceAccount().getBalance() < 
        (numOthers + 1) * mApp.getLedgerGateway().getTxFee())
    {
        return false;
    }

    if (!tx->checkValid(mApp)) 
    {
        return false;
    }
       
    mReceivedTransactions[0].push_back(tx);

    return true;
}

void
Herder::recvFBAEnvelope(FBAEnvelope envelope,
                        std::function<void(bool)> const& cb)
{
    if (mLedgersToWaitToParticipate == 0)
    {
        uint64 minLedgerSeq = ((int)mLastClosedLedger.ledgerSeq -
            LEDGER_VALIDITY_BRACKET) < 0 ? 0 :
            (mLastClosedLedger.ledgerSeq - LEDGER_VALIDITY_BRACKET);
        uint64 maxLedgerSeq = mLastClosedLedger.ledgerSeq +
            LEDGER_VALIDITY_BRACKET;

        // If we are fully synced and the envelopes are out of our validity
        // brackets, we just ignore them.
        if(envelope.slotIndex > maxLedgerSeq ||
           envelope.slotIndex < minLedgerSeq)
        {
            return;
        }

        // If we are fully synced and we see envelopes that are from future
        // ledgers we store them for later replay.
        if (envelope.slotIndex > mLastClosedLedger.ledgerSeq + 1)
        {
            mFutureEnvelopes[envelope.slotIndex]
                .push_back(std::make_pair(envelope, cb));
        }
    }

    return receiveEnvelope(envelope, cb);
}

void
Herder::ledgerClosed(LedgerHeader& ledger)
{
    // TODO(spolu) make sure this is called and when?
    
    mLastClosedLedger = ledger;

    // We start skipping ledgers only after we're in SYNCED_STATE
    if (mLedgersToWaitToParticipate > 0 &&
        mApp.getState() != Application::State::SYNCED_STATE)
    {
        mLedgersToWaitToParticipate--;
    }

    // If we haven't waited for a couple ledgers after we got in SYNCED_STATE
    // we consider ourselves not fully synced so we don't push any value.
    if (mLedgersToWaitToParticipate > 0)
    {
        return;
    }

    // We trigger next ledger EXP_LEDGER_TIMESPAN_SECONDS after our last
    // trigger.
    mTriggerTimer.cancel();
    mTriggerTimer.async_wait(std::bind(&Herder::triggerNextLedger, this,
                                       std::placeholders::_1));
    
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
}

void
Herder::removeReceivedTx(TransactionFramePtr dropTx)
{
    for (auto list : mReceivedTransactions)
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

void
Herder::triggerNextLedger(const asio::error_code& error)
{
    assert(!error);
    
    // We store at which time we triggered consensus
    mLastTrigger = mApp.getClock().now();

    // our first choice for this round's set is all the tx we have collected
    // during last ledger close
    TxSetFramePtr proposedSet = std::make_shared<TxSetFrame>();
    for (auto list : mReceivedTransactions)
    {
        for (auto tx : list)
        {
            proposedSet->add(tx);
        }
    }
    proposedSet->mPreviousLedgerHash = mLastClosedLedger.hash;

    // TODO(spolu) we shouldn't need to ping the network to store the TxSet
    //             within the ItemFetcher
    fetchTxSet(proposedSet->getContentsHash(), true);
    recvTxSet(proposedSet);

    uint64_t slotIndex = mLastClosedLedger.ledgerSeq + 1;

    // We pick as next close time the current time unless it's before the last
    // close time. We don't know how much time it will take to reach consensus
    // so this is the most appropriate value to use as closeTime.
    uint64_t nextCloseTime = VirtualClock::pointToTimeT(mLastTrigger);
    if (nextCloseTime <= mLastClosedLedger.closeTime)
    {
        nextCloseTime = mLastClosedLedger.closeTime + 1;
    }

    StellarBallot b;
    b.txSetHash = proposedSet->getContentsHash();
    b.closeTime = nextCloseTime;
    b.baseFee = mApp.getConfig().DESIRED_BASE_FEE;

    Value x = xdr::xdr_to_opaque(b);

    uint256 valueHash = sha512_256(xdr::xdr_to_msg(x));
    LOG(DEBUG) << "[hrd] Herder::triggerNextLedger"
               << "@" << binToHex(mApp.getConfig().VALIDATION_KEY.getSeed()).substr(0,6)
               << " txSet.size: " << proposedSet->mTransactions.size()
               << " previousLedgerHash: " << binToHex(proposedSet->mPreviousLedgerHash).substr(0,6)
               << " value: " << binToHex(valueHash).substr(0,6);

    mBumpValue = x;
    prepareValue(slotIndex, x);

    for (auto p : mFutureEnvelopes[slotIndex])
    {
        recvFBAEnvelope(p.first, p.second);
    }
    mFutureEnvelopes.erase(slotIndex);
}

void
Herder::expireBallot(const asio::error_code& error,
                     const uint64& slotIndex,
                     const FBABallot& ballot)
                     
{
    // The timer was simply cancelled, nothing to do.
    if (error == asio::error::operation_aborted)
    {
        return;
    }

    assert(slotIndex == mLastClosedLedger.ledgerSeq + 1);

    prepareValue(slotIndex, mBumpValue, true);
}


}
