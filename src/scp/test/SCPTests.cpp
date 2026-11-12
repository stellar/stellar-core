// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"

#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "ledger/NetworkConfig.h"
#include "scp/LocalNode.h"
#include "scp/SCP.h"
#include "scp/Slot.h"
#include "simulation/Simulation.h"
#include "test/Catch2.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"
#include <fmt/format.h>
#include <numeric>
#include <optional>
#include <queue>
#include <tuple>

// General convention in this file is that numbers in parenthesis
// refer to the rule number in the related protocol from the white paper
// For example (2) in the ballot protocol refers to:
// If phi = PREPARE and m lets v confirm new higher ballots prepared,
// then raise h to the highest such ballot and set z = h.x

namespace stellar
{

// Tx set download timeout value for tests.
constexpr std::chrono::milliseconds TX_SET_TIMEOUT{5000};

// UNDER and OVER are below and above TX_SET_TIMEOUT for tests that want to
// control whether a tx set download has timed out or not.
constexpr std::chrono::milliseconds UNDER_TX_SET_TIMEOUT{1000};
constexpr std::chrono::milliseconds OVER_TX_SET_TIMEOUT{6000};

class TestSCP : public SCPDriver
{
  public:
    SCP mSCP;
    uint32_t mInitialBallotTimeoutMS = 1000;
    uint32_t mIncrementBallotTimeoutMS = 1000;

    TestSCP(NodeID const& nodeID, SCPQuorumSet const& qSetLocal,
            bool isValidator = true)
        : mSCP(*this, nodeID, isValidator, qSetLocal)
    {
        // By default the local node leads every round: a ballot timeout then
        // bumps the counter with the value the node already votes for, like
        // the plain ballot protocol.
        mRoundLeader = [&](uint64, uint32) { return mSCP.getLocalNodeID(); };

        auto localQSet =
            std::make_shared<SCPQuorumSet>(mSCP.getLocalQuorumSet());
        storeQuorumSet(localQSet);
    }

    void
    signEnvelope(SCPEnvelope&) override
    {
    }

    void
    storeQuorumSet(SCPQuorumSetPtr qSet)
    {
        Hash qSetHash = sha256(xdr::xdr_to_opaque(*qSet.get()));
        mQuorumSets[qSetHash] = qSet;
    }

    SCPDriver::ValidationLevel
    validateValue(uint64 slotIndex, Value const& value,
                  bool proposal) const override
    {
        if (mValidateValueOverride)
        {
            return mValidateValueOverride(slotIndex, value, proposal);
        }
        // If we're tracking download wait time for this value, it's awaiting
        // download
        if (mDownloadWaitTimes.find(value) != mDownloadWaitTimes.end())
        {
            return SCPDriver::kStructurallyValidValue;
        }
        return SCPDriver::kFullyValidatedValue;
    }

    void
    ballotDidHearFromQuorum(uint64 slotIndex, SCPBallot const& ballot) override
    {
        mHeardFromQuorums[slotIndex].push_back(ballot);
    }

    void
    valueExternalized(uint64 slotIndex, Value const& value) override
    {
        if (mExternalizedValues.find(slotIndex) != mExternalizedValues.end())
        {
            throw std::out_of_range("Value already externalized");
        }
        mExternalizedValues[slotIndex] = value;
    }

    SCPQuorumSetPtr
    getQSet(Hash const& qSetHash) override
    {
        if (mQuorumSets.find(qSetHash) != mQuorumSets.end())
        {

            return mQuorumSets[qSetHash];
        }
        return SCPQuorumSetPtr();
    }

    std::optional<std::chrono::milliseconds>
    getTxSetDownloadWaitTime(Value const& v) const override
    {
        auto it = mDownloadWaitTimes.find(v);
        if (it != mDownloadWaitTimes.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    std::chrono::milliseconds
    getTxSetDownloadTimeout() const override
    {
        return TX_SET_TIMEOUT;
    }

    Value
    makeEmptyTxSetValueFromValue(Value const& value) const override
    {
        // Create an empty-tx-set value by prefixing with "EMPTY:"
        Value emptyTxSetValue;
        emptyTxSetValue.resize(6 + value.size());
        emptyTxSetValue[0] = 'E';
        emptyTxSetValue[1] = 'M';
        emptyTxSetValue[2] = 'P';
        emptyTxSetValue[3] = 'T';
        emptyTxSetValue[4] = 'Y';
        emptyTxSetValue[5] = ':';
        std::copy(value.begin(), value.end(), emptyTxSetValue.begin() + 6);
        return emptyTxSetValue;
    }

    bool
    isEmptyTxSetValue(Value const& v) const override
    {
        // Check if value starts with "EMPTY:"
        if (v.size() < 6)
        {
            return false;
        }
        return v[0] == 'E' && v[1] == 'M' && v[2] == 'P' && v[3] == 'T' &&
               v[4] == 'Y' && v[5] == ':';
    }

    bool
    isParallelTxSetDownloadEnabled() const override
    {
        return true;
    }

    bool
    protocolAllowsEmptyTxSetValues() const override
    {
        return true;
    }

    void
    emitEnvelope(SCPEnvelope const& envelope) override
    {
        mEnvs.push_back(envelope);
    }

    // used to test BallotProtocol directly, without a round leader
    bool
    bumpState(uint64 slotIndex, Value const& v)
    {
        return mSCP.getSlot(slotIndex, true)->bumpState(v, true);
    }

    // starts the slot the way the application does at its trigger; `value`
    // is what the local node proposes if it leads a round
    bool
    startSlot(uint64 slotIndex, Value const& value)
    {
        auto wv = wrapValue(value);
        return mSCP.startSlot(slotIndex, [this, wv]() {
            ++mValuesBuilt;
            return mMakeValueOverride ? mMakeValueOverride() : wv;
        });
    }

    std::optional<NodeID>
    getRoundLeader(uint64 slotIndex, uint32 round) override
    {
        return mRoundLeader(slotIndex, round);
    }

    void
    proposedValue(uint64 slotIndex, uint32 round, Value const& value) override
    {
        mProposedValues.emplace_back(round, value);
    }

    void
    enteredRound(uint64 slotIndex, uint32 round, bool timedOut) override
    {
        mEnteredRounds.emplace_back(round, timedOut);
    }

    ValueWrapperPtr
    reproposeValue(uint64 slotIndex, uint32 round, Value const& value) override
    {
        mReproposeCalls.emplace_back(round, value);
        return mReproposeValueOverride ? mReproposeValueOverride(round, value)
                                       : ValueWrapperPtr();
    }

    bool
    isProposalPending(uint64, NodeID const&) const override
    {
        return mProposalPending;
    }

    std::function<std::optional<NodeID>(uint64, uint32)> mRoundLeader;
    bool mProposalPending{false};
    std::function<ValueWrapperPtr()> mMakeValueOverride;
    int mValuesBuilt{0};
    std::vector<std::pair<uint32, Value>> mProposedValues;
    std::vector<std::pair<uint32, bool>> mEnteredRounds;
    // What the driver's reproposeValue was asked, and what it answers
    // (nothing by default: the value is proposed as it is).
    std::vector<std::pair<uint32, Value>> mReproposeCalls;
    std::function<ValueWrapperPtr(uint32, Value const&)>
        mReproposeValueOverride;

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

    std::function<SCPDriver::ValidationLevel(uint64, Value const&, bool)>
        mValidateValueOverride;

    std::map<Hash, SCPQuorumSetPtr> mQuorumSets;
    std::vector<SCPEnvelope> mEnvs;
    std::map<uint64, Value> mExternalizedValues;
    std::map<uint64, std::vector<SCPBallot>> mHeardFromQuorums;

    // Empty-tx-set value support
    std::map<Value, std::chrono::milliseconds> mDownloadWaitTimes;

    struct TimerData
    {
        std::chrono::milliseconds mAbsoluteTimeout;
        std::function<void()> mCallback;
    };
    std::map<int, TimerData> mTimers;
    std::chrono::milliseconds mCurrentTimerOffset{0};

    void
    setupTimer(uint64 slotIndex, int timerID, std::chrono::milliseconds timeout,
               std::function<void()> cb) override
    {
        mTimers[timerID] =
            TimerData{mCurrentTimerOffset +
                          (cb ? timeout : std::chrono::milliseconds::zero()),
                      cb};
    }

    void
    stopTimer(uint64 slotIndex, int timerID) override
    {
        mTimers.erase(timerID);
    }

    TimerData
    getBallotProtocolTimer()
    {
        return mTimers[Slot::BALLOT_PROTOCOL_TIMER];
    }

    // pretends the time moved forward
    std::chrono::milliseconds
    bumpTimerOffset()
    {
        // increase by more than the maximum timeout
        mCurrentTimerOffset += std::chrono::hours(5);
        return mCurrentTimerOffset;
    }

    TimerData
    getLeaderTimer()
    {
        return mTimers[Slot::LEADER_TIMER];
    }

    bool
    hasLeaderTimer()
    {
        return !!getLeaderTimer().mCallback;
    }

    // fires a timer the way a real (one-shot) timer does
    void
    fireTimer(int timerID)
    {
        auto cb = mTimers[timerID].mCallback;
        REQUIRE(cb);
        mTimers[timerID].mCallback = nullptr;
        cb();
    }

    uint32
    getLeaderRound(uint64 slotIndex)
    {
        return mSCP.getLeaderRound(slotIndex);
    }

    // returns true if a ballot protocol timer exists (in the past or future)
    bool
    hasBallotTimer()
    {
        return !!getBallotProtocolTimer().mCallback;
    }

    // returns true if the ballot protocol timer is scheduled in the future
    // false if scheduled in the past
    // this method is mostly used to verify that the timer *would* have fired
    bool
    hasBallotTimerUpcoming()
    {
        // timer must be scheduled in the past or future
        REQUIRE(hasBallotTimer());
        return mCurrentTimerOffset < getBallotProtocolTimer().mAbsoluteTimeout;
    }

    SCP::EnvelopeState
    receiveEnvelope(SCPEnvelope const& envelope)
    {
        auto envW = mSCP.getDriver().wrapEnvelope(envelope);
        return mSCP.receiveEnvelope(envW);
    }

    Slot&
    getSlot(uint64 index)
    {
        return *mSCP.getSlot(index, false);
    }

    std::vector<SCPEnvelope>
    getEntireState(uint64 index)
    {
        auto v = mSCP.getSlot(index, false)->getEntireCurrentState();
        return v;
    }

    SCPEnvelope
    getCurrentEnvelope(uint64 index, NodeID const& id)
    {
        auto r = getEntireState(index);
        auto it = std::find_if(r.begin(), r.end(), [&](SCPEnvelope const& e) {
            return e.statement.nodeID == id;
        });
        if (it != r.end())
        {
            return *it;
        }
        throw std::runtime_error("not found");
    }

    // Helper methods for empty-tx-set value testing
    void
    startDownload(Value const& v, std::chrono::milliseconds waitTime)
    {
        mDownloadWaitTimes[v] = waitTime;
    }

    void
    clearDownload(Value const& v)
    {
        mDownloadWaitTimes.erase(v);
    }

    // Copied from HerderSCPDriver.cpp
    static uint32_t const MAX_TIMEOUT_MS = (30 * 60) * 1000;

    std::chrono::milliseconds
    computeTimeout(uint32 roundNumber) override
    {
        int initialTimeoutMS = mInitialBallotTimeoutMS;
        int incrementMS = mIncrementBallotTimeoutMS;

        int timeoutMS = initialTimeoutMS + (roundNumber - 1) * incrementMS;
        if (timeoutMS > MAX_TIMEOUT_MS)
        {
            timeoutMS = MAX_TIMEOUT_MS;
        }
        return std::chrono::milliseconds(timeoutMS);
    }

    bool
    isEnvelopeReady(SCPEnvelope const& envelope) const override
    {
        // Not implemented. These tests do not use PendingEnvelopes, and so do
        // not require this method.
        releaseAssert(false);
    }
};

namespace
{
// x < y < z < zz
// k can be anything
Value xValue, yValue, zValue, zzValue, kValue;

void
setupValues()
{
    std::vector<Value> v;
    std::string d =
        fmt::format("SEED_VALUE_DATA_{}", getGlobalRandomEngine()());
    for (int i = 0; i < 4; i++)
    {
        auto h = sha256(fmt::format("{}/{}", d, i));
        v.emplace_back(xdr::xdr_to_opaque(h));
    }
    std::sort(v.begin(), v.end());
    xValue = v[0];
    yValue = v[1];
    zValue = v[2];
    zzValue = v[3];

    // kValue is independent
    auto kHash = sha256(d);
    kValue = xdr::xdr_to_opaque(kHash);
}

SCPEnvelope
makeEnvelope(SecretKey const& secretKey, uint64 slotIndex,
             SCPStatement const& statement)
{
    SCPEnvelope envelope;
    envelope.statement = statement;
    envelope.statement.nodeID = secretKey.getPublicKey();
    envelope.statement.slotIndex = slotIndex;

    envelope.signature = secretKey.sign(xdr::xdr_to_opaque(envelope.statement));

    return envelope;
}

SCPEnvelope
makeExternalize(SecretKey const& secretKey, Hash const& qSetHash,
                uint64 slotIndex, SCPBallot const& commitBallot, uint32 nH)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_EXTERNALIZE);
    auto& ext = st.pledges.externalize();
    ext.commit = commitBallot;
    ext.nH = nH;
    ext.commitQuorumSetHash = qSetHash;

    return makeEnvelope(secretKey, slotIndex, st);
}

SCPEnvelope
makeConfirm(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
            uint32 prepareCounter, SCPBallot const& b, uint32 nC, uint32 nH)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_CONFIRM);
    auto& con = st.pledges.confirm();
    con.ballot = b;
    con.nPrepared = prepareCounter;
    con.nCommit = nC;
    con.nH = nH;
    con.quorumSetHash = qSetHash;

    return makeEnvelope(secretKey, slotIndex, st);
}

SCPEnvelope
makePrepare(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
            SCPBallot const& ballot, SCPBallot* prepared = nullptr,
            uint32 nC = 0, uint32 nH = 0, SCPBallot* preparedPrime = nullptr)
{
    SCPStatement st;
    st.pledges.type(SCP_ST_PREPARE);
    auto& p = st.pledges.prepare();
    p.ballot = ballot;
    p.quorumSetHash = qSetHash;
    if (prepared)
    {
        p.prepared.activate() = *prepared;
    }

    p.nC = nC;
    p.nH = nH;

    if (preparedPrime)
    {
        p.preparedPrime.activate() = *preparedPrime;
    }

    return makeEnvelope(secretKey, slotIndex, st);
}

SCPEnvelope
makeNominate(SecretKey const& secretKey, Hash const& qSetHash, uint64 slotIndex,
             std::vector<Value> votes, std::vector<Value> accepted)
{
    std::sort(votes.begin(), votes.end());
    std::sort(accepted.begin(), accepted.end());

    SCPStatement st;
    st.pledges.type(SCP_ST_NOMINATE);
    auto& nom = st.pledges.nominate();
    nom.quorumSetHash = qSetHash;
    for (auto const& v : votes)
    {
        nom.votes.emplace_back(v);
    }
    for (auto const& a : accepted)
    {
        nom.accepted.emplace_back(a);
    }
    return makeEnvelope(secretKey, slotIndex, st);
}

void
verifyPrepare(SCPEnvelope const& actual, SecretKey const& secretKey,
              Hash const& qSetHash, uint64 slotIndex, SCPBallot const& ballot,
              SCPBallot* prepared = nullptr, uint32 nC = 0, uint32 nH = 0,
              SCPBallot* preparedPrime = nullptr)
{
    auto exp = makePrepare(secretKey, qSetHash, slotIndex, ballot, prepared, nC,
                           nH, preparedPrime);
    REQUIRE(exp.statement == actual.statement);
}

void
verifyConfirm(SCPEnvelope const& actual, SecretKey const& secretKey,
              Hash const& qSetHash, uint64 slotIndex, uint32 nPrepared,
              SCPBallot const& b, uint32 nC, uint32 nH)
{
    auto exp =
        makeConfirm(secretKey, qSetHash, slotIndex, nPrepared, b, nC, nH);
    REQUIRE(exp.statement == actual.statement);
}

void
verifyExternalize(SCPEnvelope const& actual, SecretKey const& secretKey,
                  Hash const& qSetHash, uint64 slotIndex,
                  SCPBallot const& commit, uint32 nH)
{
    auto exp = makeExternalize(secretKey, qSetHash, slotIndex, commit, nH);
    REQUIRE(exp.statement == actual.statement);
}

// Simulate xValue being only structurally valid (e.g., due to an invalid tx
// set)
SCPDriver::ValidationLevel
xValueStructurallyValidValidationOverride(uint64, Value const& v, bool)
{
    if (v == xValue)
    {
        return SCPDriver::kStructurallyValidValue;
    }
    return SCPDriver::kFullyValidatedValue;
}

// Returns kInvalidValue for xValue This simulates a value being invalid for
// some reason *other* than a bad tx set (e.g. bad close time or signature).
SCPDriver::ValidationLevel
xValueNonTxSetInvalidValidationOverride(uint64, Value const& v, bool)
{
    if (v == xValue)
    {
        return SCPDriver::kInvalidValue;
    }
    return SCPDriver::kFullyValidatedValue;
}

// Returns kMaybeValidNotCurrentValue for xValue, simulating that the value is
// NOT for the current ledger.
SCPDriver::ValidationLevel
xValueNotCurrentLedgerOverride(uint64, Value const& v, bool)
{
    return SCPDriver::kMaybeValidNotCurrentValue;
}
} // namespace

TEST_CASE("vblocking and quorum", "[scp]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);

    SCPQuorumSet qSet;
    qSet.threshold = 3;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);

    std::vector<NodeID> nodeSet;
    nodeSet.push_back(v0NodeID);

    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == false);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == false);

    nodeSet.push_back(v2NodeID);

    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == false);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);

    nodeSet.push_back(v3NodeID);
    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == true);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);

    nodeSet.push_back(v1NodeID);
    REQUIRE(LocalNode::isQuorumSlice(qSet, nodeSet) == true);
    REQUIRE(LocalNode::isVBlocking(qSet, nodeSet) == true);
}

