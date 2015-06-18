#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <map>
#include <set>
#include <memory>
#include <functional>
#include <chrono>

#include "crypto/SecretKey.h"
#include "generated/Stellar-SCP.h"
#include "lib/json/json-forwards.h"
#include "scp/SCPDriver.h"

namespace stellar
{
class Node;
class Slot;
class LocalNode;
typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;

class SCP
{
    SCPDriver& mDriver;

  public:
    SCP(SCPDriver& driver, SecretKey const& secretKey,
        SCPQuorumSet const& qSetLocal);

    SCPDriver&
    getDriver()
    {
        return mDriver;
    }
    SCPDriver const&
    getDriver() const
    {
        return mDriver;
    }

    enum EnvelopeState
    {
        INVALID, // the envelope is considered invalid
        VALID    // the envelope is valid
    };

    // this is the main entry point of the SCP library
    // it processes the envelope, updates the internal state and
    // invokes the appropriate methods
    EnvelopeState receiveEnvelope(SCPEnvelope const& envelope);

    // request to trigger a 'bumpState'
    // returns the value returned by 'bumpState'
    bool abandonBallot(uint64 slotIndex);

    // Submit a value to consider for slotIndex
    // previousValue is the value from slotIndex-1
    bool nominate(uint64 slotIndex, Value const& value,
                  Value const& previousValue);

    // Local QuorumSet interface (can be dynamically updated)
    void updateLocalQuorumSet(SCPQuorumSet const& qSet);
    SCPQuorumSet const& getLocalQuorumSet();

    // Local nodeID getter
    NodeID const& getLocalNodeID();

    // returns the local node descriptor
    std::shared_ptr<LocalNode> getLocalNode();

    // Envelope signature/verification
    void signEnvelope(SCPEnvelope& envelope);
    bool verifyEnvelope(SCPEnvelope const& envelope);

    void dumpInfo(Json::Value& ret);

    // Purges all data relative to all the slots whose slotIndex is smaller
    // than the specified `maxSlotIndex`.
    void purgeSlots(uint64 maxSlotIndex);

    // Retrieves the local secret key as specified at construction
    SecretKey const& getSecretKey();

    // Helpers for monitoring and reporting the internal memory-usage of the SCP
    // protocol to system metric reporters.
    size_t getKnownSlotsCount() const;
    size_t getCumulativeStatemtCount() const;

  protected:
    std::shared_ptr<LocalNode> mLocalNode;
    std::map<uint64, std::shared_ptr<Slot>> mKnownSlots;

    // Slot getter
    std::shared_ptr<Slot> getSlot(uint64 slotIndex);

    friend class TestSCP;
};
}
