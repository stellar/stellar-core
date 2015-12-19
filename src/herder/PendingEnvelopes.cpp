#include "PendingEnvelopes.h"
#include "main/Application.h"
#include "herder/HerderImpl.h"
#include "crypto/Hex.h"
#include <overlay/OverlayManager.h>
#include <xdrpp/marshal.h>
#include "util/Logging.h"
#include <scp/Slot.h>
#include "herder/TxSetFrame.h"
#include "main/Config.h"

using namespace std;

#define QSET_CACHE_SIZE 10000
#define TXSET_CACHE_SIZE 10000
#define NODES_QUORUM_CACHE_SIZE 1000

namespace stellar
{

PendingEnvelopes::PendingEnvelopes(Application& app, HerderImpl& herder)
    : mApp(app)
    , mHerder(herder)
    , mQsetCache(QSET_CACHE_SIZE)
    , mTxSetFetcher(app)
    , mQuorumSetFetcher(app)
    , mTxSetCache(TXSET_CACHE_SIZE)
    , mNodesInQuorum(NODES_QUORUM_CACHE_SIZE)
    , mPendingEnvelopesSize(
          app.getMetrics().NewCounter({"scp", "memory", "pending-envelopes"}))
{
}

PendingEnvelopes::~PendingEnvelopes()
{
}

void
PendingEnvelopes::peerDoesntHave(MessageType type, Hash const& itemID,
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
    CLOG(TRACE, "Herder") << "Got SCPQSet " << hexAbbrev(hash);
    SCPQuorumSetPtr qset(new SCPQuorumSet(q));
    mQsetCache.put(hash, qset);
    mQuorumSetFetcher.recv(hash);
}

void
PendingEnvelopes::recvTxSet(Hash hash, TxSetFramePtr txset)
{
    CLOG(TRACE, "Herder") << "Got TxSet " << hexAbbrev(hash);
    mTxSetCache.put(hash, txset);
    mTxSetFetcher.recv(hash);
}

bool
PendingEnvelopes::isNodeInQuorum(NodeID const& node)
{
    bool res;

    res = mNodesInQuorum.exists(node);
    if (res)
    {
        res = mNodesInQuorum.get(node);
    }

    if (!res)
    {
        // search through the known slots
        SCP::TriBool r = mHerder.getSCP().isNodeInQuorum(node);
        if (r == SCP::TB_TRUE)
        {
            // only cache positive answers
            // so that nodes can be added during rounds
            mNodesInQuorum.put(node, true);
            res = true;
        }
        else if (r == SCP::TB_FALSE)
        {
            res = false;
        }
        else
        {
            // MAYBE -> return true, but don't cache
            res = true;
        }
    }

    return res;
}

// called from Peer and when an Item tracker completes
void
PendingEnvelopes::recvSCPEnvelope(SCPEnvelope const& envelope)
{
    auto const& nodeID = envelope.statement.nodeID;
    if (!isNodeInQuorum(nodeID))
    {
        CLOG(DEBUG, "Herder") << "Dropping envelope from "
                              << mApp.getConfig().toShortString(nodeID)
                              << " (not in quorum)";
        return;
    }

    // do we already have this envelope?
    // do we have the qset
    // do we have the txset

    try
    {
        auto& set = mFetchingEnvelopes[envelope.statement.slotIndex];
        auto& processedList = mProcessedEnvelopes[envelope.statement.slotIndex];

        auto fetching = find(set.begin(), set.end(), envelope);

        if (fetching == set.end())
        { // we aren't fetching this envelope
            if (find(processedList.begin(), processedList.end(), envelope) ==
                processedList.end())
            { // we haven't seen this envelope before
                // insert it into the fetching set
                fetching = set.insert(envelope).first;
                startFetch(envelope);
            }
            else
            {
                // we already have this one
                fetching = set.end();
            }
        }

        if (fetching != set.end())
        { // we are fetching this envelope
            // check if we are done fetching it
            if (isFullyFetched(envelope))
            {
                // move the item from fetching to processed
                processedList.emplace_back(*fetching);
                set.erase(fetching);
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

    CLOG(TRACE, "Herder") << "Envelope ready i:" << envelope.statement.slotIndex
                          << " t:" << envelope.statement.pledges.type();

    mHerder.processSCPQueue();
}

bool
PendingEnvelopes::isFullyFetched(SCPEnvelope const& envelope)
{
    if (!mQsetCache.exists(
            Slot::getCompanionQuorumSetHashFromStatement(envelope.statement)))
        return false;

    std::vector<Value> vals = Slot::getStatementValues(envelope.statement);
    for (auto const& v : vals)
    {
        StellarValue wb;
        xdr::xdr_from_opaque(v, wb);

        if (!mTxSetCache.exists(wb.txSetHash))
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
        StellarValue wb;
        xdr::xdr_from_opaque(v, wb);

        if (!mTxSetCache.exists(wb.txSetHash))
        {
            mTxSetFetcher.fetch(wb.txSetHash, envelope);
            ret = false;
        }
    }

    CLOG(TRACE, "Herder") << "StartFetch i:" << envelope.statement.slotIndex
                          << " t:" << envelope.statement.pledges.type();
    return ret;
}

bool
PendingEnvelopes::pop(uint64 slotIndex, SCPEnvelope& ret)
{
    auto it = mPendingEnvelopes.begin();
    while (it != mPendingEnvelopes.end() && slotIndex >= it->first)
    {
        auto& v = it->second;
        if (v.size() != 0)
        {
            ret = v.back();
            v.pop_back();

            return true;
        }
        it++;
    }
    return false;
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
    // stop processing envelopes & downloads for the slot falling off the
    // window
    if (slotIndex > Herder::MAX_SLOTS_TO_REMEMBER)
    {
        slotIndex -= Herder::MAX_SLOTS_TO_REMEMBER;

        mPendingEnvelopes.erase(slotIndex);

        mProcessedEnvelopes.erase(slotIndex);
        mFetchingEnvelopes.erase(slotIndex);

        mTxSetFetcher.stopFetchingBelow(slotIndex + 1);
        mQuorumSetFetcher.stopFetchingBelow(slotIndex + 1);
    }
}

TxSetFramePtr
PendingEnvelopes::getTxSet(Hash const& hash)
{
    if (mTxSetCache.exists(hash))
    {
        return mTxSetCache.get(hash);
    }

    return TxSetFramePtr();
}

SCPQuorumSetPtr
PendingEnvelopes::getQSet(Hash const& hash)
{
    if (mQsetCache.exists(hash))
    {
        return mQsetCache.get(hash);
    }

    return SCPQuorumSetPtr();
}

void
PendingEnvelopes::dumpInfo(Json::Value& ret, size_t limit)
{
    Json::Value& q = ret["queue"];

    {
        auto it = mFetchingEnvelopes.rbegin();
        size_t l = limit;
        while (it != mFetchingEnvelopes.rend() && l-- != 0)
        {
            if (it->second.size() != 0)
            {
                Json::Value& slot = q[std::to_string(it->first)]["fetching"];
                for (auto const& e : it->second)
                {
                    slot.append(mHerder.getSCP().envToStr(e));
                }
            }
            it++;
        }
    }
    {
        auto it = mPendingEnvelopes.rbegin();
        size_t l = limit;
        while (it != mPendingEnvelopes.rend() && l-- != 0)
        {
            if (it->second.size() != 0)
            {
                Json::Value& slot = q[std::to_string(it->first)]["pending"];
                for (auto const& e : it->second)
                {
                    slot.append(mHerder.getSCP().envToStr(e));
                }
            }
            it++;
        }
    }
}
}