TEST_CASE("v blocking distance", "[scp]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);
    SIMULATION_CREATE_NODE(5);
    SIMULATION_CREATE_NODE(6);
    SIMULATION_CREATE_NODE(7);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    auto check = [&](SCPQuorumSet const& qSetCheck, std::set<NodeID> const& s,
                     size_t expected) {
        auto r = LocalNode::findClosestVBlocking(qSetCheck, s, nullptr);
        REQUIRE(expected == r.size());
    };

    std::set<NodeID> good;
    good.insert(v0NodeID);

    // already v-blocking
    check(qSet, good, 0);

    good.insert(v1NodeID);
    // either v0 or v1
    check(qSet, good, 1);

    good.insert(v2NodeID);
    // any 2 of v0..v2
    check(qSet, good, 2);

    SCPQuorumSet qSubSet1;
    qSubSet1.threshold = 1;
    qSubSet1.validators.push_back(v3NodeID);
    qSubSet1.validators.push_back(v4NodeID);
    qSubSet1.validators.push_back(v5NodeID);
    qSet.innerSets.push_back(qSubSet1);

    good.insert(v3NodeID);
    // any 3 of v0..v3
    check(qSet, good, 3);

    good.insert(v4NodeID);
    // v0..v2
    check(qSet, good, 3);

    qSet.threshold = 1;
    // v0..v4
    check(qSet, good, 5);

    good.insert(v5NodeID);
    // v0..v5
    check(qSet, good, 6);

    SCPQuorumSet qSubSet2;
    qSubSet2.threshold = 2;
    qSubSet2.validators.push_back(v6NodeID);
    qSubSet2.validators.push_back(v7NodeID);

    qSet.innerSets.push_back(qSubSet2);
    // v0..v5
    check(qSet, good, 6);

    good.insert(v6NodeID);
    // v0..v5
    check(qSet, good, 6);

    good.insert(v7NodeID);
    // v0..v5 and one of 6,7
    check(qSet, good, 7);

    qSet.threshold = 4;
    // v6, v7
    check(qSet, good, 2);

    qSet.threshold = 3;
    // v0..v2
    check(qSet, good, 3);

    qSet.threshold = 2;
    // v0..v2 and one of v6,v7
    check(qSet, good, 4);
}

typedef std::function<SCPEnvelope(SecretKey const& sk)> genEnvelope;

using namespace std::placeholders;

static genEnvelope
makePrepareGen(Hash const& qSetHash, SCPBallot const& ballot,
               SCPBallot* prepared = nullptr, uint32 nC = 0, uint32 nH = 0,
               SCPBallot* preparedPrime = nullptr)
{
    return std::bind(makePrepare, _1, std::cref(qSetHash), 0, std::cref(ballot),
                     prepared, nC, nH, preparedPrime);
}

static genEnvelope
makeConfirmGen(Hash const& qSetHash, uint32 prepareCounter, SCPBallot const& b,
               uint32 nC, uint32 nH)
{
    return std::bind(makeConfirm, _1, std::cref(qSetHash), 0, prepareCounter,
                     std::cref(b), nC, nH);
}

static genEnvelope
makeExternalizeGen(Hash const& qSetHash, SCPBallot const& commitBallot,
                   uint32 nH)
{
    return std::bind(makeExternalize, _1, std::cref(qSetHash), 0,
                     std::cref(commitBallot), nH);
}

// Testing matrix that covers interesting min/max values for each timeout
// parameter
static void
testTimeouts(TestSCP& scp, std::function<void(TestSCP&)> f)
{
    SECTION("minimum values")
    {
        scp.mInitialBallotTimeoutMS =
            MinimumSorobanNetworkConfig::BALLOT_TIMEOUT_INITIAL_MILLISECONDS;
        scp.mIncrementBallotTimeoutMS =
            MinimumSorobanNetworkConfig::BALLOT_TIMEOUT_INCREMENT_MILLISECONDS;
        f(scp);
    }

    SECTION("initial values")
    {
        scp.mInitialBallotTimeoutMS =
            InitialSorobanNetworkConfig::BALLOT_TIMEOUT_INITIAL_MILLISECONDS;
        scp.mIncrementBallotTimeoutMS =
            InitialSorobanNetworkConfig::BALLOT_TIMEOUT_INCREMENT_MILLISECONDS;
        f(scp);
    }

    SECTION("maximum values")
    {
        scp.mInitialBallotTimeoutMS =
            MaximumSorobanNetworkConfig::BALLOT_TIMEOUT_INITIAL_MILLISECONDS;
        scp.mIncrementBallotTimeoutMS =
            MaximumSorobanNetworkConfig::BALLOT_TIMEOUT_INCREMENT_MILLISECONDS;
        f(scp);
    }
}

