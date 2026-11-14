// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"

#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "herder/FilteredEntries.h"
#include "herder/HerderPersistence.h"
#include "herder/HerderUtils.h"
#include "herder/LeaderSchedule.h"
#include "herder/LedgerCloseData.h"
#include "herder/QuorumIntersectionChecker.h"
#include "herder/RustQuorumCheckerAdaptor.h"
#include "herder/TxSetFrame.h"
#include "herder/TxSetUtils.h"
#include "ledger/ImmutableLedgerView.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/P23HotArchiveBug.h"
#include "lib/json/json.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "main/PersistentState.h"
#include "medida/counter.h"
#include "medida/meter.h"
#include "overlay/OverlayIPC.h"
#include "overlay/RustOverlayManager.h"
#include "process/ProcessManager.h"
#include "scp/LocalNode.h"
#include "scp/Slot.h"
#include "simulation/LoadGenerator.h"
#include "transactions/EventManager.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "util/BatchExecutor.h"
#include "util/DebugMetaUtils.h"
#include "util/Decoder.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/MetricsRegistry.h"
#include "util/StatusManager.h"
#include "util/Timer.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-internal.h"
#include "xdrpp/marshal.h"
#include "xdrpp/types.h"
#include <Tracy.hpp>

#include "util/GlobalChecks.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fmt/format.h>
#include <mutex>
#include <unistd.h>

using namespace std;
namespace stellar
{

namespace
{
// Verifies the signatures in `env` that match the master keys of its source,
// operation source and fee source accounts. This populates the
// signature-verification cache that tx-set validation consults with the same
// (key, signature, hash). Other signers need ledger state; validation verifies
// them itself. Returns how many signatures it verified (were not cached).
size_t
verifyMasterKeySignatures(Hash const& networkID, TransactionEnvelope const& env)
{
    // Bound the work per transaction: a signature is only verified against
    // keys whose hint it matches, but colliding hints could otherwise make a
    // crafted transaction cost many verifications.
    size_t budget = 20;
    size_t verified = 0;
    auto verify = [&budget, &verified](Hash const& contentsHash,
                                       auto const& signatures,
                                       std::vector<AccountID> const& keys) {
        for (auto const& sig : signatures)
        {
            for (auto const& key : keys)
            {
                if (!SignatureUtils::doesHintMatch(key.ed25519(), sig.hint))
                {
                    continue;
                }
                if (budget == 0)
                {
                    return;
                }
                --budget;
                auto const res = SignatureUtils::verify(sig, key, contentsHash);
                if (res.cacheResult ==
                    PubKeyUtils::VerifySigCacheLookupResult::MISS)
                {
                    ++verified;
                }
                if (res.valid)
                {
                    break;
                }
            }
        }
    };
    auto keysOf = [](AccountID const& source, auto const& operations) {
        std::vector<AccountID> keys{source};
        for (auto const& op : operations)
        {
            if (op.sourceAccount)
            {
                auto key = toAccountID(*op.sourceAccount);
                if (std::find(keys.begin(), keys.end(), key) == keys.end())
                {
                    keys.emplace_back(std::move(key));
                }
            }
        }
        return keys;
    };

    auto frame = TransactionFrameBase::makeTransactionFromWire(networkID, env);
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
        verify(frame->getContentsHash(), env.v0().signatures,
               keysOf(frame->getSourceID(), env.v0().tx.operations));
        break;
    case ENVELOPE_TYPE_TX:
        verify(frame->getContentsHash(), env.v1().signatures,
               keysOf(frame->getSourceID(), env.v1().tx.operations));
        break;
    case ENVELOPE_TYPE_TX_FEE_BUMP:
    {
        verify(frame->getContentsHash(), env.feeBump().signatures,
               {frame->getFeeSourceID()});
        TransactionEnvelope inner(ENVELOPE_TYPE_TX);
        inner.v1() = env.feeBump().tx.innerTx.v1();
        verified += verifyMasterKeySignatures(networkID, inner);
        break;
    }
    default:
        break;
    }
    return verified;
}

// Whether an envelope's transaction goes into the Soroban phase, as
// TransactionFrame::isSoroban decides it (by the first operation).
bool
isSorobanEnvelope(TransactionEnvelope const& env)
{
    auto const* ops = env.type() == ENVELOPE_TYPE_TX_V0
                          ? &env.v0().tx.operations
                      : env.type() == ENVELOPE_TYPE_TX ? &env.v1().tx.operations
                      : env.type() == ENVELOPE_TYPE_TX_FEE_BUMP
                          ? &env.feeBump().tx.innerTx.v1().tx.operations
                          : nullptr;
    if (ops == nullptr || ops->empty())
    {
        return false;
    }
    switch (ops->front().body.type())
    {
    case INVOKE_HOST_FUNCTION:
    case EXTEND_FOOTPRINT_TTL:
    case RESTORE_FOOTPRINT:
        return true;
    default:
        return false;
    }
}

// The first `windows[phase]` envelopes of each phase (classic, then Soroban;
// a phase without a window is skipped), in mempool order.
std::vector<TransactionEnvelope>
selectPrewarmCandidates(std::vector<TransactionEnvelope>&& envelopes,
                        std::vector<size_t> const& windows)
{
    std::vector<TransactionEnvelope> selected;
    std::vector<size_t> taken(windows.size(), 0);
    for (auto& env : envelopes)
    {
        size_t const phase = isSorobanEnvelope(env) ? 1 : 0;
        if (phase < windows.size() && taken[phase] < windows[phase])
        {
            ++taken[phase];
            selected.push_back(std::move(env));
        }
    }
    return selected;
}

// The candidates a pre-validation works on: the first `windows[phase]`
// transactions of each phase in mempool order, leaving out those of the
// ledger being closed (`closingTxs`, still in the mempool) and those whose
// validity depends on the close time of the ledger they go into (their
// result could not be reused).
std::vector<TransactionFrameBasePtr>
selectPrevalidationCandidates(std::vector<TransactionEnvelope>&& envelopes,
                              std::vector<size_t> const& windows,
                              bool supportsSoroban, Hash const& networkID,
                              UnorderedSet<Hash> const& closingTxs,
                              std::atomic<bool> const& stop)
{
    std::vector<TransactionFrameBasePtr> selected;
    std::vector<size_t> taken(windows.size(), 0);
    for (auto const& env : envelopes)
    {
        if (stop.load(std::memory_order_relaxed))
        {
            break;
        }
        size_t const phase = isSorobanEnvelope(env) ? 1 : 0;
        if (phase >= windows.size() || taken[phase] >= windows[phase] ||
            (phase == 1 && !supportsSoroban) ||
            txValidityDependsOnCloseTime(env))
        {
            continue;
        }
        auto tx = TransactionFrameBase::makeTransactionFromWire(networkID, env);
        if (closingTxs.find(tx->getFullHash()) != closingTxs.end())
        {
            continue;
        }
        ++taken[phase];
        selected.push_back(std::move(tx));
    }
    return selected;
}

// The number of transactions in each phase of a generalized tx set.
std::vector<size_t>
txCountPerPhase(TxSetXDRFrame const& txSet)
{
    std::vector<size_t> counts;
    if (!txSet.isGeneralizedTxSet())
    {
        return counts;
    }
    for (auto const& phase : txSet.getGeneralizedXDR().v1TxSet().phases)
    {
        size_t count = 0;
        if (phase.v() == 0)
        {
            for (auto const& component : phase.v0Components())
            {
                count += component.txsMaybeDiscountedFee().txs.size();
            }
        }
        else
        {
            for (auto const& stage :
                 phase.parallelTxsComponent().executionStages)
            {
                for (auto const& cluster : stage)
                {
                    count += cluster.size();
                }
            }
        }
        counts.push_back(count);
    }
    return counts;
}

} // namespace

// Roughly ~10 minutes of consensus
constexpr uint32 const CLOSE_TIME_DRIFT_LEDGER_WINDOW_SIZE = 120;
// 10 seconds of drift threshold
constexpr std::chrono::seconds const CLOSE_TIME_DRIFT_SECONDS_THRESHOLD{10};

std::unique_ptr<Herder>
Herder::create(Application& app)
{
    return std::make_unique<HerderImpl>(app);
}

HerderImpl::SCPMetrics::SCPMetrics(Application& app)
    : mLostSync(app.getMetrics().NewMeter({"scp", "sync", "lost"}, "sync"))
    , mEnvelopeEmit(
          app.getMetrics().NewMeter({"scp", "envelope", "emit"}, "envelope"))
    , mEnvelopeReceive(
          app.getMetrics().NewMeter({"scp", "envelope", "receive"}, "envelope"))
    , mCumulativeStatements(app.getMetrics().NewCounter(
          {"scp", "memory", "cumulative-statements"}))
    , mEnvelopeValidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "validsig"}, "envelope"))
    , mEnvelopeInvalidSig(app.getMetrics().NewMeter(
          {"scp", "envelope", "invalidsig"}, "envelope"))
    , mTriggerPrepareStartFallback(app.getMetrics().NewMeter(
          {"scp", "trigger", "prepare-start-fallback"}, "trigger"))
    , mTxSetBuild(app.getMetrics().NewTimer({"herder", "txset", "build"}))
    , mTriggerLateness(
          app.getMetrics().NewTimer({"scp", "trigger", "lateness"}))
    , mTriggerImmediate(
          app.getMetrics().NewMeter({"scp", "trigger", "immediate"}, "trigger"))
    , mPersistSCPState(app.getMetrics().NewTimer({"herder", "scp", "persist"}))
{
}

HerderImpl::HerderImpl(Application& app)
    : mPendingEnvelopes(app, *this)
    , mHerderSCPDriver(app, *this, mUpgrades, mPendingEnvelopes)
    , mLastSlotSaved(0)
    , mTrackingTimer(app)
    , mLastExternalize(app.getClock().now())
    , mTriggerTimer(app)
    , mPrepareTxSetTimer(app)
    , mEarlyTxSetTimer(app)
    , mOutOfSyncTimer(app)
    , mTxSetGarbageCollectTimer(app)
    , mCheckForDeadNodesTimer(app)
    , mApp(app)
    , mLedgerManager(app.getLedgerManager())
    , mSCPMetrics(app)
    , mLastQuorumMapIntersectionState(
          std::make_shared<QuorumMapIntersectionState>(app))
    , mState(Herder::HERDER_BOOTING_STATE)
{
    auto ln = getSCP().getLocalNode();

    mPendingEnvelopes.addSCPQuorumSet(ln->getQuorumSetHash(),
                                      ln->getQuorumSet());

    // Note: Rust overlay is now managed by RustOverlayManager
    // SCP broadcasts go through getOverlayManager().broadcastMessage()
}

HerderImpl::~HerderImpl()
{
}

Herder::State
HerderImpl::getState() const
{
    return mState;
}

uint32_t
HerderImpl::getMaxClassicTxSize() const
{
#ifdef BUILD_TESTS
    if (mMaxClassicTxSize)
    {
        return *mMaxClassicTxSize;
    }
#endif
    return MAX_CLASSIC_TX_SIZE_BYTES;
}

uint32_t
HerderImpl::getFlowControlExtraBuffer() const
{
#ifdef BUILD_TESTS
    if (mFlowControlExtraBuffer)
    {
        return *mFlowControlExtraBuffer;
    }
#endif
    return FLOW_CONTROL_BYTES_EXTRA_BUFFER;
}

void
HerderImpl::setTrackingSCPState(uint64_t index, StellarValue const& value,
                                bool isTrackingNetwork)
{
    mTrackingSCP = ConsensusData{index, getConsensusTime(value)};
    if (isTrackingNetwork)
    {
        setState(Herder::HERDER_TRACKING_NETWORK_STATE);
    }
    else
    {
        setState(Herder::HERDER_SYNCING_STATE);
    }
}

uint32
HerderImpl::trackingConsensusLedgerIndex() const
{
    releaseAssert(getState() != Herder::State::HERDER_BOOTING_STATE);
    releaseAssert(mTrackingSCP.mConsensusIndex <= UINT32_MAX);

    auto lcl = mLedgerManager.getLastClosedLedgerNum();
    if (lcl > mTrackingSCP.mConsensusIndex)
    {
        std::string msg =
            "Inconsistent state in Herder: LCL is ahead of tracking";
        CLOG_ERROR(Herder, "{}", msg);
        CLOG_ERROR(Herder, "{}", REPORT_INTERNAL_BUG);
        throw std::runtime_error(msg);
    }

    return static_cast<uint32>(mTrackingSCP.mConsensusIndex);
}

ConsensusTime
HerderImpl::trackingConsensusCloseTime() const
{
    releaseAssert(getState() != Herder::State::HERDER_BOOTING_STATE);
    return mTrackingSCP.mConsensusCloseTime;
}

void
HerderImpl::setState(State st)
{
    bool initState = st == HERDER_BOOTING_STATE;
    if (initState && (mState == HERDER_TRACKING_NETWORK_STATE ||
                      mState == HERDER_SYNCING_STATE))
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("Invalid state transition in Herder: {} -> {}"),
            getStateHuman(mState), getStateHuman(st)));
    }
    mState = st;
}

void
HerderImpl::lostSync()
{
    discardPreparedTxSet();
    mHerderSCPDriver.stateChanged();
    setState(Herder::State::HERDER_SYNCING_STATE);
    // Without the network's latest ledger this node does not know who leads.
    updateTxFloodTargets();
}

void
HerderImpl::updateTxSetTree()
{
    auto const& cfg = mApp.getConfig();
    uint8_t const mode = cfg.txSetTreeMode();
    if (mode == 0 || mApp.isStopping())
    {
        // Off: the overlay is never told, and pushes as before.
        return;
    }
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    uint32_t const next = lcl.header.ledgerSeq + 1;
    // The nodes of the trees are the nodes of the leader schedule (the
    // transitive quorum).
    auto const& nodes = mHerderSCPDriver.getLeaderSchedule(next);
    if (nodes.empty())
    {
        return;
    }
    // Who may root a tree: the leaders of the slot's first rounds. A node
    // relays only for those, so that no other validator can have its frames
    // passed on. (A set pushed by the leader of a later round is not
    // relayed; the nodes ask for it after their wait.)
    size_t const ROUNDS = 3;
    std::vector<NodeID> nextLeaders;
    for (uint32_t round = 1; round <= ROUNDS; ++round)
    {
        auto leader = mHerderSCPDriver.getRoundLeader(next, round);
        if (leader && std::find(nextLeaders.begin(), nextLeaders.end(),
                                *leader) == nextLeaders.end())
        {
            nextLeaders.emplace_back(*leader);
        }
    }
    // The slot after: the node that closes the next ledger may keep leading
    // (its term), or a new order starts, seeded with the last closed
    // ledger's hash (see LeaderSchedule.h).
    std::vector<NodeID> afterLeaders = nextLeaders;
    auto const fresh = computeLeaderSchedule(lcl.hash, nodes);
    for (size_t i = 0; i < std::min(ROUNDS, fresh.size()); ++i)
    {
        if (std::find(afterLeaders.begin(), afterLeaders.end(), fresh[i]) ==
            afterLeaders.end())
        {
            afterLeaders.emplace_back(fresh[i]);
        }
    }
    // The seeds are those of the leader schedules: the hash of the ledger
    // before the last closed one for the next slot, and the last closed
    // ledger's own for the slot after.
    std::vector<OverlayIPC::TxSetTreeSlot> const slots{
        {next, lcl.header.previousLedgerHash, std::move(nextLeaders)},
        {next + 1, lcl.hash, std::move(afterLeaders)}};
    // For tests and fault runs: the validators with the smallest node IDs
    // withhold what they should relay. The overlay is told with the top bit
    // of the mode byte.
    uint8_t withhold = 0;
    if (auto const withholders =
            cfg.EXPERIMENTAL_TX_SET_TREE_WITHHOLDERS_FOR_TESTING;
        withholders > 0)
    {
        auto const& self = cfg.NODE_SEED.getPublicKey();
        size_t const smaller =
            std::count_if(nodes.begin(), nodes.end(),
                          [&](NodeID const& node) { return node < self; });
        bool const listed =
            std::find(nodes.begin(), nodes.end(), self) != nodes.end();
        if (listed && smaller < withholders)
        {
            withhold = 0x80;
        }
    }
    mApp.getOverlayManager().setTxSetTree(
        static_cast<uint8_t>(mode | withhold),
        static_cast<uint8_t>(
            std::min<uint32_t>(cfg.EXPERIMENTAL_TX_SET_TREE_FANOUT, UINT8_MAX)),
        slots, nodes);
}

std::optional<std::vector<NodeID>>
HerderImpl::computeTxFloodTargets()
{
    releaseAssert(threadIsMain());
    if (mApp.getConfig().TX_FLOOD_LEGACY_ADVERTS || !isTracking() ||
        !mLedgerManager.isSynced())
    {
        return std::nullopt;
    }
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    // The schedule is only known for the slot after the LCL. While this
    // node applies the ledger the network just externalized that is still
    // the slot it knows; further behind, its leaders are history.
    if (trackingConsensusLedgerIndex() >
            static_cast<uint64_t>(lcl.header.ledgerSeq) + 1 ||
        buildsLegacyTxSet(lcl.header.ledgerVersion))
    {
        return std::nullopt;
    }
    uint64_t const slot = static_cast<uint64_t>(lcl.header.ledgerSeq) + 1;
    // A node that has not started the slot (or does not run SCP at all, like
    // a watcher) is at round 0: the first round's leader comes first.
    uint64_t const round = std::max<uint64_t>(1, getSCP().getLeaderRound(slot));
    uint64_t const leaders = mApp.getConfig().TX_FLOOD_TARGET_LEADERS;
    if (leaders == 0)
    {
        // Every validator of the schedule, in an order that does not change
        // from slot to slot (the overlay is only told when the list
        // changes).
        std::vector<NodeID> everyone = mHerderSCPDriver.getLeaderSchedule(slot);
        if (everyone.empty())
        {
            return std::nullopt;
        }
        std::sort(everyone.begin(), everyone.end());
        return everyone;
    }
    std::vector<NodeID> targets;
    // 1. The leaders of the node's current round of the next slot and of
    //    the rounds after it.
    for (uint64_t r = round; r < round + leaders; ++r)
    {
        if (r > UINT32_MAX)
        {
            break;
        }
        auto leader =
            mHerderSCPDriver.getRoundLeader(slot, static_cast<uint32_t>(r));
        if (!leader)
        {
            return std::nullopt;
        }
        if (std::find(targets.begin(), targets.end(), *leader) != targets.end())
        {
            // Once around the schedule: every validator is in.
            break;
        }
        targets.emplace_back(*leader);
    }
    // 2. With leader terms (LeaderSchedule.h) the node that closes the next
    //    ledger leads the one after it too, so the list above already names
    //    it. Not when the slot after next starts a new term (every multiple
    //    of LEADER_TERM_LEDGERS): its first leader is the first node of the
    //    order seeded with the LCL's hash, known from now on. That node
    //    builds its set, and validates its candidates before that, while
    //    the next ledger is still in consensus: it has to receive the
    //    transactions a ledger ahead. (A prediction: if the next ledger
    //    costs that very node its turn, another node leads, and gets the
    //    transactions when the ledger closes.)
    if ((slot + 1) % LEADER_TERM_LEDGERS == 0)
    {
        auto const next = computeLeaderSchedule(
            lcl.hash, mHerderSCPDriver.getLeaderSchedule(slot));
        if (!next.empty() && std::find(targets.begin(), targets.end(),
                                       next.front()) == targets.end())
        {
            targets.emplace_back(next.front());
        }
    }
    if (targets.empty())
    {
        return std::nullopt;
    }
    return targets;
}

void
HerderImpl::updateTxFloodTargets()
{
    if (mApp.isStopping())
    {
        // Also reached from posted callbacks.
        return;
    }
    if (isTracking())
    {
        auto const next =
            static_cast<uint64_t>(mLedgerManager.getLastClosedLedgerNum()) + 1;
        if (trackingConsensusLedgerIndex() <= next &&
            getSCP().getLeaderRound(next) == UINT32_MAX)
        {
            // The slot is decided for this node by a v-blocking set that
            // committed (a node that got there through its own ballot keeps
            // its round) and no round leads it any more. Whatever is
            // pending goes to the next slot's leaders when this ledger
            // closes. (A node that is further behind than that goes on: it
            // has to learn that it no longer knows the leaders.)
            return;
        }
    }
    auto targets = computeTxFloodTargets();
    if (targets == mTxFloodTargets)
    {
        return;
    }
    if (targets)
    {
        std::string names;
        for (auto const& node : *targets)
        {
            names += (names.empty() ? "" : ", ") +
                     mApp.getConfig().toShortString(node);
        }
        CLOG_DEBUG(Herder, "Transactions now go to: {}", names);
    }
    else
    {
        CLOG_DEBUG(Herder, "Transactions now go to every peer");
    }
    if (!mApp.getOverlayManager().setTxFloodTargets(targets))
    {
        // Not delivered (no overlay connection): try again at the next
        // change rather than assume the overlay knows.
        return;
    }
#ifdef BUILD_TESTS
    mTxFloodTargetHistoryForTesting.push_back(
        describeTxFloodTargetsForTesting(targets));
    if (mTxFloodTargetHistoryForTesting.size() > 4096)
    {
        mTxFloodTargetHistoryForTesting.pop_front();
    }
#endif
    mTxFloodTargets = std::move(targets);
}

#ifdef BUILD_TESTS
HerderImpl::TxFloodTargetUpdate
HerderImpl::describeTxFloodTargetsForTesting(
    std::optional<std::vector<NodeID>> targets)
{
    TxFloodTargetUpdate res;
    res.lcl = mLedgerManager.getLastClosedLedgerHeader();
    res.slot = static_cast<uint64_t>(res.lcl.header.ledgerSeq) + 1;
    res.round = isTracking() ? getSCP().getLeaderRound(res.slot) : 0;
    res.targets = std::move(targets);
    return res;
}
#endif

namespace
{
// The source account and sequence number of an envelope (the inner
// transaction's for a fee bump), as TransactionFrame reports them.
std::pair<AccountID, SequenceNumber>
envelopeSourceAndSeq(TransactionEnvelope const& env)
{
    switch (env.type())
    {
    case ENVELOPE_TYPE_TX_V0:
    {
        AccountID res;
        res.ed25519() = env.v0().tx.sourceAccountEd25519;
        return {res, env.v0().tx.seqNum};
    }
    case ENVELOPE_TYPE_TX:
        return {toAccountID(env.v1().tx.sourceAccount), env.v1().tx.seqNum};
    case ENVELOPE_TYPE_TX_FEE_BUMP:
    {
        auto const& inner = env.feeBump().tx.innerTx.v1().tx;
        return {toAccountID(inner.sourceAccount), inner.seqNum};
    }
    default:
        // TransactionFrameBase::makeTransactionFromWire aborts here too.
        abort();
    }
}
}

