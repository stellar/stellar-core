// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Randomized testing for LedgerStateSnapshot, designed to
// stress-test the snapshot interface under concurrent access with timing
// variations to expose race conditions, stale reads, and data inconsistencies.

#include "test/Catch2.h"

#include "bucket/BucketBase.h"
#include "bucket/LedgerCmp.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/LedgerStateSnapshot.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "main/QueryServer.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/ThreadAnnotations.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "xdr/Stellar-ledger.h"

#include <atomic>
#include <fmt/format.h>
#include <thread>
#include <vector>

using namespace stellar;
using namespace stellar::BucketTestUtils;

namespace
{

// Types excluded from entry generation (Soroban types need special handling).
std::unordered_set<LedgerEntryType> const SOROBAN_TYPES{
    CONFIG_SETTING, CONTRACT_DATA, CONTRACT_CODE, TTL};

int const ENTRIES_PER_LEDGER = 20;

// Pick a random entry from an UnorderedMap by advancing an iterator.
template <typename K, typename V>
std::pair<K const, V> const&
randMapEntry(UnorderedMap<K, V> const& m, stellar_default_random_engine& rng)
{
    auto it = m.begin();
    std::advance(it, rand_uniform<size_t>(0, m.size() - 1, rng));
    return *it;
}

LedgerHeader
makeHeader(uint32_t seq, uint32_t protocolVersion)
{
    LedgerHeader h;
    h.ledgerSeq = seq;
    h.ledgerVersion = protocolVersion;
    return h;
}

// ---------------------------------------------------------------------------
// All test data is generated up-front before any threads start, so that the
// concurrent phase is purely exercising the snapshot interface — not the RNG
// or entry-generation code.
//
// For N ledger closes starting at startSeq+1:
//   entriesPerLedger   – the raw entries added at each ledger (fed to
//                        addBatch by the main thread).
//   stateAtLedger – the cumulative expected BucketList state at each
//                        sequence.  stateAtLedger[S] is the union of
//                        all entries added at ledgers startSeq+1 … S.
//                        Worker threads sample random entries from this
//                        to validate both current and historical reads.
// ---------------------------------------------------------------------------
struct PregenData
{
    // Each element is the set of new entries to write to the live BucketList
    // for a given ledger, in order.  Index 0 = first ledger closed.
    std::vector<std::vector<LedgerEntry>> liveEntriesToWrite;
    // Updates to existing live entries for each ledger.
    std::vector<std::vector<LedgerEntry>> liveUpdatesToWrite;
    // Same for the hot archive BucketList.
    std::vector<std::vector<LedgerEntry>> archiveEntriesToWrite;

    // Cumulative expected state at each seq (for validating reads). i.e.
    // ledgerSeq == 5 will contain a map of all entries added from [0, 5].
    UnorderedMap<uint32_t, UnorderedMap<LedgerKey, LedgerEntry>> stateAtLedger;
    UnorderedMap<uint32_t, UnorderedMap<LedgerKey, LedgerEntry>>
        hotArchiveStateAtLedger;

    uint32_t startSeq{0};
    uint32_t lastSeq{0};
};

PregenData
pregenEntries(uint32_t startSeq, int numLedgers, int entriesPerLedger)
{
    PregenData data;
    data.startSeq = startSeq;

    UnorderedSet<LedgerKey> seenKeys;
    UnorderedSet<LedgerKey> archiveSeenKeys;
    UnorderedMap<LedgerKey, LedgerEntry> runningLiveState;
    UnorderedMap<LedgerKey, LedgerEntry> runningArchiveState;

    for (uint32_t i = 0; i < numLedgers; i++)
    {
        auto seq = startSeq + 1 + i;

        // --- Live entries ---
        // Generate new unique entries for this ledger.
        auto entries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                SOROBAN_TYPES, entriesPerLedger, seenKeys);
        for (auto& e : entries)
        {
            e.lastModifiedLedgerSeq = seq;
            runningLiveState[LedgerEntryKey(e)] = e;
        }

        // Modify some existing entries so that adjacent ledgers have
        // distinguishable data for the same keys. This ensures that loading
        // from the wrong snapshot is detected by the data comparison.
        std::vector<LedgerEntry> updates;
        if (i > 0)
        {
            int updated = 0;
            for (auto& [key, entry] : runningLiveState)
            {
                if (entry.lastModifiedLedgerSeq < seq)
                {
                    entry.lastModifiedLedgerSeq = seq;
                    updates.push_back(entry);
                    if (++updated >= entriesPerLedger / 2)
                    {
                        break;
                    }
                }
            }
        }

        data.stateAtLedger[seq] = runningLiveState;
        data.liveEntriesToWrite.push_back(std::move(entries));
        data.liveUpdatesToWrite.push_back(std::move(updates));

        auto archiveEntries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {CONTRACT_CODE}, entriesPerLedger / 2, archiveSeenKeys);
        for (auto& e : archiveEntries)
        {
            e.lastModifiedLedgerSeq = seq;
            runningArchiveState[LedgerEntryKey(e)] = e;
        }
        data.hotArchiveStateAtLedger[seq] = runningArchiveState;
        data.archiveEntriesToWrite.push_back(std::move(archiveEntries));

