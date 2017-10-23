#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "crypto/SecretKey.h"
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
    SCP(SCPDriver& driver, SecretKey const& secretKey, bool isValidator,
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

    enum TriBool
    {
        TB_TRUE,
        TB_FALSE,
        TB_MAYBE
    };

    // this is the main entry point of the SCP library
    // it processes the envelope, updates the internal state and
    // invokes the appropriate methods
    EnvelopeState receiveEnvelope(SCPEnvelope const& envelope);

    // Submit a value to consider for slotIndex
    // previousValue is the value from slotIndex-1
    bool nominate(uint64 slotIndex, Value const& value,
                  Value const& previousValue);

    // stops nomination for a slot
    void stopNomination(uint64 slotIndex);

    // Local QuorumSet interface (can be dynamically updated)
    void updateLocalQuorumSet(SCPQuorumSet const& qSet);
    SCPQuorumSet const& getLocalQuorumSet();

    // Local nodeID getter
    NodeID const& getLocalNodeID();

    // returns the local node descriptor
    std::shared_ptr<LocalNode> getLocalNode();

    void dumpInfo(Json::Value& ret, size_t limit);

    // summary: only return object counts
    // index = 0 for returning information for all slots
    void dumpQuorumInfo(Json::Value& ret, NodeID const& id, bool summary,
                        uint64 index = 0);

    // Purges all data relative to all the slots whose slotIndex is smaller
    // than the specified `maxSlotIndex`.
    void purgeSlots(uint64 maxSlotIndex);

    // Retrieves the local secret key as specified at construction
    SecretKey const& getSecretKey();

    // Returns whether the local node is a validator.
    bool isValidator();

    // Helpers for monitoring and reporting the internal memory-usage of the SCP
    // protocol to system metric reporters.
    size_t getKnownSlotsCount() const;
    size_t getCumulativeStatemtCount() const;

    // returns the latest messages sent for the given slot
    std::vector<SCPEnvelope> getLatestMessagesSend(uint64 slotIndex);

    // forces the state to match the one in the envelope
    // this is used when rebuilding the state after a crash for example
    void setStateFromEnvelope(uint64 slotIndex, SCPEnvelope const& e);

    // check if we are holding some slots
    bool empty() const;
    // return lowest slot index value
    uint64 getLowSlotIndex() const;
    // return highest slot index value
    uint64 getHighSlotIndex() const;

    // returns all messages for the slot
    std::vector<SCPEnvelope> getCurrentState(uint64 slotIndex);

    // returns messages that contributed to externalizing the slot
    // (or empty if the slot didn't externalize)
    std::vector<SCPEnvelope> getExternalizingState(uint64 slotIndex);

    // returns if a node is in the (transitive) quorum originating at
    // the local node, scanning the known slots.
    // TB_TRUE iff n is in the quorum
    // TB_FALSE iff n is not in the quorum
    // TB_MAYBE iff the quorum cannot be computed
    TriBool isNodeInQuorum(NodeID const& node);

    // ** helper methods to stringify ballot for logging
    std::string getValueString(Value const& v) const;
    std::string ballotToStr(SCPBallot const& ballot) const;
    std::string ballotToStr(std::unique_ptr<SCPBallot> const& ballot) const;
    std::string envToStr(SCPEnvelope const& envelope) const;
    std::string envToStr(SCPStatement const& st) const;

  protected:
    std::shared_ptr<LocalNode> mLocalNode;
    std::map<uint64, std::shared_ptr<Slot>> mKnownSlots;

    // Slot getter
    std::shared_ptr<Slot> getSlot(uint64 slotIndex, bool create);

    friend class TestSCP;
};
}