void
HerderImpl::forgetSettledLocalPendingTxs()
{
    auto const lclSeq = mLedgerManager.getLastClosedLedgerNum();
    for (auto it = mLocalPendingTxs.begin(); it != mLocalPendingTxs.end();)
    {
        if (it->second.externalizedIn != 0 &&
            it->second.externalizedIn <= lclSeq)
        {
            it = mLocalPendingTxs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void
HerderImpl::forgetLocalPendingTxs(std::vector<Hash> const& txHashes)
{
    if (mLocalPendingTxs.empty() || txHashes.empty())
    {
        return;
    }
    UnorderedSet<Hash> hashes(txHashes.begin(), txHashes.end());
    for (auto it = mLocalPendingTxs.begin(); it != mLocalPendingTxs.end();)
    {
        if (hashes.find(it->second.fullHash) != hashes.end())
        {
            it = mLocalPendingTxs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

TxSubmitStatus
HerderImpl::checkTxAdmission(TransactionFrameBasePtr const& tx,
                             bool skipValidityCheck)
{
    releaseAssert(threadIsMain());
    auto& metrics = mApp.getMetrics();
    // Overlay-only mode (load tests that skip apply) switches off the
    // sequence-number check and the tx set's one-per-account rule; the
    // one-per-account rule here goes with them.
    bool const trackPending =
#ifdef BUILD_TESTS
        !mApp.getRunInOverlayOnlyMode() &&
#endif
        // The overlay drops fee bumps (unsupported), so one would block its
        // account here until the record times out.
        tx->getEnvelope().type() != ENVELOPE_TYPE_TX_FEE_BUMP;
    auto const now = mApp.getClock().now();
    if (trackPending)
    {
        auto it = mLocalPendingTxs.find(tx->getSourceID());
        if (it != mLocalPendingTxs.end() &&
            now - it->second.acceptedAt >= LOCAL_TX_PENDING_TIMEOUT)
        {
            mLocalPendingTxs.erase(it);
            it = mLocalPendingTxs.end();
        }
        if (it != mLocalPendingTxs.end())
        {
            // The record is normally settled by the set that includes the
            // transaction or another one of its account. The sequence number
            // can also be used up without either (a BumpSequence operation in
            // someone else's transaction, a transaction the overlay lost):
            // then the pending one is dead, and the account is free.
            auto account =
                mLedgerManager.getLCLView()->getAccount(tx->getSourceID());
            if (account &&
                account.current().data.account().seqNum >= it->second.seqNum)
            {
                mApp.getOverlayManager().removeTransactions(
                    {it->second.fullHash});
                mLocalPendingTxs.erase(it);
                it = mLocalPendingTxs.end();
            }
        }
        if (it != mLocalPendingTxs.end())
        {
            if (it->second.fullHash == tx->getFullHash())
            {
                return TxSubmitStatus::TX_STATUS_DUPLICATE;
            }
            // One transaction per source account at a time: any number of
            // transactions of one account can each be valid against the
            // LCL, but only one of them can apply.
            metrics.NewMeter({"herder", "tx-admission", "account-busy"}, "tx")
                .Mark();
            return TxSubmitStatus::TX_STATUS_TRY_AGAIN_LATER;
        }
    }
    if (!skipValidityCheck)
    {
        auto timer =
            metrics.NewTimer({"herder", "tx-admission", "check"}).TimeScope();
        // The check the leader's build runs on every candidate, against the
        // same kind of state: this node's last closed ledger.
        auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
        auto ledgerView = mLedgerManager.getLCLView();
        std::optional<uint32_t> validationLedgerSeq;
        if (protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                      ProtocolVersion::V_19))
        {
            validationLedgerSeq = lcl.header.ledgerSeq + 1;
        }
        auto diagnostics = DiagnosticEventManager::createDisabled();
        auto result =
            tx->checkValid(mApp.getAppConnector(), *ledgerView, 0, 0,
                           getUpperBoundCloseTimeOffset(
                               mApp, getApplyTime(lcl.header.scpValue)),
                           diagnostics, validationLedgerSeq);
        if (!result->isSuccess())
        {
            CLOG_DEBUG(Herder, "Rejecting transaction {} from {}: {}",
                       hexAbbrev(tx->getFullHash()),
                       KeyUtils::toShortString(tx->getSourceID()),
                       xdr::xdr_traits<TransactionResultCode>::enum_name(
                           result->getResultCode()));
            metrics.NewMeter({"herder", "tx-admission", "invalid"}, "tx")
                .Mark();
            return TxSubmitStatus::TX_STATUS_ERROR;
        }
    }
    if (trackPending)
    {
        mLocalPendingTxs[tx->getSourceID()] =
            LocalPendingTx{tx->getFullHash(), tx->getSeqNum(), now, 0};
    }
    return TxSubmitStatus::TX_STATUS_PENDING;
}

std::vector<Hash>
HerderImpl::settleLocalPendingTxs(TxSetXDRFrame const& txSet,
                                  uint32_t txSetLedgerSeq)
{
    std::vector<Hash> stale;
    if (mLocalPendingTxs.empty())
    {
        return stale;
    }
    txSet.forEachTransactionEnvelope([&](TransactionEnvelope const& env) {
        auto const [source, seqNum] = envelopeSourceAndSeq(env);
        auto it = mLocalPendingTxs.find(source);
        if (it == mLocalPendingTxs.end() || seqNum < it->second.seqNum)
        {
            // Not an account with a pending transaction here, or a
            // transaction that comes before it.
            return;
        }
        // The account's sequence number will be at or past the pending
        // transaction's once this set is applied: it is this one (the
        // overlay drops it with the rest of the set), or it can no longer
        // apply. The record itself stays until the ledger is closed
        // (forgetSettledLocalPendingTxs): submissions are checked against
        // the LCL, which still shows the old sequence number until then, so
        // a retry of this transaction would pass as new.
        stale.emplace_back(it->second.fullHash);
        it->second.externalizedIn = txSetLedgerSeq;
    });
    auto const now = mApp.getClock().now();
    for (auto it = mLocalPendingTxs.begin(); it != mLocalPendingTxs.end();)
    {
        if (now - it->second.acceptedAt >= LOCAL_TX_PENDING_TIMEOUT)
        {
            it = mLocalPendingTxs.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return stale;
}

SCP&
HerderImpl::getSCP()
{
    return mHerderSCPDriver.getSCP();
}

void
HerderImpl::syncMetrics()
{
    int64_t count = getSCP().getCumulativeStatemtCount();
    mSCPMetrics.mCumulativeStatements.set_count(count);
    TracyPlot("scp.memory.cumulative-statements", count);
}

std::string
HerderImpl::getStateHuman(State st) const
{
    static std::array<char const*, HERDER_NUM_STATE> stateStrings = {
        "HERDER_BOOTING_STATE", "HERDER_SYNCING_STATE",
        "HERDER_TRACKING_NETWORK_STATE"};
    return std::string(stateStrings[st]);
}

void
HerderImpl::bootstrap()
{
    CLOG_INFO(Herder, "Force joining SCP with local state");
    releaseAssert(getSCP().isValidator());
    releaseAssert(mApp.getConfig().FORCE_SCP);

    mLedgerManager.moveToSynced();
    mHerderSCPDriver.bootstrap();

    setupTriggerNextLedger();
    newSlotExternalized(
        mLedgerManager.getLastClosedLedgerHeader().header.scpValue);
    processSCPQueue(true);
}

void
HerderImpl::newSlotExternalized(StellarValue const& value)
{
    ZoneScoped;
    CLOG_TRACE(Herder, "HerderImpl::newSlotExternalized");

    // start timing next externalize from this point
    mLastExternalize = mApp.getClock().now();

    // perform cleanups
    purgeOldSlots();

    mPendingEnvelopes.forceRebuildQuorum();
}

void
HerderImpl::purgeOldSlots()
{
    ZoneScoped;
    CLOG_TRACE(Herder, "HerderImpl::purgeOldSlots");

    // Evict slots that are outside of our ledger validity bracket
    std::optional<uint32> minSlotToRemember;
    auto minSeq = getMinLedgerSeqToRemember();
    if (minSeq > LedgerManager::GENESIS_LEDGER_SEQ)
    {
        minSlotToRemember = minSeq;
    }

    // Evict slots that are too far in the future (only meaningful when
    // tracking, as that's when we have a reliable "current" slot index)
    std::optional<uint32> maxSlotToRemember;
    if (isTracking())
    {
        maxSlotToRemember =
            nextConsensusLedgerIndex() + LEDGER_VALIDITY_BRACKET;
    }

    if (minSlotToRemember || maxSlotToRemember)
    {
        eraseOutsideRange(minSlotToRemember, maxSlotToRemember);
    }
}

void
HerderImpl::shutdown()
{
    discardPreparedTxSet();
    stopSignaturePrewarm();
    stopProposalPrevalidation(nullptr);
    stopEarlyTxSetSelection();
    mTrackingTimer.cancel();
    mOutOfSyncTimer.cancel();
    mTriggerTimer.cancel();
    mTxSetGarbageCollectTimer.cancel();
    mCheckForDeadNodesTimer.cancel();
}

void
HerderImpl::processExternalized(uint64 slotIndex, StellarValue const& value,
                                bool isLatestSlot)
{
    ZoneScoped;

    releaseAssert(threadIsMain());

    bool validated = getSCP().isSlotFullyValidated(slotIndex);

    CLOG_DEBUG(Herder, "HerderSCPDriver::valueExternalized index: {} txSet: {}",
               slotIndex, hexAbbrev(value.txSetHash));

    if (getSCP().isValidator() && !validated)
    {
        CLOG_WARNING(Herder,
                     "Ledger {} ({}) closed and could NOT be fully "
                     "validated by validator",
                     slotIndex, hexAbbrev(value.txSetHash));
    }

    auto result = mPendingEnvelopes.getTxSet(value.txSetHash);
    TxSetXDRFrameConstPtr externalizedSet;
    if (std::holds_alternative<EmptyTxSet>(result))
    {
        externalizedSet =
            TxSetXDRFrame::makeEmpty(getProposedPreviousLedgerHash(value),
                                     getProposedPreviousLedgerVersion(value));
    }
    else
    {
        externalizedSet = std::get<TxSetXDRFrameConstPtr>(result);
    }
    releaseAssert(externalizedSet != nullptr);

    // Notify overlay to clear TXs from mempool (for RustOverlayManager)
    // Extract TX hashes from the externalized set so Rust can remove them.
    // Hash the envelopes in place: building a frame per transaction would
    // copy every envelope on the main thread only to hash it.
    std::vector<Hash> txHashes;
    if (externalizedSet)
    {
        txHashes = externalizedSet->getTransactionFullHashes();
#ifdef BUILD_TESTS
        mApp.getLoadGenerator().cleanupAccounts(
            static_cast<uint32_t>(slotIndex), *externalizedSet);
#endif
    }
    if (externalizedSet)
    {
        // Pending transactions of this node that the set made stale (their
        // account's sequence number was used by another transaction) are
        // dropped along with the set's own.
        auto stale = settleLocalPendingTxs(*externalizedSet,
                                           static_cast<uint32_t>(slotIndex));
        txHashes.insert(txHashes.end(), stale.begin(), stale.end());
    }
    mApp.getOverlayManager().notifyTxSetExternalized(value.txSetHash, txHashes);

    {
        ZoneNamedN(updateSCPHistoryZone, "update SCP history", true);
        if (slotIndex != 0)
        {
            // Save any new SCP messages received about the previous ledger.
            // NOTE: This call uses an empty `QuorumTracker::QuorumMap` because
            // there is no new quorum map for the previous ledger.
            mApp.getHerderPersistence().saveSCPHistory(
                static_cast<uint32>(slotIndex - 1),
                getSCP().getExternalizingState(slotIndex - 1),
                QuorumTracker::QuorumMap());
        }
        // Store SCP messages received about the current ledger being closed.
        mApp.getHerderPersistence().saveSCPHistory(
            static_cast<uint32>(slotIndex),
            getSCP().getExternalizingState(slotIndex),
            mPendingEnvelopes.getCurrentlyTrackedQuorum());
    }

    // reflect upgrades with the ones included in this SCP round
    {
        bool updated;
        auto newUpgrades = mUpgrades.removeUpgrades(
            value.upgrades.begin(), value.upgrades.end(), getApplyTime(value),
            updated);
        if (updated)
        {
            setUpgrades(newUpgrades);
        }
    }

    // tell the LedgerManager that this value got externalized
    // LedgerManager will perform the proper action based on its internal
    // state: apply, trigger catchup, etc
    LedgerCloseData ledgerData(static_cast<uint32_t>(slotIndex),
                               externalizedSet, value);

    // Only dump the most recent externalized tx set. Ledger sequence on a
    // written tx set shall only strictly move forward; it may have gaps with
    // the emitted debug meta, if the network is ahead of the local node
    // (assumption is that if the network is ahead of the local node, state can
    // be replayed from the archives)
    if (isLatestSlot && mApp.getConfig().METADATA_DEBUG_LEDGERS != 0)
    {
        writeDebugTxSet(ledgerData);
    }

    mLedgerManager.valueExternalized(ledgerData, isLatestSlot);
    // The node may now know of ledgers it has not closed (it is applying,
    // or catching up): the targets depend on how far behind its LCL is. Not
    // from inside the externalize callback (the schedule may need a quorum
    // rebuild).
    mApp.postOnMainThread([this]() { updateTxFloodTargets(); },
                          "Herder: update tx flood targets");
}

void
HerderImpl::writeDebugTxSet(LedgerCloseData const& lcd)
{
    ZoneScoped;

    // Dump latest externalized tx set. Do as much of error-handling as possible
    // to avoid crashing core, since this is used purely for debugging.
    auto path =
        metautils::getLatestTxSetFilePath(mApp.getConfig().BUCKET_DIR_PATH);
    try
    {
        if (fs::mkpath(path.parent_path().string()))
        {
            auto timer = LogSlowExecution(
                "write debug tx set", LogSlowExecution::Mode::AUTOMATIC_RAII,
                "took", std::chrono::milliseconds(100));
            // If we got here, then whatever previous tx set is saved has
            // already been applied, and debug meta has been emitted. Therefore,
            // it's safe to just remove it.
            std::filesystem::remove(path);
            XDROutputFileStream stream(mApp.getClock().getIOContext(),
                                       /*fsyncOnClose=*/false);
            stream.open(path.string());
            stream.writeOne(lcd.toXDR());
        }
        else
        {
            CLOG_WARNING(Ledger,
                         "Failed to make directory '{}' for debug tx set",
                         path.parent_path().string());
        }
    }
    catch (std::runtime_error& e)
    {
        CLOG_WARNING(Ledger, "Failed to dump debug tx set '{}': {}",
                     path.string(), e.what());
    }
}

static void
recordExternalizeAndCheckCloseTimeDrift(
    uint64 slotIndex, StellarValue const& value,
    std::map<uint32_t, std::pair<ConsensusTime, std::optional<ConsensusTime>>>&
        ctMap)
{
    auto it = ctMap.find(slotIndex);
    if (it != ctMap.end())
    {
        it->second.second = getConsensusTime(value);
    }

    if (ctMap.size() >= CLOSE_TIME_DRIFT_LEDGER_WINDOW_SIZE)
    {
        medida::Histogram h(medida::SamplingInterface::SampleType::kSliding);
        for (auto const& [ledgerSeq, closeTimePair] : ctMap)
        {
            auto const& [localCT, externalizedCT] = closeTimePair;
            if (externalizedCT)
            {
                h.Update(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        externalizedCT->toSystemTime() - localCT.toSystemTime())
                        .count());
            }
        }
        auto driftMs =
            static_cast<int64_t>(h.GetSnapshot().get75thPercentile());
        if (std::abs(driftMs) >
            std::chrono::duration_cast<std::chrono::milliseconds>(
                CLOSE_TIME_DRIFT_SECONDS_THRESHOLD)
                .count())
        {
            CLOG_WARNING(Herder, POSSIBLY_BAD_LOCAL_CLOCK);
            CLOG_WARNING(Herder, "Close time local drift is: {} ms", driftMs);
        }

        ctMap.clear();
    }
}

void
HerderImpl::valueExternalized(uint64 slotIndex, StellarValue const& value,
                              bool isLatestSlot)
{
    ZoneScoped;
    int const DUMP_SCP_TIMEOUT_SECONDS = 20;
    // A prewarm for this slot is done serving; the one for the next ledger
    // (started when this slot's ballot did) keeps running.
    if (mSignaturePrewarm && mSignaturePrewarm->ledgerSeq <= slotIndex)
    {
        stopSignaturePrewarm();
    }

    recordExternalizeAndCheckCloseTimeDrift(slotIndex, value,
                                            mDriftCTSlidingWindow);

    if (isLatestSlot)
    {
        // called both here and at the end (this one is in case of an exception)
        trackingHeartBeat();

        // dump SCP information if this ledger took a long time
        auto gap = std::chrono::duration<double>(mApp.getClock().now() -
                                                 mLastExternalize)
                       .count();
        if (gap > DUMP_SCP_TIMEOUT_SECONDS)
        {
            auto slotInfo = getJsonQuorumInfo(getSCP().getLocalNodeID(), false,
                                              false, slotIndex);
            Json::FastWriter fw;
            CLOG_WARNING(Herder, "Ledger took {} seconds, SCP information:{}",
                         gap, fw.write(slotInfo));
        }

        // trigger will be recreated when the ledger is closed
        // we do not want it to trigger while downloading the current set
        // and there is no point in taking a position after the round is over
        mTriggerTimer.cancel();
        discardPreparedTxSet();

        // This call may cause LedgerManager to trigger ledger close
        processExternalized(slotIndex, value, isLatestSlot);

        // Record externalize timing, purge slots outside of our validity
        // bracket, and rebuild quorum now. Processing the SCP queue for the
        // next slot happens later, in lastClosedLedgerIncreased, once the
        // ledger has actually closed.
        newSlotExternalized(value);

        // Check to see if quorums have changed and we need to reanalyze.
        if (mApp.getConfig().USE_QUORUM_INTERSECTION_CHECKER_V2)
        {
            // v2. performs quorum analysis in a separate process that runs the
            // Rust SAT solver.
            checkAndMaybeReanalyzeQuorumMapV2();
        }
        else
        {
            checkAndMaybeReanalyzeQuorumMap();
        }

        // heart beat *after* doing all the work (ensures that we do not include
        // the overhead of externalization in the way we track SCP)
        // Note: this only makes sense in the context of synchronous ledger
        // application on the main thread.
        if (!mApp.getConfig().backgroundLedgerApply())
        {
            trackingHeartBeat();
        }
    }
    else
    {
        // This call may trigger application of buffered ledgers and in some
        // cases a ledger trigger
        processExternalized(slotIndex, value, isLatestSlot);
    }
}

void
HerderImpl::outOfSyncRecovery()
{
    ZoneScoped;

    if (isTracking())
    {
        CLOG_WARNING(Herder,
                     "HerderImpl::outOfSyncRecovery called when tracking");
        return;
    }

    // see if we can shed some data as to speed up recovery
    uint32_t maxSlotsAhead = Herder::LEDGER_VALIDITY_BRACKET;
    uint32 purgeSlot = 0;
    getSCP().processSlotsDescendingFrom(
        std::numeric_limits<uint64>::max(), [&](uint64 seq) {
            if (getSCP().gotVBlocking(seq))
            {
                if (--maxSlotsAhead == 0)
                {
                    purgeSlot = static_cast<uint32>(seq);
                }
            }
            return maxSlotsAhead != 0;
        });
    if (purgeSlot)
    {
        CLOG_INFO(Herder, "Purging slots older than {}", purgeSlot);
        // Only erase old slots. Because we're not tracking, we don't have a
        // reliable "current" slot index to use for determining which future
        // slots to keep.
        eraseOutsideRange(purgeSlot, std::nullopt);
    }
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader().header;
    for (auto const& e : getSCP().getLatestMessagesSend(lcl.ledgerSeq + 1))
    {
        broadcast(e);
    }

    getMoreSCPState();
}

void
HerderImpl::broadcast(SCPEnvelope const& e)
{
    ZoneScoped;
    if (!mApp.getConfig().MANUAL_CLOSE)
    {
        CLOG_DEBUG(Herder, "broadcast  s:{} i:{}", e.statement.pledges.type(),
                   e.statement.slotIndex);

        mSCPMetrics.mEnvelopeEmit.Mark();

        // Route through overlay (RustOverlayManager handles IPC to Rust
        // overlay, OverlayManagerImpl handles built-in overlay in standalone
        // mode)
        auto m = std::make_shared<StellarMessage>();
        m->type(SCP_MESSAGE);
        m->envelope() = e;
        mApp.getOverlayManager().broadcastMessage(m);
    }
}

void
HerderImpl::startOutOfSyncTimer()
{
    if (mApp.getConfig().MANUAL_CLOSE && mApp.getConfig().RUN_STANDALONE)
    {
        return;
    }

    mOutOfSyncTimer.expires_from_now(Herder::OUT_OF_SYNC_RECOVERY_TIMER);

    mOutOfSyncTimer.async_wait(
        [&]() {
            outOfSyncRecovery();
            startOutOfSyncTimer();
        },
        &VirtualTimer::onFailureNoop);
}

void
HerderImpl::emitEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    uint64 slotIndex = envelope.statement.slotIndex;

    CLOG_DEBUG(Herder, "emitEnvelope s:{} i:{} a:{}",
               envelope.statement.pledges.type(), slotIndex,
               mApp.getStateHuman());

    persistSCPState(slotIndex);

    broadcast(envelope);

    // Before the execution is handed over: its end starts the
    // pre-validation of the next ledger's candidates, unless this node
    // selects that ledger's set itself.
    maybeScheduleEarlyTxSetSelection(envelope);
    maybeStartSpeculativeApply(envelope);
    // A set this node built and has not seen checked: by now its own
    // execution has it, or the check runs in the background.
    resolvePendingTxSetSelfCheck();
}

void
HerderImpl::txSetStoredByOverlay(Hash const& hash)
{
    mTxSetsStoredByOverlay.insert(hash);
}

void
HerderImpl::maybeStartSpeculativeApply(SCPEnvelope const& envelope)
{
    auto const& cfg = mApp.getConfig();
    if (!cfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY ||
        !cfg.backgroundLedgerApply())
    {
        return;
    }

    // The value this node is working on: what it votes to prepare, or to
    // commit. An EXTERNALIZE statement means the real apply is on its way.
    auto const& st = envelope.statement;
    Value const* value = nullptr;
    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
        value = &st.pledges.prepare().ballot.value;
        break;
    case SCP_ST_CONFIRM:
        value = &st.pledges.confirm().ballot.value;
        break;
    default:
        return;
    }

    startSpeculativeApply(st.slotIndex, *value, nullptr);
}

void
HerderImpl::startSpeculativeApply(uint64_t slotIndex, Value const& value,
                                  TxSetXDRFrameConstPtr txSet)
{
    auto const& cfg = mApp.getConfig();
    if (!cfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY ||
        !cfg.backgroundLedgerApply())
    {
        return;
    }
    // Only the ledger right after the LCL can be executed, and only while
    // nothing else is being applied: the ledger apply thread executes it on
    // top of the state the LCL left.
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (slotIndex != lcl.header.ledgerSeq + 1 ||
        !protocolValidatesTxSetsAtApply(lcl.header.ledgerVersion) ||
        !isTracking() || !mLedgerManager.isSynced() ||
        mLedgerManager.isApplying())
    {
        return;
    }
    if (mSpeculativeApplyValue && mSpeculativeApplyValue->first == slotIndex &&
        mSpeculativeApplyValue->second == value)
    {
        // already handed over
        return;
    }

    StellarValue sv;
    if (!toStellarValue(value, sv) || isEmptyTxSetStellarValue(sv))
    {
        // An empty-tx-set value takes no time to apply.
        return;
    }
    if (!txSet)
    {
        auto txSetResult = mPendingEnvelopes.getTxSet(sv.txSetHash);
        if (auto const* received =
                std::get_if<TxSetXDRFrameConstPtr>(&txSetResult))
        {
            txSet = *received;
        }
    }
    if (!txSet || txSet->getContentsHash() != sv.txSetHash ||
        txSet->previousLedgerHash() != lcl.hash)
    {
        return;
    }

    // Executing the ledger starts with validating its transaction set, which
    // verifies the signatures the prewarm has not reached itself, on all
    // cores; a prewarm that is still running would only compete with it.
    if (mSignaturePrewarm && mSignaturePrewarm->ledgerSeq <= slotIndex)
    {
        stopSignaturePrewarm();
    }
    mSpeculativeApplyValue = std::make_pair(slotIndex, value);
    // A pre-validation that is still running was for a build on top of the
    // LCL that this node has not needed (it votes for another leader's
    // value): keep what it found, and leave the cores to the execution.
    validationCacheFor(lcl);
    // A node that may lead the next ledger wants the state this execution
    // leaves, to validate its candidates on while this ledger is in its
    // ballot. A ledger with upgrades is left out: they are applied when the
    // ledger commits, after that state.
    bool const prevalidate =
        sv.upgrades.empty() && getSCP().isValidator() &&
        !buildsLegacyTxSet(lcl.header.ledgerVersion) &&
        mayLeadAfter(slotIndex, sv, /*appliedTransactions=*/true);
    // Executing a ledger starts with the full validation of its set. For a
    // set this node built, that is the verdict every other node will reach
    // when it applies the ledger: if it comes before the proposal (the
    // leader executes a prepared set ahead of the trigger), an invalid set
    // is not proposed.
    bool const ownTxSet = mLastBuiltTxSet &&
                          mLastBuiltTxSet->first == sv.txSetHash &&
                          mLastBuiltTxSet->second == getConsensusTime(sv);
    LedgerManager::SpeculativeApplyExecuted onExecuted;
    if (prevalidate || ownTxSet)
    {
        onExecuted = [this, prevalidate, ownTxSet,
                      ledgerSeq = static_cast<uint32_t>(slotIndex),
                      txSetHash = sv.txSetHash](
                         std::shared_ptr<ExecutedLedgerState const> state) {
            if (ownTxSet && state && state->mTxSetSkipped)
            {
                txSetSelfCheckFailed(txSetHash, ledgerSeq,
                                     "this node's own execution of the "
                                     "ledger found the set invalid");
            }
            if (prevalidate)
            {
                startProposalPrevalidation(std::move(state));
            }
        };
    }
    mLedgerManager.startSpeculativeApply(
        LedgerCloseData(static_cast<uint32_t>(slotIndex), txSet, sv),
        std::move(onExecuted));
}

bool
HerderImpl::mayLeadAfter(uint64_t slotIndex, StellarValue const& value,
                         bool appliedTransactions)
{
    auto const schedule = mHerderSCPDriver.getExpectedLeaderSchedule(
        slotIndex + 1, value, appliedTransactions);
    auto const& self = getSCP().getLocalNodeID();
    return (schedule.size() > 0 && schedule[0] == self) ||
           (schedule.size() > 1 && schedule[1] == self);
}

void
HerderImpl::startProposalPrevalidation(
    std::shared_ptr<ExecutedLedgerState const> state)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    if (!state || mApp.isStopping())
    {
        return;
    }
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    // Only while the executed ledger is still the next one to close and this
    // node is voting for the value that was executed. A ledger that applied
    // nothing ends its leader's term, and a skipped set means the state is
    // not the one the candidates would be applied on.
    StellarValue votedValue;
    if (!isTracking() || state->mLedgerSeq != lcl.header.ledgerSeq + 1 ||
        state->mTxSetSkipped || !state->mValue.upgrades.empty() ||
        !mSpeculativeApplyValue ||
        mSpeculativeApplyValue->first != state->mLedgerSeq ||
        !toStellarValue(mSpeculativeApplyValue->second, votedValue) ||
        !(votedValue == state->mValue) ||
        !mayLeadAfter(state->mLedgerSeq, state->mValue, state->mAppliedTxs > 0))
    {
        return;
    }
    // The node first in line for the next ledger selects its set ahead of
    // time instead (see EarlyTxSetJob): that job validates the same
    // candidates, and the selection is validated again on the closed
    // ledger, so results computed here would only serve a fallback build.
    if (mEarlyTxSetPlan && mEarlyTxSetPlan->planned &&
        mEarlyTxSetPlan->slotIndex == state->mLedgerSeq &&
        mEarlyTxSetPlan->stellarValue == state->mValue)
    {
        return;
    }
    stopProposalPrevalidation(nullptr);
    // The pre-validation verifies the same signatures, and more.
    if (mSignaturePrewarm && mSignaturePrewarm->ledgerSeq > state->mLedgerSeq)
    {
        stopSignaturePrewarm();
    }

    auto& metrics = mApp.getMetrics();
    auto const windows = validTxTargets(lcl.header.ledgerVersion);
    // The executed ledger's transactions are still in the mempool: look
    // past that many more.
    auto payload = [&]() {
        auto timer =
            metrics.NewTimer({"herder", "prevalidate", "pull"}).TimeScope();
        return std::make_shared<std::vector<uint8_t> const>(
            pullTopTransactionsPayload(2 * maxTxSetCandidates() +
                                       state->mTxHashes->size()));
    }();
    uint32_t count = 0;
    if (payload->size() >= 4)
    {
        std::memcpy(&count, payload->data(), 4);
    }
    if (count == 0)
    {
        return;
    }

    size_t const workers = static_cast<size_t>(mApp.getConfig().WORKER_THREADS);
    size_t const tasks = std::max<size_t>(
        1, std::min<size_t>(mApp.getBatchExecutor().preferredTaskCount(),
                            workers > 2 ? workers - 2 : 1));
    auto job = std::make_shared<ProposalPrevalidation>();
    job->ledgerSeq = state->mLedgerSeq;
    job->value = state->mValue;
    job->start = std::chrono::steady_clock::now();
    job->pendingTasks = tasks;
    mProposalPrevalidation = job;
    metrics.NewMeter({"herder", "prevalidate", "started"}, "prevalidation")
        .Mark();
    CLOG_DEBUG(Herder,
               "Pre-validating candidates for ledger {} on the executed state "
               "of ledger {}: {} pulled, {} tasks",
               state->mLedgerSeq + 1, state->mLedgerSeq, count, tasks);

    bool const supportsSoroban = protocolVersionStartsFrom(
        state->mHeader->ledgerVersion, SOROBAN_PROTOCOL_VERSION);
    std::optional<uint32_t> validationLedgerSeq;
    if (protocolVersionStartsFrom(state->mHeader->ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        validationLedgerSeq = state->mLedgerSeq + 1;
    }
    for (size_t task = 0; task < tasks; ++task)
    {
        mApp.postOnBackgroundThread(
            // The network ID by reference to the application's: the
            // transaction frames made below keep a reference to it, and
            // outlive this task.
            [job, state, payload, windows, supportsSoroban, validationLedgerSeq,
             &networkID = mApp.getNetworkID(),
             &appConnector = mApp.getAppConnector(), &metrics, task, tasks]() {
                ZoneScopedN("proposal prevalidation");
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    --job->pendingTasks;
                    if (job->stop.load())
                    {
                        return;
                    }
                    ++job->activeTasks;
                }
                // Results go to the job a few at a time; once it is stopped
                // they are of no use to anyone.
                std::vector<std::pair<Hash, TxValidationResult>> batch;
                auto publish = [&]() {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    if (!job->stop.load())
                    {
                        job->published.insert(
                            job->published.end(),
                            std::make_move_iterator(batch.begin()),
                            std::make_move_iterator(batch.end()));
                    }
                    batch.clear();
                };
                try
                {
                    std::call_once(job->selected, [&]() {
                        // Must not throw out of call_once.
                        try
                        {
                            job->candidates = selectPrevalidationCandidates(
                                OverlayIPC::parseTopTransactions(*payload),
                                windows, supportsSoroban, networkID,
                                *state->mTxHashes, job->stop);
                        }
                        catch (std::exception const& e)
                        {
                            CLOG_WARNING(Herder,
                                         "Proposal pre-validation could not "
                                         "read its candidates: {}",
                                         e.what());
                            job->candidates.clear();
                            std::lock_guard<std::mutex> lock(job->mutex);
                            job->stop.store(true);
                        }
                    });
                    auto const& candidates = job->candidates;
                    auto view = state->makeView(metrics);
                    auto const& header = *state->mHeader;
                    auto diagnostics = DiagnosticEventManager::createDisabled();
                    for (size_t i = task; i < candidates.size(); i += tasks)
                    {
                        if (job->stop.load(std::memory_order_relaxed))
                        {
                            break;
                        }
                        auto const& tx = candidates[i];
                        TxValidationResult result;
                        result.mValid =
                            tx->checkValid(appConnector, *view, 0, 0, 0,
                                           diagnostics, validationLedgerSeq)
                                ->isSuccess();
                        if (result.mValid)
                        {
                            auto feeSource =
                                view->getAccount(tx->getFeeSourceID());
                            if (feeSource)
                            {
                                result.mFeeSourceBalance = getAvailableBalance(
                                    header, feeSource.current());
                            }
                        }
                        batch.emplace_back(tx->getFullHash(), result);
                        if (batch.size() >= 32)
                        {
                            publish();
                        }
                    }
                }
                catch (std::exception const& e)
                {
                    // The build validates whatever has no result. What this
                    // task had not published yet is dropped with it.
                    CLOG_WARNING(Herder, "Proposal pre-validation failed: {}",
                                 e.what());
                    batch.clear();
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->stop.store(true);
                }
                publish();
                std::lock_guard<std::mutex> lock(job->mutex);
                // Complete only if every task ran to its end: a stopped job
                // (by the build, or by a failure) is not.
                if (--job->activeTasks == 0 && job->pendingTasks == 0 &&
                    !job->stop.load())
                {
                    job->finishedAfterNs =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - job->start)
                            .count();
                }
            },
            "Herder: proposal prevalidation");
    }
}

void
HerderImpl::stopProposalPrevalidation(LedgerHeaderHistoryEntry const* lcl)
{
    if (!mProposalPrevalidation)
    {
        return;
    }
    auto job = std::move(mProposalPrevalidation);
    mProposalPrevalidation.reset();
    std::vector<std::pair<Hash, TxValidationResult>> results;
    int64_t finishedAfterNs = -1;
    {
        // Take what the tasks have handed over so far. Nothing is published
        // after this, so there is nothing to wait for: tasks that have not
        // started find `stop` set and leave, running ones after their
        // current transaction.
        std::lock_guard<std::mutex> lock(job->mutex);
        finishedAfterNs = job->finishedAfterNs.load();
        job->stop.store(true);
        results.swap(job->published);
    }
    auto& metrics = mApp.getMetrics();
    metrics.NewTimer({"herder", "prevalidate", "duration"})
        .Update(finishedAfterNs >= 0
                    ? std::chrono::nanoseconds(finishedAfterNs)
                    : std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - job->start));
    metrics.NewMeter({"herder", "prevalidate", "txs"}, "transaction")
        .Mark(results.size());
    if (finishedAfterNs < 0)
    {
        metrics
            .NewMeter({"herder", "prevalidate", "incomplete"}, "prevalidation")
            .Mark();
    }
    // The results describe the ledger that was executed. They hold on top of
    // the LCL only if that is the ledger, closed with the very same value:
    // executing a ledger is deterministic, so the state it was validated on
    // is then the state the LCL left (the value has no upgrades, see
    // maybeStartSpeculativeApply).
    if (!lcl || lcl->header.ledgerSeq != job->ledgerSeq ||
        !(lcl->header.scpValue == job->value) ||
        mValidationCache.lclHash != lcl->hash)
    {
        metrics
            .NewMeter({"herder", "prevalidate", "discarded"}, "prevalidation")
            .Mark();
        return;
    }
    metrics.NewMeter({"herder", "prevalidate", "used"}, "prevalidation").Mark();
    for (auto& r : results)
    {
        mValidationCache.results.emplace(std::move(r));
    }
}

TxValidationCache*
HerderImpl::validationCacheFor(LedgerHeaderHistoryEntry const& lcl)
{
    if (mValidationCache.lclHash != lcl.hash)
    {
        mValidationCache.lclHash = lcl.hash;
        TxValidationCache().swap(mValidationCache.results);
    }
    stopProposalPrevalidation(&lcl);
    return &mValidationCache.results;
}

std::chrono::milliseconds
HerderImpl::expectedTxSetBuildDuration() const
{
    // Until this node has built a set of some size: what a build costs per
    // transaction on the slower of the machines measured (about 0.17 s for
    // 4,000 transactions with most candidates validated ahead of time).
    return std::max(mLastTxSetBuildDuration,
                    expectedTxSetBuildDuration(mLastClosedTxSetSize));
}

std::chrono::milliseconds
HerderImpl::expectedTxSetBuildDuration(size_t txs) const
{
    static std::chrono::nanoseconds const DEFAULT_COST_PER_TX{40'000};
    auto const perTx = mTxSetBuildCostPerTx > std::chrono::nanoseconds::zero()
                           ? mTxSetBuildCostPerTx
                           : DEFAULT_COST_PER_TX;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        perTx * static_cast<int64_t>(txs));
}

std::chrono::milliseconds
HerderImpl::prepareTxSetLeadTime(std::chrono::milliseconds expectedCloseTime,
                                 std::chrono::milliseconds lastBuild)
{
    using std::chrono::milliseconds;
    // What a pushed set needs to reach the peers (measured: about 0.1 s
    // across regions for a 4,000-transaction set).
    milliseconds const pushAllowance{100};
    auto lead =
        std::min<milliseconds>(expectedCloseTime / 4, milliseconds(250));
    if (lastBuild > milliseconds::zero())
    {
        lead = std::max<milliseconds>(lead, lastBuild + lastBuild / 4 +
                                                pushAllowance);
    }
    return std::min<milliseconds>(lead, expectedCloseTime / 2);
}

TxSubmitStatus
HerderImpl::recvTransaction(TransactionFrameBasePtr tx, bool submittedFromSelf
#ifdef BUILD_TESTS
                            ,
                            bool isLoadgenTx
#endif
)
{
    ZoneScoped;
    CLOG_TRACE(Herder, "recv transaction {} for {}",
               hexAbbrev(tx->getFullHash()),
               KeyUtils::toShortString(tx->getSourceID()));

    // This node vouches for what it passes on: the transaction goes to the
    // next leaders only, and they do not check it before they build.
    bool skipValidityCheck = false;
#ifdef BUILD_TESTS
    skipValidityCheck = isLoadgenTx;
#endif
    auto const admission = checkTxAdmission(tx, skipValidityCheck);
    if (admission != TxSubmitStatus::TX_STATUS_PENDING)
    {
        return admission;
    }
#ifdef BUILD_TESTS
    if (submittedFromSelf)
    {
        mLedgerManager.recordTxSubmission(tx->getContentsHash());
    }
#endif

    auto const& env = tx->getEnvelope();
    mApp.getOverlayManager().broadcastTransaction(env, tx->getFullFee(),
                                                  tx->getNumOperations());
    return TxSubmitStatus::TX_STATUS_PENDING;
}

bool
HerderImpl::checkCloseTime(SCPEnvelope const& envelope, bool enforceRecent)
{
    ZoneScoped;
    using std::placeholders::_1;
    auto const& st = envelope.statement;

    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader().header;

    ConsensusTime ctCutoff;
    if (enforceRecent)
    {
        auto const now = mApp.getClock().system_now();
        std::chrono::seconds const maxDrift(
            mApp.getConfig().MAXIMUM_LEDGER_CLOSETIME_DRIFT);
        if (now.time_since_epoch() >= maxDrift)
        {
            ctCutoff = ConsensusTime::fromSystemTime(now - maxDrift,
                                                     lcl.ledgerVersion);
        }
    }

    auto envLedgerIndex = envelope.statement.slotIndex;
    auto& scpD = getHerderSCPDriver();

    auto lastCloseIndex = lcl.ledgerSeq;
    auto lastCloseTime = getConsensusTime(lcl.scpValue);

    // see if we can get a better estimate of lastCloseTime for validating this
    // statement using consensus data: update lastCloseIndex/lastCloseTime to
    // be the highest possible but still be less than envLedgerIndex
    if (getState() != HERDER_BOOTING_STATE)
    {
        auto trackingIndex = trackingConsensusLedgerIndex();
        if (envLedgerIndex >= trackingIndex && trackingIndex > lastCloseIndex)
        {
            lastCloseIndex = static_cast<uint32>(trackingIndex);
            lastCloseTime = trackingConsensusCloseTime();
        }
    }

    StellarValue sv;
    // performs the most conservative check:
    // returns true if one of the values is valid
    auto checkCTHelper = [&](std::vector<Value> const& values) {
        return std::any_of(values.begin(), values.end(), [&](Value const& e) {
            auto r = toStellarValue(e, sv);
            // sv must be after cutoff
            r = r && getConsensusTime(sv) >= ctCutoff;
            if (r)
            {
                // statement received after the fact, only keep externalized
                // value
                r = (lastCloseIndex == envLedgerIndex &&
                     lastCloseTime == getConsensusTime(sv));
                // for older messages, just ensure that they occurred before
                r = r || (lastCloseIndex > envLedgerIndex &&
                          lastCloseTime > getConsensusTime(sv));
                // for future message, perform the same validity check than
                // within SCP
                r = r || scpD.checkCloseTime(envLedgerIndex, lastCloseTime, sv);
            }
            return r;
        });
    };

    bool b;

    switch (st.pledges.type())
    {
    case SCP_ST_NOMINATE:
        b = checkCTHelper(st.pledges.nominate().accepted) ||
            checkCTHelper(st.pledges.nominate().votes);
        break;
    case SCP_ST_PREPARE:
    {
        auto& prep = st.pledges.prepare();
        b = checkCTHelper({prep.ballot.value});
        if (!b && prep.prepared)
        {
            b = checkCTHelper({prep.prepared->value});
        }
        if (!b && prep.preparedPrime)
        {
            b = checkCTHelper({prep.preparedPrime->value});
        }
    }
    break;
    case SCP_ST_CONFIRM:
        b = checkCTHelper({st.pledges.confirm().ballot.value});
        break;
    case SCP_ST_EXTERNALIZE:
        b = checkCTHelper({st.pledges.externalize().commit.value});
        break;
    default:
        abort();
    }

    if (!b)
    {
        CLOG_TRACE(Herder, "Invalid close time processing {}",
                   getSCP().envToStr(st));
    }
    return b;
}

uint32_t
HerderImpl::getMinLedgerSeqToRemember() const
{
    auto maxSlotsToRemember = mApp.getConfig().MAX_SLOTS_TO_REMEMBER;
    auto currSlot = trackingConsensusLedgerIndex();
    if (currSlot > maxSlotsToRemember)
    {
        return (currSlot - maxSlotsToRemember + 1);
    }
    else
    {
        return LedgerManager::GENESIS_LEDGER_SEQ;
    }
}

Herder::EnvelopeStatus
HerderImpl::recvSCPEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    mSCPMetrics.mEnvelopeReceive.Mark();

    if (envelope.statement.pledges.type() == SCP_ST_NOMINATE)
    {
        // There is no nomination phase (see HerderSCPDriver::getRoundLeader).
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    // **** first perform checks that do NOT require signature verification
    // this allows to fast fail messages that we'd throw away anyways

    uint32_t minLedgerSeq = getMinLedgerSeqToRemember();
    uint32_t maxLedgerSeq = std::numeric_limits<uint32>::max();

    if (!checkCloseTime(envelope, false))
    {
        // if the envelope contains an invalid close time, don't bother
        // processing it as we're not going to forward it anyways and it's
        // going to just sit in our SCP state not contributing anything useful.
        CLOG_TRACE(
            Herder,
            "skipping invalid close time (incompatible with current state)");
        std::string txt("DISCARDED - incompatible close time");
        ZoneText(txt.c_str(), txt.size());
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    auto checkpoint = getMostRecentCheckpointSeq();
    auto index = envelope.statement.slotIndex;

    if (isTracking())
    {
        // when tracking, we can filter messages based on the information we got
        // from consensus for the max ledger

        // note that this filtering will cause a node on startup
        // to potentially drop messages outside of the bracket
        // causing it to discard CONSENSUS_STUCK_TIMEOUT_SECONDS worth of
        // ledger closing
        maxLedgerSeq = nextConsensusLedgerIndex() + LEDGER_VALIDITY_BRACKET;
    }
    // Allow message with a drift larger than MAXIMUM_LEDGER_CLOSETIME_DRIFT if
    // it is a checkpoint message. When validator joining from genesis is needed
    // to unstick the network, ignore close time that is too old in order to
    // advance consensus (this usually applies to test networks).
    else if (!checkCloseTime(envelope,
                             trackingConsensusLedgerIndex() <=
                                     LedgerManager::GENESIS_LEDGER_SEQ &&
                                 index != nextConsensusLedgerIndex()) &&
             index != checkpoint)
    {
        // if we've never been in sync, we can be more aggressive in how we
        // filter messages: we can ignore messages that are unlikely to be
        // the latest messages from the network
        CLOG_TRACE(Herder, "recvSCPEnvelope: skipping invalid close time "
                           "(check MAXIMUM_LEDGER_CLOSETIME_DRIFT)");
        std::string txt("DISCARDED - invalid close time");
        ZoneText(txt.c_str(), txt.size());
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    // If envelopes are out of our validity brackets, or if envelope does not
    // contain the checkpoint for early catchup, we just ignore them.
    if ((index > maxLedgerSeq || index < minLedgerSeq) && index != checkpoint)
    {
        CLOG_TRACE(Herder, "Ignoring SCPEnvelope outside of range: {}( {},{})",
                   envelope.statement.slotIndex, minLedgerSeq, maxLedgerSeq);
        std::string txt("DISCARDED - out of range");
        ZoneText(txt.c_str(), txt.size());
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    // **** from this point, we have to check signatures
    if (!verifyEnvelope(envelope))
    {
        std::string txt("DISCARDED - bad envelope");
        ZoneText(txt.c_str(), txt.size());
        CLOG_TRACE(Herder, "Received bad envelope, discarding");
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    if (envelope.statement.nodeID == getSCP().getLocalNode()->getNodeID())
    {
        CLOG_TRACE(Herder, "recvSCPEnvelope: skipping own message");
        std::string txt("SKIPPED_SELF");
        ZoneText(txt.c_str(), txt.size());
        return Herder::ENVELOPE_STATUS_SKIPPED_SELF;
    }

    auto status = mPendingEnvelopes.recvSCPEnvelope(envelope);
    if (status == Herder::ENVELOPE_STATUS_READY)
    {
        std::string txt("READY");
        ZoneText(txt.c_str(), txt.size());
        CLOG_DEBUG(Herder, "recvSCPEnvelope (ready) from: {} s:{} i:{} a:{}",
                   mApp.getConfig().toShortString(envelope.statement.nodeID),
                   envelope.statement.pledges.type(),
                   envelope.statement.slotIndex, mApp.getStateHuman());

        processSCPQueue(true);
    }
    else
    {
        if (status == Herder::ENVELOPE_STATUS_FETCHING)
        {
            std::string txt("FETCHING");
            ZoneText(txt.c_str(), txt.size());
        }
        else if (status == Herder::ENVELOPE_STATUS_PROCESSED)
        {
            std::string txt("PROCESSED");
            ZoneText(txt.c_str(), txt.size());
        }
        CLOG_TRACE(Herder, "recvSCPEnvelope ({}) from: {} s:{} i:{} a:{}",
                   static_cast<int>(status),
                   mApp.getConfig().toShortString(envelope.statement.nodeID),
                   envelope.statement.pledges.type(),
                   envelope.statement.slotIndex, mApp.getStateHuman());
    }
    return status;
}

#ifdef BUILD_TESTS

Herder::EnvelopeStatus
HerderImpl::recvSCPEnvelope(SCPEnvelope const& envelope,
                            SCPQuorumSet const& qset,
                            TxSetXDRFrameConstPtr txset)
{
    ZoneScoped;
    mPendingEnvelopes.addTxSet(txset->getContentsHash(),
                               envelope.statement.slotIndex, txset);
    mPendingEnvelopes.addSCPQuorumSet(xdrSha256(qset), qset);
    return recvSCPEnvelope(envelope);
}

Herder::EnvelopeStatus
HerderImpl::recvSCPEnvelope(SCPEnvelope const& envelope,
                            SCPQuorumSet const& qset,
                            StellarMessage const& txset)
{
    auto txSetFrame =
        txset.type() == TX_SET
            ? TxSetXDRFrame::makeFromWire(txset.txSet())
            : TxSetXDRFrame::makeFromWire(txset.generalizedTxSet());
    return recvSCPEnvelope(envelope, qset, txSetFrame);
}

void
HerderImpl::externalizeValue(TxSetXDRFrameConstPtr txSet, uint32_t ledgerSeq,
                             ConsensusTime closeTime,
                             xdr::xvector<UpgradeType, 6> const& upgrades,
                             std::optional<SecretKey> skToSignValue)
{
    getPendingEnvelopes().putTxSet(txSet->getContentsHash(), ledgerSeq, txSet);
    auto sk = skToSignValue ? *skToSignValue : mApp.getConfig().NODE_SEED;
    StellarValue sv =
        makeStellarValue(txSet->getContentsHash(), closeTime, upgrades, sk);
    getHerderSCPDriver().valueExternalized(ledgerSeq, xdr::xdr_to_opaque(sv));
    while (mApp.getLedgerManager().getLastClosedLedgerNum() < ledgerSeq)
    {
        mApp.getClock().crank(true);
    }
}

#endif

std::vector<SCPEnvelope>
HerderImpl::getSCPStateForPeer(uint32 ledgerSeq)
{
    ZoneScoped;
    std::vector<SCPEnvelope> envelopes;
    auto maxSlots = Herder::LEDGER_VALIDITY_BRACKET;

    // SCP keeps the sequential recent slots plus the most recent checkpoint
    // slot, which may be far behind them. Checkpoint messages are almost
    // always outside MAXIMUM_LEDGER_CLOSETIME_DRIFT and are only accepted
    // once the receiving node knows the network state from the recent
    // messages, so if the oldest slot is such a detached checkpoint, send it
    // *after* everything else.
    std::vector<uint64> slots;
    getSCP().processSlotsAscendingFrom(ledgerSeq, [&](uint64 seq) {
        slots.push_back(seq);
        return true;
    });
    std::optional<uint64> detachedCheckpoint;
    if (slots.size() >= 2 && slots[1] > slots[0] + 1)
    {
        detachedCheckpoint = slots[0];
    }

    auto appendSlot = [&](uint64 seq) {
        bool slotHadData = false;
        getSCP().processCurrentState(
            seq,
            [&](SCPEnvelope const& e) {
                envelopes.push_back(e);
                slotHadData = true;
                return true; // continue
            },
            false);
        return slotHadData;
    };

    // Collect up to maxSlots slots worth of envelopes
    for (auto seq : slots)
    {
        if (detachedCheckpoint && seq == *detachedCheckpoint)
        {
            continue; // appended below
        }
        if (appendSlot(seq))
        {
            --maxSlots;
        }
        if (maxSlots == 0)
        {
            break;
        }
    }
    // After sending the most recent slots, if we have a detached checkpoint,
    // send it at the end
    if (detachedCheckpoint)
    {
        appendSlot(*detachedCheckpoint);
    }

    return envelopes;
}

void
HerderImpl::processSCPQueue(bool synchronous)
{
    ZoneScoped;
    if (isTracking())
    {
        ZoneScoped;
        std::string txt("tracking");
        ZoneText(txt.c_str(), txt.size());

        // process any statements up to the next slot
        // this may cause it to externalize
        auto nextIndex = nextConsensusLedgerIndex();
        if (mApp.getLedgerManager().isApplying())
        {
            nextIndex =
                std::min(nextIndex,
                         mApp.getLedgerManager().getLastClosedLedgerNum() + 1);
        }
        auto processSCPQueueSomeMore = [this, nextIndex] {
            if (mApp.isStopping())
            {
                return;
            }
            processSCPQueueUpToIndex(nextIndex);
            processReadySlotsPastGaps();
        };

        if (synchronous)
        {
            processSCPQueueSomeMore();
        }
        else
        {
            mApp.postOnMainThread(processSCPQueueSomeMore,
                                  "processSCPQueueSomeMore");
        }
    }
    else
    {
        std::string txt("not tracking");
        ZoneText(txt.c_str(), txt.size());
        // we don't know which ledger we're in
        // try to consume the messages from the queue
        // starting from the smallest slot
        for (auto& slot : mPendingEnvelopes.readySlots())
        {
            processSCPQueueUpToIndex(slot);
            if (isTracking())
            {
                // one of the slots externalized
                // we go back to regular flow
                break;
            }
        }
        // The slot that externalized may have no successor on the network
        processReadySlotsPastGaps();
    }
}

// An out of sync node learns the network's state from peers' SCP state
// responses, which carry the most recent slots plus the most recent checkpoint
// slot with a gap between the two that no one will ever fill (see
// getSCPStateForPeer). Each slot is handed to SCP once its tx set is
// available, so if the checkpoint slot's tx set arrives first the node
// externalizes that slot and starts tracking it. Tracking mode then only
// consumes envelopes up to the next slot, which never comes, and the node
// would sit idle until the consensus-stuck timer fires before looking at the
// recent slots. LedgerApplyManager buffers out-of-order ledgers regardless, so
// while out of sync, step over such gaps to the ready slots beyond them.
void
HerderImpl::processReadySlotsPastGaps()
{
    ZoneScoped;
    // Only while out of sync: an in-sync node must wait for the next slot.
    auto outOfSync = [this]() {
        return isTracking() && !mLedgerManager.isSynced() &&
               !mLedgerManager.isApplying() && !mApp.isStopping();
    };
    while (outOfSync())
    {
        auto const nextIndex = nextConsensusLedgerIndex();
        if (mPendingEnvelopes.hasEnvelopesForSlot(nextIndex))
        {
            // The network has, or had, messages for the next slot, so this is
            // not a gap; the regular flow consumes them as they become ready.
            return;
        }
        auto const ready = mPendingEnvelopes.readySlots(); // ascending
        auto it = std::upper_bound(ready.begin(), ready.end(), nextIndex);
        if (it == ready.end())
        {
            return;
        }
        CLOG_INFO(Herder,
                  "Out of sync at slot {} with no SCP messages for slot {}, "
                  "processing ready slot {}",
                  nextIndex - 1, nextIndex, *it);
        // The tracked slot is not the network's latest, so this node is not
        // tracking the network. Record that before handing SCP the later
        // slot: while tracking, the driver rejects values for any slot beyond
        // the next one.
        lostSync();
        processSCPQueueUpToIndex(*it);
        if (!isTracking())
        {
            // Nothing externalized. Stay out of sync and let the recovery
            // timer ask peers for more state, as herderOutOfSync does.
            startOutOfSyncTimer();
            return;
        }
        // Tracking again from the later slot. Slots that became ready while
        // the node was stuck would have been consumed one at a time as they
        // arrived, so drain the ones that now directly follow.
        uint64 before;
        do
        {
            before = nextConsensusLedgerIndex();
            processSCPQueueUpToIndex(before);
        } while (outOfSync() && nextConsensusLedgerIndex() != before);
    }
}

void
HerderImpl::processSCPQueueUpToIndex(uint64 slotIndex)
{
    ZoneScoped;
    while (true)
    {
        SCPEnvelopeWrapperPtr envW = mPendingEnvelopes.pop(slotIndex);
        if (envW)
        {
            auto r = getSCP().receiveEnvelope(envW);
            if (r == SCP::EnvelopeState::VALID)
            {
                auto const& env = envW->getEnvelope();
                auto const& st = env.statement;
                if (st.pledges.type() == SCP_ST_EXTERNALIZE)
                {
                    mHerderSCPDriver.recordSCPExternalizeEvent(
                        st.slotIndex, st.nodeID, false);
                }
                mPendingEnvelopes.envelopeProcessed(env);
            }
        }
        else
        {
            return;
        }
    }
}

#ifdef BUILD_TESTS
PendingEnvelopes&
HerderImpl::getPendingEnvelopes()
{
    return mPendingEnvelopes;
}

Upgrades const&
HerderImpl::getUpgrades() const
{
    return mUpgrades;
}
#endif

std::chrono::milliseconds
HerderImpl::ctValidityOffset(ConsensusTime closeTime,
                             std::chrono::milliseconds maxCtOffset)
{
    auto maxCandidateCt = mApp.getClock().system_now() + maxCtOffset +
                          Herder::MAX_TIME_SLIP_SECONDS;
    auto minCandidateCt = closeTime.toSystemTime();

    if (minCandidateCt > maxCandidateCt)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   minCandidateCt - maxCandidateCt) +
               std::chrono::milliseconds(1);
    }

    return std::chrono::milliseconds::zero();
}

void
HerderImpl::lastClosedLedgerIncreased(bool latest, TxSetXDRFrameConstPtr txSet,
                                      bool upgradeApplied)
{
    releaseAssert(threadIsMain());
    discardPreparedTxSet();
    // What the next build has to be ready for (see
    // expectedTxSetBuildDuration).
    mLastClosedTxSetSize = txSet ? txSet->sizeTxTotal() : 0;
    {
        // A pre-validation is for the ledger that was executed
        // speculatively; if another one closed, its results are of no use.
        auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
        if (mProposalPrevalidation &&
            (mProposalPrevalidation->ledgerSeq != lcl.header.ledgerSeq ||
             !(mProposalPrevalidation->value == lcl.header.scpValue)))
        {
            stopProposalPrevalidation(nullptr);
        }
        mPendingTxSetSelfCheck.reset();
        // The same goes for a set selected ahead of time.
        if (mEarlyTxSetPlan &&
            (mEarlyTxSetPlan->slotIndex != lcl.header.ledgerSeq ||
             !(mEarlyTxSetPlan->stellarValue == lcl.header.scpValue)))
        {
            if (mEarlyTxSetPlan->planned &&
                mEarlyTxSetPlan->slotIndex == lcl.header.ledgerSeq)
            {
                mApp.getMetrics()
                    .NewMeter({"herder", "txset", "early-discarded"}, "txset")
                    .Mark();
            }
            stopEarlyTxSetSelection();
        }
        // Results computed on the previous ledger are of no use either,
        // whether or not this node builds on the new one.
        if (mValidationCache.lclHash != lcl.hash)
        {
            mValidationCache.lclHash = lcl.hash;
            TxValidationCache().swap(mValidationCache.results);
        }
    }
    // The LCL now shows the sequence numbers the closed set used.
    forgetSettledLocalPendingTxs();

    // Ensure potential upgrades are handled in overlay
    maybeHandleUpgrade();

    // If we're in sync and there are no buffered ledgers to apply, trigger next
    // ledger
    if (latest)
    {
        // Re-start heartbeat tracking _after_ applying the most up-to-date
        // ledger. This guarantees out-of-sync timer won't fire while we have
        // ledgers to apply (applicable during parallel ledger apply).
        trackingHeartBeat();

        // Ensure out of sync recovery did not get triggered while we were
        // applying
        releaseAssert(isTracking());
        releaseAssert(trackingConsensusLedgerIndex() ==
                      mLedgerManager.getLastClosedLedgerNum());
        releaseAssert(mLedgerManager.isSynced());

        setupTriggerNextLedger();
        // The new ledger's hash seeds the next slot's leader schedule: the
        // transactions this node holds go to the new leaders now.
        updateTxFloodTargets();
        updateTxSetTree();

        // Now that the new ledger is closed, process any already-buffered SCP
        // envelopes for the next slot. Posted to the main thread so control
        // returns to the caller first. Note: slot purging happens earlier, at
        // externalize time, so that SCP state doesn't accumulate while the
        // node is applying or catching up.
        processSCPQueue(false);
    }
}

VirtualClock::time_point
HerderImpl::triggerAnchorFromPrepareStart(
    uint64_t lastIndex, VirtualClock::time_point now,
    std::chrono::milliseconds expectedClose)
{
    auto lastStart = mHerderSCPDriver.getPrepareStart(lastIndex);
    if (lastStart)
    {
        return *lastStart;
    }
    // Pessimistic estimate: assume the previous ballot protocol started one
    // full interval ago, so the next trigger should fire immediately.
    return now - expectedClose;
}

// Compute the anchor for the next trigger. Caller fires at
// `anchor + expectedClose`. Unlike the local ballot cadence trigger, this uses
// the externalized close time from the previous ledger, which is a timestamp
// coming from a different node whose system clock may be drifted relative to
// ours. This is more accurate than just using local timers, as those skew late
// based on latency from the leader, but we need to be careful to account for
// drift.
//
// Constraints:
// 1. Track `expectedClose` on the network timeline as closely as possible.
// 2. Under drift, prefer a longer ledger to a shorter one to prevent fast
//    ledgers causing too much strain on the network.
// 3. Never let a badly lagging local clock wedge the node (i.e. never schedule
//    a trigger in the far future).
//
// Scenarios (assume last `closeTime` = N, target = 5s):
// 1. Our clock ahead of network: Check how much time our system clock says has
//    elapsed since the externalized network ledger closed
//    (timeSinceNetworkLedgerStart). If timeSinceNetworkLedgerStart > target,
//    either the network itself is slow, or our clock is ahead.
//
//   To get a better sense of which, we also take into account
//   nomination time, so the check becomes timeSinceNetworkLedgerStart > target
//   + nominationBudget. The budget is measured on the steady clock from the
//   local trigger to entry into ballot. This includes proposal construction
//   and all nomination time, including unfinished rounds and delayed timer
//   callbacks.
//
//   If we think we're drifting ahead after taking nomination into account, we
//   fall back to prepare-start anchor, which is based on our local clock and
//   can't drift, but is slower.
//
//   Note that if nomination is quick, but apply takes a long time, it still
//   appears like we're ahead and we fall back to prepare-start. This isn't a
//   problem. The prepare-start timer starts before the apply stage, so a long
//   apply is "baked in" to the time. If apply was the bottleneck, we'll
//   probably trigger immediately even if we use the prepare-start timer because
//   we're behind, so using prepare-start is identical to the network-based
//   anchor from the perf standpoint if we really are in sync anyway.
//
//   This is not true for nomination. The prepare-start timer starts after
//   nomination, so it does not reflect a long nomination. This is why we need
//   the nomination budget in our check. If nomination took a long time and we
//   fall back to the conservative timer, this is much worse from a perf
//   perspective, as we are waiting nomination time + target time before
//   starting the next ledger.
//
// 2. Our clock behind network: Check whether our local timer says
//    more time has elapsed since prepare-start vs how much time has
//    elapsed since the externalized network ledger closed.
//
//   If true, our system clock is lagging far enough that the network-based
//   anchor would schedule the next trigger later than the conservative timer
//   based on local ballot cadence. That delay can grow with drift and wedge the
//   node if arbitrarily in the future, so we fall back to the prepare-start
//   anchor. This anchor is conservative and local, but it gives us a bounded
//   trigger time even when system time is badly behind.
//
// 3. Clocks synced, but nomination/apply was slow: With synced clocks,
//    time since network close time is large because real time really passed,
//    not because our system clock drifted. The goal is to avoid falling back to
//    a conservative timer and snowballing the real delay.
//
//    See scenario 1: measured elapsed time accounts for slow nomination
//    independently of system clock drift. If nomination is slow, we
//    can't fall back to the prepare-apply timer because it would compound the
//    delay. If apply is slow, it doesn't matter which timer we use, they both
//    will result in triggering immediately.
VirtualClock::time_point
HerderImpl::triggerAnchorFromConsensusCloseTime(
    uint64_t lastIndex, VirtualClock::time_point now,
    std::chrono::milliseconds expectedClose)
{
    auto fallbackToPrepareStart = [&]() {
        mSCPMetrics.mTriggerPrepareStartFallback.Mark();
        return triggerAnchorFromPrepareStart(lastIndex, now, expectedClose);
    };

    auto consensusCloseTime = trackingConsensusCloseTime();
    if (consensusCloseTime == ConsensusTime{})
    {
        CLOG_WARNING(Herder, "Consensus close time is 0, falling back to "
                             "prepare-start anchor");
        return fallbackToPrepareStart();
    }

    // Compare elapsed time since the externalized closeTime with local steady
    // time. Their difference includes clock drift and how early or late our
    // trigger fired relative to the proposer's close time.
    auto externalizedSystemTime = consensusCloseTime.toSystemTime();
    auto currentSystemTime = mApp.getClock().system_now();
    auto timeSinceNetworkLedgerStart =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            currentSystemTime - externalizedSystemTime);

    auto localBallotStart =
        triggerAnchorFromPrepareStart(lastIndex, now, expectedClose);
    auto timeSinceLocalBallotStart =
        std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                              localBallotStart);

    auto nominationBudget =
        mHerderSCPDriver.getTriggerToBallotDuration(lastIndex);
    auto logFallback = [&](char const* reason) {
        auto zero = std::chrono::milliseconds::zero();
        CLOG_INFO(
            Herder,
            "Trigger fallback after ledger {}: {}, network elapsed {} "
            "ms, trigger-to-ballot {} ms, ballot elapsed {} ms, local "
            "wait {} ms, network wait {} ms",
            lastIndex, reason, timeSinceNetworkLedgerStart.count(),
            nominationBudget.count(), timeSinceLocalBallotStart.count(),
            std::max(zero, expectedClose - timeSinceLocalBallotStart).count(),
            std::max(zero, expectedClose - timeSinceNetworkLedgerStart)
                .count());
        return fallbackToPrepareStart();
    };

    // Scenario 2: if system time is behind the local prepare-start timer, the
    // network-based anchor can wedge the node, so use the local fallback.
    if (timeSinceLocalBallotStart > timeSinceNetworkLedgerStart)
    {
        return logFallback("clock behind");
    }

    // A timeout counts only after its callback runs, and none are counted
    // after entry into ballot. A slow fetch or busy main thread can therefore
    // delay nomination without increasing the count. Measure elapsed time
    // directly instead of approximating it with completed timeout durations.
    // Scenario 1: if elapsed system time exceeds target plus explainable
    // nomination delay, treat it as clock-ahead drift and use the fallback.
    if (timeSinceNetworkLedgerStart > expectedClose + nominationBudget)
    {
        return logFallback("clock ahead or slow ballot/apply");
    }

    return now - timeSinceNetworkLedgerStart;
}

void
HerderImpl::setupTriggerNextLedger()
{
    discardPreparedTxSet();
    // Invariant: core proceeds to vote for the next ledger only when it's _not_
    // applying to ensure block production does not conflict with ledger close.
    releaseAssert(!mLedgerManager.isApplying());

    // Invariant: tracking is equal to LCL when we trigger. This helps ensure
    // core emits SCP messages only for slots it can fully validate
    // (any closed ledger is fully validated)
    releaseAssert(isTracking());
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    releaseAssert(trackingConsensusLedgerIndex() == lcl.header.ledgerSeq);
    releaseAssert(mLedgerManager.isSynced());

    mTriggerTimer.cancel();

    uint64_t nextIndex = nextConsensusLedgerIndex();
    auto lastIndex = trackingConsensusLedgerIndex();

    // if we're in sync, we setup mTriggerTimer
    // it may get cancelled if a more recent ledger externalizes

    std::chrono::milliseconds milliseconds =
        mLedgerManager.getExpectedLedgerCloseTime();

    auto now = mApp.getClock().now();

    // Starting from protocol 28 the trigger timer is anchored on the
    // network-agreed consensus close time.
    // FORCE_OLD_STYLE_PREPARE_START_TRIGGER_TIMER forces the older
    // prepare-start anchor as an emergency fallback.
    bool const useConsensusCloseTimeAnchor =
        protocolVersionStartsFrom(
            lcl.header.ledgerVersion,
            CONSENSUS_CLOSE_TIME_TRIGGER_PROTOCOL_VERSION) &&
        !mApp.getConfig().FORCE_OLD_STYLE_PREPARE_START_TRIGGER_TIMER;

    auto lastLedgerStartingPoint =
        useConsensusCloseTimeAnchor
            ? triggerAnchorFromConsensusCloseTime(lastIndex, now, milliseconds)
            : triggerAnchorFromPrepareStart(lastIndex, now, milliseconds);

    // Adjust trigger time in case node's clock has drifted.
    // This ensures that next value to nominate is valid
    auto triggerTime = lastLedgerStartingPoint + milliseconds;

    bool const triggerDue = triggerTime <= now;
    if (triggerTime < now)
    {
        triggerTime = now;
    }

    auto triggerOffset = std::chrono::duration_cast<std::chrono::milliseconds>(
        triggerTime - now);

    // The smallest close time the next nomination may propose
    auto minCandidateCt =
        getConsensusTime(lcl.header.scpValue).next(lcl.header.ledgerVersion);
    auto ctOffset = ctValidityOffset(minCandidateCt, triggerOffset);

    if (ctOffset > std::chrono::milliseconds::zero())
    {
        CLOG_INFO(Herder, "Adjust trigger time by {} ms", ctOffset.count());
        triggerTime += ctOffset;
    }

    // even if ballot protocol started before triggering, we just use that
    // time as reference point for triggering again (this may trigger right
    // away if externalizing took a long time)
    mTriggerTimer.expires_at(triggerTime);

    if (!mApp.getConfig().MANUAL_CLOSE)
    {
        if (triggerDue)
        {
            mSCPMetrics.mTriggerImmediate.Mark();
        }
        mTriggerTimer.async_wait(
            [this, nextIndex, triggerTime]() {
                mSCPMetrics.mTriggerLateness.Update(
                    std::max(VirtualClock::duration::zero(),
                             mApp.getClock().now() - triggerTime));
                triggerNextLedger(static_cast<uint32_t>(nextIndex), true);
            },
            &VirtualTimer::onFailureNoop);

        scheduleTxSetPreparation(nextIndex, triggerTime, triggerDue, now,
                                 milliseconds, minCandidateCt);
    }

#ifdef BUILD_TESTS
    mTriggerNextLedgerSeq = static_cast<uint32_t>(nextIndex);
#endif
}

void
HerderImpl::scheduleTxSetPreparation(
    uint64_t nextIndex, VirtualClock::time_point triggerTime, bool triggerDue,
    VirtualClock::time_point now, std::chrono::milliseconds expectedCloseTime,
    ConsensusTime minCandidateCt)
{
    // Prepare early only if the trigger is still ahead. A due trigger fires
    // now and builds itself; both timers would fire at the same instant in
    // an unspecified order (event sequence numbers are per timer), so the
    // leader could build once to prepare and again at the trigger.
    if (!getSCP().isValidator() || triggerDue ||
        !mHerderSCPDriver.isLocalNodeRoundLeader(nextIndex, 1))
    {
        return;
    }
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    // Validate against the close time we expect at the trigger, not the
    // earlier wall-clock time at which we start work.
    auto closeTime = std::max(
        minCandidateCt, ConsensusTime::fromSystemTime(
                            mApp.getClock().system_now() +
                                std::chrono::duration_cast<
                                    VirtualClock::system_time_point::duration>(
                                    triggerTime - now),
                            lcl.header.ledgerVersion));
    // Build shortly before the trigger rather than right away: the set then
    // holds what arrived in the meantime, and is still built and on its way
    // to the peers when consensus starts (see makeProposalValue). This also
    // yields out of ledger close before touching its read view.
    mPrepareTxSetTimer.expires_at(std::max(
        now, triggerTime - prepareTxSetLeadTime(expectedCloseTime,
                                                expectedTxSetBuildDuration())));
    mPrepareTxSetTimer.async_wait(
        [this, nextIndex, closeTime]() {
            prepareTxSet(static_cast<uint32_t>(nextIndex), closeTime);
        },
        &VirtualTimer::onFailureNoop);
}

// Returns the ledger index of the oldest ledger that it is safe to delete while
// still keeping all the information needed to publish checkpoints
static uint32_t
getSafeLedgerToDelete(uint32_t ledger, Config const& cfg)
{
    // Calculate the minimum of ledger and/or any queued checkpoint.
    uint32_t ql = HistoryManager::getMinLedgerQueuedToPublish(cfg);
    uint32_t qmin = ql == 0 ? ledger : std::min(ql, ledger);

    // Next calculate, given qmin, the first ledger it'd be _safe to delete_
    // while still keeping everything required to publish. So if qmin is
    // (for example) 0x7f = 127, then we want to keep 64 ledgers before
    // that, and therefore can erase 0x3f = 63 and less.
    uint32_t freq = HistoryManager::getCheckpointFrequency(cfg);
    return qmin >= freq ? qmin - freq : 0;
}

void
HerderImpl::eraseOutsideRange(std::optional<uint32> minSlot,
                              std::optional<uint32> maxSlot)
{
    auto lastCheckpointSeq = getMostRecentCheckpointSeq();
    getHerderSCPDriver().purgeSlotsOutsideRange(minSlot, maxSlot,
                                                lastCheckpointSeq);
    mPendingEnvelopes.eraseOutsideRange(minSlot, maxSlot, lastCheckpointSeq);

    if (minSlot)
    {
        auto lastIndex = trackingConsensusLedgerIndex();
        mApp.getOverlayManager().clearLedgersBelow(*minSlot, lastIndex);
        uint32_t lmin = getSafeLedgerToDelete(*minSlot, mApp.getConfig());
        // To avoid blocking too long, don't delete more than one checkpoint of
        // history
        uint32_t const ledgersToDelete =
            HistoryManager::getCheckpointFrequency(mApp.getConfig());
        HerderPersistence::deleteOldEntries(
            mApp.getDatabase().getRawMiscSession(), lmin, ledgersToDelete);
    }
}

bool
HerderImpl::recvSCPQuorumSet(Hash const& hash, SCPQuorumSet const& qset)
{
    ZoneScoped;
    return mPendingEnvelopes.recvSCPQuorumSet(hash, qset);
}

bool
HerderImpl::recvTxSet(Hash const& hash, TxSetXDRFrameConstPtr txset)
{
    ZoneScoped;
    return mPendingEnvelopes.recvTxSet(hash, txset);
}

TxSetResult
HerderImpl::getTxSet(Hash const& hash)
{
    return mPendingEnvelopes.getTxSet(hash);
}

SCPQuorumSetPtr
HerderImpl::getQSet(Hash const& qSetHash)
{
    return mHerderSCPDriver.getQSet(qSetHash);
}

uint32
HerderImpl::getMinLedgerSeqToAskPeers() const
{
    // computes the smallest ledger for which we *think* we need more SCP
    // messages
    // we ask for messages older than lcl in case they have SCP
    // messages needed by other peers
    auto low = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;

    auto maxSlots = std::min<uint32>(mApp.getConfig().MAX_SLOTS_TO_REMEMBER,
                                     SCP_EXTRA_LOOKBACK_LEDGERS);

    if (low > maxSlots)
    {
        low -= maxSlots;
    }
    else
    {
        low = LedgerManager::GENESIS_LEDGER_SEQ;
    }

    // do not ask for slots we'd be dropping anyways
    auto herderLow = getMinLedgerSeqToRemember();
    low = std::max<uint32>(low, herderLow);

    return low;
}

uint32_t
HerderImpl::getMostRecentCheckpointSeq()
{
    auto lastIndex = trackingConsensusLedgerIndex();
    return HistoryManager::firstLedgerInCheckpointContaining(lastIndex,
                                                             mApp.getConfig());
}

void
HerderImpl::setInSyncAndTriggerNextLedger()
{
    // We either have not set trigger timer, or we're in the
    // middle of a consensus round. Either way, we do not want
    // to trigger ledger, as the node is already making progress
    if (mTriggerTimer.seq() > 0)
    {
        CLOG_DEBUG(Herder, "Skipping setInSyncAndTriggerNextLedger: "
                           "trigger timer already set");
        return;
    }

    // Bring Herder and LM in sync in case they aren't
    if (mLedgerManager.getState() == LedgerManager::LM_BOOTING_STATE)
    {
        mLedgerManager.moveToSynced();
    }

    // Trigger next ledger, without requiring Herder to properly track SCP
    auto lcl = mLedgerManager.getLastClosedLedgerNum();
    triggerNextLedger(lcl + 1, false);
}

void
HerderImpl::discardPreparedTxSet()
{
    mPrepareTxSetTimer.cancel();
    mPreparedTxSet.reset();
}

void
HerderImpl::prepareTxSet(uint32_t ledgerSeq, ConsensusTime closeTime)
{
    if (!isTracking() || !mLedgerManager.isSynced() ||
        mLedgerManager.isApplying() ||
        ledgerSeq != mLedgerManager.getLastClosedLedgerNum() + 1)
    {
        return;
    }
    if (!getSCP().isValidator() ||
        !mHerderSCPDriver.isLocalNodeRoundLeader(ledgerSeq, 1))
    {
        return;
    }
    // The prepared set is proposed unchanged at the trigger
    // (makeProposalValue), so it goes to the peers as soon as its XDR
    // exists: they can have it before the proposal does.
    mPreparedTxSet = buildTxSet(ledgerSeq, closeTime, /*pushToPeers=*/true);
    CLOG_INFO(Herder,
              "Prepared TX set for ledger {} before trigger: {} transactions, "
              "close time {}",
              ledgerSeq, mPreparedTxSet->txSet->sizeTxTotal(),
              closeTime.toString());
    executePreparedTxSet();
}

void
HerderImpl::handOffTxSet(TxSetXDRFrameConstPtr const& txSet, uint32_t ledgerSeq,
                         bool pushToPeers)
{
    auto t = mApp.getMetrics()
                 .NewTimer({"herder", "txset", "build-handoff"})
                 .TimeScope();
    // Legacy (pre-protocol 20) sets cannot be carried by the Rust overlay.
    if (!txSet->isGeneralizedTxSet())
    {
        return;
    }
    // Serialized straight into the IPC message, without copying the set's
    // XDR first. The overlay's eager compression runs concurrently in Rust.
    if (mApp.getOverlayManager().cacheTxSet(
            txSet->getContentsHash(), txSet->getGeneralizedXDR(), ledgerSeq))
    {
        mTxSetsStoredByOverlay.insert(txSet->getContentsHash());
    }
    // Leader's push: this set is about to be proposed, so send it to the
    // peers now.
    if (pushToPeers)
    {
        pushTxSetToPeers(txSet, ledgerSeq);
    }
}

void
HerderImpl::executePreparedTxSet()
{
    if (!mPreparedTxSet || !mPreparedTxSet->txSet ||
        !mPreparedTxSet->applicableTxSet)
    {
        return;
    }
#ifdef BUILD_TESTS
    // The trigger proposes something else than the prepared set.
    if (mProposedTxSetOverride ||
        mApp.getConfig().TESTING_PROPOSE_RANDOM_VALUES)
    {
        return;
    }
#endif
    auto const lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (mPreparedTxSet->ledgerSeq != lcl.header.ledgerSeq + 1 ||
        mPreparedTxSet->previousLedgerHash != lcl.hash ||
        !getSCP().isValidator())
    {
        return;
    }
    // The value makeProposalValue builds at the trigger, if nothing changes
    // until then.
    auto const value = makeStellarValue(
        mPreparedTxSet->txSet->getContentsHash(), mPreparedTxSet->closeTime,
        proposalUpgrades(lcl), mApp.getConfig().NODE_SEED);
    startSpeculativeApply(mPreparedTxSet->ledgerSeq,
                          Value(xdr::xdr_to_opaque(value)),
                          mPreparedTxSet->txSet);
}

void
HerderImpl::startTxSetSelfCheck(std::shared_ptr<TxSetSelfCheck const> check,
                                uint32_t ledgerSeq)
{
    if (!check)
    {
        return;
    }
    if (executesVotedValues(mLedgerManager.getLastClosedLedgerHeader()))
    {
        // This node's own execution of the set reads it back and validates
        // it in full, and its verdict is taken there. The check is kept
        // until the node has emitted a statement: if the ledger was not
        // handed to the apply thread with this set by then (see
        // resolvePendingTxSetSelfCheck), it runs after all.
        mPendingTxSetSelfCheck = std::make_pair(std::move(check), ledgerSeq);
        return;
    }
    runTxSetSelfCheck(std::move(check), ledgerSeq);
}

void
HerderImpl::runTxSetSelfCheck(std::shared_ptr<TxSetSelfCheck const> check,
                              uint32_t ledgerSeq)
{
    mApp.postOnBackgroundThread(
        [this, check = std::move(check), ledgerSeq, &app = mApp]() {
            ZoneScopedN("tx set self-check");
            auto const failure = check->run(app);
            if (!failure)
            {
                app.getMetrics()
                    .NewMeter({"herder", "txset", "self-check-passed"}, "txset")
                    .Mark();
                return;
            }
            app.postOnMainThread(
                [this, check, ledgerSeq, reason = *failure]() {
                    txSetSelfCheckFailed(check->getTxSet()->getContentsHash(),
                                         ledgerSeq, reason);
                },
                "tx set self-check failed");
        },
        "Herder: tx set self-check");
}

void
HerderImpl::resolvePendingTxSetSelfCheck()
{
    if (!mPendingTxSetSelfCheck)
    {
        return;
    }
    auto pending = std::move(*mPendingTxSetSelfCheck);
    mPendingTxSetSelfCheck.reset();
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (pending.second != lcl.header.ledgerSeq + 1)
    {
        // For a ledger that is no longer the next one: nothing to check.
        return;
    }
    // Handed to the apply thread with this set?
    StellarValue executing;
    if (mSpeculativeApplyValue &&
        mSpeculativeApplyValue->first == pending.second &&
        toStellarValue(mSpeculativeApplyValue->second, executing) &&
        executing.txSetHash == pending.first->getTxSet()->getContentsHash())
    {
        return;
    }
    runTxSetSelfCheck(std::move(pending.first), pending.second);
}

bool
HerderImpl::executesVotedValues(LedgerHeaderHistoryEntry const& lcl) const
{
    auto const& cfg = mApp.getConfig();
    return cfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY &&
           cfg.backgroundLedgerApply() &&
           protocolValidatesTxSetsAtApply(lcl.header.ledgerVersion);
}

void
HerderImpl::txSetSelfCheckFailed(Hash const& txSetHash, uint32_t ledgerSeq,
                                 std::string const& reason)
{
    releaseAssert(threadIsMain());
    CLOG_ERROR(Herder,
               "TX set {} built for ledger {} failed the builder's "
               "self-check: {}",
               hexAbbrev(txSetHash), ledgerSeq, reason);
    CLOG_ERROR(Herder, "{}", REPORT_INTERNAL_BUG);
    mApp.getMetrics()
        .NewMeter({"herder", "txset", "self-check-failed"}, "txset")
        .Mark();
    // No more unchecked sets from this process.
    mTxSetSelfCheckFailed = true;
    if (mPreparedTxSet && mPreparedTxSet->txSet &&
        mPreparedTxSet->txSet->getContentsHash() == txSetHash)
    {
        // Not proposed yet: the trigger builds again, and checks what it
        // builds before using it.
        discardPreparedTxSet();
    }
}

#ifdef BUILD_TESTS
void
HerderImpl::overrideBuiltTxSetForTesting(
    TxSetXDRFrameConstPtr& txSet,
    std::shared_ptr<TxSetSelfCheck const>& selfCheck)
{
    if (!mBuiltTxSetOverrideForTesting)
    {
        return;
    }
    txSet = mBuiltTxSetOverrideForTesting(txSet);
    if (selfCheck)
    {
        selfCheck = selfCheck->withTxSetForTesting(txSet);
    }
}
#endif

xdr::xvector<UpgradeType, 6>
HerderImpl::proposalUpgrades(LedgerHeaderHistoryEntry const& lcl)
{
    auto newUpgrades = emptyUpgradeSteps;

    // see if we need to include some upgrades
    std::vector<LedgerUpgrade> upgrades;
    {
        auto ledgerView = mApp.getLedgerManager().getLCLView();
        upgrades = mUpgrades.createUpgradesFor(lcl.header, *ledgerView,
                                               mApp.getConfig());
    }
    for (auto const& upgrade : upgrades)
    {
        Value v(xdr::xdr_to_opaque(upgrade));
        if (v.size() >= UpgradeType::max_size())
        {
            CLOG_ERROR(
                Herder,
                "HerderImpl::triggerNextLedger exceeded size for upgrade "
                "step (got {} ) for upgrade type {}",
                v.size(), upgrade.type());
            CLOG_ERROR(Herder, "{}", REPORT_INTERNAL_BUG);
        }
        else
        {
            newUpgrades.emplace_back(v.begin(), v.end());
        }
    }
    return newUpgrades;
}

void
HerderImpl::pushTxSetToPeers(TxSetXDRFrameConstPtr const& txSet,
                             uint32_t ledgerSeq)
{
    // Legacy (pre-protocol 20) sets cannot be carried by the Rust overlay.
    if (!txSet || !txSet->isGeneralizedTxSet())
    {
        return;
    }
    mApp.getOverlayManager().pushTxSet(txSet->getContentsHash(), ledgerSeq);
}

HerderImpl::PreparedTxSet
HerderImpl::buildTxSet(uint32_t ledgerSeq, ConsensusTime closeTime,
                       bool pushToPeers)
{
    auto const txSetBuildStart = std::chrono::steady_clock::now();
    auto const lcl = mLedgerManager.getLastClosedLedgerHeader();
    releaseAssert(ledgerSeq == lcl.header.ledgerSeq + 1);
    releaseAssert(closeTime > getConsensusTime(lcl.header.scpValue));
    uint32_t const round = getSCP().getLeaderRound(ledgerSeq);
    bool const activeLeader =
        round != 0 && mHerderSCPDriver.isLocalNodeRoundLeader(ledgerSeq, round);
    bool const legacyTxSet = buildsLegacyTxSet(lcl.header.ledgerVersion);
    CLOG_INFO(Herder, "Building TX set as {} for ledger {}",
              activeLeader  ? fmt::format("round {} leader", round)
              : legacyTxSet ? std::string("validator (legacy tx set)")
                            : std::string("first-round leader (early)"),
              ledgerSeq);
    // This build validates every candidate itself.
    stopSignaturePrewarm();
    auto& metrics = mApp.getMetrics();

    // How long after this node's vote for the previous ledger its set for
    // this one is needed: what the start of the next early selection is
    // timed by (earlyTxSetStartDelay).
    if (mFirstVote && mFirstVote->first + 1 == ledgerSeq && round <= 1)
    {
        // (the first build of the ledger, by the first round's leader: a
        // rebuild at the trigger or a later round's build comes when it
        // comes)
        auto const votedAt = mFirstVote->second;
        mFirstVote.reset();
        mLastVoteToBuild =
            std::max(std::chrono::milliseconds(1),
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         mApp.getClock().now() - votedAt));
        mVoteToBuild = mVoteToBuild == std::chrono::milliseconds::zero()
                           ? mLastVoteToBuild
                           : (3 * mVoteToBuild + mLastVoteToBuild) / 4;
    }

    auto finish = [&](PreparedTxSet&& prepared, bool built) {
        auto const buildDuration =
            std::chrono::steady_clock::now() - txSetBuildStart;
        mSCPMetrics.mTxSetBuild.Update(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                buildDuration));
        if (!built)
        {
            // A completed selection says nothing about what a build costs,
            // which is what a build's lead time has to cover.
            return std::move(prepared);
        }
        // The next build is scheduled far enough ahead of its trigger to fit
        // one like this (see prepareTxSetLeadTime).
        mLastTxSetBuildDuration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                buildDuration);
        if (prepared.txSet->sizeTxTotal() >= 100)
        {
            mTxSetBuildCostPerTx =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    buildDuration) /
                static_cast<int64_t>(prepared.txSet->sizeTxTotal());
        }
        return std::move(prepared);
    };

    // A set this node selected while the previous ledger was in its ballot
    // only has to be completed.
    if (auto completed =
            completeEarlyTxSet(ledgerSeq, closeTime, pushToPeers, lcl))
    {
        return finish(std::move(*completed), /*built=*/false);
    }

    // Apply-time offset: see the comment in triggerNextLedger
    auto const closeTimeOffset =
        closeTime.toApplyTime() - getApplyTime(lcl.header.scpValue);
    TxSetXDRFrameConstPtr proposedSet;
    ApplicableTxSetFrameConstPtr applicableProposedSet;

    // Build TX set from Rust overlay's mempool (not local TransactionQueue)
    // The Rust overlay maintains the mempool via TX flooding
    PerPhaseTransactionList txPhases;
    bool const supportsSoroban = protocolVersionStartsFrom(
        lcl.header.ledgerVersion, SOROBAN_PROTOCOL_VERSION);
    auto* const validationCache = validationCacheFor(lcl);

    // Signature cache lookups of this build, the validation for a refill
    // included.
    uint64_t sigCacheHits = 0;
    uint64_t sigCacheMisses = 0;
    PubKeyUtils::getVerifySigCacheCounts(sigCacheHits, sigCacheMisses);

    // The pull is the capacity of the set plus a margin. If validation
    // takes so much out of a phase that it ends below its capacity while
    // the mempool has more to give, the build pulls again, twice as much,
    // and goes on with what it has not seen yet.
    TxFrameList classicTxs;
    TxFrameList sorobanTxs;
    UnorderedSet<uint64_t> seen;
    bool mempoolExhausted = false;
    size_t pulled = 0;
    {
        std::array<size_t, 2> const capacity{
            mLedgerManager.getLastMaxTxSetSizeOps(),
            supportsSoroban &&
                    mLedgerManager.hasLastClosedSorobanNetworkConfig()
                ? static_cast<size_t>(
                      mLedgerManager.getLastClosedSorobanNetworkConfig()
                          .ledgerMaxTxCount())
                : size_t(0)};
        // Per phase: how many candidates were pulled, and how much of the
        // phase's capacity those of them fill that are valid and the first
        // of their source account (a set holds one transaction per
        // account).
        std::array<size_t, 2> candidates{0, 0};
        std::array<size_t, 2> usable{0, 0};
        std::array<size_t, 2> filled{0, 0};
        UnorderedSet<AccountID> sources;
        UnorderedMap<AccountID, int64_t> feeTotals;
        for (size_t pull = 0; pull < MAX_TX_SET_PULLS; ++pull)
        {
            bool full = false;
            auto const fresh =
                pullNewCandidates(txSetPullCount() << pull, seen, full);
            pulled += fresh.size();
            std::array<TxFrameList, 2> freshPhases;
            for (auto const& tx : fresh)
            {
                freshPhases[tx->isSoroban() ? 1 : 0].push_back(tx);
            }
            classicTxs.insert(classicTxs.end(), freshPhases[0].begin(),
                              freshPhases[0].end());
            sorobanTxs.insert(sorobanTxs.end(), freshPhases[1].begin(),
                              freshPhases[1].end());
            if (!full)
            {
                mempoolExhausted = true;
                break;
            }
            if (pull + 1 == MAX_TX_SET_PULLS)
            {
                break;
            }
            // The mempool may hold more. More is needed if a phase lost
            // candidates (to validation, or to the one-per-account rule)
            // and what is left does not fill it: validate what is new for
            // the phases that are not filled yet (the selection below finds
            // the results in the cache). A phase that lost none got all the
            // mempool's top holds for it. A phase that lost some goes on
            // as long as each pull brings it something new: a stale
            // transaction of a kind the mempool holds little of costs one
            // deeper pull, not all of them.
            auto refillTimer =
                metrics.NewTimer({"herder", "txset", "build-refill-check"})
                    .TimeScope();
            auto ctx = TxSetValidationContext::forLCL(mApp);
            ctx.mValidationCache = validationCache;
            bool shortPhase = false;
            for (size_t p = 0; p < freshPhases.size(); ++p)
            {
                if (filled[p] >= capacity[p])
                {
                    continue;
                }
                TxFrameList notValid;
                auto const ok = TxSetUtils::trimInvalid(
                    freshPhases[p], mApp, ctx, feeTotals,
                    closeTimeOffset.seconds(), closeTimeOffset.seconds(),
                    notValid);
                candidates[p] += freshPhases[p].size();
                for (auto const& tx : ok)
                {
                    if (sources.insert(tx->getSourceID()).second)
                    {
                        ++usable[p];
                        filled[p] += p == 0 ? tx->getNumOperations() : 1;
                    }
                }
                shortPhase =
                    shortPhase ||
                    (usable[p] < candidates[p] && filled[p] < capacity[p] &&
                     (pull == 0 || !freshPhases[p].empty()));
            }
            if (!shortPhase)
            {
                break;
            }
            metrics.NewMeter({"herder", "txset", "refill-pull"}, "pull").Mark();
        }
    }
    auto framesTimer =
        metrics.NewTimer({"herder", "txset", "build-frames"}).TimeScope();

    CLOG_INFO(Herder, "Got {} transactions from Rust overlay mempool", pulled);

    // One transaction per source account.
    TxSetUtils::keepOnePerSourceAccount(classicTxs, sorobanTxs);

    txPhases.emplace_back(std::move(classicTxs));
    if (supportsSoroban)
    {
        txPhases.emplace_back(std::move(sorobanTxs));
    }

    // Validate each phase only until it holds enough valid transactions (see
    // validTxTargets).
    auto const targets = validTxTargets(lcl.header.ledgerVersion);

    PerPhaseTransactionList invalidTxPhases;
    invalidTxPhases.resize(txPhases.size());
    framesTimer.Stop();

    // The builder's check of its own set does not hold up the set: it runs
    // in the background (startTxSetSelfCheck), unless one has failed before.
    std::shared_ptr<TxSetSelfCheck const> selfCheck;
    // Only where every node validates the set when it applies the ledger:
    // before that protocol the followers validate before they vote, and
    // this node records its own set as valid for them (cacheValidTxSet).
    bool const deferSelfCheck =
        !legacyTxSet && !mTxSetSelfCheckFailed &&
        protocolValidatesTxSetsAtApply(lcl.header.ledgerVersion);
    std::tie(proposedSet, applicableProposedSet) = makeTxSetFromTransactions(
        txPhases, mApp, closeTimeOffset, invalidTxPhases
#ifdef BUILD_TESTS
        ,
        false, {}, false
#endif
        ,
        [&](TxSetXDRFrameConstPtr const& txSet) {
            // Hand the final XDR to the overlay, and with `pushToPeers` to
            // the peers, before the builder's check of the set. If that
            // check fails before the proposal, nothing is proposed, and
            // peers drop the unused set.
            handOffTxSet(txSet, ledgerSeq, pushToPeers);
        },
        targets, validationCache, deferSelfCheck ? &selfCheck : nullptr);
    if (applicableProposedSet)
    {
        recordTxSetFill(*applicableProposedSet, mempoolExhausted);
    }