TEST_CASE("ballot protocol core5", "[scp][ballotprotocol]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);

    // we need 5 nodes to avoid sharing various thresholds:
    // v-blocking set size: 2
    // threshold: 4 = 3 + self or 4 others
    SCPQuorumSet qSet;
    qSet.threshold = 4;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);
    qSet.validators.push_back(v4NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);

    auto test = [&](TestSCP& scp) {
        scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
        uint256 qSetHash0 = scp.mSCP.getLocalNode()->getQuorumSetHash();

        REQUIRE(xValue < yValue);
        REQUIRE(yValue < zValue);
        REQUIRE(zValue < zzValue);

        CLOG_INFO(SCP, "");
        CLOG_INFO(SCP, "BEGIN TEST");

        auto recvVBlockingChecks = [&](genEnvelope gen, bool withChecks) {
            SCPEnvelope e1 = gen(v1SecretKey);
            SCPEnvelope e2 = gen(v2SecretKey);

            scp.bumpTimerOffset();

            // nothing should happen with first message
            size_t i = scp.mEnvs.size();
            scp.receiveEnvelope(e1);
            if (withChecks)
            {
                REQUIRE(scp.mEnvs.size() == i);
            }
            i++;
            scp.receiveEnvelope(e2);
            if (withChecks)
            {
                REQUIRE(scp.mEnvs.size() == i);
            }
        };

        auto recvVBlocking = std::bind(recvVBlockingChecks, _1, true);

        auto recvQuorumChecksEx = [&](genEnvelope gen, bool withChecks,
                                      bool delayedQuorum, bool checkUpcoming) {
            SCPEnvelope e1 = gen(v1SecretKey);
            SCPEnvelope e2 = gen(v2SecretKey);
            SCPEnvelope e3 = gen(v3SecretKey);
            SCPEnvelope e4 = gen(v4SecretKey);

            scp.bumpTimerOffset();

            scp.receiveEnvelope(e1);
            scp.receiveEnvelope(e2);
            size_t i = scp.mEnvs.size() + 1;
            scp.receiveEnvelope(e3);
            if (withChecks && !delayedQuorum)
            {
                REQUIRE(scp.mEnvs.size() == i);
            }
            if (checkUpcoming && !delayedQuorum)
            {
                REQUIRE(scp.hasBallotTimerUpcoming());
            }
            // nothing happens with an extra vote (unless we're in
            // delayedQuorum)
            scp.receiveEnvelope(e4);
            if (withChecks && delayedQuorum)
            {
                REQUIRE(scp.mEnvs.size() == i);
            }
            if (checkUpcoming && delayedQuorum)
            {
                REQUIRE(scp.hasBallotTimerUpcoming());
            }
        };
        // doesn't check timers
        auto recvQuorumChecks =
            std::bind(recvQuorumChecksEx, _1, _2, _3, false);
        // checks enabled, no delayed quorum
        auto recvQuorumEx = std::bind(recvQuorumChecksEx, _1, true, false, _2);
        // checks enabled, no delayed quorum, no check timers
        auto recvQuorum = std::bind(recvQuorumEx, _1, false);

        auto nodesAllPledgeToCommit = [&]() {
            SCPBallot b(1, xValue);
            SCPEnvelope prepare1 = makePrepare(v1SecretKey, qSetHash, 0, b);
            SCPEnvelope prepare2 = makePrepare(v2SecretKey, qSetHash, 0, b);
            SCPEnvelope prepare3 = makePrepare(v3SecretKey, qSetHash, 0, b);
            SCPEnvelope prepare4 = makePrepare(v4SecretKey, qSetHash, 0, b);

            REQUIRE(scp.bumpState(0, xValue));
            REQUIRE(scp.mEnvs.size() == 1);

            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, b);

            scp.receiveEnvelope(prepare1);
            REQUIRE(scp.mEnvs.size() == 1);
            REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

            scp.receiveEnvelope(prepare2);
            REQUIRE(scp.mEnvs.size() == 1);
            REQUIRE(scp.mHeardFromQuorums[0].size() == 0);

            scp.receiveEnvelope(prepare3);
            REQUIRE(scp.mEnvs.size() == 2);
            REQUIRE(scp.mHeardFromQuorums[0].size() == 1);
            REQUIRE(scp.mHeardFromQuorums[0][0] == b);

            // We have a quorum including us

            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, b, &b);

            scp.receiveEnvelope(prepare4);
            REQUIRE(scp.mEnvs.size() == 2);

            SCPEnvelope prepared1 =
                makePrepare(v1SecretKey, qSetHash, 0, b, &b);
            SCPEnvelope prepared2 =
                makePrepare(v2SecretKey, qSetHash, 0, b, &b);
            SCPEnvelope prepared3 =
                makePrepare(v3SecretKey, qSetHash, 0, b, &b);
            SCPEnvelope prepared4 =
                makePrepare(v4SecretKey, qSetHash, 0, b, &b);

            scp.receiveEnvelope(prepared4);
            scp.receiveEnvelope(prepared3);
            REQUIRE(scp.mEnvs.size() == 2);

            scp.receiveEnvelope(prepared2);
            REQUIRE(scp.mEnvs.size() == 3);

            // confirms prepared
            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, b, &b,
                          b.counter, b.counter);

            // extra statement doesn't do anything
            scp.receiveEnvelope(prepared1);
            REQUIRE(scp.mEnvs.size() == 3);
        };

        SECTION("bumpState x")
        {
            REQUIRE(scp.bumpState(0, xValue));
            REQUIRE(scp.mEnvs.size() == 1);

            SCPBallot expectedBallot(1, xValue);

            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                          expectedBallot);
        }

        SECTION("start <1,x>")
        {
            // no timer is set
            REQUIRE(!scp.hasBallotTimer());

            Value const& aValue = xValue;
            Value const& bValue = zValue;
            Value const& midValue = yValue;
            Value const& bigValue = zzValue;

            SCPBallot A1(1, aValue);
            SCPBallot B1(1, bValue);
            SCPBallot Mid1(1, midValue);
            SCPBallot Big1(1, bigValue);

            SCPBallot A2 = A1;
            A2.counter++;

            SCPBallot A3 = A2;
            A3.counter++;

            SCPBallot A4 = A3;
            A4.counter++;

            SCPBallot A5 = A4;
            A5.counter++;

            SCPBallot AInf(UINT32_MAX, aValue), BInf(UINT32_MAX, bValue);

            SCPBallot B2 = B1;
            B2.counter++;

            SCPBallot B3 = B2;
            B3.counter++;

            SCPBallot Mid2 = Mid1;
            Mid2.counter++;

            SCPBallot Big2 = Big1;
            Big2.counter++;

            REQUIRE(scp.bumpState(0, aValue));
            REQUIRE(scp.mEnvs.size() == 1);
            REQUIRE(!scp.hasBallotTimer());

            SECTION("prepared A1")
            {
                recvQuorumEx(makePrepareGen(qSetHash, A1), true);

                REQUIRE(scp.mEnvs.size() == 2);
                verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &A1);

                SECTION("bump prepared A2")
                {
                    // bump to (2,a)

                    scp.bumpTimerOffset();
                    REQUIRE(scp.bumpState(0, aValue));
                    REQUIRE(scp.mEnvs.size() == 3);
                    verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A2,
                                  &A1);
                    REQUIRE(!scp.hasBallotTimer());

                    recvQuorumEx(makePrepareGen(qSetHash, A2), true);
                    REQUIRE(scp.mEnvs.size() == 4);
                    verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, A2,
                                  &A2);

                    SECTION("Confirm prepared A2")
                    {
                        recvQuorum(makePrepareGen(qSetHash, A2, &A2));
                        REQUIRE(scp.mEnvs.size() == 5);
                        verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0,
                                      A2, &A2, 2, 2);
                        REQUIRE(!scp.hasBallotTimerUpcoming());

                        SECTION("Accept commit")
                        {
                            SECTION("Quorum A2")
                            {
                                recvQuorum(
                                    makePrepareGen(qSetHash, A2, &A2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 6);
                                verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                              qSetHash0, 0, 2, A2, 2, 2);
                                REQUIRE(!scp.hasBallotTimerUpcoming());

                                SECTION("Quorum prepared A3")
                                {
                                    recvVBlocking(makePrepareGen(qSetHash, A3,
                                                                 &A2, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 2, A3, 2, 2);
                                    REQUIRE(!scp.hasBallotTimer());

                                    recvQuorumEx(
                                        makePrepareGen(qSetHash, A3, &A2, 2, 2),
                                        true);
                                    REQUIRE(scp.mEnvs.size() == 8);
                                    verifyConfirm(scp.mEnvs[7], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 2);

                                    SECTION("Accept more commit A3")
                                    {
                                        recvQuorum(makePrepareGen(qSetHash, A3,
                                                                  &A3, 2, 3));
                                        REQUIRE(scp.mEnvs.size() == 9);
                                        verifyConfirm(scp.mEnvs[8], v0SecretKey,
                                                      qSetHash0, 0, 3, A3, 2,
                                                      3);
                                        REQUIRE(!scp.hasBallotTimerUpcoming());

                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            0);

                                        SECTION("Quorum externalize A3")
                                        {
                                            recvQuorum(makeConfirmGen(
                                                qSetHash, 3, A3, 2, 3));
                                            REQUIRE(scp.mEnvs.size() == 10);
                                            verifyExternalize(
                                                scp.mEnvs[9], v0SecretKey,
                                                qSetHash0, 0, A2, 3);
                                            REQUIRE(!scp.hasBallotTimer());

                                            REQUIRE(scp.mExternalizedValues
                                                        .size() == 1);
                                            REQUIRE(
                                                scp.mExternalizedValues[0] ==
                                                aValue);
                                        }
                                    }
                                    SECTION("v-blocking accept more A3")
                                    {
                                        SECTION("Confirm A3")
                                        {
                                            recvVBlocking(makeConfirmGen(
                                                qSetHash, 3, A3, 2, 3));
                                            REQUIRE(scp.mEnvs.size() == 9);
                                            verifyConfirm(
                                                scp.mEnvs[8], v0SecretKey,
                                                qSetHash0, 0, 3, A3, 2, 3);
                                            REQUIRE(
                                                !scp.hasBallotTimerUpcoming());
                                        }
                                        SECTION("Externalize A3")
                                        {
                                            recvVBlocking(makeExternalizeGen(
                                                qSetHash, A2, 3));
                                            REQUIRE(scp.mEnvs.size() == 9);
                                            verifyConfirm(
                                                scp.mEnvs[8], v0SecretKey,
                                                qSetHash0, 0, UINT32_MAX, AInf,
                                                2, UINT32_MAX);
                                            REQUIRE(!scp.hasBallotTimer());
                                        }
                                        SECTION(
                                            "other nodes moved to c=A4 h=A5")
                                        {
                                            SECTION("Confirm A4..5")
                                            {
                                                recvVBlocking(makeConfirmGen(
                                                    qSetHash, 3, A5, 4, 5));
                                                REQUIRE(scp.mEnvs.size() == 9);
                                                verifyConfirm(
                                                    scp.mEnvs[8], v0SecretKey,
                                                    qSetHash0, 0, 3, A5, 4, 5);
                                                REQUIRE(!scp.hasBallotTimer());
                                            }
                                            SECTION("Externalize A4..5")
                                            {
                                                recvVBlocking(
                                                    makeExternalizeGen(qSetHash,
                                                                       A4, 5));
                                                REQUIRE(scp.mEnvs.size() == 9);
                                                verifyConfirm(
                                                    scp.mEnvs[8], v0SecretKey,
                                                    qSetHash0, 0, UINT32_MAX,
                                                    AInf, 4, UINT32_MAX);
                                                REQUIRE(!scp.hasBallotTimer());
                                            }
                                        }
                                    }
                                }
                                SECTION("v-blocking prepared A3")
                                {
                                    recvVBlocking(makePrepareGen(qSetHash, A3,
                                                                 &A3, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 2);
                                    REQUIRE(!scp.hasBallotTimer());
                                }
                                SECTION("v-blocking prepared A3+B3")
                                {
                                    recvVBlocking(makePrepareGen(
                                        qSetHash, A3, &B3, 2, 2, &A3));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 2);
                                    REQUIRE(!scp.hasBallotTimer());
                                }
                                SECTION("v-blocking confirm A3")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 3, A3, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 2);
                                    REQUIRE(!scp.hasBallotTimer());
                                }
                                SECTION(
                                    "Hang - does not switch to B in CONFIRM")
                                {
                                    SECTION("Network EXTERNALIZE")
                                    {
                                        // externalize messages have a counter
                                        // at infinite
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, B2, 3));
                                        REQUIRE(scp.mEnvs.size() == 7);
                                        verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                      qSetHash0, 0, 2, AInf, 2,
                                                      2);
                                        REQUIRE(!scp.hasBallotTimer());

                                        // stuck
                                        recvQuorumChecks(
                                            makeExternalizeGen(qSetHash, B2, 3),
                                            false, false);
                                        REQUIRE(scp.mEnvs.size() == 7);
                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            0);
                                        // timer scheduled as there is a quorum
                                        // with (2, *)
                                        REQUIRE(scp.hasBallotTimerUpcoming());
                                    }
                                    SECTION("Network CONFIRMS other ballot")
                                    {
                                        SECTION("at same counter")
                                        {
                                            // nothing should happen here, in
                                            // particular, node should not
                                            // attempt to switch 'p'
                                            recvQuorumChecks(
                                                makeConfirmGen(qSetHash, 3, B2,
                                                               2, 3),
                                                false, false);
                                            REQUIRE(scp.mEnvs.size() == 6);
                                            REQUIRE(scp.mExternalizedValues
                                                        .size() == 0);
                                            REQUIRE(
                                                !scp.hasBallotTimerUpcoming());
                                        }
                                        SECTION("at a different counter")
                                        {
                                            recvVBlocking(makeConfirmGen(
                                                qSetHash, 3, B3, 3, 3));
                                            REQUIRE(scp.mEnvs.size() == 7);
                                            verifyConfirm(
                                                scp.mEnvs[6], v0SecretKey,
                                                qSetHash0, 0, 2, A3, 2, 2);
                                            REQUIRE(!scp.hasBallotTimer());

                                            recvQuorumChecks(
                                                makeConfirmGen(qSetHash, 3, B3,
                                                               3, 3),
                                                false, false);
                                            REQUIRE(scp.mEnvs.size() == 7);
                                            REQUIRE(scp.mExternalizedValues
                                                        .size() == 0);
                                            // timer scheduled as there is a
                                            // quorum with (3, *)
                                            REQUIRE(
                                                scp.hasBallotTimerUpcoming());
                                        }
                                    }
                                }
                            }
                            SECTION("v-blocking")
                            {
                                SECTION("CONFIRM")
                                {
                                    SECTION("CONFIRM A2")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 2, A2, 2, 2));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, 2, A2, 2,
                                                      2);
                                        REQUIRE(!scp.hasBallotTimerUpcoming());
                                    }
                                    SECTION("CONFIRM A3..4")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 4, A4, 3, 4));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, 4, A4, 3,
                                                      4);
                                        REQUIRE(!scp.hasBallotTimer());
                                    }
                                    SECTION("CONFIRM B2")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 2, B2, 2, 2));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, 2, B2, 2,
                                                      2);
                                        REQUIRE(!scp.hasBallotTimerUpcoming());
                                    }
                                }
                                SECTION("EXTERNALIZE")
                                {
                                    SECTION("EXTERNALIZE A2")
                                    {
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, A2, 2));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, UINT32_MAX,
                                                      AInf, 2, UINT32_MAX);
                                        REQUIRE(!scp.hasBallotTimer());
                                    }
                                    SECTION("EXTERNALIZE B2")
                                    {
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, B2, 2));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, UINT32_MAX,
                                                      BInf, 2, UINT32_MAX);
                                        REQUIRE(!scp.hasBallotTimer());
                                    }
                                }
                            }
                        }
                        SECTION("get conflicting prepared B")
                        {
                            SECTION("same counter")
                            {
                                recvVBlocking(
                                    makePrepareGen(qSetHash, B2, &B2));
                                REQUIRE(scp.mEnvs.size() == 6);
                                verifyPrepare(scp.mEnvs[5], v0SecretKey,
                                              qSetHash0, 0, A2, &B2, 0, 2, &A2);
                                REQUIRE(!scp.hasBallotTimerUpcoming());

                                recvQuorum(
                                    makePrepareGen(qSetHash, B2, &B2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 2, B2, 2, 2);
                                REQUIRE(!scp.hasBallotTimerUpcoming());
                            }
                            SECTION("higher counter")
                            {
                                recvVBlocking(
                                    makePrepareGen(qSetHash, B3, &B2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 6);
                                verifyPrepare(scp.mEnvs[5], v0SecretKey,
                                              qSetHash0, 0, A3, &B2, 0, 2, &A2);
                                REQUIRE(!scp.hasBallotTimer());

                                recvQuorumChecksEx(
                                    makePrepareGen(qSetHash, B3, &B2, 2, 2),
                                    true, true, true);
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 3, B3, 2, 2);
                            }
                            SECTION("higher counter mixed")
                            {
                                recvVBlocking(makePrepareGen(qSetHash, A3, &B3,
                                                             0, 2, &A2));
                                REQUIRE(scp.mEnvs.size() == 6);
                                // h still A2
                                // v-blocking
                                //     prepared B3 -> p = B3, p'=A2 (1)
                                //     counter 3, b = A3 (9) (same value than h)
                                // c = 0 (1)
                                verifyPrepare(scp.mEnvs[5], v0SecretKey,
                                              qSetHash0, 0, A3, &B3, 0, 2, &A2);
                                recvQuorumEx(makePrepareGen(qSetHash, A3, &B3,
                                                            0, 2, &A2),
                                             true);
                                // p=B3, p'=A3 (1)
                                // computed_h = B3
                                // b = computed_h = B3 (8)
                                // h = computed_h = B3 (2)
                                // c = h = B3 (3)
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyPrepare(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, B3, &B3, 3, 3, &A3);
                            }
                        }
                    }
                    SECTION("Confirm prepared mixed")
                    {
                        // a few nodes prepared B2
                        recvVBlocking(
                            makePrepareGen(qSetHash, B2, &B2, 0, 0, &A2));
                        REQUIRE(scp.mEnvs.size() == 5);
                        verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0,
                                      A2, &B2, 0, 0, &A2);
                        REQUIRE(!scp.hasBallotTimerUpcoming());

                        SECTION("mixed A2")
                        {
                            // causes h=A2
                            // but c = 0, as p >!~ h
                            scp.bumpTimerOffset();
                            scp.receiveEnvelope(
                                makePrepare(v3SecretKey, qSetHash, 0, A2, &A2));

                            REQUIRE(scp.mEnvs.size() == 6);
                            verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0,
                                          0, A2, &B2, 0, 2, &A2);
                            REQUIRE(!scp.hasBallotTimerUpcoming());

                            scp.bumpTimerOffset();
                            scp.receiveEnvelope(
                                makePrepare(v4SecretKey, qSetHash, 0, A2, &A2));

                            REQUIRE(scp.mEnvs.size() == 6);
                            REQUIRE(!scp.hasBallotTimerUpcoming());
                        }
                        SECTION("mixed B2")
                        {
                            // causes h=B2, c=B2
                            scp.bumpTimerOffset();
                            scp.receiveEnvelope(
                                makePrepare(v3SecretKey, qSetHash, 0, B2, &B2));

                            REQUIRE(scp.mEnvs.size() == 6);
                            verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0,
                                          0, B2, &B2, 2, 2, &A2);
                            REQUIRE(!scp.hasBallotTimerUpcoming());

                            scp.bumpTimerOffset();
                            scp.receiveEnvelope(
                                makePrepare(v4SecretKey, qSetHash, 0, B2, &B2));

                            REQUIRE(scp.mEnvs.size() == 6);
                            REQUIRE(!scp.hasBallotTimerUpcoming());
                        }
                    }
                }
                SECTION("switch prepared B1 from A1")
                {
                    // (p,p') = (B1, A1) [ from (A1, null) ]
                    recvVBlocking(makePrepareGen(qSetHash, B1, &B1));
                    REQUIRE(scp.mEnvs.size() == 3);
                    verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A1,
                                  &B1, 0, 0, &A1);
                    REQUIRE(!scp.hasBallotTimerUpcoming());

                    // v-blocking with n=2 -> bump n
                    recvVBlocking(makePrepareGen(qSetHash, B2));
                    REQUIRE(scp.mEnvs.size() == 4);
                    verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, A2,
                                  &B1, 0, 0, &A1);

                    // move to (p,p') = (B2, A1) [update p from B1 -> B2]
                    recvVBlocking(makePrepareGen(qSetHash, B2, &B2));
                    REQUIRE(scp.mEnvs.size() == 5);
                    verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, A2,
                                  &B2, 0, 0, &A1);
                    REQUIRE(!scp.hasBallotTimer()); // no quorum (other nodes on
                                                    // (A,1))

                    SECTION("v-blocking switches to previous value of p")
                    {
                        // v-blocking with n=3 -> bump n
                        recvVBlocking(makePrepareGen(qSetHash, B3));
                        REQUIRE(scp.mEnvs.size() == 6);
                        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                                      A3, &B2, 0, 0, &A1);
                        REQUIRE(!scp.hasBallotTimer()); // no quorum (other
                                                        // nodes on (A,1))

                        // vBlocking set says "B1" is prepared - but we already
                        // have p=B2
                        recvVBlockingChecks(makePrepareGen(qSetHash, B3, &B1),
                                            false);
                        REQUIRE(scp.mEnvs.size() == 6);
                        REQUIRE(!scp.hasBallotTimer());
                    }
                    SECTION("switch p' to Mid2")
                    {
                        // (p,p') = (B2, Mid2)
                        recvVBlocking(
                            makePrepareGen(qSetHash, B2, &B2, 0, 0, &Mid2));
                        REQUIRE(scp.mEnvs.size() == 6);
                        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                                      A2, &B2, 0, 0, &Mid2);
                        REQUIRE(!scp.hasBallotTimer());
                    }
                    SECTION("switch again Big2")
                    {
                        // both p and p' get updated
                        // (p,p') = (Big2, B2)
                        recvVBlocking(
                            makePrepareGen(qSetHash, B2, &Big2, 0, 0, &B2));
                        REQUIRE(scp.mEnvs.size() == 6);
                        verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                                      A2, &Big2, 0, 0, &B2);
                        REQUIRE(!scp.hasBallotTimer());
                    }
                }
                SECTION("switch prepare B1")
                {
                    recvQuorumChecks(makePrepareGen(qSetHash, B1), true, true);
                    REQUIRE(scp.mEnvs.size() == 3);
                    verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A1,
                                  &B1, 0, 0, &A1);
                    REQUIRE(!scp.hasBallotTimerUpcoming());
                }
                SECTION("prepare higher counter (v-blocking)")
                {
                    recvVBlocking(makePrepareGen(qSetHash, B2));
                    REQUIRE(scp.mEnvs.size() == 3);
                    verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A2,
                                  &A1);
                    REQUIRE(!scp.hasBallotTimer());

                    // more timeout from vBlocking set
                    recvVBlocking(makePrepareGen(qSetHash, B3));
                    REQUIRE(scp.mEnvs.size() == 4);
                    verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, A3,
                                  &A1);
                    REQUIRE(!scp.hasBallotTimer());
                }
            }
            SECTION("prepared B (v-blocking)")
            {
                recvVBlocking(makePrepareGen(qSetHash, B1, &B1));
                REQUIRE(scp.mEnvs.size() == 2);
                verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &B1);
                REQUIRE(!scp.hasBallotTimer());
            }
            SECTION("prepare B (quorum)")
            {
                recvQuorumChecksEx(makePrepareGen(qSetHash, B1), true, true,
                                   true);
                REQUIRE(scp.mEnvs.size() == 2);
                verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &B1);
            }
            SECTION("confirm (v-blocking)")
            {
                SECTION("via CONFIRM")
                {
                    scp.bumpTimerOffset();
                    scp.receiveEnvelope(
                        makeConfirm(v1SecretKey, qSetHash, 0, 3, A3, 3, 3));
                    scp.receiveEnvelope(
                        makeConfirm(v2SecretKey, qSetHash, 0, 4, A4, 2, 4));
                    REQUIRE(scp.mEnvs.size() == 2);
                    verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, 3,
                                  A3, 3, 3);
                    REQUIRE(!scp.hasBallotTimer());
                }
                SECTION("via EXTERNALIZE")
                {
                    scp.receiveEnvelope(
                        makeExternalize(v1SecretKey, qSetHash, 0, A2, 4));
                    scp.receiveEnvelope(
                        makeExternalize(v2SecretKey, qSetHash, 0, A3, 5));
                    REQUIRE(scp.mEnvs.size() == 2);
                    verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash0, 0,
                                  UINT32_MAX, AInf, 3, UINT32_MAX);
                    REQUIRE(!scp.hasBallotTimer());
                }
            }
        }

        // this is the same test suite than "start <1,x>" with the exception
        // that some transitions are not possible as x < z - so instead we
        // verify that nothing happens
        SECTION("start <1,z>")
        {
            // no timer is set
            REQUIRE(!scp.hasBallotTimer());

            Value const& aValue = zValue;
            Value const& bValue = xValue;

            SCPBallot A1(1, aValue);
            SCPBallot B1(1, bValue);

            SCPBallot A2 = A1;
            A2.counter++;

            SCPBallot A3 = A2;
            A3.counter++;

            SCPBallot A4 = A3;
            A4.counter++;

            SCPBallot A5 = A4;
            A5.counter++;

            SCPBallot AInf(UINT32_MAX, aValue), BInf(UINT32_MAX, bValue);

            SCPBallot B2 = B1;
            B2.counter++;

            SCPBallot B3 = B2;
            B3.counter++;

            SCPBallot B4 = B3;
            B4.counter++;

            REQUIRE(scp.bumpState(0, aValue));
            REQUIRE(scp.mEnvs.size() == 1);
            REQUIRE(!scp.hasBallotTimer());

            SECTION("prepared A1")
            {
                recvQuorumEx(makePrepareGen(qSetHash, A1), true);

                REQUIRE(scp.mEnvs.size() == 2);
                verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &A1);

                SECTION("bump prepared A2")
                {
                    // bump to (2,a)

                    scp.bumpTimerOffset();
                    REQUIRE(scp.bumpState(0, aValue));
                    REQUIRE(scp.mEnvs.size() == 3);
                    verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A2,
                                  &A1);
                    REQUIRE(!scp.hasBallotTimer());

                    recvQuorumEx(makePrepareGen(qSetHash, A2), true);
                    REQUIRE(scp.mEnvs.size() == 4);
                    verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, A2,
                                  &A2);

                    SECTION("Confirm prepared A2")
                    {
                        recvQuorum(makePrepareGen(qSetHash, A2, &A2));
                        REQUIRE(scp.mEnvs.size() == 5);
                        verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0,
                                      A2, &A2, 2, 2);
                        REQUIRE(!scp.hasBallotTimerUpcoming());

                        SECTION("Accept commit")
                        {
                            SECTION("Quorum A2")
                            {
                                recvQuorum(
                                    makePrepareGen(qSetHash, A2, &A2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 6);
                                verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                              qSetHash0, 0, 2, A2, 2, 2);
                                REQUIRE(!scp.hasBallotTimerUpcoming());

                                SECTION("Quorum prepared A3")
                                {
                                    recvVBlocking(makePrepareGen(qSetHash, A3,
                                                                 &A2, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 2, A3, 2, 2);
                                    REQUIRE(!scp.hasBallotTimer());

                                    recvQuorumEx(
                                        makePrepareGen(qSetHash, A3, &A2, 2, 2),
                                        true);
                                    REQUIRE(scp.mEnvs.size() == 8);
                                    verifyConfirm(scp.mEnvs[7], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 2);

                                    SECTION("Accept more commit A3")
                                    {
                                        recvQuorum(makePrepareGen(qSetHash, A3,
                                                                  &A3, 2, 3));
                                        REQUIRE(scp.mEnvs.size() == 9);
                                        verifyConfirm(scp.mEnvs[8], v0SecretKey,
                                                      qSetHash0, 0, 3, A3, 2,
                                                      3);
                                        REQUIRE(!scp.hasBallotTimerUpcoming());

                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            0);

                                        SECTION("Quorum externalize A3")
                                        {
                                            recvQuorum(makeConfirmGen(
                                                qSetHash, 3, A3, 2, 3));
                                            REQUIRE(scp.mEnvs.size() == 10);
                                            verifyExternalize(
                                                scp.mEnvs[9], v0SecretKey,
                                                qSetHash0, 0, A2, 3);
                                            REQUIRE(!scp.hasBallotTimer());

                                            REQUIRE(scp.mExternalizedValues
                                                        .size() == 1);
                                            REQUIRE(
                                                scp.mExternalizedValues[0] ==
                                                aValue);
                                        }
                                    }
                                    SECTION("v-blocking accept more A3")
                                    {
                                        SECTION("Confirm A3")
                                        {
                                            recvVBlocking(makeConfirmGen(
                                                qSetHash, 3, A3, 2, 3));
                                            REQUIRE(scp.mEnvs.size() == 9);
                                            verifyConfirm(
                                                scp.mEnvs[8], v0SecretKey,
                                                qSetHash0, 0, 3, A3, 2, 3);
                                            REQUIRE(
                                                !scp.hasBallotTimerUpcoming());
                                        }
                                        SECTION("Externalize A3")
                                        {
                                            recvVBlocking(makeExternalizeGen(
                                                qSetHash, A2, 3));
                                            REQUIRE(scp.mEnvs.size() == 9);
                                            verifyConfirm(
                                                scp.mEnvs[8], v0SecretKey,
                                                qSetHash0, 0, UINT32_MAX, AInf,
                                                2, UINT32_MAX);
                                            REQUIRE(!scp.hasBallotTimer());
                                        }
                                        SECTION(
                                            "other nodes moved to c=A4 h=A5")
                                        {
                                            SECTION("Confirm A4..5")
                                            {
                                                recvVBlocking(makeConfirmGen(
                                                    qSetHash, 3, A5, 4, 5));
                                                REQUIRE(scp.mEnvs.size() == 9);
                                                verifyConfirm(
                                                    scp.mEnvs[8], v0SecretKey,
                                                    qSetHash0, 0, 3, A5, 4, 5);
                                                REQUIRE(!scp.hasBallotTimer());
                                            }
                                            SECTION("Externalize A4..5")
                                            {
                                                recvVBlocking(
                                                    makeExternalizeGen(qSetHash,
                                                                       A4, 5));
                                                REQUIRE(scp.mEnvs.size() == 9);
                                                verifyConfirm(
                                                    scp.mEnvs[8], v0SecretKey,
                                                    qSetHash0, 0, UINT32_MAX,
                                                    AInf, 4, UINT32_MAX);
                                                REQUIRE(!scp.hasBallotTimer());
                                            }
                                        }
                                    }
                                }
                                SECTION("v-blocking prepared A3")
                                {
                                    recvVBlocking(makePrepareGen(qSetHash, A3,
                                                                 &A3, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 2);
                                    REQUIRE(!scp.hasBallotTimer());
                                }
                                SECTION("v-blocking prepared A3+B3")
                                {
                                    recvVBlocking(makePrepareGen(
                                        qSetHash, A3, &A3, 2, 2, &B3));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 2);
                                    REQUIRE(!scp.hasBallotTimer());
                                }
                                SECTION("v-blocking confirm A3")
                                {
                                    recvVBlocking(
                                        makeConfirmGen(qSetHash, 3, A3, 2, 2));
                                    REQUIRE(scp.mEnvs.size() == 7);
                                    verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                  qSetHash0, 0, 3, A3, 2, 2);
                                    REQUIRE(!scp.hasBallotTimer());
                                }
                                SECTION(
                                    "Hang - does not switch to B in CONFIRM")
                                {
                                    SECTION("Network EXTERNALIZE")
                                    {
                                        // externalize messages have a counter
                                        // at infinite
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, B2, 3));
                                        REQUIRE(scp.mEnvs.size() == 7);
                                        verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                                      qSetHash0, 0, 2, AInf, 2,
                                                      2);
                                        REQUIRE(!scp.hasBallotTimer());

                                        // stuck
                                        recvQuorumChecks(
                                            makeExternalizeGen(qSetHash, B2, 3),
                                            false, false);
                                        REQUIRE(scp.mEnvs.size() == 7);
                                        REQUIRE(
                                            scp.mExternalizedValues.size() ==
                                            0);
                                        // timer scheduled as there is a quorum
                                        // with (inf, *)
                                        REQUIRE(scp.hasBallotTimerUpcoming());
                                    }
                                    SECTION("Network CONFIRMS other ballot")
                                    {
                                        SECTION("at same counter")
                                        {
                                            // nothing should happen here, in
                                            // particular, node should not
                                            // attempt to switch 'p'
                                            recvQuorumChecks(
                                                makeConfirmGen(qSetHash, 3, B2,
                                                               2, 3),
                                                false, false);
                                            REQUIRE(scp.mEnvs.size() == 6);
                                            REQUIRE(scp.mExternalizedValues
                                                        .size() == 0);
                                            REQUIRE(
                                                !scp.hasBallotTimerUpcoming());
                                        }
                                        SECTION("at a different counter")
                                        {
                                            recvVBlocking(makeConfirmGen(
                                                qSetHash, 3, B3, 3, 3));
                                            REQUIRE(scp.mEnvs.size() == 7);
                                            verifyConfirm(
                                                scp.mEnvs[6], v0SecretKey,
                                                qSetHash0, 0, 2, A3, 2, 2);
                                            REQUIRE(!scp.hasBallotTimer());

                                            recvQuorumChecks(
                                                makeConfirmGen(qSetHash, 3, B3,
                                                               3, 3),
                                                false, false);
                                            REQUIRE(scp.mEnvs.size() == 7);
                                            REQUIRE(scp.mExternalizedValues
                                                        .size() == 0);
                                            // timer scheduled as there is a
                                            // quorum with (3, *)
                                            REQUIRE(
                                                scp.hasBallotTimerUpcoming());
                                        }
                                    }
                                }
                            }
                            SECTION("v-blocking")
                            {
                                SECTION("CONFIRM")
                                {
                                    SECTION("CONFIRM A2")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 2, A2, 2, 2));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, 2, A2, 2,
                                                      2);
                                        REQUIRE(!scp.hasBallotTimerUpcoming());
                                    }
                                    SECTION("CONFIRM A3..4")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 4, A4, 3, 4));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, 4, A4, 3,
                                                      4);
                                        REQUIRE(!scp.hasBallotTimer());
                                    }
                                    SECTION("CONFIRM B2")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 2, B2, 2, 2));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, 2, B2, 2,
                                                      2);
                                        REQUIRE(!scp.hasBallotTimerUpcoming());
                                    }
                                }
                                SECTION("EXTERNALIZE")
                                {
                                    SECTION("EXTERNALIZE A2")
                                    {
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, A2, 2));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, UINT32_MAX,
                                                      AInf, 2, UINT32_MAX);
                                        REQUIRE(!scp.hasBallotTimer());
                                    }
                                    SECTION("EXTERNALIZE B2")
                                    {
                                        // can switch to B2 with externalize
                                        // (higher counter)
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, B2, 2));
                                        REQUIRE(scp.mEnvs.size() == 6);
                                        verifyConfirm(scp.mEnvs[5], v0SecretKey,
                                                      qSetHash0, 0, UINT32_MAX,
                                                      BInf, 2, UINT32_MAX);
                                        REQUIRE(!scp.hasBallotTimer());
                                    }
                                }
                            }
                        }
                        SECTION("get conflicting prepared B")
                        {
                            SECTION("same counter")
                            {
                                // messages are ignored as B2 < A2
                                recvQuorumChecks(
                                    makePrepareGen(qSetHash, B2, &B2), false,
                                    false);
                                REQUIRE(scp.mEnvs.size() == 5);
                                REQUIRE(!scp.hasBallotTimerUpcoming());
                            }
                            SECTION("higher counter")
                            {
                                recvVBlocking(
                                    makePrepareGen(qSetHash, B3, &B2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 6);
                                // A2 > B2 -> p = A2, p'=B2
                                verifyPrepare(scp.mEnvs[5], v0SecretKey,
                                              qSetHash0, 0, A3, &A2, 2, 2, &B2);
                                REQUIRE(!scp.hasBallotTimer());

                                // node is trying to commit A2=<2,y> but rest
                                // of its quorum is trying to commit B2
                                // we end up with a delayed quorum
                                recvQuorumChecksEx(
                                    makePrepareGen(qSetHash, B3, &B2, 2, 2),
                                    true, true, true);
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyConfirm(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, 3, B3, 2, 2);
                            }
                            SECTION("higher counter mixed")
                            {
                                recvVBlocking(makePrepareGen(qSetHash, A3, &B3,
                                                             0, 2, &A2));
                                REQUIRE(scp.mEnvs.size() == 6);
                                // h still A2
                                // v-blocking
                                //     prepared B3 -> p = B3, p'=A2 (1)
                                //     counter 3, b = A3 (9) (same value than h)
                                // c = 0 (1)
                                verifyPrepare(scp.mEnvs[5], v0SecretKey,
                                              qSetHash0, 0, A3, &B3, 0, 2, &A2);
                                recvQuorumEx(makePrepareGen(qSetHash, A3, &B3,
                                                            0, 2, &A2),
                                             true);
                                // p=A3, p'=B3 (1)
                                // computed_h = B3 (2) z = B - cannot update b
                                REQUIRE(scp.mEnvs.size() == 7);
                                verifyPrepare(scp.mEnvs[6], v0SecretKey,
                                              qSetHash0, 0, A3, &A3, 0, 2, &B3);
                                // timeout, bump to B4
                                REQUIRE(scp.hasBallotTimerUpcoming());
                                auto cb =
                                    scp.getBallotProtocolTimer().mCallback;
                                cb();
                                // computed_h = B3
                                // h = B3 (2)
                                // c = 0
                                REQUIRE(scp.mEnvs.size() == 8);
                                verifyPrepare(scp.mEnvs[7], v0SecretKey,
                                              qSetHash0, 0, B4, &A3, 0, 3, &B3);
                            }
                        }
                    }
                    SECTION("Confirm prepared mixed")
                    {
                        // a few nodes prepared B2
                        recvVBlocking(
                            makePrepareGen(qSetHash, A2, &A2, 0, 0, &B2));
                        REQUIRE(scp.mEnvs.size() == 5);
                        verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0,
                                      A2, &A2, 0, 0, &B2);
                        REQUIRE(!scp.hasBallotTimerUpcoming());

                        SECTION("mixed A2")
                        {
                            // causes h=A2, c=A2
                            scp.bumpTimerOffset();
                            scp.receiveEnvelope(
                                makePrepare(v3SecretKey, qSetHash, 0, A2, &A2));

                            REQUIRE(scp.mEnvs.size() == 6);
                            verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0,
                                          0, A2, &A2, 2, 2, &B2);
                            REQUIRE(!scp.hasBallotTimerUpcoming());

                            scp.bumpTimerOffset();
                            scp.receiveEnvelope(
                                makePrepare(v4SecretKey, qSetHash, 0, A2, &A2));

                            REQUIRE(scp.mEnvs.size() == 6);
                            REQUIRE(!scp.hasBallotTimerUpcoming());
                        }
                        SECTION("mixed B2")
                        {
                            // causes computed_h=B2 ~ not set as h ~!= b
                            // -> noop
                            scp.bumpTimerOffset();
                            scp.receiveEnvelope(
                                makePrepare(v3SecretKey, qSetHash, 0, A2, &B2));

                            REQUIRE(scp.mEnvs.size() == 5);
                            REQUIRE(!scp.hasBallotTimerUpcoming());

                            scp.bumpTimerOffset();
                            scp.receiveEnvelope(
                                makePrepare(v4SecretKey, qSetHash, 0, B2, &B2));

                            REQUIRE(scp.mEnvs.size() == 5);
                            REQUIRE(!scp.hasBallotTimerUpcoming());
                        }
                    }
                }
                SECTION("switch prepared B1 from A1")
                {
                    // can't switch to B1
                    recvQuorumChecks(makePrepareGen(qSetHash, B1, &B1), false,
                                     false);
                    REQUIRE(scp.mEnvs.size() == 2);
                    REQUIRE(!scp.hasBallotTimerUpcoming());
                }
                SECTION("switch prepare B1")
                {
                    // doesn't switch as B1 < A1
                    recvQuorumChecks(makePrepareGen(qSetHash, B1), false,
                                     false);
                    REQUIRE(scp.mEnvs.size() == 2);
                    REQUIRE(!scp.hasBallotTimerUpcoming());
                }
                SECTION("prepare higher counter (v-blocking)")
                {
                    recvVBlocking(makePrepareGen(qSetHash, B2));
                    REQUIRE(scp.mEnvs.size() == 3);
                    verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A2,
                                  &A1);
                    REQUIRE(!scp.hasBallotTimer());

                    // more timeout from vBlocking set
                    recvVBlocking(makePrepareGen(qSetHash, B3));
                    REQUIRE(scp.mEnvs.size() == 4);
                    verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, A3,
                                  &A1);
                    REQUIRE(!scp.hasBallotTimer());
                }
            }
            SECTION("prepared B (v-blocking)")
            {
                recvVBlocking(makePrepareGen(qSetHash, B1, &B1));
                REQUIRE(scp.mEnvs.size() == 2);
                verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &B1);
                REQUIRE(!scp.hasBallotTimer());
            }
            SECTION("prepare B (quorum)")
            {
                recvQuorumChecksEx(makePrepareGen(qSetHash, B1), true, true,
                                   true);
                REQUIRE(scp.mEnvs.size() == 2);
                verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &B1);
            }
            SECTION("confirm (v-blocking)")
            {
                SECTION("via CONFIRM")
                {
                    scp.bumpTimerOffset();
                    scp.receiveEnvelope(
                        makeConfirm(v1SecretKey, qSetHash, 0, 3, A3, 3, 3));
                    scp.receiveEnvelope(
                        makeConfirm(v2SecretKey, qSetHash, 0, 4, A4, 2, 4));
                    REQUIRE(scp.mEnvs.size() == 2);
                    verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, 3,
                                  A3, 3, 3);
                    REQUIRE(!scp.hasBallotTimer());
                }
                SECTION("via EXTERNALIZE")
                {
                    scp.receiveEnvelope(
                        makeExternalize(v1SecretKey, qSetHash, 0, A2, 4));
                    scp.receiveEnvelope(
                        makeExternalize(v2SecretKey, qSetHash, 0, A3, 5));
                    REQUIRE(scp.mEnvs.size() == 2);
                    verifyConfirm(scp.mEnvs[1], v0SecretKey, qSetHash0, 0,
                                  UINT32_MAX, AInf, 3, UINT32_MAX);
                    REQUIRE(!scp.hasBallotTimer());
                }
            }
        }

        // this is the same test suite than "start <1,x>" but only keeping
        // the transitions that are observable when starting from empty
        SECTION("start from pristine")
        {
            Value const& aValue = xValue;
            Value const& bValue = zValue;

            SCPBallot A1(1, aValue);
            SCPBallot B1(1, bValue);

            SCPBallot A2 = A1;
            A2.counter++;

            SCPBallot A3 = A2;
            A3.counter++;

            SCPBallot A4 = A3;
            A4.counter++;

            SCPBallot A5 = A4;
            A5.counter++;

            SCPBallot AInf(UINT32_MAX, aValue), BInf(UINT32_MAX, bValue);

            SCPBallot B2 = B1;
            B2.counter++;

            SCPBallot B3 = B2;
            B3.counter++;

            REQUIRE(scp.mEnvs.size() == 0);

            SECTION("prepared A1")
            {
                recvQuorumChecks(makePrepareGen(qSetHash, A1), false, false);
                REQUIRE(scp.mEnvs.size() == 0);

                SECTION("bump prepared A2")
                {
                    SECTION("Confirm prepared A2")
                    {
                        recvVBlockingChecks(makePrepareGen(qSetHash, A2, &A2),
                                            false);
                        REQUIRE(scp.mEnvs.size() == 0);

                        SECTION("Quorum A2")
                        {
                            recvVBlockingChecks(
                                makePrepareGen(qSetHash, A2, &A2), false);
                            REQUIRE(scp.mEnvs.size() == 0);
                            recvQuorum(makePrepareGen(qSetHash, A2, &A2));
                            REQUIRE(scp.mEnvs.size() == 1);
                            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0,
                                          0, A2, &A2, 1, 2);
                        }
                        SECTION("Quorum B2")
                        {
                            recvVBlockingChecks(
                                makePrepareGen(qSetHash, B2, &B2), false);
                            REQUIRE(scp.mEnvs.size() == 0);
                            recvQuorum(makePrepareGen(qSetHash, B2, &B2));
                            REQUIRE(scp.mEnvs.size() == 1);
                            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0,
                                          0, B2, &B2, 2, 2, &A2);
                        }
                        SECTION("Accept commit")
                        {
                            SECTION("Quorum A2")
                            {
                                recvQuorum(
                                    makePrepareGen(qSetHash, A2, &A2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 1);
                                verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                              qSetHash0, 0, 2, A2, 2, 2);
                            }
                            SECTION("Quorum B2")
                            {
                                recvQuorum(
                                    makePrepareGen(qSetHash, B2, &B2, 2, 2));
                                REQUIRE(scp.mEnvs.size() == 1);
                                verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                              qSetHash0, 0, 2, B2, 2, 2);
                            }
                            SECTION("v-blocking")
                            {
                                SECTION("CONFIRM")
                                {
                                    SECTION("CONFIRM A2")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 2, A2, 2, 2));
                                        REQUIRE(scp.mEnvs.size() == 1);
                                        verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                      qSetHash0, 0, 2, A2, 2,
                                                      2);
                                    }
                                    SECTION("CONFIRM A3..4")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 4, A4, 3, 4));
                                        REQUIRE(scp.mEnvs.size() == 1);
                                        verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                      qSetHash0, 0, 4, A4, 3,
                                                      4);
                                    }
                                    SECTION("CONFIRM B2")
                                    {
                                        recvVBlocking(makeConfirmGen(
                                            qSetHash, 2, B2, 2, 2));
                                        REQUIRE(scp.mEnvs.size() == 1);
                                        verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                      qSetHash0, 0, 2, B2, 2,
                                                      2);
                                    }
                                }
                                SECTION("EXTERNALIZE")
                                {
                                    SECTION("EXTERNALIZE A2")
                                    {
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, A2, 2));
                                        REQUIRE(scp.mEnvs.size() == 1);
                                        verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                      qSetHash0, 0, UINT32_MAX,
                                                      AInf, 2, UINT32_MAX);
                                    }
                                    SECTION("EXTERNALIZE B2")
                                    {
                                        recvVBlocking(makeExternalizeGen(
                                            qSetHash, B2, 2));
                                        REQUIRE(scp.mEnvs.size() == 1);
                                        verifyConfirm(scp.mEnvs[0], v0SecretKey,
                                                      qSetHash0, 0, UINT32_MAX,
                                                      BInf, 2, UINT32_MAX);
                                    }
                                }
                            }
                        }
                    }
                    SECTION("Confirm prepared mixed")
                    {
                        // a few nodes prepared A2
                        // causes p=A2
                        recvVBlockingChecks(makePrepareGen(qSetHash, A2, &A2),
                                            false);
                        REQUIRE(scp.mEnvs.size() == 0);

                        // a few nodes prepared B2
                        // causes p=B2, p'=A2
                        recvVBlockingChecks(
                            makePrepareGen(qSetHash, A2, &B2, 0, 0, &A2),
                            false);
                        REQUIRE(scp.mEnvs.size() == 0);

                        SECTION("mixed A2")
                        {
                            // causes h=A2
                            // but c = 0, as p >!~ h
                            scp.receiveEnvelope(
                                makePrepare(v3SecretKey, qSetHash, 0, A2, &A2));

                            REQUIRE(scp.mEnvs.size() == 1);
                            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0,
                                          0, A2, &B2, 0, 2, &A2);

                            scp.receiveEnvelope(
                                makePrepare(v4SecretKey, qSetHash, 0, A2, &A2));

                            REQUIRE(scp.mEnvs.size() == 1);
                        }
                        SECTION("mixed B2")
                        {
                            // causes h=B2, c=B2
                            scp.receiveEnvelope(
                                makePrepare(v3SecretKey, qSetHash, 0, B2, &B2));

                            REQUIRE(scp.mEnvs.size() == 1);
                            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0,
                                          0, B2, &B2, 2, 2, &A2);

                            scp.receiveEnvelope(
                                makePrepare(v4SecretKey, qSetHash, 0, B2, &B2));

                            REQUIRE(scp.mEnvs.size() == 1);
                        }
                    }
                }
                SECTION("switch prepared B1")
                {
                    recvVBlockingChecks(makePrepareGen(qSetHash, B1, &B1),
                                        false);
                    REQUIRE(scp.mEnvs.size() == 0);
                }
            }
            SECTION("prepared B (v-blocking)")
            {
                recvVBlockingChecks(makePrepareGen(qSetHash, B1, &B1), false);
                REQUIRE(scp.mEnvs.size() == 0);
            }
            SECTION("confirm (v-blocking)")
            {
                SECTION("via CONFIRM")
                {
                    scp.receiveEnvelope(
                        makeConfirm(v1SecretKey, qSetHash, 0, 3, A3, 3, 3));
                    scp.receiveEnvelope(
                        makeConfirm(v2SecretKey, qSetHash, 0, 4, A4, 2, 4));
                    REQUIRE(scp.mEnvs.size() == 1);
                    verifyConfirm(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, 3,
                                  A3, 3, 3);
                }
                SECTION("via EXTERNALIZE")
                {
                    scp.receiveEnvelope(
                        makeExternalize(v1SecretKey, qSetHash, 0, A2, 4));
                    scp.receiveEnvelope(
                        makeExternalize(v2SecretKey, qSetHash, 0, A3, 5));
                    REQUIRE(scp.mEnvs.size() == 1);
                    verifyConfirm(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                                  UINT32_MAX, AInf, 3, UINT32_MAX);
                }
            }
        }

        SECTION("normal round (1,x)")
        {
            nodesAllPledgeToCommit();
            REQUIRE(scp.mEnvs.size() == 3);

            SCPBallot b(1, xValue);

            // bunch of prepare messages with "commit b"
            SCPEnvelope preparedC1 = makePrepare(v1SecretKey, qSetHash, 0, b,
                                                 &b, b.counter, b.counter);
            SCPEnvelope preparedC2 = makePrepare(v2SecretKey, qSetHash, 0, b,
                                                 &b, b.counter, b.counter);
            SCPEnvelope preparedC3 = makePrepare(v3SecretKey, qSetHash, 0, b,
                                                 &b, b.counter, b.counter);
            SCPEnvelope preparedC4 = makePrepare(v4SecretKey, qSetHash, 0, b,
                                                 &b, b.counter, b.counter);

            // those should not trigger anything just yet
            scp.receiveEnvelope(preparedC1);
            scp.receiveEnvelope(preparedC2);
            REQUIRE(scp.mEnvs.size() == 3);

            // this should cause the node to accept 'commit b' (quorum)
            // and therefore send a "CONFIRM" message
            scp.receiveEnvelope(preparedC3);
            REQUIRE(scp.mEnvs.size() == 4);

            verifyConfirm(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, 1, b,
                          b.counter, b.counter);

            // bunch of confirm messages
            SCPEnvelope confirm1 = makeConfirm(
                v1SecretKey, qSetHash, 0, b.counter, b, b.counter, b.counter);
            SCPEnvelope confirm2 = makeConfirm(
                v2SecretKey, qSetHash, 0, b.counter, b, b.counter, b.counter);
            SCPEnvelope confirm3 = makeConfirm(
                v3SecretKey, qSetHash, 0, b.counter, b, b.counter, b.counter);
            SCPEnvelope confirm4 = makeConfirm(
                v4SecretKey, qSetHash, 0, b.counter, b, b.counter, b.counter);

            // those should not trigger anything just yet
            scp.receiveEnvelope(confirm1);
            scp.receiveEnvelope(confirm2);
            REQUIRE(scp.mEnvs.size() == 4);

            scp.receiveEnvelope(confirm3);
            // this causes our node to
            // externalize (confirm commit c)
            REQUIRE(scp.mEnvs.size() == 5);

            // The slot should have externalized the value
            REQUIRE(scp.mExternalizedValues.size() == 1);
            REQUIRE(scp.mExternalizedValues[0] == xValue);

            verifyExternalize(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, b,
                              b.counter);

            // extra vote should not do anything
            scp.receiveEnvelope(confirm4);
            REQUIRE(scp.mEnvs.size() == 5);
            REQUIRE(scp.mExternalizedValues.size() == 1);

            // duplicate should just no-op
            scp.receiveEnvelope(confirm2);
            REQUIRE(scp.mEnvs.size() == 5);
            REQUIRE(scp.mExternalizedValues.size() == 1);

            SECTION("bumpToBallot prevented once committed")
            {
                SCPBallot b2;
                SECTION("bumpToBallot prevented once committed (by value)")
                {
                    b2 = SCPBallot(1, zValue);
                }
                SECTION("bumpToBallot prevented once committed (by counter)")
                {
                    b2 = SCPBallot(2, xValue);
                }
                SECTION("bumpToBallot prevented once committed (by value and "
                        "counter)")
                {
                    b2 = SCPBallot(2, zValue);
                }

                SCPEnvelope confirm1b2, confirm2b2, confirm3b2, confirm4b2;
                confirm1b2 = makeConfirm(v1SecretKey, qSetHash, 0, b2.counter,
                                         b2, b2.counter, b2.counter);
                confirm2b2 = makeConfirm(v2SecretKey, qSetHash, 0, b2.counter,
                                         b2, b2.counter, b2.counter);
                confirm3b2 = makeConfirm(v3SecretKey, qSetHash, 0, b2.counter,
                                         b2, b2.counter, b2.counter);
                confirm4b2 = makeConfirm(v4SecretKey, qSetHash, 0, b2.counter,
                                         b2, b2.counter, b2.counter);

                scp.receiveEnvelope(confirm1b2);
                scp.receiveEnvelope(confirm2b2);
                scp.receiveEnvelope(confirm3b2);
                scp.receiveEnvelope(confirm4b2);
                REQUIRE(scp.mEnvs.size() == 5);
                REQUIRE(scp.mExternalizedValues.size() == 1);
            }
        }

        SECTION("range check")
        {
            nodesAllPledgeToCommit();
            REQUIRE(scp.mEnvs.size() == 3);

            SCPBallot b(1, xValue);

            // bunch of prepare messages with "commit b"
            SCPEnvelope preparedC1 = makePrepare(v1SecretKey, qSetHash, 0, b,
                                                 &b, b.counter, b.counter);
            SCPEnvelope preparedC2 = makePrepare(v2SecretKey, qSetHash, 0, b,
                                                 &b, b.counter, b.counter);
            SCPEnvelope preparedC3 = makePrepare(v3SecretKey, qSetHash, 0, b,
                                                 &b, b.counter, b.counter);
            SCPEnvelope preparedC4 = makePrepare(v4SecretKey, qSetHash, 0, b,
                                                 &b, b.counter, b.counter);

            // those should not trigger anything just yet
            scp.receiveEnvelope(preparedC1);
            scp.receiveEnvelope(preparedC2);
            REQUIRE(scp.mEnvs.size() == 3);

            // this should cause the node to accept 'commit b' (quorum)
            // and therefore send a "CONFIRM" message
            scp.receiveEnvelope(preparedC3);
            REQUIRE(scp.mEnvs.size() == 4);

            verifyConfirm(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, 1, b,
                          b.counter, b.counter);

            // bunch of confirm messages with different ranges
            SCPBallot b5(5, xValue);
            SCPEnvelope confirm1 = makeConfirm(v1SecretKey, qSetHash, 0, 4,
                                               SCPBallot(4, xValue), 2, 4);
            SCPEnvelope confirm2 = makeConfirm(v2SecretKey, qSetHash, 0, 6,
                                               SCPBallot(6, xValue), 2, 6);
            SCPEnvelope confirm3 = makeConfirm(v3SecretKey, qSetHash, 0, 5,
                                               SCPBallot(5, xValue), 3, 5);
            SCPEnvelope confirm4 = makeConfirm(v4SecretKey, qSetHash, 0, 6,
                                               SCPBallot(6, xValue), 3, 6);

            // this should not trigger anything just yet
            scp.receiveEnvelope(confirm1);

            // v-blocking
            //   * b gets bumped to (4,x)
            //   * p gets bumped to (4,x)
            //   * (c,h) gets bumped to (2,4)
            scp.receiveEnvelope(confirm2);
            REQUIRE(scp.mEnvs.size() == 5);
            verifyConfirm(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, 4,
                          SCPBallot(4, xValue), 2, 4);

            // this causes to externalize
            // range is [3,4]
            scp.receiveEnvelope(confirm4);
            REQUIRE(scp.mEnvs.size() == 6);

            // The slot should have externalized the value
            REQUIRE(scp.mExternalizedValues.size() == 1);
            REQUIRE(scp.mExternalizedValues[0] == xValue);

            verifyExternalize(scp.mEnvs[5], v0SecretKey, qSetHash0, 0,
                              SCPBallot(3, xValue), 4);
        }

        SECTION("timeout when h is set -> stay locked on h")
        {
            SCPBallot bx(1, xValue);
            REQUIRE(scp.bumpState(0, xValue));
            REQUIRE(scp.mEnvs.size() == 1);

            // v-blocking -> prepared
            // quorum -> confirm prepared
            recvQuorum(makePrepareGen(qSetHash, bx, &bx));
            REQUIRE(scp.mEnvs.size() == 3);
            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, bx, &bx,
                          bx.counter, bx.counter);

            // now, see if we can timeout and move to a different value
            REQUIRE(scp.bumpState(0, yValue));
            REQUIRE(scp.mEnvs.size() == 4);
            SCPBallot newbx(2, xValue);
            verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, newbx, &bx,
                          bx.counter, bx.counter);
        }
        SECTION("timeout when h exists but can't be set -> vote for h")
        {
            // start with (1,y)
            SCPBallot by(1, yValue);
            REQUIRE(scp.bumpState(0, yValue));
            REQUIRE(scp.mEnvs.size() == 1);

            SCPBallot bx(1, xValue);
            // but quorum goes with (1,x)
            // v-blocking -> prepared
            recvVBlocking(makePrepareGen(qSetHash, bx, &bx));
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, by, &bx);
            // quorum -> confirm prepared (no-op as b > h)
            recvQuorumChecks(makePrepareGen(qSetHash, bx, &bx), false, false);
            REQUIRE(scp.mEnvs.size() == 2);

            REQUIRE(scp.bumpState(0, yValue));
            REQUIRE(scp.mEnvs.size() == 3);
            SCPBallot newbx(2, xValue);
            // on timeout:
            // * we should move to the quorum's h value
            // * c can't be set yet as b > h
            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, newbx, &bx,
                          0, bx.counter);
        }

        SECTION("timeout from multiple nodes")
        {
            REQUIRE(scp.bumpState(0, xValue));

            SCPBallot x1(1, xValue);

            REQUIRE(scp.mEnvs.size() == 1);
            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);

            recvQuorum(makePrepareGen(qSetHash, x1));
            // quorum -> prepared (1,x)
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, x1, &x1);

            SCPBallot x2(2, xValue);
            // timeout from local node
            REQUIRE(scp.bumpState(0, xValue));
            // prepares (2,x)
            REQUIRE(scp.mEnvs.size() == 3);
            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, x2, &x1);

            recvQuorum(makePrepareGen(qSetHash, x1, &x1));
            // quorum -> set nH=1
            REQUIRE(scp.mEnvs.size() == 4);
            verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, x2, &x1, 0,
                          1);
            REQUIRE(scp.mEnvs.size() == 4);

            recvVBlocking(makePrepareGen(qSetHash, x2, &x2, 1, 1));
            // v-blocking prepared (2,x) -> prepared (2,x)
            REQUIRE(scp.mEnvs.size() == 5);
            verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, x2, &x2, 0,
                          1);

            recvQuorum(makePrepareGen(qSetHash, x2, &x2, 1, 1));
            // quorum (including us) confirms (2,x) prepared -> set h=c=x2
            // we also get extra message: a quorum not including us confirms
            // (1,x) prepared
            //  -> we confirm c=h=x1
            REQUIRE(scp.mEnvs.size() == 7);
            verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0, x2, &x2, 2,
                          2);
            verifyConfirm(scp.mEnvs[6], v0SecretKey, qSetHash0, 0, 2, x2, 1, 1);
        }

        SECTION("timeout after prepare, receive old messages to prepare")
        {
            REQUIRE(scp.bumpState(0, xValue));

            SCPBallot x1(1, xValue);

            REQUIRE(scp.mEnvs.size() == 1);
            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);

            scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
            scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, x1));
            scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, x1));

            // quorum -> prepared (1,x)
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, x1, &x1);

            SCPBallot x2(2, xValue);
            // timeout from local node
            REQUIRE(scp.bumpState(0, xValue));
            // prepares (2,x)
            REQUIRE(scp.mEnvs.size() == 3);
            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, x2, &x1);

            SCPBallot x3(3, xValue);
            // timeout again
            REQUIRE(scp.bumpState(0, xValue));
            // prepares (3,x)
            REQUIRE(scp.mEnvs.size() == 4);
            verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, x3, &x1);

            // other nodes moved on with x2
            scp.receiveEnvelope(
                makePrepare(v1SecretKey, qSetHash, 0, x2, &x2, 1, 2));
            scp.receiveEnvelope(
                makePrepare(v2SecretKey, qSetHash, 0, x2, &x2, 1, 2));
            // v-blocking -> prepared x2
            REQUIRE(scp.mEnvs.size() == 5);
            verifyPrepare(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, x3, &x2);

            scp.receiveEnvelope(
                makePrepare(v3SecretKey, qSetHash, 0, x2, &x2, 1, 2));
            // quorum -> set nH=2
            REQUIRE(scp.mEnvs.size() == 6);
            verifyPrepare(scp.mEnvs[5], v0SecretKey, qSetHash0, 0, x3, &x2, 0,
                          2);
        }

        SECTION("non validator watching the network")
        {
            SIMULATION_CREATE_NODE(NV);
            TestSCP scpNV(vNVSecretKey.getPublicKey(), qSet, false);
            scpNV.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
            uint256 qSetHashNV = scpNV.mSCP.getLocalNode()->getQuorumSetHash();

            SCPBallot b(1, xValue);
            REQUIRE(scpNV.bumpState(0, xValue));
            REQUIRE(scpNV.mEnvs.size() == 0);
            verifyPrepare(scpNV.getCurrentEnvelope(0, vNVNodeID), vNVSecretKey,
                          qSetHashNV, 0, b);
            auto ext1 = makeExternalize(v1SecretKey, qSetHash, 0, b, 1);
            auto ext2 = makeExternalize(v2SecretKey, qSetHash, 0, b, 1);
            auto ext3 = makeExternalize(v3SecretKey, qSetHash, 0, b, 1);
            auto ext4 = makeExternalize(v4SecretKey, qSetHash, 0, b, 1);
            scpNV.receiveEnvelope(ext1);
            scpNV.receiveEnvelope(ext2);
            scpNV.receiveEnvelope(ext3);
            REQUIRE(scpNV.mEnvs.size() == 0);
            verifyConfirm(scpNV.getCurrentEnvelope(0, vNVNodeID), vNVSecretKey,
                          qSetHashNV, 0, UINT32_MAX,
                          SCPBallot(UINT32_MAX, xValue), 1, UINT32_MAX);
            scpNV.receiveEnvelope(ext4);
            REQUIRE(scpNV.mEnvs.size() == 0);
            verifyExternalize(scpNV.getCurrentEnvelope(0, vNVNodeID),
                              vNVSecretKey, qSetHashNV, 0, b, UINT32_MAX);
            REQUIRE(scpNV.mExternalizedValues[0] == xValue);
        }

        SECTION("restore ballot protocol")
        {
            TestSCP scp2(v0SecretKey.getPublicKey(), qSet);
            scp2.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
            SCPBallot b(2, xValue);
            SECTION("prepare")
            {
                scp2.mSCP.setStateFromEnvelope(
                    0, scp2.wrapEnvelope(
                           makePrepare(v0SecretKey, qSetHash0, 0, b)));
            }
            SECTION("confirm")
            {
                scp2.mSCP.setStateFromEnvelope(
                    0, scp2.wrapEnvelope(
                           makeConfirm(v0SecretKey, qSetHash0, 0, 2, b, 1, 2)));
            }
            SECTION("externalize")
            {
                scp2.mSCP.setStateFromEnvelope(
                    0, scp2.wrapEnvelope(
                           makeExternalize(v0SecretKey, qSetHash0, 0, b, 2)));
            }
        }
    };

    testTimeouts(scp, test);
}

