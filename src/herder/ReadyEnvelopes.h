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
 * Container for envelopes that are ready to be processed. Only allows envelopes
 * above certaing slot index that can be set by setMinimumSlotIndex.
 */
class ReadyEnvelopes
{
  public:
    explicit ReadyEnvelopes(Application& app);
    ~ReadyEnvelopes();

    /**
     * Checks if envelope has been pushed to this object before. Always returns
     * true for envelopes with slot index less than one set by
     * setMinimumSlotIndex.
     */
    bool seen(SCPEnvelope const& envelope);

    /**
     * Adds new envelope to this object only if it was not added before and its
     * slot index is equal or bigger than one set by setMinimumSlotIndex. Return
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
     * Sets minimum value of envelope slot index that is acceptable. All ready
     * envelopes with smaller slot indexes are removed and will no longer be
     * accepted.
     */
    void setMinimumSlotIndex(uint64_t slotIndex);

    void dumpInfo(Json::Value& ret, size_t limit);

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
    uint64_t mMinimumSlotIndex{0};
};
}
