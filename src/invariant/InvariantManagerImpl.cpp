// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantManagerImpl.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include "bucket/LiveBucketList.h"
#include "crypto/Hex.h"
#include "invariant/Invariant.h"
#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManagerImpl.h"
#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/LedgerTxn.h"
#include "lib/util/finally.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "medida/counter.h"
#include "util/GlobalChecks.h"
#include "util/JitterInjection.h"
#include "util/Logging.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "util/XDRCereal.h"
#include <condition_variable>
#include <fmt/format.h>
#include <mutex>

#include <memory>
#include <numeric>
#include <regex>

namespace stellar
{

std::unique_ptr<InvariantManager>
InvariantManager::create(Application& app)
{
    return std::make_unique<InvariantManagerImpl>(app);
}

InvariantManagerImpl::InvariantManagerImpl(Application& app)
    : mConfig(app.getConfig())
    , mIsStopping([&app]() { return app.isStopping(); })
    , mInvariantFailureCount(
          app.getMetrics().NewCounter({"ledger", "invariant", "failure"}))
    , mStateSnapshotInvariantSkipped(app.getMetrics().NewCounter(
          {"ledger", "invariant", "state-snapshot-skipped"}))
    , mStateSnapshotTimer(app)
{
}

Json::Value
InvariantManagerImpl::getJsonInfo()
{
    MutexLocker lock(mFailureInformationMutex);
    Json::Value failures;
    for (auto const& fi : mFailureInformation)
    {
        auto& fail = failures[fi.first];
        auto& info = fi.second;
        fail["last_failed_on_ledger"] = info.lastFailedOnLedger;
        fail["last_failed_with_message"] = info.lastFailedWithMessage;
    }
    if (!failures.empty())
    {
        failures["count"] = (Json::Int64)mInvariantFailureCount.count();
    }
    return failures;
}

std::vector<std::string>
InvariantManagerImpl::getEnabledInvariants() const
{
    std::vector<std::string> res;
    for (auto const& p : mEnabled)
    {
        res.emplace_back(p->getName());
    }
    return res;
}

bool
InvariantManagerImpl::isBucketApplyInvariantEnabled() const
{
    return std::any_of(mEnabled.begin(), mEnabled.end(), [](auto const& inv) {
        return inv->getName() == "BucketListIsConsistentWithDatabase";
    });
}

void
InvariantManagerImpl::checkOnBucketApply(
    std::shared_ptr<LiveBucket const> bucket, uint32_t ledger, uint32_t level,
    bool isCurr, std::unordered_set<LedgerKey> const& shadowedKeys)
{
    uint32_t oldestLedger =
        isCurr ? LiveBucketList::oldestLedgerInCurr(ledger, level)
               : LiveBucketList::oldestLedgerInSnap(ledger, level);
    uint32_t newestLedger =
        oldestLedger - 1 +
        (isCurr ? LiveBucketList::sizeOfCurr(ledger, level)
                : LiveBucketList::sizeOfSnap(ledger, level));
    for (auto invariant : mEnabled)
    {
        auto result = invariant->checkOnBucketApply(bucket, oldestLedger,
                                                    newestLedger, shadowedKeys);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            FMT_STRING(
                R"(invariant "{}" does not hold on bucket {}[{}] = {}: {})"),
            invariant->getName(), isCurr ? "Curr" : "Snap", level,
            binToHex(bucket->getHash()), result);
        onInvariantFailure(invariant, message, ledger);
    }
}

void
InvariantManagerImpl::checkAfterAssumeState(uint32_t newestLedger)
{
    for (auto invariant : mEnabled)
    {
        auto result = invariant->checkAfterAssumeState(newestLedger);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            FMT_STRING(
                R"(invariant "{}" does not hold after assume state: {})"),
            invariant->getName(), result);
        onInvariantFailure(invariant, message, 0);
    }
}

void
InvariantManagerImpl::checkOnOperationApply(
    Operation const& operation, OperationResult const& opres,
    LedgerTxnDelta const& ltxDelta, std::vector<ContractEvent> const& events,
    AppConnector& app)
{
    for (auto invariant : mEnabled)
    {
        if (protocolVersionIsBefore(ltxDelta.header.current.ledgerVersion,
                                    ProtocolVersion::V_8) &&
            invariant->getName() != "EventsAreConsistentWithEntryDiffs")
        {
            continue;
        }

        auto result = invariant->checkOnOperationApply(operation, opres,
                                                       ltxDelta, events, app);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            FMT_STRING(R"(Invariant "{}" does not hold on operation: {}{}{})"),
            invariant->getName(), result, "\n",
            xdrToCerealString(operation, "Operation"));
        onInvariantFailure(invariant, message,
                           ltxDelta.header.current.ledgerSeq);
    }
}