TEST_CASE("ballot protocol core3", "[scp][ballotprotocol]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    // core3 has an edge case where v-blocking and quorum can be the same
    // v-blocking set size: 2
    // threshold: 2 = 1 + self or 2 others
    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);

    auto test = [&](TestSCP& scp) {
        scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
        uint256 qSetHash0 = scp.mSCP.getLocalNode()->getQuorumSetHash();

        REQUIRE(xValue < yValue);
        REQUIRE(yValue < zValue);

        auto recvQuorumChecksEx2 = [&](genEnvelope gen, bool withChecks,
                                       bool delayedQuorum, bool checkUpcoming,
                                       bool minQuorum) {
            SCPEnvelope e1 = gen(v1SecretKey);
            SCPEnvelope e2 = gen(v2SecretKey);

            scp.bumpTimerOffset();

            size_t i = scp.mEnvs.size() + 1;
            scp.receiveEnvelope(e1);
            if (withChecks && !delayedQuorum)
            {
                REQUIRE(scp.mEnvs.size() == i);
            }
            if (checkUpcoming)
            {
                REQUIRE(scp.hasBallotTimerUpcoming());
            }
            if (!minQuorum)
            {
                // nothing happens with an extra vote (unless we're in
                // delayedQuorum)
                scp.receiveEnvelope(e2);
                if (withChecks)
                {
                    REQUIRE(scp.mEnvs.size() == i);
                }
            }
        };
        auto recvQuorumChecksEx =
            std::bind(recvQuorumChecksEx2, _1, _2, _3, _4, false);
        auto recvQuorumChecks =
            std::bind(recvQuorumChecksEx, _1, _2, _3, false);

        // no timer is set
        REQUIRE(!scp.hasBallotTimer());

        Value const& aValue = zValue;
        Value const& bValue = xValue;

        SCPBallot A1(1, aValue);
        SCPBallot B1(1, bValue);

        SCPBallot A2 = A1;
        A2.counter++;

        SCPBallot A3 = A2;
        A3.counter++;

        SCPBallot A4 = A3;
        A4.counter++;

        SCPBallot A5 = A4;
        A5.counter++;

        SCPBallot AInf(UINT32_MAX, aValue), BInf(UINT32_MAX, bValue);

        SCPBallot B2 = B1;
        B2.counter++;

        SCPBallot B3 = B2;
        B3.counter++;

        SECTION("prepared B1 (quorum votes B1) local aValue")
        {
            REQUIRE(scp.bumpState(0, aValue));
            REQUIRE(scp.mEnvs.size() == 1);
            REQUIRE(!scp.hasBallotTimer());

            scp.bumpTimerOffset();
            recvQuorumChecks(makePrepareGen(qSetHash, B1), true, true);
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &B1);
            REQUIRE(scp.hasBallotTimerUpcoming());
            SECTION("quorum prepared B1")
            {
                scp.bumpTimerOffset();
                recvQuorumChecks(makePrepareGen(qSetHash, B1, &B1), false,
                                 false);
                REQUIRE(scp.mEnvs.size() == 2);
                // nothing happens:
                // computed_h = B1 (2)
                //    does not actually update h as b > computed_h
                //    also skips (3)
                REQUIRE(!scp.hasBallotTimerUpcoming());
                SECTION("quorum bumps to A1")
                {
                    scp.bumpTimerOffset();
                    recvQuorumChecksEx2(makePrepareGen(qSetHash, A1, &B1),
                                        false, false, false, true);

                    REQUIRE(scp.mEnvs.size() == 3);
                    // still does not set h as b > computed_h
                    verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, A1,
                                  &A1, 0, 0, &B1);
                    REQUIRE(!scp.hasBallotTimerUpcoming());

                    scp.bumpTimerOffset();
                    // quorum commits A1
                    recvQuorumChecksEx2(
                        makePrepareGen(qSetHash, A2, &A1, 1, 1, &B1), false,
                        false, false, true);
                    REQUIRE(scp.mEnvs.size() == 4);
                    verifyConfirm(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, 2,
                                  A1, 1, 1);
                    REQUIRE(!scp.hasBallotTimerUpcoming());
                }
            }
        }
        SECTION("prepared A1 with timeout")
        {
            // starts with bValue (smallest)
            REQUIRE(scp.bumpState(0, bValue));
            REQUIRE(scp.mEnvs.size() == 1);

            // setup
            recvQuorumChecks(makePrepareGen(qSetHash, A1, &A1, 0, 1), false,
                             false);
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, A1, &A1, 1,
                          1);

            // now, receive bumped votes
            recvQuorumChecks(makePrepareGen(qSetHash, A2, &B2, 0, 1, &A1), true,
                             true);
            REQUIRE(scp.mEnvs.size() == 3);
            // p=B2, p'=A1 (1)
            // computed_h = B2 (2)
            //   does not update h as b < computed_h
            // v-blocking ahead -> b = computed_h = B2 (9)
            // h = B2 (2) (now possible)
            // c = 0 (1)
            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, B2, &A2, 0,
                          2, &B2);
        }
        SECTION("node without self - quorum timeout")
        {
            SIMULATION_CREATE_NODE(NodeNS);
            TestSCP scpNNS(vNodeNSSecretKey.getPublicKey(), qSet);
            scpNNS.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
            uint256 qSetHashNodeNS =
                scpNNS.mSCP.getLocalNode()->getQuorumSetHash();

            scpNNS.receiveEnvelope(
                makePrepare(v1SecretKey, qSetHash, 0, A2, &B2, 0, 1, &A1));
            scpNNS.receiveEnvelope(
                makePrepare(v2SecretKey, qSetHash, 0, A1, &A1, 1, 1));

            REQUIRE(scpNNS.mEnvs.size() == 1);
            verifyPrepare(scpNNS.mEnvs[0], vNodeNSSecretKey, qSetHashNodeNS, 0,
                          A1, &A1, 1, 1);

            scpNNS.receiveEnvelope(
                makePrepare(v0SecretKey, qSetHash, 0, A2, &B2, 0, 1, &A1));

            REQUIRE(scpNNS.mEnvs.size() == 2);
            verifyPrepare(scpNNS.mEnvs[1], vNodeNSSecretKey, qSetHashNodeNS, 0,
                          B2, &A2, 0, 2, &B2);
        }
    };

    testTimeouts(scp, test);
}