#ifdef BUILD_TESTS
    overrideBuiltTxSetForTesting(proposedSet, selfCheck);
#endif
    mLastBuiltTxSet = std::make_pair(proposedSet->getContentsHash(), closeTime);
    startTxSetSelfCheck(selfCheck, ledgerSeq);
    markSignatureCacheLookups(mApp, "build", sigCacheHits, sigCacheMisses);
    CLOG_INFO(Herder, "Proposed TX set has {} transactions",
              proposedSet->sizeTxTotal());

    return finish(PreparedTxSet{lcl.hash, ledgerSeq, closeTime, proposedSet,
                                std::move(applicableProposedSet),
                                invalidTxsToRemove(invalidTxPhases)},
                  /*built=*/true);
}

std::vector<Hash>
HerderImpl::invalidTxsToRemove(PerPhaseTransactionList const& invalidTxs)
{
    // The mempool does no stateful validation, so it would keep handing us the
    // transactions that just failed validation (stale sequence number, can't
    // pay fee, expired, ...) on every nomination, crowding out valid ones.
    // Collect removals to apply at the trigger, except for transactions with
    // a *future* sequence number: those are chained behind a pending
    // transaction from the same account and become valid once it applies.
    std::vector<Hash> invalidTxHashes;
    if (!invalidTxs.empty())
    {
        auto ledgerView = mLedgerManager.getLCLView();
        for (auto const& phase : invalidTxs)
        {
            for (auto const& tx : phase)
            {
                auto acc = ledgerView->getAccount(tx->getSourceID());
                if (acc &&
                    tx->getSeqNum() > acc.current().data.account().seqNum + 1)
                {
                    continue;
                }
                CLOG_DEBUG(Herder,
                           "Invalid tx {} in mempool pull: seq {} "
                           "(account seq {})",
                           hexAbbrev(tx->getFullHash()), tx->getSeqNum(),
                           acc ? acc.current().data.account().seqNum : -1);
                invalidTxHashes.push_back(tx->getFullHash());
            }
        }
    }
    return invalidTxHashes;
}