void
InvariantManagerImpl::checkOnLedgerCommit(
    LedgerStateSnapshot const& lclSnapshot,
    std::vector<LedgerEntry> const& persitentEvictedFromLive,
    std::vector<LedgerKey> const& tempAndTTLEvictedFromLive,
    UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromArchive,
    UnorderedMap<LedgerKey, LedgerEntry> const& restoredFromLiveState)
{
    for (auto invariant : mEnabled)
    {
        auto result = invariant->checkOnLedgerCommit(
            lclSnapshot, persitentEvictedFromLive, tempAndTTLEvictedFromLive,
            restoredFromArchive, restoredFromLiveState);
        if (result.empty())
        {
            continue;
        }

        auto message = fmt::format(
            FMT_STRING(R"(Invariant "{}" does not hold on ledger commit: {})"),
            invariant->getName(), result);
        onInvariantFailure(invariant, message, lclSnapshot.getLedgerSeq());
    }
}

void
InvariantManagerImpl::registerInvariant(std::shared_ptr<Invariant> invariant)
{
    auto name = invariant->getName();
    auto iter = mInvariants.find(name);
    if (iter == mInvariants.end())
    {
        mInvariants[name] = invariant;
    }
    else
    {
        throw std::runtime_error{"Invariant " + invariant->getName() +
                                 " already registered"};
    }
}

void
InvariantManagerImpl::enableInvariant(std::string const& invPattern)
{
    if (invPattern.empty())
    {
        throw std::invalid_argument("Invariant pattern must be non empty");
    }

    std::regex r;
    try
    {
        r = std::regex(invPattern, std::regex::ECMAScript | std::regex::icase);
    }
    catch (std::regex_error& e)
    {
        throw std::invalid_argument(
            fmt::format(FMT_STRING("Invalid invariant pattern '{}': {}"),
                        invPattern, e.what()));
    }

    bool enabledSome = false;
    for (auto const& inv : mInvariants)
    {
        auto const& name = inv.first;
        if (std::regex_match(name, r, std::regex_constants::match_not_null))
        {
            auto iter = std::find(mEnabled.begin(), mEnabled.end(), inv.second);
            if (iter == mEnabled.end())
            {
                enabledSome = true;
                mEnabled.push_back(inv.second);
                CLOG_INFO(Invariant, "Enabled invariant '{}'", name);
            }
            else
            {
                throw std::runtime_error{"Invariant " + name +
                                         " already enabled"};
            }
        }
    }
    if (!enabledSome)
    {
        std::string message = fmt::format(
            FMT_STRING("Invariant pattern '{}' did not match any invariants."),
            invPattern);
        if (mInvariants.size() > 0)
        {
            using value_type = decltype(mInvariants)::value_type;
            std::string registered = std::accumulate(
                std::next(mInvariants.cbegin()), mInvariants.cend(),
                mInvariants.cbegin()->first,
                [](std::string const& lhs, value_type const& rhs) {
                    return lhs + ", " + rhs.first;
                });
            message += " Registered invariants are: " + registered;
        }
        else
        {
            message += " There are no registered invariants";
        }
        throw std::runtime_error{message};
    }
}

void
InvariantManagerImpl::start(LedgerManager const& ledgerManager)
{
    releaseAssert(threadIsMain());

    // If state snapshot invariants are enabled, run a snapshot on the
    // initial startup state, then schedule the next run.
    if (mConfig.INVARIANT_EXTRA_CHECKS)
    {
        scheduleSnapshotTimer();
    }
}

void
InvariantManagerImpl::onInvariantFailure(std::shared_ptr<Invariant> invariant,
                                         std::string const& message,
                                         uint32_t ledger)
{
    mInvariantFailureCount.inc();

    MutexLocker lock(mFailureInformationMutex);
    mFailureInformation[invariant->getName()] = {ledger, message};
    handleInvariantFailure(invariant->isStrict(), message);
}

