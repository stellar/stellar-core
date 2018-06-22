#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "xdr/Stellar-SCP.h"

#include <map>
#include <set>
#include <vector>

namespace Json
{
class Value;
}

namespace medida
{
class Counter;
}

namespace stellar
{

class Application;

/**
 * Container for envelopes that are ready to be processed.
 */
class ReadyEnvelopes
{
  public:
    explicit ReadyEnvelopes(Application& app);
    ~ReadyEnvelopes();

    /**
     * Checks if envelope has been pushed to this object before.
     */
    bool seen(SCPEnvelope const& envelope);

    /**
     * Adds new envelope to this object only if it was not added before. Return
     * value indicates if item was added to object.
     */
    bool push(SCPEnvelope const& envelope);

    /**
     * Removes ready envelope with smallest possible slot index that is less or
     * equal to slotIndex from this object and returns it as ret. If such
     * envelope was not found, false is returned and value of ret is unchanged.
     */
    bool pop(uint64_t slotIndex, SCPEnvelope& ret);

    /**
     * Return list of slots for which ready envelopes are available.
     */
    std::vector<uint64_t> readySlots();

    /**
     * Remove all envelopes with slot index smaller than given minimum.
     */
    void clearBelow(uint64_t slotIndex);

    Json::Value getJsonInfo(size_t limit);

  private:
    Application& mApp;
    medida::Counter& mReadyEnvelopesSize;
    medida::Counter& mSeenEnvelopesSize;

    struct SlotReadyEnvelopes
    {
        std::vector<SCPEnvelope> mReadyEnvelopes;
        std::set<SCPEnvelope> mSeenEnvelopes;
    };

    std::map<uint64_t, SlotReadyEnvelopes> mEnvelopes;
};
}