size_t
HerderImpl::txSetPullCount() const
{
    size_t const capacity = maxTxSetCandidates();
    size_t const margin =
        (capacity * mApp.getConfig().EXPERIMENTAL_TX_SET_PULL_MARGIN_PERCENT +
         99) /
        100;
    return std::max<size_t>(1, capacity + margin);
}

TxFrameList
HerderImpl::pullNewCandidates(size_t count, UnorderedSet<uint64_t>& seen,
                              bool& full)
{
    auto& metrics = mApp.getMetrics();
    auto const payload = [&]() {
        auto t =
            metrics.NewTimer({"herder", "txset", "build-pull"}).TimeScope();
        return pullTopTransactionsPayload(count);
    }();
    auto framesTimer =
        metrics.NewTimer({"herder", "txset", "build-frames"}).TimeScope();
    uint32_t pulled = 0;
    if (payload.size() >= 4)
    {
        std::memcpy(&pulled, payload.data(), 4);
    }
    full = pulled >= count;
    TxFrameList fresh;
    Hash const& networkID = mApp.getNetworkID();
    bool const supportsSoroban = protocolVersionStartsFrom(
        mLedgerManager.getLastClosedLedgerHeader().header.ledgerVersion,
        SOROBAN_PROTOCOL_VERSION);
    forEachPulledTx(payload, [&](uint8_t const* data, size_t length) {
        if (!seen.insert(hashPulledTx(data, length)).second)
        {
            return;
        }
        TransactionEnvelope env;
        if (!decodePulledTx(data, length, env))
        {
            CLOG_WARNING(Herder, "Failed to parse a pulled transaction");
            return;
        }
        auto tx = TransactionFrameBase::makeTransactionFromWire(networkID, env);
        if (tx->isSoroban() && !supportsSoroban)
        {
            CLOG_DEBUG(Herder, "Ignoring Soroban transaction before Soroban "
                               "protocol support");
            return;
        }
        fresh.push_back(std::move(tx));
    });
    return fresh;
}