        data.lastSeq = seq;
    }
    return data;
}

// ---------------------------------------------------------------------------
// ThreadGroup: creates threads, registers them with the Application, and
// gates them behind a start signal so all threads begin work simultaneously.
// ---------------------------------------------------------------------------
struct ThreadGroup
{
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;

    template <typename F>
    void
    launch(int n, F body)
    {
        for (int i = 0; i < n; i++)
        {
            threads.emplace_back([this, body]() {
                Application::setTestThreadType(Application::ThreadType::WORKER);
                while (!go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                body();
            });
        }
    }

    void
    start()
    {
        go.store(true, std::memory_order_release);
    }

    void
    join()
    {
        for (auto& t : threads)
        {
            t.join();
        }
    }
};

// ---------------------------------------------------------------------------
// SnapshotThread: holds a LedgerStateSnapshot together with the ledger
// sequence the owning thread expects that snapshot to be at.  The expected
// seq is private and only updated by explicit mutation operations, so any
// unsynchronized drift is immediately detectable.
// ---------------------------------------------------------------------------
class SnapshotThread
{
    mutable ANNOTATED_SHARED_MUTEX(mMutex);
    LedgerStateSnapshot mSnapshot GUARDED_BY(mMutex);
    // Updated only by mutation methods; a mismatch with
    // mSnapshot.getLedgerSeq() indicates a race or corruption.
    uint32_t mExpectedSeq;

  public:
    explicit SnapshotThread(LedgerStateSnapshot snap)
        : mSnapshot(std::move(snap)), mExpectedSeq(mSnapshot.getLedgerSeq())
    {
    }

    uint32_t
    expectedSeq() const
    {
        return mExpectedSeq;
    }

    // Read-only access.  Only the owning thread reads without a lock;
    // other threads use copySnapshot() which takes a shared lock.
    LedgerStateSnapshot const&
    snapshot() const NO_THREAD_SAFETY_ANALYSIS
    {
        return mSnapshot;
    }

    bool
    seqMatchesExpected() const NO_THREAD_SAFETY_ANALYSIS
    {
        return mSnapshot.getLedgerSeq() == mExpectedSeq;
    }

    bool
    headerMatchesExpected() const NO_THREAD_SAFETY_ANALYSIS
    {
        return mSnapshot.getLedgerHeader().current().ledgerSeq == mExpectedSeq;
    }

    // --- Mutation operations (take exclusive lock, update mExpectedSeq) ---

    // Refresh via maybeUpdate.  Returns false if seq went backward.
    bool
    maybeUpdate(LedgerManager const& lm)
    {
        SharedLockExclusive lock(mMutex);
        lm.maybeUpdateLedgerStateSnapshot(mSnapshot);
        auto newSeq = mSnapshot.getLedgerSeq();
        bool ok = newSeq >= mExpectedSeq;
        mExpectedSeq = newSeq;
        return ok;
    }

    // Replace with a fresh copy.  Returns false if seq went backward.
    bool
    freshCopy(LedgerManager const& lm)
    {
        SharedLockExclusive lock(mMutex);
        mSnapshot = lm.copyLedgerStateSnapshot();
        auto newSeq = mSnapshot.getLedgerSeq();
        bool ok = newSeq >= mExpectedSeq;
        mExpectedSeq = newSeq;
        return ok;
    }

    // Copy the snapshot under a shared lock (peer-copy source).
    LedgerStateSnapshot
    copySnapshot() const
    {
        SharedLockShared lock(mMutex);
        return mSnapshot;
    }

    // Replace with a snapshot copied from a peer.  The peer may be
    // behind, so no monotonicity check.
    void
    replaceWith(LedgerStateSnapshot snap)
    {
        SharedLockExclusive lock(mMutex);
        mSnapshot = std::move(snap);
        mExpectedSeq = mSnapshot.getLedgerSeq();
    }
};

// ---------------------------------------------------------------------------
// SnapshotStressTest: orchestrates concurrent snapshot operations against
// a main thread that is continuously closing ledgers.
//
// Each worker thread owns a SnapshotThread that tracks both the snapshot
// and the expected ledger sequence.  On every iteration the worker randomly
// picks one of seven operations:
//   READ_LIVE              – point-lookup live entries, verify against expected
//   READ_ARCHIVE           – point-lookup archive entries
//   READ_HISTORICAL        – historical live query, verify data or nullopt
//   READ_HISTORICAL_ARCHIVE – historical archive query
//   MAYBE_UPDATE           – refresh snapshot via maybeUpdate (exclusive lock)
//   FRESH_COPY             – replace from LedgerManager (exclusive lock)
//   PEER_COPY              – copy another thread's snapshot
//
// The shared_mutex per SnapshotThread means:
//   - Reads need no lock (only the owner reads its own snapshot).
//   - Mutations take the exclusive lock on the owning SnapshotThread.
//   - Peer copies take a shared lock on the source, so multiple threads
//     can copy from the same peer simultaneously.
//
// ---------------------------------------------------------------------------
class SnapshotStressTest
{
  public:
    SnapshotStressTest(int numThreads, unsigned seed, Application& app,
                       PregenData const& pregen, QueryServer& queryServer);
    ~SnapshotStressTest() = default;

