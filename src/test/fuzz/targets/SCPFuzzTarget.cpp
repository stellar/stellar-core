// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz/FuzzTargetRegistry.h"

#include "crypto/SecretKey.h"
#include "crypto/SHA.h"
#include "scp/SCP.h"
#include "scp/SCPDriver.h"
#include "test/Catch2.h"
#include "test/TxTests.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace stellar
{
namespace
{

constexpr size_t MAX_ACTIONS = 64;
constexpr size_t ACTION_ENCODED_SIZE = 8;
constexpr size_t NODE_COUNT = 3;
constexpr uint8_t SLOT_COUNT = 32;
constexpr uint8_t VALUE_COUNT = 16;
constexpr uint8_t ACTION_KIND_COUNT = 11;

enum class FuzzValueState : uint8_t
{
    AVAILABLE_VALID = 0,
    AVAILABLE_INVALID = 1,
    FETCHING = 2,
    MAYBE_VALID = 3,
    FETCH_FAILED = 4
};

enum class ScenarioActionKind : uint8_t
{
    INJECT_NOMINATE = 0,
    INJECT_PREPARE = 1,
    INJECT_CONFIRM = 2,
    INJECT_EXTERNALIZE = 3,
    RUN_TIMERS = 4,
    DELIVER_QUEUED = 5,
    DROP_QUEUED = 6,
    DRAIN_QUEUE = 7,
    SET_VALUE_STATE = 8,
    SET_LOCAL_QSET = 9,
    COOPERATIVE_CONVERGE = 10
};

struct ScenarioAction
{
    ScenarioActionKind kind;
    uint8_t nodeIndex;
    uint8_t slotIndex;
    uint8_t valueIndex;
    uint8_t ballotCounter;
    uint8_t shape;
    uint8_t qSetChoice;
    uint8_t timerCallbacks;
};

struct QueuedEnvelope
{
    size_t fromNode;
    SCPEnvelope envelope;
};

struct ScenarioSummary
{
    size_t queuedCount{};
    size_t deliveredCount{};
    size_t droppedCount{};
    Hash digest{};

    bool
    operator==(ScenarioSummary const& other) const
    {
        return queuedCount == other.queuedCount &&
               deliveredCount == other.deliveredCount &&
               droppedCount == other.droppedCount && digest == other.digest;
    }
};

size_t
targetNodeForAction(ScenarioAction const& action)
{
    return (action.shape >> 2) % NODE_COUNT;
}

uint8_t
valueIndex(Value const& value)
{
    if (value.empty())
    {
        return 0;
    }
    return value[0] % VALUE_COUNT;
}

std::vector<Value>
statementValues(SCPStatement const& st)
{
    std::vector<Value> values;
    switch (st.pledges.type())
    {
    case SCP_ST_NOMINATE:
    {
        auto const& nominate = st.pledges.nominate();
        values.insert(values.end(), nominate.votes.begin(),
                      nominate.votes.end());
        values.insert(values.end(), nominate.accepted.begin(),
                      nominate.accepted.end());
        break;
    }
    case SCP_ST_PREPARE:
    {
        auto const& prepare = st.pledges.prepare();
        values.push_back(prepare.ballot.value);
        if (prepare.prepared)
        {
            values.push_back(prepare.prepared->value);
        }
        if (prepare.preparedPrime)
        {
            values.push_back(prepare.preparedPrime->value);
        }
        break;
    }
    case SCP_ST_CONFIRM:
        values.push_back(st.pledges.confirm().ballot.value);
        break;
    case SCP_ST_EXTERNALIZE:
        values.push_back(st.pledges.externalize().commit.value);
        break;
    }
    return values;
}

Hash
statementQSetHash(SCPStatement const& st)
{
    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
        return st.pledges.prepare().quorumSetHash;
    case SCP_ST_CONFIRM:
        return st.pledges.confirm().quorumSetHash;
    case SCP_ST_EXTERNALIZE:
        return st.pledges.externalize().commitQuorumSetHash;
    case SCP_ST_NOMINATE:
        return st.pledges.nominate().quorumSetHash;
    }
    releaseAssert(false);
}

class ByteCursor
{
  public:
    ByteCursor(uint8_t const* data, size_t size) : mData(data), mSize(size)
    {
    }

    bool
    empty() const
    {
        return mPos >= mSize;
    }

    size_t
    remaining() const
    {
        return mSize - mPos;
    }

    uint8_t
    nextU8(uint8_t defaultValue = 0)
    {
        if (empty())
        {
            return defaultValue;
        }
        return mData[mPos++];
    }

  private:
    uint8_t const* mData;
    size_t mSize;
    size_t mPos{0};
};

class FuzzSCPDriver : public SCPDriver
{
  public:
    explicit FuzzSCPDriver(size_t nodeIndex, NodeID const& localNode,
                           SCPQuorumSet const& localQSet,
                           std::array<FuzzValueState, VALUE_COUNT>* values)
        : mNodeIndex(nodeIndex)
        , mSCP(*this, localNode, true, localQSet)
        , mValueStates(values)
    {
        mKnownQSets[sha256(xdr::xdr_to_opaque(localQSet))] =
            std::make_shared<SCPQuorumSet>(localQSet);
    }

    void
    signEnvelope(SCPEnvelope&) override
    {
    }

#ifdef CAP_0083
    Value
    makeEmptyTxSetValueFromValue(Value const& v) const override
    {
        return v;
    }
#endif

    SCPQuorumSetPtr
    getQSet(Hash const& qSetHash) override
    {
        auto it = mKnownQSets.find(qSetHash);
        if (it == mKnownQSets.end())
        {
            return nullptr;
        }
        return it->second;
    }

    void
    rememberQSet(SCPQuorumSet const& qSet)
    {
        mKnownQSets[sha256(xdr::xdr_to_opaque(qSet))] =
            std::make_shared<SCPQuorumSet>(qSet);
    }

    std::optional<std::chrono::milliseconds>
    getTxSetDownloadWaitTime(Value const& value) const override
    {
        if (valueState(value) == FuzzValueState::FETCHING)
        {
            return std::chrono::milliseconds(100);
        }
        return std::nullopt;
    }

    std::chrono::milliseconds
    getTxSetDownloadTimeout() const override
    {
        return std::chrono::milliseconds(200);
    }

    bool
    isEnvelopeReady(SCPEnvelope const& envelope) const override
    {
        for (auto const& value : statementValues(envelope.statement))
        {
            if (valueState(value) == FuzzValueState::FETCHING)
            {
                return false;
            }
        }
        return true;
    }

    void
    emitEnvelope(SCPEnvelope const& envelope) override
    {
        mEmitted.push_back(envelope);
    }

    ValidationLevel
    validateValue(uint64, Value const& value, bool) const override
    {
        switch (valueState(value))
        {
        case FuzzValueState::AVAILABLE_VALID:
            return kFullyValidatedValue;
        case FuzzValueState::AVAILABLE_INVALID:
        case FuzzValueState::FETCH_FAILED:
            return kInvalidValue;
        case FuzzValueState::FETCHING:
        case FuzzValueState::MAYBE_VALID:
            return kMaybeValidNotCurrentValue;
        }
        releaseAssert(false);
    }

    bool
    isEmptyTxSetValue(Value const&) const override
    {
        return false;
    }

    bool
    isParallelTxSetDownloadEnabled() const override
    {
        return false;
    }

    bool
    protocolAllowsEmptyTxSetValues() const override
    {
        return false;
    }

    Hash
    getHashOf(std::vector<xdr::opaque_vec<>> const& vals) const override
    {
        SHA256 hasher;
        for (auto const& v : vals)
        {
            hasher.add(v);
        }
        return hasher.finish();
    }

    ValueWrapperPtr
    combineCandidates(uint64, ValueWrapperPtrSet const& candidates) override
    {
        if (candidates.empty())
        {
            Value v;
            v.push_back(0);
            return wrapValue(v);
        }
        return *candidates.begin();
    }

    bool
    hasUpgrades(Value const&) override
    {
        return false;
    }

    ValueWrapperPtr
    stripAllUpgrades(Value const& v) override
    {
        return wrapValue(v);
    }

    uint32_t
    getUpgradeNominationTimeoutLimit() const override
    {
        return std::numeric_limits<uint32_t>::max();
    }

    void
    setupTimer(uint64 slotIndex, int timerID, std::chrono::milliseconds timeout,
               std::function<void()> cb) override
    {
        if (cb)
        {
            stopTimer(slotIndex, timerID);
            mTimers.push_back({slotIndex, timerID,
                               mLogicalTime +
                                   static_cast<uint64_t>(timeout.count()),
                               mNextTimerSequence++, false, std::move(cb)});
        }
    }

    void
    stopTimer(uint64 slotIndex, int timerID) override
    {
        for (auto& timer : mTimers)
        {
            if (timer.slotIndex == slotIndex && timer.timerID == timerID)
            {
                timer.canceled = true;
            }
        }
    }

    std::chrono::milliseconds
    computeTimeout(uint32 roundNumber, bool) override
    {
        return std::chrono::milliseconds(20 + roundNumber);
    }

    void
    advanceTime(uint64_t ticks)
    {
        mLogicalTime += ticks;
    }

    void
    runPendingTimers(size_t maxCallbacks, bool latestFirst = false)
    {
        size_t n = 0;
        while (n < maxCallbacks)
        {
            auto timerIt = mTimers.end();
            for (auto it = mTimers.begin(); it != mTimers.end(); ++it)
            {
                if (it->canceled || it->deadline > mLogicalTime)
                {
                    continue;
                }
                if (timerIt == mTimers.end())
                {
                    timerIt = it;
                }
                else if (latestFirst &&
                         std::tie(it->deadline, it->sequence) >
                             std::tie(timerIt->deadline, timerIt->sequence))
                {
                    timerIt = it;
                }
                else if (!latestFirst &&
                         std::tie(it->deadline, it->sequence) <
                             std::tie(timerIt->deadline, timerIt->sequence))
                {
                    timerIt = it;
                }
            }

            if (timerIt == mTimers.end())
            {
                break;
            }

            auto cb = std::move(timerIt->callback);
            timerIt->canceled = true;
            cb();
            ++n;
        }
    }

    void
    blockEnvelope(SCPEnvelope const& envelope)
    {
        mBlockedEnvelopes.push_back(envelope);
    }

    size_t
    blockedCount() const
    {
        return mBlockedEnvelopes.size();
    }

    size_t
    activeTimerCount() const
    {
        return static_cast<size_t>(
            std::count_if(mTimers.begin(), mTimers.end(), [](auto const& t) {
                return !t.canceled;
            }));
    }

    uint64_t
    highestSlot() const
    {
        return const_cast<SCP&>(mSCP).getHighestKnownSlotIndex();
    }

    std::vector<SCPEnvelope>
    takeReadyBlocked(size_t maxEnvelopes)
    {
        std::vector<SCPEnvelope> ready;
        for (auto it = mBlockedEnvelopes.begin();
             it != mBlockedEnvelopes.end() && ready.size() < maxEnvelopes;)
        {
            if (isEnvelopeReady(*it))
            {
                ready.push_back(*it);
                it = mBlockedEnvelopes.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return ready;
    }

    size_t
    nodeIndex() const
    {
        return mNodeIndex;
    }

    std::vector<SCPEnvelope> const&
    emitted() const
    {
        return mEmitted;
    }

    bool
    hasEmittedBallotForSlot(uint64_t slotIndex) const
    {
        return std::any_of(mEmitted.begin(), mEmitted.end(),
                           [&](SCPEnvelope const& envelope) {
                               return envelope.statement.slotIndex ==
                                          slotIndex &&
                                      envelope.statement.pledges.type() !=
                                          SCP_ST_NOMINATE;
                           });
    }

    std::map<uint64_t, Value> const&
    externalized() const
    {
        return mExternalized;
    }

    struct Summary
    {
        uint64_t highestSlot{};
        size_t emittedCount{};
        size_t externalizedCount{};
        size_t blockedCount{};
        size_t activeTimerCount{};
        Hash digest{};

        bool
        operator==(Summary const& other) const
        {
            return highestSlot == other.highestSlot &&
                   emittedCount == other.emittedCount &&
                   externalizedCount == other.externalizedCount &&
                   blockedCount == other.blockedCount &&
                   activeTimerCount == other.activeTimerCount &&
                   digest == other.digest;
        }
    };

    Summary
    summarize() const
    {
        SHA256 hasher;
        for (auto const& env : mEmitted)
        {
            hasher.add(xdr::xdr_to_opaque(env.statement));
        }
        for (auto const& env : mBlockedEnvelopes)
        {
            hasher.add(xdr::xdr_to_opaque(env.statement));
        }
        for (auto const& [slot, val] : mExternalized)
        {
            xdr::opaque_vec<> slotBytes;
            slotBytes.resize(sizeof(slot));
            std::memcpy(slotBytes.data(), &slot, sizeof(slot));
            hasher.add(slotBytes);
            hasher.add(val);
        }

        Summary s;
        s.highestSlot = highestSlot();
        s.emittedCount = mEmitted.size();
        s.externalizedCount = mExternalized.size();
        s.blockedCount = mBlockedEnvelopes.size();
        s.activeTimerCount = activeTimerCount();
        s.digest = hasher.finish();
        return s;
    }

    size_t mNodeIndex;
    SCP mSCP;

  private:
    FuzzValueState
    valueState(Value const& value) const
    {
        return (*mValueStates)[valueIndex(value)];
    }

    void
    valueExternalized(uint64 slotIndex, Value const& value) override
    {
        auto existing = mExternalized.find(slotIndex);
        if (existing != mExternalized.end() && existing->second != value)
        {
            throw std::runtime_error(
                "SCP fuzz node externalized conflicting values");
        }
        mExternalized[slotIndex] = value;
    }

    std::map<Hash, SCPQuorumSetPtr> mKnownQSets;
    std::vector<SCPEnvelope> mEmitted;
    std::map<uint64_t, Value> mExternalized;
    std::vector<SCPEnvelope> mBlockedEnvelopes;
    std::array<FuzzValueState, VALUE_COUNT>* mValueStates;

    struct PendingTimer
    {
        uint64_t slotIndex;
        int timerID;
        uint64_t deadline;
        uint64_t sequence;
        bool canceled;
        std::function<void()> callback;
    };

    uint64_t mLogicalTime{0};
    uint64_t mNextTimerSequence{0};
    std::vector<PendingTimer> mTimers;
};

Hash
getQSetHash(SCPQuorumSet const& qSet)
{
    return sha256(xdr::xdr_to_opaque(qSet));
}

Value
makeValue(uint8_t valueIndex, uint8_t variant)
{
    Value v;
    v.push_back(valueIndex % VALUE_COUNT);
    v.push_back(variant);
    return v;
}

SCPBallot
makeBallot(uint8_t counter, Value const& value)
{
    SCPBallot ballot;
    ballot.counter = 1 + (counter % 8);
    ballot.value = value;
    return ballot;
}

Hash
qSetHashForChoice(std::vector<Hash> const& qSetHashes, uint8_t qSetChoice)
{
    if ((qSetChoice % 5) == 0)
    {
        return sha256("scp-fuzz-unknown-qset-" +
                      std::to_string(qSetChoice));
    }
    return qSetHashes[qSetChoice % qSetHashes.size()];
}

std::vector<ScenarioAction>
decodeScenario(uint8_t const* data, size_t size)
{
    ByteCursor cursor(data, size);
    std::vector<ScenarioAction> actions;
    while (cursor.remaining() >= ACTION_ENCODED_SIZE &&
           actions.size() < MAX_ACTIONS)
    {
        ScenarioAction action;
        action.kind = static_cast<ScenarioActionKind>(cursor.nextU8() %
                                                       ACTION_KIND_COUNT);
        action.nodeIndex = cursor.nextU8() % NODE_COUNT;
        action.slotIndex = cursor.nextU8() % SLOT_COUNT;
        action.valueIndex = cursor.nextU8() % VALUE_COUNT;
        action.ballotCounter = cursor.nextU8();
        action.shape = cursor.nextU8();
        action.qSetChoice = cursor.nextU8();
        action.timerCallbacks = cursor.nextU8();
        actions.push_back(action);
    }
    return actions;
}

void
addUint64(SHA256& hasher, uint64_t value)
{
    xdr::opaque_vec<> bytes;
    bytes.resize(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    hasher.add(bytes);
}

void
appendAction(std::vector<uint8_t>& out, ScenarioActionKind kind,
             uint8_t nodeIndex, uint8_t slotIndex, uint8_t valueIndex,
             uint8_t ballotCounter, uint8_t shape, uint8_t qSetChoice,
             uint8_t timerCallbacks)
{
    out.push_back(static_cast<uint8_t>(kind));
    out.push_back(nodeIndex);
    out.push_back(slotIndex);
    out.push_back(valueIndex);
    out.push_back(ballotCounter);
    out.push_back(shape);
    out.push_back(qSetChoice);
    out.push_back(timerCallbacks);
}

SCPEnvelope
makeEnvelopeFromAction(ScenarioAction const& action,
                       std::vector<Hash> const& qSetHashes,
                       std::array<NodeID, NODE_COUNT> const& nodes)
{
    auto qSetHash = qSetHashForChoice(qSetHashes, action.qSetChoice);
    auto value = makeValue(action.valueIndex, action.shape);
    auto otherValue = makeValue(action.valueIndex + 1, action.shape ^ 0xff);
    auto ballot = makeBallot(action.ballotCounter, value);

    SCPEnvelope env;
    auto& st = env.statement;
    st.nodeID = nodes[action.nodeIndex % nodes.size()];
    st.slotIndex = static_cast<uint64_t>(1 + (action.slotIndex % SLOT_COUNT));

    switch (action.kind)
    {
    case ScenarioActionKind::INJECT_NOMINATE:
    {
        st.pledges.type(SCP_ST_NOMINATE);
        auto& nominate = st.pledges.nominate();
        nominate.quorumSetHash = qSetHash;
        nominate.votes.emplace_back(value);
        if ((action.shape & 0x01) != 0)
        {
            nominate.votes.emplace_back(otherValue);
            std::sort(nominate.votes.begin(), nominate.votes.end());
        }
        if ((action.shape & 0x02) != 0)
        {
            nominate.accepted.emplace_back(value);
        }
        break;
    }
    case ScenarioActionKind::INJECT_PREPARE:
    {
        st.pledges.type(SCP_ST_PREPARE);
        auto& prepare = st.pledges.prepare();
        prepare.quorumSetHash = qSetHash;
        prepare.ballot = ballot;
        if ((action.shape & 0x01) != 0)
        {
            prepare.prepared.activate() = ballot;
        }
        if ((action.shape & 0x02) != 0 && ballot.counter > 1)
        {
            prepare.preparedPrime.activate() =
                makeBallot(ballot.counter - 2, otherValue);
        }
        if ((action.shape & 0x04) != 0)
        {
            prepare.nC = 1;
            prepare.nH = ballot.counter;
        }
        else
        {
            prepare.nC = 0;
            prepare.nH = 0;
        }
        break;
    }
    case ScenarioActionKind::INJECT_CONFIRM:
    {
        st.pledges.type(SCP_ST_CONFIRM);
        auto& confirm = st.pledges.confirm();
        confirm.ballot = ballot;
        confirm.nPrepared = ballot.counter;
        confirm.nCommit = 1;
        confirm.nH = ballot.counter;
        confirm.quorumSetHash = qSetHash;
        break;
    }
    case ScenarioActionKind::INJECT_EXTERNALIZE:
    {
        st.pledges.type(SCP_ST_EXTERNALIZE);
        auto& externalize = st.pledges.externalize();
        externalize.commit = ballot;
        externalize.nH = ballot.counter;
        externalize.commitQuorumSetHash = qSetHash;
        break;
    }
    case ScenarioActionKind::RUN_TIMERS:
    case ScenarioActionKind::DELIVER_QUEUED:
    case ScenarioActionKind::DROP_QUEUED:
    case ScenarioActionKind::DRAIN_QUEUE:
    case ScenarioActionKind::SET_VALUE_STATE:
    case ScenarioActionKind::SET_LOCAL_QSET:
    case ScenarioActionKind::COOPERATIVE_CONVERGE:
        releaseAssert(false);
    }

    env.signature.clear();
    return env;
}

class FuzzSCPNetwork
{
  public:
    FuzzSCPNetwork()
    {
        auto const v0 = txtest::getAccount("scp-fuzz-v0");
        auto const v1 = txtest::getAccount("scp-fuzz-v1");
        auto const v2 = txtest::getAccount("scp-fuzz-v2");

        mNodes = {v0.getPublicKey(), v1.getPublicKey(), v2.getPublicKey()};

        buildQSetCatalog();

        for (size_t i = 0; i < NODE_COUNT; ++i)
        {
            mDrivers.emplace_back(
                std::make_unique<FuzzSCPDriver>(i, mNodes[i], mQSets[0],
                                                &mValueStates));
            for (auto const& qSet : mQSets)
            {
                mDrivers.back()->rememberQSet(qSet);
            }
            mEmittedSeen.push_back(0);
        }
    }

    void
    run(std::vector<ScenarioAction> const& actions)
    {
        for (auto const& action : actions)
        {
            runAction(action);
            harvestEmitted();
            checkSafety();
        }
        advanceAllTime(1000);
        runAllTimers(32);
        harvestEmitted();
        drainQueue(64);
        checkSafety();
    }

    ScenarioSummary
    summarize() const
    {
        SHA256 hasher;
        for (auto const& driver : mDrivers)
        {
            auto nodeBytes = xdr::xdr_to_opaque(mNodes[driver->nodeIndex()]);
            hasher.add(nodeBytes);
            auto driverSummary = driver->summarize();
            addUint64(hasher, driverSummary.highestSlot);
            addUint64(hasher, driverSummary.emittedCount);
            addUint64(hasher, driverSummary.externalizedCount);
            addUint64(hasher, driverSummary.blockedCount);
            addUint64(hasher, driverSummary.activeTimerCount);
            hasher.add(xdr::xdr_to_opaque(driverSummary.digest));
        }
        for (auto const& queued : mQueue)
        {
            addUint64(hasher, queued.fromNode);
            hasher.add(xdr::xdr_to_opaque(queued.envelope.statement));
        }

        ScenarioSummary summary;
        summary.queuedCount = mQueue.size();
        summary.deliveredCount = mDeliveredCount;
        summary.droppedCount = mDroppedCount;
        summary.digest = hasher.finish();
        return summary;
    }

  private:
    void
    runAction(ScenarioAction const& action)
    {
        switch (action.kind)
        {
        case ScenarioActionKind::RUN_TIMERS:
            advanceAllTime(1 + action.timerCallbacks);
            runAllTimers(1 + (action.timerCallbacks % 16),
                         (action.shape & 0x80) != 0);
            break;
        case ScenarioActionKind::DELIVER_QUEUED:
            deliverQueued(action);
            break;
        case ScenarioActionKind::DROP_QUEUED:
            dropQueued(action);
            break;
        case ScenarioActionKind::DRAIN_QUEUE:
            drainQueue(1 + (action.timerCallbacks % 16));
            break;
        case ScenarioActionKind::SET_VALUE_STATE:
            setValueState(action);
            retryReadyBlocked(1 + (action.timerCallbacks % 16));
            break;
        case ScenarioActionKind::SET_LOCAL_QSET:
            setLocalQSet(action);
            break;
        case ScenarioActionKind::COOPERATIVE_CONVERGE:
            cooperativeConverge(action);
            break;
        default:
            injectEnvelope(action);
            break;
        }
    }

    void
    buildQSetCatalog()
    {
        SCPQuorumSet flatTwoOfThree;
        flatTwoOfThree.threshold = 2;
        flatTwoOfThree.validators = {mNodes[0], mNodes[1], mNodes[2]};

        SCPQuorumSet flatThreeOfThree;
        flatThreeOfThree.threshold = 3;
        flatThreeOfThree.validators = {mNodes[0], mNodes[1], mNodes[2]};

        SCPQuorumSet inner;
        inner.threshold = 1;
        inner.validators = {mNodes[1], mNodes[2]};

        SCPQuorumSet nested;
        nested.threshold = 2;
        nested.validators = {mNodes[0]};
        nested.innerSets = {inner};

        mQSets = {flatTwoOfThree, flatThreeOfThree, nested};
        for (auto const& qSet : mQSets)
        {
            mQSetHashes.push_back(getQSetHash(qSet));
        }
    }

    void
    injectEnvelope(ScenarioAction const& action)
    {
        auto env = makeEnvelopeFromAction(action, mQSetHashes, mNodes);
        auto target = targetNodeForAction(action);
        // receiveEnvelope models peer traffic. Avoid synthetic histories that
        // could not arrive from a peer in this local node's current slot state.
        if (env.statement.nodeID == mNodes[target])
        {
            return;
        }
        if (env.statement.pledges.type() != SCP_ST_NOMINATE &&
            !mDrivers[target]->hasEmittedBallotForSlot(
                env.statement.slotIndex))
        {
            return;
        }
        deliverTo(*mDrivers[target], env);
        mDrivers[target]->advanceTime(action.timerCallbacks);
        mDrivers[target]->runPendingTimers(action.timerCallbacks % 4,
                                          (action.shape & 0x80) != 0);
    }

    void
    setValueState(ScenarioAction const& action)
    {
        mValueStates[action.valueIndex % VALUE_COUNT] =
            static_cast<FuzzValueState>(action.shape % 5);
    }

    void
    setLocalQSet(ScenarioAction const& action)
    {
        auto target = targetNodeForAction(action);
        auto const& qSet = mQSets[action.qSetChoice % mQSets.size()];
        mDrivers[target]->mSCP.updateLocalQuorumSet(qSet);
    }

    void
    cooperativeConverge(ScenarioAction const& action)
    {
        auto slot = static_cast<uint64_t>(100 + action.slotIndex);
        auto value = makeValue(action.valueIndex, action.shape);
        auto previousValue = makeValue(0, 0);

        mValueStates[valueIndex(value)] = FuzzValueState::AVAILABLE_VALID;
        for (auto& driver : mDrivers)
        {
            driver->mSCP.updateLocalQuorumSet(mQSets[0]);
            driver->mSCP.nominate(slot, driver->wrapValue(value),
                                  previousValue);
        }
        harvestEmitted();

        for (size_t i = 0; i < 64; ++i)
        {
            drainQueue(64);
            advanceAllTime(100);
            runAllTimers(8);
            harvestEmitted();
            if (allExternalized(slot))
            {
                checkSafety();
                return;
            }
        }

        throw std::runtime_error(
            "SCP fuzz cooperative convergence did not externalize");
    }

    bool
    allExternalized(uint64_t slot) const
    {
        for (auto const& driver : mDrivers)
        {
            if (driver->externalized().find(slot) ==
                driver->externalized().end())
            {
                return false;
            }
        }
        return true;
    }

    void
    retryReadyBlocked(size_t maxRetries)
    {
        size_t retries = 0;
        for (auto& driver : mDrivers)
        {
            auto ready = driver->takeReadyBlocked(maxRetries - retries);
            for (auto const& env : ready)
            {
                deliverTo(*driver, env);
                ++retries;
                if (retries >= maxRetries)
                {
                    return;
                }
            }
        }
    }

    void
    harvestEmitted()
    {
        for (size_t i = 0; i < mDrivers.size(); ++i)
        {
            auto const& emitted = mDrivers[i]->emitted();
            while (mEmittedSeen[i] < emitted.size() &&
                   mQueue.size() < MAX_ACTIONS * NODE_COUNT)
            {
                auto const& env = emitted[mEmittedSeen[i]++];
                if (env.statement.nodeID != mNodes[i])
                {
                    throw std::runtime_error(
                        "SCP fuzz emitted envelope from wrong node");
                }
                if (std::find(mQSetHashes.begin(), mQSetHashes.end(),
                              statementQSetHash(env.statement)) ==
                    mQSetHashes.end())
                {
                    throw std::runtime_error(
                        "SCP fuzz emitted envelope with unknown qset");
                }
                mQueue.push_back({i, env});
            }
        }
    }

    void
    deliverQueued(ScenarioAction const& action)
    {
        if (mQueue.empty())
        {
            return;
        }
        auto index = action.valueIndex % mQueue.size();
        auto queued = mQueue[index];
        if ((action.qSetChoice & 0x02) == 0)
        {
            mQueue.erase(mQueue.begin() + static_cast<ptrdiff_t>(index));
        }
        if ((action.qSetChoice & 0x01) != 0)
        {
            for (auto& driver : mDrivers)
            {
                deliverTo(*driver, queued.envelope);
            }
        }
        else
        {
            deliverTo(*mDrivers[targetNodeForAction(action)], queued.envelope);
        }
    }

    void
    dropQueued(ScenarioAction const& action)
    {
        if (mQueue.empty())
        {
            return;
        }
        auto index = action.valueIndex % mQueue.size();
        mQueue.erase(mQueue.begin() + static_cast<ptrdiff_t>(index));
        ++mDroppedCount;
    }

    void
    drainQueue(size_t maxDeliveries)
    {
        size_t n = 0;
        while (!mQueue.empty() && n < maxDeliveries)
        {
            ScenarioAction action{};
            action.valueIndex = 0;
            action.qSetChoice = 1;
            deliverQueued(action);
            harvestEmitted();
            ++n;
        }
    }

    void
    deliverTo(FuzzSCPDriver& driver, SCPEnvelope const& envelope)
    {
        if (!driver.isEnvelopeReady(envelope))
        {
            driver.blockEnvelope(envelope);
            return;
        }
        auto wrapped = driver.wrapEnvelope(envelope);
        driver.mSCP.receiveEnvelope(wrapped);
        ++mDeliveredCount;
    }

    void
    advanceAllTime(uint64_t ticks)
    {
        for (auto& driver : mDrivers)
        {
            driver->advanceTime(ticks);
        }
    }

    void
    runAllTimers(size_t maxCallbacksPerNode, bool latestFirst = false)
    {
        for (auto& driver : mDrivers)
        {
            driver->runPendingTimers(maxCallbacksPerNode, latestFirst);
        }
    }

    void
    checkSafety() const
    {
        std::map<uint64_t, Value> externalized;
        for (size_t i = 0; i < mDrivers.size(); ++i)
        {
            auto const& driver = mDrivers[i];
            if (driver->highestSlot() < mHighestSeen[i])
            {
                throw std::runtime_error(
                    "SCP fuzz highest known slot regressed");
            }
            mHighestSeen[i] = driver->highestSlot();
            for (auto const& [slot, value] : driver->externalized())
            {
                auto [it, inserted] = externalized.emplace(slot, value);
                if (!inserted && it->second != value)
                {
                    throw std::runtime_error(
                        "SCP fuzz nodes externalized conflicting values");
                }
            }
        }
    }

    std::array<NodeID, NODE_COUNT> mNodes;
    std::vector<SCPQuorumSet> mQSets;
    std::vector<Hash> mQSetHashes;
    std::array<FuzzValueState, VALUE_COUNT> mValueStates{};
    mutable std::array<uint64_t, NODE_COUNT> mHighestSeen{};
    std::vector<std::unique_ptr<FuzzSCPDriver>> mDrivers;
    std::vector<size_t> mEmittedSeen;
    std::vector<QueuedEnvelope> mQueue;
    size_t mDeliveredCount{0};
    size_t mDroppedCount{0};
};

ScenarioSummary
runScenario(std::vector<ScenarioAction> const& actions)
{
    FuzzSCPNetwork network;
    network.run(actions);
    return network.summarize();
}

class SCPFuzzTarget : public FuzzTarget
{
  public:
    std::string
    name() const override
    {
        return "scp";
    }

    std::string
    description() const override
    {
        return "Fuzz SCP with deterministic multi-node simulation, value "
               "readiness, qset variation, safety, liveness, and replay "
               "checks";
    }

    void
    initialize() override
    {
        reinitializeAllGlobalStateForFuzzing(1);
    }

    FuzzResultCode
    run(uint8_t const* data, size_t size) override
    {
        if (data == nullptr || size == 0 || size > maxInputSize())
        {
            return FuzzResultCode::FUZZ_REJECTED;
        }

        auto actions = decodeScenario(data, size);
        if (actions.empty())
        {
            return FuzzResultCode::FUZZ_REJECTED;
        }

        auto s1 = runScenario(actions);
        auto s2 = runScenario(actions);
        if (!(s1 == s2))
        {
            throw std::runtime_error("SCP fuzz replay was non-deterministic");
        }

        return FuzzResultCode::FUZZ_SUCCESS;
    }

    void
    shutdown() override
    {
    }

    size_t
    maxInputSize() const override
    {
        return 32 * 1024;
    }

    std::vector<uint8_t>
    generateSeedInput() override
    {
        std::vector<uint8_t> out;
        appendAction(out, ScenarioActionKind::SET_VALUE_STATE, 0, 0, 2, 0,
                 static_cast<uint8_t>(FuzzValueState::FETCHING), 0, 0);
        appendAction(out, ScenarioActionKind::INJECT_NOMINATE, 1, 0, 2, 1, 3,
                 1, 0);
        appendAction(out, ScenarioActionKind::SET_VALUE_STATE, 0, 0, 2, 0,
                 static_cast<uint8_t>(FuzzValueState::AVAILABLE_VALID), 0,
                 4);
        appendAction(out, ScenarioActionKind::INJECT_PREPARE, 1, 1, 3, 3, 7,
                 0x80, 0);
        appendAction(out, ScenarioActionKind::INJECT_CONFIRM, 2, 1, 4, 4, 0,
                 0, 0);
        appendAction(out, ScenarioActionKind::RUN_TIMERS, 0, 0, 0, 0, 0, 1,
                 8);
        appendAction(out, ScenarioActionKind::COOPERATIVE_CONVERGE, 0, 2, 5,
                 1, 0, 0, 0);
        return out;
    }
};

REGISTER_FUZZ_TARGET(SCPFuzzTarget, "scp",
                     "Fuzz SCP with deterministic multi-node simulation, value "
                     "readiness, qset variation, safety, liveness, and replay "
                     "checks");

} // namespace
} // namespace stellar