void
HerderImpl::recordTxSetFill(ApplicableTxSetFrame const& txSet,
                            bool mempoolExhausted)
{
    auto& metrics = mApp.getMetrics();
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    // A phase below its capacity although the mempool had more to give:
    // the build stopped looking (its pulls are bounded), or what it found
    // did not fit. A phase without transactions is left out: the mempool's
    // top held no candidate of its kind, whatever else the mempool holds.
    auto record = [&](char const* fill, char const* isShort, size_t size,
                      size_t capacity) {
        if (capacity == 0)
        {
            return;
        }
        metrics.NewHistogram({"herder", "txset", fill})
            .Update(static_cast<int64_t>(
                std::min<size_t>(100, size * 100 / capacity)));
        if (size != 0 && size < capacity && !mempoolExhausted)
        {
            metrics.NewMeter({"herder", "txset", isShort}, "txset").Mark();
        }
    };
    record("fill-classic", "short-classic", txSet.sizeOp(TxSetPhase::CLASSIC),
           mLedgerManager.getLastMaxTxSetSizeOps());
    if (txSet.numPhases() > static_cast<size_t>(TxSetPhase::SOROBAN) &&
        protocolVersionStartsFrom(lcl.header.ledgerVersion,
                                  SOROBAN_PROTOCOL_VERSION) &&
        mLedgerManager.hasLastClosedSorobanNetworkConfig())
    {
        record("fill-soroban", "short-soroban",
               txSet.sizeTx(TxSetPhase::SOROBAN),
               mLedgerManager.getLastClosedSorobanNetworkConfig()
                   .ledgerMaxTxCount());
    }
}

