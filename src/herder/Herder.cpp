// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "Herder.h"

#include <time.h>
#include "herder/TxSetFrame.h"
#include "ledger/LedgerMaster.h"
#include "overlay/PeerMaster.h"
#include "main/Application.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "lib/util/easylogging++.h"
#include "xdrpp/marshal.h"
#include "crypto/SHA.h"

namespace stellar
{

Herder::Herder(Application& app)
    : mCollectingTransactionSet(std::make_shared<TxSetFrame>())
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
    , mFBAQSetFetcher(app)
    , mLedgersToWaitToParticipate(3)
    , mApp(app)
{
    if(mApp.getConfig().START_NEW_NETWORK) 
    {
        mLedgersToWaitToParticipate = 0;
    }

    FBAQuorumSet qSetLocal;
    qSetLocal.threshold = app.getConfig().QUORUM_THRESHOLD;
    for (auto q : app.getConfig().QUORUM_SET)
    {
        qSetLocal.validators.push_back(q);
    }

    mFBA = new FBA(app.getConfig().VALIDATION_SEED,
                   qSetLocal,
                   this);
}

Herder::~Herder()
{
    delete mFBA;
}

void 
Herder::validateBallot(const uint64& slotIndex,
                       const uint256& nodeID,
                       const FBABallot& ballot,
                       const Hash& evidence,
                       std::function<void(bool)> const& cb)
{
    // make sure all the tx we have in the old set are included
    // make sure the timestamp isn't too far in the future
    // make sure the base fee is within a certain range of your desired fee
    auto validate = [=] (TxSetFramePtr txSet)
    {
        // TODO(spolu): check baseFee
        /*
        if (ballot.baseFee < mApp.getConfig().DESIRED_BASE_FEE * .5)
            return INVALID_BALLOT;
        if (ballot.baseFee > mApp.getConfig().DESIRED_BASE_FEE * 2)
            return INVALID_BALLOT;
        */

        if(!txSet->checkValid(mApp))
        {
            CLOG(ERROR, "Herder") << "invalid txSet";
            return cb(false);
        }

        for (auto tx : mReceivedTransactions[mReceivedTransactions.size() - 1])
        {
            if (find(txSet->mTransactions.begin(), txSet->mTransactions.end(),
                     tx) == txSet->mTransactions.end())
            {
                return cb(false);
            }
        }

        // TODO(spolu): check closeTime
        /*
        if (ballot.closeTime <= mLastClosedLedger.closeTime)
            return INVALID_BALLOT;
        uint64_t timeNow = time(nullptr);
        if (ballot.closeTime > timeNow + MAX_SECONDS_LEDGER_CLOSE_IN_FUTURE)
            return FUTURE_BALLOT;
        */

        return cb(true);
    };

    TxSetFramePtr txSet = fetchTxSet(ballot.valueHash, true);
    if (!txSet)
    {
        mPendingValidations[ballot.valueHash].push_back(validate);
    }
    else
    {
        validate(txSet);
    }

}

void 
Herder::ballotDidPrepare(const uint64& slotIndex,
                         const FBABallot& ballot,
                         const Hash& evidence)
{
}

void 
Herder::ballotDidCommit(const uint64& slotIndex,
                        const FBABallot& ballot)
{
}

void 
Herder::valueCancelled(const uint64& slotIndex,
                       const Hash& valueHash)
{
}

void 
Herder::valueExternalized(const uint64& slotIndex,
                          const Hash& valueHash)
{
    TxSetFramePtr externalizedSet = fetchTxSet(valueHash, false);
    if (externalizedSet)
    {
        // we don't need to keep fetching any of the old TX sets
        mTxSetFetcher[mCurrentTxSetFetcher].stopFetchingAll();

        mCurrentTxSetFetcher = mCurrentTxSetFetcher ? 0 : 1;
        mTxSetFetcher[mCurrentTxSetFetcher].clear();

        mApp.getLedgerGateway().externalizeValue(externalizedSet);

        // remove all these tx from mReceivedTransactions
        for (auto tx : externalizedSet->mTransactions)
        {
            removeReceivedTx(tx);
        }
        // rebroadcast those left in set 1
        for (auto tx : mReceivedTransactions[1])
        {
            auto msg = tx->toStellarMessage();
            mApp.getPeerMaster().broadcastMessage(msg, Peer::pointer());
        }

        // move all the remaining to the next highest level
        // don't move the largest array
        for (int n = mReceivedTransactions.size() - 2; n >= 0; n--)
        {
            for (auto tx : mReceivedTransactions[n])
            {
                mReceivedTransactions[n + 1].push_back(tx);
            }
            mReceivedTransactions[n].clear();
        }
    }
    else
    {
        // TODO(spolu): This may still be possible if we contact nodes that 
        //              have already externalized, then there is no validation
        //              and we won't have necessarily fetched the txSet.
        CLOG(ERROR, "Herder") << "externalizeValue txset not found: ";
    }
}

void 
Herder::retrieveQuorumSet(const uint256& nodeID,
                          const Hash& qSetHash)
{
    auto retrieve = [=] (FBAQuorumSetPtr qSet)
    {
        mFBA->receiveQuorumSet(nodeID, *qSet);
    };

    // TODO(spolu): [ask jed]
    //              - We know which node to ask here, can we optimize?
    FBAQuorumSetPtr qSet = fetchFBAQuorumSet(qSetHash, true);
    if (!qSet)
    {
        mPendingRetrievals[qSetHash].push_back(retrieve);
    }
    else
    {
        retrieve(qSet);
    }
}

void 
Herder::emitEnvelope(const FBAEnvelope& envelope)
{
}

void 
Herder::retransmissionHinted(const uint64& slotIndex,
                             const uint256& nodeID)
{
    // Not implemented yet. Requires a new message.
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
        auto it = mPendingValidations.find(txSet->getContentsHash());
        if (it != mPendingValidations.end())
        {
            for (auto validate : it->second)
            {
                validate(txSet);
            }
            mPendingValidations.erase(it);
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
        auto it = mPendingRetrievals.find(qSetHash);
        if (it != mPendingRetrievals.end())
        {
            for (auto retrieve : it->second)
            {
                retrieve(qSet);
            }
            mPendingRetrievals.erase(it);
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
Herder::recvFBAEnvelope(FBAEnvelope envelope)
{
    mFBA->receiveEnvelope(envelope);
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
Herder::ledgerClosed(LedgerHeader& ledger)
{
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
    recvTxSet(proposedSet);

    mLastClosedLedger = ledger;

    uint64_t firstBallotTime = time(nullptr) + NUM_SECONDS_IN_CLOSE;
    if (firstBallotTime <= mLastClosedLedger.closeTime)
        firstBallotTime = mLastClosedLedger.closeTime + 1;

    uint64_t slotIndex = mLastClosedLedger.ledgerSeq+1;

    // TODO(spolu): Evidence
    Hash evidence; 

    mFBA->attemptValue(slotIndex, 
                       proposedSet->getContentsHash(), 
                       evidence);

    // TODO(spolu): closeTime (in FBA?)
    // firstBallot.ballot.closeTime = firstBallotTime;
    
    // TODO(spolu): baseFee (in TransactionSet?)
    // firstBallot.ballot.baseFee = mApp.getConfig().DESIRED_BASE_FEE;

    // TODO(spolu) [ask jed]:
    /*
    mLedgersToWaitToParticipate--;
    // don't participate in FBA for a few ledger closes so you make sure you
    //      don't send PREPAREs that don't include old tx
    if (mLedgersToWaitToParticipate < 0  && (!isZero(mApp.getConfig().VALIDATION_SEED)))
        mApp.getFBAGateway().setValidating(true);
    */
}
}