    void run();

  private:
    enum class Op
    {
        READ_LIVE,
        READ_ARCHIVE,
        READ_HISTORICAL,
        READ_HISTORICAL_ARCHIVE,
        MAYBE_UPDATE,
        FRESH_COPY,
        PEER_COPY
    };

    static constexpr Op ALL_OPS[] = {
        Op::READ_LIVE,       Op::READ_ARCHIVE,
        Op::READ_HISTORICAL, Op::READ_HISTORICAL_ARCHIVE,
        Op::MAYBE_UPDATE,    Op::FRESH_COPY,
        Op::PEER_COPY};

    // --- Configuration ---
    int const mNumThreads;
    unsigned const mSeed;
    Application& mApp;
    QueryServer& mQueryServer;
    uint32_t const mProtocolVersion;
    uint32_t const mNumHistorical;
    PregenData const& mPregen;

    // --- Shared state ---
    std::atomic<bool> mDone{false};
    std::atomic<bool> mError{false};
    std::atomic<int> mHistoricalVerifications{0};
    std::vector<std::unique_ptr<SnapshotThread>> mThreads;

    bool
    shouldStop() const
    {
        return mDone.load(std::memory_order_acquire) ||
               mError.load(std::memory_order_acquire);
    }

    void
    fail(std::string const& msg)
    {
        CLOG_ERROR(Ledger, "SnapshotStressTest FAILED: {}", msg);
        mError.store(true, std::memory_order_release);
    }

    void workerLoop(int threadIdx);
    void closeLedgers();

    // Returns true if histSeq should be retained in the historical snapshot
    // at currentSeq, based on the rotation policy.
    bool shouldHistoricalExist(uint32_t currentSeq, uint32_t histSeq) const;

