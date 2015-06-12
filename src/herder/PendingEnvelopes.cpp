#include "PendingEnvelopes.h"
#include "main/Application.h"
#include "herder/HerderImpl.h"
#include "crypto/Hex.h"
#include <overlay/OverlayManager.h>
#include <xdrpp/marshal.h>
#include "util/Logging.h"
#include <scp/Slot.h>
#include "herder/TxSetFrame.h"
#include "main/Application.h"

using namespace std;

#define QSET_CACHE_SIZE 10000
#define TXSET_CACHE_SIZE 10000

namespace stellar
{

PendingEnvelopes::PendingEnvelopes(Application& app, HerderImpl& herder)
    : mHerder(herder)
    , mApp(app)
    , mQsetCache(QSET_CACHE_SIZE)
    , mTxSetCache(TXSET_CACHE_SIZE)
    , mQuorumSetFetcher(app)
    , mTxSetFetcher(app)
    , mPendingEnvelopesSize(
          app.getMetrics().NewCounter({"scp", "memory", "pending-envelopes"}))
{
}

PendingEnvelopes::~PendingEnvelopes()
{
}

void
PendingEnvelopes::peerDoesntHave(MessageType type, uint256 const& itemID,
                                 Peer::pointer peer)
{
    switch (type)
    {
    case TX_SET:
        mTxSetFetcher.doesntHave(itemID, peer);
        break;
    case SCP_QUORUMSET:
        mQuorumSetFetcher.doesntHave(itemID, peer);
        break;
    default:
        CLOG(INFO, "Herder") << "Unknown Type in peerDoesntHave: " << type;
        break;
    }
}

void
PendingEnvelopes::recvSCPQuorumSet(Hash hash, const SCPQuorumSet& q)
{
    SCPQuorumSetPtr qset(new SCPQuorumSet(q));
    mQsetCache.put(hash, qset);
    mQuorumSetFetcher.recv(hash);
}

void
PendingEnvelopes::recvTxSet(Hash hash, TxSetFramePtr txset)
{
    mTxSetCache.put(hash, txset);
    mTxSetFetcher.recv(hash);
}

// called from Peer and when an Item tracker completes
void
PendingEnvelopes::recvSCPEnvelope(SCPEnvelope const& envelope)
{
    // do we already have this envelope?
    // do we have the qset
    // do we have the txset

    try
    {
        auto& set = mFetchingEnvelopes[envelope.statement.slotIndex];

        if (find(set.begin(), set.end(), envelope) == set.end())
        { // we aren't fetching this envelop

            auto& receivedList =
                mReceivedEnvelopes[envelope.statement.slotIndex];
            if (find(receivedList.begin(), receivedList.end(), envelope) ==
                receivedList.end())
            { // we haven't seen this envelope before

                receivedList.push_back(envelope);
                if (startFetch(envelope))
                { // fully fetched
                    envelopeReady(envelope);
                }
                else
                {
                    mFetchingEnvelopes[envelope.statement.slotIndex].insert(
                        envelope);
                }

                CLOG(DEBUG, "Herder") << "PendingEnvelopes::recvSCPEnvelope";

            } // else we already have this one
        }
        else
        { // we are fetching this envelope
            // check if we are done fetching it
            if (isFullyFetched(envelope))
            {
                envelopeReady(envelope);
            } // else just keep waiting for it to come in
        }
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(TRACE, "Herder")
            << "PendingEnvelopes::recvSCPEnvelope got corrupt message: "
            << e.what();
    }
}

void
PendingEnvelopes::envelopeReady(SCPEnvelope const& envelope)
{
    StellarMessage msg;
    msg.type(SCP_MESSAGE);
    msg.envelope() = envelope;
    mApp.getOverlayManager().broadcastMessage(msg);

    mPendingEnvelopes[envelope.statement.slotIndex].push_back(envelope);

    /*
    // if envelope is on the right slot send into SCP
    // TODO.1 : below
    if(checkFutureCommitted(envelope))
    {
        mIsFutureCommitted.insert(envelope.statement.slotIndex);
    }
    */

    mHerder.processSCPQueue();
}

bool
PendingEnvelopes::isFullyFetched(SCPEnvelope const& envelope)
{
    if (!mQsetCache.exists(Slot::getCompanionQuorumSetHashFromStatement(envelope.statement)))
        return false;

    std::vector<Value> vals = Slot::getStatementValues(envelope.statement);
    for (auto const& v : vals)
    {
        StellarBallot wb;
        xdr::xdr_from_opaque(v, wb);

        if (!mTxSetCache.exists(wb.stellarValue.txSetHash))
            return false;
    }

    return true;
}

// returns true if already fetched
bool
PendingEnvelopes::startFetch(SCPEnvelope const& envelope)
{
    bool ret = true;

    Hash h = Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);

    if (!mQsetCache.exists(h))
    {
        mQuorumSetFetcher.fetch(h, envelope);
        ret = false;
    }

