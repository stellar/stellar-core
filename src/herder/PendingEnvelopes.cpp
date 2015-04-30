#include "PendingEnvelopes.h"
#include "main/Application.h"
#include "herder/HerderImpl.h"
#include "crypto/Hex.h"
#include <overlay/OverlayManager.h>
#include <xdrpp/marshal.h>
#include "util/Logging.h"
#include <scp/Slot.h>

namespace stellar
{

using namespace std;

PendingEnvelopes::PendingEnvelopes(Application &app, HerderImpl &herder) :
    mApp(app)
    , mHerder(herder)
    , mPendingEnvelopesSize(
         app.getMetrics().NewCounter({ "scp", "memory", "pending-envelopes" }))
{}


void 
PendingEnvelopes::add(SCPEnvelope const &envelope)
{
    auto& set = mFetching[envelope.statement.slotIndex];

    if (find_if(set.begin(), set.end(),
        [&](FetchingRecordPtr fRecord) { return *fRecord->env == envelope;  }) == set.end())
    {
        fetch(envelope);
        mMinSlot = min(mMinSlot, envelope.statement.slotIndex);

        if (checkFutureCommitted(envelope))
        {
            mIsFutureCommitted.insert(envelope.statement.slotIndex);
        }

        mPendingEnvelopesSize.set_count(mFetching.size());
    }
}

void
PendingEnvelopes::erase(uint64 slotIndex)
{
    mFetching.erase(slotIndex);
    mReady.erase(slotIndex);
    mDone.erase(slotIndex);
    mIsFutureCommitted.erase(slotIndex);
}


vector<uint64>
PendingEnvelopes::readySlots()
{
    vector<uint64> result;
    for(auto entry : mReady)
    {
        if (!entry.second.empty())
            result.push_back(entry.first);
    }
    return result;
}

void
PendingEnvelopes::eraseBelow(uint64 slotIndex)
{
    set<uint64> allSlots;
    if (slotIndex < mMinSlot)
    {
        return;
    }
    for (auto entry : mFetching)
    {
        allSlots.insert(entry.first);
    }
    for (auto entry : mReady)
    {
        allSlots.insert(entry.first);
    }
    for (auto slot : mIsFutureCommitted)
    {
        allSlots.insert(slot);
    }
    for (auto entry : mDone)
    {
        allSlots.insert(entry.first);
    }

    int64_t size = 0;
    for (auto & slot : allSlots)
    {
        if (slot < slotIndex)
        {
            erase(slot);
        }
        size += mFetching[slot].size() + mReady[slot].size();

        // also drop empty slots while we are here
        if (mFetching[slot].empty())
            mFetching.erase(slot);
        if (mReady[slot].empty())
            mReady.erase(slot);
        if (mDone[slot].empty())
            mDone.erase(slot);
    }
    mMinSlot = slotIndex;

    if (mPendingEnvelopesSize.count() != size)
    {
        mPendingEnvelopesSize.set_count(size);
    }
}

optional<PendingEnvelopes::FetchingRecord> 
PendingEnvelopes::pop(uint64 slotIndex)
{
    if (mReady[slotIndex].empty())
    {
        return nullptr;
    } else
    {
        auto result = mReady[slotIndex].front();
        mReady[slotIndex].pop_front();

        auto holding = make_shared<FetchingRecord>(*result);
        // we don't need to keep the envelopes anymore, just the trackers
        holding->env.reset(); 
        mDone[slotIndex].insert(holding);

        return result;
    }
}

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
            copy(mFetching[slot].begin(), mFetching[slot].end(), back_inserter(allRecords));
            copy(mReady[slot].begin(), mReady[slot].end(), back_inserter(allRecords));

            auto count = count_if(allRecords.begin(), allRecords.end(), [&](FetchingRecordPtr fRecord)
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


void PendingEnvelopes::dumpInfo(Json::Value & ret)
{
    int count = 0;
    for (auto& entry : mFetching)
    {
        for (auto& fRecord : entry.second)
        {
            auto & envelope = fRecord->env;
            ostringstream output;
            output << "i:" << entry.first
                << " n:" << binToHex(envelope->nodeID).substr(0, 6);

            ret["pending"][count++] = output.str();
        }
    }
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

bool 
PendingEnvelopes::FetchingRecord::isReady()
{
    return (mTxSetTracker && mTxSetTracker->isItemFound()) &&
        (mQuorumSetTracker && mQuorumSetTracker->isItemFound());
}

void
PendingEnvelopes::checkReady(FetchingRecordPtr fRecord)
{
    if (fRecord->isReady())
    {
        auto iSlot = fRecord->env->statement.slotIndex;
        auto & set = mFetching[iSlot];
        set.erase(fRecord);
        mReady[iSlot].push_back(fRecord);

        mHerder.processSCPQueue();
    }
}

PendingEnvelopes::FetchingRecordPtr
PendingEnvelopes::fetch(SCPEnvelope const & env)
{
    FetchingRecordPtr fRecord = make_shared<FetchingRecord>();
    fRecord->env = make_shared<SCPEnvelope>(env);
    fRecord->mQuorumSetTracker = mApp.getOverlayManager().getQuorumSetFetcher()
        .fetch(env.statement.quorumSetHash, bind(&PendingEnvelopes::checkReady, this, fRecord));

    StellarBallot b;
    xdr::xdr_from_opaque(env.statement.ballot.value, b);

    fRecord->mTxSetTracker = mApp.getOverlayManager().getTxSetFetcher()
        .fetch(b.value.txSetHash, bind(&PendingEnvelopes::checkReady, this, fRecord));
    
    mFetching[fRecord->env->statement.slotIndex].insert(fRecord);

    // check if all items are already available from the cache.
    checkReady(fRecord);
    return fRecord;
}

}
