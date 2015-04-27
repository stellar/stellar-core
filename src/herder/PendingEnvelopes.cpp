#include "PendingEnvelopes.h"
#include "main/Application.h"
#include "herder/HerderImpl.h"
#include "crypto/Hex.h"

namespace stellar
{

PendingEnvelopes::PendingEnvelopes(Application &app, HerderImpl &herder) :
    mApp(app)
    , mHerder(herder)
    , mPendingEnvelopesSize(
         app.getMetrics().NewCounter({ "scp", "memory", "pending-envelopes" }))
{}


void 
PendingEnvelopes::add(SCPEnvelope const &envelope)
{
    auto& list = mEnvelopes[envelope.statement.slotIndex];

    if (find(list.begin(), list.end(), envelope) == list.end())
    {
        mEnvelopes[envelope.statement.slotIndex].push_back(envelope);
        if (checkFutureCommitted(envelope))
        {
            mIsFutureCommitted.insert(envelope.statement.slotIndex);
        }

        mPendingEnvelopesSize.set_count(mEnvelopes.size());
    }

}

void
PendingEnvelopes::erase(uint64 slotIndex)
{
    mEnvelopes.erase(slotIndex);
    mIsFutureCommitted.erase(slotIndex);
    mPendingEnvelopesSize.set_count(mEnvelopes.size());
}

std::vector<uint64>
PendingEnvelopes::slots()
{
    std::vector<uint64> result;
    for(auto entry : mEnvelopes)
    {
        result.push_back(entry.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void
PendingEnvelopes::eraseBelow(uint64 slotIndex)
{
    bool changed = false;
    auto it = mEnvelopes.begin();
    while (it != mEnvelopes.end())
    {
        // also drop empty slots while we are here
        if (it->first < slotIndex || it->second.empty()) 
        {
            mIsFutureCommitted.erase(it->first);
            it = mEnvelopes.erase(it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }
    if (changed)
    {
        mPendingEnvelopesSize.set_count(mEnvelopes.size());
    }

}

optional<SCPEnvelope> PendingEnvelopes::pop(uint64 slotIndex)
{
    if (mEnvelopes[slotIndex].empty())
    {
        return nullptr;
    } else
    {
        auto result = mEnvelopes[slotIndex].front();
        mEnvelopes[slotIndex].pop_front();
        return make_optional<SCPEnvelope>(result);
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

    if (newEnvelope.statement.pledges.type() == COMMITTED)
         { // is this a committed statement
        SCPQuorumSet const& qset = mHerder.getLocalQuorumSet();

        if (find(qset.validators.begin(), qset.validators.end(),
            newEnvelope.nodeID) != qset.validators.end())
        { // is it from someone we care about?

            // TODO: we probably want to fetch the txset here to save time
            unsigned int count = 0;
            for (auto& env : mEnvelopes[newEnvelope.statement.slotIndex])
            {
                if (env.statement.ballot.value ==
                    newEnvelope.statement.ballot.value)
                    count++;

                if (count >= qset.threshold)
                { // do we have enough of these for the same ballot?
                    return true;
                }
            }
        }
    }
    return false;
}


void PendingEnvelopes::dumpInfo(Json::Value & ret)
{
    int count = 0;
    for (auto& entry : mEnvelopes)
    {
        for (auto& envelope : entry.second)
        {
            std::ostringstream output;
            output << "i:" << entry.first
                << " n:" << binToHex(envelope.nodeID).substr(0, 6);

            ret["pending"][count++] = output.str();
        }
    }
}



}