TEST_CASE("ballot protocol self-emits CONFIRM after federated accept-commit on "
          "structurally-valid value",
          "[scp][ballotprotocol]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);
    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

    // xValue stays kStructurallyValidValue throughout, with a wait time
    // below the download timeout so maybeReplaceValueWithEmptyTxSet does not
    // replace it with an empty-tx-set value.
    scp.startDownload(xValue, UNDER_TX_SET_TIMEOUT);
    scp.mValidateValueOverride = xValueStructurallyValidValidationOverride;

    SCPBallot xB1(1, xValue);

    // v0 enters ballot protocol with xValue.
    REQUIRE(scp.bumpState(0, xValue));
    REQUIRE(scp.mEnvs.size() == 1);

    // v1, v2 vote-prepare for (1, xValue) → v0 accept-prepared (1, xValue).
    REQUIRE(scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, xB1)) ==
            SCP::EnvelopeState::VALID);
    REQUIRE(scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, xB1)) ==
            SCP::EnvelopeState::VALID);

    // v1, v2 signal accept-prepared (1, xValue). v0 would normally
    // confirm-prepared here, but setConfirmPrepared stalls on
    // kStructurallyValidValue so c/h stay unset on v0's side.
    REQUIRE(
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, xB1, &xB1)) ==
        SCP::EnvelopeState::VALID);
    REQUIRE(
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, xB1, &xB1)) ==
        SCP::EnvelopeState::VALID);

    // v1, v2 vote-to-commit (1, xValue) via nC/nH on their PREPAREs.
    // federatedAccept fires via the "quorum voted-or-accepted" path v0
    // accept-commits, transitions mPhase to CONFIRM, and self-emits a CONFIRM
    // with xValue.
    REQUIRE(scp.receiveEnvelope(
                makePrepare(v1SecretKey, qSetHash, 0, xB1, &xB1, 1, 1)) ==
            SCP::EnvelopeState::VALID);
    REQUIRE(scp.receiveEnvelope(
                makePrepare(v2SecretKey, qSetHash, 0, xB1, &xB1, 1, 1)) ==
            SCP::EnvelopeState::VALID);

    // Last emitted envelope should be a CONFIRM carrying xValue — proves that
    // processEnvelope correctly accepts self-emitted CONFIRMs with
    // kStructurallyValidValue.
    auto const& lastEnv = scp.mEnvs.back();
    REQUIRE(lastEnv.statement.pledges.type() == SCP_ST_CONFIRM);
    auto const& cBallot = lastEnv.statement.pledges.confirm().ballot;
    REQUIRE(cBallot.value == xValue);
    REQUIRE(!scp.isEmptyTxSetValue(cBallot.value));

    // A peer CONFIRM whose value v0 considers kStructurallyValidValue is
    // rejected by processEnvelope
    auto const res = scp.receiveEnvelope(
        makeConfirm(v1SecretKey, qSetHash, 0, 1, xB1, 1, 1));
    REQUIRE(res == SCP::EnvelopeState::INVALID);
}

