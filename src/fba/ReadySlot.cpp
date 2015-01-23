// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ReadySlot.h"

#include <cassert>
#include "util/types.h"
#include "xdrpp/marshal.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "util/Logging.h"
#include "fba/Node.h"
#include "fba/LocalNode.h"

namespace stellar
{
using xdr::operator==;

// Static helper to stringify envelope for logging
static std::string
envToStr(const FBAEnvelope& envelope)
{
    std::ostringstream oss;
    oss << "{ENV@" << binToHex(envelope.nodeID).substr(0,6) << "|";
    oss << "VALUE_PART";

    uint256 valueHash = 
      sha512_256(xdr::xdr_to_msg(envelope.payload.part().value));
    oss << "|" << binToHex(valueHash).substr(0,6);

    Hash qSetHash = envelope.payload.part().quorumSetHash;
    oss << "|" << binToHex(qSetHash).substr(0,6) << "}";

    return oss.str();
}

ReadySlot::ReadySlot(const uint64& slotIndex,
                     FBA* FBA)
    : mSlotIndex(slotIndex)
    , mFBA(FBA)
    , mIsReady(false)
    , mIsWaiting(false)
{
}

void
ReadySlot::processEnvelope(const FBAEnvelope& envelope,
                           std::function<void(FBA::EnvelopeState)> const& cb)
{
    assert(envelope.payload.type() == FBAEnvelopeType::VALUE_PART);
    assert(envelope.slotIndex == mSlotIndex);

    LOG(INFO) << "ReadySlot::processEnvelope" 
              << "@" << binToHex(mFBA->getLocalNodeID()).substr(0,6)
              << ":" << mSlotIndex
              << " " << envToStr(envelope);

    uint256 nodeID = envelope.nodeID;
    FBAValuePart p = envelope.payload.part();

    // We copy everything we need as this can be async (no reference).
    auto value_cb = [nodeID,envelope,cb,this] (bool valid)
    {
        // If the value is not valid, we just ignore it.
        if (!valid)
        {
            return cb(FBA::EnvelopeState::INVALID);
        }

        // Eeach node should submit only one ready proposal per slotIndex.
        if (mParts.find(nodeID) != mParts.end())
        {
            return cb(FBA::EnvelopeState::INVALID);
        }

        // Store the proposal and advance the preslot if possible.
        mParts[nodeID] = envelope;
        advanceReadySlot();

        // Finally call the callback saying that this was a valid envelope
        return cb(FBA::EnvelopeState::VALID);
    };

    mFBA->validateValue(mSlotIndex, nodeID, p.value, value_cb);
}

bool
ReadySlot::readyValue(const Value& valuePart,
                      std::function<void(Value, FBAReadyEvidence)> const& cb)
{
    if (mIsWaiting ||
        mParts.find(mFBA->getLocalNodeID()) != mParts.end())
    {
        return false;
    }

    mCallback = cb;
    mIsWaiting = true;

    FBAValuePart part;
    
    part.value = valuePart;
    part.quorumSetHash = mFBA->getLocalNode()->getQuorumSetHash();

    FBAEnvelope envelope;

    envelope.nodeID = mFBA->getLocalNodeID();
    envelope.slotIndex = mSlotIndex;
    envelope.payload.type(FBAEnvelopeType::VALUE_PART);
    envelope.payload.part() = part;
    mFBA->signEnvelope(envelope);

    auto emit = [envelope,this] (bool valid)
    {
        mFBA->emitEnvelope(envelope);
    };
    processEnvelope(envelope, emit);

    return true;
}

void 
ReadySlot::validateReady(const Value& value,
                         const FBAReadyEvidence& evidence,
                         std::function<void(bool)> const& cb)
{
    uint256 eHash = sha512_256(xdr::xdr_to_msg(evidence));

    // TODO(spolu) cache validation results

    // Signature validation is done in FBA. We check for structural sanity of
    // the evidence here.
    bool hasSource = false;
    for (auto e : evidence.parts)
    {
        if (e.nodeID == evidence.nodeID)
        {
            hasSource = true;
        }
        if (e.payload.type() != FBAEnvelopeType::VALUE_PART ||
            e.slotIndex != mSlotIndex)
        {
            return cb(false);
        }
    }
    if (!hasSource)
    {
        return cb(false);
    }
    
    if (mValidatePendings.find(eHash) != mValidatePendings.end())
    {
        mValidatePendings[eHash].push_back(cb);
        return;
    }
    else
    {
        mValidatePendings[eHash].push_back(cb);
    }

    mValidateInputs[eHash] = std::make_pair(value, evidence);

    for (auto e : evidence.parts)
    {
        uint256 pHash = sha512_256(xdr::xdr_to_msg(e.payload.part().value));
        
        auto validate = [this,pHash,eHash] (bool valid)
        {
            if (!valid)
            {
                mValidateResults[eHash][pHash] = false;
            }
            advanceValidate(eHash);
        };
        mFBA->validateValue(mSlotIndex, e.nodeID, 
                            e.payload.part().value, validate);
    }
}

void
ReadySlot::advanceValidate(const Hash& eHash)
{
    // `advanceValidate` suopports reentrant calls by setting and checking
    // `mInAdvanceValidate`. If a reentrant call is made, `mRunAdvanceValidate`
    // will be set and `advanceValidate` will be called again after it is done
    // executing.
    if(mInAdvanceValidate[eHash])
    {
        mRunAdvanceValidate[eHash] = true;
        return;
    }
    mInAdvanceValidate[eHash] = true;

    try
    {
        const Value& value = mValidateInputs[eHash].first;
        const FBAReadyEvidence& evidence = mValidateInputs[eHash].second;

        bool allCalled = true;
        bool valid = true;
        for (auto e : evidence.parts)
        {
            uint256 pHash = sha512_256(xdr::xdr_to_msg(e.payload.part().value));

            if (mValidateResults[eHash].find(pHash) ==
                mValidateResults[eHash].end())
            {
                allCalled = false;
                break;
            }
            else if(!mValidateResults[eHash][pHash])
            {
                valid = false;
            }
        }

        if (allCalled && valid)
        {
            // We rebuild a map of nodeID -> FBAEnvelope to later call
            // `isQuorumTransitive`
            std::map<uint256, FBAEnvelope> parts;
            for (auto e : evidence.parts)
            {
                parts[e.nodeID] = e;
            }

            const Hash& sourceQSetHash = 
                parts[evidence.nodeID].payload.part().quorumSetHash;
            const uint256& sourceNodeID = evidence.nodeID;
            
            if (mFBA->getNode(sourceNodeID)->isQuorumTransitive<FBAEnvelope>(
                    sourceQSetHash,
                    parts,
                    [] (const FBAEnvelope& e) 
                    { 
                        return e.payload.part().quorumSetHash; 
                    }))
            {
                Value x = parts[sourceNodeID].payload.part().value;
                for (auto e : evidence.parts)
                {
                    x = mFBA->mergeValueParts(x, e.payload.part().value);
                }
                valid = (x == value);
            }
            else
            {
                valid = false;
            }
        }

        if(allCalled)
        {
           for(auto cb : mValidatePendings[eHash])
           {
               cb(valid);
           }
           mValidatePendings.erase(eHash);
           mValidateResults.erase(eHash);
           mValidateInputs.erase(eHash);
           mInAdvanceValidate.erase(eHash);
           mRunAdvanceValidate.erase(eHash);
           
           // We're clean we return directly (skip reentrance)
           return;
        }
    }
    catch(Node::QuorumSetNotFound e)
    {
        auto cb = [this,e,eHash] (const FBAQuorumSet& qSet)
        {
            uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));
            if (e.qSetHash() == qSetHash)
            {
                mFBA->getNode(e.nodeID())->cacheQuorumSet(qSet);
                advanceValidate(eHash);
            }
        };
        mFBA->retrieveQuorumSet(e.nodeID(), e.qSetHash(), cb);
    }

    mInAdvanceValidate[eHash] = false;
    if (mRunAdvanceValidate[eHash])
    {
        mRunAdvanceValidate[eHash] = false;
        advanceValidate(eHash);
    }
}