void
InvariantManagerImpl::handleInvariantFailure(bool isStrict,
                                             std::string const& message) const
{
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    abort();
#endif
    if (isStrict)
    {
        CLOG_FATAL(Invariant, "{}", message);
        CLOG_FATAL(Invariant, "{}", REPORT_INTERNAL_BUG);
        throw InvariantDoesNotHold{message};
    }
    else
    {
        CLOG_ERROR(Invariant, "{}", message);
        CLOG_ERROR(Invariant, "{}", REPORT_INTERNAL_BUG);
    }
}

// The snapshot invariant is triggered periodically based on wall clock time,
// managed by the InvariantManagerImpl timing loop. After the given period has
// elapsed from out last scan, snapshotTimerFired will set
// mShouldRunStateSnapshotInvariant() to true. On the next ledger close,
// LedgerManager will read this flag shouldRunInvariantSnapshot(), copy the
// required state, then call this function in a background thread.
void
InvariantManagerImpl::runStateSnapshotInvariant(
    LedgerStateSnapshot const& snapshot,
    InMemorySorobanState const& inMemorySnapshot,
    std::function<bool()> isStopping)
{
    JITTER_INJECT_DELAY_CUSTOM(100, 100'000, 1'000'000);
    auto reset =
        gsl::finally([this]() { mStateSnapshotInvariantRunning = false; });

    try
    {
        for (auto const& invariant : mEnabled)
        {
            auto result = invariant->checkSnapshot(snapshot, inMemorySnapshot,
                                                   isStopping);
            if (!result.empty())
            {
                onInvariantFailure(invariant, result, snapshot.getLedgerSeq());
            }
        }
    }
    catch (std::exception const& e)
    {
        // Snapshot-based invariants run in a background thread. Abort on
        // failure to match the behavior of strict invariants on the main
        // thread.
        printErrorAndAbort("Exception in state snapshot invariant: ", e.what());
    }
}

void
InvariantManagerImpl::scheduleSnapshotTimer()
{
    if (mIsStopping())
    {
        return;
    }

    auto frequencySeconds = mConfig.STATE_SNAPSHOT_INVARIANT_LEDGER_FREQUENCY;
    mStateSnapshotTimer.expires_from_now(
        std::chrono::seconds(frequencySeconds));
    mStateSnapshotTimer.async_wait([this]() { snapshotTimerFired(); },
                                   &VirtualTimer::onFailureNoop);
}

void
InvariantManagerImpl::snapshotTimerFired()
{
    if (mIsStopping())
    {
        return;
    }

    // Check if the previous invariant is still running. If we haven't finished
    // the invariant in time, we will reset the timer, but not mark the
    // invariant as ready to run.
    if (mStateSnapshotInvariantRunning)
    {
        CLOG_WARNING(
            Invariant,
            "Skipping state snapshot invariant trigger "
            "because a previous scan is still running. "
            "STATE_SNAPSHOT_INVARIANT_LEDGER_FREQUENCY may be too short");
        mStateSnapshotInvariantSkipped.inc();
    }
    else
    {
        mShouldRunStateSnapshotInvariant = true;
    }

    scheduleSnapshotTimer();
}

bool
InvariantManagerImpl::shouldRunInvariantSnapshot() const
{
    if (!mConfig.INVARIANT_EXTRA_CHECKS)
    {
        return false;
    }

    return mShouldRunStateSnapshotInvariant;
}

void
InvariantManagerImpl::markStartOfInvariantSnapshot()
{
    // Safe to call from any thread since both flags are atomic. Since we check
    // mStateSnapshotInvariantRunning before setting
    // mShouldRunStateSnapshotInvariant, make sure we reset
    // mStateSnapshotInvariantRunning first to prevent multiple snapshot
    // triggers.
    mStateSnapshotInvariantRunning = true;
    mShouldRunStateSnapshotInvariant = false;
}

#ifdef BUILD_TESTS
void
InvariantManagerImpl::snapshotForFuzzer()
{
    for (auto const& invariant : mEnabled)
    {
        invariant->snapshotForFuzzer();
    }
}

void
InvariantManagerImpl::resetForFuzzer()
{
    for (auto const& invariant : mEnabled)
    {
        invariant->resetForFuzzer();
    }
}
#endif // BUILD_TESTS
}
