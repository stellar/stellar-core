#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "fba/FBA.h"

namespace stellar 
{
class Node;

/**
 * The ReadySlot object is in charge of collecting ready proposals for a slot
 * (ready phase). It emits a proposal of its own if `readyValue` is called and
 * waits to hear from a transitive quorum to create a proper ballot with
 * evidence and trigger its preparation in FBA.
 */
class ReadySlot
{
  public:
    // Constructor
    ReadySlot(const uint64& slotIndex, FBA* FBA);

    // Process a newly received envelope for this ready slot and update its
    // state accordingly. `cb` asynchronously returns wether the envelope was
    // validated or not. Must exclusively receive envelopes whose payload type
    // is READY.
    void processEnvelope(const FBAEnvelope& envelope,
                         std::function<void(FBA::EnvelopeState)> const& cb);

    // Submit a value part for this node and a callback to be called when the
    // value is ready with its evidence.
    bool readyValue(const Value& valuePart,
                    std::function<void(Value, FBAReadyEvidence)> const& cb);

    // Extract the value parts from the evidece, validate them and check the
    // construction of the value.
    void validateReady(const Value& value,
                       const FBAReadyEvidence& evidence,
                       std::function<void(bool)> const& cb);

  private:
    // `advanceReadySlot` can be called as many time as needed. It checks if we
    // have heard from a transitive quorum and if so triggers the preparation
    // of a ballot.
    void advanceReadySlot();

    // Called whenever we made progress in the validation of a value readiness.
    // Can be called as many times as needed, supports reentrant calls.
    void advanceValidate(const Hash& evidenceHash);

    const uint64                                           mSlotIndex;
    FBA*                                                   mFBA;

    bool                                                   mIsReady;
    bool                                                   mIsWaiting;

    std::map<uint256, FBAEnvelope>                         mParts;
    std::function<void(Value,FBAReadyEvidence)>            mCallback;

    std::map<Hash, std::vector<std::function<void(bool)>>> mValidatePendings;
    std::map<Hash, std::map<Hash, bool>>                   mValidateResults;
    std::map<Hash, std::pair<Value,FBAReadyEvidence>>      mValidateInputs;

    std::map<Hash,bool>                                    mInAdvanceValidate;
    std::map<Hash,bool>                                    mRunAdvanceValidate;

    friend class Node;
};

}