    // --- Per-operation methods (called from worker threads) ---
    // Read ops access the snapshot without a lock because only the owning
    // thread reads; peers take a shared lock via copySnapshot().
    void
    readCurrent(SnapshotThread& sthread, bool archive,
                stellar_default_random_engine& rng) NO_THREAD_SAFETY_ANALYSIS;
    void readHistoricalQuery(SnapshotThread& sthread, bool archive,
                             stellar_default_random_engine& rng)
        NO_THREAD_SAFETY_ANALYSIS;
    void doPeerCopy(SnapshotThread& self, int threadIdx,
                    stellar_default_random_engine& rng);
    void checkSelfConsistency(SnapshotThread const& sthread)
        NO_THREAD_SAFETY_ANALYSIS;
};

SnapshotStressTest::SnapshotStressTest(int numThreads, unsigned seed,
                                       Application& app,
                                       PregenData const& pregen,
                                       QueryServer& queryServer)
    : mNumThreads(numThreads)
    , mSeed(seed)
    , mApp(app)
    , mQueryServer(queryServer)
    , mProtocolVersion(getAppLedgerVersion(app))
    , mNumHistorical(app.getConfig().QUERY_SNAPSHOT_LEDGERS)
    , mPregen(pregen)
{
    mThreads.reserve(mNumThreads);
    for (int i = 0; i < mNumThreads; i++)
    {
        mThreads.push_back(std::make_unique<SnapshotThread>(
            mApp.getLedgerManager().copyLedgerStateSnapshot()));
    }
}

// Launch worker threads, close all pre-generated ledgers on the main thread,
// then wait briefly for workers to exercise the final state before signaling
// them to stop.  Asserts no invariant violation was observed.
void
SnapshotStressTest::run()
{
    std::atomic<int> numRegistered{0};

    ThreadGroup tg;
    for (int t = 0; t < mNumThreads; ++t)
    {
        tg.launch(1, [this, t, &numRegistered]() {
            mQueryServer.registerThread();
            ++numRegistered;

            // Wait until all threads are registered before proceeding.
            while (numRegistered.load(std::memory_order_acquire) < mNumThreads)
            {
                std::this_thread::yield();
            }
            workerLoop(t);
        });
    }
    tg.start();
    closeLedgers();

    // Give workers a brief window to exercise the final state.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    mDone.store(true, std::memory_order_release);
    tg.join();

    REQUIRE(!mError.load());

    // Ensure historical queries were actually exercised and verified
    if (mNumHistorical > 0)
    {
        REQUIRE(mHistoricalVerifications.load() > 0);
    }

    // Liveness check: after all ledgers are closed, a fresh snapshot must
    // reflect the final ledger sequence.
    auto finalSnapshot = mApp.getLedgerManager().copyLedgerStateSnapshot();
    REQUIRE(finalSnapshot.getLedgerSeq() == mPregen.lastSeq);
}

// Each worker randomly picks an operation, executes it, then checks the
// self-consistency invariant.  Runs until mDone or mError is set.
// The expected ledger seq lives inside the SnapshotThread; reads verify
// it hasn't drifted, and mutations update it atomically.
void
SnapshotStressTest::workerLoop(int threadIdx)
{
    stellar_default_random_engine rng(mSeed + threadIdx);
    auto& sthread = *mThreads[threadIdx];

    while (!shouldStop())
    {
        auto op = ALL_OPS[rand_uniform<int>(0, std::size(ALL_OPS) - 1, rng)];

        try
        {
            if (!sthread.seqMatchesExpected())
            {
                fail(fmt::format("seq drifted {}->{} seed={}",
                                 sthread.expectedSeq(),
                                 sthread.snapshot().getLedgerSeq(), mSeed));
                break;
            }

            switch (op)
            {
            case Op::READ_LIVE:
                readCurrent(sthread, false, rng);
                break;
            case Op::READ_ARCHIVE:
                readCurrent(sthread, true, rng);
                break;
            case Op::READ_HISTORICAL:
                readHistoricalQuery(sthread, false, rng);
                break;
            case Op::READ_HISTORICAL_ARCHIVE:
                readHistoricalQuery(sthread, true, rng);
                break;
            case Op::MAYBE_UPDATE:
                if (!sthread.maybeUpdate(mApp.getLedgerManager()))
                {
                    fail(fmt::format("MAYBE_UPDATE backward seed={}", mSeed));
                }
                break;
            case Op::FRESH_COPY:
                if (!sthread.freshCopy(mApp.getLedgerManager()))
                {
                    fail(fmt::format("FRESH_COPY backward seed={}", mSeed));
                }
                break;
            case Op::PEER_COPY:
                doPeerCopy(sthread, threadIdx, rng);
                break;
            }
            checkSelfConsistency(sthread);
        }
        catch (std::exception const& e)
        {
            fail(fmt::format("Exception: {}", e.what()));
        }
    }
}

// Determine whether a query for `histSeq` should
// succeed when the snapshot is at `currentSeq`.
//
// The snapshot always supports querying its own current ledger.
// If QUERY_SNAPSHOT_LEDGERS > 0, it also retains up to that many
// historical snapshots for past ledgers.  The retained window is
// [currentSeq - numHistorical, currentSeq), except during the first
// few ledgers when fewer have been accumulated.
bool
SnapshotStressTest::shouldHistoricalExist(uint32_t currentSeq,
                                          uint32_t histSeq) const
{
    // The current ledger is always queryable.
    if (histSeq == currentSeq)
    {
        return true;
    }

    // No historical snapshots configured.
    if (mNumHistorical == 0)
    {
        return false;
    }

    // Historical snapshots only cover ledgers *before* currentSeq.
    if (histSeq >= currentSeq)
    {
        return false;
    }

    // The first ledger we generated data for is startSeq + 1; earlier
    // ledgers exist in the rotation but have no pregen entries.
    uint32_t firstPregenSeq = mPregen.startSeq + 1;
    uint32_t ledgersSinceStart = currentSeq - mPregen.startSeq;

    // Early in the test we haven't accumulated a full window yet.
    uint32_t oldestRetained = (ledgersSinceStart <= mNumHistorical)
                                  ? firstPregenSeq
                                  : currentSeq - mNumHistorical;

    return histSeq >= oldestRetained;
}

// --- Per-operation implementations ---

// Point-lookup random entries from the current snapshot.  Verifies positive
// matches and negative lookups of future keys.
void
SnapshotStressTest::readCurrent(SnapshotThread& sthread, bool archive,
                                stellar_default_random_engine& rng)
{
    char const* opName = archive ? "READ_ARCHIVE" : "READ_LIVE";
    auto seq = sthread.expectedSeq();
    auto const& stateMap =
        archive ? mPregen.hotArchiveStateAtLedger : mPregen.stateAtLedger;
    auto stateIt = stateMap.find(seq);
    if (stateIt == stateMap.end())
    {
        return;
    }
    auto const& state = stateIt->second;

    // Positive lookups.
    for (int c = 0; c < 10; c++)
    {
        auto const& [key, expected] = randMapEntry(state, rng);
        if (archive)
        {
            auto loaded = sthread.snapshot().loadArchiveEntry(key);
            if (!loaded || loaded->type() != HOT_ARCHIVE_ARCHIVED ||
                loaded->archivedEntry() != expected)
            {
                fail(fmt::format("{} mismatch seq={} seed={}", opName, seq,
                                 mSeed));
                return;
            }
        }
        else
        {
            auto loaded = sthread.snapshot().loadLiveEntry(key);
            if (!loaded || *loaded != expected)
            {
                fail(fmt::format("{} mismatch seq={} seed={}", opName, seq,
                                 mSeed));
                return;
            }
        }
    }

    // Negative lookups: pick a key that only exists at a future ledger
    // and verify the current snapshot does not return it.
    if (seq < mPregen.lastSeq)
    {
        auto futureSeq = rand_uniform<uint32_t>(seq + 1, mPregen.lastSeq, rng);
        auto const& futureStateMap =
            archive ? mPregen.hotArchiveStateAtLedger : mPregen.stateAtLedger;
        auto futureIt = futureStateMap.find(futureSeq);
        if (futureIt != futureStateMap.end())
        {
            auto const& [futureKey, _] = randMapEntry(futureIt->second, rng);
            if (state.find(futureKey) == state.end())
            {
                if (archive)
                {
                    auto loaded =
                        sthread.snapshot().loadArchiveEntry(futureKey);
                    if (loaded && loaded->type() == HOT_ARCHIVE_ARCHIVED)
                    {
                        fail(fmt::format("{} false positive: found future key "
                                         "from seq={} in snapshot at seq={} "
                                         "seed={}",
                                         opName, futureSeq, seq, mSeed));
                    }
                }
                else
                {
                    if (sthread.snapshot().loadLiveEntry(futureKey))
                    {
                        fail(fmt::format("{} false positive: found future key "
                                         "from seq={} in snapshot at seq={} "
                                         "seed={}",
                                         opName, futureSeq, seq, mSeed));
                    }
                }
            }
        }
    }
}

// Bulk-load via loadKeysFromLedger on a random past ledger.  Queries a mix
// of positive keys (should exist at histSeq) and negative keys (exist only
// at a later ledger).
void
SnapshotStressTest::readHistoricalQuery(SnapshotThread& sthread, bool archive,
                                        stellar_default_random_engine& rng)
{
    char const* opName = archive ? "READ_HIST_ARCHIVE" : "READ_HIST";
    auto currentSeq = sthread.expectedSeq();
    if (currentSeq <= mPregen.startSeq)
    {
        return;
    }

    auto const& stateMap =
        archive ? mPregen.hotArchiveStateAtLedger : mPregen.stateAtLedger;

    auto histSeq =
        rand_uniform<uint32_t>(mPregen.startSeq + 1, currentSeq, rng);
    auto histStateIt = stateMap.find(histSeq);
    if (histStateIt == stateMap.end())
    {
        return;
    }
    auto const& histState = histStateIt->second;

    // Build query: positive keys (exist at histSeq) + negative keys
    // (exist at a later ledger but not at histSeq).  We only need to track
    // negativeKeys separately; any queried key not in negativeKeys is
    // positive.
    std::set<LedgerKey, LedgerEntryIdCmp> queryKeys;
    for (int c = 0; c < 5; c++)
    {
        auto const& [key, _] = randMapEntry(histState, rng);
        queryKeys.insert(key);
    }

    std::set<LedgerKey, LedgerEntryIdCmp> negativeKeys;

    // Add negative keys from nearby ledgers (histSeq+1, histSeq+2, etc.)
    // to catch off-by-one bugs in snapshot selection.
    for (uint32_t futureSeq = histSeq + 1;
         futureSeq <= std::min(histSeq + 3, currentSeq); ++futureSeq)
    {
        auto futureStateIt = stateMap.find(futureSeq);
        if (futureStateIt != stateMap.end())
        {
            for (auto const& [key, _] : futureStateIt->second)
            {
                if (histState.find(key) == histState.end())
                {
                    queryKeys.insert(key);
                    negativeKeys.insert(key);
                }
            }
        }
    }

    // Look up the historical snapshot from the QueryServer. Use the QS's
    // latest seq to determine the expected window: there is a brief window
    // where the LedgerManager has advanced to seq N but addSnapshot(N) hasn't
    // been called yet, so the worker's currentSeq may be ahead of the QS.
    auto* latestSnapshot =
        mQueryServer.getSnapshotForLedgerForTesting(std::nullopt);
    releaseAssert(latestSnapshot);
    auto qsCurrentSeq = latestSnapshot->getLedgerSeq();

    bool retained = shouldHistoricalExist(qsCurrentSeq, histSeq);
    auto* histSnapshot =
        mQueryServer.getSnapshotForLedgerForTesting(histSeq);

    // We use lazy GC for the per-thread cache, so it's possible we retain something
    // outside the window.
    if (!retained && !histSnapshot)
    {
        return;
    }
    if (!histSnapshot)
    {
        fail(fmt::format("{} unexpected nullptr histSeq={} "
                         "currentSeq={} seed={}",
                         opName, histSeq, currentSeq, mSeed));
        return;
    }

    // Load from the historical snapshot and extract LedgerEntries
    // into a uniform map for verification.
    UnorderedMap<LedgerKey, LedgerEntry> resultMap;

    if (archive)
    {
        auto result = histSnapshot->loadArchiveKeys(queryKeys);
        for (auto const& habe : result)
        {
            if (habe.type() != HOT_ARCHIVE_ARCHIVED)
            {
                fail(fmt::format("{} unexpected type histSeq={} seed={}",
                                 opName, histSeq, mSeed));
                return;
            }
            resultMap[LedgerEntryKey(habe.archivedEntry())] =
                habe.archivedEntry();
        }
    }
    else
    {
        auto result = histSnapshot->loadLiveKeys(queryKeys, "hist-query");
        for (auto const& entry : result)
        {
            resultMap[LedgerEntryKey(entry)] = entry;
        }
    }

    // Verify each queried key: positive keys must be present and match,
    // negative keys must be absent.
    for (auto const& key : queryKeys)
    {
        bool isNegative = negativeKeys.count(key) > 0;
        auto rIt = resultMap.find(key);

        if (isNegative)
        {
            if (rIt != resultMap.end())
            {
                fail(fmt::format("{} false positive for future key "
                                 "histSeq={} seed={}",
                                 opName, histSeq, mSeed));
                return;
            }
        }
        else
        {
            if (rIt == resultMap.end())
            {
                fail(fmt::format("{} missing positive key histSeq={} "
                                 "seed={}",
                                 opName, histSeq, mSeed));
                return;
            }
            if (rIt->second != histState.at(key))
            {
                fail(fmt::format("{} wrong data histSeq={} seed={}", opName,
                                 histSeq, mSeed));
                return;
            }
        }
    }

    ++mHistoricalVerifications;
}

// Copy a snapshot from a random peer thread.  The peer's copySnapshot()
// takes a shared lock (so multiple threads can copy simultaneously), then
// replaceWith() takes an exclusive lock on self to swap in.  The peer may
// be behind, so no monotonicity check — SnapshotThread::replaceWith
// updates expectedSeq to whatever the peer had.
void
SnapshotStressTest::doPeerCopy(SnapshotThread& self, int threadIdx,
                               stellar_default_random_engine& rng)
{
    int peer;
    do
    {
        peer = rand_uniform<int>(0, mNumThreads - 1, rng);
    } while (peer == threadIdx);

    auto copied = mThreads[peer]->copySnapshot();
    self.replaceWith(std::move(copied));
}

// Verify getLedgerSeq() matches both the header and the SnapshotThread's
// expected seq.  A mismatch indicates a torn/corrupt snapshot or an
// unexpected mutation.
void
SnapshotStressTest::checkSelfConsistency(SnapshotThread const& sthread)
{
    if (!sthread.seqMatchesExpected())
    {
        fail(fmt::format("consistency: expected seq {} != snapshot seq {} "
                         "seed={}",
                         sthread.expectedSeq(),
                         sthread.snapshot().getLedgerSeq(), mSeed));
        return;
    }
    if (!sthread.headerMatchesExpected())
    {
        fail(fmt::format("header/seq mismatch {}/{} seed={}",
                         sthread.snapshot().getLedgerHeader().current().ledgerSeq,
                         sthread.expectedSeq(), mSeed));
    }
}

// Main-thread driver: close all pre-generated ledgers as fast as possible,
// exercising the exclusive lock in advanceLastClosedLedgerState while worker
// threads are concurrently reading and copying snapshots.
void
SnapshotStressTest::closeLedgers()
{
    auto& bm = mApp.getBucketManager();
    auto& lm = mApp.getLedgerManager();

    for (uint32_t i = 0; i < mPregen.liveEntriesToWrite.size(); i++)
    {
        uint32_t seq = mPregen.startSeq + 1 + i;
        auto header = makeHeader(seq, mProtocolVersion);

        // Add both live and archive batches to their bucket lists, then
        // update the canonical state once so the snapshot atomically
        // includes both.
        bm.getLiveBucketList().addBatch(mApp, header.ledgerSeq,
                                        header.ledgerVersion,
                                        mPregen.liveEntriesToWrite[i],
                                        mPregen.liveUpdatesToWrite[i], {});
        if (i < mPregen.archiveEntriesToWrite.size())
        {
            bm.getHotArchiveBucketList().addBatch(
                mApp, header.ledgerSeq, header.ledgerVersion,
                mPregen.archiveEntriesToWrite[i], {});
        }
        lm.updateCanonicalStateForTesting(header);
    }
}

// ---------------------------------------------------------------------------
// Unit-test helper: verify that every entry in `entries` is found in `snap`
// with matching data.
// ---------------------------------------------------------------------------
void
requireEntries(LedgerStateSnapshot& snap,
               std::vector<LedgerEntry> const& entries)
{
    for (auto const& entry : entries)
    {
        auto loaded = snap.loadLiveEntry(LedgerEntryKey(entry));
        REQUIRE(loaded);
        CHECK(*loaded == entry);
    }
}

} // anonymous namespace

