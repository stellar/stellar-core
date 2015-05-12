#pragma once
#include <autocheck/function.hpp>
#include <queue>
#include <map>
#include <medida/medida.h>
#include <util/optional.h>
#include <set>
#include <generated/SCPXDR.h>
#include "overlay/ItemFetcher.h"
#include "lib/json/json.h"

namespace stellar
{
    
class HerderImpl;

class PendingEnvelopes
{
public:
    using SCPEnvelopePtr = std::shared_ptr<SCPEnvelope>;

    struct FetchingRecord
    {
        SCPEnvelopePtr env;
        TxSetTrackerPtr mTxSetTracker;
        QuorumSetTrackerPtr mQuorumSetTracker;

        bool isReady();
    };
    using FetchingRecordPtr = std::shared_ptr<FetchingRecord>;

    PendingEnvelopes(Application& app, HerderImpl &herder);

    void add(SCPEnvelope const & envelope);

    void erase(uint64 slotIndex);

    std::vector<uint64> readySlots();

    void eraseBelow(uint64 slotIndex);
    
    optional<FetchingRecord> pop(uint64 slotIndex);
    

    bool isFutureCommitted(uint64 slotIndex);

    void dumpInfo(Json::Value& ret);

    TxSetFramePtr getTxSet(Hash txSetHash);
    SCPQuorumSetPtr getQuorumSet(Hash qSetHash);

private:
    uint64 mMinSlot = UINT64_MAX;
    void checkReady(FetchingRecordPtr fRecord);
    FetchingRecordPtr fetch(SCPEnvelope const & env);
    bool checkFutureCommitted(SCPEnvelope envelope);

    Application &mApp;
    HerderImpl &mHerder;

    std::map<uint64, std::set<FetchingRecordPtr>> mFetching;
    std::map<uint64, std::deque<FetchingRecordPtr>> mReady;
    
    // keep holding the txSet and quorumSet until they
    // are not neeeded anymore
    std::map<uint64, std::set<FetchingRecordPtr>> mDone; 

    std::set<uint64> mIsFutureCommitted;

    medida::Counter& mPendingEnvelopesSize;
};

}