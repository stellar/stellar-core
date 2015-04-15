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
    PendingEnvelopes(Application& app, HerderImpl &herder);

    void add(SCPEnvelope const & envelope);

    void erase(uint64 slotIndex);

    std::vector<uint64> slots();

    void eraseBelow(uint64 slotIndex);
    
    optional<SCPEnvelope> pop(uint64 slotIndex);



    bool isFutureCommitted(uint64 slotIndex);

    void dumpInfo(Json::Value& ret);

private:
    bool checkFutureCommitted(SCPEnvelope envelope);

    Application &mApp;

    std::map<uint64, std::deque<SCPEnvelope>> mEnvelopes;
    std::set<uint64> mIsFutureCommitted;

    HerderImpl &mHerder;


    medida::Counter& mPendingEnvelopesSize;


};

}