// ===========================================================================
// TEST CASES
// ===========================================================================

TEST_CASE("basic snapshot copy semantics and isolation", "[snapshot]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication<BucketTestApplication>(clock, cfg);
    auto& lm = app->getLedgerManager();
    auto protocolVersion = getAppLedgerVersion(*app);
    auto startSeq = lm.getLastClosedLedgerNum();

    UnorderedSet<LedgerKey> seenKeys;
    auto entries1 =
        LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
            SOROBAN_TYPES, 30, seenKeys);
    auto entries2 =
        LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
            SOROBAN_TYPES, 30, seenKeys);

    uint32_t seq1 = startSeq + 1;
    uint32_t seq2 = startSeq + 2;

    for (auto& e : entries1)
    {
        e.lastModifiedLedgerSeq = seq1;
    }
    for (auto& e : entries2)
    {
        e.lastModifiedLedgerSeq = seq2;
    }

    // Add first batch, take snapshot S1.
    addLiveBatchAndUpdateSnapshot(*app, makeHeader(seq1, protocolVersion),
                                  entries1, {}, {});
    auto s1 = lm.copyLedgerStateSnapshot();
    REQUIRE(s1.getLedgerSeq() == seq1);

    // Advance to seq2 with new entries.
    addLiveBatchAndUpdateSnapshot(*app, makeHeader(seq2, protocolVersion),
                                  entries2, {}, {});

    // Copy-construct S2 from S1 (after state advanced — tests isolation).
    auto s2 = s1;
    REQUIRE(s2.getLedgerSeq() == seq1);

    // S1 and S2 return identical data for entries1.
    requireEntries(s1, entries1);
    requireEntries(s2, entries1);

    // S1 and S2 must not see entries from seq2.
    for (auto const& entry : entries2)
    {
        CHECK(s1.loadLiveEntry(LedgerEntryKey(entry)) == nullptr);
        CHECK(s2.loadLiveEntry(LedgerEntryKey(entry)) == nullptr);
    }

    // maybeUpdate S1: should refresh it to seq2 (jump +1).
    lm.maybeUpdateLedgerStateSnapshot(s1);
    REQUIRE(s1.getLedgerSeq() == seq2);
    requireEntries(s1, entries1);
    requireEntries(s1, entries2);

    // maybeUpdate again with no new ledger close: no-op.
    lm.maybeUpdateLedgerStateSnapshot(s1);
    REQUIRE(s1.getLedgerSeq() == seq2);

    // Advance to seq3.
    uint32_t seq3 = startSeq + 3;
    auto entries3 =
        LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
            SOROBAN_TYPES, 30, seenKeys);
    for (auto& e : entries3)
    {
        e.lastModifiedLedgerSeq = seq3;
    }
    addLiveBatchAndUpdateSnapshot(*app, makeHeader(seq3, protocolVersion),
                                  entries3, {}, {});

    // maybeUpdate S2: still at seq1, jumps +2 (seq1 -> seq3).
    REQUIRE(s2.getLedgerSeq() == seq1);
    lm.maybeUpdateLedgerStateSnapshot(s2);
    REQUIRE(s2.getLedgerSeq() == seq3);
    requireEntries(s2, entries1);
    requireEntries(s2, entries2);
    requireEntries(s2, entries3);

    // maybeUpdate S1: at seq2, jumps +1 (seq2 -> seq3).
    REQUIRE(s1.getLedgerSeq() == seq2);
    lm.maybeUpdateLedgerStateSnapshot(s1);
    REQUIRE(s1.getLedgerSeq() == seq3);
    requireEntries(s1, entries3);
}