TEST_CASE("drop tx set on download timeout", "[scp][ballotprotocol]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    // 3 node network with threshold=2 (need any 2 nodes to form quorum)
    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);
    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
    uint256 qSetHash0 = scp.mSCP.getLocalNode()->getQuorumSetHash();

    SECTION("timeout during prepare phase")
    {
        // Node v0 starts ballot protocol with xValue
        // Simulate that xValue is awaiting download with timeout exceeded
        scp.startDownload(xValue, OVER_TX_SET_TIMEOUT);

        // Now call bumpState which should trigger
        // maybeReplaceValueWithEmptyTxSet
        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        // The ballot should have an empty-tx-set value, not the original
        // xValue
        auto const& emittedBallot =
            scp.mEnvs[0].statement.pledges.prepare().ballot;

        REQUIRE(emittedBallot.counter == 1);
        REQUIRE(scp.isEmptyTxSetValue(emittedBallot.value));

        // Verify it's the empty-tx-set value derived from xValue
        Value expectedEmptyTxSetValue =
            scp.makeEmptyTxSetValueFromValue(xValue);
        REQUIRE(emittedBallot.value == expectedEmptyTxSetValue);

        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                      SCPBallot(1, expectedEmptyTxSetValue));
    }

    SECTION("no timeout when wait time under threshold")
    {
        // Node v0 starts ballot protocol with xValue
        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        SCPBallot b1(1, xValue);

        // Simulate that xValue is awaiting download but wait time is still low
        scp.startDownload(xValue, UNDER_TX_SET_TIMEOUT);

        // Try to bump state - should NOT replace with an empty-tx-set value
        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 2);

        // Verify ballot still has original xValue, not an empty-tx-set value
        auto const& emittedBallot =
            scp.mEnvs[1].statement.pledges.prepare().ballot;
        REQUIRE(emittedBallot.counter == 2);
        REQUIRE(!scp.isEmptyTxSetValue(emittedBallot.value));
        REQUIRE(emittedBallot.value == xValue);
    }

    SECTION("empty-tx-set value can be prepared and confirmed")
    {
        // Start with xValue and timeout to an empty-tx-set value
        scp.startDownload(xValue, OVER_TX_SET_TIMEOUT);

        REQUIRE(scp.bumpState(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);

        Value emptyTxSetValue = scp.makeEmptyTxSetValueFromValue(xValue);
        SCPBallot emptyTxSetB1(1, emptyTxSetValue);

        // Verify we emitted an empty-tx-set value
        REQUIRE(scp.isEmptyTxSetValue(
            scp.mEnvs[0].statement.pledges.prepare().ballot.value));
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, emptyTxSetB1);

        // Other nodes also move to the empty-tx-set value
        scp.receiveEnvelope(
            makePrepare(v1SecretKey, qSetHash, 0, emptyTxSetB1));
        scp.receiveEnvelope(
            makePrepare(v2SecretKey, qSetHash, 0, emptyTxSetB1));

        // Should prepare the empty-tx-set value (quorum reached)
        REQUIRE(scp.mEnvs.size() == 2);
        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, emptyTxSetB1,
                      &emptyTxSetB1);

        // Quorum confirms prepared empty-tx-set value
        scp.receiveEnvelope(
            makePrepare(v1SecretKey, qSetHash, 0, emptyTxSetB1, &emptyTxSetB1));
        scp.receiveEnvelope(
            makePrepare(v2SecretKey, qSetHash, 0, emptyTxSetB1, &emptyTxSetB1));

        REQUIRE(scp.mEnvs.size() == 3);
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, emptyTxSetB1,
                      &emptyTxSetB1, 1, 1);

        // Accept commit
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, emptyTxSetB1,
                                        &emptyTxSetB1, 1, 1));
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, emptyTxSetB1,
                                        &emptyTxSetB1, 1, 1));

        REQUIRE(scp.mEnvs.size() == 4);
        verifyConfirm(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, 1, emptyTxSetB1,
                      1, 1);

        // Externalize the empty-tx-set value
        scp.receiveEnvelope(
            makeConfirm(v1SecretKey, qSetHash, 0, 1, emptyTxSetB1, 1, 1));
        scp.receiveEnvelope(
            makeConfirm(v2SecretKey, qSetHash, 0, 1, emptyTxSetB1, 1, 1));

        REQUIRE(scp.mEnvs.size() == 5);
        verifyExternalize(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, emptyTxSetB1,
                          1);

        // Verify the externalized value is the empty-tx-set value
        REQUIRE(scp.mExternalizedValues.size() == 1);
        REQUIRE(scp.isEmptyTxSetValue(scp.mExternalizedValues[0]));
        REQUIRE(scp.mExternalizedValues[0] == emptyTxSetValue);
    }
}

TEST_CASE("Proper handling of non-current ledger value",
          "[scp][ballotprotocol]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);
    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

    // validateValue reports xValue is not for the current ledger.
    scp.mValidateValueOverride = xValueNotCurrentLedgerOverride;

    REQUIRE(scp.bumpState(0, xValue));

    // Do not emit a ballot with a kMaybeValid value
    REQUIRE(scp.mEnvs.size() == 0);
}

TEST_CASE("setConfirmPrepared stalls on kStructurallyValidValue value",
          "[scp][ballotprotocol]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);
    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

    // Simulate parallel downloading
    scp.startDownload(xValue, UNDER_TX_SET_TIMEOUT);

    // v0 enters ballot protocol
    REQUIRE(scp.bumpState(0, xValue));
    REQUIRE(scp.mEnvs.size() == 1);
    SCPBallot xB1(1, xValue);

    // v1 and v2 send PREPAREs with prepared — quorum confirms prepared
    REQUIRE(
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, xB1, &xB1)) ==
        SCP::EnvelopeState::VALID);
    REQUIRE(
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, xB1, &xB1)) ==
        SCP::EnvelopeState::VALID);

    SECTION("commit gate stalls mCommit but mHighBallot is set")
    {

        // setConfirmPrepared sets mHighBallot (nH > 0) but the commit gate
        // stalls mCommit (nC == 0) because xValue is kStructurallyValidValue.
        // Check the latest emitted PREPARE.
        REQUIRE(scp.mEnvs.size() >= 2);
        auto const& lastPrep = scp.mEnvs.back().statement.pledges.prepare();
        REQUIRE(lastPrep.nH == 1);
        REQUIRE(lastPrep.nC == 0);
    }

    SECTION("proceeds after value becomes validated")
    {
        auto envsBeforeClear = scp.mEnvs.size();

        // Simulate tx set arrival — value becomes fully validated
        scp.clearDownload(xValue);

        // Trigger advanceSlot with envelopes that confirm-prepare at a
        // higher counter, so attemptConfirmPrepared finds newH > mHighBallot.
        // This causes setConfirmPrepared to be called with the now-validated
        // value, setting mCommit.
        SCPBallot xB2(2, xValue);
        REQUIRE(scp.receiveEnvelope(
                    makePrepare(v1SecretKey, qSetHash, 0, xB2, &xB2)) ==
                SCP::EnvelopeState::VALID);
        REQUIRE(scp.receiveEnvelope(
                    makePrepare(v2SecretKey, qSetHash, 0, xB2, &xB2)) ==
                SCP::EnvelopeState::VALID);

        // setConfirmPrepared should now succeed — mCommit set, node
        // progresses. Expect at least one new envelope with nC > 0 or a
        // CONFIRM/EXTERNALIZE.
        REQUIRE(scp.mEnvs.size() > envsBeforeClear);
        bool foundC = false;
        for (size_t i = envsBeforeClear; i < scp.mEnvs.size(); i++)
        {
            auto const& st = scp.mEnvs[i].statement;
            if (st.pledges.type() == SCP_ST_PREPARE)
            {
                if (st.pledges.prepare().nC > 0)
                {
                    foundC = true;
                    break;
                }
            }
            else
            {
                // CONFIRM or EXTERNALIZE also proves we got past the stall
                foundC = true;
                break;
            }
        }
        REQUIRE(foundC);
    }
}

TEST_CASE("incoming PREPARE with structurally valid prepared value is accepted",
          "[scp][ballotprotocol]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);
    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

    // v0 enters ballot protocol with yValue
    REQUIRE(scp.bumpState(0, yValue));
    REQUIRE(scp.mEnvs.size() == 1);

    // Set xValue to kStructurallyValidValue
    scp.mValidateValueOverride = xValueStructurallyValidValidationOverride;

    // v1 sends PREPARE with valid ballot value but only structurally valid
    // prepared value.  This should be accepted.
    SCPBallot yB1(1, yValue);
    SCPBallot xB1(1, xValue);
    REQUIRE(
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, yB1, &xB1)) ==
        SCP::EnvelopeState::VALID);
}

TEST_CASE("incoming PREPARE with non-tx-set-invalid value is dropped",
          "[scp][ballotprotocol]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);

    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);
    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));

    // xValue is invalid for some non-tx-set reason (close time, signature,
    // ...). This should NOT be accepted as kStructurallyValidValue; the
    // statement should be dropped.
    scp.mValidateValueOverride = xValueNonTxSetInvalidValidationOverride;

    SCPBallot xB1(1, xValue);

    // Envelope should be rejected as invalid
    REQUIRE(scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, xB1)) ==
            SCP::EnvelopeState::INVALID);

    // Envelope was not recorded — recordEnvelope only runs for non-kInvalid.
    REQUIRE(scp.mSCP.getLatestMessage(v1NodeID) == nullptr);
    // No local emit triggered.
    REQUIRE(scp.mEnvs.empty());
}

// ---------------------------------------------------------------------------
// Leader rounds
//
// There is no nomination phase. A round is a ballot counter; each round has
// one leader (SCPDriver::getRoundLeader). At counter r a node votes for the
// ballot `leader(r)` votes for at counter r, unless it already has a value it
// must stick to.
// ---------------------------------------------------------------------------

namespace
{
SCPBallot const&
latestBallot(TestSCP& scp)
{
    REQUIRE(!scp.mEnvs.empty());
    auto const& pl = scp.mEnvs.back().statement.pledges;
    REQUIRE(pl.type() == SCP_ST_PREPARE);
    return pl.prepare().ballot;
}
}