void
ReadySlot::advanceReadySlot()
{
    // If we're already ready or no one is expecting a callback, just return.
    if (mIsReady || !mIsWaiting)
    {
        return;
    }

    // We don't ready the value if we haven't made a value part ourselves.
    if (mParts.find(mFBA->getLocalNodeID()) == mParts.end())
    {
        return;
    }

    try
    {
        if (mFBA->getLocalNode()->isQuorumTransitive<FBAEnvelope>(
                mParts[mFBA->getLocalNodeID()].payload.part().quorumSetHash,
                mParts,
                [] (const FBAEnvelope& e) 
                { 
                    return e.payload.part().quorumSetHash; 
                }))
        {
            FBAReadyEvidence e;
            e.nodeID = mFBA->getLocalNodeID();
            Value x = mParts[mFBA->getLocalNodeID()].payload.part().value;
            for (auto it : mParts)
            {
                x = mFBA->mergeValueParts(x, it.second.payload.part().value);
                e.parts.push_back(it.second);
            }

            mIsReady = true;
            mCallback(x, e);
        }
    }
    catch(Node::QuorumSetNotFound e)
    {
        auto cb = [this,e] (const FBAQuorumSet& qSet)
        {
            uint256 qSetHash = sha512_256(xdr::xdr_to_msg(qSet));
            if (e.qSetHash() == qSetHash)
            {
                mFBA->getNode(e.nodeID())->cacheQuorumSet(qSet);
                advanceReadySlot();
            }
        };
        mFBA->retrieveQuorumSet(e.nodeID(), e.qSetHash(), cb);
    }
}

}