std::chrono::milliseconds
HerderImpl::earlyTxSetStartDelay() const
{
    using std::chrono::milliseconds;
#ifdef BUILD_TESTS
    if (mEarlyTxSetStartDelayForTesting)
    {
        return *mEarlyTxSetStartDelayForTesting;
    }
#endif
    if (mVoteToBuild == milliseconds::zero() ||
        mEarlyTxSetJobDuration == milliseconds::zero())
    {
        // Nothing measured yet: start at once.
        return milliseconds::zero();
    }
    // The earlier of what the build did lately and what it did last time,
    // less a selection that takes as long as the slower of the two, with a
    // quarter to spare, and 30 ms for the hand-over.
    auto const untilBuild = std::min(mVoteToBuild, mLastVoteToBuild);
    auto const job =
        std::max(mEarlyTxSetJobDuration, mLastEarlyTxSetJobDuration);
    auto const delay = untilBuild - job - job / 4 - milliseconds(30);
    return std::max(milliseconds::zero(), delay);
}

void
HerderImpl::stopEarlyTxSetSelection()
{
    mEarlyTxSetTimer.cancel();
    if (mEarlyTxSet)
    {
        std::lock_guard<std::mutex> lock(mEarlyTxSet->mMutex);
        mEarlyTxSet->mStop.store(true);
    }
    mEarlyTxSet.reset();
    mEarlyTxSetPlan.reset();
}

void
HerderImpl::maybeScheduleEarlyTxSetSelection(SCPEnvelope const& envelope)
{
    auto const& cfg = mApp.getConfig();
    if (!cfg.EXPERIMENTAL_EARLY_TX_SET_SELECTION || !getSCP().isValidator())
    {
        return;
    }
#ifdef BUILD_TESTS
    // The bucket list isn't populated in this mode: there is no snapshot of
    // the last closed ledger to read from other threads.
    if (cfg.MODE_USES_IN_MEMORY_LEDGER)
    {
        return;
    }
#endif
    // The value this node is working on: what it votes to prepare, or to
    // commit.
    auto const& st = envelope.statement;
    Value const* value = nullptr;
    switch (st.pledges.type())
    {
    case SCP_ST_PREPARE:
        value = &st.pledges.prepare().ballot.value;
        break;
    case SCP_ST_CONFIRM:
        value = &st.pledges.confirm().ballot.value;
        break;
    default:
        return;
    }
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (st.slotIndex != lcl.header.ledgerSeq + 1 ||
        !protocolValidatesTxSetsAtApply(lcl.header.ledgerVersion) ||
        buildsLegacyTxSet(lcl.header.ledgerVersion) || !isTracking() ||
        !mLedgerManager.isSynced())
    {
        return;
    }
    if (!mFirstVote || mFirstVote->first != st.slotIndex)
    {
        mFirstVote = std::make_pair(st.slotIndex, mApp.getClock().now());
    }
    if (mEarlyTxSetPlan && mEarlyTxSetPlan->slotIndex == st.slotIndex &&
        mEarlyTxSetPlan->value == *value)
    {
        // Looked at already.
        return;
    }
    // The first vote for this ledger, or a vote for another value: whatever
    // was planned or running was for a value this node no longer votes for.
    stopEarlyTxSetSelection();
    EarlyTxSetPlan plan;
    plan.slotIndex = st.slotIndex;
    plan.value = *value;
    mEarlyTxSetPlan = plan;

    // No selection on a ledger with upgrades: they change what the next set
    // is validated against and the limits it is selected within. None after
    // an empty set either: nothing is known about the next ledger then that
    // a build would not know.
    StellarValue sv;
    if (!toStellarValue(*value, sv) || isEmptyTxSetStellarValue(sv) ||
        !sv.upgrades.empty())
    {
        return;
    }
    auto txSetResult = mPendingEnvelopes.getTxSet(sv.txSetHash);
    auto const* received = std::get_if<TxSetXDRFrameConstPtr>(&txSetResult);
    if (!received || !*received)
    {
        // Not in hand yet: look again at the next statement for the value.
        mEarlyTxSetPlan.reset();
        return;
    }
    if (!(*received)->isGeneralizedTxSet() ||
        (*received)->previousLedgerHash() != lcl.hash ||
        (*received)->sizeTxTotal() == 0)
    {
        return;
    }
    // Only the node first in line for the next ledger.
    auto const schedule = mHerderSCPDriver.getExpectedLeaderSchedule(
        st.slotIndex + 1, sv, /*appliedTransactions=*/true);
    if (schedule.empty() || !(schedule[0] == getSCP().getLocalNodeID()))
    {
        return;
    }
    mEarlyTxSetPlan->stellarValue = sv;
    mEarlyTxSetPlan->planned = true;
    mApp.getMetrics()
        .NewMeter({"herder", "txset", "early-planned"}, "txset")
        .Mark();
    // Through the timer even without a delay: this is called while SCP
    // emits an envelope, and the start pulls the mempool.
    mEarlyTxSetTimer.expires_from_now(earlyTxSetStartDelay());
    mEarlyTxSetTimer.async_wait(
        [this, slotIndex = st.slotIndex, value = *value, txSet = *received]() {
            startEarlyTxSetSelection(slotIndex, value, txSet);
        },
        &VirtualTimer::onFailureNoop);
}

void
HerderImpl::startEarlyTxSetSelection(uint64_t slotIndex, Value const& value,
                                     TxSetXDRFrameConstPtr const& txSet)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    if (mApp.isStopping())
    {
        return;
    }
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    // Only while the ledger is still the next one to close and this node
    // still votes for the value.
    if (!mEarlyTxSetPlan || !mEarlyTxSetPlan->planned || mEarlyTxSet ||
        mEarlyTxSetPlan->slotIndex != slotIndex ||
        !(mEarlyTxSetPlan->value == value) ||
        slotIndex != lcl.header.ledgerSeq + 1 ||
        txSet->previousLedgerHash() != lcl.hash || !isTracking())
    {
        return;
    }
    auto& metrics = mApp.getMetrics();
    auto job = std::make_shared<EarlyTxSetJob>();
    job->mStart = std::chrono::steady_clock::now();
    job->mClosingLedgerSeq = static_cast<uint32_t>(slotIndex);
    job->mClosingValue = mEarlyTxSetPlan->stellarValue;
    job->mClosingTxSet = txSet;
    job->mPreviousLedger = mLedgerManager.getLastClosedLedgerState();
    if (job->mPreviousLedger->getLastClosedLedgerHeader().hash != lcl.hash)
    {
        return;
    }
    // The closing ledger's transactions are still in the mempool: look past
    // that many more.
    job->mPayload = [&]() {
        auto timer =
            metrics.NewTimer({"herder", "txset", "early-pull"}).TimeScope();
        return std::make_shared<std::vector<uint8_t> const>(
            pullTopTransactionsPayload(txSetPullCount() +
                                       txSet->sizeTxTotal()));
    }();
    {
        // The candidates are validated for the close time the next trigger
        // is expected to have: the target after this ledger's close time,
        // or now if that has passed. The completion validates them again
        // for the close time the set really gets.
        auto const version = lcl.header.ledgerVersion;
        auto const closingCloseTime = getConsensusTime(job->mClosingValue);
        auto const expectedTrigger = std::max<VirtualClock::system_time_point>(
            closingCloseTime.toSystemTime() +
                mLedgerManager.getExpectedLedgerCloseTime(),
            mApp.getClock().system_now());
        auto const assumedCloseTime =
            std::max(ConsensusTime::fromSystemTime(expectedTrigger, version),
                     closingCloseTime.next(version));
        job->mAssumedCloseTimeOffset =
            (assumedCloseTime.toApplyTime() - getApplyTime(job->mClosingValue))
                .seconds();
    }
    {
        // What orders candidates with equal fee rates must not be
        // predictable by who submits them. The global random engine is not
        // used: whether a job runs depends on timing, and a draw here would
        // shift every later draw of this node. A hash keyed with a secret
        // of this node is as unpredictable to others.
        if (!mTieBreakSecret)
        {
            mTieBreakSecret =
                sha256(mApp.getConfig().NODE_SEED.getStrKeySeed().value +
                       " tx set tie-break");
        }
        SHA256 hasher;
        hasher.add(*mTieBreakSecret);
        hasher.add(xdr::xdr_to_opaque(job->mClosingLedgerSeq,
                                      job->mClosingValue.txSetHash));
        auto const digest = hasher.finish();
        static_assert(sizeof(size_t) <= sizeof(digest));
        std::memcpy(&job->mSelectionSeed, digest.data(), sizeof(size_t));
    }
    // As many background tasks as the batch executor would use, but leave
    // worker threads for long background jobs such as bucket merges.
    size_t const workers = static_cast<size_t>(mApp.getConfig().WORKER_THREADS);
    job->mTasks = std::max<size_t>(
        1, std::min<size_t>(mApp.getBatchExecutor().preferredTaskCount(),
                            workers > 2 ? workers - 2 : 1));
    job->mPendingTasks = job->mTasks;
    job->mTaskResults.resize(job->mTasks);
#ifdef BUILD_TESTS
    job->mHoldForTesting = mEarlyTxSetHoldForTesting;
#endif
    mEarlyTxSet = job;
    metrics.NewMeter({"herder", "txset", "early-started"}, "txset").Mark();
    CLOG_DEBUG(Herder,
               "Selecting TX set for ledger {} early, while ledger {} is in "
               "its ballot: {} tasks",
               slotIndex + 1, slotIndex, job->mTasks);
    for (size_t task = 0; task < job->mTasks; ++task)
    {
        mApp.postOnBackgroundThread(
            [job, task, &app = mApp]() {
                EarlyTxSetJob::runTask(app, job, task);
            },
            "Herder: early tx set selection");
    }
}

std::optional<HerderImpl::PreparedTxSet>
HerderImpl::completeEarlyTxSet(uint32_t ledgerSeq, ConsensusTime closeTime,
                               bool pushToPeers,
                               LedgerHeaderHistoryEntry const& lcl)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    auto& metrics = mApp.getMetrics();
    auto const plan = mEarlyTxSetPlan;
    auto const job = mEarlyTxSet;
    // Whatever comes of it, the plan ends here: it was for this build.
    stopEarlyTxSetSelection();
    if (!plan || !plan->planned || mLedgerManager.isApplying())
    {
        return std::nullopt;
    }
    auto mark = [&](char const* name) {
        metrics.NewMeter({"herder", "txset", name}, "txset").Mark();
    };
    auto discard = [&](char const* why) {
        mark("early-discarded");
        CLOG_INFO(Herder,
                  "Not using the TX set selected early for ledger {}: {}",
                  ledgerSeq, why);
        return std::nullopt;
    };
    // The selection was made for one ledger closing with one value.
    if (plan->slotIndex != lcl.header.ledgerSeq ||
        ledgerSeq != lcl.header.ledgerSeq + 1 ||
        !(plan->stellarValue == lcl.header.scpValue) ||
        !lcl.header.scpValue.upgrades.empty() ||
        buildsLegacyTxSet(lcl.header.ledgerVersion))
    {
        return discard("the ledger did not close with the value it is for");
    }
    if (!job)
    {
        // The ledger closed before the selection was due to start.
        mark("early-not-started");
        return std::nullopt;
    }
    std::shared_ptr<EarlyTxSetSelection> early;
    {
        std::lock_guard<std::mutex> lock(job->mMutex);
        early = std::move(job->mSelection);
    }
    if (!early)
    {
        // Not finished (or failed): the usual build. Nobody waits for the
        // job. It needed more time than it had: the next one starts earlier
        // by half of that again (earlyTxSetStartDelay).
        mark("early-not-ready");
        if (!job->mDone.load())
        {
            auto const had =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - job->mStart);
            mLastEarlyTxSetJobDuration =
                std::min(std::chrono::milliseconds(2000),
                         std::max(mLastEarlyTxSetJobDuration, had + had / 2));
            mEarlyTxSetJobDuration =
                std::max(mEarlyTxSetJobDuration, mLastEarlyTxSetJobDuration);
        }
        return std::nullopt;
    }
    {
        auto const took =
            std::chrono::nanoseconds(job->mFinishedAfterNs.load());
        metrics.NewTimer({"herder", "txset", "early-job"}).Update(took);
        mLastEarlyTxSetJobDuration = std::max(
            std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(took));
        mEarlyTxSetJobDuration =
            mEarlyTxSetJobDuration == std::chrono::milliseconds::zero()
                ? mLastEarlyTxSetJobDuration
                : (3 * mEarlyTxSetJobDuration + mLastEarlyTxSetJobDuration) / 4;
    }
    if (early->mLedgerSeq != ledgerSeq)
    {
        return discard("it is not for this ledger");
    }
    // Nothing but an upgrade changes the limits a set is selected within,
    // and the closed value had none. Checked all the same.
    auto const limits = TxSetBuildLimits::forLCL(mApp);
    if (!limits.sameAs(early->mLimits))
    {
        return discard("the limits of the ledger changed");
    }

    auto timer =
        metrics.NewTimer({"herder", "txset", "early-complete"}).TimeScope();
    auto const closeTimeOffset =
        closeTime.toApplyTime() - getApplyTime(lcl.header.scpValue);
    // The closed ledger, read as a build reads it. No validation cache:
    // every transaction of the selection is validated here and now.
    auto ctx = TxSetValidationContext::forLCL(mApp);
    PerPhaseTransactionList invalidTxPhases;
    invalidTxPhases.resize(early->mPhases.size());
    TxSetCompletionStats stats;

    // Further candidates for the top-up: what the job decoded and did not
    // select, then what the mempool holds that the job did not see, in
    // pulls of growing size while they come back full.
    auto seen = std::move(early->mSeen);
    bool leftoversGiven = false;
    bool mempoolExhausted = false;
    size_t pulls = 0;
    TxSetCandidateSource const moreCandidates =
        [&]() -> std::optional<TxSetCandidateBatch> {
        if (!leftoversGiven)
        {
            leftoversGiven = true;
            return std::make_optional(
                TxSetCandidateBatch{std::move(early->mLeftovers), false});
        }
        // One pull per call, twice as much each time, handed over also when
        // it holds nothing that was not seen before: a pull full of known
        // transactions says nothing about what lies behind them, and the
        // completion decides whether a phase is worth a deeper one.
        if (mempoolExhausted || pulls >= MAX_TX_SET_PULLS)
        {
            return std::nullopt;
        }
        bool full = false;
        auto fresh = pullNewCandidates(txSetPullCount() << pulls, seen, full);
        if (pulls > 0)
        {
            metrics.NewMeter({"herder", "txset", "refill-pull"}, "pull").Mark();
        }
        ++pulls;
        mempoolExhausted = !full;
        return std::make_optional(TxSetCandidateBatch{std::move(fresh), true});
    };

    TxSetXDRFrameConstPtr txSet;
    ApplicableTxSetFrameConstPtr applicableTxSet;
    std::shared_ptr<TxSetSelfCheck const> selfCheck;
    try
    {
#ifdef BUILD_TESTS
        if (mSkipEarlyTxSetFinalPassForTesting)
        {
            // What the node would propose if it trusted the selection.
            std::tie(txSet, applicableTxSet) = TxSetBuilder::writeSelection(
                early->mPhases, mApp, ctx, closeTimeOffset,
                [&](TxSetXDRFrameConstPtr const& written) {
                    handOffTxSet(written, ledgerSeq, pushToPeers);
                },
                &selfCheck);
        }
        else
#endif
        {
            auto const phases = TxSetBuilder::completeSelection(
                early->mPhases, early->mHadTxNotFittingLane, moreCandidates,
                mApp, limits, ctx, closeTimeOffset, invalidTxPhases, stats,
                early->mSelectionSeed);
            // As in a build: the structure of the selection is validated
            // before the set is written and handed over, the written set
            // in the background unless such a check has failed before.
            std::tie(txSet, applicableTxSet) = TxSetBuilder::writeSelection(
                phases, mApp, ctx, closeTimeOffset,
                [&](TxSetXDRFrameConstPtr const& written) {
                    handOffTxSet(written, ledgerSeq, pushToPeers);
                },
                mTxSetSelfCheckFailed ? nullptr : &selfCheck);
        }
    }
    catch (std::exception const& e)
    {
        // The completion made something that is not a valid set. Nothing of
        // it is proposed: the build that follows starts from the mempool,
        // and from now on this node checks every set in line.
        CLOG_ERROR(Herder,
                   "Completing the TX set selected early for ledger {} "
                   "failed: {}",
                   ledgerSeq, e.what());
        CLOG_ERROR(Herder, "{}", REPORT_INTERNAL_BUG);
        mark("early-failed");
        mTxSetSelfCheckFailed = true;
        return std::nullopt;
    }
    timer.Stop();
    // (no pull: every phase was full with what the job had left)
    recordTxSetFill(*applicableTxSet, mempoolExhausted || pulls == 0);
#ifdef BUILD_TESTS
    overrideBuiltTxSetForTesting(txSet, selfCheck);
#endif
    mLastBuiltTxSet = std::make_pair(txSet->getContentsHash(), closeTime);
    startTxSetSelfCheck(selfCheck, ledgerSeq);
    // The validation results of an earlier build on this ledger stay
    // available to a later one (a rebuild at the trigger, a later round's).
    validationCacheFor(lcl);

    mark("early-completed");
    metrics.NewMeter({"herder", "txset", "early-removed"}, "tx")
        .Mark(stats.mRemoved);
    metrics.NewMeter({"herder", "txset", "early-added"}, "tx")
        .Mark(stats.mAdded);
    metrics.NewTimer({"herder", "txset", "early-lead"})
        .Update(std::chrono::steady_clock::now() - early->mSelectedAt);
    CLOG_INFO(Herder,
              "Completed the TX set selected early for ledger {}: {} "
              "selected, {} not valid on ledger {}, {} of {} further "
              "candidates added ({} pulls), {} transactions",
              ledgerSeq, stats.mSelected, stats.mRemoved, lcl.header.ledgerSeq,
              stats.mAdded, stats.mCandidates, pulls, txSet->sizeTxTotal());
    return PreparedTxSet{lcl.hash,
                         ledgerSeq,
                         closeTime,
                         txSet,
                         std::move(applicableTxSet),
                         invalidTxsToRemove(invalidTxPhases)};
}

// called to take a position during the next round
// uses the state in LedgerManager to derive a starting position
void
HerderImpl::triggerNextLedger(uint32_t ledgerSeqToTrigger,
                              bool checkTrackingSCP)
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(ledgerSeqToTrigger));

    auto isTrackingValid = isTracking() || !checkTrackingSCP;

    if (!isTrackingValid || !mLedgerManager.isSynced())
    {
        CLOG_DEBUG(Herder, "triggerNextLedger: skipping (out of sync) : {}",
                   mApp.getStateHuman());
        return;
    }

    // If applying, the next ledger will trigger voting
    if (mLedgerManager.isApplying())
    {
        // This can only happen when closing ledgers in parallel
        releaseAssert(mApp.getConfig().backgroundLedgerApply());
        CLOG_DEBUG(Herder, "triggerNextLedger: skipping (applying) : {}",
                   mApp.getStateHuman());
        return;
    }

    // our first choice for this round's set is all the tx we have collected
    // during last few ledger closes
    // Since we are not currently applying, it is safe to use read-only LCL, as
    // it's guaranteed to be up-to-date
    auto lcl = mLedgerManager.getLastClosedLedgerHeader();

    if (ledgerSeqToTrigger != lcl.header.ledgerSeq + 1)
    {
        return;
    }

    // We pick as next close time the current time unless it's before the last
    // close time. We don't know how much time it will take to reach consensus
    // so this is the most appropriate value to use as closeTime.
    auto nextCloseTime = ConsensusTime::fromSystemTime(
        mApp.getClock().system_now(), lcl.header.ledgerVersion);
    if (ledgerSeqToTrigger == lcl.header.ledgerSeq + 1)
    {
        auto it = mDriftCTSlidingWindow.find(ledgerSeqToTrigger);
        if (it == mDriftCTSlidingWindow.end())
        {
            // Record local close time _before_ it gets adjusted to be valid
            // below
            mDriftCTSlidingWindow[ledgerSeqToTrigger] =
                std::make_pair(nextCloseTime, std::nullopt);
            while (mDriftCTSlidingWindow.size() >
                   CLOSE_TIME_DRIFT_LEDGER_WINDOW_SIZE)
            {
                mDriftCTSlidingWindow.erase(mDriftCTSlidingWindow.begin());
            }
        }
        else
        {
            CLOG_WARNING(Herder,
                         "Herder::triggerNextLedger called twice on ledger {}",
                         ledgerSeqToTrigger);
        }
    }

    auto const lclCloseTime = getConsensusTime(lcl.header.scpValue);
    if (nextCloseTime <= lclCloseTime)
    {
        nextCloseTime = lclCloseTime.next(lcl.header.ledgerVersion);
    }

    // Ensure we're about to nominate a value with valid close time
    auto isCtValid =
        ctValidityOffset(nextCloseTime) == std::chrono::milliseconds::zero();

    if (!isCtValid)
    {
        CLOG_WARNING(Herder,
                     "Invalid close time selected ({}), skipping the slot",
                     nextCloseTime.toString());
        return;
    }

    // Start the trigger-to-ballot interval only once this node is going to
    // take part. A skipped trigger must not credit the time spent waiting for
    // peers to the drift allowance.
    mHerderSCPDriver.recordTrigger(ledgerSeqToTrigger);

    if (!getSCP().isValidator())
    {
        CLOG_DEBUG(Herder, "Non-validating node, not starting the slot (SCP).");
        return;
    }

    // There is no nomination: the leader of each round (a deterministic
    // schedule derived from the LCL's header) proposes straight into the ballot
    // protocol. The value is only built if this node leads a round.
    mHerderSCPDriver.startSlot(
        ledgerSeqToTrigger,
        [this, ledgerSeqToTrigger]() {
            return makeProposalValue(ledgerSeqToTrigger);
        },
        /*constructValueNow=*/buildsLegacyTxSet(lcl.header.ledgerVersion));

    // A follower validates the leader's set when it arrives, and most of its
    // transactions are in this node's mempool too. The prewarm started at the
    // previous slot's ballot verified the candidates known then; verify the
    // current ones (mostly cached by now) while waiting for the leader's set,
    // so that validation finds them in the cache. Skip it if this node already
    // voted, i.e. validated the leader's value.
    if (!buildsLegacyTxSet(lcl.header.ledgerVersion) &&
        !mHerderSCPDriver.isLocalNodeRoundLeader(
            ledgerSeqToTrigger,
            std::max<uint32_t>(1,
                               getSCP().getLeaderRound(ledgerSeqToTrigger))) &&
        getSCP().getLatestMessagesSend(ledgerSeqToTrigger).empty())
    {
        prewarmSignatureCache(ledgerSeqToTrigger, {});
    }
}

size_t
HerderImpl::maxTxSetCandidates() const
{
    auto& lm = mApp.getLedgerManager();
    size_t maxCandidates = lm.getLastMaxTxSetSizeOps();
    if (lm.hasLastClosedSorobanNetworkConfig())
    {
        maxCandidates +=
            lm.getLastClosedSorobanNetworkConfig().ledgerMaxTxCount();
    }
    return maxCandidates;
}

std::vector<TransactionEnvelope>
HerderImpl::pullTopTransactions(size_t count)
{
#ifdef BUILD_TESTS
    if (mGetTopTransactionsForTesting)
    {
        return mGetTopTransactionsForTesting(count);
    }
#endif
    return mApp.getOverlayManager().getTopTransactions(count);
}

std::vector<size_t>
HerderImpl::validTxTargets(uint32_t lclLedgerVersion) const
{
    // Selection only uses the best valid candidates up to each phase's
    // capacity, and validating (mostly: verifying the signatures of) the rest
    // of the pull is most of the build's cost. A build validates each phase in
    // mempool order only until it holds 5/4 of its capacity in valid
    // transactions (more if some are invalid, see
    // TxSetUtils::trimInvalidPrefix). The slack covers candidates that
    // selection skips: those that don't fit the remaining resources, and those
    // the mempool ranks above ones with a higher inclusion fee (it orders by
    // total fee per operation).
    auto validationTarget = [](size_t capacity) {
        return capacity + (capacity + 3) / 4;
    };
    std::vector<size_t> targets{
        validationTarget(mLedgerManager.getLastMaxTxSetSizeOps())};
    if (protocolVersionStartsFrom(lclLedgerVersion, SOROBAN_PROTOCOL_VERSION))
    {
        targets.push_back(validationTarget(
            mLedgerManager.hasLastClosedSorobanNetworkConfig()
                ? mLedgerManager.getLastClosedSorobanNetworkConfig()
                      .ledgerMaxTxCount()
                : 0));
    }
    return targets;
}