// ---------------------------------------------------------------------------
// Parallel snapshot stress test.  The main thread continuously closes
// ledgers while worker threads randomly and simultaneously perform snapshot
// operations with no artificial synchronization — all interleavings are
// possible.  Seeded from Catch::rngSeed()
// ---------------------------------------------------------------------------
TEST_CASE("snapshot concurrent stress test", "[snapshot][acceptance]")
{
    int const NUM_THREADS = 6;
    int const NUM_LEDGERS = 100;

    auto seed = Catch::rngSeed();
    CAPTURE(seed);

    uint32_t numHistorical = GENERATE(0, 5);
    CAPTURE(numHistorical);

    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.QUERY_SNAPSHOT_LEDGERS = numHistorical;
    cfg.QUERY_SERVER_FOR_TESTING = true;
    auto app = createTestApplication<BucketTestApplication>(clock, cfg);
    auto startSeq = app->getLedgerManager().getLastClosedLedgerNum();
    auto pregen = pregenEntries(startSeq, NUM_LEDGERS, ENTRIES_PER_LEDGER);

    auto& qServer = app->getCommandHandler().getQueryServer();

    SnapshotStressTest test(NUM_THREADS, seed, *app, pregen, qServer);
    test.run();
}

// ---------------------------------------------------------------------------
// Verify that a snapshot remains valid for long-running scans (like the
// BucketListStateConsistency invariant) even after the underlying ledger
// state has advanced well past the snapshot's sequence.
// ---------------------------------------------------------------------------
TEST_CASE("invariant check concurrent with state advance", "[snapshot]")
{
    auto seed = Catch::rngSeed();
    CAPTURE(seed);

    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication<BucketTestApplication>(clock, cfg);
    auto protocolVersion = getAppLedgerVersion(*app);
    auto startSeq = app->getLedgerManager().getLastClosedLedgerNum();

    UnorderedSet<LedgerKey> seenKeys;
    auto initialEntries =
        LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
            SOROBAN_TYPES, 100, seenKeys);
    uint32_t seq1 = startSeq + 1;
    for (auto& e : initialEntries)
    {
        e.lastModifiedLedgerSeq = seq1;
    }
    addLiveBatchAndUpdateSnapshot(*app, makeHeader(seq1, protocolVersion),
                                  initialEntries, {}, {});

    auto snapshot = app->getLedgerManager().copyLedgerStateSnapshot();
    REQUIRE(snapshot.getLedgerSeq() == seq1);

    // Build expected state map for the snapshot at seq1.
    UnorderedMap<LedgerKey, LedgerEntry> expectedAtSeq1;
    for (auto const& e : initialEntries)
    {
        expectedAtSeq1[LedgerEntryKey(e)] = e;
    }

    // Collect all keys added at future ledgers so we can detect
    // snapshot corruption that leaks newer entries into an old scan.
    UnorderedSet<LedgerKey> futureKeys;

    std::atomic<bool> scanError{false};
    // Track which expected keys were found during the scan.
    UnorderedSet<LedgerKey> matchedKeys;
    bool unexpectedEntry{false};

    ThreadGroup tg;
    tg.launch(1, [&]() {
        try
        {
            // Scan all classic entry types.  For each entry, verify
            // it belongs to the snapshot at seq1 with correct data,
            // and flag any entry from a future ledger.
            for (auto type : {ACCOUNT, TRUSTLINE, OFFER, DATA,
                              CLAIMABLE_BALANCE, LIQUIDITY_POOL})
            {
                snapshot.scanLiveEntriesOfType(
                    type, [&](BucketEntry const& be) -> Loop {
                        if (be.type() == LIVEENTRY || be.type() == INITENTRY)
                        {
                            auto const& le = be.liveEntry();
                            auto key = LedgerEntryKey(le);
                            auto eIt = expectedAtSeq1.find(key);
                            if (eIt != expectedAtSeq1.end())
                            {
                                if (le != eIt->second)
                                {
                                    CLOG_ERROR(Ledger,
                                               "Scan data mismatch for entry");
                                    scanError.store(true);
                                    return Loop::COMPLETE;
                                }
                                matchedKeys.insert(key);
                            }
                            else if (futureKeys.count(key) > 0)
                            {
                                CLOG_ERROR(Ledger,
                                           "Scan found future entry that "
                                           "should not be in snapshot");
                                unexpectedEntry = true;
                                scanError.store(true);
                                return Loop::COMPLETE;
                            }
                        }
                        return Loop::INCOMPLETE;
                    });
            }
        }
        catch (std::exception const& e)
        {
            CLOG_ERROR(Ledger, "Exception in scanner: {}", e.what());
            scanError.store(true);
        }
    });
    tg.start();

    for (int i = 2; i <= 11; i++)
    {
        uint32_t seq = startSeq + static_cast<uint32_t>(i);
        auto entries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithExclusions(
                SOROBAN_TYPES, ENTRIES_PER_LEDGER, seenKeys);
        for (auto& e : entries)
        {
            e.lastModifiedLedgerSeq = seq;
            futureKeys.insert(LedgerEntryKey(e));
        }
        addLiveBatchAndUpdateSnapshot(*app, makeHeader(seq, protocolVersion),
                                      entries, {}, {});
    }

    tg.join();
    REQUIRE(!scanError.load());
    REQUIRE(!unexpectedEntry);
    // Every expected entry must have been found in the scan.
    CHECK(matchedKeys.size() == expectedAtSeq1.size());
}
