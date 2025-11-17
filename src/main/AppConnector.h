// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketUtils.h"
#include "main/Application.h"
#include "main/Config.h"
#include "medida/metrics_registry.h"
#include "rust/RustBridge.h"

namespace stellar
{
class OverlayManager;
class LedgerManager;
class Herder;
class BanManager;
struct OverlayMetrics;
class SorobanNetworkConfig;
class SorobanMetrics;
class SearchableHotArchiveBucketListSnapshot;
struct LedgerTxnDelta;
class CapacityTrackedMessage;

// Helper class to isolate access to Application; all function helpers must
// either be called from main or be thread-safe
class AppConnector
{
    Application& mApp;
    // Copy config for threads to use, and avoid warnings from thread sanitizer
    // about accessing mApp
    Config const mConfig;

  public:
    AppConnector(Application& app);

    // Methods that can only be called from main thread
    Herder& getHerder();
    LedgerManager& getLedgerManager();
    OverlayManager& getOverlayManager();
    BanManager& getBanManager();
    bool shouldYield() const;
    void checkOnOperationApply(Operation const& operation,
                               OperationResult const& opres,
                               LedgerTxnDelta const& ltxDelta,
                               std::vector<ContractEvent> const& events);
    Hash const& getNetworkID() const;

    // Thread-safe methods
    SorobanMetrics& getSorobanMetrics() const;
    void postOnMainThread(
        std::function<void()>&& f, std::string&& message,
        Scheduler::ActionType type = Scheduler::ActionType::NORMAL_ACTION);
    void postOnOverlayThread(std::function<void()>&& f,
                             std::string const& message);
    void postOnBackgroundThread(std::function<void()>&& f,
                                std::string const& jobName);
    void postOnEvictionBackgroundThread(std::function<void()>&& f,
                                        std::string const& jobName);
    VirtualClock::time_point now() const;
    Config const& getConfig() const;
    rust::Box<rust_bridge::SorobanModuleCache> getModuleCache();
    bool overlayShuttingDown() const;
    OverlayMetrics& getOverlayMetrics();
    // This method is always exclusively called from one thread
    bool
    checkScheduledAndCache(std::shared_ptr<CapacityTrackedMessage> msgTracker);
    SorobanNetworkConfig const& getLastClosedSorobanNetworkConfig() const;
    bool threadIsType(Application::ThreadType type) const;

    medida::MetricsRegistry& getMetrics() const;
    SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot();

    SearchableSnapshotConstPtr copySearchableLiveBucketListSnapshot();

    // Refreshes `snapshot` if a newer snapshot is available. No-op otherwise.
    void
    maybeCopySearchableBucketListSnapshot(SearchableSnapshotConstPtr& snapshot);

    // Get a snapshot of ledger state for use by the overlay thread only. Must
    // only be called from the overlay thread.
    SearchableSnapshotConstPtr& getOverlayThreadSnapshot();

    // Protocol 23 data corruption bug data verifier. This typically is null,
    // unless a path to a CSV file containing the corruption data was provided
    // in the config at startup.
    std::unique_ptr<p23_hot_archive_bug::Protocol23CorruptionDataVerifier>&
    getProtocol23CorruptionDataVerifier();

    std::unique_ptr<p23_hot_archive_bug::Protocol23CorruptionEventReconciler>&
    getProtocol23CorruptionEventReconciler();

#ifdef BUILD_TESTS
    // Access the runtime overlay-only mode flag for testing
    bool getRunInOverlayOnlyMode() const;
#endif
};
}