std::vector<uint8_t>
HerderImpl::pullTopTransactionsPayload(size_t count)
{
#ifdef BUILD_TESTS
    if (mGetTopTransactionsPayloadForTesting)
    {
        return mGetTopTransactionsPayloadForTesting(count);
    }
    if (mGetTopTransactionsForTesting)
    {
        // Encode the test mempool's answer as the overlay does.
        auto envelopes = mGetTopTransactionsForTesting(count);
        std::vector<uint8_t> payload(4);
        uint32_t const n = static_cast<uint32_t>(envelopes.size());
        std::memcpy(payload.data(), &n, 4);
        for (auto const& env : envelopes)
        {
            auto const bytes = xdr::xdr_to_opaque(env);
            uint32_t const len = static_cast<uint32_t>(bytes.size());
            auto const at = payload.size();
            payload.resize(at + 4);
            std::memcpy(payload.data() + at, &len, 4);
            payload.insert(payload.end(), bytes.begin(), bytes.end());
        }
        return payload;
    }
#endif
    return mApp.getOverlayManager().getTopTransactionsPayload(count);
}

void
HerderImpl::prewarmNextLedgerSignatures(uint64_t slotIndex,
                                        Hash const& txSetHash)
{
    // Once a slot starts balloting, its value is all but decided, and the next
    // ledger's candidates are the mempool's top minus this value's
    // transactions (the overlay drops them when the slot externalizes). Every
    // validator verifies their signatures while the slot finishes balloting,
    // externalizes and closes: the next leader builds from them, and the
    // others validate the set it builds.
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (!getSCP().isValidator() || !isTracking() ||
        slotIndex != lcl.header.ledgerSeq + 1 ||
        buildsLegacyTxSet(lcl.header.ledgerVersion))
    {
        return;
    }
    std::vector<size_t> pending;
    auto txSet = mPendingEnvelopes.getTxSet(txSetHash);
    if (auto const* frame = std::get_if<TxSetXDRFrameConstPtr>(&txSet);
        frame && *frame)
    {
        pending = txCountPerPhase(**frame);
    }
    prewarmSignatureCache(static_cast<uint32_t>(slotIndex + 1), pending);
}

void
HerderImpl::prewarmSignatureCache(uint32_t ledgerSeq,
                                  std::vector<size_t> const& pendingPerPhase)
{
    ZoneScoped;
    stopSignaturePrewarm();
    // The build for `ledgerSeq` validates the first validTxTargets() of each
    // phase of its pull (more only if some are invalid), and the leader's set
    // is selected from them.
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();
    auto windows = validTxTargets(lcl.header.ledgerVersion);
    size_t pending = 0;
    for (size_t i = 0; i < windows.size() && i < pendingPerPhase.size(); ++i)
    {
        windows[i] += pendingPerPhase[i];
        pending += pendingPerPhase[i];
    }
    auto& metrics = mApp.getMetrics();
    // Only the overlay round trip runs on the main thread; the tasks decode.
    auto payload = [&]() {
        auto timer =
            metrics.NewTimer({"herder", "prewarm", "pull"}).TimeScope();
        return std::make_shared<std::vector<uint8_t> const>(
            pullTopTransactionsPayload(2 * maxTxSetCandidates() + pending));
    }();
    uint32_t count = 0;
    if (payload->size() >= 4)
    {
        std::memcpy(&count, payload->data(), 4);
    }
    if (count == 0)
    {
        return;
    }
    auto prewarm = std::make_shared<SignaturePrewarm>();
    prewarm->ledgerSeq = ledgerSeq;
    prewarm->start = std::chrono::steady_clock::now();
    // The batch executor (validation) is idle until the next build or the
    // leader's set; use as many background tasks as it would, but leave
    // worker threads for long background jobs such as bucket merges.
    size_t const workers = static_cast<size_t>(mApp.getConfig().WORKER_THREADS);
    size_t const tasks = std::max<size_t>(
        1, std::min<size_t>(mApp.getBatchExecutor().preferredTaskCount(),
                            workers > 2 ? workers - 2 : 1));
    prewarm->runningTasks = tasks;
    mSignaturePrewarm = prewarm;
    CLOG_DEBUG(Herder,
               "Pre-verifying signatures for ledger {}: {} pulled, windows {} "
               "classic / {} Soroban, {} tasks",
               ledgerSeq, count, windows[0],
               windows.size() > 1 ? windows[1] : 0, tasks);
    for (size_t task = 0; task < tasks; ++task)
    {
        mApp.postOnBackgroundThread(
            [prewarm, payload, windows, networkID = mApp.getNetworkID(), task,
             tasks]() {
                ZoneScopedN("signature prewarm");
                if (!prewarm->stop.load(std::memory_order_relaxed))
                {
                    std::call_once(prewarm->selected, [&]() {
                        prewarm->candidates = selectPrewarmCandidates(
                            OverlayIPC::parseTopTransactions(*payload),
                            windows);
                    });
                    auto const& candidates = prewarm->candidates;
                    for (size_t i = task; i < candidates.size(); i += tasks)
                    {
                        if (prewarm->stop.load(std::memory_order_relaxed))
                        {
                            break;
                        }
                        prewarm->verifiedSignatures +=
                            verifyMasterKeySignatures(networkID, candidates[i]);
                        ++prewarm->checkedTxs;
                    }
                }
                if (prewarm->runningTasks.fetch_sub(1) == 1)
                {
                    // The last task: free the candidates here rather than on
                    // the main thread when the prewarm is stopped.
                    std::vector<TransactionEnvelope>().swap(
                        prewarm->candidates);
                    prewarm->finishedAfterNs =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - prewarm->start)
                            .count();
                }
            },
            "Herder: signature prewarm");
    }
}

void
HerderImpl::stopSignaturePrewarm()
{
    if (!mSignaturePrewarm)
    {
        return;
    }
    auto prewarm = std::move(mSignaturePrewarm);
    mSignaturePrewarm.reset();
    prewarm->stop.store(true, std::memory_order_relaxed);
    // Tasks still running stop after their current transaction; what they
    // did so far is recorded.
    auto const finishedAfterNs = prewarm->finishedAfterNs.load();
    auto& metrics = mApp.getMetrics();
    metrics.NewTimer({"herder", "prewarm", "duration"})
        .Update(finishedAfterNs >= 0
                    ? std::chrono::nanoseconds(finishedAfterNs)
                    : std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - prewarm->start));
    metrics.NewMeter({"herder", "prewarm", "txs"}, "transaction")
        .Mark(prewarm->checkedTxs.load());
    metrics.NewMeter({"herder", "prewarm", "verified"}, "signature")
        .Mark(prewarm->verifiedSignatures.load());
    if (finishedAfterNs < 0)
    {
        metrics.NewMeter({"herder", "prewarm", "incomplete"}, "prewarm").Mark();
    }
}

bool
HerderImpl::buildsLegacyTxSet(uint32_t lclLedgerVersion)
{
    // Ledgers closing on a pre-Soroban LCL use legacy (non-generalized) tx
    // sets, which the Rust overlay can neither fetch nor serve: a follower
    // can only obtain the leader's set by building it. Every validator
    // therefore constructs its value at the trigger for these ledgers, as
    // before leader-only construction. With an empty mempool (genesis and
    // the upgrade-only ledgers up to protocol 20) the set depends only on the
    // previous ledger, so all validators build the same set as the leader.
    return protocolVersionIsBefore(lclLedgerVersion, SOROBAN_PROTOCOL_VERSION);
}

ValueWrapperPtr
HerderImpl::makeProposalValue(uint32_t ledgerSeqToTrigger)
{
    // A follower can be promoted long after the trigger. Recheck the ledger
    // state and select a fresh close time when SCP actually needs our value.
    // Herder tracking was checked at the trigger. Do not require it again:
    // a node that lost sync while the slot stalled (e.g. waiting for a tx set
    // it cannot fetch) must still propose once promoted, like a value built at
    // the trigger would have been, or the slot can stall forever.
    if (!mLedgerManager.isSynced() || mLedgerManager.isApplying())
    {
        return nullptr;
    }
    auto lcl = mLedgerManager.getLastClosedLedgerHeader();
    if (ledgerSeqToTrigger != lcl.header.ledgerSeq + 1)
    {
        return nullptr;
    }
    auto nextCloseTime = std::max(
        ConsensusTime::fromSystemTime(mApp.getClock().system_now(),
                                      lcl.header.ledgerVersion),
        getConsensusTime(lcl.header.scpValue).next(lcl.header.ledgerVersion));
    if (ctValidityOffset(nextCloseTime) != std::chrono::milliseconds::zero())
    {
        return nullptr;
    }

    // Consume the private snapshot only for the ledger it was built against.
    // Keep its close time: changing it would invalidate time-bound transactions
    // and the cached validation result.
    auto prepared = std::move(mPreparedTxSet);
    mPreparedTxSet.reset();
    mPrepareTxSetTimer.cancel();
    if (!prepared || prepared->ledgerSeq != ledgerSeqToTrigger ||
        prepared->previousLedgerHash != lcl.hash ||
        ctValidityOffset(prepared->closeTime) !=
            std::chrono::milliseconds::zero())
    {
        // Push the set from inside the build if this node proposes it as
        // the current round's leader. (Every validator also builds here for
        // legacy tx sets, which are neither cached nor pushed.)
        uint32_t const round = getSCP().getLeaderRound(ledgerSeqToTrigger);
        bool const activeLeader =
            round != 0 &&
            mHerderSCPDriver.isLocalNodeRoundLeader(ledgerSeqToTrigger, round);
        prepared = buildTxSet(ledgerSeqToTrigger, nextCloseTime,
                              /*pushToPeers=*/activeLeader);
    }
    else
    {
        CLOG_INFO(Herder, "Using prepared TX set for ledger {}",
                  ledgerSeqToTrigger);
        // Pushed when it was prepared; this reaches peers that connected
        // since (the overlay sends a set to each peer once).
        pushTxSetToPeers(prepared->txSet, ledgerSeqToTrigger);
    }
    nextCloseTime = prepared->closeTime;
    // The offset is computed in *apply* time rather than consensus time:
    // transaction time bounds are whole-second quantities compared against
    // StellarValue::closeTime, which CAP-0088 leaves at whole seconds. With ms
    // close times the next ledger may therefore apply in the same second as the
    // LCL (a zero offset), and a transaction whose time bounds are met at that
    // second is valid in both ledgers.
    auto const closeTimeOffset =
        nextCloseTime.toApplyTime() - getApplyTime(lcl.header.scpValue);
    auto proposedSet = prepared->txSet;
    auto const& applicableProposedSet = prepared->applicableTxSet;
    Hash txSetHash;
    if (!prepared->invalidTxHashes.empty())
    {
        CLOG_DEBUG(Herder,
                   "Removing {} transactions that failed tx set validation "
                   "from the mempool",
                   prepared->invalidTxHashes.size());
        mApp.getOverlayManager().removeTransactions(prepared->invalidTxHashes);
        // Those among them that this node accepted itself no longer hold
        // their accounts.
        forgetLocalPendingTxs(prepared->invalidTxHashes);
    }

    if (!applicableProposedSet)
    {
        releaseAssert(!mApp.getConfig().FORCE_SCP);
        return nullptr;
    }

    // New proposed tx set must be valid, so we explicitly populate tx set
    // validity cache so SCP can reuse the result.
    mHerderSCPDriver.cacheValidTxSet(*applicableProposedSet, lcl,
                                     closeTimeOffset);

#ifdef BUILD_TESTS
    if (mProposedTxSetOverride)
    {
        proposedSet = mProposedTxSetOverride(lcl);
        CLOG_INFO(Herder, "Nominating overridden tx set {} for slot {}",
                  hexAbbrev(proposedSet->getContentsHash()),
                  lcl.header.ledgerSeq + 1);
        // The overlay only cached the set built above; peers fetch this one.
        if (proposedSet->isGeneralizedTxSet())
        {
            if (mApp.getOverlayManager().cacheTxSet(
                    proposedSet->getContentsHash(),
                    proposedSet->getGeneralizedXDR(), lcl.header.ledgerSeq + 1))
            {
                mTxSetsStoredByOverlay.insert(proposedSet->getContentsHash());
            }
        }
    }
#endif

    txSetHash = proposedSet->getContentsHash();

    CLOG_INFO(Herder, "Built TX set: hash={}",
              binToHex(txSetHash).substr(0, 8));

    // Inform the item fetcher so queries from other peers about his txSet
    // can be answered. Note this can trigger SCP callbacks, externalize, etc
    // if we happen to build a txset that we were trying to download.
    mPendingEnvelopes.addTxSet(txSetHash, lcl.header.ledgerSeq + 1,
                               proposedSet);

    lcl = mLedgerManager.getLastClosedLedgerHeader();
    // use the slot index from ledger manager here as our vote is based off
    // the last closed ledger stored in ledger manager
    uint32_t slotIndex = lcl.header.ledgerSeq + 1;

    // no point in sending out a prepare:
    // externalize was triggered on a more recent ledger
    // Also skip trigger if side effects from `addTxSet` caused us to start
    // applying
    if (ledgerSeqToTrigger != slotIndex || mLedgerManager.isApplying())
    {
        return nullptr;
    }

    auto const newUpgrades = proposalUpgrades(lcl);

#ifdef BUILD_TESTS
    if (mApp.getConfig().TESTING_PROPOSE_RANDOM_VALUES &&
        getHerderSCPDriver().protocolAllowsEmptyTxSetValues())
    {
        txSetHash = HashUtils::pseudoRandomForTesting();
        CLOG_INFO(Herder,
                  "TESTING_PROPOSE_RANDOM_VALUES: proposing slot {} "
                  "with random tx-set hash {}",
                  slotIndex, hexAbbrev(txSetHash));
    }
#endif

    StellarValue newProposedValue = makeStellarValue(
        txSetHash, nextCloseTime, newUpgrades, mApp.getConfig().NODE_SEED);
    return mHerderSCPDriver.wrapStellarValue(newProposedValue);
}

void
HerderImpl::setUpgrades(Upgrades::UpgradeParameters const& upgrades)
{
    mUpgrades.setParameters(upgrades, mApp.getConfig());
    persistUpgrades();

    auto desc = mUpgrades.toString();

    if (!desc.empty())
    {
        auto message =
            fmt::format(FMT_STRING("Armed with network upgrades: {}"), desc);
        auto prev = mApp.getStatusManager().getStatusMessage(
            StatusCategory::REQUIRES_UPGRADES);
        if (prev != message)
        {
            CLOG_INFO(Herder, "{}", message);
            mApp.getStatusManager().setStatusMessage(
                StatusCategory::REQUIRES_UPGRADES, message);
        }
    }
    else
    {
        CLOG_INFO(Herder, "Network upgrades cleared");
        mApp.getStatusManager().removeStatusMessage(
            StatusCategory::REQUIRES_UPGRADES);
    }
}

std::string
HerderImpl::getUpgradesJson()
{
    auto ledgerView = mApp.getLedgerManager().getLCLView();
    return mUpgrades.getParameters().toDebugJson(*ledgerView);
}

void
HerderImpl::forceSCPStateIntoSyncWithLastClosedLedger()
{
    auto const& header = mLedgerManager.getLastClosedLedgerHeader().header;
    setTrackingSCPState(header.ledgerSeq, header.scpValue,
                        /* isTrackingNetwork */ true);
}

bool
HerderImpl::resolveNodeID(std::string const& s, PublicKey& retKey)
{
    bool r = mApp.getConfig().resolveNodeID(s, retKey);
    if (!r)
    {
        if (s.size() > 1 && s[0] == '@')
        {
            std::string arg = s.substr(1);
            getSCP().processSlotsDescendingFrom(
                std::numeric_limits<uint64>::max(), [&](uint64_t seq) {
                    getSCP().processCurrentState(
                        seq,
                        [&](SCPEnvelope const& e) {
                            std::string curK =
                                KeyUtils::toStrKey(e.statement.nodeID);
                            if (curK.compare(0, arg.size(), arg) == 0)
                            {
                                retKey = e.statement.nodeID;
                                r = true;
                                return false;
                            }
                            return true;
                        },
                        true);

                    return !r;
                });
        }
    }
    return r;
}

Json::Value
HerderImpl::getJsonInfo(size_t limit, bool fullKeys)
{
    Json::Value ret;
    ret["you"] = mApp.getConfig().toStrKey(
        mApp.getConfig().NODE_SEED.getPublicKey(), fullKeys);

    ret["scp"] = getSCP().getJsonInfo(limit, fullKeys);
    ret["queue"] = mPendingEnvelopes.getJsonInfo(limit);
    return ret;
}

Json::Value
HerderImpl::getJsonTransitiveQuorumIntersectionInfo(bool fullKeys) const
{
    Json::Value ret;
    ret["intersection"] =
        mLastQuorumMapIntersectionState->enjoysQuorunIntersection();
    ret["node_count"] =
        static_cast<Json::UInt64>(mLastQuorumMapIntersectionState->mNumNodes);
    ret["last_check_ledger"] = static_cast<Json::UInt64>(
        mLastQuorumMapIntersectionState->mLastCheckLedger);
    if (mLastQuorumMapIntersectionState->enjoysQuorunIntersection())
    {
        Json::Value critical;
        for (auto const& group :
             mLastQuorumMapIntersectionState->mIntersectionCriticalNodes)
        {
            Json::Value jg;
            for (auto const& k : group)
            {
                auto s = mApp.getConfig().toStrKey(k, fullKeys);
                jg.append(s);
            }
            critical.append(jg);
        }
        ret["critical"] = critical;
    }
    else
    {
        ret["last_good_ledger"] = static_cast<Json::UInt64>(
            mLastQuorumMapIntersectionState->mLastGoodLedger);
        Json::Value split, a, b;
        auto const& pair = mLastQuorumMapIntersectionState->mPotentialSplit;
        for (auto const& k : pair.first)
        {
            auto s = mApp.getConfig().toStrKey(k, fullKeys);
            a.append(s);
        }
        for (auto const& k : pair.second)
        {
            auto s = mApp.getConfig().toStrKey(k, fullKeys);
            b.append(s);
        }
        split.append(a);
        split.append(b);
        ret["potential_split"] = split;
    }
    return ret;
}

Json::Value
HerderImpl::getJsonQuorumInfo(NodeID const& id, bool summary, bool fullKeys,
                              uint64 index)
{
    Json::Value ret;
    ret["node"] = mApp.getConfig().toStrKey(id, fullKeys);
    ret["qset"] = getSCP().getJsonQuorumInfo(id, summary, fullKeys, index);

    bool isSelf = id == mApp.getConfig().NODE_SEED.getPublicKey();
    if (isSelf)
    {
        if (mLastQuorumMapIntersectionState->hasAnyResults())
        {
            ret["transitive"] =
                getJsonTransitiveQuorumIntersectionInfo(fullKeys);
        }

        ret["qset"]["lag_ms"] =
            getHerderSCPDriver().getQsetLagInfo(summary, fullKeys);
        ret["qset"]["cost"] =
            mPendingEnvelopes.getJsonValidatorCost(summary, fullKeys, index);
        ret["maybe_dead_nodes"] = mHerderSCPDriver.getMaybeDeadNodes(fullKeys);
    }
    return ret;
}

Json::Value
HerderImpl::getJsonTransitiveQuorumInfo(NodeID const& rootID, bool summary,
                                        bool fullKeys)
{
    Json::Value ret;
    bool isSelf = rootID == mApp.getConfig().NODE_SEED.getPublicKey();
    if (isSelf && mLastQuorumMapIntersectionState->hasAnyResults())
    {
        ret = getJsonTransitiveQuorumIntersectionInfo(fullKeys);
    }

    Json::Value& nodes = ret["nodes"];

    auto& q = mPendingEnvelopes.getCurrentlyTrackedQuorum();

    auto rootLatest = getSCP().getLatestMessage(rootID);
    std::map<Value, int> knownValues;

    // walk the quorum graph, starting at id
    UnorderedSet<NodeID> visited;
    std::vector<NodeID> next;
    next.push_back(rootID);
    visited.emplace(rootID);
    int distance = 0;
    int valGenID = 0;
    while (!next.empty())
    {
        std::vector<NodeID> frontier(std::move(next));
        next.clear();
        std::sort(frontier.begin(), frontier.end());
        for (auto const& id : frontier)
        {
            Json::Value cur;
            valGenID++;
            cur["node"] = mApp.getConfig().toStrKey(id, fullKeys);
            if (!summary)
            {
                cur["distance"] = distance;
            }
            auto it = q.find(id);
            std::string status;
            if (it != q.end())
            {
                auto qSet = it->second.mQuorumSet;
                if (qSet)
                {
                    if (!summary)
                    {
                        cur["qset"] =
                            getSCP().getLocalNode()->toJson(*qSet, fullKeys);
                    }
                    LocalNode::forAllNodes(*qSet, [&](NodeID const& n) {
                        auto b = visited.emplace(n);
                        if (b.second)
                        {
                            next.emplace_back(n);
                        }
                        return true;
                    });
                }
                auto latest = getSCP().getLatestMessage(id);
                if (latest)
                {
                    auto vals = Slot::getStatementValues(latest->statement);
                    // updates the `knownValues` map, and generate a unique ID
                    // for the value (heuristic to group votes)
                    int trackingValID = -1;
                    Value const* trackingValue = nullptr;
                    for (auto const& v : vals)
                    {
                        auto p =
                            knownValues.insert(std::make_pair(v, valGenID));
                        if (p.first->second > trackingValID)
                        {
                            trackingValID = p.first->second;
                            trackingValue = &v;
                        }
                    }

                    cur["heard"] =
                        static_cast<Json::UInt64>(latest->statement.slotIndex);
                    if (!summary)
                    {
                        cur["value"] = trackingValue
                                           ? mHerderSCPDriver.getValueString(
                                                 *trackingValue)
                                           : "";
                        cur["value_id"] = trackingValID;
                    }
                    // give a sense of how this node is doing compared to rootID
                    if (rootLatest)
                    {
                        if (latest->statement.slotIndex <
                            rootLatest->statement.slotIndex)
                        {
                            status = "behind";
                        }
                        else if (latest->statement.slotIndex >
                                 rootLatest->statement.slotIndex)
                        {
                            status = "ahead";
                        }
                        else
                        {
                            status = "tracking";
                        }
                    }
                }
                else
                {
                    status = "missing";
                }
            }
            else
            {
                status = "unknown";
            }
            cur["status"] = status;
            nodes.append(cur);
        }
        distance++;
    }
    ret["maybe_dead_nodes"] = mHerderSCPDriver.getMaybeDeadNodes(fullKeys);
    return ret;
}

QuorumTracker::QuorumMap const&
HerderImpl::getCurrentlyTrackedQuorum() const
{
    return mPendingEnvelopes.getCurrentlyTrackedQuorum();
}

static Hash
getQmapHash(QuorumTracker::QuorumMap const& qmap)
{
    ZoneScoped;
    SHA256 hasher;
    std::map<NodeID, QuorumTracker::NodeInfo> ordered_map(qmap.begin(),
                                                          qmap.end());
    for (auto const& pair : ordered_map)
    {
        hasher.add(xdr::xdr_to_opaque(pair.first));
        if (pair.second.mQuorumSet)
        {
            hasher.add(xdr::xdr_to_opaque(*(pair.second.mQuorumSet)));
        }
        else
        {
            hasher.add("\0");
        }
    }
    return hasher.finish();
}

void
HerderImpl::checkAndMaybeReanalyzeQuorumMapV2()
{
    ZoneScoped;
    if (!mApp.getConfig().QUORUM_INTERSECTION_CHECKER)
    {
        return;
    }
    auto& qmap = getCurrentlyTrackedQuorum();
    Hash curr = getQmapHash(qmap);
    if (mLastQuorumMapIntersectionState->mLastCheckQuorumMapHash == curr)
    {
        // Everything's stable, nothing to do.
        return;
    }
    if (mLastQuorumMapIntersectionState->mRecalculating)
    {
        if (mLastQuorumMapIntersectionState->mCheckingQuorumMapHash == curr)
        {
            CLOG_DEBUG(Herder, "Transitive closure of quorum has "
                               "changed, already analyzing new "
                               "configuration.");
        }
        else
        {
            CLOG_DEBUG(Herder, "Transitive closure of quorum has changed,"
                               "however the previous analysis is still "
                               "in progress, the new analysis will start after "
                               "the previous one finishes or gets interrupted "
                               "by the timer");
        }
        return;
    }

    CLOG_INFO(Herder,
              "Transitive closure of quorum has changed, re-analyzing.");
    mLastQuorumMapIntersectionState->reset(mApp);
    mLastQuorumMapIntersectionState->mRecalculating = true;
    mLastQuorumMapIntersectionState->mCheckingQuorumMapHash = curr;
    quorum_checker::runQuorumIntersectionCheckAsync(
        mApp, curr, trackingConsensusLedgerIndex(),
        mLastQuorumMapIntersectionState->mTmpDir->getName(), qmap,
        mLastQuorumMapIntersectionState, mApp.getProcessManager(),
        mApp.getConfig().QUORUM_INTERSECTION_CHECKER_TIME_LIMIT_MS,
        mApp.getConfig().QUORUM_INTERSECTION_CHECKER_MEMORY_LIMIT_BYTES,
        true /*analyzeCriticalGroups*/);
}

