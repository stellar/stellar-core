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
    VirtualClock::time_point now() const;
    Config const& getConfig() const;
    rust::Box<rust_bridge::SorobanModuleCache> getModuleCache();
    bool overlayShuttingDown() const;
    OverlayMetrics& getOverlayMetrics();
    // This method is always exclusively called from one thread
    bool
    checkScheduledAndCache(std::shared_ptr<CapacityTrackedMessage> msgTracker);
    SorobanNetworkConfig const& getLastClosedSorobanNetworkConfig() const;
    SorobanNetworkConfig const& getSorobanNetworkConfigForApply() const;
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

#ifdef BUILD_TESTS
    // Access the runtime overlay-only mode flag for testing
    bool getRunInOverlayOnlyMode() const;
#endif
};
}