TEST_CASE("leader rounds core5", "[scp][leaderschedule]")
{
    setupValues();
    SIMULATION_CREATE_NODE(0);
    SIMULATION_CREATE_NODE(1);
    SIMULATION_CREATE_NODE(2);
    SIMULATION_CREATE_NODE(3);
    SIMULATION_CREATE_NODE(4);

    // we need 5 nodes to avoid sharing various thresholds:
    // v-blocking set size: 2
    // threshold: 4 = 3 + self or 4 others
    SCPQuorumSet qSet;
    qSet.threshold = 4;
    qSet.validators.push_back(v0NodeID);
    qSet.validators.push_back(v1NodeID);
    qSet.validators.push_back(v2NodeID);
    qSet.validators.push_back(v3NodeID);
    qSet.validators.push_back(v4NodeID);

    uint256 qSetHash = sha256(xdr::xdr_to_opaque(qSet));

    TestSCP scp(v0SecretKey.getPublicKey(), qSet);
    scp.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
    uint256 qSetHash0 = scp.mSCP.getLocalNode()->getQuorumSetHash();

    // round r is led by schedule[(r - 1) % size]
    std::vector<NodeID> schedule;
    scp.mRoundLeader = [&](uint64, uint32 round) -> std::optional<NodeID> {
        if (schedule.empty())
        {
            return std::nullopt;
        }
        return schedule[(round - 1) % schedule.size()];
    };

    SCPBallot x1(1, xValue), y1(1, yValue), x2(2, xValue), y2(2, yValue),
        z2(2, zValue), z3(3, zValue);

    SECTION("the round 1 leader proposes when the slot starts")
    {
        schedule = {v0NodeID, v1NodeID, v2NodeID, v3NodeID, v4NodeID};
        REQUIRE(scp.startSlot(0, xValue));
        REQUIRE(scp.mEnvs.size() == 1);
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);
        REQUIRE(scp.mValuesBuilt == 1);
        REQUIRE(scp.getLeaderRound(0) == 1);
        REQUIRE(scp.mProposedValues.size() == 1);
        REQUIRE(scp.mProposedValues[0] == std::make_pair(uint32(1), xValue));
        // it has a ballot at its round: the ballot timer rules apply, not the
        // leader timer
        REQUIRE(!scp.hasLeaderTimer());

        SECTION("starting again changes nothing")
        {
            REQUIRE(!scp.startSlot(0, yValue));
            REQUIRE(scp.mEnvs.size() == 1);
            REQUIRE(scp.mValuesBuilt == 1);
        }
        SECTION("other nodes' ballots do not move the leader")
        {
            scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, y1));
            scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, y1));
            REQUIRE(scp.mEnvs.size() == 1);
        }
    }

    SECTION("a follower waits for the leader and adopts its ballot")
    {
        schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
        REQUIRE(!scp.startSlot(0, yValue));
        REQUIRE(scp.mEnvs.empty());
        // followers do not build a value
        REQUIRE(scp.mValuesBuilt == 0);
        REQUIRE(scp.getLeaderRound(0) == 1);
        REQUIRE(scp.hasLeaderTimer());

        // a ballot from a node that does not lead the round is recorded but
        // not adopted
        REQUIRE(
            scp.receiveEnvelope(makePrepare(v4SecretKey, qSetHash, 0, y1)) ==
            SCP::EnvelopeState::VALID);
        REQUIRE(scp.mEnvs.empty());
        REQUIRE(scp.hasLeaderTimer());

        // the leader's ballot is adopted
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
        REQUIRE(scp.mEnvs.size() == 1);
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);
        REQUIRE(!scp.hasLeaderTimer());
        REQUIRE(scp.mValuesBuilt == 0);

        SECTION("and externalizes with a quorum")
        {
            // quorum votes prepare x1 -> accept prepared
            scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, x1));
            scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, x1));
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, x1, &x1);
            // quorum accepted prepared -> confirm prepared, vote commit
            scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1, &x1));
            scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, x1, &x1));
            scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, x1, &x1));
            REQUIRE(scp.mEnvs.size() == 3);
            verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, x1, &x1, 1,
                          1);
            // quorum votes commit -> accept commit
            scp.receiveEnvelope(
                makePrepare(v1SecretKey, qSetHash, 0, x1, &x1, 1, 1));
            scp.receiveEnvelope(
                makePrepare(v2SecretKey, qSetHash, 0, x1, &x1, 1, 1));
            scp.receiveEnvelope(
                makePrepare(v3SecretKey, qSetHash, 0, x1, &x1, 1, 1));
            REQUIRE(scp.mEnvs.size() == 4);
            verifyConfirm(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, 1, x1, 1, 1);
            // quorum accepted commit -> externalize
            scp.receiveEnvelope(
                makeConfirm(v1SecretKey, qSetHash, 0, 1, x1, 1, 1));
            scp.receiveEnvelope(
                makeConfirm(v2SecretKey, qSetHash, 0, 1, x1, 1, 1));
            scp.receiveEnvelope(
                makeConfirm(v3SecretKey, qSetHash, 0, 1, x1, 1, 1));
            REQUIRE(scp.mEnvs.size() == 5);
            verifyExternalize(scp.mEnvs[4], v0SecretKey, qSetHash0, 0, x1, 1);
            REQUIRE(scp.mExternalizedValues[0] == xValue);
            REQUIRE(scp.mValuesBuilt == 0);
            REQUIRE(!scp.hasLeaderTimer());
        }
        SECTION("a later ballot of the same leader at the same counter is "
                "not adopted")
        {
            // equivocating leader: the node already voted for x1
            scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, y1));
            REQUIRE(scp.mEnvs.size() == 1);
        }
    }

    SECTION("a follower adopts the round 1 leader before its own trigger")
    {
        schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
        REQUIRE(scp.getLeaderRound(0) == 0);
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
        REQUIRE(scp.mEnvs.size() == 1);
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);
        REQUIRE(scp.getLeaderRound(0) == 1);

        // its own trigger then has nothing left to do
        REQUIRE(!scp.startSlot(0, yValue));
        REQUIRE(scp.mEnvs.size() == 1);
        REQUIRE(scp.mValuesBuilt == 0);
    }

    SECTION("before its trigger a node does not adopt later rounds' leaders")
    {
        schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, y2));
        REQUIRE(scp.mEnvs.empty());
        REQUIRE(scp.getLeaderRound(0) == 0);
        REQUIRE(!scp.hasLeaderTimer());
    }

    SECTION("a leader that is not started does not propose")
    {
        schedule = {v0NodeID, v1NodeID, v2NodeID, v3NodeID, v4NodeID};
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, y1));
        REQUIRE(scp.mEnvs.empty());
        REQUIRE(scp.mValuesBuilt == 0);
    }

    SECTION("silent leader: the leader timer hands over to the next leader")
    {
        SECTION("local node leads round 2")
        {
            schedule = {v1NodeID, v0NodeID, v2NodeID, v3NodeID, v4NodeID};
            REQUIRE(!scp.startSlot(0, yValue));
            REQUIRE(scp.hasLeaderTimer());
            auto timer = scp.getLeaderTimer();
            REQUIRE(timer.mAbsoluteTimeout ==
                    std::chrono::milliseconds(scp.mInitialBallotTimeoutMS));

            scp.fireTimer(Slot::LEADER_TIMER);
            REQUIRE(scp.getLeaderRound(0) == 2);
            REQUIRE(scp.mEnteredRounds.back() ==
                    std::make_pair(uint32(2), true));
            REQUIRE(scp.mEnvs.size() == 1);
            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, y2);
            REQUIRE(scp.mValuesBuilt == 1);
            REQUIRE(!scp.hasLeaderTimer());

            SECTION("the round 1 leader showing up late changes nothing")
            {
                scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
                REQUIRE(scp.mEnvs.size() == 1);
            }
            SECTION("a stale leader timer callback is ignored")
            {
                timer.mCallback();
                REQUIRE(scp.getLeaderRound(0) == 2);
                REQUIRE(scp.mEnvs.size() == 1);
            }
        }
        SECTION("another node leads round 2")
        {
            schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
            REQUIRE(!scp.startSlot(0, zValue));

            SECTION("its ballot arrives after the timeout")
            {
                scp.fireTimer(Slot::LEADER_TIMER);
                REQUIRE(scp.getLeaderRound(0) == 2);
                REQUIRE(scp.mEnvs.empty());
                // waiting for the round 2 leader now, with the round 2
                // timeout
                REQUIRE(scp.hasLeaderTimer());
                REQUIRE(
                    scp.getLeaderTimer().mAbsoluteTimeout ==
                    std::chrono::milliseconds(scp.mInitialBallotTimeoutMS +
                                              scp.mIncrementBallotTimeoutMS));

                SECTION("the round 2 leader's ballot is adopted")
                {
                    // a round 2 ballot from a node that does not lead round 2
                    scp.receiveEnvelope(
                        makePrepare(v3SecretKey, qSetHash, 0, x2));
                    REQUIRE(scp.mEnvs.empty());

                    scp.receiveEnvelope(
                        makePrepare(v2SecretKey, qSetHash, 0, y2));
                    REQUIRE(scp.mEnvs.size() == 1);
                    verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, y2);
                    REQUIRE(!scp.hasLeaderTimer());
                }
                SECTION("a late round 1 proposal still gets the node's vote")
                {
                    // The node never voted: it votes for the round 1 leader's
                    // late proposal at the round 1 counter, and keeps waiting
                    // for the round 2 leader.
                    scp.receiveEnvelope(
                        makePrepare(v1SecretKey, qSetHash, 0, x1));
                    REQUIRE(scp.mEnvs.size() == 1);
                    verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);
                    REQUIRE(scp.getLeaderRound(0) == 2);
                    REQUIRE(scp.hasLeaderTimer());
                    REQUIRE(!scp.hasBallotTimer());

                    SECTION("and externalizes with a quorum at round 1")
                    {
                        scp.receiveEnvelope(
                            makePrepare(v2SecretKey, qSetHash, 0, x1, &x1));
                        scp.receiveEnvelope(
                            makePrepare(v3SecretKey, qSetHash, 0, x1, &x1));
                        scp.receiveEnvelope(
                            makePrepare(v4SecretKey, qSetHash, 0, x1, &x1));
                        // accepted then confirmed prepared: the node votes to
                        // commit x at counter 1
                        REQUIRE(scp.mEnvs.size() >= 2);
                        auto const& last =
                            scp.mEnvs.back().statement.pledges.prepare();
                        REQUIRE(last.ballot.value == xValue);
                        REQUIRE(last.nC == 1);
                        REQUIRE(last.nH == 1);
                    }
                    SECTION("the round 2 leader's ballot takes over")
                    {
                        scp.receiveEnvelope(
                            makePrepare(v2SecretKey, qSetHash, 0, y2));
                        REQUIRE(scp.mEnvs.size() == 2);
                        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0,
                                      y2);
                        REQUIRE(!scp.hasLeaderTimer());
                    }
                    SECTION("a second late proposal is ignored")
                    {
                        // only a node without a ballot takes a late proposal
                        schedule = {v1NodeID, v2NodeID, v3NodeID, v0NodeID,
                                    v4NodeID};
                        scp.fireTimer(Slot::LEADER_TIMER);
                        REQUIRE(scp.getLeaderRound(0) == 3);
                        scp.receiveEnvelope(
                            makePrepare(v2SecretKey, qSetHash, 0, y2));
                        REQUIRE(scp.mEnvs.size() == 1);
                    }
                }
                SECTION("a proposal that is not valid is not taken late either")
                {
                    scp.mValidateValueOverride = [&](uint64, Value const& v,
                                                     bool proposal) {
                        return (proposal && v == xValue)
                                   ? SCPDriver::kInvalidValue
                                   : SCPDriver::kFullyValidatedValue;
                    };
                    scp.receiveEnvelope(
                        makePrepare(v1SecretKey, qSetHash, 0, x1));
                    REQUIRE(scp.mEnvs.empty());
                }
            }
            SECTION("the round 1 leader's proposal is pending: one extension")
            {
                // The application holds the leader's statement (e.g. it is
                // fetching its transaction set): the node waits one more
                // timeout, once.
                scp.mProposalPending = true;
                scp.fireTimer(Slot::LEADER_TIMER);
                REQUIRE(scp.getLeaderRound(0) == 1);
                REQUIRE(scp.hasLeaderTimer());
                REQUIRE(scp.getLeaderTimer().mAbsoluteTimeout ==
                        std::chrono::milliseconds(scp.mInitialBallotTimeoutMS));
                REQUIRE(scp.mEnvs.empty());

                SECTION("the proposal arrives during the extension")
                {
                    scp.receiveEnvelope(
                        makePrepare(v1SecretKey, qSetHash, 0, x1));
                    REQUIRE(scp.mEnvs.size() == 1);
                    verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);
                    REQUIRE(scp.getLeaderRound(0) == 1);
                    REQUIRE(!scp.hasLeaderTimer());
                }
                SECTION("it does not: the node moves on")
                {
                    scp.fireTimer(Slot::LEADER_TIMER);
                    REQUIRE(scp.getLeaderRound(0) == 2);
                    REQUIRE(scp.hasLeaderTimer());
                    REQUIRE(scp.mEnvs.empty());
                }
            }
            SECTION("its ballot arrived early: kept until the round starts")
            {
                // a single node cannot make others skip a round
                scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, y2));
                REQUIRE(scp.mEnvs.empty());
                REQUIRE(scp.getLeaderRound(0) == 1);

                scp.fireTimer(Slot::LEADER_TIMER);
                REQUIRE(scp.getLeaderRound(0) == 2);
                REQUIRE(scp.mEnvs.size() == 1);
                verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, y2);
            }
            SECTION("then local node leads round 3 with its own value")
            {
                scp.fireTimer(Slot::LEADER_TIMER);
                REQUIRE(scp.mEnvs.empty());
                scp.fireTimer(Slot::LEADER_TIMER);
                REQUIRE(scp.getLeaderRound(0) == 3);
                REQUIRE(scp.mEnvs.size() == 1);
                verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, z3);
                REQUIRE(scp.mValuesBuilt == 1);
            }
        }
    }

    SECTION("ballot timeout: the next leader takes over")
    {
        schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
        REQUIRE(!scp.startSlot(0, zValue));
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
        REQUIRE(scp.mEnvs.size() == 1);

        // a quorum is at counter 1: the ballot timer runs
        REQUIRE(!scp.hasBallotTimer());
        scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, y1));
        scp.receiveEnvelope(makePrepare(v4SecretKey, qSetHash, 0, y1));
        REQUIRE(scp.hasBallotTimer());
        REQUIRE(!scp.hasLeaderTimer());

        SECTION("to a leader whose ballot is known")
        {
            scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, y2));
            REQUIRE(scp.mEnvs.size() == 1);
            scp.fireTimer(Slot::BALLOT_PROTOCOL_TIMER);
            REQUIRE(scp.getLeaderRound(0) == 2);
            REQUIRE(scp.mEnvs.size() == 2);
            // v3 and v4 voted for y at counter 1: with the leader and the
            // local node that is a quorum, so (1, y) is accepted prepared
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, y2, &y1);
        }
        SECTION("to a leader that has not spoken yet")
        {
            scp.fireTimer(Slot::BALLOT_PROTOCOL_TIMER);
            REQUIRE(scp.getLeaderRound(0) == 2);
            // keeps its round 1 ballot, votes for nothing new
            REQUIRE(scp.mEnvs.size() == 1);
            REQUIRE(scp.hasLeaderTimer());
            // the ballot timer of the round it gave up on is stopped
            REQUIRE(!scp.hasBallotTimer());

            scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, y2));
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, y2, &y1);
            REQUIRE(!scp.hasLeaderTimer());
        }
        SECTION("to the local node, which keeps the value it voted for")
        {
            schedule = {v1NodeID, v0NodeID, v2NodeID, v3NodeID, v4NodeID};
            scp.fireTimer(Slot::BALLOT_PROTOCOL_TIMER);
            REQUIRE(scp.getLeaderRound(0) == 2);
            REQUIRE(scp.mEnvs.size() == 2);
            // x, not its own z: the transaction set of x is already out
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, x2);
            REQUIRE(scp.mValuesBuilt == 0);
            // the application was offered to put an equivalent value forward
            REQUIRE(scp.mReproposeCalls ==
                    std::vector<std::pair<uint32, Value>>{{2, xValue}});
            REQUIRE(scp.mProposedValues.back() ==
                    std::make_pair(uint32(2), xValue));
        }
        SECTION("to the local node, which proposes the application's "
                "replacement for the value it voted for")
        {
            // e.g. the same content under the new leader's signature
            schedule = {v1NodeID, v0NodeID, v2NodeID, v3NodeID, v4NodeID};
            scp.mReproposeValueOverride = [&](uint32 round, Value const& v) {
                REQUIRE(round == 2);
                REQUIRE(v == xValue);
                return scp.wrapValue(kValue);
            };
            scp.fireTimer(Slot::BALLOT_PROTOCOL_TIMER);
            REQUIRE(scp.getLeaderRound(0) == 2);
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0,
                          SCPBallot(2, kValue));
            REQUIRE(scp.mValuesBuilt == 0);
            REQUIRE(scp.mReproposeCalls.size() == 1);
            REQUIRE(scp.mProposedValues.back() ==
                    std::make_pair(uint32(2), kValue));
        }
        SECTION("to the local node, whose current value is not proposable")
        {
            schedule = {v1NodeID, v0NodeID, v2NodeID, v3NodeID, v4NodeID};
            scp.mValidateValueOverride = [&](uint64, Value const& v,
                                             bool proposal) {
                return (proposal && v == xValue)
                           ? SCPDriver::kInvalidValue
                           : SCPDriver::kFullyValidatedValue;
            };
            scp.fireTimer(Slot::BALLOT_PROTOCOL_TIMER);
            REQUIRE(scp.mEnvs.size() == 2);
            verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, z2);
            REQUIRE(scp.mValuesBuilt == 1);
        }
    }

    SECTION("a leader tied to a value it accepted as prepared keeps it")
    {
        // v2 and v3 vote for the round 1 leader's x as the local node does:
        // a quorum, so the local node accepts (1, x) as prepared. Other
        // nodes may confirm it and be bound to x, so when the local node
        // leads round 2 the application is not offered a replacement.
        schedule = {v1NodeID, v0NodeID, v2NodeID, v3NodeID, v4NodeID};
        scp.mReproposeValueOverride = [&](uint32, Value const&) {
            return scp.wrapValue(kValue);
        };
        REQUIRE(!scp.startSlot(0, zValue));
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
        REQUIRE(scp.mEnvs.size() == 1);
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, x1));
        scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, x1));
        REQUIRE(scp.mEnvs.size() == 2);
        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, x1, &x1);
        REQUIRE(scp.hasBallotTimer());

        scp.fireTimer(Slot::BALLOT_PROTOCOL_TIMER);
        REQUIRE(scp.getLeaderRound(0) == 2);
        REQUIRE(scp.mEnvs.size() == 3);
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, x2, &x1);
        REQUIRE(scp.mReproposeCalls.empty());
        REQUIRE(scp.mValuesBuilt == 0);
    }

    SECTION("a confirmed prepared value wins over the leader's")
    {
        schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
        REQUIRE(!scp.startSlot(0, zValue));
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
        scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, x1));
        scp.receiveEnvelope(makePrepare(v4SecretKey, qSetHash, 0, x1));
        REQUIRE(scp.mEnvs.size() == 2);
        verifyPrepare(scp.mEnvs[1], v0SecretKey, qSetHash0, 0, x1, &x1);
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1, &x1));
        scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, x1, &x1));
        scp.receiveEnvelope(makePrepare(v4SecretKey, qSetHash, 0, x1, &x1));
        // confirmed prepared: x is now the value to stick to
        REQUIRE(scp.mEnvs.size() == 3);
        verifyPrepare(scp.mEnvs[2], v0SecretKey, qSetHash0, 0, x1, &x1, 1, 1);

        // the round 2 leader votes for something else
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, y2));
        REQUIRE(scp.mEnvs.size() == 3);
        scp.fireTimer(Slot::BALLOT_PROTOCOL_TIMER);
        REQUIRE(scp.getLeaderRound(0) == 2);
        REQUIRE(scp.mEnvs.size() == 4);
        verifyPrepare(scp.mEnvs[3], v0SecretKey, qSetHash0, 0, x2, &x1, 1, 1);
        REQUIRE(!scp.hasLeaderTimer());
    }

    SECTION("a v-blocking set ahead moves a node to that round")
    {
        schedule = {v1NodeID, v2NodeID, v3NodeID, v0NodeID, v4NodeID};
        // v1 and v2 (v-blocking) are at counter 3, on a value they must
        // stick to; local node has no ballot at all
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, z3));
        REQUIRE(scp.getLeaderRound(0) == 0);
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, z3));
        REQUIRE(scp.getLeaderRound(0) == 3);
        // it does not vote for what they vote for: only the round's leader
        // decides
        REQUIRE(scp.mEnvs.empty());
        REQUIRE(scp.hasLeaderTimer());

        SECTION("where it adopts the leader")
        {
            scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, z3));
            REQUIRE(scp.mEnvs.size() == 1);
            // with v1, v2 and the leader that is a quorum voting for z3
            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, z3, &z3);
            REQUIRE(!scp.hasLeaderTimer());
        }
        SECTION("where it leads once it is started")
        {
            schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
            REQUIRE(scp.startSlot(0, xValue));
            REQUIRE(scp.mEnvs.size() == 1);
            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                          SCPBallot(3, xValue));
        }
    }

    SECTION("a started leader pushed to its round by a v-blocking set "
            "proposes")
    {
        schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
        REQUIRE(!scp.startSlot(0, xValue));
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, z3));
        scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, z3));
        REQUIRE(scp.getLeaderRound(0) == 3);
        REQUIRE(scp.mEnvs.size() == 1);
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0,
                      SCPBallot(3, xValue));
        REQUIRE(scp.mValuesBuilt == 1);
    }

    SECTION("proposal validation gates adoption")
    {
        schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
        REQUIRE(!scp.startSlot(0, zValue));

        SECTION("a value the node would not propose is not adopted")
        {
            // valid for the ballot protocol, but not as a proposal (e.g. it
            // carries an upgrade the local node did not arm)
            scp.mValidateValueOverride = [&](uint64, Value const& v,
                                             bool proposal) {
                return (proposal && v == xValue)
                           ? SCPDriver::kInvalidValue
                           : SCPDriver::kFullyValidatedValue;
            };
            REQUIRE(scp.receiveEnvelope(
                        makePrepare(v1SecretKey, qSetHash, 0, x1)) ==
                    SCP::EnvelopeState::VALID);
            REQUIRE(scp.mEnvs.empty());
            REQUIRE(scp.hasLeaderTimer());

            // the regular protocol still applies: a quorum that prepared the
            // value brings the node along
            scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1, &x1));
            scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, x1, &x1));
            // v-blocking accepted prepared
            REQUIRE(scp.mEnvs.empty());
            scp.receiveEnvelope(makePrepare(v3SecretKey, qSetHash, 0, x1, &x1));
            scp.receiveEnvelope(makePrepare(v4SecretKey, qSetHash, 0, x1, &x1));
            // quorum accepted prepared -> confirmed prepared
            REQUIRE(scp.mEnvs.size() == 1);
            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1, &x1, 1,
                          1);
        }
        SECTION("a value whose transaction set is still downloading is "
                "adopted")
        {
            scp.startDownload(xValue, UNDER_TX_SET_TIMEOUT);
            scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
            REQUIRE(scp.mEnvs.size() == 1);
            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, x1);
        }
    }

    SECTION("a leader that cannot build a value lets the round time out")
    {
        schedule = {v0NodeID, v1NodeID, v2NodeID, v3NodeID, v4NodeID};
        scp.mMakeValueOverride = []() { return ValueWrapperPtr(); };
        REQUIRE(!scp.startSlot(0, xValue));
        REQUIRE(scp.mEnvs.empty());
        REQUIRE(scp.mValuesBuilt == 1);
        REQUIRE(scp.hasLeaderTimer());
        scp.fireTimer(Slot::LEADER_TIMER);
        REQUIRE(scp.getLeaderRound(0) == 2);
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, y2));
        REQUIRE(scp.mEnvs.size() == 1);
        verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, y2);
    }

    SECTION("building the value re-enters SCP")
    {
        schedule = {v0NodeID, v1NodeID, v2NodeID, v3NodeID, v4NodeID};
        auto xW = scp.wrapValue(xValue);

        SECTION("the slot moved to a later round meanwhile")
        {
            scp.mMakeValueOverride = [&]() {
                // envelopes that became ready while the value was built
                scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, z3));
                scp.receiveEnvelope(makePrepare(v2SecretKey, qSetHash, 0, z3));
                return xW;
            };
            REQUIRE(scp.startSlot(0, xValue));
            // the value was built for round 1; the node is in round 3 now,
            // which v2 leads: it votes for v2's ballot, not for its own value
            REQUIRE(scp.getLeaderRound(0) == 3);
            REQUIRE(scp.mEnvs.size() == 1);
            verifyPrepare(scp.mEnvs[0], v0SecretKey, qSetHash0, 0, z3);
            REQUIRE(!scp.hasLeaderTimer());
        }
        SECTION("the slot externalized meanwhile")
        {
            scp.mMakeValueOverride = [&]() {
                for (auto const& k :
                     {v1SecretKey, v2SecretKey, v3SecretKey, v4SecretKey})
                {
                    scp.receiveEnvelope(makeExternalize(k, qSetHash, 0, y1, 1));
                }
                return xW;
            };
            scp.startSlot(0, xValue);
            REQUIRE(scp.mExternalizedValues[0] == yValue);
            for (auto const& e : scp.mEnvs)
            {
                // never voted for its own value
                auto vals = Slot::getStatementValues(e.statement);
                REQUIRE(std::find(vals.begin(), vals.end(), xValue) ==
                        vals.end());
            }
            REQUIRE(!scp.hasLeaderTimer());
        }
    }

    SECTION("no schedule known: neither proposes nor adopts")
    {
        // e.g. the slot is not the next ledger to close
        REQUIRE(!scp.startSlot(0, xValue));
        scp.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
        REQUIRE(scp.mEnvs.empty());
        REQUIRE(scp.mValuesBuilt == 0);
    }

    SECTION("non-validators never vote for a leader's ballot")
    {
        TestSCP watcher(v0SecretKey.getPublicKey(), qSet, false);
        watcher.storeQuorumSet(std::make_shared<SCPQuorumSet>(qSet));
        watcher.mRoundLeader = [&](uint64, uint32) { return v1NodeID; };
        watcher.receiveEnvelope(makePrepare(v1SecretKey, qSetHash, 0, x1));
        REQUIRE(watcher.mEnvs.empty());
        REQUIRE(watcher.getLeaderRound(0) == 0);
        REQUIRE(!watcher.hasLeaderTimer());
    }

    SECTION("nomination statements are dropped")
    {
        schedule = {v1NodeID, v2NodeID, v0NodeID, v3NodeID, v4NodeID};
        REQUIRE(scp.receiveEnvelope(
                    makeNominate(v1SecretKey, qSetHash, 0, {xValue}, {})) ==
                SCP::EnvelopeState::INVALID);
        REQUIRE(scp.mSCP.getLatestMessage(v1NodeID) == nullptr);
        REQUIRE(scp.mEnvs.empty());
    }
}