void
HerderImpl::checkAndMaybeReanalyzeQuorumMap()
{
    if (!mApp.getConfig().QUORUM_INTERSECTION_CHECKER)
    {
        return;
    }
    ZoneScoped;
    QuorumTracker::QuorumMap const& qmap = getCurrentlyTrackedQuorum();
    Hash curr = getQmapHash(qmap);
    if (mLastQuorumMapIntersectionState->mLastCheckQuorumMapHash == curr)
    {
        // Everything's stable, nothing to do.
        return;
    }

    if (mLastQuorumMapIntersectionState->mRecalculating)
    {
        // Already recalculating. If we're recalculating for the hash we want,
        // we do nothing, just wait for it to finish. If we're recalculating for
        // a hash that has changed _again_ (since the calculation started), we
        // _interrupt_ the calculation-in-progress: we'll return to this
        // function on the next externalize and start a new calculation for the
        // new hash we want.
        if (mLastQuorumMapIntersectionState->mCheckingQuorumMapHash == curr)
        {
            CLOG_DEBUG(Herder, "Transitive closure of quorum has "
                               "changed, already analyzing new "
                               "configuration.");
        }
        else
        {
            CLOG_DEBUG(Herder, "Transitive closure of quorum has "
                               "changed, interrupting existing "
                               "analysis.");
            mLastQuorumMapIntersectionState->mInterruptFlag = true;
        }
    }
    else
    {
        CLOG_INFO(Herder,
                  "Transitive closure of quorum has changed, re-analyzing.");
        // Not currently recalculating: start doing so.
        mLastQuorumMapIntersectionState->mRecalculating = true;
        mLastQuorumMapIntersectionState->mInterruptFlag = false;
        mLastQuorumMapIntersectionState->mCheckingQuorumMapHash = curr;
        auto& cfg = mApp.getConfig();
        auto seed = getGlobalRandomEngine()();

        auto ledger = trackingConsensusLedgerIndex();
        auto nNodes = qmap.size();
        auto hState = mLastQuorumMapIntersectionState;
        auto& app = mApp;
        auto worker = [curr, ledger, nNodes, qmap, cfg, seed, &app, hState] {
            try
            {
                ZoneScoped;
                bool ok = false;
                std::pair<std::vector<PublicKey>, std::vector<PublicKey>> split;
                auto qic = QuorumIntersectionChecker::create(
                    qmap, cfg, hState->mInterruptFlag, seed);
                ok = qic->networkEnjoysQuorumIntersection();
                split = qic->getPotentialSplit();
                std::set<std::set<PublicKey>> critical;
                if (ok)
                {
                    // Only bother calculating the _critical_ groups if we're
                    // intersecting; if not intersecting we should finish ASAP
                    // and raise an alarm.
                    auto cb = [&hState, seed](
                                  QuorumIntersectionChecker::QuorumSetMap const&
                                      qSetMap,
                                  std::optional<Config> const& config) -> bool {
                        auto checker = QuorumIntersectionChecker::create(
                            qSetMap, config, hState->mInterruptFlag, seed,
                            /*quiet=*/true);
                        return checker->networkEnjoysQuorumIntersection();
                    };
                    critical = QuorumIntersectionChecker::
                        getIntersectionCriticalGroups(
                            toQuorumIntersectionMap(qmap), cfg, cb);
                }
                app.postOnMainThread(
                    [ok, curr, ledger, nNodes, split, critical, hState, &app] {
                        hState->reset(app);

                        hState->mNumNodes = nNodes;
                        hState->mLastCheckLedger = ledger;
                        hState->mLastCheckQuorumMapHash = curr;
                        hState->mPotentialSplit = split;
                        hState->mIntersectionCriticalNodes = critical;
                        if (ok)
                        {
                            hState->mLastGoodLedger = ledger;
                        }
                    },
                    "QuorumIntersectionChecker finished");
            }
            catch (QuorumIntersectionChecker::InterruptedException&)
            {
                CLOG_DEBUG(Herder,
                           "Quorum transitive closure analysis interrupted.");
                app.postOnMainThread([hState, &app] { hState->reset(app); },
                                     "QuorumIntersectionChecker interrupted");
            }
            catch (RustQuorumCheckerError const& e)
            {
                CLOG_DEBUG(Herder,
                           "Quorum transitive closure analysis failed due to "
                           "Rust solver error: {}",
                           e.what());
                app.postOnMainThread([hState, &app] { hState->reset(app); },
                                     "QuorumIntersectionChecker rust error");
            }
        };
        mApp.postOnBackgroundThread(worker, "QuorumIntersectionChecker");
    }
}

void
HerderImpl::persistSCPState(uint64 slot)
{
    ZoneScoped;
    if (slot < mLastSlotSaved)
    {
        return;
    }
    // Runs on the main thread in front of every SCP statement this node
    // broadcasts.
    auto persistTimer = mSCPMetrics.mPersistSCPState.TimeScope();

    mLastSlotSaved = slot;
    // saves SCP messages and related data (transaction sets, quorum sets)
    PersistedSCPState scpState;
    scpState.v(1);

    auto& latestEnvs = scpState.v1().scpEnvelopes;
    std::map<Hash, TxSetXDRFrameConstPtr> txSets;
    std::map<Hash, SCPQuorumSetPtr> quorumSets;

    for (auto const& e : getSCP().getLatestMessagesSend(slot))
    {
        latestEnvs.emplace_back(e);

        // saves transaction sets referred by the statement
        for (auto const& h : getValidatedTxSetHashes(e))
        {
            if (mTxSetsStoredByOverlay.count(h) &&
                protocolValidatesTxSetsAtApply(
                    mLedgerManager.getLastClosedLedgerHeader()
                        .header.ledgerVersion))
            {
                // (Only from the protocol that votes without validating: a
                // restart gets these sets back from the overlay a moment
                // after the SCP state is restored, and before that protocol a
                // restored value whose set is missing may not be valid.)
                // On disk already, in the compressed form the overlay holds:
                // it wrote the set before delivering it (a set from a peer),
                // and writes it before broadcasting the statement that is
                // being emitted (a set built here). Serializing and
                // committing it a second time on the main thread, in front
                // of every first vote, would only add latency.
                continue;
            }
            auto result = mPendingEnvelopes.getTxSet(h);
            if (auto* txSetPtr = std::get_if<TxSetXDRFrameConstPtr>(&result))
            {
                if (*txSetPtr && !mApp.getPersistentState().hasTxSet(h))
                {
                    txSets.insert(std::make_pair(h, *txSetPtr));
                }
            }
            // EmptyTxSet: nothing to persist
        }
        Hash qsHash = Slot::getCompanionQuorumSetHashFromStatement(e.statement);
        SCPQuorumSetPtr qSet = mPendingEnvelopes.getQSet(qsHash);
        if (qSet)
        {
            quorumSets.insert(std::make_pair(qsHash, qSet));
        }
    }

    auto& latestQSets = scpState.v1().quorumSets;
    for (auto it : quorumSets)
    {
        latestQSets.emplace_back(*it.second);
    }

    stellar::Value latestSCPData;

    std::unordered_map<Hash, std::string> txSetsToPersist;
    for (auto it : txSets)
    {
        StoredTransactionSet tempTxSet;
        it.second->storeXDR(tempTxSet);
        txSetsToPersist.emplace(
            it.first, decoder::encode_b64(xdr::xdr_to_opaque(tempTxSet)));
    }

    latestSCPData = xdr::xdr_to_opaque(scpState);

    std::string encodedScpState = decoder::encode_b64(latestSCPData);

    mApp.getPersistentState().setSCPStateV1ForSlot(slot, encodedScpState,
                                                   txSetsToPersist);
}

void
HerderImpl::restoreSCPState()
{
    ZoneScoped;

    // Delete any old tx sets
    purgeOldPersistedTxSets();

    // Load all known tx sets
    auto latestTxSets = mApp.getPersistentState().getTxSetsForAllSlots();
    for (auto const& [_, txSet] : latestTxSets)
    {
        try
        {
            std::vector<uint8_t> buffer;
            decoder::decode_b64(txSet, buffer);

            StoredTransactionSet storedSet;
            xdr::xdr_from_opaque(buffer, storedSet);
            TxSetXDRFrameConstPtr cur =
                TxSetXDRFrame::makeFromStoredTxSet(storedSet);
            Hash h = cur->getContentsHash();
            mPendingEnvelopes.addTxSet(h, 0, cur);
        }
        catch (std::exception& e)
        {
            // we may have exceptions when upgrading the protocol
            // this should be the only time we get exceptions decoding old
            // messages.
            CLOG_INFO(Herder,
                      "Error while restoring old tx sets, "
                      "proceeding without them : {}",
                      e.what());
        }
    }

    // load saved state from database
    auto latest64 = mApp.getPersistentState().getSCPStateAllSlots();

    for (auto const& [_, state] : latest64)
    {
        try
        {
            std::vector<uint8_t> buffer;
            decoder::decode_b64(state, buffer);

            PersistedSCPState scpState;
            xdr::xdr_from_opaque(buffer, scpState);
            for (auto const& qset : scpState.v1().quorumSets)
            {
                Hash hash = xdrSha256(qset);
                mPendingEnvelopes.addSCPQuorumSet(hash, qset);
            }
            for (auto const& e : scpState.v1().scpEnvelopes)
            {
                getHerderSCPDriver().markSlotAsRestored(e.statement.slotIndex);
                auto envW = getHerderSCPDriver().wrapEnvelope(e);
                getSCP().setStateFromEnvelope(e.statement.slotIndex, envW);
                mLastSlotSaved =
                    std::max<uint64>(mLastSlotSaved, e.statement.slotIndex);
                fetchRestoredTxSets(e);
            }
        }
        catch (std::exception& e)
        {
            // we may have exceptions when upgrading the protocol
            // this should be the only time we get exceptions decoding old
            // messages.
            CLOG_INFO(Herder,
                      "Error while restoring old scp messages, "
                      "proceeding without them : {}",
                      e.what());
        }
        mPendingEnvelopes.rebuildQuorumTrackerState();
    }
}

void
HerderImpl::fetchRestoredTxSets(SCPEnvelope const& envelope)
{
    // The tx sets of the statements this node emitted are not in the
    // database when the overlay stores them (see persistSCPState): ask the
    // overlay for the ones an undecided slot refers to. It is not running
    // yet at this point; the request is repeated until it answers, from its
    // store or, failing that, from the peers that voted for the same value.
    if (!mApp.getOverlayManager().storesTxSets())
    {
        return;
    }
    auto const slot = envelope.statement.slotIndex;
    if (slot <= mLedgerManager.getLastClosedLedgerNum())
    {
        return;
    }
    auto const hashes = getTxSetHashes(envelope);
    if (!hashes)
    {
        return;
    }
    for (auto const& hash : *hashes)
    {
        if (hash != Herder::EMPTY_TX_SET_HASH)
        {
            mPendingEnvelopes.fetchTxSet(hash, slot);
        }
    }
}

void
HerderImpl::persistUpgrades()
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    auto s = mUpgrades.getParameters().toJson();
    mApp.getPersistentState().setMiscState(PersistentState::kLedgerUpgrades, s);
}

void
HerderImpl::restoreUpgrades()
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    std::string s = mApp.getPersistentState().getState(
        PersistentState::kLedgerUpgrades, mApp.getDatabase().getMiscSession());
    if (!s.empty())
    {
        Upgrades::UpgradeParameters p;

        p.fromJson(s);
        try
        {
            // use common code to set status
            setUpgrades(p);
        }
        catch (std::exception& e)
        {
            CLOG_INFO(Herder,
                      "Error restoring upgrades '{}' with upgrades '{}'",
                      e.what(), s);
        }
    }
}

void
HerderImpl::maybeHandleUpgrade()
{
    ZoneScoped;

    uint32_t diff = 0;
    {
        if (protocolVersionIsBefore(mApp.getLedgerManager()
                                        .getLastClosedLedgerHeader()
                                        .header.ledgerVersion,
                                    SOROBAN_PROTOCOL_VERSION))
        {
            // no-op on any earlier protocol
            return;
        }
        auto const& conf =
            mApp.getLedgerManager().getLastClosedSorobanNetworkConfig();

        auto maybeNewMaxTxSize = saturatingAdd<uint32_t>(
            conf.txMaxSizeBytes(), getFlowControlExtraBuffer());
        if (maybeNewMaxTxSize > mMaxTxSize)
        {
            diff = maybeNewMaxTxSize - mMaxTxSize;
        }
        // mMaxTxSize may decrease post-upgrade, always choose the max between
        // classic tx size (static) and Soroban max tx size
        mMaxTxSize = std::max(getMaxClassicTxSize(), maybeNewMaxTxSize);
    }

    // Note: With Rust overlay, no per-peer notifications needed here
    // The overlay handles message sizes internally
}

void
HerderImpl::start()
{
    mMaxTxSize = mApp.getHerder().getMaxClassicTxSize();
    {
        uint32_t version = mApp.getLedgerManager()
                               .getLastClosedLedgerHeader()
                               .header.ledgerVersion;
        if (protocolVersionStartsFrom(version, SOROBAN_PROTOCOL_VERSION))
        {
            auto const& conf =
                mApp.getLedgerManager().getLastClosedSorobanNetworkConfig();
            mMaxTxSize =
                std::max(mMaxTxSize,
                         saturatingAdd<uint32_t>(conf.txMaxSizeBytes(),
                                                 getFlowControlExtraBuffer()));
        }
    }

    auto const& cfg = mApp.getConfig();
    // Core will calculate default values automatically
    bool calculateDefaults = cfg.PEER_FLOOD_READING_CAPACITY_BYTES == 0 &&
                             cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES == 0;

    if (!calculateDefaults &&
        !(cfg.PEER_FLOOD_READING_CAPACITY_BYTES -
              cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES >=
          mMaxTxSize))
    {
        std::string msg = fmt::format(
            "Invalid configuration: the difference between "
            "PEER_FLOOD_READING_CAPACITY_BYTES ({}) and "
            "FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES ({}) must be at"
            " least {} bytes",
            cfg.PEER_FLOOD_READING_CAPACITY_BYTES,
            cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES, mMaxTxSize);
        throw std::runtime_error(msg);
    }

    // setup a sufficient state that we can participate in consensus
    auto const& lcl = mLedgerManager.getLastClosedLedgerHeader();

    if (!mApp.getConfig().FORCE_SCP &&
        lcl.header.ledgerSeq == LedgerManager::GENESIS_LEDGER_SEQ)
    {
        // if we're on genesis ledger, there is no point in claiming
        // that we're "in sync"
        setTrackingSCPState(lcl.header.ledgerSeq, lcl.header.scpValue,
                            /* isTrackingNetwork */ false);
    }
    else
    {
        setTrackingSCPState(lcl.header.ledgerSeq, lcl.header.scpValue,
                            /* isTrackingNetwork */ true);
        trackingHeartBeat();
        // Load SCP state from the database
        restoreSCPState();
    }

    restoreUpgrades();
    startTxSetGCTimer();
    startCheckForDeadNodesInterval();

    // RustOverlayManager is started automatically in OverlayManager::start()
    // which is called by ApplicationImpl::start() before Herder::start()
}

void
HerderImpl::startTxSetGCTimer()
{
    mTxSetGarbageCollectTimer.expires_from_now(TX_SET_GC_DELAY);
    mTxSetGarbageCollectTimer.async_wait(
        [this]() { purgeOldPersistedTxSets(); }, &VirtualTimer::onFailureNoop);
}

void
HerderImpl::purgeOldPersistedTxSets()
{
    ZoneScoped;

    // Forget about the overlay-stored sets that are gone from memory.
    for (auto it = mTxSetsStoredByOverlay.begin();
         it != mTxSetsStoredByOverlay.end();)
    {
        auto txSet = mPendingEnvelopes.getTxSet(*it);
        auto const* frame = std::get_if<TxSetXDRFrameConstPtr>(&txSet);
        if (!frame || !*frame)
        {
            it = mTxSetsStoredByOverlay.erase(it);
        }
        else
        {
            ++it;
        }
    }

    try
    {
        auto hashesToDelete =
            mApp.getPersistentState().getTxSetHashesForAllSlots();
        for (auto const& [_, state] :
             mApp.getPersistentState().getSCPStateAllSlots())
        {
            try
            {
                std::vector<uint8_t> buffer;
                decoder::decode_b64(state, buffer);

                PersistedSCPState scpState;
                xdr::xdr_from_opaque(buffer, scpState);
                for (auto const& e : scpState.v1().scpEnvelopes)
                {
                    for (auto const& hash : getValidatedTxSetHashes(e))
                    {
                        hashesToDelete.erase(hash);
                    }
                }
            }
            catch (std::exception& e)
            {
                CLOG_ERROR(Herder, "Error while deleting old tx sets: {}",
                           e.what());
            }
        }
        mApp.getPersistentState().deleteTxSets(hashesToDelete);
        startTxSetGCTimer();
    }
    catch (std::exception& e)
    {
        CLOG_ERROR(Herder, "Error while deleting old tx sets: {}", e.what());
    }
}

void
HerderImpl::startCheckForDeadNodesInterval()
{
    mCheckForDeadNodesTimer.expires_from_now(CHECK_FOR_DEAD_NODES_MINUTES);
    mCheckForDeadNodesTimer.async_wait(
        [this]() {
            mHerderSCPDriver.startCheckForDeadNodesInterval();
            startCheckForDeadNodesInterval();
        },
        &VirtualTimer::onFailureNoop);
}

void
HerderImpl::trackingHeartBeat()
{
    releaseAssert(threadIsMain());
    if (mApp.getConfig().MANUAL_CLOSE)
    {
        return;
    }

    mOutOfSyncTimer.cancel();

    releaseAssert(isTracking());

    mTrackingTimer.expires_from_now(
        std::chrono::seconds(CONSENSUS_STUCK_TIMEOUT_SECONDS));
    mTrackingTimer.async_wait(
        [this]() {
            if (mApp.getLedgerManager().isApplying())
            {
                // if we're applying a ledger, it's possible that we're just
                // slow and the timer expired; reset the timer and wait until we
                // finished application
                trackingHeartBeat();
            }
            else
            {
                herderOutOfSync();
            }
        },
        &VirtualTimer::onFailureNoop);
}

UnorderedSet<LedgerKey>
HerderImpl::recomputeKeysToFilter(uint32_t protocolVersion) const
{
    if (!gIsProductionNetwork)
    {
        return UnorderedSet<LedgerKey>{};
    }

    auto filteredSet = [](size_t count,
                          auto const& arr) mutable -> UnorderedSet<LedgerKey> {
        UnorderedSet<LedgerKey> result;
        for (size_t i = 0; i < count; ++i)
        {
            LedgerKey key;
            fromOpaqueBase64(key, arr[i]);
            result.insert(key);
        }
        return result;
    };
    return filteredSet(KEYS_TO_FILTER_P24_COUNT, KEYS_TO_FILTER_P24);
}

void
HerderImpl::herderOutOfSync()
{
    ZoneScoped;
    // State switch from "tracking" to "out of sync" should only happen if
    // there are no ledgers queued to be applied. If there are ledgers
    // queued, it's possible the rest of the network is waiting for this
    // node to vote. In this case we should _still_ remain in tracking and
    // emit nomination; If the node does not hear anything from the network
    // after that, then node can go into out of sync recovery.
    releaseAssert(threadIsMain());
    releaseAssert(!mLedgerManager.isApplying());

    CLOG_WARNING(Herder, "Lost track of consensus");

    auto s = getJsonInfo(20).toStyledString();
    CLOG_WARNING(Herder, "Out of sync context: {}", s);

    mSCPMetrics.mLostSync.Mark();
    lostSync();

    releaseAssert(getState() == Herder::HERDER_SYNCING_STATE);
    mPendingEnvelopes.reportCostOutliersForSlot(trackingConsensusLedgerIndex(),
                                                false);

    startOutOfSyncTimer();

    processSCPQueue(true);
}

void
HerderImpl::getMoreSCPState()
{
    ZoneScoped;
    auto low = getMinLedgerSeqToAskPeers();
    CLOG_INFO(Herder, "Requesting SCP state from peers, ledger >= {}", low);

    // Request SCP state via Rust overlay - it will ask random peers
    mApp.getOverlayManager().getOverlayIPC().requestScpState(low);
}

bool
HerderImpl::verifyEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    auto [b, _] = PubKeyUtils::verifySig(
        envelope.statement.nodeID, envelope.signature,
        xdr::xdr_to_opaque(mApp.getNetworkID(), ENVELOPE_TYPE_SCP,
                           envelope.statement));
    if (b)
    {
        mSCPMetrics.mEnvelopeValidSig.Mark();
    }
    else
    {
        mSCPMetrics.mEnvelopeInvalidSig.Mark();
    }

    return b;
}
void
HerderImpl::signEnvelope(SecretKey const& s, SCPEnvelope& envelope)
{
    ZoneScoped;
    envelope.signature = s.sign(xdr::xdr_to_opaque(
        mApp.getNetworkID(), ENVELOPE_TYPE_SCP, envelope.statement));
}
bool
HerderImpl::verifyStellarValueSignature(StellarValue const& sv)
{
    ZoneScoped;
    // This function should never be called with an unsigned value
    releaseAssert(isSignedStellarValue(sv) || isEmptyTxSetStellarValue(sv));

    auto const& signature = getLcValueSignature(sv);
    auto const& txSetHash =
        isEmptyTxSetStellarValue(sv) ? getProposedTxSetHash(sv) : sv.txSetHash;
    if (isMsCloseTimeStellarValue(sv))
    {
        return PubKeyUtils::verifySig(
                   signature.nodeID, signature.signature,
                   xdr::xdr_to_opaque(
                       mApp.getNetworkID(), ENVELOPE_TYPE_SCPVALUE, txSetHash,
                       sv.closeTime, getConsensusTime(sv).milliseconds()))
            .valid;
    }

    return PubKeyUtils::verifySig(signature.nodeID, signature.signature,
                                  xdr::xdr_to_opaque(mApp.getNetworkID(),
                                                     ENVELOPE_TYPE_SCPVALUE,
                                                     txSetHash, sv.closeTime))
        .valid;
}

StellarValue
HerderImpl::makeStellarValue(Hash const& txSetHash, ConsensusTime closeTime,
                             xdr::xvector<UpgradeType, 6> const& upgrades,
                             SecretKey const& s)
{
    ZoneScoped;
    bool const useMsCloseTime = getHerderSCPDriver().protocolUsesMsCloseTime();
    // Whole-second value types can only carry whole-second close times
    releaseAssert(useMsCloseTime || closeTime.isWholeSecond());

    StellarValue sv;
    sv.txSetHash = txSetHash;
    // closeTime carries the whole-second apply time in both value formats
    sv.closeTime = closeTime.toApplyTime().timePoint();
    sv.upgrades = upgrades;
    if (useMsCloseTime)
    {
        sv.ext.v(STELLAR_VALUE_SIGNED_MS);
        sv.ext.signedMsValue().closeTimeMs = closeTime.milliseconds();
        auto& signature = getLcValueSignature(sv);
        signature.nodeID = s.getPublicKey();
        // The signature also covers the full ms close time (CAP-0088). Sign
        // exactly what verifyStellarValueSignature reads back out of the value.
        signature.signature = s.sign(xdr::xdr_to_opaque(
            mApp.getNetworkID(), ENVELOPE_TYPE_SCPVALUE, sv.txSetHash,
            sv.closeTime, getConsensusTime(sv).milliseconds()));
    }
    else
    {
        sv.ext.v(STELLAR_VALUE_SIGNED);
        auto& signature = getLcValueSignature(sv);
        signature.nodeID = s.getPublicKey();
        signature.signature = s.sign(
            xdr::xdr_to_opaque(mApp.getNetworkID(), ENVELOPE_TYPE_SCPVALUE,
                               sv.txSetHash, sv.closeTime));
    }
    releaseAssert(hasValidCloseTime(sv));
    return sv;
}

bool
HerderImpl::isNewerStatement(SCPStatement const& oldSt,
                             SCPStatement const& newSt)
{
    return getSCP().isNewerStatement(oldSt, newSt);
}
}