    std::vector<Value> vals = Slot::getStatementValues(envelope.statement);
    for (auto const& v : vals)
    {
        StellarBallot wb;
        xdr::xdr_from_opaque(v, wb);

        if (!mTxSetCache.exists(wb.stellarValue.txSetHash))
        {
            mTxSetFetcher.fetch(wb.stellarValue.txSetHash, envelope);
            ret = false;
        }
    }

    return ret;
}

bool
PendingEnvelopes::pop(uint64 slotIndex, SCPEnvelope& ret)
{
    if (mPendingEnvelopes.find(slotIndex) == mPendingEnvelopes.end())
    {
        return false;
    }
    else
    {
        if (!mPendingEnvelopes[slotIndex].size())
            return false;
        ret = mPendingEnvelopes[slotIndex].back();
        mPendingEnvelopes[slotIndex].pop_back();

        return true;
    }
}

vector<uint64>
PendingEnvelopes::readySlots()
{
    vector<uint64> result;
    for (auto const& entry : mPendingEnvelopes)
    {
        if (!entry.second.empty())
            result.push_back(entry.first);
    }
    return result;
}

void
PendingEnvelopes::eraseBelow(uint64 slotIndex)
{
    for (auto iter = mPendingEnvelopes.begin();
         iter != mPendingEnvelopes.end();)
    {
        if (iter->first < slotIndex)
        {
            iter = mPendingEnvelopes.erase(iter);
        }
        else
            break;
    }

    for (auto iter = mFetchingEnvelopes.begin();
         iter != mFetchingEnvelopes.end();)
    {
        if (iter->first < slotIndex)
        {
            iter = mFetchingEnvelopes.erase(iter);
        }
        else
            break;
    }
}

void
PendingEnvelopes::slotClosed(uint64 slotIndex)
{
    mPendingEnvelopes.erase(slotIndex);

    // keep the last few ledgers worth of messages around to give to people
    mReceivedEnvelopes.erase(slotIndex - 10);
    mFetchingEnvelopes.erase(slotIndex);

    mTxSetFetcher.stopFetchingBelow(slotIndex + 1);
    mQuorumSetFetcher.stopFetchingBelow(slotIndex + 1);
}

TxSetFramePtr
PendingEnvelopes::getTxSet(Hash hash)
{
    if (mTxSetCache.exists(hash))
    {
        return mTxSetCache.get(hash);
    }

    return TxSetFramePtr();
}

SCPQuorumSetPtr
PendingEnvelopes::getQSet(Hash hash)
{
    if (mQsetCache.exists(hash))
    {
        return mQsetCache.get(hash);
    }

    return SCPQuorumSetPtr();
}

void
PendingEnvelopes::dumpInfo(Json::Value& ret)
{
    /* TODO.1
    int count = 0;
    for(auto& entry : mFetching)
    {
        for(auto& fRecord : entry.second)
        {
            auto & envelope = fRecord->env;
            ostringstream output;
            output << "i:" << entry.first
                << " n:" << binToHex(envelope->nodeID).substr(0, 6);

            ret["pending"][count++] = output.str();
        }
    }
    */
}

///////////////////////////
//////

/* TODO.1

// returns true if we have been left behind :(
// see: walter the lazy mouse
bool
PendingEnvelopes::isFutureCommitted(uint64 slotIndex)
{
    return mIsFutureCommitted.find(slotIndex) != mIsFutureCommitted.end();
}


bool
PendingEnvelopes::checkFutureCommitted(SCPEnvelope newEnvelope)
{
    // is this a committed statement?
    if (newEnvelope.statement.pledges.type() == COMMITTED)
    {
        SCPQuorumSet const& qset = mHerder.getLocalQuorumSet();

        // is it from someone we care about?
        if (find(qset.validators.begin(), qset.validators.end(),
            newEnvelope.nodeID) != qset.validators.end())
        {
            vector<FetchingRecordPtr> allRecords;
            auto slot = newEnvelope.statement.slotIndex;
            copy(mFetching[slot].begin(), mFetching[slot].end(),
back_inserter(allRecords));
            copy(mReady[slot].begin(), mReady[slot].end(),
back_inserter(allRecords));

            auto count = count_if(allRecords.begin(), allRecords.end(),
[&](FetchingRecordPtr fRecord)
            {
                return fRecord->env->statement.ballot.value ==
                    newEnvelope.statement.ballot.value;
            });

            // do we have enough of these for the same ballot?
            if (count >= qset.threshold)
            {
                return true;
            }
        }
    }
    return false;
}



TxSetFramePtr PendingEnvelopes::getTxSet(Hash txSetHash)
{
    auto result = mApp.getOverlayManager().getTxSetFetcher().get(txSetHash);
    assert(result);
    return result;
}

SCPQuorumSetPtr PendingEnvelopes::getQuorumSet(Hash qSetHash)
{
    auto result = mApp.getOverlayManager().getQuorumSetFetcher().get(qSetHash);
    assert(result);
    return result;
}

*/
}