namespace
{
// A small network of TestSCP nodes on one slot, driven by a virtual clock:
// messages are delayed, reordered and (for a while) lost; some nodes are dead;
// nodes start the slot at different times.
struct LeaderRoundsModel
{
    struct Node
    {
        SecretKey mKey;
        std::unique_ptr<TestSCP> mSCP;
        bool mAlive{true};
        bool mStarted{false};
        std::chrono::milliseconds mStartTime{0};
        size_t mEnvsSeen{0};
        Value mValue;
    };

    struct Delivery
    {
        std::chrono::milliseconds mWhen;
        uint64_t mSeq;
        size_t mTo;
        SCPEnvelope mEnv;
        bool
        operator>(Delivery const& o) const
        {
            return std::tie(mWhen, mSeq) > std::tie(o.mWhen, o.mSeq);
        }
    };

    stellar_default_random_engine mRng;
    std::vector<Node> mNodes;
    std::vector<NodeID> mSchedule;
    std::priority_queue<Delivery, std::vector<Delivery>, std::greater<>> mQueue;
    uint64_t mSeq{0};
    std::chrono::milliseconds mNow{0};
    // until then, messages are lost with probability mLossPercent and can be
    // delayed up to mMaxChaosDelay
    std::chrono::milliseconds mChaosEnd{0};
    uint32_t mLossPercent{0};
    std::chrono::milliseconds mMaxChaosDelay{0};
    std::chrono::milliseconds mNextRebroadcast{0};
    size_t mThreshold{0};

    LeaderRoundsModel(uint32_t seed, size_t numNodes, size_t numDead)
        : mRng(seed)
    {
        mThreshold = numNodes - (numNodes - 1) / 3;
        releaseAssert(numNodes - numDead >= mThreshold);
        SCPQuorumSet qSet;
        qSet.threshold = static_cast<uint32>(mThreshold);
        for (size_t i = 0; i < numNodes; i++)
        {
            Node n;
            n.mKey = SecretKey::fromSeed(
                sha256(fmt::format("LEADER_MODEL_NODE_{}", i)));
            qSet.validators.push_back(n.mKey.getPublicKey());
            // distinct proposals
            n.mValue = xdr::xdr_to_opaque(
                sha256(fmt::format("LEADER_MODEL_VALUE_{}_{}", seed, i)));
            mNodes.emplace_back(std::move(n));
        }
        for (auto const& n : mNodes)
        {
            mSchedule.emplace_back(n.mKey.getPublicKey());
        }
        stellar::shuffle(mSchedule.begin(), mSchedule.end(), mRng);

        // dead nodes are picked from the front of the schedule half of the
        // time (so the first leaders are dead), at random otherwise
        std::vector<size_t> order(numNodes);
        std::iota(order.begin(), order.end(), 0);
        if (rand_uniform<int>(0, 1, mRng) == 0)
        {
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                auto pos = [&](size_t i) {
                    return std::find(mSchedule.begin(), mSchedule.end(),
                                     mNodes[i].mKey.getPublicKey()) -
                           mSchedule.begin();
                };
                return pos(a) < pos(b);
            });
        }
        else
        {
            stellar::shuffle(order.begin(), order.end(), mRng);
        }
        for (size_t i = 0; i < numDead; i++)
        {
            mNodes[order[i]].mAlive = false;
        }

        mChaosEnd =
            std::chrono::milliseconds(rand_uniform<int>(0, 30000, mRng));
        mLossPercent = rand_uniform<uint32_t>(0, 40, mRng);
        mMaxChaosDelay =
            std::chrono::milliseconds(rand_uniform<int>(10, 4000, mRng));
        mNextRebroadcast = mChaosEnd;

        for (auto& n : mNodes)
        {
            n.mSCP = std::make_unique<TestSCP>(n.mKey.getPublicKey(), qSet);
            n.mSCP->mRoundLeader = [this](uint64, uint32 round) {
                return mSchedule[(round - 1) % mSchedule.size()];
            };
            // most nodes start close together; some are late
            n.mStartTime = std::chrono::milliseconds(
                rand_uniform<int>(0, 3, mRng) == 0
                    ? rand_uniform<int>(0, 8000, mRng)
                    : rand_uniform<int>(0, 300, mRng));
        }
    }

    void
    send(size_t from, SCPEnvelope const& env)
    {
        for (size_t to = 0; to < mNodes.size(); to++)
        {
            if (to == from || !mNodes[to].mAlive)
            {
                continue;
            }
            bool chaos = mNow < mChaosEnd;
            if (chaos && rand_uniform<uint32_t>(0, 99, mRng) < mLossPercent)
            {
                continue;
            }
            auto maxDelay =
                chaos ? mMaxChaosDelay : std::chrono::milliseconds(150);
            auto delay = std::chrono::milliseconds(
                rand_uniform<int>(1, static_cast<int>(maxDelay.count()), mRng));
            mQueue.push(Delivery{mNow + delay, mSeq++, to, env});
        }
    }

    // forwards whatever the nodes emitted since the last call
    void
    flush()
    {
        for (size_t i = 0; i < mNodes.size(); i++)
        {
            auto& n = mNodes[i];
            while (n.mEnvsSeen < n.mSCP->mEnvs.size())
            {
                auto env = n.mSCP->mEnvs[n.mEnvsSeen++];
                send(i, env);
            }
        }
    }

    bool
    allExternalized() const
    {
        for (auto const& n : mNodes)
        {
            if (n.mAlive && n.mSCP->mExternalizedValues.empty())
            {
                return false;
            }
        }
        return true;
    }

    // runs until every live node externalized or `limit` (virtual time);
    // returns true in the first case
    bool
    run(std::chrono::milliseconds limit)
    {
        while (!allExternalized())
        {
            // next event: a start, a delivery, a timer or a rebroadcast
            enum class Kind
            {
                NONE,
                START,
                DELIVERY,
                TIMER,
                REBROADCAST
            };
            Kind kind = Kind::REBROADCAST;
            auto when = mNextRebroadcast;
            size_t node = 0;
            int timerID = 0;
            for (size_t i = 0; i < mNodes.size(); i++)
            {
                auto& n = mNodes[i];
                if (!n.mAlive)
                {
                    continue;
                }
                if (!n.mStarted && n.mStartTime < when)
                {
                    kind = Kind::START;
                    when = n.mStartTime;
                    node = i;
                }
                for (auto const& t : n.mSCP->mTimers)
                {
                    if (t.second.mCallback && t.second.mAbsoluteTimeout < when)
                    {
                        kind = Kind::TIMER;
                        when = t.second.mAbsoluteTimeout;
                        node = i;
                        timerID = t.first;
                    }
                }
            }
            if (!mQueue.empty() && mQueue.top().mWhen < when)
            {
                kind = Kind::DELIVERY;
                when = mQueue.top().mWhen;
            }
            if (when > limit)
            {
                return false;
            }
            mNow = std::max(mNow, when);
            for (auto& n : mNodes)
            {
                n.mSCP->mCurrentTimerOffset = mNow;
            }
            switch (kind)
            {
            case Kind::START:
            {
                auto& n = mNodes[node];
                n.mStarted = true;
                n.mSCP->startSlot(0, n.mValue);
            }
            break;
            case Kind::TIMER:
            {
                auto& td = mNodes[node].mSCP->mTimers[timerID];
                auto cb = td.mCallback;
                td.mCallback = nullptr;
                cb();
            }
            break;
            case Kind::DELIVERY:
            {
                auto d = mQueue.top();
                mQueue.pop();
                mNodes[d.mTo].mSCP->receiveEnvelope(d.mEnv);
            }
            break;
            case Kind::REBROADCAST:
            {
                // like Herder, nodes periodically re-send their latest
                // statement
                mNextRebroadcast = mNow + std::chrono::milliseconds(2000);
                for (size_t i = 0; i < mNodes.size(); i++)
                {
                    auto& n = mNodes[i];
                    if (n.mAlive && !n.mSCP->mEnvs.empty())
                    {
                        send(i, n.mSCP->mEnvs.back());
                    }
                }
            }
            break;
            default:
                releaseAssert(false);
            }
            flush();
        }
        return true;
    }

    void
    checkAgreement() const
    {
        std::optional<Value> externalized;
        for (auto const& n : mNodes)
        {
            auto const& ext = n.mSCP->mExternalizedValues;
            if (ext.empty())
            {
                continue;
            }
            auto const& v = ext.begin()->second;
            if (!externalized)
            {
                externalized = v;
                // it is one of the proposals
                REQUIRE(
                    std::any_of(mNodes.begin(), mNodes.end(),
                                [&](Node const& o) { return o.mValue == v; }));
            }
            REQUIRE(*externalized == v);
        }
    }
};

void
runLeaderRoundsModel(uint32_t firstSeed, uint32_t numSeeds)
{
    for (uint32_t seed = firstSeed; seed < firstSeed + numSeeds; seed++)
    {
        size_t const numNodes = 4 + seed % 4;      // 4..7
        size_t const maxDead = (numNodes - 1) / 3; // 1, 1, 1, 2
        size_t const numDead = (seed / 4) % (maxDead + 1);
        INFO(fmt::format("seed {} nodes {} dead {}", seed, numNodes, numDead));
        LeaderRoundsModel model(seed, numNodes, numDead);
        // Once messages flow again, the slot must close: the bound leaves
        // room for a full turn of the schedule with growing timeouts.
        bool done = model.run(model.mChaosEnd + std::chrono::minutes(10));
        // safety holds no matter what
        model.checkAgreement();
        REQUIRE(done);
    }
}
}

TEST_CASE("leader rounds network model", "[scp][leaderschedule]")
{
    runLeaderRoundsModel(1, 400);
}

TEST_CASE("leader rounds network model long", "[scp][leaderschedule][!hide]")
{
    runLeaderRoundsModel(100000, 20000);
}

}
